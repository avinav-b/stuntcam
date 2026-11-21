// SPDX-License-Identifier: Apache-2.0

// Package scheme finds Xcode schemes that insert their own libraries (DYLD_INSERT_LIBRARIES in a launch or test action's
// environment, e.g. Reveal or an in-house debug dylib). Boot mode's boot environment displaces them; `stuntcam chain add`
// brings them back (design §4.1 "Scheme-level inserts").
package scheme

import (
	"encoding/xml"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
	"strings"
)

type Insert struct {
	Scheme string   // .xcscheme path
	Libs   []string // the libraries named (variables like $(BUILT_PRODUCTS_DIR) kept as written)
}

type envVar struct {
	Key     string `xml:"key,attr"`
	Value   string `xml:"value,attr"`
	Enabled string `xml:"isEnabled,attr"`
}

type xcscheme struct {
	Launch struct {
		Env []envVar `xml:"EnvironmentVariables>EnvironmentVariable"`
	} `xml:"LaunchAction"`
	Test struct {
		Env []envVar `xml:"EnvironmentVariables>EnvironmentVariable"`
	} `xml:"TestAction"`
}

// Scan walks root (bounded depth, skipping dependency and build folders) for .xcscheme files that insert libraries.
func Scan(root string, maxDepth int) []Insert {
	var out []Insert
	walkFiles(root, maxDepth, func(p string) error {
		if !strings.HasSuffix(p, ".xcscheme") {
			return nil
		}
		if in, ok := parse(p); ok {
			out = append(out, in)
		}
		return nil
	})
	return out
}

// skippedDirs are the folders Scan and FindScheme never walk into: dependencies and build output.
var skippedDirs = []string{"node_modules", "Pods", "build", "DerivedData", ".git", ".dart_tool", ".build"}

// walkFiles calls visit for each file under root, at most maxDepth directory levels down, skipping skippedDirs and
// whatever can't be read. visit returns filepath.SkipAll to stop the walk.
func walkFiles(root string, maxDepth int, visit func(p string) error) {
	base := strings.Count(filepath.Clean(root), string(filepath.Separator))
	filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if d.IsDir() {
			if slices.Contains(skippedDirs, d.Name()) {
				return filepath.SkipDir
			}
			if strings.Count(p, string(filepath.Separator))-base > maxDepth {
				return filepath.SkipDir
			}
			return nil
		}
		return visit(p)
	})
}

func parse(p string) (Insert, bool) {
	b, err := os.ReadFile(p)
	if err != nil {
		return Insert{}, false
	}
	var s xcscheme
	if xml.Unmarshal(b, &s) != nil {
		return Insert{}, false
	}
	in := Insert{Scheme: p}
	for _, v := range append(s.Launch.Env, s.Test.Env...) {
		if v.Key != "DYLD_INSERT_LIBRARIES" || v.Enabled == "NO" {
			continue
		}
		for _, l := range strings.Split(v.Value, ":") {
			if l = strings.TrimSpace(l); l != "" && !slices.Contains(in.Libs, l) {
				in.Libs = append(in.Libs, l)
			}
		}
	}
	return in, len(in.Libs) > 0
}
