#include "winxterm_managed_runtime.h"

#include <string.h>

void winxterm_managed_runtime_registry_init(WinxtermManagedRuntimeRegistry *registry)
{
    if (registry == 0) return;
    memset(registry, 0, sizeof(*registry));
    InitializeCriticalSection(&registry->lock);
    registry->lock_initialized = true;
}

void winxterm_managed_runtime_registry_dispose(WinxtermManagedRuntimeRegistry *registry)
{
    if (registry == 0) return;
    if (registry->lock_initialized) DeleteCriticalSection(&registry->lock);
    memset(registry, 0, sizeof(*registry));
}
