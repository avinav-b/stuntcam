// SPDX-License-Identifier: Apache-2.0
// dumpkeys <names file> — runs in a simulator: `xcrun simctl spawn <udid> tools/bin/dumpkeys <file>`. Build it with
// tools/build.sh dumpkeys.
// It loads the capture frameworks, then prints "<name> = <value>" for each CFString constant named in the file, which
// has one name per line. The value is "(nosym)" when there is no such symbol, and "(nonstring)" when it isn't a
// CFString or its address can't be resolved.
// Exits 2 on a usage error and 1 if the file can't be read.
#import <Foundation/Foundation.h>
#import <dlfcn.h>

static const char *kLibs[] = {
    "/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture",
    "/System/Library/PrivateFrameworks/AVFCapture.framework/AVFCapture",
    "/System/Library/Frameworks/CoreMedia.framework/CoreMedia",
    "/System/Library/PrivateFrameworks/CMCaptureCore.framework/CMCaptureCore",
    "/System/Library/PrivateFrameworks/Quagga.framework/Quagga",
    NULL,
};

// The value of the CFString constant named `name`, or "(nosym)" or "(nonstring)".
static const char *valueOf(NSString *name) {
    CFStringRef *symbol = dlsym(RTLD_DEFAULT, name.UTF8String);
    if (!symbol) return "(nosym)";
    Dl_info info;
    if (dladdr(*symbol, &info) && CFGetTypeID(*symbol) == CFStringGetTypeID()) {
        return [(__bridge NSString *)*symbol UTF8String];
    }
    return "(nonstring)";
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: dumpkeys <names file>\n");
            return 2;
        }

        // A framework the runtime doesn't have is skipped: its names print as "(nosym)".
        for (int i = 0; kLibs[i]; i++) {
            dlopen(kLibs[i], RTLD_NOW);
        }

        NSError *readError = nil;
        NSString *names = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding
                                                       error:&readError];
        if (!names) {
            fprintf(stderr, "dumpkeys: cannot read %s: %s\n", argv[1], readError.localizedDescription.UTF8String);
            return 1;
        }
        for (NSString *name in [names componentsSeparatedByString:@"\n"]) {
            if (!name.length) continue;
            printf("%s = %s\n", name.UTF8String, valueOf(name));
            fflush(stdout);
        }
    }
}
