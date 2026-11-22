// SPDX-License-Identifier: Apache-2.0
package procenv

import (
	"syscall"
	"unsafe"
)

// The sysctl MIB names procargs2 reads (<sys/sysctl.h>).
const (
	ctlKern       = 1  // CTL_KERN
	kernProcargs2 = 49 // KERN_PROCARGS2
)

// procargs2 is sysctl {CTL_KERN, KERN_PROCARGS2, pid} (the caller's own processes need no privileges).
func procargs2(pid int) ([]byte, error) {
	mib := [3]int32{ctlKern, kernProcargs2, int32(pid)}
	size := uintptr(0)
	_, _, errno := syscall.Syscall6(syscall.SYS___SYSCTL,
		uintptr(unsafe.Pointer(&mib[0])), uintptr(len(mib)), 0, uintptr(unsafe.Pointer(&size)), 0, 0)
	if errno != 0 {
		return nil, errno
	}

	if size == 0 {
		return nil, syscall.EINVAL // nothing to read (and &raw[0] below would panic)
	}

	raw := make([]byte, size)
	_, _, errno = syscall.Syscall6(syscall.SYS___SYSCTL, uintptr(unsafe.Pointer(&mib[0])), uintptr(len(mib)),
		uintptr(unsafe.Pointer(&raw[0])), uintptr(unsafe.Pointer(&size)), 0, 0)
	if errno != 0 {
		return nil, errno
	}

	return raw[:size], nil
}
