// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2022 Intel Corporation. */

#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/semaphore.h>
#include <linux/slab.h>

#include "ifs.h"

/*
 * Protects against simultaneous tests on multiple cores, or
 * reloading can file while a test is in progress
 */
static DEFINE_SEMAPHORE(ifs_sem, 1);

/*
 * The sysfs interface to check additional details of last test
 * cat /sys/devices/system/platform/ifs/details
 */
static ssize_t details_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	struct ifs_data *ifsd = ifs_get_data(dev);

	return sysfs_emit(buf, "%#llx\n", ifsd->scan_details);
}

static DEVICE_ATTR_RO(details);

static const char * const status_msg[] = {
	[SCAN_NOT_TESTED] = "untested",
	[SCAN_TEST_PASS] = "pass",
	[SCAN_TEST_FAIL] = "fail"
};

/*
 * The sysfs interface to check the test status:
 * To check the status of last test
 * cat /sys/devices/platform/ifs/status
 */
static ssize_t status_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	struct ifs_data *ifsd = ifs_get_data(dev);

	return sysfs_emit(buf, "%s\n", status_msg[ifsd->status]);
}

static DEVICE_ATTR_RO(status);

/*
 * The sysfs interface for single core testing
 * To start test, for example, cpu5
 * echo 5 > /sys/devices/platform/ifs/run_test
 * To check the result:
 * cat /sys/devices/platform/ifs/result
 * The sibling core gets tested at the same time.
 */
static ssize_t run_test_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	unsigned int cpu;
	int rc;

	rc = kstrtouint(buf, 0, &cpu);
	if (rc < 0 || cpu >= nr_cpu_ids)
		return -EINVAL;

	if (down_interruptible(&ifs_sem))
		return -EINTR;

	rc = do_core_test(cpu, dev);

	up(&ifs_sem);

	return rc ? rc : count;
}

static DEVICE_ATTR_WO(run_test);

static ssize_t current_batch_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct ifs_data *ifsd = ifs_get_data(dev);
	unsigned int cur_batch;
	int rc;

	rc = kstrtouint(buf, 0, &cur_batch);
	if (rc < 0 || cur_batch > 0xff)
		return -EINVAL;

	if (down_interruptible(&ifs_sem))
		return -EINTR;

	ifsd->cur_batch = cur_batch;

	rc = ifs_load_firmware(dev);

	up(&ifs_sem);

	return (rc == 0) ? count : rc;
}

static ssize_t current_batch_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct ifs_data *ifsd = ifs_get_data(dev);

	if (!ifsd->loaded)
		return sysfs_emit(buf, "none\n");
	else
		return sysfs_emit(buf, "0x%02x\n", ifsd->cur_batch);
}

static DEVICE_ATTR_RW(current_batch);

/*
 * Display currently loaded IFS image version.
 */
static ssize_t image_version_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct ifs_data *ifsd = ifs_get_data(dev);

	if (!ifsd->loaded)
		return sysfs_emit(buf, "%s\n", "none");
	else
		return sysfs_emit(buf, "%#x\n", ifsd->loaded_version);
}

static DEVICE_ATTR_RO(image_version);

static ssize_t status_list_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ifs_data *ifsd = ifs_get_data(dev);
	struct ifs_test_output *pcpu_state;
	int lcpu, len = 0;

	if (cpumask_weight(&ifsd->grp_cpumask) == 0)
		return sysfs_emit(buf, "%s\n", "none");

	for_each_cpu(lcpu, &ifsd->grp_cpumask) {
		pcpu_state = per_cpu_ptr(ifsd->result_ptr, lcpu);
		len += sysfs_emit_at(buf, len, "%s%s", (len > 0) ? " " : "",
				     status_msg[pcpu_state->test_result]);
	}
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}

static DEVICE_ATTR_RO(status_list);

static ssize_t details_list_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ifs_data *ifsd = ifs_get_data(dev);
	struct ifs_test_output *pcpu_state;
	int lcpu, len = 0;

	if (cpumask_weight(&ifsd->grp_cpumask) == 0)
		return sysfs_emit(buf, "%s\n", "none");

	for_each_cpu(lcpu, &ifsd->grp_cpumask) {
		pcpu_state = per_cpu_ptr(ifsd->result_ptr, lcpu);
		len += sysfs_emit_at(buf, len, "%s%#llx", (len > 0) ? " " : "",
				     pcpu_state->test_details);
	}
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}

static DEVICE_ATTR_RO(details_list);

static ssize_t cpus_ran_list_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ifs_data *ifsd = ifs_get_data(dev);

	if (cpumask_weight(&ifsd->grp_cpumask) == 0)
		return sysfs_emit(buf, "%s\n", "none");

	return sysfs_emit(buf, "%*pbl\n", cpumask_pr_args(&ifsd->grp_cpumask));
}

static DEVICE_ATTR_RO(cpus_ran_list);
/* global scan sysfs attributes */
struct attribute *plat_ifs_attrs[] = {
	&dev_attr_details.attr,
	&dev_attr_status.attr,
	&dev_attr_run_test.attr,
	&dev_attr_current_batch.attr,
	&dev_attr_image_version.attr,
	&dev_attr_status_list.attr,
	&dev_attr_details_list.attr,
	&dev_attr_cpus_ran_list.attr,
	NULL
};

/* global array sysfs attributes */
struct attribute *plat_ifs_array_attrs[] = {
	&dev_attr_details.attr,
	&dev_attr_status.attr,
	&dev_attr_run_test.attr,
	NULL
};
