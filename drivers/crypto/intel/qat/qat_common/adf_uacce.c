// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 Intel Corporation */

#define dev_fmt(fmt) "UACCE: " fmt

#include <linux/bitops.h>
#include <linux/iommu.h>
#include <linux/uacce.h>

#include "adf_accel_devices.h"
#include "adf_cfg.h"
#include "adf_cfg_common.h"
#include "adf_cfg_services.h"
#include "adf_cfg_strings.h"
#include "adf_common_drv.h"
#include "adf_ring_queue.h"
#include "adf_uacce.h"

#define ADF_UQ_MODE_DISABLE		0
#define ADF_UQ_MODE_POLLING		1

#define ADF_WQ_WINDOW_SIZE		0x2000
#define ADF_UQ_WINDOW_SIZE		0x1000

#define ADF_WQM_CSR_RINGMODECTL(bank)	(0x9000 + ((bank) << 2))
#define ADF_RINGMODECTL_ENABLE_UQ	BIT(0)

static struct service_hndl adf_uacce;

struct adf_uacce_bank_queue {
	struct adf_accel_dev *accel_dev;
	u32 bank_number;
};

struct adf_uacce_bank_data {
	enum adf_services svc_type;
	u32 ref_counter;
};

struct adf_uacce_pasid_hnode {
	enum adf_services last_provided_svc;
	struct hlist_node hnode;
	u32 assigned_banks_count;
	u32 pasid;
};

static int bank_set_uq_mode(void __iomem *csr_base_addr,
			    u32 bank_number, u8 mode)
{
	u32 val = 0;

	/* Set uq_enable */
	switch (mode) {
	case ADF_UQ_MODE_DISABLE:
		break;
	case ADF_UQ_MODE_POLLING:
		val |= ADF_RINGMODECTL_ENABLE_UQ;
		break;
	default:
		return -EFAULT;
	}

	ADF_CSR_WR(csr_base_addr,
		   ADF_WQM_CSR_RINGMODECTL(bank_number),
			val);

	return 0;
}

static int uq_do_set_mode(struct adf_accel_dev *accel_dev, u32 bank_number,
			  u8 mode)
{
	struct adf_hw_device_data *hw_data = accel_dev->hw_device;
	struct adf_bar *etr_bar = NULL;
	void __iomem *csr = NULL;

	if (bank_number >= hw_data->num_banks)
		return -EINVAL;

	etr_bar = &GET_BARS(accel_dev)[hw_data->get_etr_bar_id(hw_data)];
	if (!etr_bar)
		return -EFAULT;

	csr = etr_bar->virt_addr;
	if (!csr)
		return -EFAULT;

	return bank_set_uq_mode(csr, bank_number, mode);
}

static int adf_uacce_get_instances(struct uacce_device *uacce)
{
	return 0;
}

static bool is_bank_free(struct adf_accel_dev *accel_dev, u32 bank_number)
{
	struct adf_hw_device_data *hw_data = GET_HW_DATA(accel_dev);
	struct adf_hw_csr_ops *csr_ops = GET_CSR_OPS(accel_dev);
	struct adf_bar *etr_bar;
	u32 val;

	etr_bar = &GET_BARS(accel_dev)[hw_data->get_etr_bar_id(hw_data)];
	val = csr_ops->read_ring_pasid_value(etr_bar->virt_addr, bank_number);

	return val == 0;
}

static int enable_bank(struct adf_accel_dev *accel_dev, u32 bank_number,
		       u32 pasid)
{
	struct adf_hw_device_data *hw_data = GET_HW_DATA(accel_dev);
	struct adf_hw_csr_ops *csr_ops = GET_CSR_OPS(accel_dev);
	struct adf_bar *etr_bar;

	if (bank_number >= hw_data->num_banks)
		return -EINVAL;

	etr_bar = &GET_BARS(accel_dev)[hw_data->get_etr_bar_id(hw_data)];

	if (!etr_bar || !csr_ops->ring_pasid_enable)
		return -EFAULT;

	csr_ops->ring_pasid_enable(etr_bar->virt_addr, bank_number, true, false,
				   false, pasid);

	return 0;
}

static int disable_bank(struct adf_accel_dev *accel_dev, u32 bank_number)
{
	struct adf_hw_device_data *hw_data = GET_HW_DATA(accel_dev);
	struct adf_hw_csr_ops *csr_ops = GET_CSR_OPS(accel_dev);
	struct adf_bar *etr_bar;

	if (bank_number >= hw_data->num_banks)
		return -EINVAL;

	etr_bar = &GET_BARS(accel_dev)[hw_data->get_etr_bar_id(hw_data)];

	if (!etr_bar || !csr_ops->ring_pasid_disable)
		return -EFAULT;

	csr_ops->ring_pasid_disable(etr_bar->virt_addr, bank_number);

	return 0;
}

static int set_uq_mode(struct adf_accel_dev *accel_dev, u32 bank_number)
{
	int ret = 0;

	ret = uq_do_set_mode(accel_dev, bank_number, ADF_UQ_MODE_POLLING);
	if (ret) {
		dev_err(&GET_DEV(accel_dev),
			"Setting UQ mode in bank %u failed\n", bank_number);
		return ret;
	}

	ret = enable_bank(accel_dev, bank_number, IOMMU_NO_PASID);
	if (ret)
		dev_err(&GET_DEV(accel_dev),
			"Configuration of bank %u failed\n", bank_number);

	return ret;
}

static int set_wq_mode(struct adf_accel_dev *accel_dev, u32 bank_number,
		       u32 pasid)
{
	int ret = 0;

	if (!is_bank_free(accel_dev, bank_number)) {
		dev_warn(&GET_DEV(accel_dev),
			 "Bank %u already in use\n", bank_number);
		return -EBUSY;
	}

	ret = uq_do_set_mode(accel_dev, bank_number, ADF_UQ_MODE_DISABLE);
	if (ret) {
		dev_err(&GET_DEV(accel_dev),
			"Setting WQ mode in bank %u failed\n", bank_number);
		return ret;
	}

	ret = enable_bank(accel_dev, bank_number, pasid);
	if (ret)
		dev_err(&GET_DEV(accel_dev),
			"Configuration of bank %u failed\n", bank_number);

	return ret;
}

/**
 * next_svc_to_get - get the index of next service to provide
 * @svc_bitmask: bitmask with available services
 * @previous_svc: index of previous service provided
 *
 * This function essentially returns the position of the next set least
 * significant bit in the `svc_bitmask`, after the position provided
 * in `previous_svc`.
 *
 * If there are no more set bits after `previous_svc`, the function returns
 * the position of the first set least significant bit.
 *
 * Examples:
 * svc_bitmask = 0b00000001, previous_svc = 0, returns 0
 * svc_bitmask = 0b00000101, previous_svc = 0, returns 2
 * svc_bitmask = 0b00001010, previous_svc = 3, returns 1
 *
 * Return: index of the next service to provide
 */
static enum adf_services next_svc_to_get(u32 svc_bitmask,
					 enum adf_services previous_svc)
{
	u32 services_left;
	u32 mask;

	mask = (1U << (previous_svc + 1U)) - 1U;
	services_left = svc_bitmask & ~mask;

	if (services_left)
		return ffs(services_left) - 1;

	return ffs(svc_bitmask) - 1;
}

/**
 * queue_get_bank - determine the bank number that should be assigned next to
 * the given PASID
 * @accel_dev: pointer to the accelerator device
 * @pasid: PASID of the application that requests the bank
 *
 * This function tries to distribute different services each time the same
 * application requests a ring pair (bank). It does this by iterating over
 * a bitmask of available services and comparing it with the service that was
 * previously assigned. The order in which services are distributed corresponds
 * to the order they appear in `enum adf_services`.
 *
 * Function first establishes what type of service should be given next and then
 * iterates over the banks to find a free one with the that service enabled.
 * If there is no free bank with the given service EBUSY error is returned.
 *
 * Example:
 * The device has 3 services available: SYM, ASYM, and DC. If the previously
 * assigned service was ASYM, the function will try to assign DC next.
 *
 * If an application with the given PASID requests a bank for the first time,
 * the search starts with the first available service. In the example above,
 * it would be SYM.
 *
 * Return: bank number that should be assigned next to the given PASID
 * or EBUSY if no bank is available
 */
static int queue_get_bank(struct adf_accel_dev *accel_dev, u32 pasid)
{
	enum adf_services last_provided_svc = SVC_ID_COUNT;
	struct adf_uacce_bank_data *bank_data;
	struct adf_uacce_pasid_hnode *node;
	struct adf_uacce_data *uacce_data;
	enum adf_services svc_to_get;
	u32 num_banks;
	u32 i;

	uacce_data = &accel_dev->uacce_data;
	bank_data = uacce_data->bank_data;
	num_banks = GET_MAX_BANKS(accel_dev);

	hash_for_each_possible(uacce_data->pasid_ht, node, hnode, pasid) {
		if (node->pasid == pasid)
			last_provided_svc = node->last_provided_svc;
	}

	svc_to_get = next_svc_to_get(accel_dev->uacce_data.svc_bitmask,
				     last_provided_svc);

	for (i = 0; i < num_banks; i++) {
		if (bank_data[i].ref_counter != 0)
			continue;

		if (svc_to_get == bank_data[i].svc_type) {
			bank_data[i].ref_counter++;
			return i;
		}
	}

	return -EBUSY;
}

static int queue_put_bank(struct adf_uacce_bank_queue *bank_queue)
{
	struct adf_accel_dev *accel_dev = bank_queue->accel_dev;
	struct adf_uacce_bank_data *bank_data;
	int ret = 0;

	bank_data = accel_dev->uacce_data.bank_data;

	if (bank_queue->bank_number >= GET_MAX_BANKS(accel_dev))
		return -EINVAL;

	if (bank_data[bank_queue->bank_number].ref_counter <= 0)
		return -EFAULT;

	bank_data[bank_queue->bank_number].ref_counter--;

	return ret;
}

static int pasid_ht_add_bank(struct adf_accel_dev *accel_dev, u32 pasid,
			     int added_bank_number)
{
	struct adf_uacce_pasid_hnode *node;
	struct adf_uacce_data *uacce_data;
	enum adf_services svc_type;

	uacce_data = &accel_dev->uacce_data;

	svc_type = accel_dev->uacce_data.bank_data[added_bank_number].svc_type;

	// Check if pasid is already in hashtable
	hash_for_each_possible(uacce_data->pasid_ht, node, hnode, pasid) {
		if (node->pasid == pasid) {
			node->assigned_banks_count++;
			node->last_provided_svc = svc_type;
			return 0;
		}
	}

	// At this point we know the pasid is not in hashtable
	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	node->pasid = pasid;
	node->assigned_banks_count = 1;
	node->last_provided_svc = svc_type;

	hash_add(uacce_data->pasid_ht, &node->hnode, pasid);

	return 0;
}

static int pasid_ht_del_bank(struct adf_accel_dev *accel_dev, u32 pasid)
{
	struct adf_uacce_pasid_hnode *node;
	struct adf_uacce_data *uacce_data;

	uacce_data = &accel_dev->uacce_data;

	hash_for_each_possible(uacce_data->pasid_ht, node, hnode, pasid) {
		if (node->pasid == pasid) {
			node->assigned_banks_count--;
			break;
		}
	}

	if (!node) {
		dev_err(&GET_DEV(accel_dev), "Pasid %u not found in hashtable\n",
			pasid);
		return -EFAULT;
	}

	if (node->assigned_banks_count == 0) {
		hash_del(&node->hnode);
		kfree(node);
	}

	return 0;
}

static int pasid_ht_free(struct adf_accel_dev *accel_dev)
{
	struct adf_uacce_pasid_hnode *node;
	struct adf_uacce_data *uacce_data;
	int i;

	uacce_data = &accel_dev->uacce_data;

	hash_for_each(uacce_data->pasid_ht, i, node, hnode) {
		hash_del(&node->hnode);
		kfree(node);
	}

	return 0;
}

static int bank_data_init(struct adf_accel_dev *accel_dev)
{
	u32 bundle_size = GET_HW_DATA(accel_dev)->num_banks_per_vf;
	u32 num_banks = GET_MAX_BANKS(accel_dev);
	enum adf_services svc_conv;
	enum adf_cfg_service_type svc;
	size_t size;
	u32 i, j;

	if (num_banks == 0 || bundle_size == 0)
		return -EINVAL;

	size = sizeof(*accel_dev->uacce_data.bank_data);
	accel_dev->uacce_data.bank_data = kcalloc(num_banks, size, GFP_KERNEL);

	if (!accel_dev->uacce_data.bank_data)
		return -ENOMEM;

	for (i = 0; i < bundle_size; i++) {
		svc = GET_SRV_TYPE(accel_dev, i);

		switch (svc) {
		case COMP:
			svc_conv = SVC_ID_DC;
			break;
		case SYM:
			svc_conv = SVC_ID_SYM;
			break;
		case ASYM:
			svc_conv = SVC_ID_ASYM;
			break;
		case DECOMP:
			svc_conv = SVC_ID_DECOMP;
			break;
		case UNUSED:
			svc_conv = SVC_ID_COUNT;
			break;
		default: // USED
			return -EINVAL;
		}

		for (j = i; j < num_banks; j += bundle_size)
			accel_dev->uacce_data.bank_data[j].svc_type = svc_conv;
	}

	return 0;
}

static int adf_uacce_get_queue(struct uacce_device *uacce, unsigned long arg,
			       struct uacce_queue *q)
{
	struct adf_uacce_bank_queue *bank_queue;
	struct adf_accel_dev *accel_dev;
	u32 ref_counter;
	int bank_number;
	int ret = 0;

	accel_dev = uacce->priv;
	if (!accel_dev) {
		pr_err("Queue does not have a valid accel_dev pointer");
		return -EFAULT;
	}

	bank_queue = kmalloc(sizeof(*bank_queue), GFP_KERNEL);
	if (!bank_queue)
		return -ENOMEM;

	bank_number = queue_get_bank(accel_dev, q->pasid);
	if (bank_number < 0) {
		dev_err(&GET_DEV(accel_dev), "Error getting bank\n");
		ret = bank_number;
		goto err_free;
	}

	bank_queue->accel_dev = accel_dev;
	bank_queue->bank_number = bank_number;
	q->priv = bank_queue;

	ref_counter = accel_dev->uacce_data.bank_data[bank_number].ref_counter;

	// Enable bank only if it was not used before
	if (ref_counter == 1) {
		if (accel_dev->ring_queue_mode == ADF_RING_QUEUE_UQ)
			ret = set_uq_mode(accel_dev, bank_number);
		else
			ret = set_wq_mode(accel_dev, bank_number, q->pasid);

		if (ret) {
			dev_err(&GET_DEV(accel_dev), "Error setting mode\n");
			goto err_free;
		}
	}

	ret = pasid_ht_add_bank(accel_dev, q->pasid, bank_number);
	if (ret) {
		dev_err(&GET_DEV(accel_dev), "Error updating hashtable\n");
		goto err_free;
	}

	return ret;

err_free:
	kfree(bank_queue);
	return ret;
}

static void adf_uacce_put_queue(struct uacce_queue *q)
{
	struct adf_uacce_bank_queue *bank_queue = q->priv;
	struct adf_uacce_bank_data *bank_data;
	struct adf_accel_dev *accel_dev;
	u32 bank_number;
	int ret = 0;

	if (!bank_queue)
		return;

	accel_dev = bank_queue->accel_dev;
	bank_data = accel_dev->uacce_data.bank_data;
	bank_number = bank_queue->bank_number;

	if (queue_put_bank(bank_queue)) {
		dev_err(&GET_DEV(accel_dev), "Cannot put bank %u\n", bank_number);
		goto err_free;
	}

	// Disable bank only if it is not used by any other queue
	if (bank_data[bank_number].ref_counter == 0) {
		ret = disable_bank(accel_dev, bank_number);
		if (ret)
			dev_err(&GET_DEV(accel_dev), "Cannot disable bank %u\n",
				bank_number);
	}

	ret = pasid_ht_del_bank(accel_dev, q->pasid);
	if (ret)
		dev_err(&GET_DEV(accel_dev), "Error during pasid hashtable del\n");

err_free:
	kfree(bank_queue);
}

static int adf_uacce_start_queue(struct uacce_queue *q)
{
	return 0;
}

static void adf_uacce_stop_queue(struct uacce_queue *q)
{
}

static int adf_uacce_is_q_updated(struct uacce_queue *q)
{
	return 0;
}

static int adf_uacce_mmap(struct uacce_queue *q, struct vm_area_struct *vma,
			  struct uacce_qfile_region *qfr)
{
	struct adf_uacce_bank_queue *bank_queue = q->priv;
	size_t sz = vma->vm_end - vma->vm_start;
	struct adf_hw_device_data *hw_device;
	struct adf_accel_dev *accel_dev;
	resource_size_t base_addr = 0;
	size_t max_window_size;
	int ret;

	if (!bank_queue) {
		pr_err("Bank queue is not initialized");
		return -EFAULT;
	}

	accel_dev = bank_queue->accel_dev;
	if (!accel_dev) {
		pr_err("Queue does not have a valid accel_dev pointer");
		return -EFAULT;
	}

	hw_device = GET_HW_DATA(accel_dev);
	if (!hw_device->get_ring_base_addr)
		return -EFAULT;

	if (accel_dev->ring_queue_mode == ADF_RING_QUEUE_UQ)
		max_window_size = ADF_UQ_WINDOW_SIZE;
	else
		max_window_size = ADF_WQ_WINDOW_SIZE;

	if (sz > max_window_size) {
		dev_warn(&GET_DEV(accel_dev),
			 "Requested mmap size is invalid\n");
		return -EINVAL;
	}

	ret = hw_device->get_ring_base_addr(accel_dev, &base_addr,
					    bank_queue->bank_number,
					    accel_dev->ring_queue_mode);
	if (ret) {
		dev_err(&GET_DEV(accel_dev),
			"Getting ring base address failed\n");
		return ret;
	}

	return remap_pfn_range(vma, vma->vm_start, base_addr >> PAGE_SHIFT, sz,
			       pgprot_noncached(vma->vm_page_prot));
}

static long adf_uacce_ioctl(struct uacce_queue *q, unsigned int cmd,
			    unsigned long arg)
{
	return 0;
}

static enum uacce_dev_state
adf_uacce_get_isolate_state(struct uacce_device *uacce)
{
	return UACCE_DEV_NORMAL;
}

static int adf_uacce_isolate_err_threshold_write(struct uacce_device *uacce,
						 u32 num)
{
	return 0;
}

static u32 adf_uacce_isolate_err_threshold_read(struct uacce_device *uacce)
{
	return 0;
}

static const struct uacce_ops adf_uacce_ops = {
	.get_available_instances = adf_uacce_get_instances,
	.get_queue = adf_uacce_get_queue,
	.put_queue = adf_uacce_put_queue,
	.start_queue = adf_uacce_start_queue,
	.stop_queue = adf_uacce_stop_queue,
	.is_q_updated = adf_uacce_is_q_updated,
	.mmap = adf_uacce_mmap,
	.ioctl = adf_uacce_ioctl,
	.get_isolate_state = adf_uacce_get_isolate_state,
	.isolate_err_threshold_write = adf_uacce_isolate_err_threshold_write,
	.isolate_err_threshold_read = adf_uacce_isolate_err_threshold_read,
};

static int adf_uacce_shutdown(struct adf_accel_dev *accel_dev)
{
	if (!accel_dev->uacce_data.uacce_dev)
		return 0;

	uacce_remove(accel_dev->uacce_data.uacce_dev);
	accel_dev->uacce_data.uacce_dev = NULL;

	kfree(accel_dev->uacce_data.bank_data);
	accel_dev->uacce_data.bank_data = NULL;

	pasid_ht_free(accel_dev);

	return 0;
}

static int adf_uacce_init(struct adf_accel_dev *accel_dev)
{
	struct device *dev = &GET_DEV(accel_dev);
	char val[ADF_CFG_MAX_VAL_LEN_IN_BYTES] = { };
	struct uacce_interface interface = {
		.flags = UACCE_DEV_SVA,
		.ops = &adf_uacce_ops,
	};
	unsigned long num_inst_cy = 0;
	unsigned long num_inst_dc = 0;
	struct uacce_device *uacce;
	int ret;

	if (!adf_uacce_is_enabled(accel_dev))
		return 0;

	ret = adf_cfg_get_param_value(accel_dev, ADF_KERNEL_SEC, ADF_NUM_CY, val);
	if (!ret) {
		ret = kstrtoul(val, 0, &num_inst_cy);
		if (ret)
			return ret;
	}

	ret = adf_cfg_get_param_value(accel_dev, ADF_KERNEL_SEC, ADF_NUM_DC, val);
	if (!ret) {
		ret = kstrtoul(val, 0, &num_inst_dc);
		if (ret)
			return ret;
	}

	if (num_inst_cy || num_inst_dc) {
		dev_warn(dev,
			 "LKCF instances are enabled (CY: %lu, DC: %lu), skipping UACCE init\n",
			 num_inst_cy, num_inst_dc);
		return 0;
	}

	ret = strscpy(interface.name, dev_driver_string(dev),
		      sizeof(interface.name));
	if (ret < 0)
		return -ENAMETOOLONG;

	uacce = uacce_alloc(dev, &interface);
	if (IS_ERR(uacce))
		return PTR_ERR(uacce);

	accel_dev->uacce_data.uacce_dev = uacce;

	uacce->is_vf = accel_dev->is_vf;
	uacce->priv = accel_dev;

	uacce->qf_pg_num[UACCE_QFRT_MMIO] = 1;
	uacce->qf_pg_num[UACCE_QFRT_DUS] = 1;

	ret = uacce_register(uacce);
	if (ret) {
		uacce_remove(uacce);
		return ret;
	}

	ret = adf_ring_queue_enable(accel_dev);
	if (ret) {
		dev_err(dev, "Cannot enable ring queue\n");
		uacce_remove(uacce);
	}

	ret = bank_data_init(accel_dev);
	if (ret)
		goto err_shutdown;

	ret = adf_get_service_enabled(accel_dev,
				      &accel_dev->uacce_data.svc_bitmask);
	if (ret)
		goto err_shutdown;

	hash_init(accel_dev->uacce_data.pasid_ht);

	return ret;

err_shutdown:
	adf_uacce_shutdown(accel_dev);
	return ret;
}

static int adf_uacce_event_handler(struct adf_accel_dev *accel_dev,
				   enum adf_event event)
{
	int ret = -EINVAL;

	switch (event) {
	case ADF_EVENT_INIT:
		ret = adf_uacce_init(accel_dev);
		break;
	case ADF_EVENT_SHUTDOWN:
		ret = adf_uacce_shutdown(accel_dev);
		break;
	case ADF_EVENT_RESTARTING:
	case ADF_EVENT_RESTARTED:
	case ADF_EVENT_START:
		ret = 0;
		break;
	case ADF_EVENT_STOP:
		ret = 0;
		break;
	default:
		ret = 0;
	}
	return ret;
}

int adf_uacce_register(void)
{
	memset(&adf_uacce, 0, sizeof(adf_uacce));
	adf_uacce.event_hld = adf_uacce_event_handler;
	adf_uacce.name = "qat_uacce";
	return adf_service_register(&adf_uacce);
}

int adf_uacce_unregister(void)
{
	return adf_service_unregister(&adf_uacce);
}

bool adf_uacce_is_enabled(struct adf_accel_dev *accel_dev)
{
	char val[ADF_CFG_MAX_VAL_LEN_IN_BYTES] = { };
	bool enabled = false;
	int ret = 0;
	int phrase_ret = 0;

	ret = adf_cfg_get_param_value(accel_dev, ADF_GENERAL_SEC, ADF_UACCE_ENABLED, val);
	if (!ret)
		phrase_ret = kstrtobool(val, &enabled);

	if (phrase_ret) {
		dev_err(&GET_DEV(accel_dev),
			"Value in (%s) config entry is not recognized\n",
			ADF_UACCE_ENABLED);
		return false;
	}

	return enabled;
}

int adf_uacce_enable(struct adf_accel_dev *accel_dev)
{
	unsigned long val = 1;
	int ret_cfg = 0;
	int ret = 0;

	ret = iommu_dev_enable_feature(&GET_DEV(accel_dev), IOMMU_DEV_FEAT_IOPF);
	if (ret)
		goto err_iopf;

	ret = iommu_dev_enable_feature(&GET_DEV(accel_dev), IOMMU_DEV_FEAT_SVA);
	if (ret)
		goto err_svm;

	ret_cfg = adf_cfg_add_key_value_param(accel_dev, ADF_GENERAL_SEC,
					      ADF_UACCE_ENABLED, (void *)&val,
					      ADF_DEC);
	if (ret_cfg)
		dev_err(&GET_DEV(accel_dev),
			"Config entry (%s) cannot be added\n", ADF_UACCE_ENABLED);

	iommu_dev_disable_feature(&GET_DEV(accel_dev), IOMMU_DEV_FEAT_SVA);
err_svm:
	iommu_dev_disable_feature(&GET_DEV(accel_dev), IOMMU_DEV_FEAT_IOPF);
err_iopf:
	if (ret)
		dev_err(&GET_DEV(accel_dev),
			"SVM not supported! UACCE mode cannot be enabled\n");

	return ret || ret_cfg;
}

void adf_uacce_disable(struct adf_accel_dev *accel_dev)
{
	adf_cfg_del_key_value_param(accel_dev, ADF_GENERAL_SEC,
				    ADF_UACCE_ENABLED);
	adf_ring_queue_disable_uq(accel_dev);
}
