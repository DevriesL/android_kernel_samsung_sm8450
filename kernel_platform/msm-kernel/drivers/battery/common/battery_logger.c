/*
 * battery_logger.c
 * Samsung Mobile Battery Driver
 *
 * Copyright (C) 2021 Samsung Electronics
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/version.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/sched/clock.h>
#include <linux/kernel.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <stdarg.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0))
#include <linux/time64.h>
#else
#include <linux/time.h>
#endif
#include <linux/rtc.h>
#include "sec_battery.h"
#include "battery_logger.h"

#define BATTERYLOG_MAX_STRING_SIZE (1 << 7) /* 128 */
#define BATTERYLOG_MAX_BUF_SIZE	200 /* 200 */

struct batterylog_buf {
	unsigned long log_index;
	char batstr_buffer[BATTERYLOG_MAX_BUF_SIZE*BATTERYLOG_MAX_STRING_SIZE];
};

struct batterylog_root_str {
	struct batterylog_buf *batterylog_buffer;
	struct mutex battery_log_lock;
	int init;
};

static struct batterylog_root_str batterylog_root;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0))
static void logger_get_time_of_the_day_in_hr_min_sec(char *tbuf, int len)
{
	struct timespec64 tv;
	struct rtc_time tm;
	unsigned long local_time;

	/* Format the Log time R#: [hr:min:sec.microsec] */
	ktime_get_real_ts64(&tv);
	/* Convert rtc to local time */
	local_time = (u32)(tv.tv_sec - (sys_tz.tz_minuteswest * 60));
	rtc_time64_to_tm(local_time, &tm);
	scnprintf(tbuf, len,
		  "[%d-%02d-%02d %02d:%02d:%02d]",
		  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		  tm.tm_hour, tm.tm_min, tm.tm_sec);
}
#else
static void logger_get_time_of_the_day_in_hr_min_sec(char *tbuf, int len)
{
	struct timeval tv;
	struct rtc_time tm;
	unsigned long local_time;

	/* Format the Log time R#: [hr:min:sec.microsec] */
	do_gettimeofday(&tv);
	/* Convert rtc to local time */
	local_time = (u32)(tv.tv_sec - (sys_tz.tz_minuteswest * 60));
	rtc_time_to_tm(local_time, &tm);
	scnprintf(tbuf, len,
		"[%d-%02d-%02d %02d:%02d:%02d]",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec);
}
#endif

static int batterylog_proc_show(struct seq_file *m, void *v)
{
	struct batterylog_buf *temp_buffer;

	temp_buffer = batterylog_root.batterylog_buffer;

	if (!temp_buffer)
		goto err;
	pr_info("%s\n", __func__);

	if (sec_bat_get_lpmode()) {
		seq_printf(m,
			"*****Battery LPM Logs*****\n");
	} else {
		seq_printf(m,
			"*****Battery Power On Logs*****\n");
	}

	seq_printf(m, "%s", temp_buffer->batstr_buffer);

err:
	return 0;
}

void store_battery_log(const char *fmt, ...)
{
	unsigned long long tnsec;
	unsigned long rem_nsec;
	unsigned long target_index;
	char *bat_buf;
	int string_len, rem_buf;
	char temp[BATTERYLOG_MAX_STRING_SIZE];
	va_list ap;

	if (!batterylog_root.init)
		return;

	pr_err("%s\n", __func__);

	mutex_lock(&batterylog_root.battery_log_lock);
	tnsec = local_clock();
	rem_nsec = do_div(tnsec, 1000000000);

	pr_info("%s\n", __func__);

	logger_get_time_of_the_day_in_hr_min_sec(temp, BATTERYLOG_MAX_STRING_SIZE);
	string_len = strlen(temp);

	/* To give rem buf size to vsnprint so that it can add '\0' at the end of string. */
	rem_buf = BATTERYLOG_MAX_STRING_SIZE - string_len;

	pr_debug("%s string len after storing time = %d, time = %llu , rem temp buf = %d\n",
			__func__, string_len, tnsec, rem_buf);

	va_start(ap, fmt);

	/* store upto buff size, data after buff is ignored, hence can't have illegal buff overflow access */
	vsnprintf(temp+string_len, sizeof(char)*rem_buf, fmt, ap);

	va_end(ap);

	target_index = batterylog_root.batterylog_buffer->log_index;

	/* Remaining size of actual storage buffer. -2 is used as last 2 indexs are fixed for '\n' and '\0' */
	rem_buf = BATTERYLOG_MAX_BUF_SIZE*BATTERYLOG_MAX_STRING_SIZE - target_index-2;

	string_len = strlen(temp);

	/* If remaining buff size is less than the string then overwrite from start */
	if (rem_buf < string_len)
		target_index = 0;

	pr_info("%s string len = %d, before index(%lu) , time = %llu\n",
			__func__, string_len, target_index, tnsec);

	bat_buf = &batterylog_root.batterylog_buffer->batstr_buffer[target_index];
	if (bat_buf == NULL) {
		pr_err("%s target_buffer error\n", __func__);
		goto err;
	}
	strncpy(bat_buf, temp, string_len);

	/* '\n' Diffrentiator between two stored strings */
	bat_buf[string_len] = '\n';

	target_index = target_index+string_len+1;

	batterylog_root.batterylog_buffer->log_index = target_index;

	pr_info("%s after index(%lu)\n", __func__, target_index);

err:
	mutex_unlock(&batterylog_root.battery_log_lock);

	pr_err("%s end\n", __func__);
}

static int batterylog_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, batterylog_proc_show, NULL);
}

static const struct proc_ops batterylog_proc_fops = {
	.proc_open	= batterylog_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

int register_batterylog_proc(void)
{
	int ret = 0;

	if (batterylog_root.init) {
		pr_err("%s already registered\n", __func__);
		goto err;
	}
	pr_info("%s\n", __func__);
	mutex_init(&batterylog_root.battery_log_lock);

	batterylog_root.batterylog_buffer
		= kzalloc(sizeof(struct batterylog_buf), GFP_KERNEL);
	if (!batterylog_root.batterylog_buffer) {
		ret = -ENOMEM;
		goto err;
	}
	pr_info("%s size=%zu\n", __func__, sizeof(struct batterylog_buf));
	proc_create("batterylog", 0, NULL, &batterylog_proc_fops);
	batterylog_root.init = 1;

err:
	return ret;
}
EXPORT_SYMBOL(register_batterylog_proc);

void unregister_batterylog_proc(void)
{
	pr_info("%s\n", __func__);
	mutex_destroy(&batterylog_root.battery_log_lock);
	kfree(batterylog_root.batterylog_buffer);
	batterylog_root.batterylog_buffer = NULL;
	remove_proc_entry("batterylog", NULL);
	batterylog_root.init = 0;
}
EXPORT_SYMBOL(unregister_batterylog_proc);
