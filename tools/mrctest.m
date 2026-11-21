// SPDX-License-Identifier: Apache-2.0
// mrctest <image> — runs in a simulator (tools/build.sh mrctest): decodes the QR code in <image> with CoreImage, then
// builds a descriptor from its error-corrected payload with Quagga's private MRCDescriptor functions (three sets of
// attributes). Prints one "CI: msg=… ver=…" line for what CoreImage decoded, then for each set of attributes the
// descriptor, the attributes it reports, what it decodes to and any errors.
#import <Foundation/Foundation.h>
#import <CoreImage/CoreImage.h>
#import <ImageIO/ImageIO.h>
#import <dlfcn.h>
int main(int argc, char **argv) { @autoreleasepool {
  void *q = dlopen("/System/Library/PrivateFrameworks/Quagga.framework/Quagga", RTLD_NOW);
  CFTypeRef (*create)(CFDictionaryRef, CFErrorRef *) = dlsym(q, "MRCDescriptorCreateWithAttributes");
  CFTypeRef (*decode)(CFTypeRef, CFErrorRef *) = dlsym(q, "MRCDescriptorDecodePayload");
  CFDictionaryRef (*copyAttrs)(CFTypeRef) = dlsym(q, "MRCDescriptorCopyAttributes");
  CIImage *img = [CIImage imageWithContentsOfURL:[NSURL fileURLWithPath:@(argv[1])]];
  CIDetector *det = [CIDetector detectorOfType:CIDetectorTypeQRCode context:nil options:nil];
  CIQRCodeFeature *f = (CIQRCodeFeature *)[det featuresInImage:img].firstObject;
  CIQRCodeDescriptor *qd = (CIQRCodeDescriptor *)f.symbolDescriptor;
  printf("CI: msg=%s ver=%ld mask=%d ecl=%ld payload=%lu\n", f.messageString.UTF8String, (long)qd.symbolVersion, qd.maskPattern, (long)qd.errorCorrectionLevel, (unsigned long)qd.errorCorrectedPayload.length);
  NSArray *variants = @[
    @{@"BarcodeType": @"QR", @"BarcodeRawData": qd.errorCorrectedPayload, @"SymbolVersion": @(qd.symbolVersion), @"QRMASK": @(qd.maskPattern), @"ErrorCorrectionLevel": @(qd.errorCorrectionLevel)},
    @{@"BarcodeType": @"QR", @"BarcodeRawData": qd.errorCorrectedPayload, @"SymbolVersion": @(qd.symbolVersion)},
    @{@"BarcodeType": @"QR", @"BarcodeRawData": qd.errorCorrectedPayload, @"SymbolVersion": @(qd.symbolVersion), @"QRMASK": @(qd.maskPattern), @"ErrorCorrectionLevel": @"L"},
  ];
  for (NSDictionary *v in variants) {
    CFErrorRef e = NULL; CFTypeRef d = create((__bridge CFDictionaryRef)v, &e);
    CFErrorRef e2 = NULL; CFTypeRef s = d ? decode(d, &e2) : NULL;
    printf("variant: desc=%p err=%s -> decode=%s err=%s attrs=%s\n", d, e ? [[(__bridge id)e description] UTF8String] : "-", s ? [[(__bridge id)s description] UTF8String] : "nil", e2 ? [[(__bridge id)e2 description] UTF8String] : "-", d ? [[(__bridge id)copyAttrs(d) description] UTF8String] : "");
  }
}}
