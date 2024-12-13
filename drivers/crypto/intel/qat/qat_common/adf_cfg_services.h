/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2023 Intel Corporation */
#ifndef _ADF_CFG_SERVICES_H_
#define _ADF_CFG_SERVICES_H_

#include "adf_cfg_strings.h"

struct adf_accel_dev;

#define MAX_NUM_CONCURR_SVC 2

enum adf_services {
	SVC_ID_SYM = 0,
	SVC_ID_ASYM,
	SVC_ID_DC,
	SVC_ID_DCC,
	SVC_ID_COUNT
};

#define SVC_SYM		BIT(SVC_ID_SYM)
#define SVC_ASYM	BIT(SVC_ID_ASYM)
#define SVC_DC		BIT(SVC_ID_DC)
#define SVC_DCC		BIT(SVC_ID_DCC)

extern const char *const adf_cfg_services[SVC_ID_COUNT];

int adf_get_service_enabled(struct adf_accel_dev *accel_dev, u32 *mask);
int adf_service_string_to_mask(struct adf_accel_dev *accel_dev,
			       const char *buf, size_t count, u32 *out_mask);
int adf_service_mask_to_string(u32 in_mask, char *buf, size_t count);

#endif
