// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 Intel Corporation */
#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/types.h>

#include <adf_accel_devices.h>
#include <adf_cfg.h>
#include <adf_common_drv.h>
#include <adf_dbgfs.h>

#include "adf_gen6_shared.h"
#include "adf_6xxx_hw_data.h"
#include "adf_sysfs_kpt.h"

#define SRAM_BAR_INDEX		0
#define PMISC_BAR_INDEX		2
#define ETR_BAR_INDEX		4

static int bar_map[] = {
	SRAM_BAR_INDEX,
	PMISC_BAR_INDEX,
	ETR_BAR_INDEX
};

static void adf_cleanup_accel(struct adf_accel_dev *accel_dev)
{
	if (accel_dev->hw_device) {
		adf_clean_hw_data_6xxx(accel_dev->hw_device);
		accel_dev->hw_device = NULL;
	}
	adf_dbgfs_exit(accel_dev);
	adf_cfg_dev_remove(accel_dev);
	adf_devmgr_rm_dev(accel_dev, NULL);
}

static int adf_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct adf_accel_pci *accel_pci_dev;
	struct adf_hw_device_data *hw_data;
	struct device *dev = &pdev->dev;
	struct adf_accel_dev *accel_dev;
	struct adf_bar *bar;
	unsigned int i;
	int ret;

	if (num_possible_nodes() > 1 && dev_to_node(dev) < 0) {
		/*
		 * If the accelerator is connected to a node with no memory
		 * there is no point in using the accelerator since the remote
		 * memory transaction will be very slow.
		 */
		return dev_err_probe(dev, -EINVAL, "Invalid NUMA configuration.\n");
	}

	accel_dev = devm_kzalloc(dev, sizeof(*accel_dev), GFP_KERNEL);
	if (!accel_dev)
		return -ENOMEM;

	INIT_LIST_HEAD(&accel_dev->crypto_list);
	INIT_LIST_HEAD(&accel_dev->list);
	accel_pci_dev = &accel_dev->accel_pci_dev;
	accel_pci_dev->pci_dev = pdev;
	accel_dev->owner = THIS_MODULE;

	hw_data = devm_kzalloc(dev, sizeof(*hw_data), GFP_KERNEL);
	if (!hw_data)
		return -ENOMEM;

	pci_read_config_byte(pdev, PCI_REVISION_ID, &accel_pci_dev->revid);
	pci_read_config_dword(pdev, ADF_GEN6_FUSECTL4_OFFSET, &hw_data->fuses[ADF_FUSECTL4]);
	pci_read_config_dword(pdev, ADF_GEN6_FUSECTL0_OFFSET, &hw_data->fuses[ADF_FUSECTL0]);
	pci_read_config_dword(pdev, ADF_GEN6_FUSECTL1_OFFSET, &hw_data->fuses[ADF_FUSECTL1]);

	if (!(hw_data->fuses[ADF_FUSECTL1] & ICP_ACCEL_GEN6_MASK_WCP_WAT_SLICE))
		return dev_err_probe(dev, -EFAULT, "Wireless mode is not supported.\n");

	/* Enable PCI device */
	ret = pcim_enable_device(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot enable PCI device.\n");

	ret = adf_devmgr_add_dev(accel_dev, NULL);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add new accelerator device.\n");

	accel_dev->hw_device = hw_data;
	adf_init_hw_data_6xxx(accel_dev->hw_device);

	/* Get Accelerators and Accelerator Engine masks */
	hw_data->accel_mask = hw_data->get_accel_mask(hw_data);
	hw_data->ae_mask = hw_data->get_ae_mask(hw_data);
	accel_pci_dev->sku = hw_data->get_sku(hw_data);

	/* If the device has no acceleration engines then ignore it */
	if (!hw_data->accel_mask || !hw_data->ae_mask ||
	    (~hw_data->ae_mask & ADF_GEN6_ACCELERATORS_MASK)) {
		dev_err(dev, "No acceleration units were found.\n");
		ret = -EFAULT;
		goto out_err;
	}

	/* Create device configuration table */
	ret = adf_cfg_dev_add(accel_dev);
	if (ret)
		goto out_err;

	/* Set DMA identifier */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err(dev, "No usable DMA configuration.\n");
		goto out_err;
	}

	ret = adf_gen6_cfg_dev_init(accel_dev);
	if (ret) {
		dev_err(dev, "Failed to initialize configuration.\n");
		goto out_err;
	}

	/* Get accelerator capability mask */
	hw_data->accel_capabilities_mask = hw_data->get_accel_cap(accel_dev);
	if (!hw_data->accel_capabilities_mask) {
		dev_err(dev, "Failed to get capabilities mask.\n");
		ret = -EINVAL;
		goto out_err;
	}

	for (i = 0; i < ADF_PCI_MAX_BARS; i++) {
		bar = &accel_pci_dev->pci_bars[i];

		/* Map 64-bit PCIe BAR */
		bar->virt_addr = pcim_iomap_region(pdev, bar_map[i], pci_name(pdev));
		if (!bar->virt_addr) {
			dev_err(dev, "Failed to ioremap PCI region.\n");
			ret = -ENOMEM;
			goto out_err;
		}
	}

	pci_set_master(pdev);

	/*
	 * The PCI config space is saved at this point and will be restored
	 * after a Function Level Reset (FLR) as the FLR does not completely
	 * restore it.
	 */
	ret = pci_save_state(pdev);
	if (ret) {
		dev_err(dev, "Failed to save pci state.\n");
		goto out_err;
	}

	accel_dev->ras_errors.enabled = true;
	adf_dbgfs_init(accel_dev);

	ret = adf_dev_up(accel_dev, true);
	if (ret)
		goto out_err_dev_stop;

	ret = adf_sysfs_init(accel_dev);
	if (ret)
		goto out_err_dev_stop;

	return ret;

out_err_dev_stop:
	adf_dev_down(accel_dev);
out_err:
	adf_cleanup_accel(accel_dev);
	return ret;
}

static void adf_remove(struct pci_dev *pdev)
{
	struct adf_accel_dev *accel_dev = adf_devmgr_pci_to_accel_dev(pdev);

	adf_dev_down(accel_dev);
	adf_cleanup_accel(accel_dev);
}

static void adf_shutdown(struct pci_dev *pdev)
{
	struct adf_accel_dev *accel_dev = adf_devmgr_pci_to_accel_dev(pdev);

	adf_dev_down(accel_dev);
}

static const struct pci_device_id adf_pci_tbl[] = {
	{ PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_QAT_6XXX) },
	{ }
};
MODULE_DEVICE_TABLE(pci, adf_pci_tbl);

static struct pci_driver adf_driver = {
	.id_table = adf_pci_tbl,
	.name = ADF_6XXX_DEVICE_NAME,
	.probe = adf_probe,
	.remove = adf_remove,
	.shutdown = adf_shutdown,
	.sriov_configure = adf_sriov_configure,
	.err_handler = &adf_err_handler,
};
module_pci_driver(adf_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel");
MODULE_FIRMWARE(ADF_6XXX_FW);
MODULE_FIRMWARE(ADF_6XXX_MMP);
MODULE_DESCRIPTION("Intel(R) QuickAssist Technology");
MODULE_SOFTDEP("pre: crypto-intel_qat");
MODULE_IMPORT_NS("CRYPTO_QAT");
