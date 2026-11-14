// SPDX-License-Identifier: Apache-2.0
// SCTry.h — run a block, catch any Objective-C exception it raises (features-probe, w4 e22)
#import <Foundation/Foundation.h>
NS_ASSUME_NONNULL_BEGIN
/// nil if the block returned normally, else "<name>: <reason>" of the exception it raised
NSString *_Nullable SCTry(NS_NOESCAPE void (^block)(void));
NS_ASSUME_NONNULL_END
