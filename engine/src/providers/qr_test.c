// SPDX-License-Identifier: Apache-2.0
// Host tests for providers/qr.c (the `qr:` source's spec decoding and code size). Run with qr_test.sh.
#include <stdio.h>
#include <string.h>

#include "qr.c"

static int failures, checks;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static void decodes(const char *in, const char *want, size_t wantLen) {
    unsigned char out[512];
    size_t n = QRSpecDecode(in, strlen(in), out);
    CHECK(n == wantLen && memcmp(out, want, wantLen) == 0, "decode(%s): got %zu bytes", in, n);
}

int main(void) {
    // plain text is itself
    decodes("https://example.com/pay?id=42", "https://example.com/pay?id=42", 29);
    decodes("", "", 0);
    // what the CLI escapes: '%', space, control bytes, DEL and non-ASCII
    decodes("50%25%20off", "50% off", 7);
    decodes("a%0Ab%0d%0A", "a\nb\r\n", 5);
    decodes("%E6%97%A5%e6%9c%ac", "\xe6\x97\xa5\xe6\x9c\xac", 6);
    decodes("%00x%FF", "\0x\xff", 3);   // binary-safe: NUL and an invalid UTF-8 byte
    decodes("%7F", "\x7f", 1);
    // a '%' without two hex digits stands for itself (hand-written specs)
    decodes("50%", "50%", 3);
    decodes("50%2", "50%2", 4);
    decodes("%zz%2", "%zz%2", 5);
    decodes("%%41", "%A", 2);
    decodes("%4", "%4", 2);
    decodes("%+1", "%+1", 3);
    // the Wi-Fi example, escaped as the CLI writes it
    decodes("WIFI:S:my%20net;T:WPA;P:p@ss:w;rd\\;\"x\"%2520;;", "WIFI:S:my net;T:WPA;P:p@ss:w;rd\\;\"x\"%20;;", 41);
    // the decoded text never outgrows the input
    {
        const char *in = "%41%42%43";
        unsigned char out[9];
        CHECK(QRSpecDecode(in, strlen(in), out) == 3, "shrinks");
    }

    // code size: whole pixels per module, within 75% of the shorter side, the same in both canvas orientations
    for (int w = 21; w <= 185; w++) {
        int land = QRModuleScale(w, 1920, 1080), port = QRModuleScale(w, 1080, 1920);
        CHECK(land == port, "w=%d: landscape %d, portrait %d", w, land, port);
        CHECK(land >= 1 && land * w <= 810 && (land + 1) * w > 810, "w=%d: scale %d", w, land);
    }
    CHECK(QRModuleScale(23, 1920, 1080) == 35, "version 1 with CI's border: %d", QRModuleScale(23, 1920, 1080));
    CHECK(QRModuleScale(179, 1920, 1080) == 4, "version 40 with CI's border: %d", QRModuleScale(179, 1920, 1080));
    CHECK(QRModuleScale(2000, 1920, 1080) == 1, "never below 1");
    CHECK(QRModuleScale(0, 1920, 1080) == 1, "an empty image");

    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
