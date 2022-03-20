// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Samsung Electronics Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/* sec vibrator inputff */
#ifndef __SEC_VIBRATOR_INPUTFF_H__
#define __SEC_VIBRATOR_INPUTFF_H__

#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/pm_wakeup.h>

#define VIB_NOTIFIER_ON	(1)
#define FWLOAD_TRY (3)

struct sec_vib_inputff_ops {
	int (*upload)(struct input_dev *dev,
		struct ff_effect *effect, struct ff_effect *old);
	int (*playback)(struct input_dev *dev,
		int effect_id, int val);
	void (*set_gain)(struct input_dev *dev, u16 gain);
	int (*erase)(struct input_dev *dev, int effect_id);
	int (*get_i2s_test)(struct input_dev *dev);
	int (*fw_load)(struct input_dev *dev, unsigned int fw_id);
};

struct sec_vib_inputff_fwdata {
	int id;
	int retry;
	int ret[FWLOAD_TRY];
	int stat;
	struct workqueue_struct *fw_workqueue;
	struct work_struct wk;
	struct delayed_work retry_wk;
	struct delayed_work store_wk;
	struct wakeup_source ws;
	struct mutex stat_lock;
};

struct sec_vib_inputff_drvdata {
	struct class *sec_vib_inputff_class;
	struct device *virtual_dev;
	u32 devid : 24;
	u8 revid;
	u64 ff_val;
	bool vibe_init_success;
	struct device *dev;
	struct input_dev *input;
	struct attribute_group *dev_attr_group;
	struct attribute_group *dev_attr_cal_group;
	struct attribute_group *sec_vib_attr_group;
	const struct sec_vib_inputff_ops *vib_ops;
	void *private_data;
	int temperature;
	int ach_percent;
	int support_fw;
	struct sec_vib_inputff_fwdata fw;
};

/* firmware load status. if fail, return err number */
#define FW_LOAD_INIT	    (1<<0)
#define	FW_LOAD_STORE	    (1<<1)
#define FW_LOAD_SUCCESS	    (1<<2)

extern int sec_vib_inputff_notifier_register(struct notifier_block *nb);
extern int sec_vib_inputff_notifier_unregister(struct notifier_block *nb);
extern int sec_vib_inputff_notifier_notify(void);
extern int sec_vib_inputff_setbit(struct sec_vib_inputff_drvdata *ddata, int val);
extern int sec_vib_inputff_register(struct sec_vib_inputff_drvdata *ddata);
extern void sec_vib_inputff_unregister(struct sec_vib_inputff_drvdata *ddata);
extern int sec_vib_inputff_get_current_temp(struct sec_vib_inputff_drvdata *ddata);
extern int sec_vib_inputff_get_ach_percent(struct sec_vib_inputff_drvdata *ddata);
extern int sec_vib_inputff_sysfs_init(struct sec_vib_inputff_drvdata *ddata);
extern void sec_vib_inputff_sysfs_exit(struct sec_vib_inputff_drvdata *ddata);
#endif
