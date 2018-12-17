/* Copyright (c) 2017, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 XiaoMi, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mod_devicetable.h>
#include <linux/of_device.h>
#include <linux/timer.h>

#include "jpeg_enc_core.h"
#include "jpeg_enc_soc.h"
#include "cam_hw.h"
#include "cam_hw_intf.h"
#include "cam_io_util.h"
#include "cam_jpeg_hw_intf.h"
#include "cam_jpeg_hw_mgr_intf.h"
#include "cam_cpas_api.h"
#include "cam_debug_util.h"
#include "cam_jpeg_enc_hw_info_ver_4_2_0.h"

static int cam_jpeg_enc_register_cpas(struct cam_hw_soc_info *soc_info,
	struct cam_jpeg_enc_device_core_info *core_info,
	uint32_t hw_idx)
{
	struct cam_cpas_register_params cpas_register_params;
	int rc;

	cpas_register_params.dev = soc_info->dev;
	memcpy(cpas_register_params.identifier, "jpeg-enc",
		sizeof("jpeg-enc"));
	cpas_register_params.cam_cpas_client_cb = NULL;
	cpas_register_params.cell_index = hw_idx;
	cpas_register_params.userdata = NULL;

	rc = cam_cpas_register_client(&cpas_register_params);
	if (rc) {
		CAM_ERR(CAM_JPEG, "cpas_register failed: %d", rc);
		return rc;
	}
	core_info->cpas_handle = cpas_register_params.client_handle;

	return rc;
}

static int cam_jpeg_enc_unregister_cpas(
	struct cam_jpeg_enc_device_core_info *core_info)
{
	int rc;

	rc = cam_cpas_unregister_client(core_info->cpas_handle);
	if (rc)
		CAM_ERR(CAM_JPEG, "cpas unregister failed: %d", rc);
	core_info->cpas_handle = 0;

	return rc;
}

static int cam_jpeg_enc_remove(struct platform_device *pdev)
{
	struct cam_hw_info *jpeg_enc_dev = NULL;
	struct cam_hw_intf *jpeg_enc_dev_intf = NULL;
	struct cam_jpeg_enc_device_core_info *core_info = NULL;
	int rc;

	jpeg_enc_dev_intf = platform_get_drvdata(pdev);
	if (!jpeg_enc_dev_intf) {
		CAM_ERR(CAM_JPEG, "error No data in pdev");
		return -EINVAL;
	}

	jpeg_enc_dev = jpeg_enc_dev_intf->hw_priv;
	if (!jpeg_enc_dev) {
		CAM_ERR(CAM_JPEG, "error HW data is NULL");
		rc = -ENODEV;
		goto free_jpeg_hw_intf;
	}

	core_info = (struct cam_jpeg_enc_device_core_info *)
		jpeg_enc_dev->core_info;
	if (!core_info) {
		CAM_ERR(CAM_JPEG, "error core data NULL");
		goto deinit_soc;
	}

	rc = cam_jpeg_enc_unregister_cpas(core_info);
	if (rc)
		CAM_ERR(CAM_JPEG, " unreg failed to reg cpas %d", rc);

	mutex_destroy(&core_info->core_mutex);
	kfree(core_info);

deinit_soc:
	rc = cam_soc_util_release_platform_resource(&jpeg_enc_dev->soc_info);
	if (rc)
		CAM_ERR(CAM_JPEG, "Failed to deinit soc rc=%d", rc);

	mutex_destroy(&jpeg_enc_dev->hw_mutex);
	kfree(jpeg_enc_dev);

free_jpeg_hw_intf:
	kfree(jpeg_enc_dev_intf);
	return rc;
}

static int cam_jpeg_enc_probe(struct platform_device *pdev)
{
	struct cam_hw_info *jpeg_enc_dev = NULL;
	struct cam_hw_intf *jpeg_enc_dev_intf = NULL;
	const struct of_device_id *match_dev = NULL;
	struct cam_jpeg_enc_device_core_info *core_info = NULL;
	struct cam_jpeg_enc_device_hw_info *hw_info = NULL;
	int rc;

	jpeg_enc_dev_intf = kzalloc(sizeof(struct cam_hw_intf), GFP_KERNEL);
	if (!jpeg_enc_dev_intf)
		return -ENOMEM;

	of_property_read_u32(pdev->dev.of_node,
		"cell-index", &jpeg_enc_dev_intf->hw_idx);

	jpeg_enc_dev = kzalloc(sizeof(struct cam_hw_info), GFP_KERNEL);
	if (!jpeg_enc_dev) {
		rc = -ENOMEM;
		goto error_alloc_dev;
	}
	jpeg_enc_dev->soc_info.pdev = pdev;
	jpeg_enc_dev->soc_info.dev = &pdev->dev;
	jpeg_enc_dev->soc_info.dev_name = pdev->name;
	jpeg_enc_dev_intf->hw_priv = jpeg_enc_dev;
	jpeg_enc_dev_intf->hw_ops.init = cam_jpeg_enc_init_hw;
	jpeg_enc_dev_intf->hw_ops.deinit = cam_jpeg_enc_deinit_hw;
	jpeg_enc_dev_intf->hw_ops.start = cam_jpeg_enc_start_hw;
	jpeg_enc_dev_intf->hw_ops.stop = cam_jpeg_enc_stop_hw;
	jpeg_enc_dev_intf->hw_ops.reset = cam_jpeg_enc_reset_hw;
	jpeg_enc_dev_intf->hw_ops.process_cmd = cam_jpeg_enc_process_cmd;
	jpeg_enc_dev_intf->hw_type = CAM_JPEG_DEV_ENC;

	platform_set_drvdata(pdev, jpeg_enc_dev_intf);
	jpeg_enc_dev->core_info =
		kzalloc(sizeof(struct cam_jpeg_enc_device_core_info),
			GFP_KERNEL);
	if (!jpeg_enc_dev->core_info) {
		rc = -ENOMEM;
		goto error_alloc_core;
	}
	core_info = (struct cam_jpeg_enc_device_core_info *)jpeg_enc_dev->
		core_info;

	match_dev = of_match_device(pdev->dev.driver->of_match_table,
		&pdev->dev);
	if (!match_dev) {
		CAM_ERR(CAM_JPEG, " No jpeg_enc hardware info");
		rc = -EINVAL;
		goto error_match_dev;
	}
	hw_info = (struct cam_jpeg_enc_device_hw_info *)match_dev->data;
	core_info->jpeg_enc_hw_info = hw_info;
	core_info->core_state = CAM_JPEG_ENC_CORE_NOT_READY;
	mutex_init(&core_info->core_mutex);

	rc = cam_jpeg_enc_init_soc_resources(&jpeg_enc_dev->soc_info,
		cam_jpeg_enc_irq,
		jpeg_enc_dev);
	if (rc) {
		CAM_ERR(CAM_JPEG, " failed to init_soc %d", rc);
		goto error_init_soc;
	}

	rc = cam_jpeg_enc_register_cpas(&jpeg_enc_dev->soc_info,
		core_info, jpeg_enc_dev_intf->hw_idx);
	if (rc) {
		CAM_ERR(CAM_JPEG, " failed to reg cpas %d", rc);
		goto error_reg_cpas;
	}
	jpeg_enc_dev->hw_state = CAM_HW_STATE_POWER_DOWN;
	mutex_init(&jpeg_enc_dev->hw_mutex);
	spin_lock_init(&jpeg_enc_dev->hw_lock);
	init_completion(&jpeg_enc_dev->hw_complete);

	return rc;

error_reg_cpas:
	cam_soc_util_release_platform_resource(&jpeg_enc_dev->soc_info);
error_init_soc:
	mutex_destroy(&core_info->core_mutex);
error_match_dev:
	kfree(jpeg_enc_dev->core_info);
error_alloc_core:
	kfree(jpeg_enc_dev);
error_alloc_dev:
	kfree(jpeg_enc_dev_intf);

	return rc;
}

static const struct of_device_id cam_jpeg_enc_dt_match[] = {
	{
		.compatible = "qcom,cam_jpeg_enc",
		.data = &cam_jpeg_enc_hw_info,
	},
	{}
};
MODULE_DEVICE_TABLE(of, cam_jpeg_enc_dt_match);

static struct platform_driver cam_jpeg_enc_driver = {
	.probe = cam_jpeg_enc_probe,
	.remove = cam_jpeg_enc_remove,
	.driver = {
		.name = "cam-jpeg-enc",
		.owner = THIS_MODULE,
		.of_match_table = cam_jpeg_enc_dt_match,
	},
};

static int __init cam_jpeg_enc_init_module(void)
{
	return platform_driver_register(&cam_jpeg_enc_driver);
}

static void __exit cam_jpeg_enc_exit_module(void)
{
	platform_driver_unregister(&cam_jpeg_enc_driver);
}

module_init(cam_jpeg_enc_init_module);
module_exit(cam_jpeg_enc_exit_module);
MODULE_DESCRIPTION("CAM JPEG_ENC driver");
MODULE_LICENSE("GPL v2");
