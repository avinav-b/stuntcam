// SPDX-License-Identifier: Apache-2.0
// simcam boot stub: frozen, libSystem only (design §4.1, B-MF4). Inserted at boot into every simulator process; it returns at
// once unless the process is an app (an executable in a .app bundle outside the runtime root) and SIMCAM != 0. Then it reads
// $SIMCAM_SIM_DIR/current, one line "<proto> <absolute path of libsimcamloader.dylib>", and dlopens that loader, which does
// the rest (restore rules, chain file, engine). Rewriting `current` upgrades the next launch, no reboot. A newer proto than
// STUB_PROTO: one log line, nothing loaded. SIMCAM_LOADER=<path> bypasses the file. SIMCAM_STUB_LOG=<file> logs.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <mach-o/dyld.h>
#define STUB_PROTO 1
static int gfd = -1;
static void slog(const char *a, const char *b) {
    if (gfd < 0) return;
    char buf[1400]; struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    int n = snprintf(buf, sizeof buf, "%ld.%06ld [stub %d] %s%s\n", (long)ts.tv_sec, ts.tv_nsec / 1000, getpid(), a, b ? b : "");
    write(gfd, buf, n > (int)sizeof buf ? (int)sizeof buf : n);
}
static void load(const char *exe) {   // every early return lands back in the constructor, which closes the log fd
    const char *on = getenv("SIMCAM");
    if (on && on[0] == '0') { slog("SIMCAM=0, nothing loaded: ", exe); return; }
    slog("app process ", exe);
    static char line[1100]; const char *path = getenv("SIMCAM_LOADER"); int proto = STUB_PROTO;
    if (!path) {
        const char *dir = getenv("SIMCAM_SIM_DIR");
        if (!dir) { slog("no SIMCAM_SIM_DIR; nothing loaded", 0); return; }
        char cf[1100]; snprintf(cf, sizeof cf, "%s/current", dir);
        int fd = open(cf, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { slog("no current file; nothing loaded: ", cf); return; }
        ssize_t n = read(fd, line, sizeof line - 1); close(fd);
        if (n <= 0) { slog("empty current file; nothing loaded: ", cf); return; }
        line[n] = 0; char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *sp = strchr(line, ' ');
        if (!sp) { slog("current file is not '<proto> <path>'; nothing loaded: ", line); return; }
        *sp = 0; proto = atoi(line); path = sp + 1;
    }
    if (proto < 1 || path[0] != '/') { slog("current file needs '<proto >= 1> <absolute loader path>'; nothing loaded: ", path); return; }
    if (proto > STUB_PROTO) { slog("current file needs a newer stub (reboot the sim with a newer simcam); nothing loaded: ", path); return; }
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    slog(h ? "loader loaded: " : "loader failed: ", h ? path : dlerror());
}
__attribute__((constructor)) static void simcam_stub(void) {
    static char exe[1024]; uint32_t sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &sz) != 0) return;   // (_dyld_get_image_name(0) is the stub itself at this point)
    // an app = an executable inside a .app bundle, in the device's data dir or an Xcode build-products dir, and not under the
    // runtime root (system apps live there). Not tied to /Containers/Bundle/Application/: a hosted-test host was once
    // launched through a containermanagerd/Dead/ hard-link alias of its binary, failed that check and hung xcodebuild.
    const char *root = getenv("SIMULATOR_ROOT"); if (!root) root = getenv("DYLD_ROOT_PATH");
    if (!strstr(exe, ".app/") || (!strstr(exe, "/data/") && !strstr(exe, "/Build/Products/")) || (root && *root && !strncmp(exe, root, strlen(root)))) return;
    const char *lg = getenv("SIMCAM_STUB_LOG");
    if (lg) gfd = open(lg, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    load(exe);
    if (gfd >= 0) { close(gfd); gfd = -1; }
}
