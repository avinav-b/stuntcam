// SPDX-License-Identifier: Apache-2.0
// libu4mark.dylib: stands in for a tool that inserts its own library per launch (Detox's
// SIMCTL_CHILD_DYLD_INSERT_LIBRARIES, idb). Its constructor appends "<pid> loaded <exe>" to $U4MARK_OUT (if set).
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <mach-o/dyld.h>
__attribute__((constructor)) static void u4mark(void) {
    const char *o = getenv("U4MARK_OUT"); if (!o) return;
    char exe[1024]; uint32_t sz = sizeof exe; if (_NSGetExecutablePath(exe, &sz)) exe[0] = 0;
    FILE *f = fopen(o, "a"); if (f) { fprintf(f, "%d loaded %s\n", getpid(), exe); fclose(f); }
}
