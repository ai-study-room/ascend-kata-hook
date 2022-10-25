﻿/*
 * Copyright: Copyright (c) Huawei Technologies Co., Ltd. 2021-2022. All rights reserved.
 * Description: DCMI API Reference
 */

#ifndef __DCMI_INTERFACE_API_H__
#define __DCMI_INTERFACE_API_H__
#include <stddef.h>
#define _GNU_SOURCE
#include <link.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* __cplusplus */

void *dcmiHandle;
#define SO_NOT_FOUND  (-99999)
#define FUNCTION_NOT_FOUND  (-99998)
#define SUCCESS  (0)
#define ERROR_UNKNOWN  (-99997)
#define SO_NOT_CORRECT  (-99996)
#define CALL_FUNC(name, ...) if (name##_func == NULL) {return FUNCTION_NOT_FOUND;}return name##_func(__VA_ARGS__)
#define DCMI_VDEV_FOR_RESERVE (32)
struct dcmi_create_vdev_out {
    unsigned int vdev_id;
    unsigned int pcie_bus;
    unsigned int pcie_device;
    unsigned int pcie_func;
    unsigned int vfg_id;
    unsigned char reserved[DCMI_VDEV_FOR_RESERVE];
};
struct dcmi_create_vdev_res_stru {
    unsigned int vdev_id;
    unsigned int vfg_id;
    char template_name[32];
    unsigned char reserved[64];
};

// dcmi
int (*dcmi_init_func)();
int dcmi_init()
{
    CALL_FUNC(dcmi_init);
}

int (*dcmi_get_card_num_list_func)(int *card_num, int *card_list, int list_length);
int dcmi_get_card_num_list(int *card_num, int *card_list, int list_length)
{
    CALL_FUNC(dcmi_get_card_num_list, card_num, card_list, list_length);
}

int (*dcmi_get_device_num_in_card_func)(int card_id, int *device_num);
int dcmi_get_device_num_in_card(int card_id, int *device_num)
{
    CALL_FUNC(dcmi_get_device_num_in_card, card_id, device_num);
}

int (*dcmi_get_device_logic_id_func)(int *device_logic_id, int card_id, int device_id);
int dcmi_get_device_logic_id(int *device_logic_id, int card_id, int device_id)
{
    CALL_FUNC(dcmi_get_device_logic_id, device_logic_id, card_id, device_id);
}

int (*dcmi_create_vdevice_func)(int card_id, int device_id,
                                struct dcmi_create_vdev_res_stru *vdev,
                                struct dcmi_create_vdev_out *out);
int dcmi_create_vdevice(int card_id, int device_id,
                        struct dcmi_create_vdev_res_stru *vdev,
                        struct dcmi_create_vdev_out *out)
{
    CALL_FUNC(dcmi_create_vdevice, card_id, device_id, vdev, out);
}

int (*dcmi_set_destroy_vdevice_func)(int card_id, int device_id, unsigned int VDevid);
int dcmi_set_destroy_vdevice(int card_id, int device_id, unsigned int VDevid)
{
    CALL_FUNC(dcmi_set_destroy_vdevice, card_id, device_id, VDevid);
}

int (*dcmi_get_device_logicid_from_phyid_func)(unsigned int phyid, unsigned int *logicid);
int dcmi_get_device_logicid_from_phyid(unsigned int phyid, unsigned int *logicid)
{
    CALL_FUNC(dcmi_get_device_logicid_from_phyid, phyid, logicid);
}

// load .so files and functions
int dcmiInit_dl(char *dl_path)
{
    dcmiHandle = dlopen("libdcmi.so", RTLD_LAZY | RTLD_GLOBAL);
    if (dcmiHandle == NULL) {
        fprintf (stderr, "%s\n", dlerror());
        return SO_NOT_FOUND;
    }
    struct link_map *pLinkMap;
    int ret = dlinfo(dcmiHandle, RTLD_DI_LINKMAP, &pLinkMap);
    if (ret != 0) {
        fprintf(stderr, "dlinfo sofile failed :%s\n", dlerror());
        return SO_NOT_CORRECT;
    }

    size_t path_size = strlen(pLinkMap->l_name);
    for (int i = 0; i < path_size && i < PATH_MAX; i++) {
        dl_path[i] = pLinkMap->l_name[i];
    }

    dcmi_init_func = dlsym(dcmiHandle, "dcmi_init");

    dcmi_get_card_num_list_func = dlsym(dcmiHandle, "dcmi_get_card_num_list");

    dcmi_get_device_num_in_card_func = dlsym(dcmiHandle, "dcmi_get_device_num_in_card");

    dcmi_get_device_logic_id_func = dlsym(dcmiHandle, "dcmi_get_device_logic_id");

    dcmi_create_vdevice_func = dlsym(dcmiHandle, "dcmi_create_vdevice");

    dcmi_set_destroy_vdevice_func = dlsym(dcmiHandle, "dcmi_set_destroy_vdevice");

    dcmi_get_device_logicid_from_phyid_func = dlsym(dcmiHandle, "dcmi_get_device_logicid_from_phyid");

    return SUCCESS;
}

int dcmiShutDown(void)
{
    if (dcmiHandle == NULL) {
        return SUCCESS;
    }
    return (dlclose(dcmiHandle) ? ERROR_UNKNOWN : SUCCESS);
}

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */

#endif /* __DCMI_INTERFACE_API_H__ */
