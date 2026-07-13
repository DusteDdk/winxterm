#include "winxterm_job_plan.h"
#include "winxterm_job_protocol.h"

#include <stdlib.h>
#include <string.h>

static bool winxterm_job_plan_string_valid(const char *text)
{
    return text != 0 && strlen(text) <= WINXTERM_JOB_PLAN_MAX_STRING_BYTES;
}

static bool winxterm_job_plan_endpoint_valid(uint32_t endpoint)
{ return endpoint <= WINXTERM_JOB_PLAN_ENDPOINT_CONNECTABLE; }

static bool winxterm_job_plan_validate(const WinxtermJobExecutionPlan *plan)
{
    if (plan == 0 || !winxterm_job_plan_string_valid(plan->cwd) ||
        plan->stage_count == 0u || plan->stage_count > WINXTERM_JOB_PLAN_MAX_STAGES ||
        plan->environment_count > WINXTERM_JOB_PLAN_MAX_ENVIRONMENT ||
        (plan->flags & ~(WINXTERM_JOB_PLAN_FLAG_BACKGROUND |
                         WINXTERM_JOB_PLAN_FLAG_CONNECTABLE_STDIN)) != 0u) return false;
    for (size_t i = 0u; i < plan->environment_count; ++i) {
        const char *entry = plan->environment[i];
        const char *equals = entry != 0 ? strchr(entry, '=') : 0;
        if (!winxterm_job_plan_string_valid(entry) || equals == 0 || equals == entry) return false;
    }
    size_t arguments = 0u;
    for (size_t i = 0u; i < plan->stage_count; ++i) {
        const WinxtermJobPlanStage *stage = plan->stages + i;
        if (stage->argument_count == 0u ||
            arguments > WINXTERM_JOB_PLAN_MAX_ARGUMENTS - stage->argument_count ||
            !winxterm_job_plan_endpoint_valid((uint32_t)stage->stdin_endpoint) ||
            !winxterm_job_plan_endpoint_valid((uint32_t)stage->stdout_endpoint) ||
            !winxterm_job_plan_endpoint_valid((uint32_t)stage->stderr_endpoint) ||
            (stage->flags & ~(WINXTERM_JOB_STAGE_FLAG_ISOLATED_BUILTIN |
                              WINXTERM_JOB_STAGE_FLAG_APPEND |
                              WINXTERM_JOB_STAGE_FLAG_TEE)) != 0u) return false;
        arguments += stage->argument_count;
        for (size_t j = 0u; j < stage->argument_count; ++j) {
            if (!winxterm_job_plan_string_valid(stage->arguments[j])) return false;
        }
        if (stage->stdout_endpoint == WINXTERM_JOB_PLAN_ENDPOINT_FILE) {
            if (i + 1u != plan->stage_count || !winxterm_job_plan_string_valid(stage->path)) return false;
        } else if (stage->path != 0) return false;
        if ((i == 0u && stage->stdin_endpoint == WINXTERM_JOB_PLAN_ENDPOINT_PIPE) ||
            (i != 0u && stage->stdin_endpoint != WINXTERM_JOB_PLAN_ENDPOINT_PIPE) ||
            (i + 1u != plan->stage_count && stage->stdout_endpoint != WINXTERM_JOB_PLAN_ENDPOINT_PIPE) ||
            (i + 1u == plan->stage_count && stage->stdout_endpoint == WINXTERM_JOB_PLAN_ENDPOINT_PIPE)) return false;
    }
    return true;
}

static bool winxterm_job_plan_append_string(uint8_t *payload, size_t capacity, size_t *length,
                                            uint16_t type, const char *text)
{
    return winxterm_job_plan_string_valid(text) &&
           winxterm_job_tlv_append(payload, capacity, length, type, text, (uint32_t)strlen(text));
}

bool winxterm_job_plan_encode(const WinxtermJobExecutionPlan *plan,
                             uint8_t **payload, uint32_t *payload_length)
{
    if (payload != 0) *payload = 0;
    if (payload_length != 0) *payload_length = 0u;
    if (payload == 0 || payload_length == 0 || !winxterm_job_plan_validate(plan)) return false;
    uint8_t *out = (uint8_t *)malloc(WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD);
    uint8_t *stage_bytes = (uint8_t *)malloc(WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD);
    if (out == 0 || stage_bytes == 0) { free(out); free(stage_bytes); return false; }
    size_t length = 0u;
    bool ok = winxterm_job_tlv_append_u32(out, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD, &length,
                                         WINXTERM_JOB_TLV_FLAGS, plan->flags) &&
              winxterm_job_plan_append_string(out, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD, &length,
                                              WINXTERM_JOB_TLV_CWD, plan->cwd);
    for (size_t i = 0u; ok && i < plan->environment_count; ++i) {
        ok = winxterm_job_plan_append_string(out, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD, &length,
                                             WINXTERM_JOB_TLV_ENVIRONMENT, plan->environment[i]);
    }
    for (size_t i = 0u; ok && i < plan->stage_count; ++i) {
        const WinxtermJobPlanStage *stage = plan->stages + i;
        size_t stage_length = 0u;
        ok = winxterm_job_tlv_append_u32(stage_bytes, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                         &stage_length, WINXTERM_JOB_TLV_FLAGS, stage->flags) &&
             winxterm_job_tlv_append_u32(stage_bytes, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                         &stage_length, WINXTERM_JOB_TLV_ENDPOINT,
                                         (uint32_t)stage->stdin_endpoint) &&
             winxterm_job_tlv_append_u32(stage_bytes, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                         &stage_length, WINXTERM_JOB_TLV_ENDPOINT,
                                         (uint32_t)stage->stdout_endpoint) &&
             winxterm_job_tlv_append_u32(stage_bytes, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                         &stage_length, WINXTERM_JOB_TLV_ENDPOINT,
                                         (uint32_t)stage->stderr_endpoint);
        for (size_t j = 0u; ok && j < stage->argument_count; ++j) {
            ok = winxterm_job_plan_append_string(stage_bytes, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                                 &stage_length, WINXTERM_JOB_TLV_ARGUMENT,
                                                 stage->arguments[j]);
        }
        if (ok && stage->path != 0) {
            ok = winxterm_job_plan_append_string(stage_bytes, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                                 &stage_length, WINXTERM_JOB_TLV_PATH, stage->path);
        }
        if (ok) ok = winxterm_job_tlv_append(out, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD, &length,
                                             WINXTERM_JOB_TLV_STAGE, stage_bytes,
                                             (uint32_t)stage_length);
    }
    free(stage_bytes);
    if (!ok || length > UINT32_MAX) { free(out); return false; }
    uint8_t *shrunk = length != 0u ? (uint8_t *)realloc(out, length) : out;
    *payload = shrunk != 0 ? shrunk : out;
    *payload_length = (uint32_t)length;
    return true;
}

static bool winxterm_job_plan_copy_string(const WinxtermJobTlv *tlv, char **text)
{
    if (tlv == 0 || text == 0 || tlv->length > WINXTERM_JOB_PLAN_MAX_STRING_BYTES ||
        memchr(tlv->value, 0, tlv->length) != 0) return false;
    char *copy = (char *)malloc((size_t)tlv->length + 1u);
    if (copy == 0) return false;
    memcpy(copy, tlv->value, tlv->length);
    copy[tlv->length] = '\0';
    *text = copy;
    return true;
}

static bool winxterm_job_plan_decode_stage(const WinxtermJobTlv *container,
                                           WinxtermJobPlanStage *stage)
{
    WinxtermJobTlvReader reader;
    WinxtermJobTlv tlv;
    winxterm_job_tlv_reader_init(&reader, container->value, container->length);
    uint32_t endpoints[3] = {0u, 0u, 0u};
    size_t endpoint_count = 0u;
    bool found_flags = false;
    while (reader.offset < reader.length) {
        if (!winxterm_job_tlv_next(&reader, &tlv)) return false;
        if (tlv.type == WINXTERM_JOB_TLV_FLAGS) {
            if (found_flags || !winxterm_job_tlv_read_u32(&tlv, &stage->flags)) return false;
            found_flags = true;
        } else if (tlv.type == WINXTERM_JOB_TLV_ENDPOINT) {
            if (endpoint_count == 3u || !winxterm_job_tlv_read_u32(&tlv,
                                                                   endpoints + endpoint_count)) return false;
            ++endpoint_count;
        } else if (tlv.type == WINXTERM_JOB_TLV_ARGUMENT) {
            if (stage->argument_count == WINXTERM_JOB_PLAN_MAX_ARGUMENTS) return false;
            void *grown = realloc(stage->arguments,
                                  (stage->argument_count + 1u) * sizeof(*stage->arguments));
            if (grown == 0) return false;
            stage->arguments = (char **)grown;
            stage->arguments[stage->argument_count] = 0;
            if (!winxterm_job_plan_copy_string(&tlv, stage->arguments + stage->argument_count)) return false;
            ++stage->argument_count;
        } else if (tlv.type == WINXTERM_JOB_TLV_PATH) {
            if (stage->path != 0 || !winxterm_job_plan_copy_string(&tlv, &stage->path)) return false;
        } else return false;
    }
    if (!found_flags || endpoint_count != 3u) return false;
    stage->stdin_endpoint = (WinxtermJobPlanEndpoint)endpoints[0];
    stage->stdout_endpoint = (WinxtermJobPlanEndpoint)endpoints[1];
    stage->stderr_endpoint = (WinxtermJobPlanEndpoint)endpoints[2];
    return true;
}

bool winxterm_job_plan_decode(const uint8_t *payload, size_t payload_length,
                             WinxtermJobExecutionPlan *plan)
{
    if (plan == 0 || (payload == 0 && payload_length != 0u) ||
        payload_length > WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD) return false;
    memset(plan, 0, sizeof(*plan));
    WinxtermJobTlvReader reader;
    WinxtermJobTlv tlv;
    bool found_flags = false, ok = true;
    winxterm_job_tlv_reader_init(&reader, payload, payload_length);
    while (ok && reader.offset < reader.length && winxterm_job_tlv_next(&reader, &tlv)) {
        if (tlv.type == WINXTERM_JOB_TLV_FLAGS) {
            ok = !found_flags && winxterm_job_tlv_read_u32(&tlv, &plan->flags);
            found_flags = true;
        } else if (tlv.type == WINXTERM_JOB_TLV_CWD) {
            ok = plan->cwd == 0 && winxterm_job_plan_copy_string(&tlv, &plan->cwd);
        } else if (tlv.type == WINXTERM_JOB_TLV_ENVIRONMENT) {
            if (plan->environment_count == WINXTERM_JOB_PLAN_MAX_ENVIRONMENT) { ok = false; break; }
            void *grown = realloc(plan->environment,
                                  (plan->environment_count + 1u) * sizeof(*plan->environment));
            if (grown == 0) { ok = false; break; }
            plan->environment = (char **)grown;
            plan->environment[plan->environment_count] = 0;
            ok = winxterm_job_plan_copy_string(&tlv,
                                               plan->environment + plan->environment_count);
            if (ok) ++plan->environment_count;
        } else if (tlv.type == WINXTERM_JOB_TLV_STAGE) {
            if (plan->stage_count == WINXTERM_JOB_PLAN_MAX_STAGES) { ok = false; break; }
            void *grown = realloc(plan->stages, (plan->stage_count + 1u) * sizeof(*plan->stages));
            if (grown == 0) { ok = false; break; }
            plan->stages = (WinxtermJobPlanStage *)grown;
            memset(plan->stages + plan->stage_count, 0, sizeof(*plan->stages));
            ok = winxterm_job_plan_decode_stage(&tlv, plan->stages + plan->stage_count);
            if (ok) ++plan->stage_count;
        } else ok = false;
    }
    ok = ok && reader.offset == reader.length && found_flags && winxterm_job_plan_validate(plan);
    if (!ok) winxterm_job_plan_dispose(plan);
    return ok;
}

void winxterm_job_plan_dispose(WinxtermJobExecutionPlan *plan)
{
    if (plan == 0) return;
    free(plan->cwd);
    for (size_t i = 0u; i < plan->environment_count; ++i) free(plan->environment[i]);
    free(plan->environment);
    for (size_t i = 0u; i < plan->stage_count; ++i) {
        for (size_t j = 0u; j < plan->stages[i].argument_count; ++j) free(plan->stages[i].arguments[j]);
        free(plan->stages[i].arguments);
        free(plan->stages[i].path);
    }
    free(plan->stages);
    memset(plan, 0, sizeof(*plan));
}
