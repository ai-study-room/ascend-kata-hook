/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 * Description: ascend-docker-cli工具，配置容器挂载Ascend NPU设备
*/
#define _GNU_SOURCE
#include <stdbool.h>
#include <getopt.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include "securec.h"

#include "basic.h"
#include "ns.h"
#include "u_mount.h"
#include "cgrp.h"
#include "options.h"
#include "utils.h"
#include "logger.h"

#define DECIMAL     10

struct CmdArgs {
    char     devices[BUF_SIZE];
    char     rootfs[BUF_SIZE];
    int      pid;
    char     options[BUF_SIZE];
    struct MountList files;
    struct MountList dirs;
};

static struct option g_cmdOpts[] = {
    {"devices", required_argument, 0, 'd'},
    {"pid", required_argument, 0, 'p'},
    {"rootfs", required_argument, 0, 'r'},
    {"options", required_argument, 0, 'o'},
    {"mount-file", required_argument, 0, 'f'},
    {"mount-dir", required_argument, 0, 'i'},
    {0, 0, 0, 0}
};

typedef bool (*CmdArgParser)(struct CmdArgs *args, const char *arg);

static bool DevicesCmdArgParser(struct CmdArgs *args, const char *arg)
{
    if (args == NULL || arg == NULL) {
        Logger("args, arg pointer is null!", LEVEL_ERROR, SCREEN_YES);
        return false;
    }
    for (size_t iLoop = 0; iLoop < strlen(arg); iLoop++) {
        if ((isdigit(arg[iLoop]) == 0) && (arg[iLoop] != ',')) {
            Logger("failed to check devices.", LEVEL_ERROR, SCREEN_YES);
            return false;
        }
    }

    errno_t err = strcpy_s(args->devices, BUF_SIZE, arg);
    if (err != EOK) {
        Logger("failed to get devices from cmd args.", LEVEL_ERROR, SCREEN_YES);
        return false;
    }

    return true;
}

static bool PidCmdArgParser(struct CmdArgs *args, const char *arg)
{
    char buff[PATH_MAX] = {0};

    if (args == NULL || arg == NULL) {
        Logger("args, arg pointer is null!", LEVEL_ERROR, SCREEN_YES);
        return false;
    }
    errno = 0;
    args->pid = strtol(optarg, NULL, DECIMAL);
    const char* pidMax = "/proc/sys/kernel/pid_max";
    const size_t maxFileSzieMb = 10; // max 10MB
    if (!CheckExternalFile(pidMax, strlen(pidMax), maxFileSzieMb, true)) {
        Logger("failed to check pid_max path.", LEVEL_ERROR, SCREEN_YES);
        return false;
    }
    FILE* pFile = NULL;
    pFile = fopen(pidMax, "r");
    if ((pFile == NULL) || (fgets(buff, PATH_MAX, pFile) == NULL)) {
        if (pFile != NULL) {
            (void)fclose(pFile);
        }
        pFile = NULL;
        Logger("failed to get pid_max buff.", LEVEL_ERROR, SCREEN_YES);
        return false;
    }
    (void)fclose(pFile);
    if ((strlen(buff) > 0) && (buff[strlen(buff) -1] == '\n')) {
        buff[strlen(buff) -1] = '\0';
    }
    for (size_t iLoop = 0; iLoop < strlen(buff); iLoop++) {
        if (isdigit(buff[iLoop]) == 0) {
            Logger("failed to get pid_max value.", LEVEL_ERROR, SCREEN_YES);
            return false;
        }
    }
    if ((args->pid < 0) || (args->pid >= strtol(buff, NULL, DECIMAL))) {
        Logger("The PID out of bounds.", LEVEL_ERROR, SCREEN_YES);
        return false;
    }
    if (errno != 0) {
        char* str = FormatLogMessage("failed to convert pid string from cmd args, pid string: %s.", arg);
        Logger(str, LEVEL_ERROR, SCREEN_YES);
        free(str);
        return false;
    }
    return true;
}

static bool CheckFileLegality(const char* filePath, const size_t filePathLen,
    const size_t maxFileSzieMb)
{
    if ((filePathLen > PATH_MAX) || (filePathLen <= 0)) { // 长度越界
        Logger("filePathLen out of bounds!", LEVEL_ERROR, SCREEN_YES);
        return false;
    }
    for (size_t iLoop = 0; iLoop < filePathLen; iLoop++) {
        if (!IsValidChar(filePath[iLoop])) { // 非法字符
            Logger("filePath has an illegal character!", LEVEL_ERROR, SCREEN_YES);
            return false;
        }
    }
    char resolvedPath[PATH_MAX] = {0};
    if (realpath(filePath, resolvedPath) == NULL && errno != ENOENT) {
        Logger("realpath failed!", LEVEL_ERROR, SCREEN_YES);
        return false;
    }
    if (strcmp(resolvedPath, filePath) != 0) { // 存在软链接
        Logger("filePath has a soft link!", LEVEL_ERROR, SCREEN_YES);
        return false;
    }
    return true;
}

static bool RootfsCmdArgParser(struct CmdArgs *args, const char *arg)
{
    if (args == NULL || arg == NULL) {
        Logger("args, arg pointer is null!", LEVEL_ERROR, SCREEN_YES);
        return false;
    }

    errno_t err = strcpy_s(args->rootfs, BUF_SIZE, arg);
    if (err != EOK) {
        Logger("failed to get rootfs path from cmd args", LEVEL_ERROR, SCREEN_YES);
        return false;
    }
    const size_t maxFileSzieMb = 50; // max 50MB
    if (!CheckFileLegality(args->rootfs, strlen(args->rootfs), maxFileSzieMb)) {
        Logger("failed to check rootf.", LEVEL_ERROR, SCREEN_YES);
        return false;
    }

    return true;
}

static bool OptionsCmdArgParser(struct CmdArgs *args, const char *arg)
{
    if (args == NULL || arg == NULL) {
        Logger("args, arg pointer is null!", LEVEL_ERROR, SCREEN_YES);
        return false;
    }

    errno_t err = strcpy_s(args->options, BUF_SIZE, arg);
    if (err != EOK) {
        Logger("failed to get options string from cmd args", LEVEL_ERROR, SCREEN_YES);
        return false;
    }

    if ((strcmp(args->options, "NODRV,VIRTUAL") != 0) &&
        (strcmp(args->options, "NODRV") != 0) &&
        (strcmp(args->options, "VIRTUAL") != 0)) {
        Logger("Whitelist check failed.", LEVEL_ERROR, SCREEN_YES);
        return false;
    }

    return true;
}

static bool CheckWhiteList(const char* fileName)
{
    if (fileName == NULL) {
        return false;
    }
    bool fileExists = false;
    const char mountWhiteList[WHITE_LIST_NUM][PATH_MAX] = {{"/usr/local/Ascend/driver/lib64"},
        {"/usr/local/Ascend/driver/include"},
        {"/usr/local/dcmi"},
        {"/usr/local/bin/npu-smi"}};

    for (size_t iLoop = 0; iLoop < WHITE_LIST_NUM; iLoop++) {
        if (strcmp(mountWhiteList[iLoop], fileName) == 0) {
            fileExists = true;
        }
    }
    if (!fileExists) {
        char* str = FormatLogMessage("failed to check whiteList value: %s.", fileName);
        Logger(str, LEVEL_ERROR, SCREEN_YES);
        free(str);
        return false;
    }
    return true;
}

static bool MountFileCmdArgParser(struct CmdArgs *args, const char *arg)
{
    if (args == NULL || arg == NULL) {
        Logger("args, arg pointer is null!", LEVEL_ERROR, SCREEN_YES);
        return false;
    }

    if (args->files.count == MAX_MOUNT_NR) {
        char* str = FormatLogMessage("too many files to mount, max number is %u", MAX_MOUNT_NR);
        Logger(str, LEVEL_ERROR, SCREEN_YES);
        free(str);
        return false;
    }

    char *dst = &args->files.list[args->files.count++][0];
    errno_t err = strcpy_s(dst, PATH_MAX, arg);
    if (err != EOK) {
        char* str = FormatLogMessage("failed to copy mount file path: %s", arg);
        Logger(str, LEVEL_ERROR, SCREEN_YES);
        free(str);
        return false;
    }

    const size_t maxFileSzieMb = 50; // max 50MB
    if (!CheckFileLegality(dst, strlen(dst), maxFileSzieMb)) {
        char* str = FormatLogMessage("failed to check files: %s", dst);
        Logger(str, LEVEL_ERROR, SCREEN_YES);
        free(str);
        return false;
    }

    return CheckWhiteList(dst) ? true : false;
}

static bool MountDirCmdArgParser(struct CmdArgs *args, const char *arg)
{
    if (args == NULL || arg == NULL) {
        Logger("args, arg pointer is null!", LEVEL_ERROR, SCREEN_YES);
        return false;
    }

    if (args->dirs.count == MAX_MOUNT_NR) {
        char* str = FormatLogMessage("too many directories to mount, max number is %u", MAX_MOUNT_NR);
        Logger(str, LEVEL_ERROR, SCREEN_YES);
        free(str);
        return false;
    }

    char *dst = &args->dirs.list[args->dirs.count++][0];
    errno_t  err = strcpy_s(dst, PATH_MAX, arg);
    if (err != EOK) {
        char* str = FormatLogMessage("error: failed to copy mount directory path: %s", arg);
        Logger(str, LEVEL_ERROR, SCREEN_YES);
        free(str);
        return false;
    }
    const size_t maxFileSzieMb = 50; // max 50MB
    if (!CheckFileLegality(dst, strlen(dst), maxFileSzieMb)) {
        Logger("failed to check dir.", LEVEL_ERROR, SCREEN_YES);
        return false;
    }

    return CheckWhiteList(dst) ? true : false;
}

#define NUM_OF_CMD_ARGS 6

static struct {
    const char c;
    CmdArgParser parser;
} g_cmdArgParsers[NUM_OF_CMD_ARGS] = {
    {'d', DevicesCmdArgParser},
    {'p', PidCmdArgParser},
    {'r', RootfsCmdArgParser},
    {'o', OptionsCmdArgParser},
    {'f', MountFileCmdArgParser},
    {'i', MountDirCmdArgParser}
};

static int ParseOneCmdArg(struct CmdArgs *args, char indicator, const char *value)
{
    if (args == NULL || value == NULL) {
        Logger("args, value pointer is null!", LEVEL_ERROR, SCREEN_YES);
        return -1;
    }

    int i;
    for (i = 0; i < NUM_OF_CMD_ARGS; i++) {
        if (g_cmdArgParsers[i].c == indicator) {
            break;
        }
    }

    if (i == NUM_OF_CMD_ARGS) {
        char* str = FormatLogMessage("unrecognized cmd arg: indicate char: %c, value: %s.", indicator, value);
        Logger(str, LEVEL_ERROR, SCREEN_YES);
        free(str);
        return -1;
    }
    bool isOK = g_cmdArgParsers[i].parser(args, value);
    if (!isOK) {
        char* str = FormatLogMessage("failed while parsing cmd arg, indicate char: %c, value: %s.", indicator, value);
        Logger(str, LEVEL_ERROR, SCREEN_YES);
        free(str);
        return -1;
    }
    return 0;
}

static inline bool IsCmdArgsValid(const struct CmdArgs *args)
{
    if (args == NULL) {
        Logger("args pointer is null!", LEVEL_ERROR, SCREEN_YES);
        return false;
    }
    return (strlen(args->devices) > 0) && (strlen(args->rootfs) > 0) && (args->pid > 0);
}

static int ParseDeviceIDs(unsigned int *idList, size_t *idListSize, char *devices)
{
    if (idList == NULL || idListSize == NULL || devices == NULL) {
        Logger("idList, idListSize, devices pointer is null!", LEVEL_ERROR, SCREEN_YES);
        return -1;
    }

    static const char *sep = ",";
    char *token = NULL;
    char *context = NULL;
    size_t idx = 0;

    token = strtok_s(devices, sep, &context);
    while (token != NULL) {
        if (idx >= *idListSize) {
            char* str = FormatLogMessage("too many devices(%u), support %u devices maximally", idx, *idListSize);
            Logger(str, LEVEL_ERROR, SCREEN_YES);
            free(str);
            return -1;
        }

        errno = 0;
        idList[idx] = strtoul((const char *)token, NULL, DECIMAL);
        if (errno != 0) {
            char* str = FormatLogMessage("failed to convert device id (%s) from cmd args", token);
            Logger(str, LEVEL_ERROR, SCREEN_YES);
            free(str);
            return -1;
        }

        idx++;
        token = strtok_s(NULL, sep, &context);
    }

    *idListSize = idx;
    return 0;
}

int DoPrepare(const struct CmdArgs *args, struct ParsedConfig *config)
{
    if (args == NULL || config == NULL) {
        Logger("args, config pointer is null!", LEVEL_ERROR, SCREEN_YES);
        return -1;
    }

    int ret;
    errno_t err;

    err = strcpy_s(config->rootfs, BUF_SIZE, args->rootfs);
    if (err != EOK) {
        Logger("failed to copy rootfs path to parsed config.", LEVEL_ERROR, SCREEN_YES);
        return -1;
    }

    ret = ParseDeviceIDs(config->devices, &config->devicesNr, (char *)args->devices);
    if (ret < 0) {
        Logger("failed to parse device ids from cmdline argument", LEVEL_ERROR, SCREEN_YES);
        return -1;
    }

    ret = GetNsPath(args->pid, "mnt", config->containerNsPath, BUF_SIZE);
    if (ret < 0) {
        char* str = FormatLogMessage("failed to get container mnt ns path: pid(%d).", args->pid);
        Logger(str, LEVEL_ERROR, SCREEN_YES);
        free(str);
        return -1;
    }
    ret = GetCgroupPath(args->pid, config->cgroupPath, BUF_SIZE);
    if (ret < 0) {
        Logger("failed to get cgroup path.", LEVEL_ERROR, SCREEN_YES);
        return -1;
    }

    char originNsPath[BUF_SIZE] = {0};
    ret = GetSelfNsPath("mnt", originNsPath, BUF_SIZE);
    if (ret < 0) {
        Logger("failed to get self ns path.", LEVEL_ERROR, SCREEN_YES);
        return -1;
    }

    config->originNsFd = open((const char *)originNsPath, O_RDONLY); // proc接口，非外部输入
    if (config->originNsFd < 0) {
        char* str = FormatLogMessage("failed to get self ns fd: %s.", originNsPath);
        Logger(str, LEVEL_ERROR, SCREEN_YES);
        free(str);
        return -1;
    }

    config->files = (const struct MountList *)&args->files;
    config->dirs  = (const struct MountList *)&args->dirs;

    return 0;
}

int SetupContainer(struct CmdArgs *args)
{
    if (args == NULL) {
        Logger("args pointer is null!", LEVEL_ERROR, SCREEN_YES);
        return -1;
    }

    int ret;
    struct ParsedConfig config;

    InitParsedConfig(&config);

    Logger("prepare necessary config", LEVEL_INFO, SCREEN_YES);
    ret = DoPrepare(args, &config);
    if (ret < 0) {
        Logger("failed to prepare nesessary config.", LEVEL_ERROR, SCREEN_YES);
        return -1;
    }

    // enter container's mount namespace
    Logger("enter container's mount namespace", LEVEL_INFO, SCREEN_YES);
    ret = EnterNsByPath((const char *)config.containerNsPath, CLONE_NEWNS);
    if (ret < 0) {
        char* str = FormatLogMessage("failed to set to container ns: %s.", config.containerNsPath);
        Logger(str, LEVEL_ERROR, SCREEN_YES);
        free(str);
        close(config.originNsFd);
        return -1;
    }
    Logger("do mounting", LEVEL_INFO, SCREEN_YES);
    ret = DoMounting(&config);
    if (ret < 0) {
        Logger("failed to do mounting.", LEVEL_ERROR, SCREEN_YES);
        close(config.originNsFd);
        return -1;
    }
    Logger("setup up cgroup", LEVEL_INFO, SCREEN_YES);
    ret = SetupCgroup(&config);
    if (ret < 0) {
        Logger("failed to set up cgroup.", LEVEL_ERROR, SCREEN_YES);
        close(config.originNsFd);
        return -1;
    }

    // back to original namespace
    Logger("back to original namespace", LEVEL_INFO, SCREEN_YES);
    ret = EnterNsByFd(config.originNsFd, CLONE_NEWNS);
    if (ret < 0) {
        Logger("failed to set ns back.", LEVEL_ERROR, SCREEN_YES);
        close(config.originNsFd);
        return -1;
    }

    close(config.originNsFd);
    return 0;
}

int Process(int argc, char **argv)
{
    if (argv == NULL) {
        Logger("argv pointer is null!", LEVEL_ERROR, SCREEN_YES);
        return -1;
    }
    int c;
    int ret;
    int optionIndex;
    struct CmdArgs args = {0};

    Logger("runc start prestart-hook ...", LEVEL_INFO, SCREEN_YES);
    while ((c = getopt_long(argc, argv, "d:p:r:o:f:i", g_cmdOpts, &optionIndex)) != -1) {
        ret = ParseOneCmdArg(&args, (char)c, optarg);
        if (ret < 0) {
            Logger("failed to parse cmd args.", LEVEL_ERROR, SCREEN_YES);
            return -1;
        }
    }
    Logger("verify parameters valid and parse runtime options", LEVEL_INFO, SCREEN_YES);
    if (!IsCmdArgsValid(&args)) {
        Logger("information not completed or valid.", LEVEL_ERROR, SCREEN_YES);
        return -1;
    }

    ParseRuntimeOptions(args.options);
    Logger("setup container config ...", LEVEL_INFO, SCREEN_YES);
    ret = SetupContainer(&args);
    if (ret < 0) {
        Logger("failed to setup container.", LEVEL_ERROR, SCREEN_YES);
        return ret;
    }
    Logger("prestart-hook setup container successful.", LEVEL_INFO, SCREEN_YES);
    return 0;
}

#ifdef gtest
int _main(int argc, char **argv)
{
#else
int main(int argc, char **argv)
{
#endif
    int ret = Process(argc, argv);
    return ret;
}