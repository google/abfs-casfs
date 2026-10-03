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
	"flag"
	"fmt"
	"os"
	"sync"
	"testing"
)

var rootDir string
var nonCasDir string
var objectsDir string
var blobsDir string
var arisDir string
var scratchDir string
var cli string
var env *Env

func init() {
	flag.StringVar(&rootDir, "root", "", "root directory to run tests in")
	flag.StringVar(&nonCasDir, "nonCasDir", "", "non-casfs scratch directory")
	flag.StringVar(&objectsDir, "objectsDir", "", "where to put git objects from non-cas repo")
	flag.StringVar(&blobsDir, "blobsDir", "", "where blobs are written to")
	flag.StringVar(&arisDir, "arisDir", "", "where ARIs are written to")
	flag.StringVar(&scratchDir, "scratchDir", "", "where temp file are written to")
	flag.StringVar(&cli, "cli", "", "path to casfsapi cli")
}

func TestMain(m *testing.M) {
	flag.Parse()
	if rootDir == "" && nonCasDir == "" && objectsDir == "" && arisDir == "" {
		// Plain `go test ./...` runs without the flags this suite needs.
		// It requires a mounted casfs; tests/api_test.sh sets it up.
		fmt.Println("skipping casfs integration tests: no -root, -nonCasDir, -objectsDir or -arisDir given (run tests/api_test.sh)")
		os.Exit(0)
	}
	if rootDir == "" || nonCasDir == "" || objectsDir == "" || arisDir == "" {
		panic(fmt.Sprintf("Missing required input <rootDir:%s> <nonCasDir:%s> <objectsDir:%s> <arisDir:%s>", rootDir, nonCasDir, objectsDir, arisDir))

	}
	if _, err := os.Stat(rootDir); err != nil {
		panic(fmt.Sprintf("Root directory %s does not exist", rootDir))
	}
	if _, err := os.Stat(nonCasDir); err != nil {
		panic(fmt.Sprintf("Non cas directory %s does not exist", nonCasDir))
	}
	if _, err := os.Stat(objectsDir); err != nil {
		panic(fmt.Sprintf("Objects directory %s does not exist", objectsDir))
	}
	if _, err := os.Stat(blobsDir); err != nil {
		panic(fmt.Sprintf("Blobs directory %s does not exists", blobsDir))
	}
	if _, err := os.Stat(arisDir); err != nil {
		panic(fmt.Sprintf("ARIs directory %s does not exists", arisDir))
	}
	if _, err := os.Stat(scratchDir); err != nil {
		panic(fmt.Sprintf("Scratch directory %s does not exists", scratchDir))
	}
	var err error
	env, err = AllTestSetup(rootDir, nonCasDir, cli)
	if err != nil {
		panic(fmt.Sprintf("Unable to test setup: %v", err))
	}

	// Create dir for non cas that can be deleted when done
	env.mountObjDir = objectsDir
	env.mountBlobDir = blobsDir
	env.mountAriDir = arisDir

	err = gitTestInit(env.nonCasfsTestDir, env.mountObjDir)
	if err != nil {
		panic(fmt.Sprintf("Unable to setup git tests: %v", err))
	}
	// Defer does not run after os.Exit(). This defer is only executed on a
	// fatal or panic exit.
	defer AllTestTearDown(env)

	var wg sync.WaitGroup
	var watchErr error
	wg.Add(1)
	go func() {
		watchErr = Watch(objectsDir, blobsDir, scratchDir)
		wg.Done()
	}()
	exitCode := m.Run()
	EndWatch()
	wg.Wait()
	if watchErr != nil {
		panic(fmt.Sprintf("Watch error %s", watchErr))
	}
	AllTestTearDown(env)
	os.Exit(exitCode)

}
