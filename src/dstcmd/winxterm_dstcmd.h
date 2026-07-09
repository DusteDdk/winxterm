#ifndef WINXTERM_DSTCMD_H
#define WINXTERM_DSTCMD_H

#include "dstcmd/api/scratch.h"
#include "dstcmd/winxterm_dstcmd_jobs.h"
#include "winxterm_diagnostics.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

#define WINXTERM_DSTCMD_PATH_CAPACITY 32768u
#define WINXTERM_DSTCMD_LINE_CAPACITY 4096u
#define WINXTERM_DSTCMD_STACK_CAPACITY 64u
#define WINXTERM_DSTCMD_HISTORY_CAPACITY 1000u
#define WINXTERM_DSTCMD_ESCAPE_CAPACITY 32u
#define WINXTERM_DSTCMD_PENDING_INPUT_CAPACITY 4096u
#define WINXTERM_DSTCMD_HISTORY_SEARCH_STATUS_CAPACITY 128u

typedef enum WinxtermDstcmdTimingMode {
    WINXTERM_DSTCMD_TIMING_OFF = 0,
    WINXTERM_DSTCMD_TIMING_BASIC,
    WINXTERM_DSTCMD_TIMING_VERBOSE
} WinxtermDstcmdTimingMode;

typedef enum WinxtermDstcmdDirRefreshSource {
    WINXTERM_DSTCMD_DIR_REFRESH_CD = 0,
    WINXTERM_DSTCMD_DIR_REFRESH_LS
} WinxtermDstcmdDirRefreshSource;

typedef struct WinxtermDstcmdDirEntry {
    wchar_t *name;
    DWORD attributes;
    ULONGLONG size;
    FILETIME write_time;
} WinxtermDstcmdDirEntry;

typedef struct WinxtermDstcmdDirSnapshot {
    wchar_t path[WINXTERM_DSTCMD_PATH_CAPACITY];
    WinxtermDstcmdDirEntry *entries;
    size_t count;
    bool valid;
} WinxtermDstcmdDirSnapshot;

typedef struct WinxtermDstcmdOutputBuilder {
    wchar_t *chars;
    size_t count;
    size_t capacity;
    bool failed;
} WinxtermDstcmdOutputBuilder;

typedef struct WinxtermDstcmdDirCache {
    CRITICAL_SECTION lock;
    WinxtermDstcmdDirSnapshot snapshot;
    bool lock_initialized;
} WinxtermDstcmdDirCache;

typedef struct WinxtermDstcmdAlias {
    wchar_t *name;
    wchar_t *target;
    wchar_t *description;
} WinxtermDstcmdAlias;

typedef struct WinxtermDstcmdHistoryPendingWrite {
    struct WinxtermDstcmdHistoryPendingWrite *next;
    char *command;
    DWORD pid;
    int64_t invocation_date_ms;
    unsigned int attempts;
} WinxtermDstcmdHistoryPendingWrite;

typedef enum WinxtermDstcmdHistorySearchRanking {
    WINXTERM_DSTCMD_HISTORY_SEARCH_RANK_BEST = 0,
    WINXTERM_DSTCMD_HISTORY_SEARCH_RANK_RECENT
} WinxtermDstcmdHistorySearchRanking;

typedef enum WinxtermDstcmdHistorySearchMatching {
    WINXTERM_DSTCMD_HISTORY_SEARCH_MATCH_FUZZY = 0,
    WINXTERM_DSTCMD_HISTORY_SEARCH_MATCH_CONTAINS
} WinxtermDstcmdHistorySearchMatching;

typedef struct WinxtermDstcmdHistorySearchCandidate {
    char *command;
    int64_t last_invocation_ms;
    unsigned int invocation_count;
    size_t order;
    bool session;
} WinxtermDstcmdHistorySearchCandidate;

typedef struct WinxtermDstcmdHistorySearchResult {
    size_t candidate_index;
    int score;
} WinxtermDstcmdHistorySearchResult;

typedef struct WinxtermDstcmdShell {
    HANDLE shutdown_event;
    HANDLE input_handle;
    HANDLE output_handle;
    HANDLE error_handle;
    DWORD original_input_console_mode;
    DWORD shell_input_console_mode;
    DWORD original_output_console_mode;
    DWORD shell_output_console_mode;
    DWORD original_error_console_mode;
    DWORD shell_error_console_mode;
    WinxtermDstcmdJobPool jobs;
    WinxtermDstcmdDirCache dir_cache;
    wchar_t cwd[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t previous_cwd[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t *directory_stack[WINXTERM_DSTCMD_STACK_CAPACITY];
    size_t directory_stack_count;
    bool cwd_env_sync_enabled;
    WinxtermDstcmdAlias *aliases;
    size_t alias_count;
    char line[WINXTERM_DSTCMD_LINE_CAPACITY];
    size_t line_length;
    size_t line_cursor;
    char completion_repeat_line[WINXTERM_DSTCMD_LINE_CAPACITY];
    size_t completion_repeat_cursor;
    char saved_history_line[WINXTERM_DSTCMD_LINE_CAPACITY];
    char history_search_original_line[WINXTERM_DSTCMD_LINE_CAPACITY];
    char history_search_query[WINXTERM_DSTCMD_LINE_CAPACITY];
    char history_search_pending_delete[WINXTERM_DSTCMD_LINE_CAPACITY];
    char history_search_undo_delete[WINXTERM_DSTCMD_LINE_CAPACITY];
    char history_search_status[WINXTERM_DSTCMD_HISTORY_SEARCH_STATUS_CAPACITY];
    char *history[WINXTERM_DSTCMD_HISTORY_CAPACITY];
    size_t history_count;
    size_t history_index;
    size_t history_search_original_cursor;
    size_t history_search_query_length;
    size_t history_search_query_cursor;
    size_t history_search_selected;
    size_t history_search_scroll;
    size_t history_search_candidate_count;
    size_t history_search_result_count;
    int history_search_overlay_columns;
    int history_search_overlay_rows;
    size_t history_search_overlay_reserved_rows;
    int64_t history_search_undo_last_invocation_ms;
    unsigned int history_search_undo_invocation_count;
    WinxtermDstcmdHistorySearchCandidate history_search_candidates[WINXTERM_DSTCMD_HISTORY_CAPACITY];
    WinxtermDstcmdHistorySearchResult history_search_results[WINXTERM_DSTCMD_HISTORY_CAPACITY];
    char *persisted_history[WINXTERM_DSTCMD_HISTORY_CAPACITY];
    size_t persisted_history_count;
    size_t persisted_history_index;
    wchar_t history_db_path[WINXTERM_DSTCMD_PATH_CAPACITY];
    sqlite3 *history_db;
    sqlite3_stmt *history_upsert_stmt;
    sqlite3_stmt *history_query_stmt;
    sqlite3_stmt *history_search_stmt;
    WinxtermDstcmdHistoryPendingWrite *history_write_head;
    WinxtermDstcmdHistoryPendingWrite *history_write_tail;
    HANDLE history_retry_event;
    HANDLE history_retry_thread;
    CRITICAL_SECTION history_db_lock;
    CRITICAL_SECTION history_state_lock;
    char escape_sequence[WINXTERM_DSTCMD_ESCAPE_CAPACITY];
    size_t escape_sequence_length;
    size_t prompt_rows_before_input;
    bool prompt_cursor_saved;
    uint8_t *capture_bytes;
    size_t capture_count;
    size_t capture_capacity;
    uint8_t pending_input[WINXTERM_DSTCMD_PENDING_INPUT_CAPACITY];
    size_t pending_input_count;
    HANDLE stream_input_handle;
    HANDLE stream_output_handle;
    CRITICAL_SECTION output_lock;
    WinxtermDstcmdScratch scratch;
    uint64_t timing_start_ns;
    WinxtermCommandDiagnostics timing_diagnostics;
    WinxtermCommandDiagnostics timing_snapshot;
    int last_status;
    bool output_lock_initialized;
    bool command_running;
    bool prompt_active;
    bool capture_active;
    bool capture_failed;
    bool stream_output_failed;
    bool disposing;
    bool input_console_mode_saved;
    bool output_console_mode_saved;
    bool error_console_mode_saved;
    bool completion_repeat_valid;
    bool history_db_lock_initialized;
    bool history_state_lock_initialized;
    bool history_retry_shutdown;
    bool history_db_ready;
    bool history_db_disabled;
    bool history_search_active;
    bool history_search_overlay_active;
    bool history_search_delete_confirm_active;
    bool history_search_undo_available;
    bool persisted_history_active;
    bool persisted_history_loaded;
    bool persisted_history_refresh_requested;
    unsigned int persisted_history_refresh_attempts;
    WinxtermDstcmdHistorySearchRanking history_search_ranking;
    WinxtermDstcmdHistorySearchMatching history_search_matching;
    WinxtermDstcmdTimingMode timing_mode;
    bool timing_command_active;
    bool timing_verbose_active;
    bool exit_requested;
    unsigned int host_request_id;
} WinxtermDstcmdShell;

bool winxterm_dstcmd_shell_init(WinxtermDstcmdShell *shell);
void winxterm_dstcmd_shell_dispose(WinxtermDstcmdShell *shell);
DWORD winxterm_dstcmd_run(void);
DWORD winxterm_dstcmd_run_with_notice(const wchar_t *notice);
int winxterm_dstcmd_smoke_run(void);
void winxterm_dstcmd_shell_enter_line_editor_mode(WinxtermDstcmdShell *shell);
void winxterm_dstcmd_shell_enter_foreground_child_mode(WinxtermDstcmdShell *shell);
void winxterm_dstcmd_shell_restore_original_console_modes(WinxtermDstcmdShell *shell);
int winxterm_dstcmd_shell_submit_line(WinxtermDstcmdShell *shell, const wchar_t *line);
bool winxterm_dstcmd_shell_set_cwd(WinxtermDstcmdShell *shell, const wchar_t *path);
const wchar_t *winxterm_dstcmd_shell_cwd(const WinxtermDstcmdShell *shell);
const WinxtermDstcmdAlias *winxterm_dstcmd_shell_find_alias(const WinxtermDstcmdShell *shell,
                                                           const wchar_t *name);
bool winxterm_dstcmd_shell_set_alias(WinxtermDstcmdShell *shell,
                                     const wchar_t *name,
                                     const wchar_t *target,
                                     const wchar_t *description);
bool winxterm_dstcmd_shell_clone_aliases(WinxtermDstcmdShell *shell,
                                        const WinxtermDstcmdShell *source);
void winxterm_dstcmd_shell_dispose_aliases(WinxtermDstcmdShell *shell);
bool winxterm_dstcmd_shell_refresh_line(WinxtermDstcmdShell *shell);
bool winxterm_dstcmd_shell_write_bytes(WinxtermDstcmdShell *shell,
                                       const uint8_t *bytes,
                                       size_t byte_count);
bool winxterm_dstcmd_shell_write_utf8(WinxtermDstcmdShell *shell, const char *text);
bool winxterm_dstcmd_shell_write_wide(WinxtermDstcmdShell *shell, const wchar_t *text);
bool winxterm_dstcmd_shell_write_widef(WinxtermDstcmdShell *shell, const wchar_t *format, ...);
bool winxterm_dstcmd_shell_write_error_wide(WinxtermDstcmdShell *shell, const wchar_t *text);
bool winxterm_dstcmd_shell_write_error_widef(WinxtermDstcmdShell *shell, const wchar_t *format, ...);
bool winxterm_dstcmd_shell_set_title_wide(WinxtermDstcmdShell *shell, const wchar_t *title);
bool winxterm_dstcmd_shell_update_cwd_title(WinxtermDstcmdShell *shell);
size_t winxterm_dstcmd_shell_read_input(WinxtermDstcmdShell *shell,
                                        uint8_t *buffer,
                                        size_t buffer_capacity,
                                        bool wait);
int winxterm_dstcmd_shell_terminal_columns(const WinxtermDstcmdShell *shell);
uint64_t winxterm_dstcmd_shell_timestamp_ns(void);
void winxterm_dstcmd_output_builder_init(WinxtermDstcmdOutputBuilder *builder);
void winxterm_dstcmd_output_builder_dispose(WinxtermDstcmdOutputBuilder *builder);
bool winxterm_dstcmd_output_builder_append_wide(WinxtermDstcmdOutputBuilder *builder,
                                                const wchar_t *text);
bool winxterm_dstcmd_output_builder_append_widef(WinxtermDstcmdOutputBuilder *builder,
                                                 const wchar_t *format,
                                                 ...);
bool winxterm_dstcmd_output_builder_append_repeat(WinxtermDstcmdOutputBuilder *builder,
                                                  wchar_t ch,
                                                  size_t count);
bool winxterm_dstcmd_output_builder_flush(WinxtermDstcmdOutputBuilder *builder,
                                          WinxtermDstcmdShell *shell);
void winxterm_dstcmd_shell_notify_async_error(WinxtermDstcmdShell *shell, const wchar_t *message);
bool winxterm_dstcmd_shell_notify_async_widef(WinxtermDstcmdShell *shell,
                                             const wchar_t *format,
                                             ...);
bool winxterm_dstcmd_shell_notify_async_output_widef(WinxtermDstcmdShell *shell,
                                                    const wchar_t *format,
                                                    ...);
void winxterm_dstcmd_dir_snapshot_dispose(WinxtermDstcmdDirSnapshot *snapshot);
bool winxterm_dstcmd_dir_snapshot_clone(const WinxtermDstcmdDirSnapshot *snapshot,
                                        WinxtermDstcmdDirSnapshot *out);
bool winxterm_dstcmd_shell_copy_dir_cache(WinxtermDstcmdShell *shell,
                                          WinxtermDstcmdDirSnapshot *out);
void winxterm_dstcmd_shell_invalidate_dir_cache(WinxtermDstcmdShell *shell);
bool winxterm_dstcmd_shell_schedule_dir_cache_refresh(WinxtermDstcmdShell *shell,
                                                      WinxtermDstcmdDirRefreshSource source);
bool winxterm_dstcmd_shell_init_dir_cache(WinxtermDstcmdShell *shell);
void winxterm_dstcmd_shell_dispose_dir_cache(WinxtermDstcmdShell *shell);

#endif
