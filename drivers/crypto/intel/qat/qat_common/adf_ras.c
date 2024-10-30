// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 Intel Corporation */
#include <linux/container_of.h>
#include <linux/jiffies.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "adf_common_drv.h"
#include "adf_ras.h"
#include "adf_sysfs_ras_counters.h"
#include "adf_timer.h"

static void ras_work_handler(struct work_struct *work)
{
	unsigned int curr_counter, counter;
	static unsigned int prev_counter;
	struct adf_accel_dev *accel_dev;
	unsigned int uncorr_error_timer;
	struct adf_timer *timer_ctx;
	unsigned long delay;

	timer_ctx = container_of(to_delayed_work(work), struct adf_timer, work_ctx);
	accel_dev = timer_ctx->accel_dev;
	curr_counter = ADF_RAS_ERR_CTR_READ(accel_dev->ras_errors, ADF_RAS_UNCORR);
	uncorr_error_timer = accel_dev->ras_errors.uncorr_error_timer;

	counter = curr_counter - prev_counter;
	prev_counter = curr_counter;
	if (counter > accel_dev->ras_errors.uncorr_error_threshold) {
		dev_err(&GET_DEV(accel_dev), "Fatal error, reset required\n");
		if (adf_notify_fatal_error(accel_dev))
			dev_err(&GET_DEV(accel_dev), "Failed to notify fatal error\n");
	} else {
		delay = msecs_to_jiffies(uncorr_error_timer);
		adf_misc_wq_queue_delayed_work(&timer_ctx->work_ctx, delay);
	}
}

int adf_ras_uncorrectable_timer_start(struct adf_accel_dev *accel_dev)
{
	struct adf_errors *ras_errors;
	struct adf_timer *timer_ctx;
	unsigned long delay;

	timer_ctx = kzalloc(sizeof(*timer_ctx), GFP_KERNEL);
	if (!timer_ctx)
		return -ENOMEM;

	timer_ctx->accel_dev = accel_dev;
	accel_dev->ras_timer = timer_ctx;
	ras_errors = &accel_dev->ras_errors;
	timer_ctx->initial_ktime = ktime_get_real();

	if (!ras_errors->uncorr_error_timer)
		ras_errors->uncorr_error_timer = ADF_DEFAULT_UNCORRECTABLE_TIMER_MS;

	if (!ras_errors->uncorr_error_threshold)
		ras_errors->uncorr_error_threshold = ADF_DEFAULT_UNCORRECTABLE_THRESHOLD;

	INIT_DELAYED_WORK(&timer_ctx->work_ctx, ras_work_handler);
	delay = msecs_to_jiffies(ras_errors->uncorr_error_timer);
	adf_misc_wq_queue_delayed_work(&timer_ctx->work_ctx, delay);

	return 0;
}
EXPORT_SYMBOL_GPL(adf_ras_uncorrectable_timer_start);

void adf_ras_uncorrectable_timer_stop(struct adf_accel_dev *accel_dev)
{
	struct adf_timer *timer_ctx = accel_dev->ras_timer;

	if (!timer_ctx)
		return;

	cancel_delayed_work_sync(&timer_ctx->work_ctx);

	kfree(timer_ctx);
	accel_dev->ras_timer = NULL;
}
EXPORT_SYMBOL_GPL(adf_ras_uncorrectable_timer_stop);
