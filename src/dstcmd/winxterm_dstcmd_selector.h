#ifndef WINXTERM_DSTCMD_SELECTOR_H
#define WINXTERM_DSTCMD_SELECTOR_H

#include "dstcmd/winxterm_dstcmd.h"

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

typedef bool (*WinxtermDstcmdSelectorRefresh)(void *context,
                                              const wchar_t *const **items,
                                              size_t *item_count);

const char *winxterm_dstcmd_selector_contains_utf8(const char *text,
                                                   const char *query,
                                                   bool smart_case);
bool winxterm_dstcmd_selector_fuzzy_utf8(const char *text, const char *query,
                                        bool smart_case, int *gap_penalty);

bool winxterm_dstcmd_selector_select(WinxtermDstcmdShell *shell,
                                    const wchar_t *title,
                                    const wchar_t *const *items,
                                    size_t item_count,
                                    size_t visible_rows,
                                    size_t *selected_index);
bool winxterm_dstcmd_selector_select_dynamic(WinxtermDstcmdShell *shell,
                                            const wchar_t *title,
                                            const wchar_t *const *items,
                                            size_t item_count,
                                            size_t visible_rows,
                                            HANDLE refresh_event,
                                            WinxtermDstcmdSelectorRefresh refresh,
                                            void *refresh_context,
                                            size_t *selected_index);
bool winxterm_dstcmd_selector_select_menu(WinxtermDstcmdShell *shell,
                                         const wchar_t *title,
                                         const wchar_t *const *items,
                                         const bool *enabled,
                                         size_t item_count,
                                         size_t *selected_index);

#endif
