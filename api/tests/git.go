// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package tests

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"strings"

	"github.com/google/abfs-casfs/api"
	"golang.org/x/sync/errgroup"
)

type gitTest struct {
	fs  api.FS
	bin string
}

func gitError(stdout, stderr []string, err error) error {
	if err == nil {
		return nil
	}
	return fmt.Errorf("commit fail %v \nstdout:\n%s\nstderr:\n%s", err,
		strings.Join(stdout, "\n"),
		strings.Join(stderr, "\n"))

}

// ObjectPath returns the .git/objects/xx/yyy... path for a hash under root.
func (g *gitTest) ObjectPath(root, hash string) string {
	return filepath.Join(root, ".git", "objects", hash[:2], hash[2:])
}

func (g *gitTest) RunCmdInDir(root string, args ...string) (stdout []string, stderr []string, err error) {
	stdoutB := new(bytes.Buffer)
	stderrB := new(bytes.Buffer)
	cmd := exec.Command(g.bin, args...)
	cmd.Dir = root
	cmd.Stdout = stdoutB
	cmd.Stderr = stderrB

	err = cmd.Run()
	scanner := bufio.NewScanner(stdoutB)
	for scanner.Scan() {
		stdout = append(stdout, scanner.Text())
	}
	scanner = bufio.NewScanner(stderrB)
	for scanner.Scan() {
		stderr = append(stderr, scanner.Text())
	}

	return stdout, stderr, err
}

func (g *gitTest) InitEmpty(root string) error {
	return gitError(g.RunCmdInDir(root, "init", "."))

}
func (g *gitTest) Add(root string, file string) error {
	return gitError(g.RunCmdInDir(root, "add", file))
}
func (g *gitTest) AddAll(root string) error {
	return gitError(g.RunCmdInDir(root, "add", "."))
}
func (g *gitTest) Commit(root, msg string) error {
	return gitError(g.RunCmdInDir(root, "commit", "-m", msg))
}
func (g *gitTest) Config(root, key, value string) error {
	return gitError(g.RunCmdInDir(root, "config", key, value))
}
func (g *gitTest) PopulateNonCasRepo(root string) error {
	// Create a git repo outside of cas to drive cas tests
	if err := g.InitEmpty(root); err != nil {
		return err
	}
	if err := g.Config(root, "core.compression", "0"); err != nil {
		return err
	}
	if err := PopulateTestStage(root); err != nil {
		return err
	}
	if err := g.AddAll(root); err != nil {
		return err
	}
	if err := g.Commit(root, "Setup reference git repo"); err != nil {
		return err
	}
	return nil
}
func (g *gitTest) CreateNonCasRepo(root string) error {
	if err := MkdirTest(root); err != nil {
		return fmt.Errorf("failed to create starting dir: %v", err)
	}
	return g.PopulateNonCasRepo(root)
}
func (g *gitTest) GetHead(root string) (string, error) {
	stdout, stderr, err := g.RunCmdInDir(root, "rev-parse", "HEAD")
	if err != nil {
		return "", fmt.Errorf("rev-parse: error %v (%s)", err, strings.Join(stderr, " "))
	}
	if len(stdout) != 1 {
		return "", fmt.Errorf("expected 1 line got %d lines", len(stdout))
	}
	return stdout[0], nil
}

func (g *gitTest) CatBlob(root string, hash string) ([]string, error) {
	stdout, stderr, err := g.RunCmdInDir(root, "cat-file", "blob", hash)
	if err != nil {
		return nil, fmt.Errorf("cat-file: error %v (%s)", err, strings.Join(stderr, " "))
	}
	return stdout, nil
}

func (g *gitTest) AllBlobs(root string) ([]string, error) {
	cmd := exec.Command("bash", "-c", "git rev-list --objects --all | git cat-file --batch-check='%(objectname) %(objecttype) %(rest)'")
	cmd.Dir = root
	output, err := cmd.Output()
	if err != nil {
		return nil, err
	}

	blobs := []string{}
	scanner := bufio.NewScanner(bytes.NewBuffer(output))
	for scanner.Scan() {
		line := scanner.Text()
		splits := strings.Split(line, " ")
		if len(splits) < 2 {
			return nil, fmt.Errorf("badly formatted line: %v", line)
		}
		if splits[1] == "blob" {
			blobs = append(blobs, splits[0])
		}
	}

	return blobs, nil
}

func (g *gitTest) GitShow(root string) ([]string, error) {
	stdout, stderr, err := g.RunCmdInDir(root, "show")
	if err != nil {
		return nil, gitError(stdout, stderr, err)
	}
	return stdout, err
}
func (g *gitTest) GitStatus(root string) ([]string, error) {
	stdout, stderr, err := g.RunCmdInDir(root, "status", "--short")
	if err != nil {
		return nil, gitError(stdout, stderr, err)
	}
	return stdout, err

}
func (g *gitTest) GitCommit(root, message string) ([]string, error) {
	stdout, stderr, err := g.RunCmdInDir(root, "commit", "-m", message)
	if err != nil {
		return nil, gitError(stdout, stderr, err)
	}
	return stdout, err
}
func (g *gitTest) CtrlGitTest(root, head string) error {
	if err := MkdirTest(root); err != nil {
		return err
	}
	return g.fs.CreateGit(root, ".git", head)

}

func (g *gitTest) CtrlGitRmTest(root, head string) error {
	if err := g.CtrlGitTest(root, head); err != nil {
		return err
	}
	path := filepath.Join(root, ".git")
	if err := os.RemoveAll(path); err != nil {
		return err
	}
	if _, err := os.Stat(path); err == nil {
		return fmt.Errorf("unexpected stat after delete on %s", path)
	}

	return nil

}
func (g *gitTest) CtrlGitObjTest(parent, dirName string) error {
	if err := MkdirTest(parent); err != nil {
		return err
	}
	if err := g.fs.CreateGitObjects(parent, dirName); err != nil {
		return err
	}
	return ValidateDir(filepath.Join(parent, dirName))
}

func (g *gitTest) CtrlGitObjRmTest(parent, dirName string) error {
	if err := g.CtrlGitObjTest(parent, dirName); err != nil {
		return err
	}
	path := filepath.Join(parent, dirName)
	if err := os.RemoveAll(path); err != nil {
		return err
	}
	if _, err := os.Stat(path); err == nil {
		return fmt.Errorf("unexpected stat after delete on %s", path)
	}
	return nil
}

func (g *gitTest) CtrlGitIndexTest(mntRoot, dotGitParent, workTreeDir, hash string) error {
	// Create our directories
	if err := MkdirTest(dotGitParent); err != nil {
		return err
	}
	if err := MkdirTest(workTreeDir); err != nil {
		return err
	}

	return g.fs.CreateGitIndex(dotGitParent, ".git", workTreeDir, hash)
}

func (g *gitTest) HeadTest(root, head string) error {
	if err := g.CtrlGitTest(root, head); err != nil {
		return err
	}
	back, err := g.GetHead(root)
	if err != nil {
		return err
	}
	if back != head {
		return fmt.Errorf("unexpected head %s expected %s", back, head)
	}
	return nil
}
func (g *gitTest) ShowTest(root, head string) error {
	if err := g.CtrlGitTest(root, head); err != nil {
		return err
	}
	_, err := g.GitShow(root)
	return err

}
func (g *gitTest) CommitTest(root, head string) error {
	if err := g.CtrlGitTest(root, head); err != nil {
		return err
	}
	start, err := g.GetHead(root)
	if err != nil {
		return fmt.Errorf("failed go get head: %v", err)
	}
	_, err = g.GitShow(root)
	if err != nil {
		return fmt.Errorf("failed to run show: %v", err)
	}
	filepath := path.Join(root, "test-file")
	if err := FileTestContent(filepath, "with data"); err != nil {
		return fmt.Errorf("failed to write test content: %v", err)
	}
	// Add file
	if err := g.Add(root, filepath); err != nil {
		return fmt.Errorf("failed to add file: %v", err)
	}
	if _, err := g.GitCommit(root, "commit test message"); err != nil {
		return fmt.Errorf("failed to commit: %v", err)
	}
	back, err := g.GetHead(root)
	if err != nil {
		return fmt.Errorf("failed to get new head: %v", err)
	}
	if start == back {
		return fmt.Errorf("expected new head got same %s as previous head", start)
	}

	return nil
}

func CopyFile(src, dst string) error {
	srcFile, err := os.Open(src)
	if err != nil {
		return err
	}
	defer srcFile.Close()
	dstFile, err := os.Create(dst)
	if err != nil {
		if os.IsExist(err) {
			return nil
		}
		return err
	}
	defer dstFile.Close()
	_, err = io.Copy(dstFile, srcFile)
	return err
}

// TxOneObject copies a single loose object from a non-cas repo into the cache
// objects dir, using the same 3/37 split as TxObjects.
func TxOneObject(srcRoot, cacheDir, hash string) error {
	src := filepath.Join(srcRoot, ".git", "objects", hash[:2], hash[2:])
	dstDir := filepath.Join(cacheDir, hash[:3])
	if err := os.MkdirAll(dstDir, 0755); err != nil {
		return err
	}
	return CopyFile(src, filepath.Join(dstDir, hash[3:]))
}
func TxObjects(rootDir, cacheDir string) error {
	// Copy all objects from git dir to cache dir
	// inside cache dir create objects as xxx/xxx...
	gitObjDir := filepath.Join(rootDir, ".git", "objects")
	err := filepath.Walk(gitObjDir, func(path string, info fs.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}
		relPath, found := strings.CutPrefix(path, gitObjDir)
		if !found {
			return nil
		}
		name := strings.ReplaceAll(relPath, "/", "")
		if len(name) == 40 {
			dir := name[0:3]
			fname := name[3:]
			if err := os.MkdirAll(filepath.Join(cacheDir, dir), 0755); err != nil {
				return err
			}
			if err := CopyFile(path, filepath.Join(cacheDir, dir, fname)); err != nil {
				return err
			}

		}
		return nil
	})
	return err
}

func (g *gitTest) LFSLikeAccess(root, head string) error {
	if err := g.CtrlGitTest(root, head); err != nil {
		return err
	}
	blobs, err := g.AllBlobs(root)
	if err != nil {
		return err
	}

	readFile := func(path string) error {
		f, err := os.Open(path)
		if err != nil {
			return fmt.Errorf("open error: %v", err)
		}
		defer f.Close()

		// git-lfs reads 4096 at a time
		buffer := make([]byte, 4096)

		_, err = f.Read(buffer)
		if err != nil {
			if err == io.EOF {
				return nil
			}
			return fmt.Errorf("read error: %v", err)
		}

		return nil
	}

	var eg errgroup.Group
	// Read all blobs multiple times from .git/objects
	// This simulates a LFS access pattern in a directory of files
	// where multiple files contain the same content and therefore same SHA
	for _, blob := range blobs {
		path := filepath.Join(root, ".git", "objects", blob[0:2], blob[2:])
		for i := 0; i < 10; i++ {
			eg.Go(func() error {
				return readFile(path)
			})
		}
	}

	return eg.Wait()
}
func (g *gitTest) GitFsck(root string, extraArgs ...string) ([]string, error) {
	args := append([]string{"fsck", "--full"}, extraArgs...)
	stdout, stderr, err := g.RunCmdInDir(root, args...)
	if err != nil {
		return nil, gitError(stdout, stderr, err)
	}
	return stdout, nil
}
