// SPDX-License-Identifier: Apache-2.0
// Host unit test for the loader's pure helpers (JSON reader, build-number order, conditions, launch-only libraries):
//   xcrun clang -Wall -o "$TMPDIR/loader_test" engine/loader/loader_test.c &&
//     "$TMPDIR/loader_test" restore-rules/27A266a.json
// The loader's constructor also runs here; it returns at once (this executable is not in a .app).
#include "loader.c"

static int fails;

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c);         \
            fails++;                                                            \
        }                                                                       \
    } while (0)

enum { TEST_PATH_MAX = 1200 };

// Writes data to path, opened with O_WRONLY | flags. Returns what write() returned.
static ssize_t write_file(const char *path, int flags, const char *data, size_t len) {
    int fd = open(path, O_WRONLY | flags, 0644);
    ssize_t written = write(fd, data, len);
    close(fd);
    return written;
}

// Forgets what the loader read from config.json and picks the rules again.
static void repick_rules(void) {
    gConfigRead = 0;
    gRulesVerified = 0;
    gXcodeBuild[0] = 0;
    pick_rules();
}

static int count_occurrences(const char *haystack, const char *needle) {
    int count = 0;
    for (const char *at = haystack; (at = strstr(at, needle)); at++) {
        count++;
    }
    return count;
}

static int count_elements(const char *array) {
    int count = 0;
    for (const char *element = json_first(array); element; element = json_next(element)) {
        count++;
    }
    return count;
}

static void test_json(void) {
    char out[256];
    const char *doc = "{ \"a\": [1, {\"x\": \"y\"}], \"proto\": 1, \"s\": \"q\\\"z\", \"o\": {\"k\": \"v\"} }";
    CHECK(json_get(doc, "proto") && atoi(json_get(doc, "proto")) == 1);
    CHECK(json_string(json_get(doc, "s"), out, sizeof out) && !strcmp(out, "q\"z"));
    CHECK(json_string(json_get(json_get(doc, "o"), "k"), out, sizeof out) && !strcmp(out, "v"));
    CHECK(json_get(doc, "missing") == NULL);
    const char *array = json_get(doc, "a");
    const char *first = json_first(array);
    const char *second = json_next(first);
    CHECK(first && atoi(first) == 1);
    CHECK(second && json_string(json_get(second, "x"), out, sizeof out) && !strcmp(out, "y"));
    CHECK(json_next(second) == NULL);
    // truncated input: no crash
    CHECK(json_first("[]") == NULL && json_get("{\"a\": ", "a") != NULL && json_skip_value("{\"a\": ") == NULL);
}

static void test_json_escapes(void) {
    char out[256];
    // Go's \u0026 for '&', other \u, \/ and a surrogate pair
    CHECK(json_string("\"/a\\u0026b\\/c\"", out, sizeof out) && !strcmp(out, "/a&b/c"));
    CHECK(json_string("\"\\u003cx\\u003e \\u00e9 \\ud83d\\ude00\"", out, sizeof out) &&
          !strcmp(out, "<x> \xc3\xa9 \xf0\x9f\x98\x80"));
    CHECK(!json_string("\"\\u00zz\"", out, sizeof out));
    // never half a character; a cut-off value fails
    char small[4];
    CHECK(!json_string("\"\\u00e9\\u00e9\"", small, sizeof small) && !strcmp(small, "\xc3\xa9"));
    // stops at the first character that doesn't fit
    char two[2];
    CHECK(!json_string("\"\\u00e9a\"", two, sizeof two) && two[0] == 0);
    // terminated on a bad escape
    char eight[8] = "zzzzzzz";
    CHECK(!json_string("\"ab\\u00zz\"", eight, sizeof eight) && !strcmp(eight, "ab"));
}

static void test_build_numbers(void) {
    CHECK(build_compare("27A266a", "27A9275") < 0 && build_compare("27A9275", "27B52") < 0 &&
          build_compare("26E5", "27A266a") < 0);
    CHECK(build_compare("27A266a", "27A266a") == 0 && build_compare("27A266b", "27A266a") > 0);
    CHECK(build_compare("100A1", "27Z999") > 0);
}

static void test_conditions(void) {
    setenv("U4T_A", "2", 1);
    setenv("U4T_PREFIX_X", "1", 1);
    CHECK(cond_holds(NULL) && cond_holds(""));
    CHECK(cond_holds("U4T_A=2") && !cond_holds("U4T_A=3"));
    CHECK(cond_holds("U4T_A") && !cond_holds("U4T_NONE"));
    CHECK(cond_holds("U4T_PREFIX_*") && !cond_holds("U4T_NOPE_*"));
}

// The stub's app test: a .app executable in a data dir or build products, not under the runtime root.
static void test_app_process(void) {
    CHECK(is_app_process("/Users/x/Library/Developer/CoreSimulator/Devices/D/data/Containers/Bundle/Application/X/"
                         "A.app/A",
                         "/rt"));
    CHECK(is_app_process("/Users/x/DerivedData/P/Build/Products/Debug-iphonesimulator/A.app/A", NULL));
    CHECK(!is_app_process("/rt/data/Applications/A.app/A", "/rt") && !is_app_process("/x/data/bin/tool", "/rt"));
    CHECK(!is_app_process("/Applications/A.app/A", "/rt") && is_app_process("/rt/data/A.app/A", ""));
}

static void test_launch_only(void) {
    CHECK(launch_only("/x/Frameworks/libclang_rt.asan_iossim_dynamic.dylib"));
    CHECK(launch_only("/usr/lib/libgmalloc.dylib"));
    CHECK(!launch_only("/usr/lib/libMainThreadChecker.dylib") && !launch_only("/usr/lib/libgmalloc.dylib.bak"));
    CHECK(!strcmp(tool_of("/a/libclang_rt.tsan_iossim_dynamic.dylib"), "ThreadSanitizer"));
    // A skipped runtime that is not loaded here is displaced; try_load never dlopens it.
    CHECK(try_load("/nonexistent/libclang_rt.asan_iossim_dynamic.dylib", "t") == TRYLOAD_SKIPPED_LAUNCH_ONLY);
    CHECK(strstr(gSkipped, "not in this process") && strstr(gDisplaced, "AddressSanitizer"));
    try_load("/nonexistent/libclang_rt.asan_iossim_dynamic.dylib", "t");
    CHECK(count_occurrences(gSkipped, "libclang_rt.asan") == 1);   // listed once
    // A mapped image counts as already loaded (libSystem is mapped in every process).
    CHECK(mapped_image("libSystem.B.dylib", 1) != NULL && mapped_image("libSystem", 0) != NULL);
    CHECK(mapped_image("libNoSuch", 0) == NULL);
}

// A usable rules file: the whole document and its "rules" value parse.
static void test_rules_ok(void) {
    CHECK(rules_usable("{\"proto\": 1, \"rules\": [{\"name\": \"x\"}]}"));
    CHECK(!rules_usable("{\"proto\": 1}") && !rules_usable("{\"rules\": [{\"name\": \"x\"}"));
    CHECK(!rules_usable("{\"rules\": [1, 2"));
}

// read_whole_file: a file that fills the buffer counts as unreadable (it may be truncated).
static void test_slurp(const char *tmp) {
    char path[TEST_PATH_MAX];
    char buf8[8];
    snprintf(path, sizeof path, "%s/seven", tmp);
    CHECK(write_file(path, O_CREAT, "1234567", 7) == 7);
    CHECK(read_whole_file(path, buf8, sizeof buf8) == -1 && buf8[0] == 0);
    snprintf(path, sizeof path, "%s/six", tmp);
    CHECK(write_file(path, O_CREAT, "123456", 6) == 6);
    CHECK(read_whole_file(path, buf8, sizeof buf8) == 6 && !strcmp(buf8, "123456"));
}

// pick_rules: SIMCAM_SIM_DIR with a trailing slash and no rules_dir gives <home>/restore-rules; a rules_dir with '&'
// written by Go as \u0026; config.json's boot_signals.
static void test_pick_rules(const char *tmp) {
    char path[TEST_PATH_MAX];
    char sim_dir[TEST_PATH_MAX];
    snprintf(sim_dir, sizeof sim_dir, "%s/h&x/sims/U", tmp);
    snprintf(path, sizeof path, "mkdir -p '%s' '%s/../../restore-rules' '%s/../../rr&2'", sim_dir, sim_dir, sim_dir);
    CHECK(!system(path));
    const char *rules = "{\"proto\": 1, \"rules\": [{\"name\": \"xcode-run\", \"libs\": []}]}";
    snprintf(path, sizeof path, "%s/../../restore-rules/27A266a.json", sim_dir);
    CHECK(write_file(path, O_CREAT, rules, strlen(rules)) > 0);
    snprintf(path, sizeof path, "%s/../../rr&2/27A266a.json", sim_dir);
    CHECK(write_file(path, O_CREAT, rules, strlen(rules)) > 0);
    char config_path[TEST_PATH_MAX];
    snprintf(config_path, sizeof config_path, "%s/config.json", sim_dir);

    const char *config1 = "{\"proto\": 1, \"xcode_build\": \"27A266a\", "
                          "\"boot_signals\": [\"ASAN_OPTIONS=detect_leaks=0\", \"IDB_X=1\"]}";
    CHECK(write_file(config_path, O_CREAT | O_TRUNC, config1, strlen(config1)) > 0);
    snprintf(path, sizeof path, "%s///", sim_dir);
    setenv("SIMCAM_SIM_DIR", path, 1);
    repick_rules();
    CHECK(gRulesVerified && strstr(gRulesSource, "/h&x/restore-rules/27A266a.json") && !strstr(gRulesSource, "//"));
    CHECK(is_boot_signal("ASAN_OPTIONS=detect_leaks=0") && is_boot_signal("IDB_X=1"));
    CHECK(!is_boot_signal("ASAN_OPTIONS=other") && !is_boot_signal("TSAN_OPTIONS=detect_leaks=0"));

    char config2[2400];
    snprintf(config2, sizeof config2,
             "{\"proto\": 1, \"xcode_build\": \"27A266a\", \"rules_dir\": \"%s/h\\u0026x/rr\\u00262\"}", tmp);
    CHECK(write_file(config_path, O_CREAT | O_TRUNC, config2, strlen(config2)) > 0);
    repick_rules();
    CHECK(gRulesVerified && strstr(gRulesSource, "/h&x/rr&2/27A266a.json"));
    CHECK(!is_boot_signal("ASAN_OPTIONS=detect_leaks=0"));

    // A rules file without a parsable "rules" value is not verified; the best guess skips to the built-in rules.
    snprintf(path, sizeof path, "%s/../../rr&2/27A266a.json", sim_dir);
    CHECK(write_file(path, O_TRUNC, "{\"rules\": [", 11) == 11);
    repick_rules();
    CHECK(!gRulesVerified && strstr(gRulesSource, "built-in"));

    // rules_dir "" (nothing staged): the built-in rules, never a best guess from the flat legacy dir.
    snprintf(config2, sizeof config2, "{\"proto\": 1, \"xcode_build\": \"27A266a\", \"rules_dir\": \"\"}");
    CHECK(write_file(config_path, O_CREAT | O_TRUNC, config2, strlen(config2)) > 0);
    repick_rules();
    CHECK(!gRulesVerified && strstr(gRulesSource, "built-in"));

    // A config.json that exists but is unusable (here: an unknown proto): the built-in rules, never the flat legacy
    // dir.
    snprintf(config2, sizeof config2, "{\"proto\": 9, \"xcode_build\": \"27A266a\"}");
    CHECK(write_file(config_path, O_CREAT | O_TRUNC, config2, strlen(config2)) > 0);
    gConfigUnusable = 0;
    repick_rules();
    CHECK(gConfigUnusable && !gRulesVerified && strstr(gRulesSource, "built-in"));
}

// A chain entry whose basename is already mapped from another path is skipped as a second copy, never dlopened.
static void test_second_copy(void) {
    CHECK(try_load("/nonexistent/libSystem.B.dylib", "t") == TRYLOAD_SKIPPED_SECOND_COPY);
    CHECK(strstr(gSkipped, "/nonexistent/libSystem.B.dylib [second copy; already loaded: "));
}

// inject_entry_path: /usr/ and /System/ entries get the runtime root when the file is there; anything else as given.
static void test_inject_entry_path(const char *tmp) {
    char root[TEST_PATH_MAX];
    char path[TEST_PATH_MAX];
    char out[1024];
    snprintf(root, sizeof root, "%s/rt", tmp);
    snprintf(path, sizeof path, "mkdir -p '%s/usr/lib' && : > '%s/usr/lib/libX.dylib'", root, root);
    CHECK(!system(path));
    inject_entry_path("/usr/lib/libX.dylib", root, out, sizeof out);
    snprintf(path, sizeof path, "%s/usr/lib/libX.dylib", root);
    CHECK(!strcmp(out, path));
    inject_entry_path("/usr/lib/libAbsent.dylib", root, out, sizeof out);
    CHECK(!strcmp(out, "/usr/lib/libAbsent.dylib"));
    inject_entry_path(path, root, out, sizeof out);   // neither /usr/ nor /System/: as given
    CHECK(!strcmp(out, path));
    inject_entry_path("/tmp/DerivedData/A.app/Frameworks/libXCTestBundleInject.dylib", root, out, sizeof out);
    CHECK(!strcmp(out, "/tmp/DerivedData/A.app/Frameworks/libXCTestBundleInject.dylib"));
    inject_entry_path("/usr/lib/libX.dylib", NULL, out, sizeof out);
    CHECK(!strcmp(out, "/usr/lib/libX.dylib"));
}

// The real rules file: both loader rules have libs, in order. It has 5 rules: hosted-tests, ui-test-target,
// ui-test-runner, test-runner (read by the runner hook) and xcode-run.
static void test_rules_file(const char *rules_path) {
    CHECK(read_whole_file(rules_path, gRulesJson, sizeof gRulesJson) > 0);
    const char *rules = json_get(gRulesJson, "rules");
    CHECK(rules != NULL);
    if (!rules) {
        return;
    }
    int rule_count = 0;
    int found = 0;
    for (const char *rule = json_first(rules); rule; rule = json_next(rule), rule_count++) {
        char name[64];
        json_string(json_get(rule, "name"), name, sizeof name);
        if (!strcmp(name, "xcode-run")) {
            const char *libs = json_get(rule, "libs");
            char path[256];
            CHECK(json_string(json_get(json_first(libs), "path"), path, sizeof path) &&
                  !strcmp(path, "/usr/lib/libLogRedirect.dylib"));
            CHECK(count_elements(libs) == 4);
            found++;
        }
        if (!strcmp(name, "ui-test-target")) {
            CHECK(count_elements(json_get(rule, "libs")) == 3);
            found++;
        }
    }
    CHECK(rule_count == 5 && found == 2);
}

int main(int argc, char **argv) {
    test_json();
    test_build_numbers();
    test_conditions();
    test_app_process();
    test_launch_only();
    test_json_escapes();
    test_rules_ok();
    char tmp_template[] = "/tmp/loader_test.XXXXXX";
    const char *tmp = mkdtemp(tmp_template);
    CHECK(tmp != NULL);
    test_slurp(tmp);
    test_pick_rules(tmp);
    test_second_copy();
    test_inject_entry_path(tmp);
    char command[TEST_PATH_MAX];
    snprintf(command, sizeof command, "rm -rf '%s'", tmp);
    CHECK(!system(command));
    unsetenv("SIMCAM_SIM_DIR");
    if (argc > 1) {
        test_rules_file(argv[1]);
    }
    printf("%s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
