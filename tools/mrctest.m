// SPDX-License-Identifier: Apache-2.0
// mrctest <image> — runs in a simulator (tools/build.sh mrctest). It decodes the QR code in <image> with CoreImage,
// then builds a descriptor from the code's error-corrected payload with Quagga's private MRCDescriptor functions, for
// three sets of attributes. It prints one "CI: msg=… ver=…" line for what CoreImage decoded, then for each set of
// attributes the descriptor, any error, what it decodes to and the attributes it reports.
// Exits 2 on a usage error, and 1 with a message on stderr if Quagga, the image or a QR code in it can't be found.
#import <Foundation/Foundation.h>
#import <CoreImage/CoreImage.h>
#import <ImageIO/ImageIO.h>
#import <dlfcn.h>

static const char *const kQuaggaPath = "/System/Library/PrivateFrameworks/Quagga.framework/Quagga";

static const char *describe(CFTypeRef value, const char *none) {
    return value ? [[(__bridge id)value description] UTF8String] : none;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: mrctest <image>\n");
            return 2;
        }

        void *quagga = dlopen(kQuaggaPath, RTLD_NOW);
        if (!quagga) {
            fprintf(stderr, "mrctest: cannot load %s: %s\n", kQuaggaPath, dlerror());
            return 1;
        }
        CFTypeRef (*create)(CFDictionaryRef, CFErrorRef *) = dlsym(quagga, "MRCDescriptorCreateWithAttributes");
        CFTypeRef (*decode)(CFTypeRef, CFErrorRef *) = dlsym(quagga, "MRCDescriptorDecodePayload");
        CFDictionaryRef (*copyAttributes)(CFTypeRef) = dlsym(quagga, "MRCDescriptorCopyAttributes");
        if (!create || !decode || !copyAttributes) {
            fprintf(stderr, "mrctest: Quagga lacks one of MRCDescriptorCreateWithAttributes, MRCDescriptorDecodePayload"
                            " and MRCDescriptorCopyAttributes\n");
            return 1;
        }

        CIImage *image = [CIImage imageWithContentsOfURL:[NSURL fileURLWithPath:@(argv[1])]];
        if (!image) {
            fprintf(stderr, "mrctest: cannot read an image from %s\n", argv[1]);
            return 1;
        }
        CIDetector *detector = [CIDetector detectorOfType:CIDetectorTypeQRCode context:nil options:nil];
        CIQRCodeFeature *feature = (CIQRCodeFeature *)[detector featuresInImage:image].firstObject;
        CIQRCodeDescriptor *code = (CIQRCodeDescriptor *)feature.symbolDescriptor;
        if (!code.errorCorrectedPayload) {
            fprintf(stderr, "mrctest: no QR code found in %s\n", argv[1]);
            return 1;
        }
        printf("CI: msg=%s ver=%ld mask=%d ecl=%ld payload=%lu\n", feature.messageString.UTF8String,
               (long)code.symbolVersion, code.maskPattern, (long)code.errorCorrectionLevel,
               (unsigned long)code.errorCorrectedPayload.length);

        // all of CoreImage's attributes; without the mask and error correction level; with level L instead of the
        // decoded one
        NSData *payload = code.errorCorrectedPayload;
        NSArray *attributeSets = @[
            @{
                @"BarcodeType" : @"QR",
                @"BarcodeRawData" : payload,
                @"SymbolVersion" : @(code.symbolVersion),
                @"QRMASK" : @(code.maskPattern),
                @"ErrorCorrectionLevel" : @(code.errorCorrectionLevel)
            },
            @{@"BarcodeType" : @"QR", @"BarcodeRawData" : payload, @"SymbolVersion" : @(code.symbolVersion)},
            @{
                @"BarcodeType" : @"QR",
                @"BarcodeRawData" : payload,
                @"SymbolVersion" : @(code.symbolVersion),
                @"QRMASK" : @(code.maskPattern),
                @"ErrorCorrectionLevel" : @"L"
            },
        ];
        for (NSDictionary *attributes in attributeSets) {
            CFErrorRef createError = NULL;
            CFTypeRef descriptor = create((__bridge CFDictionaryRef)attributes, &createError);
            CFErrorRef decodeError = NULL;
            CFTypeRef decoded = descriptor ? decode(descriptor, &decodeError) : NULL;
            const char *reported = descriptor ? describe(copyAttributes(descriptor), "(null)") : "";
            printf("variant: desc=%p err=%s -> decode=%s err=%s attrs=%s\n", descriptor, describe(createError, "-"),
                   describe(decoded, "nil"), describe(decodeError, "-"), reported);
        }
    }
}
