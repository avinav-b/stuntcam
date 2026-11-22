// SPDX-License-Identifier: Apache-2.0
// audiosrc — a simulator command-line tool (run it with simctl spawn). It prints the audio capture sources that
// +[AVCaptureFigAudioDevice _newFigCaptureSources] returns, with each source's AttributesDictionary, Formats and
// UniqueID properties, and then the default audio device. Exits 1 with a message on stderr if it can't read a
// source's properties (no CMBaseObjectGetVTable, or a source without copyProperty).
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <dlfcn.h>

// The leading part of a CoreMedia base object's class (CMBaseClass): a version, the derived storage size, then function
// pointers. copyProperty is the one this tool calls; the fields before it only keep its offset.
typedef struct {
    uint32_t version;
    size_t derivedStorageSize;
    void *isEqual;
    void *invalidate;
    void *finalize;
    void *copyDebugDescription;
    OSStatus (*copyProperty)(const void *object, CFStringRef key, CFAllocatorRef allocator, void *valueOut);
} BaseClass;

// A CoreMedia base object's vtable (CMBaseVTable): a reserved pointer, the base class, the derived class.
typedef struct {
    const void *reserved;
    const BaseClass *base;
    const void *derived;
} BaseVTable;

static const char *describe(CFTypeRef value) {
    return value ? [[(__bridge id)value description] UTF8String] : "(null)";
}

int main(int argc, char **argv) {
    @autoreleasepool {
        dlopen("/System/Library/PrivateFrameworks/AVFCapture.framework/AVFCapture", RTLD_NOW);
        const BaseVTable *(*getVTable)(const void *) = dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable");
        id sources = ((id(*)(id, SEL))objc_msgSend)(objc_getClass("AVCaptureFigAudioDevice"),
                                                     sel_registerName("_newFigCaptureSources"));
        printf("audio sources: %lu\n", (unsigned long)[sources count]);
        for (id source in sources) {
            if (!getVTable) {
                fprintf(stderr, "audiosrc: CMBaseObjectGetVTable not found\n");
                return 1;
            }
            const BaseVTable *vtable = getVTable((__bridge void *)source);
            if (!vtable || !vtable->base || !vtable->base->copyProperty) {
                fprintf(stderr, "audiosrc: a capture source has no CMBaseClass copyProperty\n");
                return 1;
            }
            for (NSString *key in @[ @"AttributesDictionary", @"Formats", @"UniqueID" ]) {
                CFTypeRef value = NULL;
                OSStatus status = vtable->base->copyProperty((__bridge void *)source, (__bridge CFStringRef)key, NULL,
                                                             &value);
                printf("%s -> %d %s\n", key.UTF8String, (int)status, describe(value));
            }
            Dl_info info;
            dladdr(vtable->base->copyProperty, &info);
            printf("copyProperty impl %s\n", info.dli_sname);
        }
        id device = ((id(*)(id, SEL, id))objc_msgSend)(objc_getClass("AVCaptureDevice"),
                                                       sel_registerName("defaultDeviceWithMediaType:"), @"soun");
        printf("default audio device: %s\n", [[device description] UTF8String]);
    }
}
