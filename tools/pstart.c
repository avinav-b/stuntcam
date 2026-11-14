// SPDX-License-Identifier: Apache-2.0
// pstart <pid>: process start time (epoch seconds, µs) from kinfo_proc
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
int main(int c, char **v) { struct kinfo_proc kp; size_t l = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, atoi(v[1])};
  if (sysctl(mib, 4, &kp, &l, NULL, 0) || !l) return 1; printf("%ld.%06d\n", (long)kp.kp_proc.p_starttime.tv_sec, kp.kp_proc.p_starttime.tv_usec); return 0; }
