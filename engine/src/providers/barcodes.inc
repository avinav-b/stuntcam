// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): barcode detection for the metadata sinks: the plain
// scene's fixture (CIDetector, once) and scene v2 / the other sources (Vision, re-run when the picture changes).
static NSArray *gBarcodeDicts;   // the metadata objects the pump sends (both detectors)

// ---- the metadata objects, as AVF's metadata sink reads them ----
// The decode data of a QR code: Quagga MRCDescriptor attributes (AVF: stringValue =
// MRCDescriptorDecodePayload(MRCDescriptorCreateWithAttributes(it))). With no descriptor, the symbology alone.
static NSMutableDictionary *BarcodeQRDecodeData(CIQRCodeDescriptor *qd) {
    // MRCDescriptorAttributeSymbology = MRCSymbologyQR
    NSMutableDictionary *mrc = [@{@"BarcodeType": @"QR"} mutableCopy];
    if (qd) {
        mrc[@"BarcodeRawData"] = qd.errorCorrectedPayload;
        mrc[@"SymbolVersion"] = @(qd.symbolVersion);
        mrc[@"QRMASK"] = @(qd.maskPattern);
        // MRC wants its own ECL enum (CI's ASCII 'L'/'M'/'Q'/'H' makes MRCDescriptorDecodePayload fail -3001)
        long ecl = qd.errorCorrectionLevel;
        mrc[@"ErrorCorrectionLevel"] = @(ecl == 'L' ? 0 : ecl == 'M' ? 1 : ecl == 'Q' ? 2 : 3);
    }
    return mrc;
}
// A QR code's top-level attributes: its decode data, with CI's own ECL (top level -> CIQRCodeDescriptor)
static NSMutableDictionary *BarcodeQRTop(NSDictionary *decodeData, CIQRCodeDescriptor *qd) {
    NSMutableDictionary *top = [decodeData mutableCopy];
    if (qd) top[@"ErrorCorrectionLevel"] = @((long)qd.errorCorrectionLevel);
    return top;
}
// The four corners, given normalized with the origin bottom-left, as AVF wants them (origin top-left)
static NSArray *BarcodeCorners(CGPoint tl, CGPoint bl, CGPoint br, CGPoint tr) {
    return @[ (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(tl.x, 1 - tl.y)),
              (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(bl.x, 1 - bl.y)),
              (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(br.x, 1 - br.y)),
              (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(tr.x, 1 - tr.y)) ];
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
static void BarcodeDetectFixture(CVPixelBufferRef pb) {
    if (gBarcodeDicts) return;
    CIDetector *det = [CIDetector detectorOfType:CIDetectorTypeQRCode context:nil
                                         options:@{CIDetectorAccuracy: CIDetectorAccuracyHigh}];
    NSArray *features = [det featuresInImage:[CIImage imageWithCVPixelBuffer:pb]];
    NSMutableArray *out = [NSMutableArray array];
    for (CIQRCodeFeature *f in features) {
        // AVF metadata rects are normalized, in the landscape sensor buffer (0..1, origin top-left)
        CGRect b = f.bounds;
        double W = kSrcW, H = kSrcH;
        CGRect norm = CGRectMake(b.origin.x / W, 1 - (b.origin.y + b.size.height) / H,
                                 b.size.width / W, b.size.height / H);
        NSArray *corners = BarcodeCorners(CGPointMake(f.topLeft.x / W, f.topLeft.y / H),
                                          CGPointMake(f.bottomLeft.x / W, f.bottomLeft.y / H),
                                          CGPointMake(f.bottomRight.x / W, f.bottomRight.y / H),
                                          CGPointMake(f.topRight.x / W, f.topRight.y / H));
        CIQRCodeDescriptor *qd = (CIQRCodeDescriptor *)f.symbolDescriptor;
        NSMutableDictionary *mrc = BarcodeQRDecodeData(qd);
        [out addObject:BarcodeObject(BarcodeQRTop(mrc, qd), mrc, norm, corners)];
    }
    gBarcodeDicts = out;
    LOG(@"barcode detect: %lu codes %@", (unsigned long)out.count, out.count ? out[0][@"BarcodeDecodeData"] : @"");
}

// ---- barcodes from the rendered buffer (Vision), re-run when the picture changes: QR + EAN-13 (+ the other 1D/2D
// types Vision finds) ----
static NSString *(*pMRCDecode)(CFTypeRef);
static CFTypeRef (*pMRCCreate)(CFDictionaryRef);
static NSString *BarcodeMRCString(NSDictionary *attrs) {
    if (!pMRCCreate) {
        pMRCCreate = dlsym(RTLD_DEFAULT, "MRCDescriptorCreateWithAttributes");
        pMRCDecode = dlsym(RTLD_DEFAULT, "MRCDescriptorDecodePayload");
    }
    if (!pMRCCreate || !pMRCDecode) return nil;
    CFTypeRef d = pMRCCreate((__bridge CFDictionaryRef)attrs);
    if (!d) return nil;
    NSString *s = pMRCDecode(d);
    CFRelease(d);
    return s;
}
static NSString *BarcodeSymbol(const char *name) {   // the NSString constant exported as `name`; nil if there is none
    CFStringRef *p = dlsym(RTLD_DEFAULT, name);
    return p ? (__bridge NSString *)*p : nil;
}
static NSDictionary *BarcodeOneDAttrs(NSString *mrcSym, NSString *payload) {
    // Quagga's MRC wants 1D payload digits as byte values 0..9 (ASCII bytes decode as digit-2 mod 10, measured on
    // 26.3). Verified once per symbology by decoding it back; never pass an NSString as BarcodeRawData (hung the pump
    // in the probe).
    NSMutableData *dv = [NSMutableData data];
    for (NSUInteger i = 0; i < payload.length; i++) {
        unichar c = [payload characterAtIndex:i];
        uint8_t b = (c >= '0' && c <= '9') ? (uint8_t)(c - '0') : (uint8_t)c;
        [dv appendBytes:&b length:1];
    }
    NSDictionary *d = @{@"BarcodeType": mrcSym, @"BarcodeRawData": dv};
    static NSMutableSet *checked;
    if (!checked) checked = [NSMutableSet set];
    if (![checked containsObject:mrcSym]) {
        [checked addObject:mrcSym];
        LOG(@"MRC check %@ digits-as-values -> %@ (want %@)", mrcSym, BarcodeMRCString(d), payload);
    }
    return d;
}
static long gScanGenDone = -1;
// as BarcodeSymbol, @"" if there is none
static NSString *BarcodeVisionSymbol(const char *n) { return BarcodeSymbol(n) ?: @""; }
static CGPoint BarcodePoint(id o, NSString *k) {
    NSValue *v = SafeKVC(o, k);
    CGPoint p = CGPointZero;
    if (v) [v getValue:&p size:sizeof p];
    return p;
}
static void VisionInstall(void);   // client/vision.inc
static void BarcodeDetectScene(CVPixelBufferRef pb) {
    long gen = atomic_load(&gScanGen);
    if (gen == gScanGenDone && gBarcodeDicts) return;
    gScanGenDone = gen;
    static Class RQ, HD;
    static dispatch_once_t o1;
    dispatch_once(&o1, ^{
        dlopen("/System/Library/Frameworks/Vision.framework/Vision", RTLD_NOW);
        RQ = objc_getClass("VNDetectBarcodesRequest");
        HD = objc_getClass("VNImageRequestHandler");
        // On 17.5 the add_image callback in client/vision.inc fires before Vision's classes are registered, so when we
        // are the first to load Vision the revision pin would never be installed and detection would find 0 codes.
        // Install it here too (unless SIMCAM_OFF_VISION_REVISION_PIN turns the pin off).
        if (RQ && !getenv(ENV_OFF_VISION_REVISION_PIN)) VisionInstall();
    });
    if (!RQ || !HD) {
        LOG(@"barcode detect: Vision unavailable");
        gBarcodeDicts = @[];
        return;
    }
    id request = [[RQ alloc] init];
    NSError *e = nil;
    id handler = ((id (*)(id, SEL, CVPixelBufferRef, id))objc_msgSend)(
        [HD alloc], sel_registerName("initWithCVPixelBuffer:options:"), pb, @{});
    ((BOOL (*)(id, SEL, id, NSError **))objc_msgSend)(handler, sel_registerName("performRequests:error:"), @[request],
                                                      &e);
    NSArray *results = SafeKVC(request, @"results");
    NSString *symQR = BarcodeVisionSymbol("VNBarcodeSymbologyQR"),
             *symEAN13 = BarcodeVisionSymbol("VNBarcodeSymbologyEAN13"),
             *symEAN8 = BarcodeVisionSymbol("VNBarcodeSymbologyEAN8"),
             *symCode128 = BarcodeVisionSymbol("VNBarcodeSymbologyCode128");
    NSMutableArray *out = [NSMutableArray array], *names = [NSMutableArray array];
    for (id o in results) {
        NSString *sym = SafeKVC(o, @"symbology"), *payload = SafeKVC(o, @"payloadStringValue");
        id desc = SafeKVC(o, @"barcodeDescriptor");
        [names addObject:[NSString stringWithFormat:@"%@=%@", sym, payload]];
        CGRect b = CGRectZero;
        [(NSValue *)SafeKVC(o, @"boundingBox") getValue:&b size:sizeof b];   // normalized, origin bottom-left
        CGRect norm = CGRectMake(b.origin.x, 1 - b.origin.y - b.size.height, b.size.width, b.size.height);
        CGPoint tl = BarcodePoint(o, @"topLeft"), bl = BarcodePoint(o, @"bottomLeft"),
                br = BarcodePoint(o, @"bottomRight"), tr = BarcodePoint(o, @"topRight");
        NSArray *corners = BarcodeCorners(tl, bl, br, tr);
        NSMutableDictionary *mrc = nil, *top = nil;
        if ([sym isEqualToString:symQR] && [desc isKindOfClass:[CIQRCodeDescriptor class]]) {
            CIQRCodeDescriptor *qd = (CIQRCodeDescriptor *)desc;
            mrc = BarcodeQRDecodeData(qd);
            top = BarcodeQRTop(mrc, qd);
        } else if (payload) {
            NSString *mrcSym = [sym isEqualToString:symEAN13]    ? BarcodeSymbol("MRCSymbologyEAN13")
                               : [sym isEqualToString:symEAN8]   ? BarcodeSymbol("MRCSymbologyEAN8")
                               : [sym isEqualToString:symCode128] ? BarcodeSymbol("MRCSymbologyCode128")
                                                                  : nil;
            if (!mrcSym) continue;
            mrc = [BarcodeOneDAttrs(mrcSym, payload) mutableCopy];
            top = [mrc mutableCopy];
        } else {
            continue;
        }
        [out addObject:BarcodeObject(top, mrc, norm, corners)];
    }
    gBarcodeDicts = out;
    static int emptyRuns;
    if (!out.count && ++emptyRuns <= 5) gScanGenDone = -1;   // the scene always has codes; retry before caching none
    LOG(@"barcode detect (gen %ld): %lu codes %@ err=%@", gen, (unsigned long)out.count, names, e);
}
