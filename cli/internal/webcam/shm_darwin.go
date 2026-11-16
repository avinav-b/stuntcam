// SPDX-License-Identifier: Apache-2.0
package webcam

import (
	"encoding/binary"
	"syscall"
	"unsafe"
)

func shmOpen(name string, flags int) (int, error) {
	p, err := syscall.BytePtrFromString(name)
	if err != nil {
		return -1, err
	}
	fd, _, e := syscall.Syscall(syscall.SYS_SHM_OPEN, uintptr(unsafe.Pointer(p)), uintptr(flags), 0)
	if e != 0 {
		return -1, e
	}
	return int(fd), nil
}

// ShmOwner is the uid owning the shm object at name. ENOENT: nothing there; EACCES: another user's object (0600).
func ShmOwner(name string) (int, error) {
	fd, err := shmOpen(name, syscall.O_RDONLY)
	if err != nil {
		return -1, err
	}
	defer syscall.Close(fd)
	var st syscall.Stat_t
	if err := syscall.Fstat(fd, &st); err != nil {
		return -1, err
	}
	return int(st.Uid), nil
}

// ShmProducer reads the producerPid of the ring at name (owned by this user, a ring header with the magic set), or 0.
func ShmProducer(name string) int {
	fd, err := shmOpen(name, syscall.O_RDONLY)
	if err != nil {
		return 0
	}
	defer syscall.Close(fd)
	var st syscall.Stat_t
	if syscall.Fstat(fd, &st) != nil || int(st.Uid) != syscall.Getuid() || st.Size < 4096 {
		return 0
	}
	b, err := syscall.Mmap(fd, 0, 4096, syscall.PROT_READ, syscall.MAP_SHARED)
	if err != nil {
		return 0
	}
	defer syscall.Munmap(b)
	if binary.LittleEndian.Uint32(b[0:4]) != 0x52574353 {
		return 0
	}
	return int(binary.LittleEndian.Uint32(b[76:80]))
}

// ShmUnlink removes the shm name.
func ShmUnlink(name string) error {
	p, err := syscall.BytePtrFromString(name)
	if err != nil {
		return err
	}
	if _, _, e := syscall.Syscall(syscall.SYS_SHM_UNLINK, uintptr(unsafe.Pointer(p)), 0, 0); e != 0 {
		return e
	}
	return nil
}
