// SPDX-License-Identifier: Apache-2.0
// simcam loader (libsimcamloader.dylib): dlopened by the frozen boot stub from the path in $SIMCAM_SIM_DIR/current, so
// it can change without a reboot (design §4.1, B-MF4). libSystem only (it runs inside the stub's constructor). In app
// processes (an executable in a .app bundle, in the device's data dir or an Xcode build-products dir, not under the
// runtime root) it:
//   1. restores what Xcode/XCTest would have inserted (the boot env's DYLD_INSERT_LIBRARIES displaced it):
//      XCTestBundleInjectPath set (hosted tests)  -> exactly that list;
//      else XCODE_SCHEME_NAME set (UI-test target) -> the rules file's "ui-test-target" libs;
//      else Xcode-launched (MTC_/PERFC_/IDE_ vars, DYLD_LIBRARY_PATH in DerivedData) -> its "xcode-run" libs.
//      The rules file is <rules_dir>/<xcode_build>.json, the build being the one `stuntcam up` (or scripts/boot.sh)
//      recorded in $SIMCAM_SIM_DIR/config.json, and rules_dir the per-content dir it staged
//      ($SIMCAM_HOME/restore-rules/<hash>/, so an armed sim's rules never change under it): under ⌘R nothing in the app
//      names the launching Xcode, so the arm-time record is the source. No record or no file for that build: the newest
//      rules file (else the built-in 27A266a rules) plus ONE line, "restore rules unverified for Xcode <build>; …"
//      (design §4.1 "Which Xcode's libs", B-MF5).
//      Never dlopened, in any rule or the chain: libraries that only work when inserted at launch (sanitizer runtimes
//      libclang_rt.*, libgmalloc). A sanitized app links its own runtime; dlopening the test bundle's copy aborts ASan
//      and maps a second TSan. Nor a second copy of a library whose basename is already mapped from another path. Both
//      are logged in one line, "skipped (must load at launch, or a second copy)"; a chain entry skipped as a second
//      copy is also said on stderr (the chain is the user's own list).
//      SIMCAM_XCODE_LIBS=0 skips all restores; SIMCAM_MTC=0 skips MTC.
//   2. says, in ONE line on stderr and in the log, when boot mode displaced another tool's per-launch insert (design
//      §4.1 rule 4): Detox launch arguments, IDB_* variables, sanitizer / Guard Malloc settings with their runtime
//      absent, a launch-only library from step 1 that isn't mapped, while the insert list is our stub alone (a launch
//      that merged its list with ours displaced nothing). A signal variable whose NAME=value config.json lists in
//      "boot_signals" (it was in the boot environment, so every process has it) is not a signal.
//   3. dlopens the engine: $SIMCAM_DYLIB, else libsimcam.dylib next to this loader (a build dir holds a matching pair).
//      Not in *-Runner.app (UI-test runners) or app extensions (.appex).
//   4. dlopens each line of the chain file ($SIMCAM_CHAIN, else $SIMCAM_SIM_DIR/chain.txt), so tools that arrive after
//      boot can still add libraries without a reboot. Lines starting with '#' and our own libraries (libsimcam*) are
//      skipped.
// SIMCAM=0 -> nothing. SIMCAM_STUB_LOG=<file> logs; SIMCAM_STUB_ENVDUMP=1 also logs the whole env.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <crt_externs.h>
#include <mach-o/dyld.h>
extern char **environ;

// Buffer sizes. Every buffer is fixed-size and static or on the stack: the loader allocates nothing.
enum {
    LOADER_PATH_MAX = 1024,      // one path (an executable, a library, a directory)
    PATH_NOTE_MAX = 1100,        // a path plus a short suffix or note ("<dir>/config.json", "<path> [second copy; …]")
    LOG_LINE_MAX = 4096,         // one stub.log line
    SAID_LINE_MAX = 1024,        // one line said on stderr
    LIST_LINE_MAX = 1024,        // the one-line lists of skipped libraries
    INSERT_LIST_MAX = 8192,      // a ':'-separated insert list (XCTestBundleInjectPath) or the chain file
    INSERT_ENV_COPY_MAX = 4096,  // the copy of DYLD_INSERT_LIBRARIES that detect_displaced() splits
    BOOT_SIGNAL_MAX = 4096,      // one config.json "boot_signals" entry
    CONFIG_FILE_MAX = 16384,     // $SIMCAM_SIM_DIR/config.json
    RULES_FILE_MAX = 32768,      // a restore-rules/<build>.json file
    INJECT_LIST_ENTRIES_MAX = 32,  // distinct XCTestBundleInjectPath entries restored
};
#define CLANG_RT_PREFIX "libclang_rt."
enum { CLANG_RT_PREFIX_LEN = sizeof CLANG_RT_PREFIX - 1 };   // 12
enum { SUPPORTED_CONFIG_PROTO = 1 };   // the config.json "proto" this loader reads

static int gfd = -1;
static void slog(const char *a, const char *b) {
    if (gfd < 0) return;
    char buf[LOG_LINE_MAX];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int n = snprintf(buf, sizeof buf, "%ld.%06ld [loader %d] %s%s\n", (long)ts.tv_sec, ts.tv_nsec / 1000, getpid(), a,
                     b ? b : "");
    write(gfd, buf, n > (int)sizeof buf ? (int)sizeof buf : n);
}
// a user-facing line: stderr (Xcode's console, `simctl launch --console-pty`) and the log
static void say(const char *line) {
    char buf[SAID_LINE_MAX];
    int n = snprintf(buf, sizeof buf, "%s\n", line);
    write(2, buf, n > (int)sizeof buf ? (int)sizeof buf : n);
    slog("said: ", line);
}
static int hasprefix(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static const char *base(const char *p) {
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

// ---- images already mapped -------------------------------------------------------------------------------------------
// the path of a mapped image whose basename starts with pre (exact basename when exact), else NULL
static const char *mapped(const char *pre, int exact) {
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
        const char *nm = _dyld_get_image_name(i);
        if (!nm) continue;
        const char *b = base(nm);
        if (exact ? !strcmp(b, pre) : hasprefix(b, pre)) return nm;
    }
    return NULL;
}

// ---- libraries that only work inserted at launch ---------------------------------------------------------------------
// a launch-only library (a sanitizer runtime or Guard Malloc): never dlopened. runnerhook.m's launchOnly() is the same
// test; restore-rules/*.json documents it as "never_dlopened".
static int launch_only(const char *path) {
    const char *b = base(path);
    return hasprefix(b, CLANG_RT_PREFIX) || !strcmp(b, "libgmalloc.dylib");
}
static const char *toolof(const char *path) {   // the tool a launch-only library belongs to (for the displaced line)
    const char *b = base(path);
    if (hasprefix(b, "libclang_rt.asan")) return "AddressSanitizer";
    if (hasprefix(b, "libclang_rt.tsan")) return "ThreadSanitizer";
    if (hasprefix(b, "libclang_rt.ubsan")) return "UndefinedBehaviorSanitizer";
    if (!strcmp(b, "libgmalloc.dylib")) return "Guard Malloc";
    return "a sanitizer runtime";
}
static char skipped[LIST_LINE_MAX];   // "skipped (must load at launch)" entries, logged as one line
static char displaced[256];           // tools whose launch-time insert is missing, said as one line
static void addto(char *buf, size_t n, const char *s) {
    if (strstr(buf, s)) return;
    size_t l = strlen(buf);
    snprintf(buf + l, n - l, "%s%s", l ? ", " : "", s);
}
// what tryload() did with a library
enum tryload_result {
    TRYLOAD_DLOPENED = 0,              // dlopen tried (logged with its result)
    TRYLOAD_SKIPPED_LAUNCH_ONLY = 1,   // a library that only works inserted at launch
    TRYLOAD_SKIPPED_SECOND_COPY = 2,   // its basename is already mapped from another path
};
// restores and chain entries; the engine itself is dlopened directly (its singleton handles a second copy)
static enum tryload_result tryload(const char *path, const char *what) {
    if (launch_only(path)) {
        // same family already mapped (the app links its runtime): nothing lost. Else that tool is off in this launch.
        // The family is the basename up to the first '_' or '.' after "libclang_rt." ("libclang_rt.asan").
        const char *b = base(path);
        char fam[64];
        snprintf(fam, sizeof fam, "%.*s", (int)(strcspn(b + CLANG_RT_PREFIX_LEN, "_.") + CLANG_RT_PREFIX_LEN), b);
        const char *have = hasprefix(b, CLANG_RT_PREFIX) ? mapped(fam, 0) : mapped(b, 1);
        char e[PATH_NOTE_MAX];
        snprintf(e, sizeof e, "%s [%s%s]", path, have ? "already loaded: " : "not in this process", have ? have : "");
        addto(skipped, sizeof skipped, e);
        if (!have) addto(displaced, sizeof displaced, toolof(path));
        return TRYLOAD_SKIPPED_LAUNCH_ONLY;
    }
    const char *have = mapped(base(path), 1);
    if (have && strcmp(have, path)) {
        char e[PATH_NOTE_MAX];
        snprintf(e, sizeof e, "%s [second copy; already loaded: %s]", path, have);
        addto(skipped, sizeof skipped, e);
        return TRYLOAD_SKIPPED_SECOND_COPY;
    }
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    slog(what, h ? " loaded" : dlerror());
    return TRYLOAD_DLOPENED;
}
static char chainskip[LIST_LINE_MAX];   // chain entries skipped as a second copy (said on stderr: the user's list)
static void chain(void) {
    const char *cf = getenv("SIMCAM_CHAIN");
    char dflt[LOADER_PATH_MAX];
    const char *sd = getenv("SIMCAM_SIM_DIR");
    if (!cf && sd && *sd) {   // never "/chain.txt"
        snprintf(dflt, sizeof dflt, "%s/chain.txt", sd);
        cf = dflt;
    }
    if (!cf) return;
    int fd = open(cf, O_RDONLY);
    if (fd < 0) {
        slog("chain file absent ", cf);
        return;
    }
    char buf[INSERT_LIST_MAX];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    if ((size_t)n >= sizeof buf - 1) {   // longer than we read: drop the cut-off last line rather than dlopen half a path
        char *nl = strrchr(buf, '\n');
        if (nl) *nl = 0;
        else buf[0] = 0;
        slog("chain file longer than 8 KB; entries past it ignored: ", cf);
    }
    char *sv = NULL;
    for (char *line = strtok_r(buf, "\n", &sv); line; line = strtok_r(NULL, "\n", &sv)) {
        if (!line[0] || line[0] == '#') continue;
        if (hasprefix(base(line), "libsimcam")) {
            slog("chain: skipping our own library ", line);
            continue;
        }
        if (tryload(line, "chain lib") == TRYLOAD_SKIPPED_SECOND_COPY) addto(chainskip, sizeof chainskip, line);
    }
}

// ---- a small JSON reader (no allocation): enough for config.json and restore-rules/<build>.json ----------------------
static const char *jws(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    return p;
}
static const char *jskip(const char *p) {   // past one value; NULL on malformed input
    p = jws(p);
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++)
            if (*p == '\\' && p[1]) p++;
        return *p ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = jws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (close == '}') {   // a key and its ':'
                p = jskip(p);
                if (!p) return NULL;
                p = jws(p);
                if (*p != ':') return NULL;
                p++;
            }
            p = jskip(p);
            if (!p) return NULL;
            p = jws(p);
            if (*p == ',') {
                p = jws(p + 1);
                continue;
            }
            return *p == close ? p + 1 : NULL;
        }
    }
    if (!*p) return NULL;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}
static int jhex4(const char *p) {   // 4 hex digits -> 0..0xFFFF, else -1
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}
// a string value into out (JSON escapes, \uXXXX as UTF-8); 0 if not a string. On failure out holds what was read so far.
static int jstr(const char *p, char *out, size_t n) {
    if (!n) return 0;
    out[0] = 0;
    p = jws(p);
    if (*p != '"') return 0;
    size_t k = 0;
    for (p++; *p && *p != '"'; p++) {
        char u[4];   // the next character's UTF-8 bytes
        int ul = 1;
        u[0] = *p;
        if (*p == '\\' && p[1]) {
            char c = *++p;
            if (c == 'n') u[0] = '\n';
            else if (c == 't') u[0] = '\t';
            else if (c == 'r') u[0] = '\r';
            else if (c == 'b') u[0] = '\b';
            else if (c == 'f') u[0] = '\f';
            else if (c == 'u') {   // Go's encoder writes \u0026 for '&' unless told not to
                int cp = jhex4(p + 1);
                if (cp < 0) {
                    out[k] = 0;
                    return 0;
                }
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {   // a surrogate pair
                    int lo = jhex4(p + 3);
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                if (cp >= 0xD800 && cp < 0xE000) cp = '?';   // a lone surrogate
                if (cp < 0x80) {
                    u[0] = (char)cp;
                } else if (cp < 0x800) {
                    u[0] = (char)(0xC0 | cp >> 6);
                    u[1] = (char)(0x80 | (cp & 0x3F));
                    ul = 2;
                } else if (cp < 0x10000) {
                    u[0] = (char)(0xE0 | cp >> 12);
                    u[1] = (char)(0x80 | (cp >> 6 & 0x3F));
                    u[2] = (char)(0x80 | (cp & 0x3F));
                    ul = 3;
                } else {
                    u[0] = (char)(0xF0 | cp >> 18);
                    u[1] = (char)(0x80 | (cp >> 12 & 0x3F));
                    u[2] = (char)(0x80 | (cp >> 6 & 0x3F));
                    u[3] = (char)(0x80 | (cp & 0x3F));
                    ul = 4;
                }
            } else {
                u[0] = c;   // \" \\ \/
            }
        }
        if (k + ul >= n) {   // doesn't fit: a prefix, reported as a failure
            out[k] = 0;
            return 0;
        }
        memcpy(out + k, u, ul);
        k += ul;
    }
    out[k] = 0;
    return *p == '"';
}
static const char *jget(const char *obj, const char *key) {   // the value of key in the object at obj, else NULL
    obj = jws(obj);
    if (*obj != '{') return NULL;
    const char *p = jws(obj + 1);
    while (*p == '"') {
        char k[64];
        int match = jstr(p, k, sizeof k) && !strcmp(k, key);
        p = jskip(p);
        if (!p) return NULL;
        p = jws(p);
        if (*p != ':') return NULL;
        p = jws(p + 1);
        if (match) return p;
        p = jskip(p);
        if (!p) return NULL;
        p = jws(p);
        if (*p != ',') return NULL;
        p = jws(p + 1);
    }
    return NULL;
}
static const char *jfirst(const char *arr) {   // the first element of the array at arr, else NULL
    arr = jws(arr);
    if (*arr != '[') return NULL;
    arr = jws(arr + 1);
    return *arr == ']' ? NULL : arr;
}
static const char *jnext(const char *el) {   // the element after el, else NULL
    const char *p = jskip(el);
    if (!p) return NULL;
    p = jws(p);
    return *p == ',' ? jws(p + 1) : NULL;
}
// the whole file into buf (NUL-terminated); -1 if unreadable or if it fills the buffer (it may be truncated)
static ssize_t slurp(const char *path, char *buf, size_t n) {
    buf[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t k = read(fd, buf, n - 1);
    close(fd);
    if (k < 0 || (size_t)k >= n - 1) {
        buf[0] = 0;
        return -1;
    }
    buf[k] = 0;
    return k;
}
// a usable rules file: the whole document parses and has a "rules" value that parses (the CLI's xcode.Rules checks the
// same)
static int rulesok(const char *buf) {
    const char *r;
    return jskip(buf) && (r = jget(buf, "rules")) && jskip(r);
}

// ---- which Xcode's rules ---------------------------------------------------------------------------------------------
// Apple build numbers: <major><train letter><number><suffix> (27A266a < 27A9275 < 27B52); returns <0, 0, >0
static int buildcmp(const char *a, const char *b) {
    long ma = strtol(a, (char **)&a, 10), mb = strtol(b, (char **)&b, 10);
    if (ma != mb) return ma < mb ? -1 : 1;
    if (*a != *b) return *a < *b ? -1 : 1;
    if (*a) {
        a++;
        b++;
    }
    long na = strtol(a, (char **)&a, 10), nb = strtol(b, (char **)&b, 10);
    if (na != nb) return na < nb ? -1 : 1;
    return strcmp(a, b);
}
static char rulesbuf[RULES_FILE_MAX];   // the rules file in use ("" = built-in)
static char rulesfrom[PATH_NOTE_MAX];
static char xbuild[64];                 // the recorded Xcode build, "" if none
static int rulesverified;
static char cfgbuf[CONFIG_FILE_MAX];    // $SIMCAM_SIM_DIR/config.json ("" if absent, unreadable or of an unknown proto)
static int cfgbad;                      // config.json exists but can't be used (unreadable, truncated, unknown proto)
static char simdir[LOADER_PATH_MAX];    // $SIMCAM_SIM_DIR without trailing slashes
static int cfgdone;
static void readcfg(void) {             // once per process, on first use
    if (cfgdone) return;
    cfgdone = 1;
    const char *sd = getenv("SIMCAM_SIM_DIR");
    if (!sd || !*sd) return;
    snprintf(simdir, sizeof simdir, "%s", sd);
    for (size_t l = strlen(simdir); l > 1 && simdir[l - 1] == '/'; l--) simdir[l - 1] = 0;
    char cf[PATH_NOTE_MAX];
    snprintf(cf, sizeof cf, "%s/config.json", simdir);
    if (slurp(cf, cfgbuf, sizeof cfgbuf) <= 0 || !jskip(cfgbuf)) {
        cfgbuf[0] = 0;
        cfgbad = access(cf, F_OK) == 0;
        slog("no readable config.json (Xcode build not recorded): ", cf);
        return;
    }
    const char *v = jget(cfgbuf, "proto");
    int proto = v ? atoi(v) : 0;
    if (proto != SUPPORTED_CONFIG_PROTO) {
        cfgbuf[0] = 0;
        cfgbad = 1;
        slog("config.json: unknown proto; Xcode build ignored: ", cf);
    }
}
// a NAME=value that config.json's "boot_signals" lists: it was in the boot environment, so it says nothing about this
// launch
static int bootsig(const char *kv) {
    readcfg();
    const char *a = cfgbuf[0] ? jget(cfgbuf, "boot_signals") : NULL;
    for (const char *e = a ? jfirst(a) : NULL; e; e = jnext(e)) {
        char s[BOOT_SIGNAL_MAX];
        if (jstr(e, s, sizeof s) && !strcmp(s, kv)) return 1;
    }
    return 0;
}
static void pickrules(void) {
    char dir[LOADER_PATH_MAX] = "";
    const char *v;
    int hasdir = 0;
    readcfg();
    if (cfgbuf[0]) {
        // jstr's result is deliberately unused: a value that is not a string leaves "" (no Xcode build recorded; for
        // rules_dir, the built-in rules), and one too long for the buffer (the CLI writes short ones) or with a bad
        // \u escape leaves the prefix read so far, used as read
        if ((v = jget(cfgbuf, "xcode_build"))) (void)jstr(v, xbuild, sizeof xbuild);
        if ((v = jget(cfgbuf, "rules_dir"))) {   // "" = no rules staged: built-in
            (void)jstr(v, dir, sizeof dir);
            hasdir = 1;
        }
    }
    if (!hasdir && cfgbad) hasdir = 1;   // a record we can't use: the built-in rules, never the stale flat dir
    // no record of a dir (older records): $SIMCAM_HOME/sims/<udid> -> $SIMCAM_HOME/restore-rules
    if (!hasdir && simdir[0]) {
        snprintf(dir, sizeof dir, "%s", simdir);
        char *s = strrchr(dir, '/');
        if (s) *s = 0;
        s = strrchr(dir, '/');
        if (s) *s = 0;
        strncat(dir, "/restore-rules", sizeof dir - strlen(dir) - 1);
    }
    rulesbuf[0] = 0;
    rulesfrom[0] = 0;
    if (xbuild[0] && dir[0]) {
        snprintf(rulesfrom, sizeof rulesfrom, "%s/%s.json", dir, xbuild);
        if (slurp(rulesfrom, rulesbuf, sizeof rulesbuf) > 0 && rulesok(rulesbuf)) {
            rulesverified = 1;
            slog("rules: ", rulesfrom);
            return;
        }
        rulesbuf[0] = 0;
    }
    char best[128] = "";   // best guess: the newest build that has rules
    DIR *d = dir[0] ? opendir(dir) : NULL;
    for (struct dirent *e; d && (e = readdir(d));) {
        size_t l = strlen(e->d_name);
        if (l < 6 || l > 100 || strcmp(e->d_name + l - 5, ".json") || e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char b[128];
        snprintf(b, sizeof b, "%.*s", (int)(l - 5), e->d_name);
        if (!best[0] || buildcmp(b, best) > 0) snprintf(best, sizeof best, "%s", b);
    }
    if (d) closedir(d);
    if (best[0]) {
        snprintf(rulesfrom, sizeof rulesfrom, "%s/%s.json", dir, best);
        if (slurp(rulesfrom, rulesbuf, sizeof rulesbuf) > 0 && rulesok(rulesbuf)) {
            slog("rules (best guess, newest): ", rulesfrom);
            return;
        }
        rulesbuf[0] = 0;
    }
    snprintf(rulesfrom, sizeof rulesfrom, "built-in (27A266a)");
    slog("rules: built-in 27A266a; none readable in ", dir);
}
// the built-in rules = restore-rules/27A266a.json's "xcode-run" and "ui-test-target" libs (used when no rules file can
// be read). Keep them in step with that file; runnerhook.m's builtinRunnerLibs() is the same file's "test-runner" rule.
struct lib { const char *path, *cond, *unless; };
static const struct lib builtin_xcode[] = {
    {"/usr/lib/libLogRedirect.dylib", "IDE_CHOSEN_LOGGING_METHOD=2", NULL},
    {"/usr/lib/libMainThreadChecker.dylib", NULL, "SIMCAM_MTC=0"},
    {"/usr/lib/libRPAC.dylib", "PERFC_*", NULL},
    {"/usr/lib/libViewDebuggerSupport.dylib", NULL, NULL},
    {NULL, NULL, NULL}};
static const struct lib builtin_ui[] = {
    {"/usr/lib/libLogRedirect.dylib", "IDE_CHOSEN_LOGGING_METHOD=2", NULL},
    {"/usr/lib/libMainThreadChecker.dylib", NULL, "SIMCAM_MTC=0"},
    {"/usr/lib/libRPAC.dylib", "PERFC_*", NULL},
    {NULL, NULL, NULL}};
// a condition: VAR=value (equal), PREFIX* (any variable with that prefix), VAR (set): the restore-rules "if"/"unless"
// syntax. runnerhook.m's cond() matches the same syntax against an NSDictionary; the two must stay in step (they are
// separate builds: this file is libSystem-only C, that one Foundation)
static int cond(const char *c) {
    if (!c || !*c) return 1;
    size_t l = strlen(c);
    if (c[l - 1] == '*') {
        for (char **e = environ; *e; e++)
            if (!strncmp(*e, c, l - 1)) return 1;
        return 0;
    }
    const char *eq = strchr(c, '=');
    if (!eq) return getenv(c) != NULL;
    char k[128];
    snprintf(k, sizeof k, "%.*s", (int)(eq - c), c);
    const char *v = getenv(k);
    return v && !strcmp(v, eq + 1);
}
static void loadlib(const char *root, const char *rel) {
    char p[LOADER_PATH_MAX];
    snprintf(p, sizeof p, "%s%s", rel[0] == '/' ? root : "", rel);
    tryload(p, base(rel));
}
static void restore(const char *rule, const struct lib *dflt, const char *root) {
    const char *rs = rulesbuf[0] ? jget(rulesbuf, "rules") : NULL, *libs = NULL;
    for (const char *r = rs ? jfirst(rs) : NULL; r; r = jnext(r)) {
        char nm[64];
        const char *v = jget(r, "name");
        if (v && jstr(v, nm, sizeof nm) && !strcmp(nm, rule)) {
            libs = jget(r, "libs");
            break;
        }
    }
    if (!libs) {
        if (rulesbuf[0]) slog("rules file has no libs for this rule; built-in used: ", rule);
        for (const struct lib *l = dflt; l->path; l++)
            if (cond(l->cond) && !(l->unless && cond(l->unless))) loadlib(root, l->path);
        return;
    }
    for (const char *l = jfirst(libs); l; l = jnext(l)) {
        char path[512] = "", c[128] = "", u[128] = "";
        const char *v;
        if (!(v = jget(l, "path")) || !jstr(v, path, sizeof path) || !path[0]) continue;
        if ((v = jget(l, "if")) && !jstr(v, c, sizeof c)) {
            slog("rules: unreadable condition; lib skipped: ", path);
            continue;
        }
        if ((v = jget(l, "unless")) && !jstr(v, u, sizeof u)) {
            slog("rules: unreadable condition; lib skipped: ", path);
            continue;
        }
        if (cond(c) && !(u[0] && cond(u))) loadlib(root, path);
    }
}

// ---- displaced per-launch inserts (design §4.1 rule 4) ---------------------------------------------------------------
static void detect_displaced(void) {
    // a launch whose own list still holds something besides our stub merged it with ours (stuntcam env): nothing displaced
    const char *il = getenv("DYLD_INSERT_LIBRARIES");
    if (il) {
        char l[INSERT_ENV_COPY_MAX];
        snprintf(l, sizeof l, "%s", il);
        char *sv = NULL;
        for (char *e = strtok_r(l, ":", &sv); e; e = strtok_r(NULL, ":", &sv))
            if (*e && !hasprefix(base(e), "libsimcam")) return;
    }
    int *argc = _NSGetArgc();
    char ***argv = _NSGetArgv();
    for (int i = 1; argc && argv && *argv && i < *argc; i++)
        if (!strcmp((*argv)[i], "-detoxServer") || !strcmp((*argv)[i], "-detoxSessionId")) {
            if (!mapped("Detox", 1)) addto(displaced, sizeof displaced, "Detox");
            break;
        }
    int idb = 0, gm = 0, asan = 0, tsan = 0;
    for (char **e = environ; *e; e++) {   // (bootsig reads config.json only when a variable looks like a signal)
        if (hasprefix(*e, "IDB_") && !bootsig(*e)) idb = 1;
        if ((hasprefix(*e, "MALLOC_PROTECT_BEFORE=") || hasprefix(*e, "MALLOC_FILL_SPACE=") ||
             hasprefix(*e, "MALLOC_STRICT_SIZE=") || hasprefix(*e, "MALLOC_ALLOW_READS=") ||
             hasprefix(*e, "MALLOC_VECTOR_SIZE=") || hasprefix(*e, "MALLOC_PERMIT_INSANE_REQUESTS=")) &&
            !bootsig(*e))
            gm = 1;
        if (hasprefix(*e, "ASAN_OPTIONS=") && !bootsig(*e)) asan = 1;
        if (hasprefix(*e, "TSAN_OPTIONS=") && !bootsig(*e)) tsan = 1;
    }
    if (idb) addto(displaced, sizeof displaced, "idb");
    if (gm && !mapped("libgmalloc.dylib", 1)) addto(displaced, sizeof displaced, "Guard Malloc");
    if (asan && !mapped("libclang_rt.asan", 0)) addto(displaced, sizeof displaced, "AddressSanitizer");
    if (tsan && !mapped("libclang_rt.tsan", 0)) addto(displaced, sizeof displaced, "ThreadSanitizer");
}

// ---- the constructor's steps -----------------------------------------------------------------------------------------
// an app process this loader acts in (the same test as the stub): an executable in a .app bundle, in the device's data
// dir or an Xcode build-products dir, not under the runtime root
static int is_app_process(const char *exe, const char *root) {
    if (!strstr(exe, ".app/") || (!strstr(exe, "/data/") && !strstr(exe, "/Build/Products/")) ||
        (root && *root && !strncmp(exe, root, strlen(root))))
        return 0;
    return 1;
}
static void open_log(void) {
    const char *lg = getenv("SIMCAM_STUB_LOG");
    if (lg) gfd = open(lg, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
}
// what the environment says about who launched this process
struct launch_markers {
    int xcode;    // Xcode launched it (MTC_/PERFC_/IDE_ variables, DYLD_LIBRARY_PATH in DerivedData)
    int xctest;   // an XCTest process (XCInjectBundleInto, XCTestConfigurationFilePath, ...)
    int perfc;    // a PERFC_ variable is set
};
// one pass over the environment; with SIMCAM_STUB_ENVDUMP set, each variable is also logged
static struct launch_markers scan_launch_env(void) {
    struct launch_markers m = {0, 0, 0};
    for (char **e = environ; *e; e++) {
        if (hasprefix(*e, "MTC_") || hasprefix(*e, "PERFC_") || hasprefix(*e, "IDE_")) m.xcode = 1;
        if (hasprefix(*e, "PERFC_")) m.perfc = 1;
        if (hasprefix(*e, "DYLD_LIBRARY_PATH=") && strstr(*e, "/DerivedData/")) m.xcode = 1;
        if (hasprefix(*e, "XCInjectBundleInto=") || hasprefix(*e, "XCTestConfigurationFilePath=") ||
            hasprefix(*e, "XCTestBundlePath=") || hasprefix(*e, "XCTestSessionIdentifier="))
            m.xctest = 1;
        if (getenv("SIMCAM_STUB_ENVDUMP")) slog("env ", *e);
    }
    return m;
}
static void log_restore_inputs(struct launch_markers m) {
    char d[160];
    snprintf(d, sizeof d, "xctest=%d inject=%d xcode=%d perfc=%d uitarget=%d logmethod=%s", m.xctest,
             getenv("XCTestBundleInjectPath") != NULL, m.xcode, m.perfc, getenv("XCODE_SCHEME_NAME") != NULL,
             getenv("IDE_CHOSEN_LOGGING_METHOD") ? getenv("IDE_CHOSEN_LOGGING_METHOD") : "-");
    slog("restore inputs: ", d);
}
// the path an inject-list entry loads from, into p: runtime-relative entries (/usr/lib/libRPAC.dylib) get the runtime
// root when that file exists; everything else (DerivedData anywhere, incl. /Volumes, /private/var, /tmp; paths already
// under the root) loads as given
static void inject_entry_path(const char *e, const char *root, char *p, size_t n) {
    snprintf(p, n, "%s", e);
    if (root && (!strncmp(e, "/usr/", 5) || !strncmp(e, "/System/", 8)) && strncmp(e, root, strlen(root))) {
        char q[LOADER_PATH_MAX];
        snprintf(q, sizeof q, "%s%s", root, e);
        if (access(q, F_OK) == 0) snprintf(p, n, "%s", q);
    }
}
// hosted tests: XCTest copied the displaced DYLD_INSERT_LIBRARIES list to XCTestBundleInjectPath; each entry is loaded
// once, in order, except our own libraries. The list is XCTest's own, and what never loads late is fixed in launch_only():
// the rules play no part here and are picked only for the "unverified" line.
static void restore_inject_list(const char *inject_path, const char *root) {
    pickrules();
    char p[LOADER_PATH_MAX];
    char list[INSERT_LIST_MAX];
    strncpy(list, inject_path, sizeof list - 1);
    list[sizeof list - 1] = 0;
    char seen[INJECT_LIST_ENTRIES_MAX][LOADER_PATH_MAX];
    int ns = 0;
    char *sv = NULL;   // strtok_r: tryload runs the library's initializers (test-bundle code), which may use strtok
    for (char *e = strtok_r(list, ":", &sv); e; e = strtok_r(NULL, ":", &sv)) {
        int dup = 0;
        for (int i = 0; i < ns; i++)
            if (!strcmp(seen[i], e)) dup = 1;
        if (dup) continue;
        if (ns >= INJECT_LIST_ENTRIES_MAX || strlen(e) >= LOADER_PATH_MAX - 1) {
            slog("inject list: entry skipped (more than 32, or path too long): ", e);
            continue;
        }
        strncpy(seen[ns], e, LOADER_PATH_MAX - 1);
        seen[ns++][LOADER_PATH_MAX - 1] = 0;
        if (strstr(e, "libsimcamstub") || strstr(e, "libsimcamloader")) continue;
        inject_entry_path(e, root, p, sizeof p);
        tryload(p, p);   // log the path actually loaded (it may carry the runtime root)
    }
}
// an Xcode launch without a hosted-test inject list: the rules file's "ui-test-target" libs for an XCUIApplication target (MTC +
// RPAC, + LogRedirect under the Xcode GUI, no view debugger), else its "xcode-run" libs
static void restore_xcode_libs(const char *root) {
    int uitarget = getenv("XCODE_SCHEME_NAME") != NULL;
    pickrules();
    slog(uitarget ? "ui-test target: restoring MTC/RPAC, root=" : "xcode run: restoring Xcode's libraries, root=", root);
    restore(uitarget ? "ui-test-target" : "xcode-run", uitarget ? builtin_ui : builtin_xcode, root);
}
// the engine ($SIMCAM_DYLIB, else libsimcam.dylib next to this loader), except in UI-test runners and app extensions
static void load_engine(const char *exe, const Dl_info *self) {
    if (strstr(exe, "-Runner.app/")) {
        slog("xctest runner: not loading our dylib", 0);
        return;
    }
    if (strstr(exe, ".appex/")) {
        slog("app extension: not loading our dylib", 0);
        return;
    }
    const char *real = getenv("SIMCAM_DYLIB");
    char sib[LOADER_PATH_MAX];
    if (!real && self->dli_fname) {   // the engine next to this loader
        const char *sl = strrchr(self->dli_fname, '/');
        if (sl) {
            snprintf(sib, sizeof sib, "%.*s/libsimcam.dylib", (int)(sl - self->dli_fname), self->dli_fname);
            real = sib;
        }
    }
    if (real) {
        void *h = dlopen(real, RTLD_NOW | RTLD_GLOBAL);
        slog("real dylib", h ? " loaded" : dlerror());
    } else {
        slog("no engine path", 0);
    }
}
// the one-line announcements, after everything that can add to them (restores and the chain); ruled: rules were picked
static void announce(int ruled) {
    if (skipped[0]) slog("skipped (must load at launch, or a second copy): ", skipped);
    if (chainskip[0]) {
        char l[PATH_NOTE_MAX];
        snprintf(l, sizeof l, "stuntcam chain: not loaded, a copy is already loaded: %s", chainskip);
        say(l);
    }
    if (ruled && !rulesverified) {
        char l[256];
        snprintf(l, sizeof l, "restore rules unverified for Xcode %s; if console/debugger misbehave use --mode launchd",
                 xbuild[0] ? xbuild : "(not recorded)");
        say(l);
        slog("  rules used: ", rulesfrom);
    }
    detect_displaced();
    if (displaced[0]) {
        char l[400];
        snprintf(l, sizeof l, "stuntcam boot mode displaced %s; use 'stuntcam up --mode launchd'", displaced);
        say(l);
    }
}

__attribute__((constructor)) static void simcam_loader(void) {
    // is this process ours?
    static char exe[LOADER_PATH_MAX];
    uint32_t sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &sz) != 0) return;   // (_dyld_get_image_name(0) is the stub itself at this point)
    const char *on = getenv("SIMCAM");
    // the stub checked these too; the loader may also be dlopened directly (SIMCAM_LOADER)
    if (on && on[0] == '0') return;
    // the simulator runtime root: read once (both former reads came before any library was loaded)
    const char *root = getenv("SIMULATOR_ROOT");
    if (!root) root = getenv("DYLD_ROOT_PATH");
    if (!is_app_process(exe, root)) return;
    open_log();
    Dl_info self = {0};
    dladdr((const void *)simcam_loader, &self);
    slog("loader ", self.dli_fname);
    // 1. restore what Xcode/XCTest would have inserted (the numbers are the file header's)
    struct launch_markers m = scan_launch_env();
    const char *xcode_libs = getenv("SIMCAM_XCODE_LIBS");
    int restores_off = xcode_libs && xcode_libs[0] == '0';
    log_restore_inputs(m);
    int ruled = 0;   // rules were picked (for the "unverified" line)
    const char *inject_path = getenv("XCTestBundleInjectPath");
    if (m.xctest && inject_path && !restores_off) {
        restore_inject_list(inject_path, root);
        ruled = 1;
    } else if (m.xcode && !restores_off && root) {
        restore_xcode_libs(root);
        ruled = 1;
    }
    // 3. the engine, 4. the chain file, then 2. the announcements, which the restores and the chain can add to
    load_engine(exe, &self);
    chain();
    announce(ruled);
    if (gfd >= 0) {
        close(gfd);
        gfd = -1;
    }
}
