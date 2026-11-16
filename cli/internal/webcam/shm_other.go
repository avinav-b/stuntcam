// SPDX-License-Identifier: Apache-2.0
//go:build !darwin

package webcam

import "syscall"

func ShmOwner(string) (int, error) { return -1, syscall.ENOSYS }
func ShmProducer(string) int       { return 0 }
func ShmUnlink(string) error       { return syscall.ENOSYS }
