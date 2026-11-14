// SPDX-License-Identifier: Apache-2.0
//go:build !darwin

package procenv

import "errors"

func procargs2(int) ([]byte, error) { return nil, errors.New("procargs2: macOS only") }
