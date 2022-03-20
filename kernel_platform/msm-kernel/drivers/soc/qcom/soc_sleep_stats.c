// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2011-2021, The Linux Foundation. All rights reserved.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox/qmp.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/string.h>

#include <linux/soc/qcom/smem.h>
#include <soc/qcom/soc_sleep_stats.h>
#include <clocksource/arm_arch_timer.h>

#if IS_ENABLED(CONFIG_SEC_PM)
#include <trace/events/power.h>

#define MAX_BUF_LEN		512
#define MSM_ARCH_TIMER_FREQ	19200000
#define GET_SEC(A)		((A) / (MSM_ARCH_TIMER_FREQ))
#define GET_MSEC(A)		(((A) / (MSM_ARCH_TIMER_FREQ / 1000)) % 1000)

static DEFINE_MUTEX(sleep_stats_mutex);
#endif /* CONFIG_SEC_PM */

#define STAT_TYPE_ADDR		0x0
#define COUNT_ADDR		0x4
#define LAST_ENTERED_AT_ADDR	0x8
#define LAST_EXITED_AT_ADDR	0x10
#define ACCUMULATED_ADDR	0x18
#define CLIENT_VOTES_ADDR	0x1c

#define DDR_STATS_MAGIC_KEY	0xA1157A75
#define DDR_STATS_MAX_NUM_MODES	0x14
#define MAX_DRV			18
#define MAX_MSG_LEN		35
#define DRV_ABSENT		0xdeaddead
#define DRV_INVALID		0xffffdead
#define VOTE_MASK		0x3fff
#define VOTE_X_SHIFT		14

#define DDR_STATS_MAGIC_KEY_ADDR	0x0
#define DDR_STATS_NUM_MODES_ADDR	0x4
#define DDR_STATS_NAME_ADDR		0x0
#define DDR_STATS_COUNT_ADDR		0x4
#define DDR_STATS_DURATION_ADDR		0x8

#if IS_ENABLED(CONFIG_QCOM_SMEM)
struct subsystem_data {
	const char *name;
	u32 smem_item;
	u32 pid;
};

static struct subsystem_data subsystems[] = {
	{ "modem", 605, 1 },
	{ "wpss", 605, 13 },
	{ "adsp", 606, 2 },
	{ "adsp_island", 613, 2 },
	{ "cdsp", 607, 5 },
	{ "slpi", 608, 3 },
	{ "slpi_island", 613, 3 },
	{ "gpu", 609, 0 },
	{ "display", 610, 0 },
	{ "apss", 631, QCOM_SMEM_HOST_ANY },
};
#endif

struct stats_config {
	unsigned int offset_addr;
	unsigned int ddr_offset_addr;
	unsigned int num_records;
	bool appended_stats_avail;
};

struct stats_entry {
	uint32_t name;
	uint32_t count;
	uint64_t duration;
};

struct stats_prv_data {
	const struct stats_config *config;
	void __iomem *reg;
};

struct ddr_stats_g_data {
	bool read_vote_info;
	void __iomem *ddr_reg;
	u32 entry_count;
	struct mutex ddr_stats_lock;
	struct mbox_chan *stats_mbox_ch;
	struct mbox_client stats_mbox_cl;
};

struct sleep_stats {
	u32 stat_type;
	u32 count;
	u64 last_entered_at;
	u64 last_exited_at;
	u64 accumulated;
};

struct appended_stats {
	u32 client_votes;
	u32 reserved[3];
};

struct ddr_stats_g_data *ddr_gdata;

#define DSP_SLEEP_DEBUG_ON

#if defined(DSP_SLEEP_DEBUG_ON)
#include <linux/samsung/debug/sec_debug.h>
#include <linux/workqueue.h>
#include <linux/soc/qcom/cdsp-loader.h>

#define MAX_COUNT 7

struct _dsp_entry {
	char name[4];
	uint64_t entry_sec;
	uint64_t entry_msec;
	uint64_t prev_exit_sec;
	uint64_t prev_exit_msec;
	uint64_t error_count;
	int (*ssr)(void);
} DSP_ENTRY[1];		// 0 : CDSP, 1 : SLPI - slpi is disabled for the time being.

static DECLARE_WORK(dsp_ssr, cdsp_restart);
#endif

static void print_sleep_stats(struct seq_file *s, struct sleep_stats *stat)
{
	u64 accumulated = stat->accumulated;
	/*
	 * If a subsystem is in sleep when reading the sleep stats adjust
	 * the accumulated sleep duration to show actual sleep time.
	 */
	if (stat->last_entered_at > stat->last_exited_at)
		accumulated += arch_timer_read_counter()
			       - stat->last_entered_at;

	seq_printf(s, "Count = %u\n", stat->count);
	seq_printf(s, "Last Entered At = %llu\n", stat->last_entered_at);
	seq_printf(s, "Last Exited At = %llu\n", stat->last_exited_at);
	seq_printf(s, "Accumulated Duration = %llu\n", accumulated);
}

static int subsystem_sleep_stats_show(struct seq_file *s, void *d)
{
#if IS_ENABLED(CONFIG_QCOM_SMEM)
	struct subsystem_data *subsystem = s->private;
	struct sleep_stats *stat;

	stat = qcom_smem_get(subsystem->pid, subsystem->smem_item, NULL);
	if (IS_ERR(stat))
		return PTR_ERR(stat);

	print_sleep_stats(s, stat);

#endif
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(subsystem_sleep_stats);

static int soc_sleep_stats_show(struct seq_file *s, void *d)
{
	struct stats_prv_data *prv_data = s->private;
	void __iomem *reg = prv_data->reg;
	struct sleep_stats stat;

	stat.count = readl_relaxed(reg + COUNT_ADDR);
	stat.last_entered_at = readq(reg + LAST_ENTERED_AT_ADDR);
	stat.last_exited_at = readq(reg + LAST_EXITED_AT_ADDR);
	stat.accumulated = readq(reg + ACCUMULATED_ADDR);

	print_sleep_stats(s, &stat);

	if (prv_data->config->appended_stats_avail) {
		struct appended_stats app_stat;

		app_stat.client_votes = readl_relaxed(reg + CLIENT_VOTES_ADDR);
		seq_printf(s, "Client_votes = %#x\n", app_stat.client_votes);
	}

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(soc_sleep_stats);

static void  print_ddr_stats(struct seq_file *s, int *count,
			     struct stats_entry *data, u64 accumulated_duration)
{

	u32 cp_idx = 0;
	u32 name, duration = 0;

	if (accumulated_duration)
		duration = (data->duration * 100) / accumulated_duration;

	name = (data->name >> 8) & 0xFF;
	if (name == 0x0) {
		name = (data->name) & 0xFF;
		*count = *count + 1;
		seq_printf(s,
		"LPM %d:\tName:0x%x\tcount:%u\tDuration (ticks):%ld (~%d%%)\n",
			*count, name, data->count, data->duration, duration);
	} else if (name == 0x1) {
		cp_idx = data->name & 0x1F;
		name = data->name >> 16;

		if (!name || !data->count)
			return;

		seq_printf(s,
		"Freq %dMhz:\tCP IDX:%u\tcount:%u\tDuration (ticks):%ld (~%d%%)\n",
			name, cp_idx, data->count, data->duration, duration);
	}
}

static int ddr_stats_show(struct seq_file *s, void *d)
{
	struct stats_entry data[DDR_STATS_MAX_NUM_MODES];
	void __iomem *reg = s->private;
	u32 entry_count;
	u64 accumulated_duration = 0;
	int i, lpm_count = 0;

	entry_count = readl_relaxed(reg + DDR_STATS_NUM_MODES_ADDR);
	if (entry_count > DDR_STATS_MAX_NUM_MODES) {
		pr_err("Invalid entry count\n");
		return 0;
	}

	reg += DDR_STATS_NUM_MODES_ADDR + 0x4;

	for (i = 0; i < entry_count; i++) {
		data[i].count = readl_relaxed(reg + DDR_STATS_COUNT_ADDR);

		data[i].name = readl_relaxed(reg + DDR_STATS_NAME_ADDR);

		data[i].duration = readq_relaxed(reg + DDR_STATS_DURATION_ADDR);

		accumulated_duration += data[i].duration;
		reg += sizeof(struct stats_entry);
	}

	for (i = 0; i < entry_count; i++)
		print_ddr_stats(s, &lpm_count, &data[i], accumulated_duration);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(ddr_stats);

int ddr_stats_get_ss_count(void)
{
	return ddr_gdata->read_vote_info ? MAX_DRV : -EOPNOTSUPP;
}
EXPORT_SYMBOL(ddr_stats_get_ss_count);

int ddr_stats_get_ss_vote_info(int ss_count,
				struct ddr_stats_ss_vote_info *vote_info)
{
	char buf[MAX_MSG_LEN] = {};
	struct qmp_pkt pkt;
	void __iomem *reg;
	u32 vote_offset, val[MAX_DRV];
	int ret, i;

	if (!vote_info || !(ss_count == MAX_DRV) || !ddr_gdata)
		return -ENODEV;

	if (!ddr_gdata->read_vote_info)
		return -EOPNOTSUPP;

	mutex_lock(&ddr_gdata->ddr_stats_lock);
	ret = scnprintf(buf, MAX_MSG_LEN, "{class: ddr, res: drvs_ddr_votes}");
	pkt.size = (ret + 0x3) & ~0x3;
	pkt.data = buf;

	ret = mbox_send_message(ddr_gdata->stats_mbox_ch, &pkt);
	if (ret < 0) {
		pr_err("Error sending mbox message: %d\n", ret);
		mutex_unlock(&ddr_gdata->ddr_stats_lock);
		return ret;
	}

	vote_offset = sizeof(u32) + sizeof(u32) +
			(ddr_gdata->entry_count * sizeof(struct stats_entry));
	reg = ddr_gdata->ddr_reg;

	for (i = 0; i < ss_count; i++, reg += sizeof(u32)) {
		val[i] = readl_relaxed(reg + vote_offset);
		if (val[i] == DRV_ABSENT) {
			vote_info[i].ab = DRV_ABSENT;
			vote_info[i].ib = DRV_ABSENT;
			continue;
		} else if (val[i] == DRV_INVALID) {
			vote_info[i].ab = DRV_INVALID;
			vote_info[i].ib = DRV_INVALID;
			continue;
		}

		vote_info[i].ab = (val[i] >> VOTE_X_SHIFT) & VOTE_MASK;
		vote_info[i].ib = val[i] & VOTE_MASK;
	}

	mutex_unlock(&ddr_gdata->ddr_stats_lock);
	return 0;

}
EXPORT_SYMBOL(ddr_stats_get_ss_vote_info);

static struct dentry *create_debugfs_entries(void __iomem *reg,
					     void __iomem *ddr_reg,
					     struct stats_prv_data *prv_data,
					     struct device_node *node)
{
	struct dentry *root;
	char stat_type[sizeof(u32) + 1] = {0};
	u32 offset, type, key;
	int i, j, n_subsystems;
	const char *name;

	root = debugfs_create_dir("qcom_sleep_stats", NULL);

	for (i = 0; i < prv_data[0].config->num_records; i++) {
		offset = STAT_TYPE_ADDR + (i * sizeof(struct sleep_stats));

		if (prv_data[0].config->appended_stats_avail)
			offset += i * sizeof(struct appended_stats);

		prv_data[i].reg = reg + offset;

		type = readl_relaxed(prv_data[i].reg);
		memcpy(stat_type, &type, sizeof(u32));
		strim(stat_type);

		debugfs_create_file(stat_type, 0444, root,
				    &prv_data[i],
				    &soc_sleep_stats_fops);
	}

	n_subsystems = of_property_count_strings(node, "ss-name");
	if (n_subsystems < 0)
		goto exit;

	for (i = 0; i < n_subsystems; i++) {
		of_property_read_string_index(node, "ss-name", i, &name);

		for (j = 0; j < ARRAY_SIZE(subsystems); j++) {
			if (!strcmp(subsystems[j].name, name)) {
				debugfs_create_file(subsystems[j].name, 0444,
						    root, &subsystems[j],
						    &subsystem_sleep_stats_fops);
				break;
			}
		}

	}

	if (!ddr_reg)
		goto exit;

	key = readl_relaxed(ddr_reg + DDR_STATS_MAGIC_KEY_ADDR);
	if (key == DDR_STATS_MAGIC_KEY)
		debugfs_create_file("ddr_stats", 0444,
				     root, ddr_reg, &ddr_stats_fops);

exit:
	return root;
}

#if IS_ENABLED(CONFIG_SEC_PM)
extern int _debug_vx_show(void);
extern int aop_lpm_mon_enable(bool enable);
struct delayed_work restart_work;
int restart_flag;
u64 last_accumulated[5];
static void __iomem *global_reg;
static struct stats_prv_data *global_prv_data;
struct device_node *global_node;
static bool smem_init = true;

static void sec_sleep_stats_show(const char *annotation)
{
	char buf[MAX_BUF_LEN];
	char *buf_ptr = buf;
	unsigned int duration_sec, duration_msec;

#if defined(DSP_SLEEP_DEBUG_ON)
	struct _dsp_entry *dsp_entry = NULL;
	int is_debug_low = 0;
	unsigned int debug_level = 0;
#endif

	char stat_type[sizeof(u32) + 1] = {0};
	u32 offset, type;
	int i, j, n_subsystems;
	const char *name;
	bool is_exit = (!strcmp("exit", annotation)) ? true : false;

	mutex_lock(&sleep_stats_mutex);

	/* sleep_stats */
	buf_ptr += sprintf(buf_ptr, "PM: %s: ", annotation);
	for (i = 0; i < global_prv_data[0].config->num_records; i++) {
		struct sleep_stats stat;
		u64 accumulated;

		offset = STAT_TYPE_ADDR + (i * sizeof(struct sleep_stats));

		if (global_prv_data[0].config->appended_stats_avail)
			offset += i * sizeof(struct appended_stats);

		global_prv_data[i].reg = global_reg + offset;

		type = readl_relaxed(global_prv_data[i].reg);
		memcpy(stat_type, &type, sizeof(u32));
		strim(stat_type);	// stat_type: sleep_stats name

		stat.count = readl_relaxed(global_prv_data[i].reg + COUNT_ADDR);
		stat.last_entered_at = readq(global_prv_data[i].reg + LAST_ENTERED_AT_ADDR);
		stat.last_exited_at = readq(global_prv_data[i].reg + LAST_EXITED_AT_ADDR);
		stat.accumulated = readq(global_prv_data[i].reg + ACCUMULATED_ADDR);
		accumulated = stat.accumulated;

		if (stat.last_entered_at > stat.last_exited_at)
			accumulated += arch_timer_read_counter()
					- stat.last_entered_at;

		if (is_exit && accumulated == last_accumulated[i]) {
			buf_ptr += sprintf(buf_ptr, "*");
			if (!(strcmp("cxsd", stat_type)))
				restart_flag = _debug_vx_show();
		}
		last_accumulated[i] = accumulated;

		duration_sec = GET_SEC(accumulated);
		duration_msec = GET_MSEC(accumulated);

		buf_ptr += sprintf(buf_ptr, "%s(%d, %u.%u), ",
				stat_type,
				stat.count,
				duration_sec, duration_msec);
	}
	buf_ptr += sprintf(buf_ptr, "\n");

	/* subsystem stats */
	if (smem_init) {
		buf_ptr += sprintf(buf_ptr, "PM: %s: ", annotation);
		n_subsystems = of_property_count_strings(global_node, "ss-name");
		if (n_subsystems < 0) {
			pr_err("%s: n_subsystems is under 0, ret=%d\n", __func__, n_subsystems);
			mutex_unlock(&sleep_stats_mutex);
			return;
		}

		for (i = 0; i < n_subsystems; i++) {
			of_property_read_string_index(global_node, "ss-name", i, &name);

			for (j = 0; j < ARRAY_SIZE(subsystems); j++) {
				if (!strcmp(subsystems[j].name, name)) {
#if IS_ENABLED(CONFIG_QCOM_SMEM)
					struct subsystem_data *subsystem = &subsystems[j];
					struct sleep_stats *stat;
					u64 accumulated;
			
					stat = qcom_smem_get(subsystem->pid, subsystem->smem_item, NULL);
					if (IS_ERR(stat)) {
						pr_err("%s: Failed to get qcom_smem, ret=%d\n", __func__, PTR_ERR(stat));
						smem_init = false;
						break;
					}

					accumulated = stat->accumulated;

					if (stat->last_entered_at > stat->last_exited_at)
						accumulated += arch_timer_read_counter()
							       - stat->last_entered_at;

					duration_sec = GET_SEC(accumulated);
					duration_msec = GET_MSEC(accumulated);

#if defined(DSP_SLEEP_DEBUG_ON)
#if 0
					dsp_entry = (!strcmp(subsystem->name, "cdsp")) ? &DSP_ENTRY[0] :
								(!strcmp(subsystem->name, "slpi") ? &DSP_ENTRY[1] : NULL);
#else
					dsp_entry = (!strcmp(subsystem->name, "cdsp")) ? &DSP_ENTRY[0] : NULL;
#endif

					if (dsp_entry != NULL) {
						if (!is_exit) {
							// entry
							dsp_entry->entry_sec = duration_sec;
							dsp_entry->entry_msec = duration_msec;
						} else {
							//exit
							/* Error detected if exit duration is same as entry */
							if((duration_sec == dsp_entry->entry_sec &&
										duration_msec == dsp_entry->entry_msec) &&
									(duration_sec == dsp_entry->prev_exit_sec &&
									 duration_msec == dsp_entry->prev_exit_msec)) {
								dsp_entry->error_count++;
								printk("entry error cnt : %d\n", dsp_entry->error_count);
							} else {
								dsp_entry->error_count = 0;
							}
							dsp_entry->prev_exit_sec = duration_sec;
							dsp_entry->prev_exit_msec = duration_msec;
						}
					}
#endif
					buf_ptr += sprintf(buf_ptr, "%s(%d, %u.%u), ",
							subsystems[j].name,
							stat->count,
							duration_sec, duration_msec);
#endif
					break;
				}
			}

			if (!smem_init)
				break;
		}

		buf_ptr--;
		buf_ptr--;
		buf_ptr += sprintf(buf_ptr, "\n");
	}

	mutex_unlock(&sleep_stats_mutex);

	pr_info("%s", buf);

#if defined(DSP_SLEEP_DEBUG_ON)
	// 0 : CDSP, 1 : SLPI
	for (i = 0; i < sizeof(DSP_ENTRY) / sizeof(struct _dsp_entry); i++) {
		dsp_entry = &DSP_ENTRY[i];

		if(dsp_entry->error_count > MAX_COUNT) {
			debug_level = sec_debug_level();

			switch (debug_level) {
				case SEC_DEBUG_LEVEL_LOW:
					is_debug_low = 1;
					break;
				case SEC_DEBUG_LEVEL_MID:
					is_debug_low = 0;
					break;
			}

			if (!is_debug_low) {
				pr_err("entry error cnt : %d\n", dsp_entry->error_count);
				pr_err("Intentional crash for %s\n", dsp_entry->name);
				BUG_ON(1);
			} else {
				dsp_entry->error_count = 0;
				pr_err("reset entry error cnt : %d\n", dsp_entry->error_count);
				pr_err("Intentional cdsp subsystem restart\n");
				schedule_work(&dsp_ssr);
			}
		}
	}
#endif
}

static void do_restart_work(struct work_struct *work)
{
	aop_lpm_mon_enable(true);
}

static void soc_sleep_stats_debug_suspend_trace_probe(void *unused,
					const char *action, int val, bool start)
{
	/*
		SUSPEND
		start(1), val(1), action(suspend_enter)
	*/
	if (start && val > 0 && !strcmp("suspend_enter", action))
		cancel_delayed_work_sync(&restart_work);

	/*
		SUSPEND
		start(1), val(1), action(machine_suspend)
	*/
	if (start && val > 0 && !strcmp("machine_suspend", action))
		sec_sleep_stats_show("entry");

	/*
		RESUME
		start(0), val(1), action(machine_suspend)
	*/
	if (!start && val > 0 && !strcmp("machine_suspend", action))
		sec_sleep_stats_show("exit");

	/*
		RESUME
		start(0), val(1), action(resume_console)
	*/
	if ((restart_flag == 1) && !strcmp("resume_console", action))
		schedule_delayed_work(&restart_work, msecs_to_jiffies(100));
}
#endif

static int soc_sleep_stats_probe(struct platform_device *pdev)
{
	struct resource *res;
	void __iomem *reg;
	void __iomem *offset_addr;
	phys_addr_t stats_base;
	resource_size_t stats_size;
	struct dentry *root;
	const struct stats_config *config;
	struct stats_prv_data *prv_data;
	int i;
#if IS_ENABLED(CONFIG_SEC_PM)
	int ret;

	/* Register callback for cheking subsystem stats */
	ret = register_trace_suspend_resume(
		soc_sleep_stats_debug_suspend_trace_probe, NULL);
	if (ret) {
		pr_err("%s: Failed to register suspend trace callback, ret=%d\n",
			__func__, ret);
	}
#endif

	config = device_get_match_data(&pdev->dev);
	if (!config)
		return -ENODEV;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return PTR_ERR(res);

	offset_addr = ioremap(res->start + config->offset_addr, sizeof(u32));
	if (IS_ERR(offset_addr))
		return PTR_ERR(offset_addr);

	stats_base = res->start | readl_relaxed(offset_addr);
	stats_size = resource_size(res);
	iounmap(offset_addr);

	reg = devm_ioremap(&pdev->dev, stats_base, stats_size);
	if (!reg)
		return -ENOMEM;

	prv_data = devm_kzalloc(&pdev->dev, config->num_records *
				sizeof(struct stats_prv_data), GFP_KERNEL);
	if (!prv_data)
		return -ENOMEM;

	for (i = 0; i < config->num_records; i++)
		prv_data[i].config = config;

	ddr_gdata = devm_kzalloc(&pdev->dev, sizeof(*ddr_gdata), GFP_KERNEL);
	if (!ddr_gdata)
		return -ENOMEM;

	ddr_gdata->read_vote_info = false;
	if (!config->ddr_offset_addr)
		goto skip_ddr_stats;

	offset_addr = ioremap(res->start + config->ddr_offset_addr,
								sizeof(u32));
	if (IS_ERR(offset_addr))
		return PTR_ERR(offset_addr);

	stats_base = res->start | readl_relaxed(offset_addr);
	iounmap(offset_addr);

	ddr_gdata->ddr_reg = devm_ioremap(&pdev->dev, stats_base, stats_size);
	if (!ddr_gdata->ddr_reg)
		return -ENOMEM;

	mutex_init(&ddr_gdata->ddr_stats_lock);

	ddr_gdata->entry_count = readl_relaxed(ddr_gdata->ddr_reg + DDR_STATS_NUM_MODES_ADDR);
	if (ddr_gdata->entry_count > DDR_STATS_MAX_NUM_MODES) {
		pr_err("Invalid entry count\n");
		goto skip_ddr_stats;
	}

	ddr_gdata->stats_mbox_cl.dev = &pdev->dev;
	ddr_gdata->stats_mbox_cl.tx_block = true;
	ddr_gdata->stats_mbox_cl.tx_tout = 1000;
	ddr_gdata->stats_mbox_cl.knows_txdone = false;

	ddr_gdata->stats_mbox_ch = mbox_request_channel(&ddr_gdata->stats_mbox_cl, 0);
	if (IS_ERR(ddr_gdata->stats_mbox_ch))
		goto skip_ddr_stats;

	ddr_gdata->read_vote_info = true;

	INIT_DELAYED_WORK(&restart_work, do_restart_work);

skip_ddr_stats:
	root = create_debugfs_entries(reg, ddr_gdata->ddr_reg,  prv_data,
				      pdev->dev.of_node);
	platform_set_drvdata(pdev, root);

#if IS_ENABLED(CONFIG_SEC_PM)
	global_reg = reg;
	global_prv_data = prv_data;
	global_node = pdev->dev.of_node;
#endif

#if defined(DSP_SLEEP_DEBUG_ON)
	strncpy(DSP_ENTRY[0].name, "cdsp", 4);
	//strncpy(DSP_ENTRY[1].name, "slpi", 4);
#endif

	return 0;
}

static int soc_sleep_stats_remove(struct platform_device *pdev)
{
	struct dentry *root = platform_get_drvdata(pdev);
#if IS_ENABLED(CONFIG_SEC_PM)
	int ret;

	ret = unregister_trace_suspend_resume(
		soc_sleep_stats_debug_suspend_trace_probe, NULL);
#endif

	debugfs_remove_recursive(root);

	return 0;
}

static const struct stats_config rpm_data = {
	.offset_addr = 0x14,
	.num_records = 2,
	.appended_stats_avail = true,
};

static const struct stats_config rpmh_data = {
	.offset_addr = 0x4,
	.ddr_offset_addr = 0x1c,
	.num_records = 3,
	.appended_stats_avail = false,
};

static const struct of_device_id soc_sleep_stats_table[] = {
	{ .compatible = "qcom,rpm-sleep-stats", .data = &rpm_data },
	{ .compatible = "qcom,rpmh-sleep-stats", .data = &rpmh_data },
	{ }
};

static struct platform_driver soc_sleep_stats_driver = {
	.probe = soc_sleep_stats_probe,
	.remove = soc_sleep_stats_remove,
	.driver = {
		.name = "soc_sleep_stats",
		.of_match_table = soc_sleep_stats_table,
	},
};
module_platform_driver(soc_sleep_stats_driver);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. (QTI) SoC Sleep Stats driver");
MODULE_LICENSE("GPL v2");
MODULE_SOFTDEP("pre: smem  cdsp-loader");
