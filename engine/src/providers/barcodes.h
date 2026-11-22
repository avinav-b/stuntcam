// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): barcode detection's internal interface. providers/barcodes.inc includes
// it first. It defines the detectors' state (gBarcodes) and declares the functions providers/pump.inc calls.
//
// Who owns what: every field belongs to the pump queue. A Vision scan runs on its own queue (simcam.barcodes) and
// hands its result back to the pump queue, which stores it.
#ifndef SIMCAM_BARCODES_H
#define SIMCAM_BARCODES_H

struct Barcodes {
    NSArray *dicts;   // the metadata objects the pump sends (both detectors; read by providers/pump.inc)
    // the source (gScene.applied.gen) and orientation (gScene.scan.lastOrientation) dicts were found in; -1 before
    long foundGen;
    long foundOrientation;
    BOOL scanBusy;      // a Vision scan is running on the scan queue
    long scanGenDone;   // the gScene.scan.gen last scanned; -1: scan again
    Class visionRequest;   // VNDetectBarcodesRequest (nil: Vision is unavailable)
    Class visionHandler;   // VNImageRequestHandler
    dispatch_once_t visionOnce;   // BarcodeVisionLoad, on the first scan
    dispatch_queue_t scanQueue;   // simcam.barcodes: the Vision scans run here
    // Empty scans retried so far (BarcodeRetryEmptyScan), at most kBarcodeEmptyRetries each.
    struct {
        int sharedUsed;   // the scene and the sources other than qr: one count for the whole process
        int qrUsed;       // the qr source's current picture
        long qrGen;       // the gScene.scan.gen that qrUsed counts for; -1 before the first qr scan
    } emptyRetries;
};

// The detectors' state (pump queue).
static struct Barcodes gBarcodes = {
    .foundGen = -1,
    .foundOrientation = -1,
    .scanGenDone = -1,
    .emptyRetries.qrGen = -1,
};

// ---- functions other fragments call

// Each tick's detection (providers/pump.inc).
static void BarcodeDetectFixture(CVPixelBufferRef buffer);
static void BarcodeDetectScene(CVPixelBufferRef buffer);

#endif
