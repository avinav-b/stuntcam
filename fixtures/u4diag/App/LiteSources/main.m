// SPDX-License-Identifier: Apache-2.0
// M11Lite: UIKit only (does not link AVFoundation). -M11MODE latecam: dlopen AVFoundation 1 s after launch and run a capture session via the runtime.
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <dlfcn.h>
#import <mach-o/dyld.h>
static NSString *gOut = @"/tmp/simcam-m11/results/app";   // the probe's output dir
static void note(NSString *s) {
    NSString *p = [NSString stringWithFormat:@"%@/lite.%d.log", gOut, getpid()];
    NSString *line = [NSString stringWithFormat:@"%.3f %@\n", [NSDate date].timeIntervalSince1970, s];
    NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:p];
    if (!h) { [line writeToFile:p atomically:YES encoding:NSUTF8StringEncoding error:nil]; return; }
    [h seekToEndOfFile]; [h writeData:[line dataUsingEncoding:NSUTF8StringEncoding]]; [h closeFile];
}
static BOOL loaded(const char *frag) { for (uint32_t i = 0; i < _dyld_image_count(); i++) if (strstr(_dyld_get_image_name(i), frag)) return YES; return NO; }
@interface Del : NSObject @property int frames; @end
@implementation Del
- (void)captureOutput:(id)o didOutputSampleBuffer:(id)sb fromConnection:(id)c { self.frames++; }
@end
@interface SD : UIResponder <UIWindowSceneDelegate> @property UIWindow *window; @end
@implementation SD
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)s options:(UISceneConnectionOptions *)o {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene]; UIViewController *vc = [UIViewController new]; vc.view.backgroundColor = UIColor.systemOrangeColor;
    self.window.rootViewController = vc; [self.window makeKeyAndVisible];
}
@end
@interface AD : UIResponder <UIApplicationDelegate> @property id session; @property Del *del; @end
@implementation AD
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o {
    NSArray *args = NSProcessInfo.processInfo.arguments; NSUInteger i = [args indexOfObject:@"-M11MODE"];
    NSString *mode = (i != NSNotFound && i + 1 < args.count) ? args[i + 1] : @"nocam";
    note([NSString stringWithFormat:@"START lite mode=%@ AVFCapture-loaded-at-launch=%d stub=%d real=%d", mode, loaded("/AVFCapture.framework/"), loaded("libsimcamstub"), loaded("libm11fig")]);
    if ([mode isEqualToString:@"latecam"]) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        double t0 = CFAbsoluteTimeGetCurrent();
        note([NSString stringWithFormat:@"CoreMotion loaded before dlopen=%d", loaded("/CoreMotion.framework/")]);
        void *cmh = dlopen("/System/Library/Frameworks/CoreMotion.framework/CoreMotion", RTLD_NOW);
        note([NSString stringWithFormat:@"dlopen CoreMotion -> %p in %.1f ms; real loaded now=%d", cmh, (CFAbsoluteTimeGetCurrent() - t0) * 1000, loaded("libm11fig")]);
        t0 = CFAbsoluteTimeGetCurrent();
        void *h = dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_NOW);
        note([NSString stringWithFormat:@"dlopen AVFoundation -> %p in %.1f ms; real loaded now=%d", h, (CFAbsoluteTimeGetCurrent() - t0) * 1000, loaded("libm11fig")]);
        Class D = NSClassFromString(@"AVCaptureDevice"), I = NSClassFromString(@"AVCaptureDeviceInput"), S = NSClassFromString(@"AVCaptureSession"), O = NSClassFromString(@"AVCaptureVideoDataOutput");
        id dev = ((id (*)(id, SEL, id))objc_msgSend)(D, @selector(defaultDeviceWithMediaType:), @"vide");
        note([NSString stringWithFormat:@"device=%@", [dev valueForKey:@"localizedName"]]);
        if (!dev) { note(@"DONE nocamera"); return; }
        id in = ((id (*)(id, SEL, id, id *))objc_msgSend)(I, @selector(deviceInputWithDevice:error:), dev, nil);
        self.session = [S new]; id out = [O new]; self.del = [Del new];
        ((void (*)(id, SEL, id, id))objc_msgSend)(out, @selector(setSampleBufferDelegate:queue:), self.del, dispatch_queue_create("f", 0));
        ((void (*)(id, SEL, id))objc_msgSend)(self.session, NSSelectorFromString(@"addInput:"), in); ((void (*)(id, SEL, id))objc_msgSend)(self.session, NSSelectorFromString(@"addOutput:"), out);
        dispatch_async(dispatch_get_global_queue(0, 0), ^{ ((void (*)(id, SEL))objc_msgSend)(self.session, NSSelectorFromString(@"startRunning")); });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ note([NSString stringWithFormat:@"DONE frames=%d", self.del.frames]); });
    });
    else note(@"DONE nocam");
    return YES;
}
@end
int main(int argc, char *argv[]) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, @"AD"); } }
