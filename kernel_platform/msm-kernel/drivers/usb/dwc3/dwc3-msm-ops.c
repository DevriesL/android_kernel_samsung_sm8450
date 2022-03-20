// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2021, The Linux Foundation. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/sched.h>
#include <linux/usb/dwc3-msm.h>
#include "core.h"
#include "gadget.h"

#if IS_ENABLED(CONFIG_USB_NOTIFY_LAYER)
#include <linux/usb_notify.h>
#endif
#if IS_ENABLED(CONFIG_USB_CONFIGFS_F_SS_MON_GADGET)
#include <linux/usb/f_ss_mon_gadget.h>
#endif

struct kprobe_data {
	void *x0;
	void *x1;
	void *x2;
};

static int entry_dwc3_gadget_run_stop(struct kretprobe_instance *ri,
				   struct pt_regs *regs)
{
	struct dwc3 *dwc = (struct dwc3 *)regs->regs[0];
	int is_on = (int)regs->regs[1];

	if (is_on) {
		/*
		 * DWC3 gadget IRQ uses a threaded handler which normally runs
		 * at SCHED_FIFO priority.  If it gets busy processing a high
		 * volume of events (usually EP events due to heavy traffic) it
		 * can potentially starve non-RT taks from running and trigger
		 * RT throttling in the scheduler; on some build configs this
		 * will panic.  So lower the thread's priority to run as non-RT
		 * (with a nice value equivalent to a high-priority workqueue).
		 * It has been found to not have noticeable performance impact.
		 */
		struct irq_desc *irq_desc = irq_to_desc(dwc->irq_gadget);
		struct irqaction *action = irq_desc->action;

		for ( ; action != NULL; action = action->next) {
			if (action->thread) {
				dev_info(dwc->dev, "Set IRQ thread:%s pid:%d to SCHED_NORMAL prio\n",
					action->thread->comm, action->thread->pid);
				sched_set_normal(action->thread, MIN_NICE);
				break;
			}
		}
	} else {
		dwc3_core_stop_hw_active_transfers(dwc);
		dwc3_msm_notify_event(dwc, DWC3_GSI_EVT_BUF_CLEAR, 0);
		dwc3_msm_notify_event(dwc, DWC3_CONTROLLER_NOTIFY_CLEAR_DB, 0);
	}

	return 0;
}

static int entry_dwc3_send_gadget_ep_cmd(struct kretprobe_instance *ri,
				   struct pt_regs *regs)
{
	struct dwc3_ep *dep = (struct dwc3_ep *)regs->regs[0];
	unsigned int cmd = (unsigned int)regs->regs[1];
	struct dwc3 *dwc = dep->dwc;

	if (cmd == DWC3_DEPCMD_ENDTRANSFER)
		dwc3_msm_notify_event(dwc,
				DWC3_CONTROLLER_NOTIFY_DISABLE_UPDXFER,
				dep->number);

	return 0;
}

static int entry_dwc3_gadget_reset_interrupt(struct kretprobe_instance *ri,
				   struct pt_regs *regs)
{
	struct dwc3 *dwc = (struct dwc3 *)regs->regs[0];

	dwc3_msm_notify_event(dwc, DWC3_CONTROLLER_NOTIFY_CLEAR_DB, 0);

#if IS_ENABLED(CONFIG_USB_CONFIGFS_F_SS_MON_GADGET)
	usb_reset_notify(dwc->gadget);
#endif

	return 0;
}

static int entry_dwc3_gadget_conndone_interrupt(struct kretprobe_instance *ri,
				   struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;
	struct dwc3 *dwc = (struct dwc3 *)regs->regs[0];

	data->x0 = dwc;
	return 0;
}

static int exit_dwc3_gadget_conndone_interrupt(struct kretprobe_instance *ri,
				   struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;
	struct dwc3 *dwc = (struct dwc3 *)data->x0;

	dwc3_msm_notify_event(dwc, DWC3_CONTROLLER_CONNDONE_EVENT, 0);

	switch (dwc->speed) {
	case DWC3_DSTS_SUPERSPEED_PLUS:
#if defined(CONFIG_USB_NOTIFY_PROC_LOG)
		store_usblog_notify(NOTIFY_USBSTATE,
			(void *)"USB_STATE=ENUM:CONNDONE:PSS", NULL);
#endif
		break;
	case DWC3_DSTS_SUPERSPEED:
#if defined(CONFIG_USB_NOTIFY_PROC_LOG)
		store_usblog_notify(NOTIFY_USBSTATE,
			(void *)"USB_STATE=ENUM:CONNDONE:SS", NULL);
#endif
		break;
	case DWC3_DSTS_HIGHSPEED:
#if defined(CONFIG_USB_NOTIFY_PROC_LOG)
		store_usblog_notify(NOTIFY_USBSTATE,
			(void *)"USB_STATE=ENUM:CONNDONE:HS", NULL);
#endif
		break;
	case DWC3_DSTS_FULLSPEED:
#if defined(CONFIG_USB_NOTIFY_PROC_LOG)
		store_usblog_notify(NOTIFY_USBSTATE,
			(void *)"USB_STATE=ENUM:CONNDONE:FS", NULL);
#endif
		break;
	case DWC3_DSTS_LOWSPEED:
#if defined(CONFIG_USB_NOTIFY_PROC_LOG)
		store_usblog_notify(NOTIFY_USBSTATE,
			(void *)"USB_STATE=ENUM:CONNDONE:LS", NULL);
#endif
		break;
	}
	return 0;
}

static int entry_dwc3_gadget_pullup(struct kretprobe_instance *ri,
				   struct pt_regs *regs)
{
	struct usb_gadget *g = (struct usb_gadget *)regs->regs[0];
	int is_on = (int)regs->regs[1];
	struct dwc3 *dwc = gadget_to_dwc(g);

	dwc3_msm_notify_event(dwc, DWC3_CONTROLLER_PULLUP, is_on);

	return 0;
}

static int entry_dwc3_gadget_vbus_draw(struct kretprobe_instance *ri,
				   struct pt_regs *regs)
{

	unsigned int mA = (unsigned int)regs->regs[1];

	switch (mA) {
	case 2:
#if IS_ENABLED(CONFIG_USB_CONFIGFS_F_SS_MON_GADGET)
		pr_info("[USB] dwc3_gadget_vbus_draw: suspend\n");
		make_suspend_current_event();
#endif
		break;
	case 100:
		break;
	case 500:
		break;
	case 900:
		break;
	default:
		break;
	}
	return 0;
}

#define ENTRY_EXIT(name) {\
	.handler = exit_##name,\
	.entry_handler = entry_##name,\
	.data_size = sizeof(struct kprobe_data),\
	.maxactive = 8,\
	.kp.symbol_name = #name,\
}

#define ENTRY(name) {\
	.entry_handler = entry_##name,\
	.data_size = sizeof(struct kprobe_data),\
	.maxactive = 8,\
	.kp.symbol_name = #name,\
}

static struct kretprobe dwc3_msm_probes[] = {
	ENTRY(dwc3_gadget_run_stop),
	ENTRY(dwc3_send_gadget_ep_cmd),
	ENTRY(dwc3_gadget_reset_interrupt),
	ENTRY_EXIT(dwc3_gadget_conndone_interrupt),
	ENTRY(dwc3_gadget_pullup),
	ENTRY(dwc3_gadget_vbus_draw),
};


int dwc3_msm_kretprobe_init(void)
{
	int ret;
	int i;

	for (i = 0; i < ARRAY_SIZE(dwc3_msm_probes) ; i++) {
		ret = register_kretprobe(&dwc3_msm_probes[i]);
		if (ret < 0) {
			pr_err("register_kretprobe failed, returned %d\n", ret);
			return ret;
		}
	}

	return 0;
}

void dwc3_msm_kretprobe_exit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dwc3_msm_probes); i++)
		unregister_kretprobe(&dwc3_msm_probes[i]);
}

