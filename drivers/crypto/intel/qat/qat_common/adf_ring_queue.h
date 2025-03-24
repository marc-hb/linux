/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2025 Intel Corporation */

#ifndef ADF_RING_QUEUE_H
#define ADF_RING_QUEUE_H

struct adf_accel_dev;

enum adf_ring_queue_mode {
	ADF_RING_QUEUE_WQ,
	ADF_RING_QUEUE_UQ,
};

int adf_ring_queue_set_mode(struct adf_accel_dev *accel_dev,
			    enum adf_ring_queue_mode ring_queue_mode);
int adf_ring_queue_enable_uq(struct adf_accel_dev *accel_dev);
int adf_ring_queue_disable_uq(struct adf_accel_dev *accel_dev);
int adf_ring_queue_enable(struct adf_accel_dev *accel_dev);
int adf_ring_queue_get_cfg_mode(struct adf_accel_dev *accel_dev,
				enum adf_ring_queue_mode *config_mode);

#endif /* ADF_RING_QUEUE_H */
