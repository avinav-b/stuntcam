// SPDX-License-Identifier: Apache-2.0

package main

import (
	"fmt"
	"maps"
	"slices"
	"strings"
	"time"
)

func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		return strings.TrimSpace(s[:i])
	}
	return s
}

func errText(err error) string {
	if err != nil {
		return " (" + err.Error() + ")"
	}
	return ""
}

func prefix(p, s string) string {
	if s == "" {
		return ""
	}
	return p + s
}

func ago(age time.Duration) string {
	switch {
	case age < time.Minute:
		return fmt.Sprintf("%ds", int(age.Seconds()))
	case age < time.Hour:
		return fmt.Sprintf("%dm", int(age.Minutes()))
	case age < 48*time.Hour:
		return fmt.Sprintf("%dh%02dm", int(age.Hours()), int(age.Minutes())%60)
	}
	return fmt.Sprintf("%dd", int(age.Hours()/24))
}

// plural is one if n is 1, else many.
func plural(n int, one, many string) string {
	if n == 1 {
		return one
	}
	return many
}

// shellQuote single-quotes a value for sh ($, backticks and backslashes stay literal).
func shellQuote(v string) string { return "'" + strings.ReplaceAll(v, "'", `'\''`) + "'" }

// sortedKeys is m's keys in order (for stable output).
func sortedKeys[V any](m map[string]V) []string { return slices.Sorted(maps.Keys(m)) }
