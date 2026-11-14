// SPDX-License-Identifier: Apache-2.0
#import "SCTry.h"
NSString *SCTry(NS_NOESCAPE void (^block)(void)) {
  @try { block(); return nil; }
  @catch (NSException *e) { return [NSString stringWithFormat:@"%@: %@", e.name, e.reason]; }
  @catch (id e) { return [NSString stringWithFormat:@"non-NSException %@", e]; }
}
