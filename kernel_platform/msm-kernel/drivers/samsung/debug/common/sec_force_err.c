// SPDX-License-Identifier: GPL-2.0
/*
 * COPYRIGHT(C) 2017-2020 Samsung Electronics Co., Ltd. All Right Reserved.
 */

#define pr_fmt(fmt)     KBUILD_MODNAME ":%s() " fmt, __func__

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/hashtable.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include <linux/samsung/builder_pattern.h>
#include <linux/samsung/debug/sec_force_err.h>

#define FORCE_ERR_HASH_BITS	3

struct force_err_drvdata {
	struct mutex lock;
	DECLARE_HASHTABLE(htbl, FORCE_ERR_HASH_BITS);
#if IS_ENABLED(CONFIG_DEBUG_FS)
	struct dentry *dbgfs;
#endif
};

static struct force_err_drvdata __force_err;
static struct force_err_drvdata *force_err = &__force_err;

static u32 __force_err_hash(const char *val)
{
	u32 hash = 0;

	while (*val++)
		hash += (u32)*val;

	return hash;
}

static struct force_err_handle *__force_err_find_handle_locked(const char *val)
{
	struct force_err_handle *h;
	size_t len = strlen(val) + 1;
	u32 key = __force_err_hash(val);

	hash_for_each_possible(force_err->htbl, h, node, key) {
		if (!memcmp(h->val, val, len))
			return h;
	}

	return ERR_PTR(-ENOENT);
}

int sec_force_err_add_custom_handle(struct force_err_handle *h)
{
	struct force_err_handle *h_old;
	u32 key = __force_err_hash(h->val);
	int ret = 0;

	mutex_lock(&force_err->lock);

	if (hash_empty(force_err->htbl)) {
		ret = -ENODEV;
		goto not_initialized;
	}

	if (hash_hashed(&h->node)) {
		pr_warn("The node is aready added! (%s)\n", h->val);
		goto already_added;
	}

	h_old = __force_err_find_handle_locked(h->val);
	if (!IS_ERR(h_old)) {
		pr_warn("A same handler for %s is regitered before. I'll be removed.\n",
				h->val);
		hash_del(&h_old->node);
	}

	hash_add(force_err->htbl, &h->node, key);

already_added:
not_initialized:
	mutex_unlock(&force_err->lock);
	return ret;
}
EXPORT_SYMBOL(sec_force_err_add_custom_handle);

int sec_force_err_del_custom_handle(struct force_err_handle *h)
{
	int ret = 0;

	mutex_lock(&force_err->lock);

	if (hash_empty(force_err->htbl)) {
		ret = -ENODEV;
		goto not_initialized;
	}

	if (!hash_hashed(&h->node))
		goto already_removed;

	hash_del(&h->node);

already_removed:
not_initialized:
	mutex_unlock(&force_err->lock);
	return ret;
}
EXPORT_SYMBOL(sec_force_err_del_custom_handle);

/* timeout for dog bark/bite */
#define DELAY_TIME 20000

static void __simulate_apps_wdog_bark(struct force_err_handle *h)
{
	unsigned long time_out_jiffies;

	pr_emerg("Simulating apps watch dog bark\n");
	local_irq_disable();

	time_out_jiffies = jiffies + msecs_to_jiffies(DELAY_TIME);
	while (time_is_after_jiffies(time_out_jiffies))
		udelay(1);

	local_irq_enable();
	/* if we reach here, simulation failed */
	pr_emerg("Simulation of apps watch dog bark failed\n");
}

static void __simulate_apps_wdog_bite(struct force_err_handle *h)
{
	unsigned long time_out_jiffies;

#if IS_ENABLED(CONFIG_HOTPLUG_CPU)
	int cpu;

	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;
		remove_cpu(cpu);
	}
#endif

	pr_emerg("Simulating apps watch dog bite\n");
	local_irq_disable();

	time_out_jiffies = jiffies + msecs_to_jiffies(DELAY_TIME);
	while (time_is_after_jiffies(time_out_jiffies))
		udelay(1);

	local_irq_enable();
	/* if we reach here, simulation had failed */
	pr_emerg("Simualtion of apps watch dog bite failed\n");
}

static void __simulate_bus_hang(struct force_err_handle *h)
{
	void __iomem *p = NULL;

	pr_emerg("Generating Bus Hang!\n");

	if (!IS_ENABLED(CONFIG_UML)) {
		p = ioremap_wt(0xFC4B8000, 32);
		*(unsigned int *)p = *(unsigned int *)p;
		mb();	/* memory barriar to generate bus hang */
	}

	pr_info("*p = %x\n", *(unsigned int *)p);

	pr_emerg("Clk may be enabled.Try again if it reaches here!\n");
}

static void __simulate_dabort(struct force_err_handle *h)
{
	char *buf = NULL;

	*buf = 0x0;
}

static void __simulate_pabort(struct force_err_handle *h)
{
	((void (*)(void))NULL)();
}

static void __simulate_undef(struct force_err_handle *h)
{
	BUG();
}

static void __simulate_dblfree(struct force_err_handle *h)
{
	unsigned int *ptr = kmalloc(sizeof(unsigned int), GFP_KERNEL);

	kfree(ptr);
	msleep(1000);
	kfree(ptr);
}

static void __simulate_danglingref(struct force_err_handle *h)
{
	unsigned int *ptr = kmalloc(sizeof(unsigned int), GFP_KERNEL);

	kfree(ptr);

	*ptr = 0x1234;
}

static void __simulate_lowmem(struct force_err_handle *h)
{
	size_t i;

	for (i = 0; kmalloc(128 * 1024, GFP_KERNEL); i++)
		;

	pr_emerg("Allocated %zu KB!\n", i * 128);
}

static void __simulate_memcorrupt(struct force_err_handle *h)
{
	unsigned int *ptr = kmalloc(sizeof(unsigned int), GFP_KERNEL);

	*ptr++ = 4;
	*ptr = 2;

	panic("MEMORY CORRUPTION");
}

static struct force_err_handle __force_err_default[] = {
	FORCE_ERR_HANDLE("appdogbark", "Generating an apps wdog bark!",
			__simulate_apps_wdog_bark),
	FORCE_ERR_HANDLE("appdogbite", "Generating an apps wdog bite!",
			__simulate_apps_wdog_bite),
	FORCE_ERR_HANDLE("dabort", "Generating a data abort exception!",
			__simulate_dabort),
	FORCE_ERR_HANDLE("pabort", "Generating a data abort exception!",
			__simulate_pabort),
	FORCE_ERR_HANDLE("undef", "Generating a undefined instruction exception!",
			__simulate_undef),
	FORCE_ERR_HANDLE("bushang", "Generating a Bus Hang!",
			__simulate_bus_hang),
	FORCE_ERR_HANDLE("dblfree", NULL,
			__simulate_dblfree),
	FORCE_ERR_HANDLE("danglingref", NULL,
			__simulate_danglingref),
	FORCE_ERR_HANDLE("lowmem", "Allocating memory until failure!",
			__simulate_lowmem),
	FORCE_ERR_HANDLE("memcorrupt", NULL,
			__simulate_memcorrupt),
	FORCE_ERR_HANDLE("KP", "Generating a data abort exception!",
			__simulate_dabort),
	FORCE_ERR_HANDLE("DP", NULL,
			__simulate_apps_wdog_bark),
};

static long __force_error(const char *val, bool is_kunit)
{
	struct force_err_handle *h;
	long err = 0;

	pr_emerg("!!!WARN forced error : %s\n", val);

	mutex_lock(&force_err->lock);

	h = __force_err_find_handle_locked(val);
	if (IS_ERR(h)) {
		pr_warn("%s is not supported!\n", val);
		mutex_unlock(&force_err->lock);
		return 0;
	}

	if (!is_kunit) {
		h->func(h);
		pr_emerg("No such error defined for now!\n");
	} else
		err = PTR_ERR(h->func);

	mutex_unlock(&force_err->lock);
	return err;
}

static int force_error(const char *val, const struct kernel_param *kp)
{
	char *__trimed_val, *trimed_val;
	int err;

	__trimed_val = kstrdup(val, GFP_KERNEL);
	if (!__trimed_val) {
		pr_err("Not enough memory!\n");
		return 0;
	}
	trimed_val = strim(__trimed_val);

	err = (int)__force_error(trimed_val, false);

	kfree(__trimed_val);

	return err;
}
module_param_call(force_error, force_error, NULL, NULL, 0644);

#if IS_ENABLED(CONFIG_KUNIT) && IS_ENABLED(CONFIG_UML)
long kunit_force_error(const char *val)
{
	return __force_error(val, true);
}
#endif

static int __force_err_probe_prolog(struct builder *bd)
{
	mutex_init(&force_err->lock);
	hash_init(force_err->htbl);

	return 0;
}

static void __force_err_remove_epilog(struct builder *bd)
{
	mutex_destroy(&force_err->lock);
}

static int __force_err_build_htbl(struct builder *bd)
{
	struct force_err_handle *h;
	u32 key;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(__force_err_default); i++) {
		h = &__force_err_default[i];

		INIT_HLIST_NODE(&h->node);

		key = __force_err_hash(h->val);
		hash_add(force_err->htbl, &h->node, key);
	}

	return 0;
}

#if IS_ENABLED(CONFIG_DEBUG_FS)
static void __force_err_dbgfs_show_each_locked(struct seq_file *m,
		struct force_err_handle *h)
{
	seq_printf(m, "[<%p>] %s\n", h, h->val);
	seq_printf(m, "  - msg  : %s\n", h->msg);
	seq_printf(m, "  - func : [<%p>] %ps\n", h->func, h->func);

	seq_puts(m, "\n");
}

static int sec_force_err_dbgfs_show_all(struct seq_file *m, void *unsed)
{
	struct force_err_handle *h;
	int bkt;

	mutex_lock(&force_err->lock);
	hash_for_each(force_err->htbl, bkt, h, node) {
		__force_err_dbgfs_show_each_locked(m, h);
	}
	mutex_unlock(&force_err->lock);

	return 0;
}

static int sec_force_err_dbgfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, sec_force_err_dbgfs_show_all,
			inode->i_private);
}

static const struct file_operations sec_force_err_dgbfs_fops = {
	.open = sec_force_err_dbgfs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

static int __force_err_debugfs_create(struct builder *bd)
{
	force_err->dbgfs = debugfs_create_file("sec_force_err", 0440,
			NULL, NULL, &sec_force_err_dgbfs_fops);

	return 0;
}

static void __force_err_debugfs_remove(struct builder *bd)
{
	debugfs_remove(force_err->dbgfs);
}
#else
static int __force_err_debugfs_create(struct builder *bd) { return 0; }
static void __force_err_debugfs_remove(struct builder *bd) {}
#endif /* IS_ENABLED(CONFIG_DEBUG_FS) */

static struct dev_builder __force_err_dev_builder[] = {
	DEVICE_BUILDER(__force_err_probe_prolog, __force_err_remove_epilog),
	DEVICE_BUILDER(__force_err_build_htbl, NULL),
	DEVICE_BUILDER(__force_err_debugfs_create, __force_err_debugfs_remove),
};

int sec_force_err_init(struct builder *bd)
{
	struct builder bd_dummy = { .dev = NULL, };

	if (!IS_ENABLED(CONFIG_SEC_FORCE_ERR))
		return 0;

	return sec_director_probe_dev(&bd_dummy, __force_err_dev_builder,
			ARRAY_SIZE(__force_err_dev_builder));
}

void sec_force_err_exit(struct builder *bd)
{
	struct builder bd_dummy = { .dev = NULL, };

	if (!IS_ENABLED(CONFIG_SEC_FORCE_ERR))
		return;

	sec_director_destruct_dev(&bd_dummy, __force_err_dev_builder,
			ARRAY_SIZE(__force_err_dev_builder),
			ARRAY_SIZE(__force_err_dev_builder));
}
