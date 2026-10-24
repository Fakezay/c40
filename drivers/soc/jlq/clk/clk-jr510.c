// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2020   JLQ Co.,Ltd

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; *
 */

#include <linux/kernel.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/gcd.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <dt-bindings/jlq/jr510/offset-top_crg.h>
#include "clk-jr510.h"

#define FREQ_1M		1000000
#define FREQ_50M	50000000
#define PLL_TIMEOUT_US	2000
//#define CLK_DEBUG
#ifdef CLK_DEBUG
#define clk_debug(fmt, ...) printk(fmt, ##__VA_ARGS__)
#else
#define clk_debug(fmt, ...)
#endif

static void __iomem __init *jlq_clk_get_base(struct device_node *np);

struct jlq_clk jlq_clk = {
	.lock = __SPIN_LOCK_UNLOCKED(jlq_clk.lock),
};
EXPORT_SYMBOL_GPL(jlq_clk);

static int clk_debug_rate_set(void *data, u64 val)
{
	struct clk *clk = (struct clk *)data;
	unsigned long rate = 0;
	int ret;

	ret = clk_set_rate(clk, val);
	if (ret)
		pr_err("clk_set_rate(%s, %lu) failed (%d)\n",
		((char *)__clk_get_name(clk)), (unsigned long)val, ret);
	rate = clk_get_rate(clk);

	return ret;
}

static int clk_debug_rate_get(void *data, u64 *val)
{
	struct clk *clk = data;
	*val = clk_get_rate(clk);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(clk_debug_rate_fops, clk_debug_rate_get,
			clk_debug_rate_set, "%llu\n");

static int clk_debug_enable_set(void *data, u64 val)
{
	struct clk *clk = data;
	int rc = 0;

	if (val)
		rc = clk_prepare_enable(clk);
	else
		clk_disable_unprepare(clk);

	return rc;
}

static int clk_debug_enable_get(void *data, u64 *val)
{
	struct clk *clk = (struct clk *)data;
	int enabled;

	enabled = __clk_is_enabled(clk);

	*val = enabled;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(clk_debug_enable_fops, clk_debug_enable_get,
			clk_debug_enable_set, "%lld\n");
#if 0
static ssize_t clk_debug_parent_read(struct file *filp, char __user *ubuf,
		size_t cnt, loff_t *ppos)
{
	struct clk *clk = filp->private_data;
	struct clk *parent;
	char name[256] = {0};

	parent = clk_get_parent(clk);
	snprintf(name, sizeof(name), "%s\n",
		parent ? __clk_get_name(parent) : "None\n");

	return simple_read_from_buffer(ubuf, cnt, ppos, name, strlen(name));
}

static ssize_t clk_debug_parent_write(struct file *filp,
		const char __user *ubuf, size_t cnt, loff_t *ppos)
{
	struct clk *clk = filp->private_data;
	char buf[256];
	char *cmp;
	int ret = -EINVAL;
	struct clk *parent = NULL;

	cnt = min(cnt, sizeof(buf) - 1);
	if (copy_from_user(&buf, ubuf, cnt))
		return -EFAULT;
	buf[cnt] = '\0';
	cmp = strstrip(buf);

	parent = __clk_lookup(cmp);
	if (!parent)
		return ret;
	ret = clk_set_parent(clk, parent);
	if (ret)
		return ret;

	return cnt;
}

static const struct file_operations clk_debug_parent_fops = {
	.open		= simple_open,
	.read		= clk_debug_parent_read,
	.write		= clk_debug_parent_write,
};
#endif
void jlq_clk_debug_init(struct clk_hw *hw, struct dentry *dentry)
{
	struct dentry *mdir;

	if (!hw || !dentry)
		return;

	mdir = debugfs_create_dir("measure", dentry);
	if (!mdir) {
		pr_err("create debugfs for clk failed\n");
		return;
	}

	if (!debugfs_create_file("clk_rate", 0644, mdir,
				hw->clk, &clk_debug_rate_fops))
		goto err;

	if (!debugfs_create_file("clk_enable", 0644, mdir,
				hw->clk, &clk_debug_enable_fops))
		goto err;
#if 0
	if (!debugfs_create_file("clk_parent", 0644, mdir,
				hw->clk, &clk_debug_parent_fops))
		goto err;
#endif
	return;
err:
	debugfs_remove_recursive(mdir);
}
EXPORT_SYMBOL_GPL(jlq_clk_debug_init);

static bool jlq_clk_better_rate(unsigned long rate, unsigned long now,
			   unsigned long gap, unsigned long flags)
{
	long delta = 0;

	if (flags & FLAG_RATE_HIGHER) {
		delta = now - rate;
		return (delta >= 0) && (delta < gap);
	} else if (flags & FLAG_RATE_LOWER) {
		delta = rate - now;
		return (delta >= 0) && (delta < gap);
	} else
		return abs(now - rate) < gap;
}

static void jlq_formula_cdiv(unsigned int val, unsigned int mask,
				struct jlq_div_table *ptbl)
{
	ptbl->val = val;
	ptbl->grp = 1;
	ptbl->div = val + 1;
}

static void jlq_formula_gdiv(unsigned int val, unsigned int mask,
				struct jlq_div_table *ptbl)
{
	ptbl->val = val;
	ptbl->grp = val + 1;
	ptbl->div = mask + 1;
}

static unsigned int jlq_clkdiv_table_get_bestdiv(struct jlq_div_table *table,
		unsigned long prate, unsigned long rate, unsigned long flags)
{
	const struct jlq_div_table *ptbl = table;
	int i, idx = 0;
	unsigned long now, gap = ULONG_MAX;
	unsigned int f;

	if (rate > prate) {
		pr_debug("[%s] rate(%lu) > prate(%lu)!\n",
						__func__, rate, prate);
		return idx;
	}

	for (i = 0; ptbl[i].div != 0; i++) {
		f = gcd(ptbl[i].div, ptbl[i].grp);
		now = prate / (ptbl[i].div/f) * (ptbl[i].grp/f);

		if (jlq_clk_better_rate(rate, now, gap, flags)) {
			idx = i;
			gap = abs(now - rate);
		}
	}

	return idx;
}

static bool jlq_clkdiv_table_get_grp_div(struct jlq_div_table *table,
		unsigned int val, unsigned int *pgrp, unsigned int *pdiv)
{
	const struct jlq_div_table *ptbl = NULL;

	for (ptbl = table; ptbl->div; ptbl++) {
		if (ptbl->val == val) {
			*pgrp = ptbl->grp;
			*pdiv = ptbl->div;
			return RET_FOUND;
		}
	}

	return RET_MISS;
}

static int jlq_clkdiv_table_init(struct jlq_div_table *ptbl, unsigned int mask,
		struct table_param *pprm)
{
	int val, i;
	int min = (pprm->min_max[0] == -1) ? 0 : pprm->min_max[0];
	int max = (pprm->min_max[0] == -1) ? mask : pprm->min_max[1];
	enum ord val_ord = ORD_NONE;

	if (pprm->flags & FLAG_DIV_DESCEND) { /*request div descend seq.*/
		if (pprm->def_div_ord == ORD_DES)
			val_ord = ORD_AS;
		else if (pprm->def_div_ord == ORD_AS)
			val_ord = ORD_DES;
	} else { /*request div ascend seq.*/
		if (pprm->def_div_ord == ORD_DES)
			val_ord = ORD_DES;
		else if (pprm->def_div_ord == ORD_AS)
			val_ord = ORD_AS;
	}

	switch (val_ord) {
	case ORD_DES:
		for (i = 0, val = max; val >= min; val--) {
			if ((pprm->flags & FLAG_VAL_ODD) && !(val % 2))
				continue;
			(pprm->formula)(val, mask, &(ptbl[i]));
			i += 1;
		}
		break;

	case ORD_AS:
		for (i = 0, val = min; val <= max; val++) {
			if ((pprm->flags & FLAG_VAL_ODD) && !(val % 2))
				continue;
			(pprm->formula)(val, mask, &(ptbl[i]));
			i += 1;
		}
		break;

	default:
		panic("unkowned val order for clk div table\n");
	}

	return 0;
}

static unsigned long jlq_clkdiv_recalc_rate(struct clk_hw *hw,
					unsigned long parent_rate)
{
	struct jlq_divclk *dclk = container_of(hw, struct jlq_divclk, hw);
	unsigned int val, ret;
	unsigned int grp = 0;
	unsigned int div = 0;
	unsigned int f;
	unsigned long rate = 0;

	val = readl(dclk->reg);
	val &= (dclk->mask << dclk->shift);
	val = val >> dclk->shift;

	ret = jlq_clkdiv_table_get_grp_div(dclk->table, val, &grp, &div);
	if (ret) {
		pr_warn("%s: Invalid divisor(%d) for clock %s\n", __func__,
			val, __clk_get_name(hw->clk));
		return parent_rate;
	}

	f = gcd(div, grp);
	rate = (parent_rate / (div/f) * (grp/f));

	clk_debug("%s: %s, parent_rate(%ld), calc_rate(%ld)\n",
		__func__, __clk_get_name(hw->clk), parent_rate, rate);
	return rate;
}

static long jlq_clkdiv_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *prate)
{
	struct jlq_divclk *dclk = container_of(hw, struct jlq_divclk, hw);
	struct jlq_div_table *ptbl = dclk->table;
	struct clk_hw *clk_parent = clk_hw_get_parent(hw);
	int i, bestidx = -1;
	unsigned long parent_rate, best = 0, now;
	unsigned long gap = ULONG_MAX;
	unsigned int f;
	unsigned long max_rate = 0, min_rate = ULONG_MAX;
	unsigned long max_idx = 0, min_idx = 0;
	unsigned long round = 0, idx = 0;

	if (!rate) {
		pr_warn("[%s]:%s rate is Zero!\n", __func__,
			(char *)clk_hw_get_name(hw));
		rate = 1;
	}

	if (!(clk_hw_get_flags(hw) & CLK_SET_RATE_PARENT)) {
		bestidx = jlq_clkdiv_table_get_bestdiv(ptbl, *prate,
						rate, dclk->flags);
		f = gcd(ptbl[bestidx].div, ptbl[bestidx].grp);
		round = (*prate) / (ptbl[bestidx].div/f) * (ptbl[bestidx].grp/f);

		clk_debug("[%s]: %s(%ld),bestidx(%d),rate(%ld),prate(%d)\n", __func__,
			clk_hw_get_name(hw), round, bestidx, rate, *prate);

		return round;
	}

	for (i = 0; ptbl[i].div != 0; i++) {
		if (rate > (ULONG_MAX / ptbl[i].div * ptbl[i].grp))
			continue;

		f = gcd(ptbl[i].div, ptbl[i].grp);

		if ((clk_hw_get_flags(hw) & CLK_SET_RATE_PARENT))
			parent_rate = clk_hw_round_rate(clk_parent,
				JLQ_ROUND_UP(rate, (ptbl[i].grp/f), (ptbl[i].div/f)));
		else
			parent_rate = *prate;

		now = parent_rate / (ptbl[i].div/f) * (ptbl[i].grp/f);

		if (now >= max_rate)
			max_idx = i;
		if (now <= min_rate)
			min_idx = i;
		max_rate = max(now, max_rate);
		min_rate = min(now, min_rate);

		if (jlq_clk_better_rate(rate, now, gap, dclk->flags)) {
			bestidx = i;
			best = now;
			*prate = parent_rate;
			gap = abs(now - rate);
		}
	}

	if (-1 == bestidx) {
		round = (dclk->flags & FLAG_RATE_HIGHER) ? max_rate : min_rate;
		idx = (dclk->flags & FLAG_RATE_HIGHER) ? max_idx : min_idx;
		*prate = clk_hw_round_rate(clk_parent,
			JLQ_ROUND_UP(round, (ptbl[idx].grp/f), (ptbl[idx].div/f)));
		clk_debug("[%s] no match %ld,max or min.%s(%ld),prate(%ld)\n",
			__func__, rate, clk_hw_get_name(hw), round, *prate);
		return round;
	}

	f = gcd(ptbl[bestidx].div, ptbl[bestidx].grp);
	round = (*prate) / (ptbl[bestidx].div/f) * (ptbl[bestidx].grp/f);

	clk_debug("%s: %s, parent_rate(%ld), round_rate(%ld)\n",
		__func__, __clk_get_name(hw->clk), *prate, round);

	return round;
}

static inline void jlq_clkdiv_set_rate_reg(struct jlq_divclk *dclk,
				unsigned int val, unsigned int dly_us)
{
	if (dclk->weshift)
		val |= dclk->mask << dclk->weshift;

	writel(val, dclk->reg);
	if (dly_us)
		udelay(dly_us);
}

static int jlq_clkdiv_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct jlq_divclk *dclk = container_of(hw, struct jlq_divclk, hw);
	unsigned int idx, val_0, val;
	int ret = 0;
	unsigned long flags = 0;


	idx = jlq_clkdiv_table_get_bestdiv(dclk->table, parent_rate,
						rate, dclk->flags);

	if (dclk->lock)
		spin_lock_irqsave(dclk->lock, flags);

	/* Some enable bit and div in the same reg, such as DDR_PWR_TSCLKCTL! */
	val_0 = readl(dclk->reg);
	val_0 &= ~(dclk->mask << dclk->shift);
	val = val_0 | ((dclk->table)[idx].val << dclk->shift);
	jlq_clkdiv_set_rate_reg(dclk, val, 0);

	if (dclk->lock)
		spin_unlock_irqrestore(dclk->lock, flags);

	clk_debug("%s: %s, parent_rate(%ld), set_rate(%ld)\n",
		__func__, __clk_get_name(hw->clk), parent_rate, rate);
	return ret;
}

static const struct clk_ops jlq_clkdiv_ops = {
	.recalc_rate = jlq_clkdiv_recalc_rate,
	.round_rate = jlq_clkdiv_round_rate,
	.set_rate = jlq_clkdiv_set_rate,
	.debug_init = jlq_clk_debug_init,
};

static void __init jlq_clkdiv_setup(struct device_node *np)
{
	struct clk *clk;
	const char *clk_name, **parent_names;
	struct clk_init_data *init;
	struct jlq_div_table *table;
	struct jlq_divclk *dclk;
	void __iomem *reg_base;
	unsigned int data[4] = {0};
	struct table_param param = {0};

	param.flags = 0;
	param.min_max[0] = param.min_max[1] = -1;
	param.def_div_ord = ORD_AS;
	param.formula = NULL;

	reg_base = jlq_clk_get_base(np);
	if (!reg_base) {
		pr_err("[%s] %s fail to get reg_base!\n", __func__, np->name);
		return;
	}

	if (of_property_read_string(np, "clock-output-names", &clk_name)) {
		pr_err("[%s] node %s doesn't have clock-output-names property!\n",
			__func__, np->name);
		return;
	}

	if (of_property_read_u32_array(np, "jlq,clkdiv", &data[0], 4)) {
		pr_err("[%s] node %s doesn't have jlq,clkdiv property!\n",
			__func__, np->name);
		return;
	}

	if (of_property_read_u32_array(np, "jlq,val_min_max",
					&(param.min_max[0]), 2)) {
		pr_err("[%s] node %s doesn't have jlq,val_min_max property!\n",
			__func__, np->name);
		return;
	}

	/* table range [0, mask] and end with {0, 0, 0}*/
	table = kzalloc(sizeof(struct jlq_div_table) * (data[1] + 2),
				GFP_KERNEL);
	if (!table)
		goto err_out;

	if (of_get_property(np, "jlq,cdiv", NULL)) {
		param.formula = jlq_formula_cdiv;
		param.def_div_ord = ORD_AS;
	} else if (of_get_property(np, "jlq,gdiv", NULL)) {
		param.formula = jlq_formula_gdiv;
		param.def_div_ord = ORD_DES;
	}

	if (jlq_clkdiv_table_init(table, data[1], &param))
		return;

	/* fixed parent */
	parent_names = kzalloc(sizeof(char *), GFP_KERNEL);
	if (!parent_names)
		goto err_par;

	parent_names[0] = of_clk_get_parent_name(np, 0);

	dclk = kzalloc(sizeof(struct jlq_divclk), GFP_KERNEL);
	if (!dclk)
		goto err_dclk;

	init = kzalloc(sizeof(struct clk_init_data), GFP_KERNEL);
	if (!init)
		goto err_init;

	init->name = clk_name;
	init->ops = &jlq_clkdiv_ops;
	init->parent_names = parent_names;
	init->num_parents = 1;
	if (of_get_property(np, "set_rate_parent", NULL))
		init->flags |= CLK_SET_RATE_PARENT;
	if (of_get_property(np, "clk_is_critical", NULL))
		init->flags |= CLK_IS_CRITICAL;

	dclk->reg = reg_base + data[0];
	dclk->mask    = data[1];
	dclk->shift   = data[2];
	dclk->weshift = data[3];
	dclk->lock = &jlq_clk.lock;
	dclk->hw.init = init;
	dclk->table = table;
	dclk->flags = 0;

	if (of_get_property(np, "set_rate_higher", NULL))
		dclk->flags |= FLAG_RATE_HIGHER;
	else if (of_get_property(np, "set_rate_lower", NULL))
		dclk->flags |= FLAG_RATE_LOWER;
	else if (of_get_property(np, "set_rate_closer", NULL))
		dclk->flags |= FLAG_RATE_CLOSER;
	else
		dclk->flags |= FLAG_RATE_HIGHER;

	clk = clk_register(NULL, &(dclk->hw));
	if (IS_ERR(clk)) {
		pr_err("[%s] fail to register div clk %s!\n",
			__func__, clk_name);
		goto err_clk;
	}

	clk_register_clkdev(clk, clk_name, NULL);
	of_clk_add_provider(np, of_clk_src_simple_get, clk);
	kfree(init);
	kfree(parent_names);
	return;

err_clk:
	kfree(init);
err_init:
	kfree(dclk);
err_dclk:
	kfree(parent_names);
err_par:
	kfree(table);
err_out:
	pr_err("[%s] fail!\n", __func__);
}

static int jlq_clkgate_enable(struct clk_hw *hw)
{
	struct jlq_gateclk *gclk;
	unsigned long flags = 0;
	int val = 0;

	gclk = container_of(hw, struct jlq_gateclk, hw);
	if (gclk->reg == NULL) {
		pr_err("%s: reg is NULL for clock %s\n", __func__,
			__clk_get_name(hw->clk));
		WARN_ON(1);
		return 0;
	}

	if (gclk->lock)
		spin_lock_irqsave(gclk->lock, flags);

	val = readl(gclk->reg);
	if (val & (1 << gclk->shift))
		goto unlock;

	val |= (1 << gclk->shift);
	if (gclk->weshift)
		val |= (1 << gclk->weshift);
	writel(val, gclk->reg);

unlock:
	if (gclk->lock)
		spin_unlock_irqrestore(gclk->lock, flags);

	clk_debug("%s: %s\n",
		__func__, __clk_get_name(hw->clk));
	return 0;
}

static void jlq_clkgate_disable(struct clk_hw *hw)
{
	struct jlq_gateclk *gclk;
	unsigned long flags = 0;
	int val = 0;

	gclk = container_of(hw, struct jlq_gateclk, hw);
	if (gclk->reg == NULL) {
		pr_err("%s: reg is NULL for clock %s\n", __func__,
			__clk_get_name(hw->clk));
		WARN_ON(1);
		return;
	}

	if (gclk->lock)
		spin_lock_irqsave(gclk->lock, flags);

	val = readl(gclk->reg);
	if (!(val & (1 << gclk->shift)))
		goto unlock;

	val &= ~(1 << gclk->shift);
	if (gclk->weshift)
		val |= (1 << gclk->weshift);
	writel(val, gclk->reg);

unlock:
	if (gclk->lock)
		spin_unlock_irqrestore(gclk->lock, flags);
	clk_debug("%s: %s\n",
		__func__, __clk_get_name(hw->clk));
}

static int __maybe_unused jlq_clkgate_is_enabled(struct clk_hw *hw)
{
	struct jlq_gateclk *gclk;
	unsigned long flags = 0;
	unsigned int val = 0;

	gclk = container_of(hw, struct jlq_gateclk, hw);
	if (gclk->reg == NULL) {
		pr_err("%s: reg is NULL for clock %s\n", __func__,
			__clk_get_name(hw->clk));
		WARN_ON(1);
		return -EINVAL;
	}

	if (gclk->lock)
		spin_lock_irqsave(gclk->lock, flags);

	val = readl(gclk->reg);
	val &= (1 << gclk->shift);

	if (gclk->lock)
		spin_unlock_irqrestore(gclk->lock, flags);

	clk_debug("%s: %s\n",
		__func__, __clk_get_name(hw->clk));

	return val ? 1 : 0;
}

static const struct clk_ops jlq_clkgate_ops = {
	.enable     = jlq_clkgate_enable,
	.disable    = jlq_clkgate_disable,
	.debug_init = jlq_clk_debug_init,
	//.is_enabled = jlq_clkgate_is_enabled,
};

static void __init jlq_clkgate_setup(struct device_node *np)
{
	struct jlq_gateclk *gclk;
	struct clk_init_data *init;
	struct clk *clk;
	const char *clk_name, **parent_names;
	void __iomem *reg_base;
	unsigned int data[3] = {0};

	reg_base = jlq_clk_get_base(np);
	if (!reg_base) {
		pr_err("[%s] %s fail to get reg_base!\n", __func__, np->name);
		return;
	}

	if (of_property_read_string(np, "clock-output-names", &clk_name)) {
		pr_err("[%s] %s node doesn't have clock-output-name property!\n",
			__func__, np->name);
		return;
	}

	if (of_property_read_u32_array(np, "jlq,clkgt", &data[0], 3)) {
		pr_err("[%s] %s node doesn't have jlq,clkgt property!\n",
			__func__, np->name);
		return;
	}
	/* fixed parent */
	parent_names = kzalloc(sizeof(char *), GFP_KERNEL);
	if (!parent_names)
		goto err_out;

	parent_names[0] = of_clk_get_parent_name(np, 0);

	gclk = kzalloc(sizeof(struct jlq_gateclk), GFP_KERNEL);
	if (!gclk)
		goto err_gclk;

	init = kzalloc(sizeof(struct clk_init_data), GFP_KERNEL);
	if (!init)
		goto err_init;

	init->name = clk_name;
	init->ops = &jlq_clkgate_ops;
	init->flags = CLK_IGNORE_UNUSED;
	init->parent_names = parent_names;
	init->num_parents = 1;
	if (of_get_property(np, "set_rate_parent", NULL))
		init->flags |= CLK_SET_RATE_PARENT;
	if (of_get_property(np, "clk_is_critical", NULL))
		init->flags |= CLK_IS_CRITICAL;

	gclk->reg = reg_base + data[0];
	gclk->shift = data[1];
	gclk->weshift = data[2];
	gclk->lock = &jlq_clk.lock;
	gclk->hw.init = init;

	clk = clk_register(NULL, &(gclk->hw));
	if (IS_ERR(clk)) {
		pr_err("[%s] fail to reigister clk %s!\n",
			__func__, clk_name);
		goto err_clk;
	}

	clk_register_clkdev(clk, clk_name, NULL);
	of_clk_add_provider(np, of_clk_src_simple_get, clk);
	kfree(init);
	kfree(parent_names);
	return;

err_clk:
	kfree(init);
err_init:
	kfree(gclk);
err_gclk:
	kfree(parent_names);
err_out:
	pr_err("[%s] fail!\n", __func__);
}

static unsigned long jlq_clkfdiv_recalc_rate(struct clk_hw *hw,
				unsigned long parent_rate)
{
	struct jlq_fdivclk *fdclk = container_of(hw, struct jlq_fdivclk, hw);
	unsigned int div, mul, val;
	unsigned long rate = 0;

	if (fdclk->table == NULL) {
		pr_err("[%s] fail to get fdiv table!\n", __func__);
		goto out;
	}

	val = readl(fdclk->reg);
	div = (val & (fdclk->divmask << fdclk->divshift)) >> fdclk->divshift;
	mul = (val & (fdclk->mulmask << fdclk->mulshift)) >> fdclk->mulshift;

	rate = (parent_rate / 2) * mul / div;
	if (!rate) {
		pr_err("%s: Invalid factor for clock %s\n", __func__,
			__clk_get_name(hw->clk));
		return parent_rate;
	}

out:
	clk_debug("%s: %s, parent_rate(%ld), calc_rate(%ld)\n",
		__func__, __clk_get_name(hw->clk), parent_rate, rate);
	return rate;
}

static long jlq_clkfdiv_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *prate)
{
	struct jlq_fdivclk *fdclk = container_of(hw, struct jlq_fdivclk, hw);
	const struct jlq_fdiv_table *clkft;
	unsigned long round = 0;
	unsigned long best = 0;
	unsigned long gap = ULONG_MAX;
	unsigned long max_rate = 0, min_rate = ULONG_MAX;

	for (clkft = fdclk->table; clkft->rate; clkft++) {
		max_rate = max(clkft->rate, max_rate);
		min_rate = min(clkft->rate, min_rate);
		if (jlq_clk_better_rate(rate, clkft->rate,
			       gap, fdclk->flags)) {
			best = clkft->rate;
			gap = abs(clkft->rate - rate);
		}
	}

	if (best == 0) {
		clk_debug("[%s] no match %ld,max or min.%s(%ld)\n",
			__func__, rate, clk_hw_get_name(hw),
			((fdclk->flags & FLAG_RATE_HIGHER) ? max_rate : min_rate));
		return (fdclk->flags & FLAG_RATE_HIGHER) ? max_rate : min_rate;
	}
	round = best;
	clk_debug("%s: %s, parent_rate(%ld), round_rate(%ld)\n",
		__func__, __clk_get_name(hw->clk), *prate, round);
	return round;
}

static int jlq_clkfdiv_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct jlq_fdivclk *fdclk = container_of(hw, struct jlq_fdivclk, hw);
	const struct jlq_fdiv_table *clkft;
	unsigned int val;
	unsigned long flags = 0;

	if (fdclk->table == NULL) {
		pr_err("[%s] fail to get fdiv table!\n", __func__);
		goto out;
	}

	for (clkft = fdclk->table; clkft->rate && clkft->rate != rate; clkft++)
		;

	if (!clkft->div) {
		pr_err("[%s] no match item in fdiv table!\n", __func__);
		goto out;
	}

	val = ((clkft->div & fdclk->divmask) << fdclk->divshift) |
	((clkft->mul & fdclk->mulmask) << fdclk->mulshift);

	if (fdclk->lock)
		spin_lock_irqsave(fdclk->lock, flags);

	writel(val, fdclk->reg);

	if (fdclk->lock)
		spin_unlock_irqrestore(fdclk->lock, flags);

out:
	clk_debug("%s: %s, parent_rate(%ld), set_rate(%ld)\n",
		__func__, __clk_get_name(hw->clk), parent_rate, rate);
	return 0;
}

static int jlq_clkfdiv_table_init(struct device_node *np,
				struct jlq_fdivclk *fdclk)
{
	int prop_len, i;
	unsigned int *array;
	struct jlq_fdiv_table *table;

	if (!of_find_property(np, "jlq,clkfdiv-ftbl", &prop_len)) {
		pr_err("[%s] %s fail to of_find_property!\n",
			 __func__, np->name);
		return -EINVAL;
	}
	prop_len /= sizeof(u32);
	if (prop_len % 4) {
		pr_err("[%s] bad length %d!\n", __func__, prop_len);
		return -EINVAL;
	}

	prop_len /= 4;

	fdclk->table = kzalloc((prop_len+1) * sizeof(struct jlq_fdiv_table),
						GFP_KERNEL);

	if (!fdclk->table)
		goto err_table;

	array = kzalloc(prop_len * 4 * sizeof(u32), GFP_KERNEL);
	if (!array)
		goto err_array;

	if (of_property_read_u32_array(np, "jlq,clkfdiv-ftbl",
			array, prop_len * 4)) {
		pr_err("[%s] %s doesn't have jlq,clkfdiv-ftbl property!\n",
			__func__, np->name);
		goto err_prop;
	}

	table = fdclk->table;
	for (i = 0; i < prop_len; i++) {
		table[i].baud = array[4 * i];
		table[i].rate = array[4 * i + 1];
		table[i].mul = array[4 * i + 2];
		table[i].div = array[4 * i + 3];
	}
	table[i].baud = 0;
	table[i].rate = 0;
	table[i].mul = 0;
	table[i].div = 0;

	kfree(array);
	return 0;
err_prop:
	kfree(array);
err_array:
	kfree(fdclk->table);
err_table:
	pr_err("[%s] fail!\n", __func__);
	return -EINVAL;
}

const struct clk_ops jlq_clkfdiv_ops = {
	.round_rate = jlq_clkfdiv_round_rate,
	.set_rate = jlq_clkfdiv_set_rate,
	.recalc_rate = jlq_clkfdiv_recalc_rate,
	.debug_init = jlq_clk_debug_init,
};

static void __init jlq_clkfdiv_setup(struct device_node *np)
{
	struct jlq_fdivclk *fdclk;
	struct clk_init_data *init;
	struct clk *clk;
	const char *clk_name, **parent_names;
	void __iomem *reg_base;
	unsigned int data[5] = {0};


	reg_base = jlq_clk_get_base(np);
	if (!reg_base) {
		pr_err("[%s] %s fail to get reg_base!\n", __func__, np->name);
		return;
	}

	if (of_property_read_string(np, "clock-output-names", &clk_name)) {
		pr_err("[%s] %s node doesn't have clock-output-name property!\n",
			__func__, np->name);
		return;
	}

	if (of_property_read_u32_array(np, "jlq,clkfdiv", &data[0], 5)) {
		pr_err("[%s] %s node doesn't have jlq,clkfdiv property!\n",
			__func__, np->name);
		return;
	}

	parent_names = kzalloc(sizeof(char *), GFP_KERNEL);
	if (!parent_names)
		goto err_out;

	parent_names[0] = of_clk_get_parent_name(np, 0);

	fdclk = kzalloc(sizeof(struct jlq_fdivclk), GFP_KERNEL);
	if (!fdclk)
		goto err_fdclk;

	init = kzalloc(sizeof(struct clk_init_data), GFP_KERNEL);
	if (!init)
		goto err_init;

	init->name = clk_name;
	init->ops = &jlq_clkfdiv_ops;
	init->parent_names = parent_names;
	init->num_parents = 1;
	if (of_get_property(np, "clk_is_critical", NULL))
		init->flags |= CLK_IS_CRITICAL;

	fdclk->reg = reg_base + data[0];
	fdclk->divmask  = data[1];
	fdclk->divshift = data[2];
	fdclk->mulmask  = data[3];
	fdclk->mulshift = data[4];
	fdclk->lock = &jlq_clk.lock;
	fdclk->hw.init = init;
	fdclk->flags = 0;

	if (of_get_property(np, "set_rate_bigger", NULL))
		fdclk->flags |= FLAG_RATE_HIGHER;
	else if (of_get_property(np, "set_rate_smaller", NULL))
		fdclk->flags |= FLAG_RATE_LOWER;
	else if (of_get_property(np, "set_rate_closer", NULL))
		fdclk->flags |= FLAG_RATE_CLOSER;
	else
		fdclk->flags |= FLAG_RATE_HIGHER;

	if (jlq_clkfdiv_table_init(np, fdclk)) {
		pr_err("[%s] fail to of_get_fdiv_ftbl!\n", __func__);
		goto err_clk;
	}

	clk = clk_register(NULL, &fdclk->hw);
	if (IS_ERR(clk)) {
		pr_err("[%s] fail to reigister clk %s!\n",
			__func__, clk_name);
		goto err_reg;
	}

	clk_register_clkdev(clk, clk_name, NULL);
	of_clk_add_provider(np, of_clk_src_simple_get, clk);
	kfree(init);
	kfree(parent_names);
	return;

err_reg:
	kfree(fdclk->table);
err_clk:
	kfree(init);
err_init:
	kfree(fdclk);
err_fdclk:
	kfree(parent_names);
err_out:
	pr_err("[%s] fail!\n", __func__);
}

static int __maybe_unused jlq_clkpllx_enable(struct clk_hw *hw)
{
	struct jlq_pllxclk *pllclk = container_of(hw, struct jlq_pllxclk, hw);
	unsigned long ktime_a = 0, ktime_b = 0;
	unsigned long flags = 0;
	int val = 0, adj = 0;

	if (pllclk->reg0 == NULL) {
		pr_err("%s: reg is NULL for clock %s\n", __func__,
			__clk_get_name(hw->clk));
		WARN_ON(1);
		return 0;
	}

	if (pllclk->lock)
		spin_lock_irqsave(pllclk->lock, flags);

	val = readl(pllclk->reg0);
	if (!(val & (1 << pllclk->pdshift)))
		goto unlock;

	val |= (1 << pllclk->adjshift);
	if (pllclk->adjweshift)
		val |= (1 << pllclk->adjweshift);
	writel(val, pllclk->reg0);

	val = 0;
	val &= ~(1 << pllclk->pdshift);
	val |= (1 << pllclk->pdweshift);
	writel(val, pllclk->reg0);

	ktime_a = ktime_to_us(ktime_get());
	adj = readl(pllclk->reg0) & (1 << pllclk->adjshift);
	while (adj) {
		cpu_relax();
		adj = readl(pllclk->reg0) & (1 << pllclk->adjshift);
		ktime_b = ktime_to_us(ktime_get());
		if ((ktime_b - ktime_a) > PLL_TIMEOUT_US)
			panic("%s:%s cannot be locked!\n",
				__func__, clk_hw_get_name(hw));
	}

unlock:
	if (pllclk->lock)
		spin_unlock_irqrestore(pllclk->lock, flags);

	clk_debug("%s: %s\n",
		__func__, __clk_get_name(hw->clk));
	return 0;
}

static void __maybe_unused jlq_clkpllx_disable(struct clk_hw *hw)
{
	struct jlq_pllxclk *pllclk = container_of(hw, struct jlq_pllxclk, hw);
	unsigned long flags = 0;
	int val = 0;

	if (pllclk->reg0 == NULL) {
		pr_err("%s: reg is NULL for clock %s\n", __func__,
			__clk_get_name(hw->clk));
		WARN_ON(1);
		return;
	}

	if (pllclk->lock)
		spin_lock_irqsave(pllclk->lock, flags);

	val = readl(pllclk->reg0);
	if ((val & (1 << pllclk->pdshift)))
		goto unlock;

	val |= (1 << pllclk->pdshift);
	if (pllclk->pdweshift)
		val |= (1 << pllclk->pdweshift);
	writel(val, pllclk->reg0);

unlock:
	if (pllclk->lock)
		spin_unlock_irqrestore(pllclk->lock, flags);
	clk_debug("%s: %s\n",
		__func__, __clk_get_name(hw->clk));
}

static long jlq_clkpllx_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *prate)
{
	struct jlq_pllxclk *pllclk = container_of(hw, struct jlq_pllxclk, hw);
	struct jlq_pllx_table *clkpt;
	unsigned long round = 0;
	unsigned long best = 0;
	unsigned long gap = ULONG_MAX;
	unsigned long max_rate = 0, min_rate = ULONG_MAX;

	for (clkpt = pllclk->table; clkpt->rate; clkpt++) {
		max_rate = max(clkpt->rate, max_rate);
		min_rate = min(clkpt->rate, min_rate);
		if (jlq_clk_better_rate(rate, clkpt->rate,
				gap, pllclk->flags)) {
			best = clkpt->rate;
			gap = abs(clkpt->rate - rate);
		}
	}
	if (best == 0) {
		clk_debug("[%s] no match %ld,max or min.%s(%ld)\n",
			__func__, rate, clk_hw_get_name(hw),
			((pllclk->flags & FLAG_RATE_HIGHER) ? max_rate : min_rate));
		return (pllclk->flags & FLAG_RATE_HIGHER) ? max_rate : min_rate;
	}
	round = best;
	clk_debug("%s: %s, parent_rate(%ld), round_rate(%ld)\n",
		__func__, __clk_get_name(hw->clk), *prate, round);
	return round;
}

static unsigned long jlq_clkpll43x_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct jlq_pllxclk *pllclk = container_of(hw, struct jlq_pllxclk, hw);
	struct jlq_pllx_table *clkpt;
	unsigned int p = 0, m = 0, s = 0;
	unsigned int val1 = 0, val2 = 0;
	unsigned short k = 0;
	unsigned long rate = 0;

	val1 = readl(pllclk->reg1);
	val2 = readl(pllclk->reg2);

	p = (val1 >> pllclk->pshift) & (pllclk->pmask);
	m = (val1 >> pllclk->mshift) & (pllclk->mmask);
	k = (val1 >> pllclk->kshift) & (pllclk->kmask);

	s = (val2 >> pllclk->sshift) & (pllclk->smask);

	for (clkpt = pllclk->table; clkpt->rate; clkpt++)
		if ((clkpt->p == p) && (clkpt->m == m)
				&& (clkpt->k == k) && (clkpt->s == s))
			break;
	rate = clkpt->rate;

	clk_debug("%s: %s, parent_rate(%ld), calc_rate(%ld)\n",
		__func__, __clk_get_name(hw->clk), parent_rate, rate);
	return rate;
}

static int jlq_clkpll43x_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct jlq_pllxclk *pllclk = container_of(hw, struct jlq_pllxclk, hw);
	struct jlq_pllx_table *clkpt;
	unsigned int val0 = 0, val1 = 0, val2 = 0, adj = 0, pd = 0;
	unsigned long flags = 0;
	unsigned int i = 0;
	unsigned long ktime_a = 0, ktime_b = 0;

	for (clkpt = pllclk->table; clkpt->rate && clkpt->rate != rate; clkpt++)
		i++;

	if (!clkpt->rate) {
		pr_err("[%s] no match item in pll table!\n", __func__);
		goto out;
	}

	if (pllclk->lock)
		spin_lock_irqsave(pllclk->lock, flags);

	val0 = readl(pllclk->reg0);
	pd = val0 >> pllclk->pdshift;

	val1 = readl(pllclk->reg1);
	val1 &= ~(pllclk->pmask << pllclk->pshift);
	val1 &= ~(pllclk->mmask << pllclk->mshift);
	val1 &= ~(pllclk->kmask << pllclk->kshift);

	val2 = readl(pllclk->reg2);
	val2 &= ~(pllclk->smask << pllclk->sshift);

	val1 |= (clkpt->p & pllclk->pmask) << pllclk->pshift;
	val1 |= (clkpt->m & pllclk->mmask) << pllclk->mshift;
	val1 |= (clkpt->k & pllclk->kmask) << pllclk->kshift;

	val2 |= (clkpt->s & pllclk->smask) << pllclk->sshift;

	writel(val1, pllclk->reg1);
	writel(val2, pllclk->reg2);

	val0 |= (1 << pllclk->adjshift);
	if (pllclk->adjweshift)
		val0 |= (1 << pllclk->adjweshift);
	writel(val0, pllclk->reg0);

	if (pd) {
		val0 = 0;//TBD, pd & adj not at the same time
		val0 &= ~(1 << pllclk->pdshift);
		val0 |= (1 << pllclk->pdweshift);
		writel(val0, pllclk->reg0);
	}

	ktime_a = ktime_to_us(ktime_get());
	adj = readl(pllclk->reg0) & (1 << pllclk->adjshift);
	while (adj) {
		cpu_relax();
		adj = readl(pllclk->reg0) & (1 << pllclk->adjshift);
		ktime_b = ktime_to_us(ktime_get());
		if ((ktime_b - ktime_a) > PLL_TIMEOUT_US)
			panic("%s:%s cannot be locked!\n",
				__func__, clk_hw_get_name(hw));
	}

	if (pllclk->lock)
		spin_unlock_irqrestore(pllclk->lock, flags);

out:
	clk_debug("%s: %s, parent_rate(%ld), set_rate(%ld)\n",
		__func__, __clk_get_name(hw->clk), parent_rate, rate);
	return 0;
}

static void jlq_pll_add_freq_entry(struct jlq_pllx_table *table, int max_entries,
				   unsigned long rate, unsigned int p,
				   unsigned int m, unsigned short k, unsigned int s)
{
	int i;
	for (i = 0; i < max_entries && table[i].rate; i++) {
		if (table[i].rate == rate)
			return; /* already present */
	}
	if (i < max_entries - 1) {
		table[i].rate = rate;
		table[i].p = p;
		table[i].m = m;
		table[i].k = k;
		table[i].s = s;
		table[i+1].rate = 0;
	}
}

static int jlq_clkpll43x_table_init(struct device_node *np,
					struct jlq_pllxclk *pllclk)
{
	int prop_len, i;
	unsigned int *array;
	struct jlq_pllx_table *table;

	if (!of_find_property(np, "jlq,clkpllx-ftbl", &prop_len)) {
		pr_err("[%s] fail to of_find_property!\n", __func__);
		return -EINVAL;
	}

	prop_len /= sizeof(u32);
	if (prop_len % 5) {
		pr_err("[%s] bad length %d!\n", __func__, prop_len);
		return -EINVAL;
	}

	prop_len /= 5;

	pllclk->table = kzalloc((prop_len + 5) * sizeof(struct jlq_pllx_table),
						GFP_KERNEL);

	if (!pllclk->table)
		goto err_table;

	array = kzalloc(prop_len * 5 * sizeof(u32), GFP_KERNEL);
	if (!array)
		goto err_array;

	if (of_property_read_u32_array(np, "jlq,clkpllx-ftbl",
				array, prop_len * 5)) {
		pr_err("[%s] %s doesn't have jlq,clkpllx-ftbl property!\n",
			__func__, np->name);
		goto err_prop;
	}

	table = pllclk->table;
	for (i = 0; i < prop_len; i++) {
		table[i].rate = array[5 * i];
		table[i].p = array[5 * i + 1];
		table[i].m = array[5 * i + 2];
		table[i].k = array[5 * i + 3];
		table[i].s = array[5 * i + 4];
	}
	table[i].rate = 0;
	table[i].p = 0;
	table[i].m = 0;
	table[i].k = 0;
	table[i].s = 0;

	/* Add overclock frequencies dynamically for app control */
	if (np->name && (!strcmp(np->name, "pll_cpu_big_out") || strstr(np->name, "cpu_big"))) {
		jlq_pll_add_freq_entry(table, prop_len + 5, 2100000000UL, 1, 109, 24576, 0);
		jlq_pll_add_freq_entry(table, prop_len + 5, 2200000000UL, 1, 114, 38229, 0);
	} else if (np->name && (!strcmp(np->name, "pll_cpu_little_out") || strstr(np->name, "cpu_little"))) {
		jlq_pll_add_freq_entry(table, prop_len + 5, 1600000000UL, 1, 166, 43690, 1);
	}

	kfree(array);

	return 0;
err_prop:
	kfree(array);
err_array:
	kfree(pllclk->table);
err_table:
	pr_err("[%s] fail!\n", __func__);
	return -EINVAL;
}

const struct clk_ops jlq_clkpll43x_ops = {
	.enable     = jlq_clkpllx_enable,
	.disable    = jlq_clkpllx_disable,
	.set_rate = jlq_clkpll43x_set_rate,
	.recalc_rate = jlq_clkpll43x_recalc_rate,
	.round_rate = jlq_clkpllx_round_rate,
	.debug_init = jlq_clk_debug_init,
};

static void __init jlq_pll43x_setup(struct device_node *np)
{
	struct jlq_pllxclk *pllclk;
	struct clk_init_data *init;
	struct clk *clk;
	const char *clk_name, **parent_names;
	void __iomem *reg_base;
	unsigned int data0[5] = {0};
	unsigned int data1[7] = {0};
	unsigned int data2[3] = {0};


	reg_base = jlq_clk_get_base(np);
	if (!reg_base) {
		pr_err("[%s] %s fail to get reg_base!\n", __func__, np->name);
		return;
	}

	if (of_property_read_string(np, "clock-output-names", &clk_name)) {
		pr_err("[%s] %s node doesn't have clock-output-name property!\n",
			__func__, np->name);
		return;
	}

	if (of_property_read_u32_array(np, "jlq,clkpllx-cfg0", &data0[0], 5)) {
		pr_err("[%s] %s node doesn't have jlq,clkpllx-cfg0 property!\n",
			__func__, np->name);
		return;
	}

	if (of_property_read_u32_array(np, "jlq,clkpllx-cfg1", &data1[0], 7)) {
		pr_err("[%s] %s node doesn't have jlq,clkpllx-cfg1 property!\n",
			__func__, np->name);
		return;
	}

	if (of_property_read_u32_array(np, "jlq,clkpllx-cfg2", &data2[0], 3)) {
		pr_err("[%s] %s node doesn't have jlq,clkpllx-cfg2 property!\n",
			__func__, np->name);
		return;
	}

	parent_names = kzalloc(sizeof(char *), GFP_KERNEL);
	if (!parent_names)
		goto err_out;

	parent_names[0] = of_clk_get_parent_name(np, 0);

	pllclk = kzalloc(sizeof(struct jlq_pllxclk), GFP_KERNEL);
	if (!pllclk)
		goto err_pllclk;

	init = kzalloc(sizeof(struct clk_init_data), GFP_KERNEL);
	if (!init)
		goto err_init;

	init->name = clk_name;
	init->ops = &jlq_clkpll43x_ops;
	init->parent_names = parent_names;
	init->num_parents = 1;
	if (of_get_property(np, "clk_is_critical", NULL))
		init->flags |= CLK_IS_CRITICAL;

	pllclk->reg0 = reg_base + data0[0];
	pllclk->reg1 = reg_base + data1[0];
	pllclk->reg2 = reg_base + data2[0];
	pllclk->lock = &jlq_clk.lock;
	pllclk->hw.init = init;

	pllclk->pdshift = data0[1];
	pllclk->pdweshift = data0[2];
	pllclk->adjshift = data0[3];
	pllclk->adjweshift = data0[4];

	pllclk->kshift = data1[1];
	pllclk->kmask = data1[2];
	pllclk->mshift = data1[3];
	pllclk->mmask = data1[4];
	pllclk->pshift = data1[5];
	pllclk->pmask = data1[6];

	pllclk->sshift = data2[1];
	pllclk->smask  = data2[2];

	if (of_get_property(np, "set_rate_bigger", NULL))
		pllclk->flags |= FLAG_RATE_HIGHER;
	else if (of_get_property(np, "set_rate_smaller", NULL))
		pllclk->flags |= FLAG_RATE_LOWER;
	else if (of_get_property(np, "set_rate_closer", NULL))
		pllclk->flags |= FLAG_RATE_CLOSER;
	else
		pllclk->flags |= FLAG_RATE_HIGHER;

	if (jlq_clkpll43x_table_init(np, pllclk)) {
		pr_err("[%s] fail to of_get_pllx_ftbl!\n", __func__);
		goto err_clk;
	}

	clk = clk_register(NULL, &pllclk->hw);
	if (IS_ERR(clk)) {
		pr_err("[%s] fail to reigister clk %s!\n",
			__func__, clk_name);
		goto err_reg;
	}

	clk_register_clkdev(clk, clk_name, NULL);
	of_clk_add_provider(np, of_clk_src_simple_get, clk);
	kfree(init);
	kfree(parent_names);
	return;

err_reg:
	kfree(pllclk->table);
err_clk:
	kfree(init);
err_init:
	kfree(pllclk);
err_pllclk:
	kfree(parent_names);
err_out:
	pr_err("[%s] fail!\n", __func__);
}

static unsigned long jlq_clkpll80x_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct jlq_pllxclk *pllclk = container_of(hw, struct jlq_pllxclk, hw);
	unsigned int p = 0, m = 0, s = 0;
	unsigned int val0 = 0, val1 = 0;
	unsigned long rate = 0;

	val0 = readl(pllclk->reg0);
	val1 = readl(pllclk->reg1);

	p = (val0 >> pllclk->pshift) & (pllclk->pmask);
	m = (val0 >> pllclk->mshift) & (pllclk->mmask);
	s = (val0 >> pllclk->sshift) & (pllclk->smask);

	rate = (parent_rate / p) * (2 * m);
	rate = rate >> s;

	clk_debug("%s: %s, parent_rate(%ld), calc_rate(%ld)\n",
		__func__, __clk_get_name(hw->clk), parent_rate, rate);
	return rate;
}

static int jlq_clkpll80x_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct jlq_pllxclk *pllclk = container_of(hw, struct jlq_pllxclk, hw);
	struct jlq_pllx_table *clkpt;
	unsigned int val0 = 0, val1 = 0, adj = 0, pd = 0;
	unsigned long flags = 0;
	unsigned int i = 0;
	unsigned long ktime_a = 0, ktime_b = 0;

	for (clkpt = pllclk->table; clkpt->rate && clkpt->rate != rate; clkpt++)
		i++;

	if (!clkpt->rate) {
		pr_err("[%s] no match item in pll table!\n", __func__);
		goto out;
	}

	if (pllclk->lock)
		spin_lock_irqsave(pllclk->lock, flags);

	val0 = readl(pllclk->reg0);
	val0 &= ~(pllclk->pmask << pllclk->pshift);
	val0 &= ~(pllclk->mmask << pllclk->mshift);
	val0 &= ~(pllclk->smask << pllclk->sshift);

	val1 = readl(pllclk->reg1);
	pd = val1 >> pllclk->pdshift;

	val0 |= (clkpt->p & pllclk->pmask) << pllclk->pshift;
	val0 |= (clkpt->m & pllclk->mmask) << pllclk->mshift;
	val0 |= (clkpt->s & pllclk->smask) << pllclk->sshift;

	writel(val0, pllclk->reg0);

	val1 |= (1 << pllclk->adjshift);
	if (pllclk->adjweshift)
		val1 |= (1 << pllclk->adjweshift);
	writel(val1, pllclk->reg1);

	if (pd) {
		val1 = 0;//TBD, pd & adj not at the same time
		val1 &= ~(1 << pllclk->pdshift);
		val1 |= (1 << pllclk->pdweshift);
		writel(val1, pllclk->reg1);
	}

	ktime_a = ktime_to_us(ktime_get());
	adj = readl(pllclk->reg1) & (1 << pllclk->adjshift);
	while (adj) {
		cpu_relax();
		adj = readl(pllclk->reg1) & (1 << pllclk->adjshift);
		ktime_b = ktime_to_us(ktime_get());
		if ((ktime_b - ktime_a) > PLL_TIMEOUT_US)
			panic("%s:%s cannot lock!\n",
				__func__, clk_hw_get_name(hw));
	}

	if (pllclk->lock)
		spin_unlock_irqrestore(pllclk->lock, flags);

out:
	clk_debug("%s: %s, parent_rate(%ld), set_rate(%ld)\n",
		__func__, __clk_get_name(hw->clk), parent_rate, rate);
	return 0;
}

static int jlq_clkpll80x_table_init(struct device_node *np,
					struct jlq_pllxclk *pllclk)
{
	int prop_len, i;
	unsigned int *array;
	struct jlq_pllx_table *table;

	if (!of_find_property(np, "jlq,clkpllx-ftbl", &prop_len)) {
		pr_err("[%s] fail to of_find_property!\n", __func__);
		return -EINVAL;
	}

	prop_len /= sizeof(u32);
	if (prop_len % 5) {
		pr_err("[%s] bad length %d!\n", __func__, prop_len);
		return -EINVAL;
	}

	prop_len /= 5;

	pllclk->table = kzalloc((prop_len+1) * sizeof(struct jlq_pllx_table),
						GFP_KERNEL);

	if (!pllclk->table)
		goto err_table;

	array = kzalloc(prop_len * 5 * sizeof(u32), GFP_KERNEL);
	if (!array)
		goto err_array;

	if (of_property_read_u32_array(np, "jlq,clkpllx-ftbl",
				array, prop_len * 5)) {
		pr_err("[%s] %s doesn't have jlq,clkpllx-ftbl property!\n",
			__func__, np->name);
		goto err_prop;
	}

	table = pllclk->table;
	for (i = 0; i < prop_len; i++) {
		table[i].rate = array[5 * i];
		table[i].p = array[5 * i + 1];
		table[i].m = array[5 * i + 2];
		table[i].m = array[5 * i + 3];
		table[i].s = array[5 * i + 4];
	}
	table[i].rate = 0;
	table[i].p = 0;
	table[i].m = 0;
	table[i].k = 0;
	table[i].s = 0;

	kfree(array);

	return 0;
err_prop:
	kfree(array);
err_array:
	kfree(pllclk->table);
err_table:
	pr_err("[%s] fail!\n", __func__);
	return -EINVAL;
}

const struct clk_ops jlq_clkpll80x_ops = {
	.set_rate = jlq_clkpll80x_set_rate,
	.recalc_rate = jlq_clkpll80x_recalc_rate,
	.round_rate = jlq_clkpllx_round_rate,
	.debug_init = jlq_clk_debug_init,
};

static void __init jlq_pll80x_setup(struct device_node *np)
{
	struct jlq_pllxclk *pllclk;
	struct clk_init_data *init;
	struct clk *clk;
	const char *clk_name, **parent_names;
	void __iomem *reg_base;
	unsigned int data0[7] = {0};
	unsigned int data1[5] = {0};


	reg_base = jlq_clk_get_base(np);
	if (!reg_base) {
		pr_err("[%s] %s fail to get reg_base!\n", __func__, np->name);
		return;
	}

	if (of_property_read_string(np, "clock-output-names", &clk_name)) {
		pr_err("[%s] %s node doesn't have clock-output-name property!\n",
			__func__, np->name);
		return;
	}

	if (of_property_read_u32_array(np, "jlq,clkpllx-cfg0", &data0[0], 7)) {
		pr_err("[%s] %s node doesn't have jlq,clkpllx-cfg0 property!\n",
			__func__, np->name);
		return;
	}

	if (of_property_read_u32_array(np, "jlq,clkpllx-cfg1", &data1[0], 5)) {
		pr_err("[%s] %s node doesn't have jlq,clkpllx-cfg1 property!\n",
			__func__, np->name);
		return;
	}

	parent_names = kzalloc(sizeof(char *), GFP_KERNEL);
	if (!parent_names)
		goto err_out;

	parent_names[0] = of_clk_get_parent_name(np, 0);

	pllclk = kzalloc(sizeof(struct jlq_pllxclk), GFP_KERNEL);
	if (!pllclk)
		goto err_pllclk;

	init = kzalloc(sizeof(struct clk_init_data), GFP_KERNEL);
	if (!init)
		goto err_init;

	init->name = clk_name;
	init->ops = &jlq_clkpll80x_ops;
	init->parent_names = parent_names;
	init->num_parents = 1;
	if (of_get_property(np, "clk_is_critical", NULL))
		init->flags |= CLK_IS_CRITICAL;

	pllclk->reg0 = reg_base + data0[0];
	pllclk->reg1 = reg_base + data1[0];
	pllclk->lock = &jlq_clk.lock;
	pllclk->hw.init = init;

	pllclk->sshift = data0[1];
	pllclk->smask  = data0[2];
	pllclk->mshift = data0[3];
	pllclk->mmask = data0[4];
	pllclk->pshift = data0[5];
	pllclk->pmask = data0[6];

	pllclk->pdshift = data1[1];
	pllclk->pdweshift = data1[2];
	pllclk->adjshift = data1[3];
	pllclk->adjweshift = data1[4];

	if (of_get_property(np, "set_rate_bigger", NULL))
		pllclk->flags |= FLAG_RATE_HIGHER;
	else if (of_get_property(np, "set_rate_smaller", NULL))
		pllclk->flags |= FLAG_RATE_LOWER;
	else if (of_get_property(np, "set_rate_closer", NULL))
		pllclk->flags |= FLAG_RATE_CLOSER;
	else
		pllclk->flags |= FLAG_RATE_HIGHER;

	if (jlq_clkpll80x_table_init(np, pllclk)) {
		pr_err("[%s] fail to of_get_pllx_ftbl!\n", __func__);
		goto err_clk;
	}

	clk = clk_register(NULL, &pllclk->hw);
	if (IS_ERR(clk)) {
		pr_err("[%s] fail to reigister clk %s!\n",
			__func__, clk_name);
		goto err_reg;
	}

	clk_register_clkdev(clk, clk_name, NULL);
	of_clk_add_provider(np, of_clk_src_simple_get, clk);
	kfree(init);
	kfree(parent_names);
	return;

err_reg:
	kfree(pllclk->table);
err_clk:
	kfree(init);
err_init:
	kfree(pllclk);
err_pllclk:
	kfree(parent_names);
err_out:
	pr_err("[%s] fail!\n", __func__);
}

static u8 jlq_clkmux_get_parent(struct clk_hw *hw)
{
	struct jlq_muxclk *muxclk = container_of(hw, struct jlq_muxclk, hw);
	u32 val;

	if (muxclk->flags & FLAG_MULTI_LEVEL) {//cpu mux
		val = readl(muxclk->reg) >> muxclk->shift;
		val &= muxclk->mask;
		if (val) {
			val = readl(muxclk->reg) >> muxclk->shift1;
			val &= muxclk->mask1;
			val += 1;
		}
	} else {
		val = readl(muxclk->reg) >> muxclk->shift;
		val &= muxclk->mask;
	}

	return val;
}

static int jlq_clkmux_set_parent(struct clk_hw *hw, u8 index)
{
	struct jlq_muxclk *muxclk = container_of(hw, struct jlq_muxclk, hw);
	int num_parents = clk_hw_get_num_parents(hw);
	unsigned int adj = 0;
	unsigned long flags = 0;
	unsigned int val;

	if (index >= num_parents)
		return -EINVAL;

	if (muxclk->lock)
		spin_lock_irqsave(muxclk->lock, flags);

	if (muxclk->flags & FLAG_MULTI_LEVEL) {//cpu mux
		if (index == 0) {
			val = readl(muxclk->reg);
			val &= ~(muxclk->mask << muxclk->shift);
			val |= index << muxclk->shift;
			if (muxclk->weshift)
				val |= (muxclk->mask << muxclk->weshift);

			val &= ~(muxclk->mask1 << muxclk->shift1);
			val |= index << muxclk->shift1;
			if (muxclk->weshift1)
				val |= (muxclk->mask1 << muxclk->weshift1);

			writel(val, muxclk->reg);
		} else {
			val = readl(muxclk->reg);
			val &= ~(muxclk->mask << muxclk->shift);
			val |= 0x1 << muxclk->shift;
			if (muxclk->weshift)
				val |= (muxclk->mask << muxclk->weshift);

			val &= ~(muxclk->mask1 << muxclk->shift1);
			val |= (index - 1) << muxclk->shift1;
			if (muxclk->weshift1)
				val |= (muxclk->mask1 << muxclk->weshift1);

			writel(val, muxclk->reg);
		}
	} else {
		val = readl(muxclk->reg);
		val &= ~(muxclk->mask << muxclk->shift);
		val |= index << muxclk->shift;
		if (muxclk->weshift)
			val |= (muxclk->mask << muxclk->weshift);

		writel(val, muxclk->reg);
	}

	if (muxclk->lock)
		spin_unlock_irqrestore(muxclk->lock, flags);

	if (muxclk->flags & FLAG_NEED_UPDATE) {
		if (muxclk->lock)
			spin_lock_irqsave(muxclk->lock, flags);

		val = readl(muxclk->reg);
		val |= 1 << muxclk->upshift;
		if (muxclk->upweshift)
			val |= 1 << muxclk->upweshift;
		writel(val, muxclk->reg);

		adj = readl(muxclk->reg) & (1 << muxclk->upshift);
		while (adj) {
			cpu_relax();
			adj = readl(muxclk->reg) & (1 << muxclk->upshift);
		}

		if (muxclk->lock)
			spin_unlock_irqrestore(muxclk->lock, flags);
	}

	clk_debug("%s: %s, index(%d)\n",
		__func__, __clk_get_name(hw->clk), index);
	return 0;
}

static long jlq_clkmux_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *prate)
{
	struct jlq_muxclk *mclk = container_of(hw, struct jlq_muxclk, hw);
	struct jlq_mux_table *ptbl = mclk->table;
	int i, bestidx = -1;
	long delta = 0;
	unsigned long gap = ULONG_MAX;
	unsigned long min_rate = ULONG_MAX;

	for (i = 0; ptbl[i].parent_clk != NULL; i++) {
		min_rate = min(min_rate, ptbl[i].rate);
		/*
		 * Fix me: If you want output rate lower than request rate,
		 * should use the expression:
		 *	delta = rate - ptbl[i].rate;
		 * But there is some thing confusing for float divisor.
		 * If set gpu_mclk_idx0 to 536.256M(~536,256,666), you should
		 * call clk_set_rate(clk, 536257000);
		 */
		delta = max(rate, ptbl[i].rate) - min(rate, ptbl[i].rate);
		if ((delta >= 0) && (delta < gap)) {
			bestidx = i;
			gap = delta;
			*prate = ptbl[bestidx].rate;
		}

		if (!gap)
			break;
	}

	if (-1 == bestidx) {//impossible to be here.
		*prate = min_rate;
		pr_warn("[%s]:%s can not round out %ld, use %ld\n",
			__func__, clk_hw_get_name(hw), rate, min_rate);
	}

	clk_debug("%s: %s, parent_rate(%ld), round_rate(%ld)\n",
		__func__, __clk_get_name(hw->clk), *prate, rate);
	return *prate;
}

static int jlq_clkmux_determine_rate_flags(struct clk_hw *hw,
				 struct clk_rate_request *req,
				 unsigned long flags)
{
	struct clk_hw *parent, *best_parent = NULL;
	int i, num_parents, ret;
	unsigned long best = 0;
	unsigned long gap = ULONG_MAX;
	struct clk_rate_request parent_req = *req;
	unsigned long max_rate = 0, min_rate = ULONG_MAX;
	struct clk_hw *max_parent, *min_parent;
	unsigned long max_parent_rate = 0, min_parent_rate = 0;

	/* find the parent that can provide the fastest rate <= rate */
	num_parents = clk_hw_get_num_parents(hw);

	for (i = 0; i < num_parents; i++) {
		parent = clk_hw_get_parent_by_index(hw, i);
		if (!parent)
			continue;

		if (clk_hw_get_flags(hw) & CLK_SET_RATE_PARENT) {
			parent_req = *req;
			ret = __clk_determine_rate(parent, &parent_req);
			if (ret)
				continue;
		} else {
			parent_req.rate = clk_hw_get_rate(parent);
		}

		if (parent_req.rate >= max_rate) {
			max_parent = parent;
			max_parent_rate = parent_req.rate;
		}
		if (parent_req.rate <= min_rate) {
			min_parent = parent;
			min_parent_rate = parent_req.rate;
		}
		max_rate = max(parent_req.rate, max_rate);
		min_rate = min(parent_req.rate, min_rate);

		if (jlq_clk_better_rate(req->rate, parent_req.rate,
			       gap, flags)) {
			best_parent = parent;
			best = parent_req.rate;
			gap = abs(parent_req.rate - req->rate);
		}
	}

	if (best_parent) {
		req->best_parent_hw = best_parent;
		req->best_parent_rate = best;
		req->rate = best;
		clk_debug("%s: %s, parent_hw(%s),parent_rate(%ld),rate(%ld)\n",
			__func__, __clk_get_name(hw->clk),
			__clk_get_name(best_parent->clk), best, best);
	} else {
		clk_debug("[%s] no match %ld,max or min.%s(%ld)\n",
			__func__, req->rate, clk_hw_get_name(hw),
			((flags & FLAG_RATE_HIGHER) ? max_rate : min_rate));
		req->best_parent_hw = (flags & FLAG_RATE_HIGHER) ?
					max_parent : min_parent;
		req->best_parent_rate = (flags & FLAG_RATE_HIGHER) ?
					max_parent_rate : min_parent_rate;
		req->rate = (flags & FLAG_RATE_HIGHER) ? max_rate : min_rate;
		clk_debug("%s: %s, parent_hw(%s),parent_rate(%ld),rate(%ld)\n",
			__func__, __clk_get_name(hw->clk),
			__clk_get_name(req->best_parent_hw->clk),
			req->best_parent_rate, req->rate);
	}

	return 0;
}

static int jlq_clkmux_determine_rate(struct clk_hw *hw,
				  struct clk_rate_request *req)
{
	struct jlq_muxclk *mclk = container_of(hw, struct jlq_muxclk, hw);

	return  jlq_clkmux_determine_rate_flags(hw, req, mclk->flags);
}

static int __init jlq_parse_mux(struct device_node *np,
			unsigned int *num_parents)
{
	int i, cnt;

	/* get the count of items in mux */
	for (i = 0, cnt = 0;; i++, cnt++) {
		/* parent's #clock-cells property is always 0 */
		if (!of_parse_phandle(np, "clocks", i))
			break;
	}

	for (i = 0; i < cnt; i++) {
		if (!of_clk_get_parent_name(np, i)) {
			pr_err("[%s] cannot get %dth parent_clk name!\n",
				__func__, i);
			return -ENOENT;
		}
	}
	*num_parents = cnt;

	return 0;
}

const struct clk_ops jlq_clkmux_ops = {
	.get_parent = jlq_clkmux_get_parent,
	.set_parent = jlq_clkmux_set_parent,
	.round_rate = jlq_clkmux_round_rate,
	.determine_rate = jlq_clkmux_determine_rate,
	.debug_init = jlq_clk_debug_init,
};

static void __init jlq_clkmux_setup(struct device_node *np)
{
	struct jlq_muxclk *muxclk;
	struct jlq_mux_table *table;
	struct clk_init_data *init;
	struct clk *clk;
	const char *clk_name, **parent_names = NULL;
	unsigned int data[4] = {0};
	unsigned int num_parents = 0;
	void __iomem *reg_base;
	unsigned int size = 0;
	int i, ret;


	reg_base = jlq_clk_get_base(np);
	if (!reg_base) {
		pr_err("[%s] %s fail to get reg_base!\n", __func__, np->name);
		return;
	}

	if (of_property_read_string(np, "clock-output-names", &clk_name)) {
		pr_err("[%s] %s node doesn't have clock-output-name property!\n",
				__func__, np->name);
		return;
	}

	if (of_property_read_u32_array(np, "jlq,clkmux", &data[0], 4)) {
		pr_err("[%s] %s node doesn't have jlq,clkmux property!\n",
				__func__, np->name);
		return;
	}

	ret = jlq_parse_mux(np, &num_parents);
	if (ret) {
		pr_err("[%s] %s node cannot get num_parents!\n",
			__func__, np->name);
		return;
	}

	size = sizeof(struct jlq_mux_table) * (num_parents + 1);
	table = kzalloc(size, GFP_KERNEL);
	if (!table)
		goto err_out;

	/* table range [0, num_parents] and end with {0, 0, 0}*/
	size = sizeof(char *) * (num_parents + 1);
	parent_names = kzalloc(size, GFP_KERNEL);
	if (!parent_names)
		goto err_par;

	for (i = 0; i < num_parents; i++) {
		parent_names[i] = of_clk_get_parent_name(np, i);
		table[i].idx = i;
#if 0
		table[i].parent_clk = __clk_lookup(parent_names[i]);
		if (table[i].parent_clk == NULL) {
			pr_err("[%s] %s muxclk's parent must list firstly!\n",
					__func__, clk_name);
			goto err_pars;
		}
		table[i].rate = clk_get_rate(table[i].parent_clk);
#endif
	}

	muxclk = kzalloc(sizeof(struct jlq_muxclk), GFP_KERNEL);
	if (!muxclk)
		goto err_muxclk;

	init = kzalloc(sizeof(struct clk_init_data), GFP_KERNEL);
	if (!init)
		goto err_init;

	init->name = clk_name;
	init->ops = &jlq_clkmux_ops;
	init->parent_names = parent_names;
	init->num_parents = num_parents;
	if (of_get_property(np, "set_rate_parent", NULL))
		init->flags |= CLK_SET_RATE_PARENT;
	if (of_get_property(np, "clk_is_critical", NULL))
		init->flags |= CLK_IS_CRITICAL;

	muxclk->reg = reg_base + data[0];
	muxclk->mask    = data[1];
	muxclk->shift   = data[2];
	muxclk->weshift = data[3];
	muxclk->hw.init = init;
	muxclk->lock = &jlq_clk.lock;
	muxclk->table = table;
	muxclk->flags = 0;

	if (of_get_property(np, "set_rate_bigger", NULL))
		muxclk->flags |= FLAG_RATE_HIGHER;
	else if (of_get_property(np, "set_rate_smaller", NULL))
		muxclk->flags |= FLAG_RATE_LOWER;
	else if (of_get_property(np, "set_rate_closer", NULL))
		muxclk->flags |= FLAG_RATE_CLOSER;
	else
		muxclk->flags |= FLAG_RATE_HIGHER;

	if (of_get_property(np, "set_multi_level", NULL))
		muxclk->flags |= FLAG_MULTI_LEVEL;

	if (muxclk->flags & FLAG_MULTI_LEVEL) {
		if (of_property_read_u32_array(np, "jlq,clkmux1",
							&data[0], 3)) {
			pr_err("[%s] %s node doesn't have clkmux1 property!\n",
				__func__, np->name);
			goto err_mux;
		}
		muxclk->mask1    = data[0];
		muxclk->shift1   = data[1];
		muxclk->weshift1 = data[2];
	}

	if (!of_property_read_u32_array(np, "jlq,clkmuxup", &data[0], 2)) {
		muxclk->upshift   = data[0];
		muxclk->upweshift = data[1];
		muxclk->flags |= FLAG_NEED_UPDATE;
	}

	clk = clk_register(NULL, &muxclk->hw);
	if (IS_ERR(clk)) {
		pr_err("[%s] fail to reigister clk %s!\n",
			__func__, clk_name);
		goto err_clk;
	}

	clk_register_clkdev(clk, clk_name, NULL);
	of_clk_add_provider(np, of_clk_src_simple_get, clk);
	kfree(init);
	kfree(parent_names);

	return;
err_clk:
err_mux:
	kfree(init);
err_init:
	kfree(muxclk);
err_muxclk:
#if 0
err_pars:
	kfree(parent_names);
#endif
err_par:
	kfree(table);
err_out:
	pr_err("[%s] %s fail!\n", __func__, clk_name);
}

static unsigned long jlq_clkthru_recalc_rate(struct clk_hw *hw,
					unsigned long parent_rate)
{
	clk_debug("%s: %s, parent_rate(%ld), calc_rate(%ld)\n",
		__func__, __clk_get_name(hw->clk), parent_rate, parent_rate);
	return parent_rate;
}

static const struct clk_ops jlq_clkthru_ops = {
	.recalc_rate = jlq_clkthru_recalc_rate,
	.debug_init = jlq_clk_debug_init,
};

static void __init jlq_clkthru_setup(struct device_node *np)
{
	struct jlq_thruclk *tclk;
	struct clk_init_data *init;
	struct clk *clk;
	const char *clk_name, **parent_names;

	if (of_property_read_string(np, "clock-output-names", &clk_name)) {
		pr_err("[%s] %s node doesn't have clock-output-name property!\n",
			__func__, np->name);
		return;
	}

	/* fixed parent */
	parent_names = kzalloc(sizeof(char *), GFP_KERNEL);
	if (!parent_names)
		goto err_out;

	parent_names[0] = of_clk_get_parent_name(np, 0);

	tclk = kzalloc(sizeof(struct jlq_thruclk), GFP_KERNEL);
	if (!tclk)
		goto err_tclk;

	init = kzalloc(sizeof(struct clk_init_data), GFP_KERNEL);
	if (!init)
		goto err_init;

	init->name = clk_name;
	init->ops = &jlq_clkthru_ops;
	init->flags = CLK_IGNORE_UNUSED;
	init->parent_names = parent_names;
	init->num_parents = 1;
	if (of_get_property(np, "set_rate_parent", NULL))
		init->flags |= CLK_SET_RATE_PARENT;
	if (of_get_property(np, "clk_is_critical", NULL))
		init->flags |= CLK_IS_CRITICAL;

	tclk->hw.init = init;

	clk = clk_register(NULL, &(tclk->hw));
	if (IS_ERR(clk)) {
		pr_err("[%s] fail to reigister clk %s!\n",
			__func__, clk_name);
		goto err_clk;
	}

	clk_register_clkdev(clk, clk_name, NULL);
	of_clk_add_provider(np, of_clk_src_simple_get, clk);
	kfree(init);
	kfree(parent_names);
	return;

err_clk:
	kfree(init);
err_init:
	kfree(tclk);
err_tclk:
	kfree(parent_names);
err_out:
	pr_err("[%s] fail!\n", __func__);
}

#ifndef MODULE
static void __iomem __init *jlq_clk_get_base(struct device_node *np)
{
	void __iomem *ret = NULL;
	unsigned int flags = 0;
	struct device_node *parent;

	of_property_read_u32_array(np, "jlq,base", &flags, 1);

	switch (flags) {
	case JLQ_TOP_CRG:
		if (!jlq_clk.top_crg) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,top_crg_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.top_crg = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.top_crg;
		}
		break;
	case JLQ_AI_SYSCTRL:
		if (!jlq_clk.ai_sysctrl) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,ai_sysctrl_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.ai_sysctrl = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.ai_sysctrl;
		}
		break;
	case JLQ_CM4_SYSCTRL:
		if (!jlq_clk.cm4_sysctrl) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,cm4_sysctrl_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.cm4_sysctrl = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.cm4_sysctrl;
		}
		break;
	case JLQ_DISPLAY_SYSCTRL:
		if (!jlq_clk.display_sysctrl) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,display_sysctrl_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.display_sysctrl = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.display_sysctrl;
		}
		break;
	case JLQ_HBLK_SYSCTRL:
		if (!jlq_clk.hblk_sysctrl) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,hblk_sysctrl_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.hblk_sysctrl = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.hblk_sysctrl;
		}
		break;
	case JLQ_VDSP_SYSCTRL:
		if (!jlq_clk.vdsp_sysctrl) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,vdsp_sysctrl_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.vdsp_sysctrl = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.vdsp_sysctrl;
		}
		break;
	case JLQ_VPU_SYSCTRL:
		if (!jlq_clk.vpu_sysctrl) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,vpu_sysctrl_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.vpu_sysctrl = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.vpu_sysctrl;
		}
		break;
	case JLQ_AUDIO_SYSCTRL:
		if (!jlq_clk.audio_sysctrl) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,audio_sysctrl_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.audio_sysctrl = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.audio_sysctrl;
		}
		break;
	default:
		pr_err("[%s] cannot find the reg base!\n", __func__);
		ret = NULL;
	}

out:
	return ret;
}

CLK_OF_DECLARE(jlq_div,  "jlq,clk-div", jlq_clkdiv_setup);
CLK_OF_DECLARE(jlq_gate, "jlq,clk-gate", jlq_clkgate_setup);
CLK_OF_DECLARE(jlq_fdiv, "jlq,clk-fdiv", jlq_clkfdiv_setup);
CLK_OF_DECLARE(jlq_pll43x, "jlq,clk-pll1143x", jlq_pll43x_setup);
CLK_OF_DECLARE(jlq_pll80x, "jlq,clk-pll1180x", jlq_pll80x_setup);
CLK_OF_DECLARE(jlq_mux,  "jlq,clk-mux", jlq_clkmux_setup);
CLK_OF_DECLARE(jlq_thru, "jlq,clk-thru", jlq_clkthru_setup);

#else

const struct jlq_clk_ops clk_div =	{ .setup = jlq_clkdiv_setup  };
const struct jlq_clk_ops clk_gate =	{ .setup = jlq_clkgate_setup };
const struct jlq_clk_ops clk_fdiv =	{ .setup = jlq_clkfdiv_setup };
const struct jlq_clk_ops clk_pll1143x = { .setup = jlq_pll43x_setup  };
const struct jlq_clk_ops clk_pll1180x = { .setup = jlq_pll80x_setup  };
const struct jlq_clk_ops clk_mux =	{ .setup = jlq_clkmux_setup  };
const struct jlq_clk_ops clk_thru =	{ .setup = jlq_clkthru_setup };

static const struct of_device_id jlq_clk_match[] = {
	{ .compatible = "jlq,clk-div",  .data = &clk_div  },
	{ .compatible = "jlq,clk-gate", .data = &clk_gate },
	{ .compatible = "jlq,clk-fdiv",	.data = &clk_fdiv },
	{ .compatible = "jlq,clk-pll1143x", .data = &clk_pll1143x },
	{ .compatible = "jlq,clk-pll1180x", .data = &clk_pll1180x },
	{ .compatible = "jlq,clk-mux",  .data = &clk_mux  },
	{ .compatible = "jlq,clk-thru",	.data = &clk_thru },
	{ }
};

static void __iomem __init *jlq_clk_get_base(struct device_node *np)
{
	void __iomem *ret = NULL;
	unsigned int flags = 0;
	struct device_node *parent;

	of_property_read_u32_array(np, "jlq,base", &flags, 1);

	switch (flags) {
	case JLQ_TOP_CRG:
		if (!jlq_clk.top_crg) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,top_crg_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.top_crg = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.top_crg;
		}
		break;
	case JLQ_AI_SYSCTRL:
		if (!jlq_clk.ai_sysctrl) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,ai_sysctrl_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.ai_sysctrl = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.ai_sysctrl;
		}
		break;
	case JLQ_CM4_SYSCTRL:
		if (!jlq_clk.cm4_sysctrl) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,cm4_sysctrl_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.cm4_sysctrl = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.cm4_sysctrl;
		}
		break;
	case JLQ_DISPLAY_SYSCTRL:
		if (!jlq_clk.display_sysctrl) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,display_sysctrl_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.display_sysctrl = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.display_sysctrl;
		}
		break;
	case JLQ_HBLK_SYSCTRL:
		if (!jlq_clk.hblk_sysctrl) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,hblk_sysctrl_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.hblk_sysctrl = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.hblk_sysctrl;
		}
		break;
	case JLQ_VDSP_SYSCTRL:
		if (!jlq_clk.vdsp_sysctrl) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,vdsp_sysctrl_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.vdsp_sysctrl = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.vdsp_sysctrl;
		}
		break;
	case JLQ_VPU_SYSCTRL:
		if (!jlq_clk.vpu_sysctrl) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,vpu_sysctrl_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.vpu_sysctrl = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.vpu_sysctrl;
		}
		break;
	case JLQ_AUDIO_SYSCTRL:
		if (!jlq_clk.audio_sysctrl) {
			parent = of_find_compatible_node(NULL, NULL,
					"jlq,audio_sysctrl_clks");
			if (!parent) {
				pr_err("%s: %s: base not available!\n",
					__func__, np->name);
				goto out;
			}
			ret = of_iomap(parent, 0);
			WARN_ON(!ret);
			jlq_clk.audio_sysctrl = ret;
			of_node_put(parent);
		} else {
			ret = jlq_clk.audio_sysctrl;
		}
		break;
	default:
		pr_err("[%s] cannot find the reg base!\n", __func__);
		ret = NULL;
	}

out:
	return ret;
}

static int jlq_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct of_device_id *match;
	const struct jlq_clk_ops *data;

	match = of_match_node(jlq_clk_match, np);
	data = match->data;
	if (data->setup)
		data->setup(np);

	return 0;
}

static struct platform_driver jlq_clk_driver = {
	.probe = jlq_clk_probe,
	.driver = {
		.name = "jlq-clk",
		.of_match_table = jlq_clk_match,
	},
};

static int __init jlq_clk_init(void)
{
	return platform_driver_register(&jlq_clk_driver);
}
core_initcall(jlq_clk_init);

static void __exit jlq_clk_exit(void)
{
	platform_driver_unregister(&jlq_clk_driver);
}
module_exit(jlq_clk_exit);

MODULE_SOFTDEP("post: jlq-smd ");
MODULE_SOFTDEP("post: jlq-gpio reset-jlq arm-smmu-v3");
MODULE_SOFTDEP("post: spmi-pmic-arb i2c-designware-core");
#endif

MODULE_DESCRIPTION("JLQ CLK DIV Driver");
MODULE_LICENSE("GPL");
