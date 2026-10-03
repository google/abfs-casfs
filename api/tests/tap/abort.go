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

package main

import (
	"fmt"
	"log"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"syscall"
	"time"

	"github.com/google/abfs-casfs/api/hash"
	"github.com/google/abfs-casfs/api/xattr"
)

type TestType int

// Testing tap exits.
//
// These test will use the API directly (instead of running the
// standalone tap program).  As this is stand alone program the
// PGID will be correct allowing the use of the API directly.
// By using the API we can better control the exit scenarios by
// canceling the jobs.
//
// For each test this application will spawn a "program to tap",
// which is this program with the options:
//    TestTypeHold
//    TestTypeHoldBackground/TestTypeHoldBackgroundWait
//
// TestTypeHold:
//   - create and write some test files
//   - wait on signal from user
//   - write one more file
//   - close files on process exit
//
// TestTypeHoldBackground/TestTypeHoldBackgroundWait:
//   This is used for testing a run process that has completed
//  and the tap program has transitioned to wait for files to
//  be released.  This is a challenge here as to get a return
//  correctly need to detach which changes the pgid.  Instead
//  the program will start in the correct group and open a
//  file in the workspace it will then create a new process
//  via Start() and pass the file to the child process.
//  The child process holding the file will prevent it from
//  releasing and allow the tap session to transition to the
//  wait state.
//
// The tap exit scenarios covered are:
//
// Normal
//   no intervention from the user tap and tapped command exit
//  normally.
//
//  AbortWait
//   the application has notified the tap session that the
//   program has finished.  The tap session is waiting on files
//   to close and the user aborts.
//
//  AbortRun
//   the program being tapped has not yet completed and the
//   user has aborted the program.
//

const (
	TestTypeNormal    int = 0
	TestTypeAbortWait     = 1
	TestTypeAbortRun      = 2
	// Internal commands to run our application
	TestTypeHold               = 3
	TestTypeHoldBackground     = 4
	TestTypeHoldBackgroundWait = 5
)

// main mountpoint workspace test-type
func usage(prg string) {
	log.Fatalf("usage: %s mountpoint workspace test-type (%d..%d)", prg, TestTypeNormal, TestTypeAbortRun)
}
func runHold(workspace string) {
	sz := 10
	var files [10]*os.File
	for i := range sz {
		f, err := os.Create(filepath.Join(workspace, fmt.Sprintf("test-file-x-%d", i)))
		if err != nil {
			log.Fatal(err)
		}
		if _, err := f.WriteString(fmt.Sprintf("test file %d", i)); err != nil {
			log.Fatal(err)
		}
		files[i] = f
	}
	// Create another write action
	f, err := os.Create(filepath.Join(workspace, "test-file-final"))
	if err != nil {
		log.Fatal(err)
	}

	// Wait for signal
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGUSR1)

	_ = <-sigs

	if _, err := f.WriteString("test-file-final"); err != nil {
		log.Fatal(err)
	}
}
func runBackground() {
	// Wait for signal
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGUSR1)

	_ = <-sigs

}
func main() {
	args := os.Args
	argCount := len(args)
	if argCount != 4 {
		usage(args[0])
	}
	mount := args[1]
	workspace := args[2]
	test, err := strconv.Atoi(args[3])
	if err != nil {
		log.Fatal(err)
	}

	switch test {
	case TestTypeHold:
		runHold(workspace)
		return
	case TestTypeHoldBackground:
		// Create a file and pass to holder
		f, err := os.Create(filepath.Join(workspace, "backhold"))
		if err != nil {
			log.Fatal(err)
		}
		cmd := exec.Command(args[0], mount, workspace, strconv.Itoa(TestTypeHoldBackgroundWait))
		cmd.ExtraFiles = []*os.File{f}
		if err := cmd.Start(); err != nil {
			log.Fatal(err)
		}
		return
	case TestTypeHoldBackgroundWait:
		runBackground()
		return
	}
	if test > TestTypeAbortRun {
		log.Printf("Invalid test %d expected (%d..%d)\n", test, TestTypeNormal, TestTypeAbortRun)
		usage(args[0])
	}

	hasher := hash.NewHasher()
	tapPath := filepath.Join(mount, ".ctrl/tap")

	appPid := 0
	tapPid := 0
	go func() {
		var appProcess *os.Process
		var tapProcess *os.Process
		var err error
		time.Sleep(2 * time.Second)
		if appPid != 0 {
			appProcess, err = os.FindProcess(appPid)
			if err != nil {
				fmt.Printf("failed to get app process")
				return
			}
		}
		if tapPid != 0 {
			tapProcess, err = os.FindProcess(tapPid)
			if err != nil {
				fmt.Printf("failed to get tap process")
				return
			}

		}
		switch test {
		case TestTypeNormal:
			err = appProcess.Signal(syscall.SIGUSR1)
			if err != nil {
				fmt.Printf("failed to raise signal to hold")
			}
		case TestTypeAbortWait, TestTypeAbortRun:
			err = tapProcess.Signal(syscall.SIGKILL)
			if err != nil {
				fmt.Printf("failed to raise kill to tap")
			}
		}

	}()

	holdType := TestTypeHold
	if test == TestTypeAbortWait {
		holdType = TestTypeHoldBackground
	}

	cmd := exec.Command(args[0], mount, workspace, strconv.Itoa(holdType))
	// We don't care about the results want to verify that the spawned holder always exits.
	_, _ = xattr.RunTapCmd(cmd, hasher, tapPath, func(p int) {
		tapPid = os.Getpid()
		appPid = p
	})
	// TODO: Still need to cleanup the holder threads but by using kill
	// our process dies.
}
