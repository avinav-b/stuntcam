// SPDX-License-Identifier: Apache-2.0
// audiosrc: (sim CLI via simctl spawn) print +[AVCaptureFigAudioDevice _newFigCaptureSources] and each source's AttributesDictionary.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <dlfcn.h>
typedef struct { uint32_t version; size_t sz; void *eq, *inv, *fin, *desc; OSStatus (*copyProperty)(const void *, CFStringRef, CFAllocatorRef, void *); } BC;
typedef struct { const void *r; const BC *base; const void *derived; } VT;
int main(int argc, char **argv) { @autoreleasepool {
    dlopen("/System/Library/PrivateFrameworks/AVFCapture.framework/AVFCapture", RTLD_NOW);
    const VT *(*getVT)(const void *) = dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable");
    id srcs = ((id (*)(id, SEL))objc_msgSend)(objc_getClass("AVCaptureFigAudioDevice"), sel_registerName("_newFigCaptureSources"));
    printf("audio sources: %lu\n", (unsigned long)[srcs count]);
    for (id s in srcs) {
        const VT *vt = getVT((__bridge void *)s);
        for (NSString *k in @[@"AttributesDictionary", @"Formats", @"UniqueID"]) { CFTypeRef v = NULL; OSStatus e = vt->base->copyProperty((__bridge void *)s, (__bridge CFStringRef)k, NULL, &v); printf("%s -> %d %s\n", k.UTF8String, (int)e, v ? [[(__bridge id)v description] UTF8String] : "(null)"); }
        Dl_info di; dladdr(vt->base->copyProperty, &di); printf("copyProperty impl %s\n", di.dli_sname);
    }
    printf("default audio device: %s\n", [[((id (*)(id, SEL, id))objc_msgSend)(objc_getClass("AVCaptureDevice"), sel_registerName("defaultDeviceWithMediaType:"), @"soun") description] UTF8String]);
}}
