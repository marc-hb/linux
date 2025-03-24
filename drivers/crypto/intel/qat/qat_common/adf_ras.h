/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2025 Intel Corporation */
#ifndef ADF_RAS_H_
#define ADF_RAS_H_

#define ADF_DEFAULT_UNCORRECTABLE_TIMER_PERIOD_MS	2000
#define ADF_DEFAULT_UNCORRECTABLE_THRESHOLD		10

int adf_ras_uncorrectable_timer_start(struct adf_accel_dev *accel_dev);
void adf_ras_uncorrectable_timer_stop(struct adf_accel_dev *accel_dev);

#endif /* ADF_RAS_H_ */
