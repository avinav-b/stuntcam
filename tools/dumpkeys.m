// SPDX-License-Identifier: Apache-2.0
// dumpkeys <names file> — runs in a simulator (`xcrun simctl spawn <udid> tools/bin/dumpkeys <file>`; build it with
// tools/build.sh dumpkeys): loads the capture frameworks, then prints "<name> = <value>" for each CFString constant named
// in the file, one name per line ("(nosym)": no such symbol, "(nonstring)": not a CFString, or its address
// could not be resolved).
#import <Foundation/Foundation.h>
#import <dlfcn.h>
int main(int argc, char **argv) { @autoreleasepool {
  const char *libs[] = {"/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture","/System/Library/PrivateFrameworks/AVFCapture.framework/AVFCapture","/System/Library/Frameworks/CoreMedia.framework/CoreMedia","/System/Library/PrivateFrameworks/CMCaptureCore.framework/CMCaptureCore","/System/Library/PrivateFrameworks/Quagga.framework/Quagga",NULL};
  for (int i=0; libs[i]; i++) dlopen(libs[i], RTLD_NOW);
  NSString *s = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:nil];
  for (NSString *name in [s componentsSeparatedByString:@"\n"]) { if (!name.length) continue;
    CFStringRef *p = dlsym(RTLD_DEFAULT, name.UTF8String);
    Dl_info di; const char *v="(nosym)"; if (p) { v="(nonstring)"; if (dladdr(*p,&di) && CFGetTypeID(*p)==CFStringGetTypeID()) v=[(__bridge NSString*)*p UTF8String]; } printf("%s = %s\n", name.UTF8String, v); fflush(stdout); }
}}
