/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2022. All rights reserved.
 * Description: ascend-docker-hook工具，配置容器挂载Ascend NPU设备
 */
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"syscall"

	"github.com/opencontainers/runtime-spec/specs-go"

	"mindxcheckutils"

	"huawei.com/mindx/common/hwlog"
)

const (
	loggingPrefix          = "ascend-docker-hook"
	runLogPath             = "/var/log/ascend-docker-runtime/hook-run.log"
	operateLogPath         = "/var/log/ascend-docker-runtime/hook-operate.log"
	ascendVisibleDevices   = "ASCEND_VISIBLE_DEVICES"
	ascendRuntimeOptions   = "ASCEND_RUNTIME_OPTIONS"
	ascendRuntimeMounts    = "ASCEND_RUNTIME_MOUNTS"
	ascendDockerCli        = "ascend-docker-cli"
	defaultAscendDockerCli = "/usr/local/bin/ascend-docker-cli"
	configDir              = "/etc/ascend-docker-runtime.d"
	baseConfig             = "base"
	configFileSuffix       = "list"

	borderNum        = 2
	kvPairSize       = 2
	maxCommandLength = 65535
)

var (
	containerConfigInputStream = os.Stdin
	doExec                     = syscall.Exec
	ascendDockerCliName        = ascendDockerCli
	defaultAscendDockerCliName = defaultAscendDockerCli
)

var validRuntimeOptions = [...]string{
	"NODRV",
	"VIRTUAL",
}

type containerConfig struct {
	Pid    int
	Rootfs string
	Env    []string
}

func initLogModule(ctx context.Context) error {
	const backups = 2
	const logMaxAge = 365
	runLogConfig := hwlog.LogConfig{
		LogFileName: runLogPath,
		LogLevel:    0,
		MaxBackups:  backups,
		MaxAge:      logMaxAge,
		OnlyToFile:  true,
	}
	if err := hwlog.InitRunLogger(&runLogConfig, ctx); err != nil {
		fmt.Printf("hwlog init failed, error is %v", err)
		return err
	}
	operateLogConfig := hwlog.LogConfig{
		LogFileName: operateLogPath,
		LogLevel:    0,
		MaxBackups:  backups,
		MaxAge:      logMaxAge,
		OnlyToFile:  true,
	}
	if err := hwlog.InitOperateLogger(&operateLogConfig, ctx); err != nil {
		fmt.Printf("hwlog init failed, error is %v", err)
		return err
	}
	return nil
}

func removeDuplication(devices []int) []int {
	list := make([]int, 0, len(devices))
	prev := -1

	for _, device := range devices {
		if device == prev {
			continue
		}

		list = append(list, device)
		prev = device
	}

	return list
}

func parseDevices(visibleDevices string) ([]int, error) {
	devices := make([]int, 0)
	const maxDevice = 128

	for _, d := range strings.Split(visibleDevices, ",") {
		d = strings.TrimSpace(d)
		if strings.Contains(d, "-") {
			borders := strings.Split(d, "-")
			if len(borders) != borderNum {
				return nil, fmt.Errorf("invalid device range: %s", d)
			}

			borders[0] = strings.TrimSpace(borders[0])
			borders[1] = strings.TrimSpace(borders[1])

			left, err := strconv.Atoi(borders[0])
			if err != nil || left < 0 {
				return nil, fmt.Errorf("invalid left boarder range parameter: %s", borders[0])
			}

			right, err := strconv.Atoi(borders[1])
			if err != nil || right > maxDevice {
				return nil, fmt.Errorf("invalid right boarder range parameter: %s", borders[1])
			}

			if left > right {
				return nil, fmt.Errorf("left boarder (%d) should not be larger than the right one(%d)", left, right)
			}

			for n := left; n <= right; n++ {
				devices = append(devices, n)
			}
		} else {
			n, err := strconv.Atoi(d)
			if err != nil {
				return nil, fmt.Errorf("invalid single device parameter: %s", d)
			}

			devices = append(devices, n)
		}
	}

	sort.Slice(devices, func(i, j int) bool { return i < j })
	return removeDuplication(devices), nil
}

func parseMounts(mounts string) []string {
	if mounts == "" {
		return []string{baseConfig}
	}
	const maxMountLength = 128
	if len(mounts) > maxMountLength {
		return []string{baseConfig}
	}

	mountConfigs := make([]string, 0)
	for _, m := range strings.Split(mounts, ",") {
		m = strings.TrimSpace(m)
		m = strings.ToLower(m)
		mountConfigs = append(mountConfigs, m)
	}

	return mountConfigs
}

func isRuntimeOptionValid(option string) bool {
	for _, validOption := range validRuntimeOptions {
		if option == validOption {
			return true
		}
	}

	return false
}

func parseRuntimeOptions(runtimeOptions string) ([]string, error) {
	parsedOptions := make([]string, 0)

	if runtimeOptions == "" {
		return parsedOptions, nil
	}
	const maxLength = 128
	if len(runtimeOptions) > maxLength {
		return nil, fmt.Errorf("invalid runtime option")
	}

	for _, option := range strings.Split(runtimeOptions, ",") {
		option = strings.TrimSpace(option)
		if !isRuntimeOptionValid(option) {
			return nil, fmt.Errorf("invalid runtime option")
		}

		parsedOptions = append(parsedOptions, option)
	}

	return parsedOptions, nil
}

func parseOciSpecFile(file string) (*specs.Spec, error) {
	f, err := os.Open(file)
	if err != nil {
		return nil, fmt.Errorf("failed to open the OCI config file: %s", file)
	}
	defer f.Close()

	spec := new(specs.Spec)
	if err := json.NewDecoder(f).Decode(spec); err != nil {
		return nil, fmt.Errorf("failed to parse OCI config file: %s, caused by: %v", file, err)
	}

	if spec.Process == nil {
		return nil, fmt.Errorf("invalid OCI spec for empty process")
	}

	if spec.Root == nil {
		return nil, fmt.Errorf("invalid OCI spec for empty root")
	}

	return spec, nil
}

var getContainerConfig = func() (*containerConfig, error) {
	state := new(specs.State)
	decoder := json.NewDecoder(containerConfigInputStream)

	if err := decoder.Decode(state); err != nil {
		return nil, fmt.Errorf("failed to parse the container's state")
	}

	configPath := path.Join(state.Bundle, "config.json")
	if _, err := mindxcheckutils.RealFileChecker(configPath, true, true, mindxcheckutils.DefaultSize); err != nil {
		return nil, err
	}

	ociSpec, err := parseOciSpecFile(configPath)
	if err != nil {
		return nil, fmt.Errorf("failed to parse OCI spec: %v", err)
	}
	// when use ctr->containerd. the rootfs in config.json is a relative path
	rfs := ociSpec.Root.Path
	if !filepath.IsAbs(rfs) {
		rfs = path.Join(state.Bundle, ociSpec.Root.Path)
	}

	ret := &containerConfig{
		Pid:    state.Pid,
		Rootfs: rfs,
		Env:    ociSpec.Process.Env,
	}

	return ret, nil
}

func getValueByKey(data []string, name string) string {
	for _, s := range data {
		p := strings.SplitN(s, "=", 2)
		if len(p) != kvPairSize {
			log.Panicln("environment error")
		}

		if p[0] == name && len(p) == kvPairSize {
			return p[1]
		}
	}

	return ""
}

func readMountConfig(dir string, name string) ([]string, []string, error) {
	configFileName := fmt.Sprintf("%s.%s", name, configFileSuffix)
	baseConfigFilePath, err := filepath.Abs(filepath.Join(dir, configFileName))
	if err != nil {
		return nil, nil, fmt.Errorf("failed to assemble base config file path: %v", err)
	}

	fileInfo, err := os.Stat(baseConfigFilePath)
	if _, err := mindxcheckutils.RealFileChecker(baseConfigFilePath, true, false,
		mindxcheckutils.DefaultSize); err != nil {
		return nil, nil, err
	}
	if err != nil {
		return nil, nil, fmt.Errorf("cannot stat base configuration file %s : %v", baseConfigFilePath, err)
	}

	if !fileInfo.Mode().IsRegular() {
		return nil, nil, fmt.Errorf("base configuration file damaged because is not a regular file")
	}

	f, err := os.Open(baseConfigFilePath)
	if err != nil {
		return nil, nil, fmt.Errorf("failed to open base configuration file %s: %v", baseConfigFilePath, err)
	}
	defer f.Close()

	fileMountList, dirMountList := make([]string, 0), make([]string, 0)
	const maxEntryNumber = 128
	entryCount := 0
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		mountPath := scanner.Text()
		entryCount = entryCount + 1
		if entryCount > maxEntryNumber {
			return nil, nil, fmt.Errorf("mount list too long")
		}
		absMountPath, err := filepath.Abs(mountPath)
		if err != nil {
			continue // skipping files/dirs with any problems
		}
		mountPath = absMountPath

		stat, err := os.Stat(mountPath)
		if err != nil {
			continue // skipping files/dirs with any problems
		}

		if stat.Mode().IsRegular() {
			fileMountList = append(fileMountList, mountPath)
		} else if stat.Mode().IsDir() {
			dirMountList = append(dirMountList, mountPath)
		}
	}

	return fileMountList, dirMountList, nil
}

func readConfigsOfDir(dir string, configs []string) ([]string, []string, error) {
	fileInfo, err := os.Stat(dir)
	if err != nil {
		return nil, nil, fmt.Errorf("cannot stat configuration directory %s : %v", dir, err)
	}

	if !fileInfo.Mode().IsDir() {
		return nil, nil, fmt.Errorf("%s should be a dir for ascend docker runtime, but now it is not", dir)
	}

	fileMountList := make([]string, 0)
	dirMountList := make([]string, 0)

	for _, config := range configs {
		fileList, dirList, err := readMountConfig(dir, config)
		if err != nil {
			return nil, nil, fmt.Errorf("failed to process config %s: %v", config, err)
		}

		fileMountList = append(fileMountList, fileList...)
		dirMountList = append(dirMountList, dirList...)
	}

	return fileMountList, dirMountList, nil
}

func getArgs(cliPath string, devices []int, containerConfig *containerConfig,
	fileMountList []string, dirMountList []string) []string {
	args := append([]string{cliPath},
		"--devices", strings.Trim(strings.Join(strings.Fields(fmt.Sprint(devices)), ","), "[]"),
		"--pid", fmt.Sprintf("%d", containerConfig.Pid), "--rootfs", containerConfig.Rootfs)
	for _, filePath := range fileMountList {
		args = append(args, "--mount-file", filePath)
	}
	for _, dirPath := range dirMountList {
		args = append(args, "--mount-dir", dirPath)
	}
	return args
}

func doPrestartHook() error {
	containerConfig, err := getContainerConfig()
	if err != nil {
		return fmt.Errorf("failed to get container config: %v", err)
	}

	visibleDevices := getValueByKey(containerConfig.Env, ascendVisibleDevices)
	if visibleDevices == "" {
		return nil
	}

	devices, err := parseDevices(visibleDevices)
	if err != nil {
		return fmt.Errorf("failed to parse device setting: %v", err)
	}

	mountConfigs := parseMounts(getValueByKey(containerConfig.Env, ascendRuntimeMounts))

	fileMountList, dirMountList, err := readConfigsOfDir(configDir, mountConfigs)
	if err != nil {
		return fmt.Errorf("failed to read configuration from config directory: %v", err)
	}

	parsedOptions, err := parseRuntimeOptions(getValueByKey(containerConfig.Env, ascendRuntimeOptions))
	if err != nil {
		return fmt.Errorf("failed to parse runtime options: %v", err)
	}

	currentExecPath, err := os.Executable()
	if err != nil {
		return fmt.Errorf("cannot get the path of ascend-docker-hook: %v", err)
	}

	cliPath := path.Join(path.Dir(currentExecPath), ascendDockerCliName)
	if _, err = os.Stat(cliPath); err != nil {
		return fmt.Errorf("cannot find ascend-docker-cli executable file at %s: %v", cliPath, err)
	}
	if _, err := mindxcheckutils.RealFileChecker(cliPath, true, false, mindxcheckutils.DefaultSize); err != nil {
		return err
	}
	args := getArgs(cliPath, devices, containerConfig, fileMountList, dirMountList)
	if len(parsedOptions) > 0 {
		args = append(args, "--options", strings.Join(parsedOptions, ","))
	}
	hwlog.OpLog.Infof("ascend docker hook success, will start cli")
	if err := mindxcheckutils.ChangeRuntimeLogMode("hook-run-", "hook-operate-"); err != nil {
		return err
	}
	if err := doExec(cliPath, args, os.Environ()); err != nil {
		return fmt.Errorf("failed to exec ascend-docker-cli %v: %v", args, err)
	}
	return nil
}

func main() {
	defer func() {
		if err := recover(); err != nil {
			log.Fatal(err)
		}
	}()
	log.SetPrefix(loggingPrefix)

	ctx, _ := context.WithCancel(context.Background())
	if err := initLogModule(ctx); err != nil {
		log.Fatal(err)
	}
	logPrefixWords, err := mindxcheckutils.GetLogPrefix()
	if err != nil {
		log.Fatal(err)
	}
	defer func() {
		if err := mindxcheckutils.ChangeRuntimeLogMode("hook-run-", "hook-operate-"); err != nil {
			fmt.Println("defer changeFileMode function failed")
		}
	}()
	hwlog.OpLog.Infof("%v ascend docker hook starting, try to setup container", logPrefixWords)
	hwlog.RunLog.Infof("ascend docker hook starting")
	if !mindxcheckutils.StringChecker(strings.Join(os.Args, " "), 0,
		maxCommandLength, mindxcheckutils.DefaultWhiteList+" ") {
		hwlog.RunLog.Errorf("ascend docker hook failed")
		hwlog.OpLog.Errorf("%v ascend docker hook failed", logPrefixWords)
		log.Fatal("command error")
	}
	if err := doPrestartHook(); err != nil {
		hwlog.RunLog.Errorf("ascend docker hook failed")
		hwlog.OpLog.Errorf("%v ascend docker hook failed", logPrefixWords)
		log.Fatal(fmt.Errorf("failed in runtime.doProcess "))
	}
}
