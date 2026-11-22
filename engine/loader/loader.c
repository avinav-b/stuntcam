// SPDX-License-Identifier: Apache-2.0
// simcam loader (libsimcamloader.dylib).
//
// Who loads it: the frozen boot stub (engine/stub) dlopens it from the stub's constructor, at the path named in
// $SIMCAM_SIM_DIR/current (or in SIMCAM_LOADER), so the loader can change without a simulator reboot while the stub
// never changes. It runs before the engine and before any of the app's code, so it uses libSystem only and allocates
// nothing: every buffer is fixed-size, static or on the stack.
//
// What it provides: one constructor, simcam_loader(), and no functions for other code. In an app process (an
// executable in a .app bundle, in the device's data dir or an Xcode build-products dir, and not under the runtime
// root) it does four things:
//   1. It restores the libraries that Xcode or XCTest would have inserted, which the boot environment's
//      DYLD_INSERT_LIBRARIES displaced. Which list it uses:
//      - XCTestBundleInjectPath is set (hosted tests): exactly that list.
//      - Else XCODE_SCHEME_NAME is set (a UI-test target): the rules file's "ui-test-target" libs.
//      - Else Xcode launched the app (MTC_, PERFC_ or IDE_ variables, or DYLD_LIBRARY_PATH in DerivedData): the
//        rules file's "xcode-run" libs.
//      The rules file is <rules_dir>/<xcode_build>.json. `stuntcam up` (or scripts/boot.sh) records the Xcode build
//      and the rules_dir it staged in $SIMCAM_SIM_DIR/config.json. The rules_dir is a per-content directory
//      ($SIMCAM_HOME/restore-rules/<hash>/), so an armed simulator's rules never change under it. The record made at
//      arm time is the only source, because under ⌘R nothing in the app names the Xcode that launched it. With no
//      record, or no file for that build, the loader uses the newest rules file (else the built-in 27A266a rules)
//      and says ONE line: "restore rules unverified for Xcode <build>; …".
//      Some libraries are never dlopened, by any rule or by the chain. Libraries that only work when inserted at
//      launch (the sanitizer runtimes libclang_rt.* and libgmalloc): a sanitized app links its own runtime, and
//      dlopening the test bundle's copy aborts ASan and maps a second TSan. And a second copy of a library whose
//      basename is already mapped from another path. Both kinds are logged in one line, "skipped (must load at
//      launch, or a second copy)". A chain entry skipped as a second copy is also said on stderr, because the chain
//      is the user's own list.
//      SIMCAM_XCODE_LIBS=0 skips all restores; SIMCAM_MTC=0 skips the Main Thread Checker.
//   2. It says, in ONE line on stderr and in the log, when boot mode displaced another tool's per-launch insert, so the
//      user can switch to launchd mode. The signals are Detox launch arguments, IDB_* variables, sanitizer or Guard
//      Malloc settings whose runtime is absent, and a launch-only library from the restore lists that isn't mapped.
//      They count only while the insert list is our stub alone: a launch that merged its own list with ours displaced
//      nothing. A variable whose NAME=value config.json lists in "boot_signals" is not a signal: it was in the boot
//      environment, so every process has it.
//   3. It dlopens the engine: $SIMCAM_DYLIB, else libsimcam.dylib next to this loader (a build dir holds a matching
//      pair). It skips this step in *-Runner.app (UI-test runners) and in app extensions (.appex).
//   4. It dlopens each line of the chain file ($SIMCAM_CHAIN, else $SIMCAM_SIM_DIR/chain.txt), so tools that arrive
//      after boot can still add libraries without a reboot. Lines starting with '#' and our own libraries
//      (libsimcam*) are skipped.
// SIMCAM=0 turns it off. SIMCAM_STUB_LOG=<file> turns on the log; SIMCAM_STUB_ENVDUMP=1 also logs the whole
// environment.
//
// State: file-scope globals, all written on the constructor's thread before the app's code runs. The log descriptor
// (gLogFd); config.json and the rules file in use, each read at most once (gConfigJson, gRulesJson, gXcodeBuild,
// gRulesSource, gRulesVerified, gConfigUnusable, gSimDir, gConfigRead); and the one-line lists that announce() prints
// at the end (gSkipped, gChainSkipped, gDisplaced).
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
    LOADER_PATH_MAX = 1024,          // one path (an executable, a library, a directory)
    PATH_NOTE_MAX = 1100,            // a path plus a short note ("<dir>/config.json", "<path> [second copy; …]")
    LOG_LINE_MAX = 4096,             // one stub.log line
    SAID_LINE_MAX = 1024,            // one line said on stderr
    LIST_LINE_MAX = 1024,            // the one-line lists of skipped libraries
    DISPLACED_LIST_MAX = 256,        // the list of displaced tools
    INSERT_LIST_MAX = 8192,          // a ':'-separated insert list (XCTestBundleInjectPath) or the chain file
    INSERT_ENV_COPY_MAX = 4096,      // the copy of DYLD_INSERT_LIBRARIES that detect_displaced() splits
    BOOT_SIGNAL_MAX = 4096,          // one config.json "boot_signals" entry
    CONFIG_FILE_MAX = 16384,         // $SIMCAM_SIM_DIR/config.json
    RULES_FILE_MAX = 32768,          // a restore-rules/<build>.json file
    INJECT_LIST_ENTRIES_MAX = 32,    // distinct XCTestBundleInjectPath entries restored
    LIB_FAMILY_MAX = 64,             // a sanitizer runtime's family name ("libclang_rt.asan")
    JSON_KEY_MAX = 64,               // one object key in a JSON file
    RULE_NAME_MAX = 64,              // a rule's "name" in the rules file
    RULE_LIB_PATH_MAX = 512,         // a lib's "path" in the rules file
    CONDITION_MAX = 128,             // a lib's "if" or "unless" condition, or the variable name in one
    XCODE_BUILD_MAX = 64,            // the recorded Xcode build ("27A266a")
    BUILD_NAME_MAX = 128,            // a build read from a rules file name (see RULES_FILE_NAME_MAX)
    RESTORE_INPUTS_LINE_MAX = 160,   // the "restore inputs:" log detail
    UNVERIFIED_LINE_MAX = 256,       // the "restore rules unverified" line
    DISPLACED_LINE_MAX = 400,        // the "boot mode displaced" line
};

#define CLANG_RT_PREFIX "libclang_rt."
enum { CLANG_RT_PREFIX_LEN = sizeof CLANG_RT_PREFIX - 1 };   // 12
#define JSON_SUFFIX ".json"
enum {
    JSON_SUFFIX_LEN = sizeof JSON_SUFFIX - 1,    // 5
    RULES_FILE_NAME_MIN = JSON_SUFFIX_LEN + 1,   // a one-character build and ".json"
    RULES_FILE_NAME_MAX = 100,                   // longer names are not Xcode builds (and fit BUILD_NAME_MAX)
};

// the runtime-relative directories of an inject-list entry (see inject_entry_path())
#define USR_PREFIX "/usr/"
#define SYSTEM_PREFIX "/System/"
enum {
    USR_PREFIX_LEN = sizeof USR_PREFIX - 1,         // 5
    SYSTEM_PREFIX_LEN = sizeof SYSTEM_PREFIX - 1,   // 8
};

enum { SUPPORTED_CONFIG_PROTO = 1 };   // the config.json "proto" this loader reads

static int gLogFd = -1;

static void log_line(const char *message, const char *detail) {
    if (gLogFd < 0) {
        return;
    }
    char buf[LOG_LINE_MAX];
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    // seconds and microseconds
    int length = snprintf(buf, sizeof buf, "%ld.%06ld [loader %d] %s%s\n", (long)now.tv_sec, now.tv_nsec / 1000,
                          getpid(), message, detail ? detail : "");
    // a log line that can't be written is dropped: the loader never fails an app launch over its log
    (void)write(gLogFd, buf, length > (int)sizeof buf ? (int)sizeof buf : length);
}

// A user-facing line: on stderr (Xcode's console, `simctl launch --console-pty`) and in the log.
static void say(const char *line) {
    char buf[SAID_LINE_MAX];
    int length = snprintf(buf, sizeof buf, "%s\n", line);
    (void)write(STDERR_FILENO, buf, length > (int)sizeof buf ? (int)sizeof buf : length);   // the log has it too
    log_line("said: ", line);
}

static int has_prefix(const char *str, const char *prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// ---- images already mapped ------------------------------------------------------------------------------------------
// The path of a mapped image whose basename starts with prefix (or equals it, when exact is set), else NULL.
static const char *mapped_image(const char *prefix, int exact) {
    for (uint32_t i = 0, count = _dyld_image_count(); i < count; i++) {
        const char *name = _dyld_get_image_name(i);
        if (!name) {
            continue;
        }
        const char *leaf = base_name(name);
        if (exact ? !strcmp(leaf, prefix) : has_prefix(leaf, prefix)) {
            return name;
        }
    }
    return NULL;
}

// ---- libraries that only work inserted at launch --------------------------------------------------------------------
// A launch-only library (a sanitizer runtime or Guard Malloc) is never dlopened. runnerhook.m's launchOnly() makes the
// same test, and restore-rules/*.json documents it as "never_dlopened".
static int launch_only(const char *path) {
    const char *leaf = base_name(path);
    return has_prefix(leaf, CLANG_RT_PREFIX) || !strcmp(leaf, "libgmalloc.dylib");
}

// The tool a launch-only library belongs to, for the "displaced" line.
static const char *tool_of(const char *path) {
    const char *leaf = base_name(path);
    if (has_prefix(leaf, "libclang_rt.asan")) {
        return "AddressSanitizer";
    }
    if (has_prefix(leaf, "libclang_rt.tsan")) {
        return "ThreadSanitizer";
    }
    if (has_prefix(leaf, "libclang_rt.ubsan")) {
        return "UndefinedBehaviorSanitizer";
    }
    if (!strcmp(leaf, "libgmalloc.dylib")) {
        return "Guard Malloc";
    }
    return "a sanitizer runtime";
}

static char gSkipped[LIST_LINE_MAX];         // "skipped (must load at launch)" entries, logged as one line
static char gDisplaced[DISPLACED_LIST_MAX];  // tools whose launch-time insert is missing, said as one line

// Appends entry to a ", "-separated list, unless the list already holds it.
static void list_add(char *list, size_t size, const char *entry) {
    if (strstr(list, entry)) {
        return;
    }
    size_t used = strlen(list);
    snprintf(list + used, size - used, "%s%s", used ? ", " : "", entry);
}

// What try_load() did with a library.
enum tryload_result {
    TRYLOAD_DLOPENED = 0,              // dlopen tried (logged with its result)
    TRYLOAD_SKIPPED_LAUNCH_ONLY = 1,   // a library that only works inserted at launch
    TRYLOAD_SKIPPED_SECOND_COPY = 2,   // its basename is already mapped from another path
};

// Loads a restored library or a chain entry. The engine itself is dlopened directly: its singleton handles a second
// copy.
static enum tryload_result try_load(const char *path, const char *what) {
    if (launch_only(path)) {
        // If the same family is already mapped (the app links its runtime), nothing is lost. Otherwise that tool is
        // off in this launch. The family is the basename up to the first '_' or '.' after "libclang_rt."
        // ("libclang_rt.asan").
        const char *leaf = base_name(path);
        char family[LIB_FAMILY_MAX];
        size_t family_len = strcspn(leaf + CLANG_RT_PREFIX_LEN, "_.") + CLANG_RT_PREFIX_LEN;
        snprintf(family, sizeof family, "%.*s", (int)family_len, leaf);
        const char *loaded = has_prefix(leaf, CLANG_RT_PREFIX) ? mapped_image(family, 0) : mapped_image(leaf, 1);
        char entry[PATH_NOTE_MAX];
        snprintf(entry, sizeof entry, "%s [%s%s]", path, loaded ? "already loaded: " : "not in this process",
                 loaded ? loaded : "");
        list_add(gSkipped, sizeof gSkipped, entry);
        if (!loaded) {
            list_add(gDisplaced, sizeof gDisplaced, tool_of(path));
        }
        return TRYLOAD_SKIPPED_LAUNCH_ONLY;
    }

    const char *loaded = mapped_image(base_name(path), 1);

    if (loaded && strcmp(loaded, path)) {
        char entry[PATH_NOTE_MAX];
        snprintf(entry, sizeof entry, "%s [second copy; already loaded: %s]", path, loaded);
        list_add(gSkipped, sizeof gSkipped, entry);
        return TRYLOAD_SKIPPED_SECOND_COPY;
    }

    void *handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    log_line(what, handle ? " loaded" : dlerror());
    return TRYLOAD_DLOPENED;
}

// Chain entries skipped as a second copy. They are said on stderr, because the chain is the user's own list.
static char gChainSkipped[LIST_LINE_MAX];

static void load_chain(void) {
    const char *chain_path = getenv("SIMCAM_CHAIN");
    char default_path[LOADER_PATH_MAX];
    const char *sim_dir_env = getenv("SIMCAM_SIM_DIR");

    if (!chain_path && sim_dir_env && *sim_dir_env) {   // never "/chain.txt"
        snprintf(default_path, sizeof default_path, "%s/chain.txt", sim_dir_env);
        chain_path = default_path;
    }

    if (!chain_path) {
        return;
    }
    int fd = open(chain_path, O_RDONLY);

    if (fd < 0) {
        log_line("chain file absent ", chain_path);
        return;
    }

    char contents[INSERT_LIST_MAX];
    ssize_t length = read(fd, contents, sizeof contents - 1);
    close(fd);
    if (length <= 0) {
        return;
    }
    contents[length] = 0;

    // The file is longer than the buffer: drop the cut-off last line rather than dlopen half a path.
    if ((size_t)length >= sizeof contents - 1) {
        char *last_newline = strrchr(contents, '\n');
        if (last_newline) {
            *last_newline = 0;
        } else {
            contents[0] = 0;
        }
        log_line("chain file longer than 8 KB; entries past it ignored: ", chain_path);
    }

    char *strtok_state = NULL;

    for (char *line = strtok_r(contents, "\n", &strtok_state); line; line = strtok_r(NULL, "\n", &strtok_state)) {
        if (!line[0] || line[0] == '#') {
            continue;
        }
        if (has_prefix(base_name(line), "libsimcam")) {
            log_line("chain: skipping our own library ", line);
            continue;
        }
        if (try_load(line, "chain lib") == TRYLOAD_SKIPPED_SECOND_COPY) {
            list_add(gChainSkipped, sizeof gChainSkipped, line);
        }
    }
}

// ---- a small JSON reader (no allocation): enough for config.json and restore-rules/<build>.json ---------------------
// Each function takes a cursor into a NUL-terminated document and returns where it stopped.
enum {
    HEX4_DIGITS = 4,                   // the XXXX of a \uXXXX escape
    UNICODE_ESCAPE_LEN = 6,            // "\uXXXX"
    SURROGATE_HIGH_FIRST = 0xD800,     // UTF-16 surrogates: high (first) 0xD800-0xDBFF, low (second) 0xDC00-0xDFFF
    SURROGATE_LOW_FIRST = 0xDC00,
    SURROGATE_END = 0xE000,            // one past the last low surrogate
    SUPPLEMENTARY_FIRST = 0x10000,     // the first code point that needs a surrogate pair
};

static int is_json_space(char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

static const char *json_skip_space(const char *cursor) {
    while (is_json_space(*cursor)) {
        cursor++;
    }
    return cursor;
}

// Past one value; NULL on malformed input.
static const char *json_skip_value(const char *cursor) {
    cursor = json_skip_space(cursor);

    if (*cursor == '"') {
        for (cursor++; *cursor && *cursor != '"'; cursor++) {
            if (*cursor == '\\' && cursor[1]) {
                cursor++;
            }
        }
        return *cursor ? cursor + 1 : NULL;
    }

    if (*cursor == '{' || *cursor == '[') {
        char closer = *cursor == '{' ? '}' : ']';
        cursor = json_skip_space(cursor + 1);
        if (*cursor == closer) {
            return cursor + 1;
        }
        for (;;) {
            if (closer == '}') {   // a key and its ':'
                cursor = json_skip_value(cursor);
                if (!cursor) {
                    return NULL;
                }
                cursor = json_skip_space(cursor);
                if (*cursor != ':') {
                    return NULL;
                }
                cursor++;
            }
            cursor = json_skip_value(cursor);
            if (!cursor) {
                return NULL;
            }
            cursor = json_skip_space(cursor);
            if (*cursor == ',') {
                cursor = json_skip_space(cursor + 1);
                continue;
            }
            return *cursor == closer ? cursor + 1 : NULL;
        }
    }

    if (!*cursor) {
        return NULL;
    }

    // a bare value (number, true, false, null) runs to the next separator, closing bracket or space
    while (*cursor && *cursor != ',' && *cursor != '}' && *cursor != ']' && !is_json_space(*cursor)) {
        cursor++;
    }
    return cursor;
}

// Four hex digits as 0 to 0xFFFF, else -1.
static int json_hex4(const char *digits) {
    int value = 0;
    for (int i = 0; i < HEX4_DIGITS; i++) {
        char digit = digits[i];
        value <<= 4;
        if (digit >= '0' && digit <= '9') {
            value |= digit - '0';
        } else if (digit >= 'a' && digit <= 'f') {
            value |= digit - 'a' + 10;
        } else if (digit >= 'A' && digit <= 'F') {
            value |= digit - 'A' + 10;
        } else {
            return -1;
        }
    }
    return value;
}

// Reads a string value into out (JSON escapes decoded, \uXXXX as UTF-8). Returns 0 if the value is not a string, or
// on failure; out then holds what was read so far.
static int json_string(const char *cursor, char *out, size_t size) {
    if (!size) {
        return 0;
    }
    out[0] = 0;
    cursor = json_skip_space(cursor);
    if (*cursor != '"') {
        return 0;
    }
    size_t used = 0;

    for (cursor++; *cursor && *cursor != '"'; cursor++) {
        char utf8[4];   // the next character's UTF-8 bytes
        int utf8_len = 1;
        utf8[0] = *cursor;
        if (*cursor == '\\' && cursor[1]) {
            char escaped = *++cursor;
            if (escaped == 'n') {
                utf8[0] = '\n';
            } else if (escaped == 't') {
                utf8[0] = '\t';
            } else if (escaped == 'r') {
                utf8[0] = '\r';
            } else if (escaped == 'b') {
                utf8[0] = '\b';
            } else if (escaped == 'f') {
                utf8[0] = '\f';
            } else if (escaped == 'u') {   // Go's encoder writes & for '&' unless told not to
                int codepoint = json_hex4(cursor + 1);
                if (codepoint < 0) {
                    out[used] = 0;
                    return 0;
                }
                cursor += HEX4_DIGITS;   // on the last hex digit
                // a high surrogate followed by another \u escape: a surrogate pair
                if (codepoint >= SURROGATE_HIGH_FIRST && codepoint < SURROGATE_LOW_FIRST && cursor[1] == '\\' &&
                    cursor[2] == 'u') {
                    int low = json_hex4(cursor + 3);
                    if (low >= SURROGATE_LOW_FIRST && low < SURROGATE_END) {
                        // each surrogate carries 10 bits of the code point
                        codepoint = SUPPLEMENTARY_FIRST + ((codepoint - SURROGATE_HIGH_FIRST) << 10) +
                                    (low - SURROGATE_LOW_FIRST);
                        cursor += UNICODE_ESCAPE_LEN;
                    }
                }
                if (codepoint >= SURROGATE_HIGH_FIRST && codepoint < SURROGATE_END) {
                    codepoint = '?';   // a lone one
                }
                // UTF-8: one byte below 0x80, two below 0x800, three below 0x10000, else four
                if (codepoint < 0x80) {
                    utf8[0] = (char)codepoint;
                } else if (codepoint < 0x800) {
                    utf8[0] = (char)(0xC0 | codepoint >> 6);
                    utf8[1] = (char)(0x80 | (codepoint & 0x3F));
                    utf8_len = 2;
                } else if (codepoint < 0x10000) {
                    utf8[0] = (char)(0xE0 | codepoint >> 12);
                    utf8[1] = (char)(0x80 | (codepoint >> 6 & 0x3F));
                    utf8[2] = (char)(0x80 | (codepoint & 0x3F));
                    utf8_len = 3;
                } else {
                    utf8[0] = (char)(0xF0 | codepoint >> 18);
                    utf8[1] = (char)(0x80 | (codepoint >> 12 & 0x3F));
                    utf8[2] = (char)(0x80 | (codepoint >> 6 & 0x3F));
                    utf8[3] = (char)(0x80 | (codepoint & 0x3F));
                    utf8_len = 4;
                }
            } else {
                utf8[0] = escaped;   // \" \\ \/
            }
        }
        if (used + utf8_len >= size) {   // it doesn't fit: a prefix, reported as a failure
            out[used] = 0;
            return 0;
        }
        memcpy(out + used, utf8, utf8_len);
        used += utf8_len;
    }

    out[used] = 0;
    return *cursor == '"';
}

// The value of key in the object at obj, else NULL.
static const char *json_get(const char *obj, const char *key) {
    obj = json_skip_space(obj);
    if (*obj != '{') {
        return NULL;
    }
    const char *cursor = json_skip_space(obj + 1);

    while (*cursor == '"') {
        char name[JSON_KEY_MAX];
        int match = json_string(cursor, name, sizeof name) && !strcmp(name, key);
        cursor = json_skip_value(cursor);
        if (!cursor) {
            return NULL;
        }
        cursor = json_skip_space(cursor);
        if (*cursor != ':') {
            return NULL;
        }
        cursor = json_skip_space(cursor + 1);
        if (match) {
            return cursor;
        }
        cursor = json_skip_value(cursor);
        if (!cursor) {
            return NULL;
        }
        cursor = json_skip_space(cursor);
        if (*cursor != ',') {
            return NULL;
        }
        cursor = json_skip_space(cursor + 1);
    }

    return NULL;
}

// The first element of the array at arr, else NULL.
static const char *json_first(const char *arr) {
    arr = json_skip_space(arr);
    if (*arr != '[') {
        return NULL;
    }
    arr = json_skip_space(arr + 1);
    return *arr == ']' ? NULL : arr;
}

// The array element after element, else NULL.
static const char *json_next(const char *element) {
    const char *cursor = json_skip_value(element);
    if (!cursor) {
        return NULL;
    }
    cursor = json_skip_space(cursor);
    return *cursor == ',' ? json_skip_space(cursor + 1) : NULL;
}

// Reads the whole file into buf, NUL-terminated. Returns -1 if it is unreadable or fills the buffer (it may be
// truncated).
static ssize_t read_whole_file(const char *path, char *buf, size_t size) {
    buf[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t length = read(fd, buf, size - 1);
    close(fd);
    if (length < 0 || (size_t)length >= size - 1) {
        buf[0] = 0;
        return -1;
    }
    buf[length] = 0;
    return length;
}

// A usable rules file: the whole document parses, and it has a "rules" value that parses. The CLI's xcode.Rules
// checks the same.
static int rules_usable(const char *buf) {
    const char *rules;
    return json_skip_value(buf) && (rules = json_get(buf, "rules")) && json_skip_value(rules);
}

// ---- which Xcode's rules --------------------------------------------------------------------------------------------
// Orders Apple build numbers, <major><train letter><number><suffix> (27A266a < 27A9275 < 27B52). Returns <0, 0 or >0.
static int build_compare(const char *left, const char *right) {
    long left_major = strtol(left, (char **)&left, 10);
    long right_major = strtol(right, (char **)&right, 10);
    if (left_major != right_major) {
        return left_major < right_major ? -1 : 1;
    }
    if (*left != *right) {
        return *left < *right ? -1 : 1;
    }
    if (*left) {
        left++;
        right++;
    }
    long left_number = strtol(left, (char **)&left, 10);
    long right_number = strtol(right, (char **)&right, 10);
    if (left_number != right_number) {
        return left_number < right_number ? -1 : 1;
    }
    return strcmp(left, right);
}

// A rules file name: "<build>.json", the build starting with a digit and short enough for the build buffers below.
static int is_rules_file_name(const char *name, size_t len) {
    if (len < RULES_FILE_NAME_MIN || len > RULES_FILE_NAME_MAX) {
        return 0;
    }
    if (strcmp(name + len - JSON_SUFFIX_LEN, JSON_SUFFIX)) {
        return 0;
    }
    return name[0] >= '0' && name[0] <= '9';
}

static char gRulesJson[RULES_FILE_MAX];     // the rules file in use ("" means the built-in rules)
static char gRulesSource[PATH_NOTE_MAX];  // where the rules in use came from (a path, or "built-in (27A266a)")
static char gXcodeBuild[XCODE_BUILD_MAX]; // the recorded Xcode build, "" if none
static int gRulesVerified;                // the rules file is the recorded build's own

// $SIMCAM_SIM_DIR/config.json ("" if absent, unreadable or of unknown proto)
static char gConfigJson[CONFIG_FILE_MAX];
static int gConfigUnusable;               // config.json exists but can't be used (unreadable, truncated, unknown proto)
static char gSimDir[LOADER_PATH_MAX];     // $SIMCAM_SIM_DIR without trailing slashes
static int gConfigRead;                   // read_config() has run

// Reads config.json once per process, on first use.
static void read_config(void) {
    if (gConfigRead) {
        return;
    }
    gConfigRead = 1;
    const char *sim_dir_env = getenv("SIMCAM_SIM_DIR");
    if (!sim_dir_env || !*sim_dir_env) {
        return;
    }
    snprintf(gSimDir, sizeof gSimDir, "%s", sim_dir_env);
    for (size_t len = strlen(gSimDir); len > 1 && gSimDir[len - 1] == '/'; len--) {
        gSimDir[len - 1] = 0;   // a trailing slash
    }

    char config_path[PATH_NOTE_MAX];
    snprintf(config_path, sizeof config_path, "%s/config.json", gSimDir);

    if (read_whole_file(config_path, gConfigJson, sizeof gConfigJson) <= 0 || !json_skip_value(gConfigJson)) {
        gConfigJson[0] = 0;
        gConfigUnusable = access(config_path, F_OK) == 0;
        log_line("no readable config.json (Xcode build not recorded): ", config_path);
        return;
    }

    const char *proto_value = json_get(gConfigJson, "proto");
    int proto = proto_value ? atoi(proto_value) : 0;

    if (proto != SUPPORTED_CONFIG_PROTO) {
        gConfigJson[0] = 0;
        gConfigUnusable = 1;
        log_line("config.json: unknown proto; Xcode build ignored: ", config_path);
    }
}

// A NAME=value that config.json's "boot_signals" lists. It was in the boot environment, so it says nothing about this
// launch.
static int is_boot_signal(const char *name_value) {
    read_config();
    const char *signals = gConfigJson[0] ? json_get(gConfigJson, "boot_signals") : NULL;
    for (const char *element = signals ? json_first(signals) : NULL; element; element = json_next(element)) {
        char signal[BOOT_SIGNAL_MAX];
        if (json_string(element, signal, sizeof signal) && !strcmp(signal, name_value)) {
            return 1;
        }
    }
    return 0;
}

// Picks the rules file (gRulesJson, gRulesSource, gRulesVerified): the recorded build's own, else the newest build's in
// the same dir, else the built-in rules.
static void pick_rules(void) {
    char dir[LOADER_PATH_MAX] = "";
    const char *value;
    int has_dir = 0;
    read_config();

    if (gConfigJson[0]) {
        // json_string's result is deliberately unused. A value that is not a string leaves "": no Xcode build
        // recorded, or for rules_dir, the built-in rules. A value too long for the buffer (the CLI writes short ones)
        // or with a bad \u escape leaves the prefix read so far, which is used as read.
        if ((value = json_get(gConfigJson, "xcode_build"))) {
            (void)json_string(value, gXcodeBuild, sizeof gXcodeBuild);
        }
        if ((value = json_get(gConfigJson, "rules_dir"))) {   // "" means no rules were staged: the built-in rules
            (void)json_string(value, dir, sizeof dir);
            has_dir = 1;
        }
    }

    // A record we can't use means the built-in rules, never the stale flat dir.
    if (!has_dir && gConfigUnusable) {
        has_dir = 1;
    }

    // No dir recorded (an older record): $SIMCAM_HOME/sims/<udid> gives $SIMCAM_HOME/restore-rules.
    if (!has_dir && gSimDir[0]) {
        snprintf(dir, sizeof dir, "%s", gSimDir);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
        }
        slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
        }
        strncat(dir, "/restore-rules", sizeof dir - strlen(dir) - 1);
    }

    gRulesJson[0] = 0;
    gRulesSource[0] = 0;

    if (gXcodeBuild[0] && dir[0]) {
        snprintf(gRulesSource, sizeof gRulesSource, "%s/%s.json", dir, gXcodeBuild);
        if (read_whole_file(gRulesSource, gRulesJson, sizeof gRulesJson) > 0 && rules_usable(gRulesJson)) {
            gRulesVerified = 1;
            log_line("rules: ", gRulesSource);
            return;
        }
        gRulesJson[0] = 0;
    }

    char best[BUILD_NAME_MAX] = "";   // the best guess: the newest build that has rules
    DIR *dir_stream = dir[0] ? opendir(dir) : NULL;

    for (struct dirent *entry; dir_stream && (entry = readdir(dir_stream));) {
        size_t name_len = strlen(entry->d_name);
        if (!is_rules_file_name(entry->d_name, name_len)) {
            continue;
        }
        char build[BUILD_NAME_MAX];
        snprintf(build, sizeof build, "%.*s", (int)(name_len - JSON_SUFFIX_LEN), entry->d_name);
        if (!best[0] || build_compare(build, best) > 0) {
            snprintf(best, sizeof best, "%s", build);
        }
    }

    if (dir_stream) {
        closedir(dir_stream);
    }

    if (best[0]) {
        snprintf(gRulesSource, sizeof gRulesSource, "%s/%s.json", dir, best);
        if (read_whole_file(gRulesSource, gRulesJson, sizeof gRulesJson) > 0 && rules_usable(gRulesJson)) {
            log_line("rules (best guess, newest): ", gRulesSource);
            return;
        }
        gRulesJson[0] = 0;
    }

    snprintf(gRulesSource, sizeof gRulesSource, "built-in (27A266a)");
    log_line("rules: built-in 27A266a; none readable in ", dir);
}

// The built-in rules are restore-rules/27A266a.json's "xcode-run" and "ui-test-target" libs, used when no rules file
// can be read. Keep them in step with that file; runnerhook.m's builtinRunnerLibs() is the same file's "test-runner"
// rule.
struct restore_lib {
    const char *path;
    const char *condition;   // the rule's "if"
    const char *unless;
};

static const struct restore_lib builtin_xcode[] = {
    {"/usr/lib/libLogRedirect.dylib", "IDE_CHOSEN_LOGGING_METHOD=2", NULL},
    {"/usr/lib/libMainThreadChecker.dylib", NULL, "SIMCAM_MTC=0"},
    {"/usr/lib/libRPAC.dylib", "PERFC_*", NULL},
    {"/usr/lib/libViewDebuggerSupport.dylib", NULL, NULL},
    {NULL, NULL, NULL}};
static const struct restore_lib builtin_ui[] = {
    {"/usr/lib/libLogRedirect.dylib", "IDE_CHOSEN_LOGGING_METHOD=2", NULL},
    {"/usr/lib/libMainThreadChecker.dylib", NULL, "SIMCAM_MTC=0"},
    {"/usr/lib/libRPAC.dylib", "PERFC_*", NULL},
    {NULL, NULL, NULL}};

// Tests a condition in the restore-rules "if"/"unless" syntax: VAR=value (equal), PREFIX* (any variable with that
// prefix) or VAR (set). runnerhook.m's condHolds() matches the same syntax against an NSDictionary. The two must stay
// in step; they are separate builds, because this file is libSystem-only C and that one uses Foundation.
static int cond_holds(const char *condition) {
    if (!condition || !*condition) {
        return 1;
    }
    size_t len = strlen(condition);
    if (condition[len - 1] == '*') {
        for (char **var = environ; *var; var++) {
            if (!strncmp(*var, condition, len - 1)) {
                return 1;
            }
        }
        return 0;
    }
    const char *eq = strchr(condition, '=');
    if (!eq) {
        return getenv(condition) != NULL;
    }
    char name[CONDITION_MAX];
    snprintf(name, sizeof name, "%.*s", (int)(eq - condition), condition);
    const char *value = getenv(name);
    return value && !strcmp(value, eq + 1);
}

// Loads one lib of a rule; an absolute path gets the runtime root in front.
static void load_rule_lib(const char *root, const char *rel) {
    char path[LOADER_PATH_MAX];
    snprintf(path, sizeof path, "%s%s", rel[0] == '/' ? root : "", rel);
    try_load(path, base_name(rel));
}

// Loads the libs of the named rule from the rules file, else from builtin.
static void restore_rule(const char *rule, const struct restore_lib *builtin, const char *root) {
    const char *rules = gRulesJson[0] ? json_get(gRulesJson, "rules") : NULL;
    const char *libs = NULL;

    for (const char *entry = rules ? json_first(rules) : NULL; entry; entry = json_next(entry)) {
        char name[RULE_NAME_MAX];
        const char *value = json_get(entry, "name");
        if (value && json_string(value, name, sizeof name) && !strcmp(name, rule)) {
            libs = json_get(entry, "libs");
            break;
        }
    }

    if (!libs) {
        if (gRulesJson[0]) {
            log_line("rules file has no libs for this rule; built-in used: ", rule);
        }
        for (const struct restore_lib *lib = builtin; lib->path; lib++) {
            if (cond_holds(lib->condition) && !(lib->unless && cond_holds(lib->unless))) {
                load_rule_lib(root, lib->path);
            }
        }
        return;
    }

    for (const char *lib = json_first(libs); lib; lib = json_next(lib)) {
        char path[RULE_LIB_PATH_MAX] = "";
        char if_cond[CONDITION_MAX] = "";
        char unless_cond[CONDITION_MAX] = "";
        const char *value;
        if (!(value = json_get(lib, "path")) || !json_string(value, path, sizeof path) || !path[0]) {
            continue;
        }
        if ((value = json_get(lib, "if")) && !json_string(value, if_cond, sizeof if_cond)) {
            log_line("rules: unreadable condition; lib skipped: ", path);
            continue;
        }
        if ((value = json_get(lib, "unless")) && !json_string(value, unless_cond, sizeof unless_cond)) {
            log_line("rules: unreadable condition; lib skipped: ", path);
            continue;
        }
        if (cond_holds(if_cond) && !(unless_cond[0] && cond_holds(unless_cond))) {
            load_rule_lib(root, path);
        }
    }
}

// ---- displaced per-launch inserts (item 2 of the header) --------------------------------------------------------
// One of Guard Malloc's own settings (NAME=value). libmalloc's MallocGuardEdges, MallocScribble and MallocStackLogging
// work without the insert, so they are not signals.
static int is_guard_malloc_setting(const char *var) {
    return has_prefix(var, "MALLOC_PROTECT_BEFORE=") || has_prefix(var, "MALLOC_FILL_SPACE=") ||
           has_prefix(var, "MALLOC_STRICT_SIZE=") || has_prefix(var, "MALLOC_ALLOW_READS=") ||
           has_prefix(var, "MALLOC_VECTOR_SIZE=") || has_prefix(var, "MALLOC_PERMIT_INSANE_REQUESTS=");
}

// Adds to gDisplaced each tool whose per-launch insert boot mode displaced.
static void detect_displaced(void) {
    // A launch whose own list still holds something besides our stub merged it with ours (the stuntcam env): nothing
    // was displaced.
    const char *insert_env = getenv("DYLD_INSERT_LIBRARIES");

    if (insert_env) {
        char list[INSERT_ENV_COPY_MAX];
        snprintf(list, sizeof list, "%s", insert_env);
        char *strtok_state = NULL;
        for (char *entry = strtok_r(list, ":", &strtok_state); entry; entry = strtok_r(NULL, ":", &strtok_state)) {
            if (*entry && !has_prefix(base_name(entry), "libsimcam")) {
                return;
            }
        }
    }

    int *argc = _NSGetArgc();
    char ***argv = _NSGetArgv();
    int have_args = argc && argv && *argv;

    for (int i = 1; have_args && i < *argc; i++) {
        if (!strcmp((*argv)[i], "-detoxServer") || !strcmp((*argv)[i], "-detoxSessionId")) {
            if (!mapped_image("Detox", 1)) {
                list_add(gDisplaced, sizeof gDisplaced, "Detox");
            }
            break;
        }
    }

    int idb = 0;
    int guard_malloc = 0;
    int asan = 0;
    int tsan = 0;

    // is_boot_signal() reads config.json only when a variable looks like a signal
    for (char **var = environ; *var; var++) {
        if (has_prefix(*var, "IDB_") && !is_boot_signal(*var)) {
            idb = 1;
        }
        if (is_guard_malloc_setting(*var) && !is_boot_signal(*var)) {
            guard_malloc = 1;
        }
        if (has_prefix(*var, "ASAN_OPTIONS=") && !is_boot_signal(*var)) {
            asan = 1;
        }
        if (has_prefix(*var, "TSAN_OPTIONS=") && !is_boot_signal(*var)) {
            tsan = 1;
        }
    }

    if (idb) {
        list_add(gDisplaced, sizeof gDisplaced, "idb");
    }
    if (guard_malloc && !mapped_image("libgmalloc.dylib", 1)) {
        list_add(gDisplaced, sizeof gDisplaced, "Guard Malloc");
    }
    if (asan && !mapped_image("libclang_rt.asan", 0)) {
        list_add(gDisplaced, sizeof gDisplaced, "AddressSanitizer");
    }
    if (tsan && !mapped_image("libclang_rt.tsan", 0)) {
        list_add(gDisplaced, sizeof gDisplaced, "ThreadSanitizer");
    }
}

// ---- the constructor's steps ----------------------------------------------------------------------------------------
// An app process this loader acts in, by the same test as the stub: an executable in a .app bundle, in the device's
// data dir or an Xcode build-products dir, and not under the runtime root.
static int is_app_process(const char *exe, const char *root) {
    if (!strstr(exe, ".app/")) {
        return 0;
    }
    if (!strstr(exe, "/data/") && !strstr(exe, "/Build/Products/")) {
        return 0;
    }
    if (root && *root && !strncmp(exe, root, strlen(root))) {
        return 0;   // under the runtime root
    }
    return 1;
}

static void open_log(void) {
    const char *log_path = getenv("SIMCAM_STUB_LOG");
    if (log_path) {
        gLogFd = open(log_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    }
}

// What the environment says about who launched this process.
struct launch_markers {
    int xcode;    // Xcode launched it (MTC_/PERFC_/IDE_ variables, DYLD_LIBRARY_PATH in DerivedData)
    int xctest;   // an XCTest process (XCInjectBundleInto, XCTestConfigurationFilePath, ...)
    int perfc;    // a PERFC_ variable is set
};

// A variable (NAME=value) that only an XCTest process has.
static int is_xctest_marker(const char *var) {
    return has_prefix(var, "XCInjectBundleInto=") || has_prefix(var, "XCTestConfigurationFilePath=") ||
           has_prefix(var, "XCTestBundlePath=") || has_prefix(var, "XCTestSessionIdentifier=");
}

// One pass over the environment. With SIMCAM_STUB_ENVDUMP set, each variable is also logged.
static struct launch_markers scan_launch_env(void) {
    struct launch_markers markers = {0, 0, 0};
    for (char **var = environ; *var; var++) {
        if (has_prefix(*var, "MTC_") || has_prefix(*var, "PERFC_") || has_prefix(*var, "IDE_")) {
            markers.xcode = 1;
        }
        if (has_prefix(*var, "PERFC_")) {
            markers.perfc = 1;
        }
        if (has_prefix(*var, "DYLD_LIBRARY_PATH=") && strstr(*var, "/DerivedData/")) {
            markers.xcode = 1;
        }
        if (is_xctest_marker(*var)) {
            markers.xctest = 1;
        }
        if (getenv("SIMCAM_STUB_ENVDUMP")) {
            log_line("env ", *var);
        }
    }
    return markers;
}

static void log_restore_inputs(struct launch_markers markers) {
    char line[RESTORE_INPUTS_LINE_MAX];
    snprintf(line, sizeof line, "xctest=%d inject=%d xcode=%d perfc=%d uitarget=%d logmethod=%s", markers.xctest,
             getenv("XCTestBundleInjectPath") != NULL, markers.xcode, markers.perfc,
             getenv("XCODE_SCHEME_NAME") != NULL,
             getenv("IDE_CHOSEN_LOGGING_METHOD") ? getenv("IDE_CHOSEN_LOGGING_METHOD") : "-");
    log_line("restore inputs: ", line);
}

// Writes into out the path an inject-list entry loads from. A runtime-relative entry (/usr/lib/libRPAC.dylib) gets the
// runtime root in front when that file exists. Everything else loads as given: DerivedData anywhere (including
// /Volumes, /private/var and /tmp), and paths already under the root.
static void inject_entry_path(const char *entry, const char *root, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", entry);
    if (!root) {
        return;
    }
    int runtime_relative = !strncmp(entry, USR_PREFIX, USR_PREFIX_LEN) ||
                           !strncmp(entry, SYSTEM_PREFIX, SYSTEM_PREFIX_LEN);
    if (!runtime_relative) {
        return;
    }
    if (!strncmp(entry, root, strlen(root))) {
        return;   // already under the root
    }
    char rooted[LOADER_PATH_MAX];
    snprintf(rooted, sizeof rooted, "%s%s", root, entry);
    if (access(rooted, F_OK) == 0) {
        snprintf(out, out_size, "%s", rooted);
    }
}

// Hosted tests: XCTest copied the displaced DYLD_INSERT_LIBRARIES list to XCTestBundleInjectPath. Each entry is loaded
// once, in order, except our own libraries. The list is XCTest's own, and launch_only() alone decides what never loads
// late: the rules play no part here, and are picked only for the "unverified" line.
static void restore_inject_list(const char *inject_path, const char *root) {
    pick_rules();
    char load_path[LOADER_PATH_MAX];
    char list[INSERT_LIST_MAX];
    strncpy(list, inject_path, sizeof list - 1);
    list[sizeof list - 1] = 0;
    char seen[INJECT_LIST_ENTRIES_MAX][LOADER_PATH_MAX];
    int seen_count = 0;

    // strtok_r, not strtok: try_load runs the library's initializers (test-bundle code), which may use strtok
    char *strtok_state = NULL;

    for (char *entry = strtok_r(list, ":", &strtok_state); entry; entry = strtok_r(NULL, ":", &strtok_state)) {
        int duplicate = 0;
        for (int i = 0; i < seen_count; i++) {
            if (!strcmp(seen[i], entry)) {
                duplicate = 1;
            }
        }
        if (duplicate) {
            continue;
        }
        if (seen_count >= INJECT_LIST_ENTRIES_MAX || strlen(entry) >= LOADER_PATH_MAX - 1) {
            log_line("inject list: entry skipped (more than 32, or path too long): ", entry);
            continue;
        }
        strncpy(seen[seen_count], entry, LOADER_PATH_MAX - 1);
        seen[seen_count++][LOADER_PATH_MAX - 1] = 0;
        if (strstr(entry, "libsimcamstub") || strstr(entry, "libsimcamloader")) {
            continue;
        }
        inject_entry_path(entry, root, load_path, sizeof load_path);
        try_load(load_path, load_path);   // log the path actually loaded (it may carry the runtime root)
    }
}

// An Xcode launch without a hosted-test inject list. For an XCUIApplication target, the rules file's "ui-test-target"
// libs: MTC and RPAC, plus LogRedirect under the Xcode GUI, but no view debugger. Otherwise its "xcode-run" libs.
static void restore_xcode_libs(const char *root) {
    int uitarget = getenv("XCODE_SCHEME_NAME") != NULL;
    pick_rules();
    if (uitarget) {
        log_line("ui-test target: restoring MTC/RPAC, root=", root);
        restore_rule("ui-test-target", builtin_ui, root);
    } else {
        log_line("xcode run: restoring Xcode's libraries, root=", root);
        restore_rule("xcode-run", builtin_xcode, root);
    }
}

// Loads the engine ($SIMCAM_DYLIB, else libsimcam.dylib next to this loader), except in UI-test runners and app
// extensions.
static void load_engine(const char *exe, const Dl_info *self) {
    if (strstr(exe, "-Runner.app/")) {
        log_line("xctest runner: not loading our dylib", 0);
        return;
    }

    if (strstr(exe, ".appex/")) {
        log_line("app extension: not loading our dylib", 0);
        return;
    }

    const char *engine_path = getenv("SIMCAM_DYLIB");
    char sibling[LOADER_PATH_MAX];

    if (!engine_path && self->dli_fname) {   // the engine next to this loader
        const char *slash = strrchr(self->dli_fname, '/');
        if (slash) {
            snprintf(sibling, sizeof sibling, "%.*s/libsimcam.dylib", (int)(slash - self->dli_fname), self->dli_fname);
            engine_path = sibling;
        }
    }

    if (engine_path) {
        void *handle = dlopen(engine_path, RTLD_NOW | RTLD_GLOBAL);
        log_line("real dylib", handle ? " loaded" : dlerror());
    } else {
        log_line("no engine path", 0);
    }
}

// The one-line announcements, after everything that can add to them (the restores and the chain). ruled: rules were
// picked.
static void announce(int ruled) {
    if (gSkipped[0]) {
        log_line("skipped (must load at launch, or a second copy): ", gSkipped);
    }

    if (gChainSkipped[0]) {
        char line[PATH_NOTE_MAX];
        snprintf(line, sizeof line, "stuntcam chain: not loaded, a copy is already loaded: %s", gChainSkipped);
        say(line);
    }

    if (ruled && !gRulesVerified) {
        char line[UNVERIFIED_LINE_MAX];
        snprintf(line, sizeof line,
                 "restore rules unverified for Xcode %s; if console/debugger misbehave use --mode launchd",
                 gXcodeBuild[0] ? gXcodeBuild : "(not recorded)");
        say(line);
        log_line("  rules used: ", gRulesSource);
    }

    detect_displaced();

    if (gDisplaced[0]) {
        char line[DISPLACED_LINE_MAX];
        snprintf(line, sizeof line, "stuntcam boot mode displaced %s; use 'stuntcam up --mode launchd'", gDisplaced);
        say(line);
    }
}

__attribute__((constructor)) static void simcam_loader(void) {
    // Is this process one we act in?
    static char exe[LOADER_PATH_MAX];
    uint32_t size = sizeof exe;

    // (_dyld_get_image_name(0) would name the stub itself at this point)
    if (_NSGetExecutablePath(exe, &size) != 0) {
        return;
    }
    const char *enabled = getenv("SIMCAM");

    // The stub checked these too, but it can be pointed at this loader directly (SIMCAM_LOADER).
    if (enabled && enabled[0] == '0') {
        return;
    }

    // The simulator runtime root, read once, before any library is loaded.
    const char *root = getenv("SIMULATOR_ROOT");
    if (!root) {
        root = getenv("DYLD_ROOT_PATH");
    }
    if (!is_app_process(exe, root)) {
        return;
    }
    open_log();
    Dl_info self = {0};
    dladdr((const void *)simcam_loader, &self);
    log_line("loader ", self.dli_fname);

    // Step 1 of the file header: restore what Xcode or XCTest would have inserted.
    struct launch_markers markers = scan_launch_env();
    const char *xcode_libs = getenv("SIMCAM_XCODE_LIBS");
    int restores_off = xcode_libs && xcode_libs[0] == '0';
    log_restore_inputs(markers);
    int ruled = 0;   // rules were picked (for the "unverified" line)
    const char *inject_path = getenv("XCTestBundleInjectPath");

    if (markers.xctest && inject_path && !restores_off) {
        restore_inject_list(inject_path, root);
        ruled = 1;
    } else if (markers.xcode && !restores_off && root) {
        restore_xcode_libs(root);
        ruled = 1;
    }

    // Steps 3 and 4: the engine and the chain file. Then step 2, the announcements, which the restores and the chain
    // can add to.
    load_engine(exe, &self);
    load_chain();
    announce(ruled);

    if (gLogFd >= 0) {
        close(gLogFd);
        gLogFd = -1;
    }
}
