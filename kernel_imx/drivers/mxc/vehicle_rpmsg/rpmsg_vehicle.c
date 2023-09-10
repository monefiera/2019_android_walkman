/*
 * Copyright 2018 NXP
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/imx_rpmsg.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/pm_domain.h>
#include <linux/reboot.h>
#include <linux/rpmsg.h>
#include <linux/delay.h>
#include <linux/extcon.h>
#include <soc/imx8/sc/sci.h>
#include <linux/slab.h>
#include <linux/of_platform.h>

#define RPMSG_TIMEOUT 1000
#define REGISTER_PERIOD 50

/*command type which used between AP and M4 core*/
enum vehicle_cmd {
	VEHICLE_RPMSG_REGISTER = 0,
	VEHICLE_RPMSG_UNREGISTER,
	VEHICLE_RPMSG_CONTROL,
	VEHICLE_RPMSG_PWR_REPORT,
	VEHICLE_RPMSG_GET_INFO,
	VEHICLE_RPMSG_BOOT_REASON,
	VEHICLE_RPMSG_PWR_CTRL,
	VEHICLE_RPMSG_VSTATE,
};

/*
 * There are three types for vehicle_cmd.
 * REQUEST/RESPONSE is paired.
 * NOTIFICATION have no response.
 */
enum vehicle_cmd_type {
	VEHICLE_RPMSG_REQUEST = 0,
	VEHICLE_RPMSG_RESPONSE,
	VEHICLE_RPMSG_NOTIFICATION,
};

/*vehicle_event_type: stateType in command VSTATE*/
enum vehicle_event_type {
	VEHICLE_AC = 0,
	VEHICLE_DOOR,
	VEHICLE_FAN,
	VEHICLE_HEATER,
	VEHICLE_DEFROST,
	VEHICLE_MUTE,
	VEHICLE_VOLUME,
	VEHICLE_RVC,
	VEHICLE_LIGHT,
	VEHICLE_GEAR,
};

/*vehicle_event_door_type: stateValue of type VEHICLE_DOOR*/
enum vehicle_event_door_type {
	VEHICLE_DOOR_UNLOCK = 0,
	VEHICLE_DOOR_LOCK,
};

/*vehicle_event_gear: stateValue of type VEHICLE_GEAR*/
enum vehicle_event_gear {
	VEHICLE_GEAR_NONE = 0,
	VEHICLE_GEAR_PARKING,
	VEHICLE_GEAR_REVERSE,
	VEHICLE_GEAR_NEUTRAL,
	VEHICLE_GEAR_DRIVE,
	VEHICLE_GEAR_FIRST,
	VEHICLE_GEAR_SECOND,
	VEHICLE_GEAR_SPORT,
	VEHICLE_GEAR_MANUAL_1,
	VEHICLE_GEAR_MANUAL_2,
	VEHICLE_GEAR_MANUAL_3,
	VEHICLE_GEAR_MANUAL_4,
	VEHICLE_GEAR_MANUAL_5,
	VEHICLE_GEAR_MANUAL_6,
};

/*mcu_state: mcuOperateMode in command REGISTER*/
enum mcu_state {
	RESOURCE_FREE = 0,
	REROURCE_BUSY,
	USER_ACTIVITY_BUSY,
};

/*vehicle_power_report_type: androidPwrState in command PWR_REPORT*/
enum vehicle_power_report_type {
	BOOT_COMPLETE = 0,
	DEEP_SLEEP_ENTRY,
	DEEP_SLEEP_EXIT,
	SHUTDOWN_POSTPONE,
	SHUTDOWN_START,
	DISPLAY_OFF,
	DISPLAY_ON,
};

struct vehicle_rpmsg_data {
	struct imx_rpmsg_head header;
	u32 client;
	union {
		u8 reserved1;
		u8 retcode;
	};
	union {
		u8 reason;
		u8 partition_id;
		u16 control_id;
		u16 powerstate;
		u16 infoindex;
		u16 currentstate;
		u16 statetype;
		u16 devicestate;
		u16 result;
	};
	union {
		u8 length;
		u16 substate;
		u32 timeout;
		u32 statevalue;
		u32 time_postphone;
		u64 data;
	};
	u8 reserved2;
} __attribute__((packed));


struct rpmsg_vehicle_drvdata {
	struct rpmsg_device *rpdev;
	struct device *dev;
	struct device *vehicle_dev;
	struct vehicle_rpmsg_data *msg;
	struct pm_qos_request pm_qos_req;
	struct delayed_work vehicle_register_work;
	struct completion cmd_complete;
	u32 resources_count;
	u32 *resource;
	/*
	 * register_ready means ap have been registered as client and been ready
	 * load camera/display modoules. When it get the driver signal first time
	 * and register_ready is true, it will loader related moduled.
	 */
	bool register_ready;
	struct clk *clk_core;
	struct clk *clk_esc;
	/* register_ready means ap have been registered as client */
	bool vehicle_client_registered;
};

#ifdef CONFIG_EXTCON
static const unsigned int imx_vehicle_rpmsg_extcon_register_cables[] = {
	EXTCON_VEHICLE_RPMSG_REGISTER,
	EXTCON_NONE,
};
static const unsigned int imx_vehicle_rpmsg_extcon_event_cables[] = {
	EXTCON_VEHICLE_RPMSG_EVENT,
	EXTCON_NONE,
};
struct extcon_dev *rg_edev;
struct extcon_dev *ev_edev;
#endif

static struct rpmsg_vehicle_drvdata *vehicle_rpmsg;
static struct class* vehicle_rpmsg_class;
int state = 0;

/*send message to M4 core through rpmsg*/
static int vehicle_send_message(struct vehicle_rpmsg_data *msg,
			struct rpmsg_vehicle_drvdata *info, bool ack)
{
	int err;

	if (!info->rpdev) {
		dev_dbg(info->dev,
			"rpmsg channel not ready, m4 image ready?\n");
		return -EINVAL;
	}

	pm_qos_add_request(&info->pm_qos_req,
			PM_QOS_CPU_DMA_LATENCY, 0);

	if (ack) {
		reinit_completion(&info->cmd_complete);
	}

	err = rpmsg_send(info->rpdev->ept, (void *)msg,
			    sizeof(struct vehicle_rpmsg_data));
	if (err) {
		dev_err(&info->rpdev->dev, "rpmsg_send failed: %d\n", err);
		goto err_out;
	}

	if (ack) {
		err = wait_for_completion_timeout(&info->cmd_complete,
					msecs_to_jiffies(RPMSG_TIMEOUT));
		if (!err) {
			dev_err(&info->rpdev->dev, "rpmsg_send timeout!\n");
			err = -ETIMEDOUT;
			goto err_out;
		}

		if (info->msg->retcode != 0) {
			dev_err(&info->rpdev->dev, "rpmsg not ack %d!\n",
				info->msg->retcode);
			err = -EINVAL;
			goto err_out;
		}

		err = 0;
	}

err_out:
	pm_qos_remove_request(&info->pm_qos_req);

	return err;
}

/*power on display/camera related power domain once AP get control of display/camera*/
static void force_power_on(void)
{
	sc_ipc_t ipcHndl;
	sc_err_t sciErr;
	uint32_t mu_id;
	uint32_t num;

	sciErr = sc_ipc_getMuID(&mu_id);
	if (sciErr != SC_ERR_NONE) {
		pr_err("Cannot obtain MU ID\n");
		return;
	}

	sciErr = sc_ipc_open(&ipcHndl, mu_id);
	if (sciErr != SC_ERR_NONE) {
		pr_err("sc_ipc_open failed! (sciError = %d)\n", sciErr);
		return;
	}

	for (num = 0; num < vehicle_rpmsg->resources_count; num++) {
		sciErr = sc_pm_set_resource_power_mode(ipcHndl, vehicle_rpmsg->resource[num], SC_PM_PW_MODE_ON);
		if (sciErr)
			pr_err("Failed power operation on resource %d sc_err %d\n", vehicle_rpmsg->resource[num],sciErr);
	}
}

static void sync_hw_clk(void)
{
	struct device *dev = vehicle_rpmsg->vehicle_dev;
	vehicle_rpmsg->clk_core = devm_clk_get(dev, "clk_core");
	if (IS_ERR(vehicle_rpmsg->clk_core)) {
		dev_err(dev, "failed to get csi core clk\n");
		return;
	}

	vehicle_rpmsg->clk_esc = devm_clk_get(dev, "clk_esc");
	if (IS_ERR(vehicle_rpmsg->clk_esc)) {
		dev_err(dev, "failed to get csi esc clk\n");
		return;
	}

	/* clk_get_rate will sync scu clk into linux software clk tree*/
	clk_get_rate(vehicle_rpmsg->clk_core);
	clk_get_rate(vehicle_rpmsg->clk_esc);
}

static void notice_evs_released(struct rpmsg_device *rpdev)
{
	int count = of_get_available_child_count(vehicle_rpmsg->vehicle_dev->of_node);
	if (count) {
		if (of_platform_populate(vehicle_rpmsg->vehicle_dev->of_node,
			NULL, NULL, vehicle_rpmsg->vehicle_dev)) {
			dev_err(&rpdev->dev, "failed to populate child nodes\n");
		}
	}
}

/*vehicle_rpmsg_cb is called once get rpmsg from M4*/
static int vehicle_rpmsg_cb(struct rpmsg_device *rpdev,
	void *data, int len, void *priv, u32 src)
{
	struct vehicle_rpmsg_data *msg = (struct vehicle_rpmsg_data *)data;

	vehicle_rpmsg->msg = msg;
	if (msg->header.cmd == VEHICLE_RPMSG_REGISTER) {
		complete(&vehicle_rpmsg->cmd_complete);
		if (msg->retcode == 0) {
			if (msg->devicestate == RESOURCE_FREE) {
				force_power_on();
				sync_hw_clk();
				notice_evs_released(rpdev);
#ifdef CONFIG_EXTCON
				extcon_set_state_sync(rg_edev, EXTCON_VEHICLE_RPMSG_REGISTER, 1);
#endif
			} else
				vehicle_rpmsg->register_ready = true;
			vehicle_rpmsg->vehicle_client_registered = true;
		}
	} else if (msg->header.cmd == VEHICLE_RPMSG_UNREGISTER) {
		complete(&vehicle_rpmsg->cmd_complete);
		if (msg->retcode == 0) {
#ifdef CONFIG_EXTCON
			extcon_set_state_sync(rg_edev, EXTCON_VEHICLE_RPMSG_REGISTER, 0);
#endif
			vehicle_rpmsg->vehicle_client_registered = false;
		}
	} else if (msg->header.cmd == VEHICLE_RPMSG_CONTROL) {
		complete(&vehicle_rpmsg->cmd_complete);
		dev_dbg(&rpdev->dev, "get vehicle control signal %d", msg->result);
	} else if (msg->header.cmd == VEHICLE_RPMSG_PWR_REPORT) {
		complete(&vehicle_rpmsg->cmd_complete);
	} else if (msg->header.cmd == VEHICLE_RPMSG_GET_INFO) {
		complete(&vehicle_rpmsg->cmd_complete);
	} else if (msg->header.cmd == VEHICLE_RPMSG_BOOT_REASON) {
		msg->header.type = VEHICLE_RPMSG_RESPONSE;
		msg->retcode = 0;
		if (vehicle_send_message(msg, vehicle_rpmsg, false))
			dev_warn(&rpdev->dev, "vehicle_rpmsg_cb send message error \n");
	} else if (msg->header.cmd == VEHICLE_RPMSG_PWR_CTRL) {
		msg->header.type = VEHICLE_RPMSG_RESPONSE;
		msg->retcode = 0;
		if (vehicle_send_message(msg, vehicle_rpmsg, false))
			dev_warn(&rpdev->dev, "vehicle_rpmsg_cb send message error \n");
	} else if (msg->header.cmd == VEHICLE_RPMSG_VSTATE) {
		msg->header.type = VEHICLE_RPMSG_RESPONSE;
		msg->retcode = 0;
		if(msg->statetype == VEHICLE_DOOR) {
			if (msg->statevalue == VEHICLE_DOOR_UNLOCK)
				dev_dbg(&rpdev->dev, "vehicle door is unlock\n");
			else
				dev_dbg(&rpdev->dev, "vehicle door is lock\n");
		} else if(msg->statetype == VEHICLE_GEAR) {
			if (msg->statevalue == VEHICLE_GEAR_DRIVE) {
				if (vehicle_rpmsg->register_ready) {
					force_power_on();
					sync_hw_clk();
					notice_evs_released(rpdev);
#ifdef CONFIG_EXTCON
					extcon_set_state_sync(rg_edev, EXTCON_VEHICLE_RPMSG_REGISTER, 1);
#endif
					vehicle_rpmsg->register_ready = false;
				} else {
					extcon_set_state_sync(ev_edev, EXTCON_VEHICLE_RPMSG_EVENT, 0);
				}
			} else if (msg->statevalue == VEHICLE_GEAR_REVERSE) {
#ifdef CONFIG_EXTCON
				extcon_set_state_sync(ev_edev, EXTCON_VEHICLE_RPMSG_EVENT, 1);
#endif
			}
		} else if(msg->statetype == VEHICLE_FAN) {
			dev_dbg(&rpdev->dev, "vehicle fan state is changed\n");
		} else if(msg->statetype == VEHICLE_AC) {
			dev_dbg(&rpdev->dev, "vehicle air condition state is changed\n");
		}

		if (vehicle_send_message(msg, vehicle_rpmsg, false))
			dev_warn(&rpdev->dev, "vehicle_rpmsg_cb send message error \n");
	}

	return 0;
}

/* send register event to M4 core
 * M4 will release camera/display once register successfully.
 */
static void vehicle_init_handler(struct work_struct *work)
{
	struct vehicle_rpmsg_data msg;
	sc_ipc_t ipc_handle;
	sc_rm_pt_t os_part;
	sc_err_t err;
	uint32_t mu_id;

	msg.header.cate = IMX_RPMSG_VEHICLE;
	msg.header.major = IMX_RMPSG_MAJOR;
	msg.header.minor = IMX_RMPSG_MINOR;
	msg.header.type = 0;
	msg.header.cmd = VEHICLE_RPMSG_REGISTER;
	msg.client = 0;
	msg.reserved1 = 0;

	err = sc_ipc_getMuID(&mu_id);
	if (err != SC_ERR_NONE) {
		pr_err("Cannot obtain MU ID\n");
		return;
	}

	err = sc_ipc_open(&ipc_handle, mu_id);
	if (err != SC_ERR_NONE) {
		pr_err("sc_ipc_open failed!\n");
		return;
	}
	err = sc_rm_get_partition(ipc_handle, &os_part);
	if (err != SC_ERR_NONE) {
		pr_err("sc_rm_get_partition failed!\n");
		msg.partition_id = 0xff;
	} else {
		msg.partition_id = os_part;
	}
	/* need check whether ap have been unregistered before register the ap*/
	if (!vehicle_rpmsg->vehicle_client_registered) {
		while (vehicle_send_message(&msg, vehicle_rpmsg, true)) {
			msleep(REGISTER_PERIOD);
		}
	}
}

/* send unregister event to M4 core
 * M4 will not send can event to ap once get the unregister signal successfully.
 */
static void vehicle_deinit_handler(struct work_struct *work)
{
	struct vehicle_rpmsg_data msg;
	sc_err_t err;

	msg.header.cate = IMX_RPMSG_VEHICLE;
	msg.header.major = IMX_RMPSG_MAJOR;
	msg.header.minor = IMX_RMPSG_MINOR;
	msg.header.type = 0;
	msg.header.cmd = VEHICLE_RPMSG_UNREGISTER;
	msg.client = 0;
	msg.reserved1 = 0;
	msg.reason = 0;

	/* need check whether ap have been registered before deregister the ap*/
	if (vehicle_rpmsg->vehicle_client_registered) {
		if (vehicle_send_message(&msg, vehicle_rpmsg, true)) {
			pr_err("unregister vehicle device failed!\n");
		}
	}
}

static struct notifier_block vehicle_reboot_nb = {
	.notifier_call = vehicle_deinit_handler,
};

static struct notifier_block vehicle_panic_nb = {
	.notifier_call = vehicle_deinit_handler,
};

static int vehicle_rpmsg_probe(struct rpmsg_device *rpdev)
{
#ifdef CONFIG_EXTCON
	int ret = 0;
	rg_edev = devm_extcon_dev_allocate(&rpdev->dev, imx_vehicle_rpmsg_extcon_register_cables);
	if (IS_ERR(rg_edev)) {
		dev_err(&rpdev->dev, "failed to allocate extcon device\n");
	}
	ret = devm_extcon_dev_register(&rpdev->dev,rg_edev);
	if (ret < 0) {
		dev_err(&rpdev->dev, "failed to register extcon device\n");
	}
	ev_edev = devm_extcon_dev_allocate(&rpdev->dev, imx_vehicle_rpmsg_extcon_event_cables);
	if (IS_ERR(ev_edev)) {
		dev_err(&rpdev->dev, "failed to allocate extcon device\n");
	}
	ret = devm_extcon_dev_register(&rpdev->dev,ev_edev);
	if (ret < 0) {
		dev_err(&rpdev->dev, "failed to register extcon device\n");
	}
#endif
	vehicle_rpmsg->rpdev = rpdev;

	dev_info(&rpdev->dev, "new channel: 0x%x -> 0x%x!\n",
			rpdev->src, rpdev->dst);

	init_completion(&vehicle_rpmsg->cmd_complete);

	INIT_DELAYED_WORK(&vehicle_rpmsg->vehicle_register_work,
				vehicle_init_handler);
	schedule_delayed_work(&vehicle_rpmsg->vehicle_register_work, 0);

	return 0;
}

static struct rpmsg_device_id vehicle_rpmsg_id_table[] = {
	{ .name	= "rpmsg-vehicle-channel" },
	{ },
};

static struct rpmsg_driver vehicle_rpmsg_driver = {
	.drv.name       = "vehicle_rpmsg",
	.drv.owner      = THIS_MODULE,
	.id_table       = vehicle_rpmsg_id_table,
	.probe          = vehicle_rpmsg_probe,
	.callback       = vehicle_rpmsg_cb,
};

static ssize_t register_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%u\n", state);
}

/*register ap to M4 once echo 1 > /sys/devices/platform/vehicle_rpmsg/register*/
static ssize_t register_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf,
		size_t size)
{
	size_t err;

	if (!size)
		return -EINVAL;

	unsigned long state_set = simple_strtoul(buf, NULL, 10);
	if (state_set == 1 && state_set != state) {
		err = register_rpmsg_driver(&vehicle_rpmsg_driver);
		if (err < 0) {
			pr_err("register rpmsg driver failed\n");
			return -EINVAL;
		}
		/*detach the power domain pd_dc0*/
		dev_pm_domain_detach(dev, false);
		state = state_set;
	}
	return size;
}

static DEVICE_ATTR(register, 0664, register_show, register_store);

static struct rpmsg_vehicle_drvdata *
rpmsg_vehicle_get_devtree_pdata(struct device *dev)
{
	struct rpmsg_vehicle_drvdata *ddata;

	ddata = devm_kzalloc(dev,
			     sizeof(*ddata),
			     GFP_KERNEL);
	if (!ddata)
		return ERR_PTR(-ENOMEM);

	return ddata;
}

static int vehicle_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rpmsg_vehicle_drvdata *ddata;
	int err;

	ddata = rpmsg_vehicle_get_devtree_pdata(dev);
	if (IS_ERR(ddata))
		return PTR_ERR(ddata);

	err = of_property_read_u32(dev->of_node, "fsl,resources-num", &ddata->resources_count);
	if (err) {
		dev_err(dev, "failed to read soc name from dts\n");
		return err;
	}
	ddata->resource = kzalloc(sizeof(*ddata->resource) * ddata->resources_count, GFP_KERNEL);
	if (!ddata->resource) {
		pr_err("failed to allocate memory for resources\n");
		return ERR_PTR(-ENOMEM);
	}
	err = of_property_read_u32_array(dev->of_node, "fsl,resources", ddata->resource, ddata->resources_count);
	if (err) {
		dev_err(dev, "failed to read soc name from dts\n");
		goto out_free_mem;
	}
	vehicle_rpmsg = ddata;
	vehicle_rpmsg->vehicle_dev = dev;
	vehicle_rpmsg->register_ready = false;
	vehicle_rpmsg->vehicle_client_registered = false;
	platform_set_drvdata(pdev, ddata);

	vehicle_rpmsg_class = class_create(THIS_MODULE, "vehicle_rpmsg");
	if (IS_ERR(vehicle_rpmsg_class)) {
		dev_err(dev, "failed to create class.\n");
		goto out_free_mem;
	}
	err = device_create_file(dev, &dev_attr_register);
	if (err)
		goto out_free_mem;
	return 0;
out_free_mem:
	kfree(ddata->resource);
	return ERR_PTR(-ENOMEM);
}

static void vehicle_remove(struct platform_device *pdev)
{
	kfree(vehicle_rpmsg->resource);
	class_destroy(vehicle_rpmsg_class);
	unregister_rpmsg_driver(&vehicle_rpmsg_driver);
}

static const struct of_device_id imx_vehicle_id[] = {
	{ .compatible = "nxp,imx-vehicle", },
	{},
};

static struct platform_driver vehicle_device_driver = {
	.probe          = vehicle_probe,
	.remove         = vehicle_remove,
	.driver         =  {
		.name   = "rpmsg-vehicle",
		.of_match_table = imx_vehicle_id,
	}
};

static int vehicle_init(void)
{
	int err;
	err = platform_driver_register(&vehicle_device_driver);
	if (err) {
		pr_err("Failed to register rpmsg vehicle driver\n");
		return err;
	}
	atomic_notifier_chain_register(&panic_notifier_list,
					&vehicle_panic_nb);
	register_reboot_notifier(&vehicle_reboot_nb);
	return 0;
}

static void __exit vehicle_exit(void)
{
	platform_driver_unregister(&vehicle_device_driver);
	atomic_notifier_chain_unregister(&panic_notifier_list,
				&vehicle_panic_nb);
	unregister_reboot_notifier(&vehicle_reboot_nb);
}

core_initcall(vehicle_init);
module_exit(vehicle_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NXP Semiconductor");
MODULE_DESCRIPTION("VEHICLE driver based on rpmsg");
