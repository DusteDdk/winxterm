#ifndef WINXTERM_DSTCMD_API_PATH_H
#define WINXTERM_DSTCMD_API_PATH_H

#include "dstcmd/api/scratch.h"

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>
#include <windows.h>

typedef struct WinxtermDstcmdWin32Path {
    const wchar_t *display;
    wchar_t *native;
    wchar_t *syscall;
    bool extended;
} WinxtermDstcmdWin32Path;

typedef struct WinxtermDstcmdPathInfo {
    DWORD attributes;
    ULONGLONG size;
    FILETIME write_time;
    DWORD error;
    bool exists;
} WinxtermDstcmdPathInfo;

typedef struct WinxtermDstcmdDirIter {
    HANDLE handle;
    WIN32_FIND_DATAW data;
    DWORD error;
    bool first;
} WinxtermDstcmdDirIter;

bool winxterm_dstcmd_path_resolve(const wchar_t *cwd,
                                  const wchar_t *operand,
                                  wchar_t *out,
                                  size_t out_count);
bool winxterm_dstcmd_path_resolve_scratch(WinxtermDstcmdScratch *scratch,
                                          const wchar_t *cwd,
                                          const wchar_t *operand,
                                          wchar_t *out,
                                          size_t out_count);
bool winxterm_dstcmd_path_resolve_full_scratch(WinxtermDstcmdScratch *scratch,
                                               const wchar_t *cwd,
                                               const wchar_t *operand,
                                               wchar_t *out,
                                               size_t out_count);
bool winxterm_dstcmd_path_prepare_win32_scratch(WinxtermDstcmdScratch *scratch,
                                                const wchar_t *path,
                                                WinxtermDstcmdWin32Path *out);
bool winxterm_dstcmd_path_from_win32_display(const wchar_t *path,
                                             wchar_t *out,
                                             size_t out_count);
bool winxterm_dstcmd_path_get_info_scratch(WinxtermDstcmdScratch *scratch,
                                           const wchar_t *path,
                                           WinxtermDstcmdPathInfo *info);
bool winxterm_dstcmd_path_is_directory(const wchar_t *path);
bool winxterm_dstcmd_path_is_directory_scratch(WinxtermDstcmdScratch *scratch,
                                               const wchar_t *path);
bool winxterm_dstcmd_path_set_attributes_scratch(WinxtermDstcmdScratch *scratch,
                                                 const wchar_t *path,
                                                 DWORD attributes);
bool winxterm_dstcmd_path_create_directory_scratch(WinxtermDstcmdScratch *scratch,
                                                   const wchar_t *path);
bool winxterm_dstcmd_path_copy_file_scratch(WinxtermDstcmdScratch *scratch,
                                            const wchar_t *source,
                                            const wchar_t *destination,
                                            bool fail_if_exists);
bool winxterm_dstcmd_path_move_file_scratch(WinxtermDstcmdScratch *scratch,
                                            const wchar_t *source,
                                            const wchar_t *destination,
                                            DWORD flags);
bool winxterm_dstcmd_path_delete_file_scratch(WinxtermDstcmdScratch *scratch,
                                              const wchar_t *path);
bool winxterm_dstcmd_path_remove_directory_scratch(WinxtermDstcmdScratch *scratch,
                                                   const wchar_t *path);
bool winxterm_dstcmd_dir_iter_open_scratch(WinxtermDstcmdScratch *scratch,
                                           const wchar_t *directory,
                                           WinxtermDstcmdDirIter *iter);
bool winxterm_dstcmd_dir_iter_next(WinxtermDstcmdDirIter *iter,
                                   const WIN32_FIND_DATAW **data);
void winxterm_dstcmd_dir_iter_close(WinxtermDstcmdDirIter *iter);
bool winxterm_dstcmd_path_append_child(const wchar_t *directory,
                                       const wchar_t *name,
                                       wchar_t *out,
                                       size_t out_count);
void winxterm_dstcmd_path_trim_trailing_separators(wchar_t *path);
const wchar_t *winxterm_dstcmd_path_basename(const wchar_t *path);
bool winxterm_dstcmd_path_to_display(const wchar_t *path, wchar_t *out, size_t out_count);
bool winxterm_dstcmd_path_to_native(const wchar_t *path, wchar_t *out, size_t out_count);

#endif
