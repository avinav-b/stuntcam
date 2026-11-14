// SPDX-License-Identifier: Apache-2.0
package procenv

import (
	"syscall"
	"unsafe"
)

// procargs2 is sysctl {CTL_KERN, KERN_PROCARGS2, pid} (the caller's own processes need no privileges).
func procargs2(pid int) ([]byte, error) {
	mib := [3]int32{1, 49, int32(pid)} // CTL_KERN, KERN_PROCARGS2
	n := uintptr(0)
	if _, _, e := syscall.Syscall6(syscall.SYS___SYSCTL, uintptr(unsafe.Pointer(&mib[0])), 3, 0, uintptr(unsafe.Pointer(&n)), 0, 0); e != 0 {
		return nil, e
	}
	if n == 0 {
		return nil, syscall.EINVAL // nothing to read (and &b[0] below would panic)
	}
	b := make([]byte, n)
	if _, _, e := syscall.Syscall6(syscall.SYS___SYSCTL, uintptr(unsafe.Pointer(&mib[0])), 3, uintptr(unsafe.Pointer(&b[0])), uintptr(unsafe.Pointer(&n)), 0, 0); e != 0 {
		return nil, e
	}
	return b[:n], nil
}
