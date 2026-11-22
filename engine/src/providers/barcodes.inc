// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): barcode detection for the metadata sinks: the plain
// scene's fixture (CIDetector, once) and scene v2 / the other sources (Vision, re-run when the picture changes).
//
// Provides: BarcodeDetectFixture, BarcodeDetectScene (providers/pump.inc)
// Owns:     gBarcodes, the detectors' state, defined in providers/barcodes.h (pump queue; providers/pump.inc reads
//           the metadata objects)
// Uses:     fig/session_state.inc, providers/scene.inc, client/vision.inc
#include "barcodes.h"
#include "../fig/session.h"
#include "pump.h"
#include "scene.h"
#include "../client/vision.h"

// ---- the metadata objects, as AVF's metadata sink reads them ----
// The decode data of a QR code: Quagga MRCDescriptor attributes (AVF: stringValue =
// MRCDescriptorDecodePayload(MRCDescriptorCreateWithAttributes(it))). With no descriptor, the symbology alone.
// MRC's error correction level for CI's ASCII one: L 0, M 1, Q 2, anything else (H) 3. An int, so the attribute boxes
// as a 32-bit number, as it always has.
static int BarcodeMRCErrorCorrection(long ciLevel) {
    if (ciLevel == 'L') {
        return 0;
    }
    if (ciLevel == 'M') {
        return 1;
    }
    if (ciLevel == 'Q') {
        return 2;
    }
    return 3;
}

static NSMutableDictionary *BarcodeQRDecodeData(CIQRCodeDescriptor *descriptor) {
    // MRCDescriptorAttributeSymbology = MRCSymbologyQR
    NSMutableDictionary *attributes = [@{@"BarcodeType": @"QR"} mutableCopy];
    if (descriptor) {
        attributes[@"BarcodeRawData"] = descriptor.errorCorrectedPayload;
        attributes[@"SymbolVersion"] = @(descriptor.symbolVersion);
        attributes[@"QRMASK"] = @(descriptor.maskPattern);
        // MRC wants its own ECL enum (CI's ASCII 'L'/'M'/'Q'/'H' makes MRCDescriptorDecodePayload fail -3001)
        attributes[@"ErrorCorrectionLevel"] = @(BarcodeMRCErrorCorrection(descriptor.errorCorrectionLevel));
    }
    return attributes;
}

// A QR code's top-level attributes: its decode data, with CI's own ECL (top level -> CIQRCodeDescriptor)
static NSMutableDictionary *BarcodeQRTop(NSDictionary *decodeData, CIQRCodeDescriptor *descriptor) {
    NSMutableDictionary *top = [decodeData mutableCopy];
    if (descriptor) {
        top[@"ErrorCorrectionLevel"] = @((long)descriptor.errorCorrectionLevel);
    }
    return top;
}

// The four corners, given normalized with the origin bottom-left, as AVF wants them (origin top-left)
static NSArray *BarcodeCorners(CGPoint topLeft, CGPoint bottomLeft, CGPoint bottomRight, CGPoint topRight) {
    return @[
        (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(topLeft.x, 1 - topLeft.y)),
        (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(bottomLeft.x, 1 - bottomLeft.y)),
        (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(bottomRight.x, 1 - bottomRight.y)),
        (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(topRight.x, 1 - topRight.y)),
    ];
}

// One metadata object: the top-level attributes plus the decode data, the rect (normalized, origin top-left) and
// the corners
static NSMutableDictionary *BarcodeObject(NSMutableDictionary *top, NSDictionary *decodeData, CGRect rect,
                                          NSArray *corners) {
    [top addEntriesFromDictionary:@{@"BarcodeDecodeData": decodeData,
                                    @"BarcodeRect": (__bridge_transfer id)CGRectCreateDictionaryRepresentation(rect),
                                    @"BarcodeCorners": corners}];
    return top;
}

// ---- barcodes (static fixture: detect once with CIDetector) --------------------
static void BarcodeDetectFixture(CVPixelBufferRef buffer) {
    if (gBarcodes.dicts) {
        return;
    }
    CIDetector *detector = [CIDetector detectorOfType:CIDetectorTypeQRCode context:nil
                                         options:@{CIDetectorAccuracy: CIDetectorAccuracyHigh}];
    NSArray *features = [detector featuresInImage:[CIImage imageWithCVPixelBuffer:buffer]];
    NSMutableArray *out = [NSMutableArray array];

    for (CIQRCodeFeature *feature in features) {
        // AVF metadata rects are normalized, in the landscape sensor buffer (0..1, origin top-left)
        CGRect bounds = feature.bounds;
        double width = kSrcW;
        double height = kSrcH;
        CGRect normalized = CGRectMake(bounds.origin.x / width, 1 - (bounds.origin.y + bounds.size.height) / height,
                                       bounds.size.width / width, bounds.size.height / height);
        NSArray *corners = BarcodeCorners(CGPointMake(feature.topLeft.x / width, feature.topLeft.y / height),
                                          CGPointMake(feature.bottomLeft.x / width, feature.bottomLeft.y / height),
                                          CGPointMake(feature.bottomRight.x / width, feature.bottomRight.y / height),
                                          CGPointMake(feature.topRight.x / width, feature.topRight.y / height));
        CIQRCodeDescriptor *descriptor = (CIQRCodeDescriptor *)feature.symbolDescriptor;
        NSMutableDictionary *decodeData = BarcodeQRDecodeData(descriptor);
        [out addObject:BarcodeObject(BarcodeQRTop(decodeData, descriptor), decodeData, normalized, corners)];
    }

    gBarcodes.dicts = out;
    LOG(@"barcode detect: %lu codes %@", (unsigned long)out.count, out.count ? out[0][@"BarcodeDecodeData"] : @"");
}

// ---- barcodes from the rendered buffer (Vision), re-run when the picture changes: QR + EAN-13 (+ the other 1D/2D
// types Vision finds) ----
static NSString *(*pMRCDescriptorDecodePayload)(CFTypeRef);
static CFTypeRef (*pMRCDescriptorCreateWithAttributes)(CFDictionaryRef);

static NSString *BarcodeMRCString(NSDictionary *attrs) {
    if (!pMRCDescriptorCreateWithAttributes) {
        pMRCDescriptorCreateWithAttributes = dlsym(RTLD_DEFAULT, "MRCDescriptorCreateWithAttributes");
        pMRCDescriptorDecodePayload = dlsym(RTLD_DEFAULT, "MRCDescriptorDecodePayload");
    }
    if (!pMRCDescriptorCreateWithAttributes || !pMRCDescriptorDecodePayload) {
        return nil;
    }
    CFTypeRef descriptor = pMRCDescriptorCreateWithAttributes((__bridge CFDictionaryRef)attrs);
    if (!descriptor) {
        return nil;
    }
    NSString *payload = pMRCDescriptorDecodePayload(descriptor);
    CFRelease(descriptor);
    return payload;
}

static NSString *BarcodeSymbol(const char *name) {   // the NSString constant exported as `name`; nil if there is none
    CFStringRef *symbol = dlsym(RTLD_DEFAULT, name);
    return symbol ? (__bridge NSString *)*symbol : nil;
}

static NSDictionary *BarcodeOneDAttrs(NSString *mrcSym, NSString *payload) {
    // Quagga's MRC wants 1D payload digits as byte values 0..9 (ASCII bytes decode as digit-2 mod 10, measured on
    // 26.3). Verified once per symbology by decoding it back; never pass an NSString as BarcodeRawData (hung the pump
    // in the probe).
    NSMutableData *digitValues = [NSMutableData data];
    for (NSUInteger i = 0; i < payload.length; i++) {
        unichar character = [payload characterAtIndex:i];
        uint8_t byte = (character >= '0' && character <= '9') ? (uint8_t)(character - '0') : (uint8_t)character;
        [digitValues appendBytes:&byte length:1];
    }
    NSDictionary *attributes = @{@"BarcodeType": mrcSym, @"BarcodeRawData": digitValues};
    static NSMutableSet *checked;
    if (!checked) {
        checked = [NSMutableSet set];
    }
    if (![checked containsObject:mrcSym]) {
        [checked addObject:mrcSym];
        LOG(@"MRC check %@ digits-as-values -> %@ (want %@)", mrcSym, BarcodeMRCString(attributes), payload);
    }
    return attributes;
}

// the MRC symbology of a Vision 1D symbology the metadata objects carry (EAN-13, EAN-8, Code 128); nil for the others
static NSString *BarcodeMRCSymbology(NSString *visionSymbology, NSString *symEAN13, NSString *symEAN8,
                                     NSString *symCode128) {
    if ([visionSymbology isEqualToString:symEAN13]) {
        return BarcodeSymbol("MRCSymbologyEAN13");
    }
    if ([visionSymbology isEqualToString:symEAN8]) {
        return BarcodeSymbol("MRCSymbologyEAN8");
    }
    if ([visionSymbology isEqualToString:symCode128]) {
        return BarcodeSymbol("MRCSymbologyCode128");
    }
    return nil;
}

// as BarcodeSymbol, @"" if there is none
static NSString *BarcodeVisionSymbol(const char *name) { return BarcodeSymbol(name) ?: @""; }

static CGPoint BarcodePoint(id object, NSString *key) {
    NSValue *value = SafeKVC(object, key);
    CGPoint point = CGPointZero;
    if (value) {
        [value getValue:&point size:sizeof point];
    }
    return point;
}

// The codes Vision finds in pb, as metadata objects (any queue)
static NSArray *BarcodeVisionScan(CVPixelBufferRef buffer, NSMutableArray *names, NSError **error) {
    Class requestClass = gBarcodes.visionRequest;
    Class handlerClass = gBarcodes.visionHandler;
    id request = [[requestClass alloc] init];
    id handler = ((id (*)(id, SEL, CVPixelBufferRef, id))objc_msgSend)(
        [handlerClass alloc], sel_registerName("initWithCVPixelBuffer:options:"), buffer, @{});
    SEL perform = sel_registerName("performRequests:error:");
    ((BOOL (*)(id, SEL, id, NSError **))objc_msgSend)(handler, perform, @[request], error);
    NSArray *results = SafeKVC(request, @"results");
    NSString *symQR = BarcodeVisionSymbol("VNBarcodeSymbologyQR");
    NSString *symEAN13 = BarcodeVisionSymbol("VNBarcodeSymbologyEAN13");
    NSString *symEAN8 = BarcodeVisionSymbol("VNBarcodeSymbologyEAN8");
    NSString *symCode128 = BarcodeVisionSymbol("VNBarcodeSymbologyCode128");
    NSMutableArray *out = [NSMutableArray array];

    for (id observation in results) {
        NSString *symbology = SafeKVC(observation, @"symbology");
        NSString *payload = SafeKVC(observation, @"payloadStringValue");
        id descriptor = SafeKVC(observation, @"barcodeDescriptor");
        [names addObject:[NSString stringWithFormat:@"%@=%@", symbology, payload]];
        CGRect box = CGRectZero;
        // normalized, origin bottom-left
        [(NSValue *)SafeKVC(observation, @"boundingBox") getValue:&box size:sizeof box];
        CGRect normalized =
            CGRectMake(box.origin.x, 1 - box.origin.y - box.size.height, box.size.width, box.size.height);
        CGPoint topLeft = BarcodePoint(observation, @"topLeft");
        CGPoint bottomLeft = BarcodePoint(observation, @"bottomLeft");
        CGPoint bottomRight = BarcodePoint(observation, @"bottomRight");
        CGPoint topRight = BarcodePoint(observation, @"topRight");
        NSArray *corners = BarcodeCorners(topLeft, bottomLeft, bottomRight, topRight);
        NSMutableDictionary *decodeData = nil;
        NSMutableDictionary *top = nil;
        if ([symbology isEqualToString:symQR] && [descriptor isKindOfClass:[CIQRCodeDescriptor class]]) {
            CIQRCodeDescriptor *qrDescriptor = (CIQRCodeDescriptor *)descriptor;
            decodeData = BarcodeQRDecodeData(qrDescriptor);
            top = BarcodeQRTop(decodeData, qrDescriptor);
        } else if (payload) {
            NSString *mrcSym = BarcodeMRCSymbology(symbology, symEAN13, symEAN8, symCode128);
            if (!mrcSym) {
                continue;
            }
            decodeData = [BarcodeOneDAttrs(mrcSym, payload) mutableCopy];
            top = [decodeData mutableCopy];
        } else {
            continue;
        }
        [out addObject:BarcodeObject(top, decodeData, normalized, corners)];
    }

    return out;
}

// the most times an empty scan is retried (BarcodeRetryEmptyScan)
#define kBarcodeEmptyRetries 5

// YES if the codes were found in another source or orientation than the current one: their payloads or positions are
// wrong now
static BOOL BarcodeStale(long srcGen, long orient) {
    return srcGen != gScene.applied.gen || orient != gScene.scan.lastOrientation;
}

// After a scan of picture gen that found codeCount codes: YES to scan it again. The scene always has codes, and so
// does a qr source, so an empty scan is retried: up to kBarcodeEmptyRetries times for each qr picture, and that many
// times in all for the scene and the other sources.
static BOOL BarcodeRetryEmptyScan(long gen, NSUInteger codeCount) {
    BOOL isQR = gScene.applied.kind == kSourceQR;
    if (isQR && gen != gBarcodes.emptyRetries.qrGen) {   // a new qr picture gets its own retries
        gBarcodes.emptyRetries.qrGen = gen;
        gBarcodes.emptyRetries.qrUsed = 0;
    }
    if (codeCount > 0) {
        return NO;
    }
    int used = isQR ? ++gBarcodes.emptyRetries.qrUsed : ++gBarcodes.emptyRetries.sharedUsed;
    return used <= kBarcodeEmptyRetries;
}

// Once, on the first scan: loads Vision, looks up its two classes and makes the scan queue.
static void BarcodeVisionLoad(void) {
    dlopen("/System/Library/Frameworks/Vision.framework/Vision", RTLD_NOW);
    gBarcodes.visionRequest = objc_getClass("VNDetectBarcodesRequest");
    gBarcodes.visionHandler = objc_getClass("VNImageRequestHandler");
    // On 17.5 the add_image callback in client/vision.inc fires before Vision's classes are registered, so when we are
    // the first to load Vision the revision pin would never be installed and detection would find 0 codes. Install it
    // here too (unless SIMCAM_OFF_VISION_REVISION_PIN turns the pin off).
    if (gBarcodes.visionRequest && VisionRevisionPinOn()) {
        VisionInstall();
    }
    // drain Vision's autoreleased objects after each scan
    dispatch_queue_attr_t serial = dispatch_queue_attr_make_with_autorelease_frequency(
        DISPATCH_QUEUE_SERIAL, DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM);
    gBarcodes.scanQueue = dispatch_queue_create("simcam.barcodes", serial);
}

// Pump queue. Vision takes 60-140 ms on a 1080p frame in the simulator. Run inline, it stalled the preview twice a
// second on a video source, so it runs on its own queue and the pump keeps sending the codes it has until it's done.
// The first metadata samples go out with no codes, until the first scan is back.
static void BarcodeDetectScene(CVPixelBufferRef buffer) {
    long gen = atomic_load(&gScene.scan.gen);
    if (gBarcodes.dicts && BarcodeStale(gBarcodes.foundGen, gBarcodes.foundOrientation)) {
        gBarcodes.dicts = @[];
    }
    if (gBarcodes.scanBusy || (gen == gBarcodes.scanGenDone && gBarcodes.dicts)) {
        return;
    }

    // The qr source's code is still being made (white frames): no codes yet, and no retry spent; its arrival bumps
    // gScene.scan.gen.
    if (gScene.applied.kind == kSourceQR && !gScene.applied.qrImage) {
        gBarcodes.dicts = @[];
        return;
    }

    gBarcodes.scanGenDone = gen;
    dispatch_once(&gBarcodes.visionOnce, ^{
        BarcodeVisionLoad();
    });

    if (!gBarcodes.visionRequest || !gBarcodes.visionHandler) {
        LOG(@"barcode detect: Vision unavailable");
        gBarcodes.dicts = @[];
        return;
    }

    gBarcodes.scanBusy = YES;
    long srcGen = gScene.applied.gen;
    long orientation = gScene.scan.lastOrientation;
    dispatch_queue_t pumpQ = gSessionLayer.pumpQueue;
    CVPixelBufferRetain(buffer);
    dispatch_async(gBarcodes.scanQueue, ^{
        NSMutableArray *names = [NSMutableArray array];
        NSError *error = nil;
        NSArray *out = BarcodeVisionScan(buffer, names, &error);
        CVPixelBufferRelease(buffer);
        dispatch_async(pumpQ, ^{
            gBarcodes.scanBusy = NO;
            if (BarcodeStale(srcGen, orientation)) {   // the source or orientation changed while Vision ran: rescan
                gBarcodes.scanGenDone = -1;
                return;
            }
            gBarcodes.dicts = out;
            gBarcodes.foundGen = srcGen;
            gBarcodes.foundOrientation = orientation;
            if (BarcodeRetryEmptyScan(gen, out.count)) {
                gBarcodes.scanGenDone = -1;
            }
            LOG(@"barcode detect (gen %ld): %lu codes %@ err=%@", gen, (unsigned long)out.count, names, error);
        });
    });
}
