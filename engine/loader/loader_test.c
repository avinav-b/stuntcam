// SPDX-License-Identifier: Apache-2.0
// Host unit test for the loader's pure helpers (JSON reader, build-number order, conditions, launch-only libraries):
//   xcrun clang -Wall -o "$TMPDIR/loader_test" engine/loader/loader_test.c && "$TMPDIR/loader_test" restore-rules/27A266a.json
// The loader's constructor also runs here; it returns at once (this executable is not in a .app).
#include "loader.c"
static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
int main(int argc, char **argv) {
    char s[256];
    // JSON
    const char *j = "{ \"a\": [1, {\"x\": \"y\"}], \"proto\": 1, \"s\": \"q\\\"z\", \"o\": {\"k\": \"v\"} }";
    CHECK(jget(j, "proto") && atoi(jget(j, "proto")) == 1);
    CHECK(jstr(jget(j, "s"), s, sizeof s) && !strcmp(s, "q\"z"));
    CHECK(jstr(jget(jget(j, "o"), "k"), s, sizeof s) && !strcmp(s, "v"));
    CHECK(jget(j, "missing") == NULL);
    const char *a = jget(j, "a"), *e1 = jfirst(a), *e2 = jnext(e1);
    CHECK(e1 && atoi(e1) == 1 && e2 && jstr(jget(e2, "x"), s, sizeof s) && !strcmp(s, "y") && jnext(e2) == NULL);
    CHECK(jfirst("[]") == NULL && jget("{\"a\": ", "a") != NULL && jskip("{\"a\": ") == NULL);   // truncated input: no crash
    // build numbers
    CHECK(buildcmp("27A266a", "27A9275") < 0 && buildcmp("27A9275", "27B52") < 0 && buildcmp("26E5", "27A266a") < 0);
    CHECK(buildcmp("27A266a", "27A266a") == 0 && buildcmp("27A266b", "27A266a") > 0 && buildcmp("100A1", "27Z999") > 0);
    // conditions
    setenv("U4T_A", "2", 1); setenv("U4T_PREFIX_X", "1", 1);
    CHECK(cond(NULL) && cond("") && cond("U4T_A=2") && !cond("U4T_A=3") && cond("U4T_A") && !cond("U4T_NONE"));
    CHECK(cond("U4T_PREFIX_*") && !cond("U4T_NOPE_*"));
    // the app test (the stub's): a .app executable in a data dir or build products, not under the runtime root
    CHECK(is_app_process("/Users/x/Library/Developer/CoreSimulator/Devices/D/data/Containers/Bundle/Application/X/A.app/A", "/rt"));
    CHECK(is_app_process("/Users/x/DerivedData/P/Build/Products/Debug-iphonesimulator/A.app/A", NULL));
    CHECK(!is_app_process("/rt/data/Applications/A.app/A", "/rt") && !is_app_process("/x/data/bin/tool", "/rt"));
    CHECK(!is_app_process("/Applications/A.app/A", "/rt") && is_app_process("/rt/data/A.app/A", ""));
    // launch-only libraries
    CHECK(launch_only("/x/Frameworks/libclang_rt.asan_iossim_dynamic.dylib") && launch_only("/usr/lib/libgmalloc.dylib"));
    CHECK(!launch_only("/usr/lib/libMainThreadChecker.dylib") && !launch_only("/usr/lib/libgmalloc.dylib.bak"));
    CHECK(!strcmp(toolof("/a/libclang_rt.tsan_iossim_dynamic.dylib"), "ThreadSanitizer"));
    // a skipped runtime: not loaded here -> displaced; tryload never dlopens it
    CHECK(tryload("/nonexistent/libclang_rt.asan_iossim_dynamic.dylib", "t") == TRYLOAD_SKIPPED_LAUNCH_ONLY);
    CHECK(strstr(skipped, "not in this process") && strstr(displaced, "AddressSanitizer"));
    tryload("/nonexistent/libclang_rt.asan_iossim_dynamic.dylib", "t");   // listed once
    { int k = 0; for (const char *q = skipped; (q = strstr(q, "libclang_rt.asan")); q++) k++; CHECK(k == 1); }
    // a mapped image counts as already loaded (libSystem is mapped in every process)
    CHECK(mapped("libSystem.B.dylib", 1) != NULL && mapped("libSystem", 0) != NULL && mapped("libNoSuch", 0) == NULL);
    // JSON string escapes: Go's \u0026 for '&', other \u, \/ and a surrogate pair
    CHECK(jstr("\"/a\\u0026b\\/c\"", s, sizeof s) && !strcmp(s, "/a&b/c"));
    CHECK(jstr("\"\\u003cx\\u003e \\u00e9 \\ud83d\\ude00\"", s, sizeof s) && !strcmp(s, "<x> \xc3\xa9 \xf0\x9f\x98\x80"));
    CHECK(!jstr("\"\\u00zz\"", s, sizeof s));
    { char small[4]; CHECK(!jstr("\"\\u00e9\\u00e9\"", small, sizeof small) && !strcmp(small, "\xc3\xa9")); }   // never half a character; a cut-off value fails
    { char two[2]; CHECK(!jstr("\"\\u00e9a\"", two, sizeof two) && two[0] == 0); }   // stops at the first char that doesn't fit
    { char t[8] = "zzzzzzz"; CHECK(!jstr("\"ab\\u00zz\"", t, sizeof t) && !strcmp(t, "ab")); }   // terminated on a bad escape
    // a usable rules file: the whole document and its "rules" value parse
    CHECK(rulesok("{\"proto\": 1, \"rules\": [{\"name\": \"x\"}]}") && !rulesok("{\"proto\": 1}") && !rulesok("{\"rules\": [{\"name\": \"x\"}"));
    CHECK(!rulesok("{\"rules\": [1, 2"));
    // slurp: a file that fills the buffer counts as unreadable (it may be truncated)
    char tmpl[] = "/tmp/loader_test.XXXXXX"; const char *td = mkdtemp(tmpl); CHECK(td != NULL);
    char f[1200], buf8[8];
    snprintf(f, sizeof f, "%s/seven", td); { int fd = open(f, O_WRONLY | O_CREAT, 0644); CHECK(write(fd, "1234567", 7) == 7); close(fd); }
    CHECK(slurp(f, buf8, sizeof buf8) == -1 && buf8[0] == 0);
    snprintf(f, sizeof f, "%s/six", td); { int fd = open(f, O_WRONLY | O_CREAT, 0644); CHECK(write(fd, "123456", 6) == 6); close(fd); }
    CHECK(slurp(f, buf8, sizeof buf8) == 6 && !strcmp(buf8, "123456"));
    // pickrules: SIMCAM_SIM_DIR with a trailing slash and no rules_dir -> <home>/restore-rules; a rules_dir
    // with '&' written by Go as \u0026; config.json's boot_signals
    char d[1200];
    snprintf(d, sizeof d, "%s/h&x/sims/U", td); CHECK(!system((snprintf(f, sizeof f, "mkdir -p '%s' '%s/../../restore-rules' '%s/../../rr&2'", d, d, d), f)));
    const char *rules = "{\"proto\": 1, \"rules\": [{\"name\": \"xcode-run\", \"libs\": []}]}";
    snprintf(f, sizeof f, "%s/../../restore-rules/27A266a.json", d); { int fd = open(f, O_WRONLY | O_CREAT, 0644); CHECK(write(fd, rules, strlen(rules)) > 0); close(fd); }
    snprintf(f, sizeof f, "%s/../../rr&2/27A266a.json", d); { int fd = open(f, O_WRONLY | O_CREAT, 0644); CHECK(write(fd, rules, strlen(rules)) > 0); close(fd); }
    const char *cfg1 = "{\"proto\": 1, \"xcode_build\": \"27A266a\", \"boot_signals\": [\"ASAN_OPTIONS=detect_leaks=0\", \"IDB_X=1\"]}";
    snprintf(f, sizeof f, "%s/config.json", d); { int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644); CHECK(write(fd, cfg1, strlen(cfg1)) > 0); close(fd); }
    snprintf(f, sizeof f, "%s///", d); setenv("SIMCAM_SIM_DIR", f, 1);
    cfgdone = 0; rulesverified = 0; xbuild[0] = 0; pickrules();
    CHECK(rulesverified && strstr(rulesfrom, "/h&x/restore-rules/27A266a.json") && !strstr(rulesfrom, "//"));
    CHECK(bootsig("ASAN_OPTIONS=detect_leaks=0") && bootsig("IDB_X=1") && !bootsig("ASAN_OPTIONS=other") && !bootsig("TSAN_OPTIONS=detect_leaks=0"));
    char cfg2[2400]; snprintf(cfg2, sizeof cfg2, "{\"proto\": 1, \"xcode_build\": \"27A266a\", \"rules_dir\": \"%s/h\\u0026x/rr\\u00262\"}", td);
    snprintf(f, sizeof f, "%s/config.json", d); { int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644); CHECK(write(fd, cfg2, strlen(cfg2)) > 0); close(fd); }
    cfgdone = 0; rulesverified = 0; xbuild[0] = 0; pickrules();
    CHECK(rulesverified && strstr(rulesfrom, "/h&x/rr&2/27A266a.json"));
    CHECK(!bootsig("ASAN_OPTIONS=detect_leaks=0"));
    // a rules file without a parsable "rules" value is not verified; the best guess skips to the built-in rules
    snprintf(f, sizeof f, "%s/../../rr&2/27A266a.json", d); { int fd = open(f, O_WRONLY | O_TRUNC, 0644); CHECK(write(fd, "{\"rules\": [", 11) == 11); close(fd); }
    cfgdone = 0; rulesverified = 0; xbuild[0] = 0; pickrules();
    CHECK(!rulesverified && strstr(rulesfrom, "built-in"));
    // rules_dir "" (nothing staged): the built-in rules, never a best guess from the flat legacy dir
    snprintf(cfg2, sizeof cfg2, "{\"proto\": 1, \"xcode_build\": \"27A266a\", \"rules_dir\": \"\"}");
    snprintf(f, sizeof f, "%s/config.json", d); { int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644); CHECK(write(fd, cfg2, strlen(cfg2)) > 0); close(fd); }
    cfgdone = 0; rulesverified = 0; xbuild[0] = 0; pickrules();
    CHECK(!rulesverified && strstr(rulesfrom, "built-in"));
    // a config.json that exists but is unusable (here: an unknown proto): the built-in rules, never the flat legacy dir
    snprintf(cfg2, sizeof cfg2, "{\"proto\": 9, \"xcode_build\": \"27A266a\"}");
    snprintf(f, sizeof f, "%s/config.json", d); { int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644); CHECK(write(fd, cfg2, strlen(cfg2)) > 0); close(fd); }
    cfgdone = 0; cfgbad = 0; rulesverified = 0; xbuild[0] = 0; pickrules();
    CHECK(cfgbad && !rulesverified && strstr(rulesfrom, "built-in"));
    // a chain entry whose basename is already mapped from another path: skipped as a second copy, never dlopened
    CHECK(tryload("/nonexistent/libSystem.B.dylib", "t") == TRYLOAD_SKIPPED_SECOND_COPY && strstr(skipped, "/nonexistent/libSystem.B.dylib [second copy; already loaded: "));
    // inject_entry_path: /usr/ and /System/ entries get the runtime root when the file is there; anything else as given
    {
        char rootd[1200], p[1024];
        snprintf(rootd, sizeof rootd, "%s/rt", td);
        snprintf(f, sizeof f, "mkdir -p '%s/usr/lib' && : > '%s/usr/lib/libX.dylib'", rootd, rootd);
        CHECK(!system(f));
        inject_entry_path("/usr/lib/libX.dylib", rootd, p, sizeof p);
        snprintf(f, sizeof f, "%s/usr/lib/libX.dylib", rootd);
        CHECK(!strcmp(p, f));
        inject_entry_path("/usr/lib/libAbsent.dylib", rootd, p, sizeof p);
        CHECK(!strcmp(p, "/usr/lib/libAbsent.dylib"));
        inject_entry_path(f, rootd, p, sizeof p);   // neither /usr/ nor /System/: as given
        CHECK(!strcmp(p, f));
        inject_entry_path("/tmp/DerivedData/A.app/Frameworks/libXCTestBundleInject.dylib", rootd, p, sizeof p);
        CHECK(!strcmp(p, "/tmp/DerivedData/A.app/Frameworks/libXCTestBundleInject.dylib"));
        inject_entry_path("/usr/lib/libX.dylib", NULL, p, sizeof p);
        CHECK(!strcmp(p, "/usr/lib/libX.dylib"));
    }
    snprintf(f, sizeof f, "rm -rf '%s'", td); CHECK(!system(f));
    unsetenv("SIMCAM_SIM_DIR");
    // the rules file: both loader rules have libs, in order (5 rules: hosted-tests, ui-test-target, ui-test-runner,
    // test-runner (read by the runner hook), xcode-run)
    if (argc > 1) {
        CHECK(slurp(argv[1], rulesbuf, sizeof rulesbuf) > 0);
        const char *rs = jget(rulesbuf, "rules"); int n = 0, found = 0;
        for (const char *r = jfirst(rs); r; r = jnext(r), n++) {
            char nm[64]; jstr(jget(r, "name"), nm, sizeof nm);
            if (!strcmp(nm, "xcode-run")) {
                const char *l = jfirst(jget(r, "libs")); char p[256];
                CHECK(jstr(jget(l, "path"), p, sizeof p) && !strcmp(p, "/usr/lib/libLogRedirect.dylib"));
                int k = 0; for (; l; l = jnext(l)) k++;
                CHECK(k == 4); found++;
            }
            if (!strcmp(nm, "ui-test-target")) { int k = 0; for (const char *l = jfirst(jget(r, "libs")); l; l = jnext(l)) k++; CHECK(k == 3); found++; }
        }
        CHECK(n == 5 && found == 2);
    }
    printf("%s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
