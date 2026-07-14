#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PCF_FILE_VERSION 0x70636601u

#define PCF_METRICS (1u << 2)
#define PCF_BITMAPS (1u << 3)
#define PCF_BDF_ENCODINGS (1u << 5)

#define PCF_FORMAT_MASK 0xffffff00u
#define PCF_COMPRESSED_METRICS 0x00000100u
#define PCF_GLYPH_PAD_MASK 0x00000003u
#define PCF_BYTE_MASK 0x00000004u
#define PCF_BIT_MASK 0x00000008u

#define FONT_WIDTH 6u
#define FONT_HEIGHT 13u
#define SOURCE_GLYPH_COUNT 256u
#define TOTAL_GLYPH_COUNT 257u
#define FALLBACK_GLYPH_INDEX 256u
#define PCF_MISSING_GLYPH 0xffffu

typedef struct Buffer {
    uint8_t *data;
    size_t size;
} Buffer;

typedef struct PcfTableEntry {
    uint32_t type;
    uint32_t format;
    uint32_t size;
    uint32_t offset;
} PcfTableEntry;

typedef struct PcfMetric {
    int left_side_bearing;
    int right_side_bearing;
    int character_width;
    int ascent;
    int descent;
} PcfMetric;

typedef struct PcfMetrics {
    PcfMetric *items;
    uint32_t count;
} PcfMetrics;

typedef struct PcfBitmaps {
    uint32_t format;
    uint32_t glyph_count;
    uint32_t *offsets;
    const uint8_t *bitmap_data;
    size_t bitmap_size;
} PcfBitmaps;

typedef struct ParsedFont {
    PcfMetrics metrics;
    PcfBitmaps bitmaps;
    uint16_t encoding_to_glyph[SOURCE_GLYPH_COUNT];
    uint32_t min_char_or_byte2;
    uint32_t max_char_or_byte2;
    uint32_t min_byte1;
    uint32_t max_byte1;
    uint32_t missing_source_glyphs;
} ParsedFont;

typedef struct FontRows {
    uint8_t masks[TOTAL_GLYPH_COUNT][FONT_HEIGHT];
    uint32_t non_empty_rows;
    uint32_t max_ascent;
    uint32_t max_descent;
} FontRows;

static uint32_t read_le32_unchecked(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static int has_range(const Buffer *buffer, size_t offset, size_t length)
{
    return offset <= buffer->size && length <= buffer->size - offset;
}

static int read_u16_at(const Buffer *buffer, size_t offset, int msb_first, uint16_t *out)
{
    const uint8_t *data = NULL;
    if (!has_range(buffer, offset, 2u)) {
        return 0;
    }

    data = buffer->data + offset;
    if (msb_first) {
        *out = (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
    } else {
        *out = (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
    }
    return 1;
}

static int read_u32_at(const Buffer *buffer, size_t offset, int msb_first, uint32_t *out)
{
    const uint8_t *data = NULL;
    if (!has_range(buffer, offset, 4u)) {
        return 0;
    }

    data = buffer->data + offset;
    if (msb_first) {
        *out = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | (uint32_t)data[3];
    } else {
        *out = read_le32_unchecked(data);
    }
    return 1;
}

static int read_le32_at(const Buffer *buffer, size_t offset, uint32_t *out)
{
    return read_u32_at(buffer, offset, 0, out);
}

static int load_file(const char *path, Buffer *out)
{
    FILE *file = NULL;
    long size_long = 0;
    size_t size = 0u;
    size_t bytes_read = 0u;

    memset(out, 0, sizeof(*out));

    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "pcf_to_header: failed to open input '%s': %s\n", path, strerror(errno));
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "pcf_to_header: failed to seek input '%s'\n", path);
        fclose(file);
        return 0;
    }

    size_long = ftell(file);
    if (size_long < 0) {
        fprintf(stderr, "pcf_to_header: failed to size input '%s'\n", path);
        fclose(file);
        return 0;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "pcf_to_header: failed to rewind input '%s'\n", path);
        fclose(file);
        return 0;
    }

    size = (size_t)size_long;
    out->data = (uint8_t *)malloc(size == 0u ? 1u : size);
    if (out->data == NULL) {
        fprintf(stderr, "pcf_to_header: failed to allocate %zu bytes\n", size);
        fclose(file);
        return 0;
    }

    bytes_read = fread(out->data, 1u, size, file);
    fclose(file);

    if (bytes_read != size) {
        fprintf(stderr, "pcf_to_header: failed to read input '%s'\n", path);
        free(out->data);
        memset(out, 0, sizeof(*out));
        return 0;
    }

    out->size = size;
    return 1;
}

static void free_parsed_font(ParsedFont *font)
{
    free(font->metrics.items);
    free(font->bitmaps.offsets);
    memset(font, 0, sizeof(*font));
}

static int read_toc(const Buffer *buffer, PcfTableEntry **tables_out, uint32_t *table_count_out)
{
    uint32_t version = 0u;
    uint32_t table_count = 0u;
    PcfTableEntry *tables = NULL;
    size_t offset = 0u;
    uint32_t i = 0u;

    if (!read_le32_at(buffer, 0u, &version) || version != PCF_FILE_VERSION) {
        fprintf(stderr, "pcf_to_header: input is not a supported PCF file\n");
        return 0;
    }

    if (!read_le32_at(buffer, 4u, &table_count)) {
        fprintf(stderr, "pcf_to_header: PCF table count is truncated\n");
        return 0;
    }

    if (table_count == 0u || table_count > 32u) {
        fprintf(stderr, "pcf_to_header: unsupported PCF table count %u\n", table_count);
        return 0;
    }

    if (!has_range(buffer, 8u, (size_t)table_count * 16u)) {
        fprintf(stderr, "pcf_to_header: PCF table of contents is truncated\n");
        return 0;
    }

    tables = (PcfTableEntry *)calloc(table_count, sizeof(*tables));
    if (tables == NULL) {
        fprintf(stderr, "pcf_to_header: failed to allocate table of contents\n");
        return 0;
    }

    offset = 8u;
    for (i = 0u; i < table_count; ++i) {
        if (!read_le32_at(buffer, offset, &tables[i].type) ||
            !read_le32_at(buffer, offset + 4u, &tables[i].format) ||
            !read_le32_at(buffer, offset + 8u, &tables[i].size) ||
            !read_le32_at(buffer, offset + 12u, &tables[i].offset)) {
            fprintf(stderr, "pcf_to_header: PCF table entry is truncated\n");
            free(tables);
            return 0;
        }

        offset += 16u;
    }

    *tables_out = tables;
    *table_count_out = table_count;
    return 1;
}

static const PcfTableEntry *find_table(const PcfTableEntry *tables, uint32_t table_count, uint32_t type)
{
    uint32_t i = 0u;

    for (i = 0u; i < table_count; ++i) {
        if (tables[i].type == type) {
            return tables + i;
        }
    }

    return NULL;
}

static int parse_metrics(const Buffer *buffer, const PcfTableEntry *table, PcfMetrics *metrics_out)
{
    uint32_t format = 0u;
    int msb_first = 0;
    uint32_t count = 0u;
    size_t offset = (size_t)table->offset;
    uint32_t i = 0u;
    PcfMetric *items = NULL;

    if (!read_le32_at(buffer, offset, &format)) {
        fprintf(stderr, "pcf_to_header: metrics table is truncated\n");
        return 0;
    }
    offset += 4u;

    msb_first = (format & PCF_BYTE_MASK) != 0u;

    if ((format & PCF_FORMAT_MASK) == PCF_COMPRESSED_METRICS) {
        uint16_t compressed_count = 0u;

        if (!read_u16_at(buffer, offset, msb_first, &compressed_count)) {
            fprintf(stderr, "pcf_to_header: compressed metrics count is truncated\n");
            return 0;
        }
        offset += 2u;
        count = (uint32_t)compressed_count;

        if (!has_range(buffer, offset, (size_t)count * 5u)) {
            fprintf(stderr, "pcf_to_header: compressed metrics are truncated\n");
            return 0;
        }

        items = (PcfMetric *)calloc(count, sizeof(*items));
        if (items == NULL) {
            fprintf(stderr, "pcf_to_header: failed to allocate metrics\n");
            return 0;
        }

        for (i = 0u; i < count; ++i) {
            items[i].left_side_bearing = (int)buffer->data[offset + 0u] - 0x80;
            items[i].right_side_bearing = (int)buffer->data[offset + 1u] - 0x80;
            items[i].character_width = (int)buffer->data[offset + 2u] - 0x80;
            items[i].ascent = (int)buffer->data[offset + 3u] - 0x80;
            items[i].descent = (int)buffer->data[offset + 4u] - 0x80;
            offset += 5u;
        }
    } else {
        if (!read_u32_at(buffer, offset, msb_first, &count)) {
            fprintf(stderr, "pcf_to_header: metrics count is truncated\n");
            return 0;
        }
        offset += 4u;

        if (!has_range(buffer, offset, (size_t)count * 12u)) {
            fprintf(stderr, "pcf_to_header: metrics are truncated\n");
            return 0;
        }

        items = (PcfMetric *)calloc(count, sizeof(*items));
        if (items == NULL) {
            fprintf(stderr, "pcf_to_header: failed to allocate metrics\n");
            return 0;
        }

        for (i = 0u; i < count; ++i) {
            uint16_t raw = 0u;
            if (!read_u16_at(buffer, offset + 0u, msb_first, &raw)) {
                free(items);
                return 0;
            }
            items[i].left_side_bearing = (int)(int16_t)raw;
            if (!read_u16_at(buffer, offset + 2u, msb_first, &raw)) {
                free(items);
                return 0;
            }
            items[i].right_side_bearing = (int)(int16_t)raw;
            if (!read_u16_at(buffer, offset + 4u, msb_first, &raw)) {
                free(items);
                return 0;
            }
            items[i].character_width = (int)(int16_t)raw;
            if (!read_u16_at(buffer, offset + 6u, msb_first, &raw)) {
                free(items);
                return 0;
            }
            items[i].ascent = (int)(int16_t)raw;
            if (!read_u16_at(buffer, offset + 8u, msb_first, &raw)) {
                free(items);
                return 0;
            }
            items[i].descent = (int)(int16_t)raw;
            offset += 12u;
        }
    }

    metrics_out->items = items;
    metrics_out->count = count;
    return 1;
}

static int parse_bitmaps(const Buffer *buffer, const PcfTableEntry *table, PcfBitmaps *bitmaps_out)
{
    uint32_t format = 0u;
    int msb_first = 0;
    uint32_t glyph_count = 0u;
    uint32_t sizes[4] = { 0u, 0u, 0u, 0u };
    uint32_t pad_index = 0u;
    uint32_t *offsets = NULL;
    size_t offset = (size_t)table->offset;
    uint32_t i = 0u;

    if (!read_le32_at(buffer, offset, &format)) {
        fprintf(stderr, "pcf_to_header: bitmap table is truncated\n");
        return 0;
    }
    offset += 4u;
    msb_first = (format & PCF_BYTE_MASK) != 0u;

    if (!read_u32_at(buffer, offset, msb_first, &glyph_count)) {
        fprintf(stderr, "pcf_to_header: bitmap glyph count is truncated\n");
        return 0;
    }
    offset += 4u;

    if (glyph_count == 0u || glyph_count > 65535u) {
        fprintf(stderr, "pcf_to_header: unsupported bitmap glyph count %u\n", glyph_count);
        return 0;
    }

    if (!has_range(buffer, offset, (size_t)glyph_count * 4u + 16u)) {
        fprintf(stderr, "pcf_to_header: bitmap offsets are truncated\n");
        return 0;
    }

    offsets = (uint32_t *)calloc(glyph_count, sizeof(*offsets));
    if (offsets == NULL) {
        fprintf(stderr, "pcf_to_header: failed to allocate bitmap offsets\n");
        return 0;
    }

    for (i = 0u; i < glyph_count; ++i) {
        if (!read_u32_at(buffer, offset, msb_first, &offsets[i])) {
            free(offsets);
            return 0;
        }
        offset += 4u;
    }

    for (i = 0u; i < 4u; ++i) {
        if (!read_u32_at(buffer, offset, msb_first, &sizes[i])) {
            free(offsets);
            return 0;
        }
        offset += 4u;
    }

    pad_index = format & PCF_GLYPH_PAD_MASK;
    if (!has_range(buffer, offset, (size_t)sizes[pad_index])) {
        fprintf(stderr, "pcf_to_header: bitmap data is truncated\n");
        free(offsets);
        return 0;
    }

    bitmaps_out->format = format;
    bitmaps_out->glyph_count = glyph_count;
    bitmaps_out->offsets = offsets;
    bitmaps_out->bitmap_data = buffer->data + offset;
    bitmaps_out->bitmap_size = (size_t)sizes[pad_index];
    return 1;
}

static int parse_encodings(const Buffer *buffer, const PcfTableEntry *table, const PcfBitmaps *bitmaps, ParsedFont *font)
{
    uint32_t format = 0u;
    int msb_first = 0;
    uint16_t min_char_or_byte2 = 0u;
    uint16_t max_char_or_byte2 = 0u;
    uint16_t min_byte1 = 0u;
    uint16_t max_byte1 = 0u;
    uint16_t default_char = 0u;
    uint32_t byte2_count = 0u;
    uint32_t byte1_count = 0u;
    size_t index_count = 0u;
    size_t indices_offset = 0u;
    size_t offset = (size_t)table->offset;
    uint32_t code = 0u;

    if (!read_le32_at(buffer, offset, &format)) {
        fprintf(stderr, "pcf_to_header: encoding table is truncated\n");
        return 0;
    }
    offset += 4u;
    msb_first = (format & PCF_BYTE_MASK) != 0u;

    if (!read_u16_at(buffer, offset + 0u, msb_first, &min_char_or_byte2) ||
        !read_u16_at(buffer, offset + 2u, msb_first, &max_char_or_byte2) ||
        !read_u16_at(buffer, offset + 4u, msb_first, &min_byte1) ||
        !read_u16_at(buffer, offset + 6u, msb_first, &max_byte1) ||
        !read_u16_at(buffer, offset + 8u, msb_first, &default_char)) {
        fprintf(stderr, "pcf_to_header: encoding bounds are truncated\n");
        return 0;
    }
    (void)default_char;
    offset += 10u;

    if (max_char_or_byte2 < min_char_or_byte2 || max_byte1 < min_byte1) {
        fprintf(stderr, "pcf_to_header: unsupported descending encoding bounds\n");
        return 0;
    }

    byte2_count = (uint32_t)max_char_or_byte2 - (uint32_t)min_char_or_byte2 + 1u;
    byte1_count = (uint32_t)max_byte1 - (uint32_t)min_byte1 + 1u;
    index_count = (size_t)byte1_count * (size_t)byte2_count;
    indices_offset = offset;

    if (!has_range(buffer, indices_offset, index_count * 2u)) {
        fprintf(stderr, "pcf_to_header: encoding indices are truncated\n");
        return 0;
    }

    for (code = 0u; code < SOURCE_GLYPH_COUNT; ++code) {
        const uint32_t byte1 = code >> 8;
        const uint32_t byte2 = code & 0xffu;
        uint32_t position = 0u;
        uint16_t glyph_index = 0xffffu;

        if (byte1 < (uint32_t)min_byte1 || byte1 > (uint32_t)max_byte1 ||
            byte2 < (uint32_t)min_char_or_byte2 || byte2 > (uint32_t)max_char_or_byte2) {
            fprintf(stderr, "pcf_to_header: ISO-8859-1 code 0x%02x is outside the PCF encoding bounds\n", code);
            return 0;
        }

        position = (byte1 - (uint32_t)min_byte1) * byte2_count + (byte2 - (uint32_t)min_char_or_byte2);
        if (!read_u16_at(buffer, indices_offset + (size_t)position * 2u, msb_first, &glyph_index)) {
            return 0;
        }

        if (glyph_index == PCF_MISSING_GLYPH) {
            font->encoding_to_glyph[code] = PCF_MISSING_GLYPH;
            ++font->missing_source_glyphs;
            continue;
        }

        if ((uint32_t)glyph_index >= bitmaps->glyph_count) {
            fprintf(stderr, "pcf_to_header: ISO-8859-1 code 0x%02x references glyph %u outside bitmap table\n", code, glyph_index);
            return 0;
        }

        font->encoding_to_glyph[code] = glyph_index;
    }

    font->min_char_or_byte2 = min_char_or_byte2;
    font->max_char_or_byte2 = max_char_or_byte2;
    font->min_byte1 = min_byte1;
    font->max_byte1 = max_byte1;
    return 1;
}

static int parse_pcf(const Buffer *buffer, ParsedFont *font)
{
    PcfTableEntry *tables = NULL;
    uint32_t table_count = 0u;
    const PcfTableEntry *metrics_table = NULL;
    const PcfTableEntry *bitmaps_table = NULL;
    const PcfTableEntry *encodings_table = NULL;
    int ok = 0;

    memset(font, 0, sizeof(*font));

    if (!read_toc(buffer, &tables, &table_count)) {
        return 0;
    }

    metrics_table = find_table(tables, table_count, PCF_METRICS);
    bitmaps_table = find_table(tables, table_count, PCF_BITMAPS);
    encodings_table = find_table(tables, table_count, PCF_BDF_ENCODINGS);

    if (metrics_table == NULL || bitmaps_table == NULL || encodings_table == NULL) {
        fprintf(stderr, "pcf_to_header: PCF is missing required metrics, bitmaps, or encoding tables\n");
        goto cleanup;
    }

    if (!parse_metrics(buffer, metrics_table, &font->metrics)) {
        goto cleanup;
    }

    if (!parse_bitmaps(buffer, bitmaps_table, &font->bitmaps)) {
        goto cleanup;
    }

    if (font->metrics.count != font->bitmaps.glyph_count) {
        fprintf(stderr, "pcf_to_header: metrics count %u does not match bitmap glyph count %u\n",
                font->metrics.count,
                font->bitmaps.glyph_count);
        goto cleanup;
    }

    if (!parse_encodings(buffer, encodings_table, &font->bitmaps, font)) {
        goto cleanup;
    }

    ok = 1;

cleanup:
    free(tables);
    if (!ok) {
        free_parsed_font(font);
    }
    return ok;
}

static uint32_t padded_row_bytes(uint32_t width_bits, uint32_t format)
{
    const uint32_t pad_bytes = 1u << (format & PCF_GLYPH_PAD_MASK);
    uint32_t row_bytes = (width_bits + 7u) / 8u;

    row_bytes = (row_bytes + pad_bytes - 1u) & ~(pad_bytes - 1u);
    return row_bytes;
}

static int source_pixel_is_set(const uint8_t *row, uint32_t x, int msb_first)
{
    const uint32_t byte_index = x / 8u;
    const uint32_t bit_index = x % 8u;
    const uint8_t mask = msb_first ? (uint8_t)(0x80u >> bit_index) : (uint8_t)(1u << bit_index);

    return (row[byte_index] & mask) != 0u;
}

static void set_target_pixel(uint8_t *target_mask, uint32_t x)
{
    *target_mask = (uint8_t)(*target_mask | (uint8_t)(1u << (FONT_WIDTH - 1u - x)));
}

static void build_fallback(uint8_t rows[FONT_HEIGHT])
{
    uint32_t y = 0u;

    for (y = 0u; y < FONT_HEIGHT; ++y) {
        const uint32_t diagonal_a = (y * (FONT_WIDTH - 1u) + (FONT_HEIGHT - 1u) / 2u) / (FONT_HEIGHT - 1u);
        const uint32_t diagonal_b = ((FONT_HEIGHT - 1u - y) * (FONT_WIDTH - 1u) + (FONT_HEIGHT - 1u) / 2u) / (FONT_HEIGHT - 1u);

        rows[y] = 0u;
        if (y == 0u || y == FONT_HEIGHT - 1u) {
            rows[y] = 0x3fu;
        }

        set_target_pixel(&rows[y], 0u);
        set_target_pixel(&rows[y], FONT_WIDTH - 1u);
        set_target_pixel(&rows[y], diagonal_a);
        set_target_pixel(&rows[y], diagonal_b);
    }
}

static int build_font_rows(const ParsedFont *font, FontRows *rows)
{
    uint32_t code = 0u;
    uint32_t max_ascent = 0u;
    uint32_t max_descent = 0u;
    const int msb_first_bitmap_bits = (font->bitmaps.format & PCF_BIT_MASK) != 0u;

    memset(rows, 0, sizeof(*rows));

    for (code = 0u; code < SOURCE_GLYPH_COUNT; ++code) {
        const uint32_t glyph_index = font->encoding_to_glyph[code];
        const PcfMetric *metric = NULL;

        if (glyph_index == PCF_MISSING_GLYPH) {
            continue;
        }

        metric = font->metrics.items + glyph_index;

        if (metric->ascent < 0 || metric->descent < 0) {
            fprintf(stderr, "pcf_to_header: glyph %u has unsupported negative vertical metrics\n", glyph_index);
            return 0;
        }

        if ((uint32_t)metric->ascent > max_ascent) {
            max_ascent = (uint32_t)metric->ascent;
        }
        if ((uint32_t)metric->descent > max_descent) {
            max_descent = (uint32_t)metric->descent;
        }
    }

    if (max_ascent + max_descent != FONT_HEIGHT) {
        fprintf(stderr,
                "pcf_to_header: expected source metrics height %u, got ascent %u + descent %u\n",
                FONT_HEIGHT,
                max_ascent,
                max_descent);
        return 0;
    }

    rows->max_ascent = max_ascent;
    rows->max_descent = max_descent;

    for (code = 0u; code < SOURCE_GLYPH_COUNT; ++code) {
        const uint32_t glyph_index = font->encoding_to_glyph[code];
        const PcfMetric *metric = NULL;
        int source_width = 0;
        int source_height = 0;
        int target_y_offset = 0;
        uint32_t row_bytes = 0u;
        uint32_t source_y = 0u;
        uint32_t bitmap_offset = 0u;

        if (glyph_index == PCF_MISSING_GLYPH) {
            continue;
        }

        metric = font->metrics.items + glyph_index;
        source_width = metric->right_side_bearing - metric->left_side_bearing;
        source_height = metric->ascent + metric->descent;
        target_y_offset = (int)max_ascent - metric->ascent;

        if (source_width < 0 || source_height < 0) {
            fprintf(stderr, "pcf_to_header: glyph %u has unsupported negative dimensions\n", glyph_index);
            return 0;
        }

        row_bytes = padded_row_bytes((uint32_t)source_width, font->bitmaps.format);
        bitmap_offset = font->bitmaps.offsets[glyph_index];

        if ((size_t)bitmap_offset > font->bitmaps.bitmap_size ||
            (size_t)row_bytes * (size_t)source_height > font->bitmaps.bitmap_size - (size_t)bitmap_offset) {
            fprintf(stderr, "pcf_to_header: glyph %u bitmap extends outside bitmap table\n", glyph_index);
            return 0;
        }

        for (source_y = 0u; source_y < (uint32_t)source_height; ++source_y) {
            const int target_y = target_y_offset + (int)source_y;
            const uint8_t *source_row = font->bitmaps.bitmap_data + bitmap_offset + (size_t)source_y * row_bytes;
            uint32_t source_x = 0u;

            if (target_y < 0 || target_y >= (int)FONT_HEIGHT) {
                fprintf(stderr, "pcf_to_header: glyph %u row maps outside the 6x13 target cell\n", glyph_index);
                return 0;
            }

            for (source_x = 0u; source_x < (uint32_t)source_width; ++source_x) {
                const int target_x = metric->left_side_bearing + (int)source_x;

                if (target_x >= 0 && target_x < (int)FONT_WIDTH &&
                    source_pixel_is_set(source_row, source_x, msb_first_bitmap_bits)) {
                    set_target_pixel(&rows->masks[code][target_y], (uint32_t)target_x);
                }
            }
        }
    }

    build_fallback(rows->masks[FALLBACK_GLYPH_INDEX]);

    for (code = 0u; code < SOURCE_GLYPH_COUNT; ++code) {
        if (font->encoding_to_glyph[code] == PCF_MISSING_GLYPH) {
            memcpy(rows->masks[code], rows->masks[FALLBACK_GLYPH_INDEX], FONT_HEIGHT);
        }
    }

    for (code = 0u; code < TOTAL_GLYPH_COUNT; ++code) {
        uint32_t y = 0u;
        for (y = 0u; y < FONT_HEIGHT; ++y) {
            if (rows->masks[code][y] != 0u) {
                ++rows->non_empty_rows;
            }
        }
    }

    return 1;
}

static const char *basename_for_comment(const char *path)
{
    const char *last_slash = strrchr(path, '/');
    const char *last_backslash = strrchr(path, '\\');
    const char *base = path;

    if (last_slash != NULL && last_slash + 1 > base) {
        base = last_slash + 1;
    }
    if (last_backslash != NULL && last_backslash + 1 > base) {
        base = last_backslash + 1;
    }

    return base;
}

static int write_header(const char *output_path, const char *input_path, const FontRows *rows)
{
    FILE *file = fopen(output_path, "w");
    uint32_t glyph = 0u;
    const char *source_name = basename_for_comment(input_path);

    if (file == NULL) {
        fprintf(stderr, "pcf_to_header: failed to open output '%s': %s\n", output_path, strerror(errno));
        return 0;
    }

    fprintf(file, "#ifndef WINXTERM_FONT_6X13_H\n");
    fprintf(file, "#define WINXTERM_FONT_6X13_H\n\n");
    fprintf(file, "#include <stdint.h>\n\n");
    fprintf(file, "/* Generated by pcf_to_header from %s. */\n", source_name);
    fprintf(file, "/* Row mask bit 0x20 is the leftmost pixel; bit 0x01 is the rightmost pixel. */\n\n");
    fprintf(file, "#define WINXTERM_FONT_6X13_WIDTH 6u\n");
    fprintf(file, "#define WINXTERM_FONT_6X13_HEIGHT 13u\n");
    fprintf(file, "#define WINXTERM_FONT_6X13_SOURCE_GLYPH_COUNT 256u\n");
    fprintf(file, "#define WINXTERM_FONT_6X13_TOTAL_GLYPH_COUNT 257u\n");
    fprintf(file, "#define WINXTERM_FONT_6X13_FALLBACK_GLYPH_INDEX 256u\n");
    fprintf(file, "#define WINXTERM_FONT_6X13_ROW_MASK_LEFT_BIT 0x20u\n\n");
    fprintf(file, "typedef struct WinxtermFont6x13Row {\n");
    fprintf(file, "    uint8_t foreground_mask;\n");
    fprintf(file, "} WinxtermFont6x13Row;\n\n");
    fprintf(file, "static const WinxtermFont6x13Row winxterm_font_6x13_rows[WINXTERM_FONT_6X13_TOTAL_GLYPH_COUNT][WINXTERM_FONT_6X13_HEIGHT] = {\n");

    for (glyph = 0u; glyph < TOTAL_GLYPH_COUNT; ++glyph) {
        uint32_t y = 0u;
        if (glyph < SOURCE_GLYPH_COUNT) {
            fprintf(file, "    /* glyph 0x%02X */\n", glyph);
        } else {
            fprintf(file, "    /* missing-glyph fallback */\n");
        }
        fprintf(file, "    {\n");
        for (y = 0u; y < FONT_HEIGHT; ++y) {
            const uint8_t foreground_mask = rows->masks[glyph][y];
            fprintf(file,
                    "        { 0x%02xu }%s\n",
                    foreground_mask,
                    y + 1u == FONT_HEIGHT ? "" : ",");
        }
        fprintf(file, "    }%s\n", glyph + 1u == TOTAL_GLYPH_COUNT ? "" : ",");
    }

    fprintf(file, "};\n\n");
    fprintf(file, "#endif\n");

    if (fclose(file) != 0) {
        fprintf(stderr, "pcf_to_header: failed to finish output '%s'\n", output_path);
        return 0;
    }

    return 1;
}

int main(int argc, char **argv)
{
    Buffer input = { 0 };
    ParsedFont font = { 0 };
    FontRows rows = { 0 };
    int ok = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: pcf_to_header inputName.pcf outputName.h\n");
        return 2;
    }

    if (!load_file(argv[1], &input)) {
        return 1;
    }

    if (!parse_pcf(&input, &font)) {
        goto cleanup;
    }

    if (!build_font_rows(&font, &rows)) {
        goto cleanup;
    }

    if (!write_header(argv[2], argv[1], &rows)) {
        goto cleanup;
    }

    printf("Read PCF: %s\n", argv[1]);
    printf("Wrote header: %s\n", argv[2]);
    printf("Parsed: %u ISO slots, %u PCF glyphs, %u missing source slots, ascent=%u, descent=%u\n",
           SOURCE_GLYPH_COUNT,
           font.bitmaps.glyph_count,
           font.missing_source_glyphs,
           rows.max_ascent,
           rows.max_descent);
    printf("Generated: %u total glyphs, %u rows, fallback index %u, %u non-empty rows\n",
           TOTAL_GLYPH_COUNT,
           TOTAL_GLYPH_COUNT * FONT_HEIGHT,
           FALLBACK_GLYPH_INDEX,
           rows.non_empty_rows);

    ok = 1;

cleanup:
    free_parsed_font(&font);
    free(input.data);
    return ok ? 0 : 1;
}
