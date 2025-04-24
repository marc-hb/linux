// SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0-only)
/* Copyright(c) 2022 Intel Corporation */
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include "adf_accel_devices.h"
#include "adf_cfg.h"
#include "adf_cfg_services.h"
#include "adf_common_drv.h"
#include "adf_ring_queue.h"
#include "adf_uacce.h"
#include "icp_qat_hw.h"

#define UNSET_RING_NUM -1

static const char * const state_operations[] = {
	[DEV_DOWN] = "down",
	[DEV_UP] = "up",
};

static const char *const adf_cfg_ring_queue_modes[] = {
	[ADF_RING_QUEUE_WQ] = ADF_CFG_RING_QUEUE_WQ,
	[ADF_RING_QUEUE_UQ] = ADF_CFG_RING_QUEUE_UQ
};

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct adf_accel_dev *accel_dev;
	char *state;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	state = adf_dev_started(accel_dev) ? "up" : "down";
	return sysfs_emit(buf, "%s\n", state);
}

static ssize_t state_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct adf_accel_dev *accel_dev;
	u32 accel_id;
	int ret;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	accel_id = accel_dev->accel_id;

	if (adf_devmgr_in_reset(accel_dev) || adf_dev_in_use(accel_dev)) {
		dev_info(dev, "Device qat_dev%d is busy\n", accel_id);
		return -EBUSY;
	}

	ret = sysfs_match_string(state_operations, buf);
	if (ret < 0)
		return ret;

	switch (ret) {
	case DEV_DOWN:
		dev_info(dev, "Stopping device qat_dev%d\n", accel_id);

		if (!adf_dev_started(accel_dev)) {
			dev_info(&GET_DEV(accel_dev), "Device qat_dev%d already down\n",
				 accel_id);

			break;
		}

		ret = adf_dev_down(accel_dev);
		if (ret)
			return ret;

		break;
	case DEV_UP:
		dev_info(dev, "Starting device qat_dev%d\n", accel_id);

		ret = adf_dev_up(accel_dev, true);
		if (ret == -EALREADY) {
			break;
		} else if (ret) {
			dev_err(dev, "Failed to start device qat_dev%d\n",
				accel_id);
			adf_dev_down(accel_dev);
			return ret;
		}
		break;
	default:
		return -EINVAL;
	}

	return count;
}

static ssize_t cfg_services_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	char services[ADF_CFG_MAX_VAL_LEN_IN_BYTES] = {0};
	struct adf_accel_dev *accel_dev;
	int ret;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	ret = adf_cfg_get_param_value(accel_dev, ADF_GENERAL_SEC,
				      ADF_SERVICES_ENABLED, services);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%s\n", services);
}

static int adf_sysfs_update_dev_config(struct adf_accel_dev *accel_dev,
				       const char *services)
{
	return adf_cfg_add_key_value_param(accel_dev, ADF_GENERAL_SEC,
					   ADF_SERVICES_ENABLED, services,
					   ADF_STR);
}

static ssize_t cfg_services_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	char services[ADF_CFG_MAX_VAL_LEN_IN_BYTES] = { };
	struct adf_hw_device_data *hw_data;
	struct adf_accel_dev *accel_dev;
	int ret;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	ret = adf_parse_service_string(accel_dev, buf, count, services,
				       ADF_CFG_MAX_VAL_LEN_IN_BYTES);
	if (ret)
		return ret;

	if (adf_dev_started(accel_dev)) {
		dev_info(dev, "Device qat_dev%d must be down to reconfigure the service.\n",
			 accel_dev->accel_id);
		return -EINVAL;
	}

	ret = adf_sysfs_update_dev_config(accel_dev, services);
	if (ret < 0)
		return ret;

	hw_data = GET_HW_DATA(accel_dev);

	/* Update capabilities mask after change in configuration.
	 * A call to this function is required as capabilities are, at the
	 * moment, tied to configuration
	 */
	hw_data->accel_capabilities_mask = hw_data->get_accel_cap(accel_dev);
	if (!hw_data->accel_capabilities_mask)
		return -EINVAL;

	return count;
}

static ssize_t pm_idle_enabled_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	char pm_idle_enabled[ADF_CFG_MAX_VAL_LEN_IN_BYTES] = {};
	struct adf_accel_dev *accel_dev;
	int ret;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	ret = adf_cfg_get_param_value(accel_dev, ADF_GENERAL_SEC,
				      ADF_PM_IDLE_SUPPORT, pm_idle_enabled);
	if (ret)
		return sysfs_emit(buf, "1\n");

	return sysfs_emit(buf, "%s\n", pm_idle_enabled);
}

static ssize_t pm_idle_enabled_store(struct device *dev, struct device_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned long pm_idle_enabled_cfg_val;
	struct adf_accel_dev *accel_dev;
	bool pm_idle_enabled;
	int ret;

	ret = kstrtobool(buf, &pm_idle_enabled);
	if (ret)
		return ret;

	pm_idle_enabled_cfg_val = pm_idle_enabled;
	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	if (adf_dev_started(accel_dev)) {
		dev_info(dev, "Device qat_dev%d must be down to set pm_idle_enabled.\n",
			 accel_dev->accel_id);
		return -EINVAL;
	}

	ret = adf_cfg_add_key_value_param(accel_dev, ADF_GENERAL_SEC,
					  ADF_PM_IDLE_SUPPORT, &pm_idle_enabled_cfg_val,
					  ADF_DEC);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(pm_idle_enabled);

static ssize_t auto_reset_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	char *auto_reset;
	struct adf_accel_dev *accel_dev;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	auto_reset = accel_dev->autoreset_on_error ? "on" : "off";

	return sysfs_emit(buf, "%s\n", auto_reset);
}

static ssize_t auto_reset_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct adf_accel_dev *accel_dev;
	bool enabled = false;
	int ret;

	ret = kstrtobool(buf, &enabled);
	if (ret)
		return ret;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	accel_dev->autoreset_on_error = enabled;

	return count;
}
static DEVICE_ATTR_RW(auto_reset);

static DEVICE_ATTR_RW(state);
static DEVICE_ATTR_RW(cfg_services);

static ssize_t rp2srv_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct adf_hw_device_data *hw_data;
	struct adf_accel_dev *accel_dev;
	enum adf_cfg_service_type svc;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	hw_data = GET_HW_DATA(accel_dev);

	if (accel_dev->sysfs.ring_num == UNSET_RING_NUM)
		return -EINVAL;

	down_read(&accel_dev->sysfs.lock);
	svc = GET_SRV_TYPE(accel_dev, accel_dev->sysfs.ring_num %
					      hw_data->num_banks_per_vf);
	up_read(&accel_dev->sysfs.lock);

	switch (svc) {
	case COMP:
		return sysfs_emit(buf, "%s\n", ADF_CFG_DC);
	case SYM:
		return sysfs_emit(buf, "%s\n", ADF_CFG_SYM);
	case ASYM:
		return sysfs_emit(buf, "%s\n", ADF_CFG_ASYM);
	case DECOMP:
		return sysfs_emit(buf, "%s\n", ADF_CFG_DECOMP);
	default:
		break;
	}
	return -EINVAL;
}

static ssize_t rp2srv_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct adf_accel_dev *accel_dev;
	int num_rings, ret;
	unsigned int ring;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	ret = kstrtouint(buf, 10, &ring);
	if (ret)
		return ret;

	num_rings = GET_MAX_BANKS(accel_dev);
	if (ring >= num_rings) {
		dev_err(&GET_DEV(accel_dev),
			"Device does not support more than %u ring pairs\n",
			num_rings);
		return -EINVAL;
	}

	down_write(&accel_dev->sysfs.lock);
	accel_dev->sysfs.ring_num = ring;
	up_write(&accel_dev->sysfs.lock);

	return count;
}
static DEVICE_ATTR_RW(rp2srv);

static ssize_t num_rps_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct adf_accel_dev *accel_dev;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	return sysfs_emit(buf, "%u\n", GET_MAX_BANKS(accel_dev));
}
static DEVICE_ATTR_RO(num_rps);

static ssize_t num_rps_per_vf_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct adf_accel_dev *accel_dev;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	return sysfs_emit(buf, "%u\n", GET_NUM_BANKS_PER_VF(accel_dev));
}
static DEVICE_ATTR_RO(num_rps_per_vf);

static ssize_t dev_event_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct adf_accel_dev *accel_dev;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	if (!accel_dev->uacce_data.last_event)
		return -EINVAL;

	atomic_inc(&accel_dev->uacce_data.last_event_read_cnt);

	return sysfs_emit(buf, "%s\n", accel_dev->uacce_data.last_event);
}

void adf_sysfs_dev_event_notify(struct adf_accel_dev *accel_dev)
{
	dev_dbg(&GET_DEV(accel_dev), "QAT dev event: %s\n", accel_dev->uacce_data.last_event);
	sysfs_notify(&GET_DEV(accel_dev).kobj, "qat", "dev_event");
}
static DEVICE_ATTR_RO(dev_event);

static ssize_t uacce_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	char *uacce_enabled;
	struct adf_accel_dev *accel_dev;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	uacce_enabled = adf_uacce_is_enabled(accel_dev) ? "on" : "off";

	return sysfs_emit(buf, "%s\n", uacce_enabled);
}

static ssize_t uacce_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct adf_accel_dev *accel_dev;
	bool uacce_enabled = false;
	int ret;

	ret = kstrtobool(buf, &uacce_enabled);
	if (ret)
		return ret;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	if (adf_dev_started(accel_dev)) {
		dev_warn(dev,
			 "Device qat_dev%d must be down to control uacce enablement.\n",
			 accel_dev->accel_id);
		return -EINVAL;
	}

	if (uacce_enabled == adf_uacce_is_enabled(accel_dev))
		return count;

	if (uacce_enabled) {
		ret = adf_uacce_enable(accel_dev);
		if (ret) {
			dev_err(dev, "Enablement of the uacce failed: %d", ret);
			return ret;
		}

		ret = adf_ring_queue_enable_uq(accel_dev);
		if (ret) {
			dev_err(dev,
				"Setting ring queue mode to UQ failed: %d, uacce cannot be enabled",
				ret);
			adf_uacce_disable(accel_dev);
			return ret;
		}
	} else {
		adf_uacce_disable(accel_dev);
	}

	return count;
}
static DEVICE_ATTR_RW(uacce);

static ssize_t ring_queue_mode_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	enum adf_ring_queue_mode ring_queue_mode;
	struct adf_accel_dev *accel_dev;
	int ret;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	if (!adf_uacce_is_enabled(accel_dev))
		return sysfs_emit(buf, "unsupported\n");

	ret = adf_ring_queue_get_cfg_mode(accel_dev, &ring_queue_mode);
	if (ret) {
		dev_err(dev,
			"Cannot read ring_queue_mode from config\n");
		return ret;
	}

	return sysfs_emit(buf, "%s\n", adf_cfg_ring_queue_modes[ring_queue_mode]);
}

static ssize_t ring_queue_mode_store(struct device *dev, struct device_attribute *attr,
				     const char *buf, size_t count)
{
	enum adf_ring_queue_mode ring_queue_mode;
	struct adf_accel_dev *accel_dev;
	int ret;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	if (adf_dev_started(accel_dev)) {
		dev_warn(dev,
			 "Device qat_dev%d must be down to set ring_queue_mode.\n",
			 accel_dev->accel_id);
		return -EINVAL;
	}

	if (!adf_uacce_is_enabled(accel_dev)) {
		dev_warn(dev,
			 "uacce must be enabled on device qat_dev%d to set ring_queue_mode.\n",
			 accel_dev->accel_id);
		return -EINVAL;
	}

	ret = sysfs_match_string(adf_cfg_ring_queue_modes, buf);
	if (ret < 0)
		return ret;

	ring_queue_mode = ret;

	ret = adf_ring_queue_set_mode(accel_dev, ring_queue_mode);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(ring_queue_mode);

static ssize_t caps_dev_show(struct device *dev, struct device_attribute *dev_attr,
			     char *buf)
{
	struct adf_accel_dev *accel_dev;
	u32 capabilities_mask;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	capabilities_mask = GET_HW_DATA(accel_dev)->accel_capabilities_mask;

	return sysfs_emit(buf, "%#08x\n", capabilities_mask);
}
static DEVICE_ATTR_RO(caps_dev);

static ssize_t caps_dev_dc_ext_show(struct device *dev, struct device_attribute *dev_attr,
				    char *buf)
{
	struct adf_accel_dev *accel_dev;
	u32 extended_dc_capabilities;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	extended_dc_capabilities = GET_HW_DATA(accel_dev)->extended_dc_capabilities;

	return sysfs_emit(buf, "%#08x\n", extended_dc_capabilities);
}
static DEVICE_ATTR_RO(caps_dev_dc_ext);

static ssize_t caps_comp_algos_show(struct device *dev, struct device_attribute *dev_attr,
				    char *buf)
{
	struct adf_accel_dev *accel_dev;
	u16 comp_algos;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	comp_algos = GET_HW_DATA(accel_dev)->fw_dc_caps.comp_algos;

	return sysfs_emit(buf, "%#08x\n", comp_algos);
}
static DEVICE_ATTR_RO(caps_comp_algos);

static ssize_t caps_cksum_algos_show(struct device *dev, struct device_attribute *dev_attr,
				     char *buf)
{
	struct adf_accel_dev *accel_dev;
	u16 cksum_algos;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	cksum_algos = GET_HW_DATA(accel_dev)->fw_dc_caps.cksum_algos;

	return sysfs_emit(buf, "%#08x\n", cksum_algos);
}
static DEVICE_ATTR_RO(caps_cksum_algos);

static ssize_t caps_deflate_show(struct device *dev, struct device_attribute *dev_attr,
				 char *buf)
{
	struct adf_accel_dev *accel_dev;
	u32 deflate_caps;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	deflate_caps = GET_HW_DATA(accel_dev)->fw_dc_caps.deflate_caps;

	return sysfs_emit(buf, "%#08x\n", deflate_caps);
}
static DEVICE_ATTR_RO(caps_deflate);

static ssize_t caps_lz4_show(struct device *dev, struct device_attribute *dev_attr,
			     char *buf)
{
	struct adf_accel_dev *accel_dev;
	u16 lz4_caps;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	lz4_caps = GET_HW_DATA(accel_dev)->fw_dc_caps.lz4_caps;

	return sysfs_emit(buf, "%#08x\n", lz4_caps);
}
static DEVICE_ATTR_RO(caps_lz4);

static ssize_t caps_lz4s_show(struct device *dev, struct device_attribute *dev_attr,
			      char *buf)
{
	struct adf_accel_dev *accel_dev;
	u16 lz4s_caps;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	lz4s_caps = GET_HW_DATA(accel_dev)->fw_dc_caps.lz4s_caps;

	return sysfs_emit(buf, "%#08x\n", lz4s_caps);
}
static DEVICE_ATTR_RO(caps_lz4s);

static ssize_t caps_zstd_show(struct device *dev, struct device_attribute *dev_attr,
			      char *buf)
{
	struct adf_accel_dev *accel_dev;
	u16 zstd_caps;

	accel_dev = adf_devmgr_pci_to_accel_dev(to_pci_dev(dev));
	if (!accel_dev)
		return -EINVAL;

	zstd_caps = GET_HW_DATA(accel_dev)->fw_dc_caps.zstd_caps;

	return sysfs_emit(buf, "%#08x\n", zstd_caps);
}
static DEVICE_ATTR_RO(caps_zstd);

static ssize_t caps_version_show(struct device *dev, struct device_attribute *dev_attr,
				 char *buf)
{
	u8 version_maj = QAT_CAPS_MAJOR;
	u8 version_min = QAT_CAPS_MINOR;

	return sysfs_emit(buf, "%u.%u\n", version_maj, version_min);
}
static DEVICE_ATTR_RO(caps_version);

static struct attribute *qat_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_cfg_services.attr,
	&dev_attr_pm_idle_enabled.attr,
	&dev_attr_rp2srv.attr,
	&dev_attr_num_rps.attr,
	&dev_attr_auto_reset.attr,
	&dev_attr_num_rps_per_vf.attr,
	&dev_attr_uacce.attr,
	&dev_attr_dev_event.attr,
	&dev_attr_ring_queue_mode.attr,
	&dev_attr_caps_cksum_algos.attr,
	&dev_attr_caps_comp_algos.attr,
	&dev_attr_caps_deflate.attr,
	&dev_attr_caps_dev.attr,
	&dev_attr_caps_dev_dc_ext.attr,
	&dev_attr_caps_lz4.attr,
	&dev_attr_caps_lz4s.attr,
	&dev_attr_caps_version.attr,
	&dev_attr_caps_zstd.attr,
	NULL,
};

static struct attribute_group qat_group = {
	.attrs = qat_attrs,
	.name = "qat",
};

int adf_sysfs_init(struct adf_accel_dev *accel_dev)
{
	int ret;

	ret = devm_device_add_group(&GET_DEV(accel_dev), &qat_group);
	if (ret) {
		dev_err(&GET_DEV(accel_dev),
			"Failed to create qat attribute group: %d\n", ret);
	}

	accel_dev->sysfs.ring_num = UNSET_RING_NUM;

	return ret;
}
EXPORT_SYMBOL_GPL(adf_sysfs_init);
