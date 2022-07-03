// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022, The Linux Foundation. All rights reserved.
 */

#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/qcom,apss-ipq8074.h>

#include "common.h"
#include "clk-regmap.h"
#include "clk-pll.h"
#include "clk-rcg.h"
#include "clk-branch.h"
#include "clk-alpha-pll.h"
#include "clk-regmap-divider.h"
#include "clk-regmap-mux.h"

enum {
	P_XO,
	P_GPLL0,
	P_GPLL2,
	P_GPLL4,
	P_APSS_PLL_EARLY,
	P_APSS_PLL
};

static struct clk_alpha_pll apss_pll_early = {
	.offset = 0x5000,
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_APSS],
	.clkr = {
		.enable_reg = 0x5000,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "apss_pll_early",
			.parent_data = &(const struct clk_parent_data) {
				.fw_name = "xo", .name = "xo"
			},
			.num_parents = 1,
			.ops = &clk_alpha_pll_huayra_ops,
		},
	},
};

static struct clk_alpha_pll_postdiv apss_pll = {
	.offset = 0x5000,
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_APSS],
	.width = 2,
	.clkr.hw.init = &(struct clk_init_data){
		.name = "apss_pll",
		.parent_hws = (const struct clk_hw *[]){
			&apss_pll_early.clkr.hw },
		.num_parents = 1,
		.ops = &clk_alpha_pll_postdiv_ro_ops,
	},
};

static const struct clk_parent_data parents_apcs_alias0_clk_src[] = {
	{ .fw_name = "xo", .name = "xo" },
	{ .fw_name = "gpll0", .name = "gpll0" },
	{ .fw_name = "gpll2", .name = "gpll2" },
	{ .fw_name = "gpll4", .name = "gpll4" },
	{ .hw = &apss_pll.clkr.hw },
	{ .hw = &apss_pll_early.clkr.hw },
};

static const struct parent_map parents_apcs_alias0_clk_src_map[] = {
	{ P_XO, 0 },
	{ P_GPLL0, 4 },
	{ P_GPLL2, 2 },
	{ P_GPLL4, 1 },
	{ P_APSS_PLL, 3 },
	{ P_APSS_PLL_EARLY, 5 },
};

struct freq_tbl ftbl_apcs_alias0_clk_src[] = {
	F(19200000, P_XO, 1, 0, 0),
	F(403200000, P_APSS_PLL_EARLY, 1, 0, 0),
	F(806400000, P_APSS_PLL_EARLY, 1, 0, 0),
	F(1017600000, P_APSS_PLL_EARLY, 1, 0, 0),
	F(1382400000, P_APSS_PLL_EARLY, 1, 0, 0),
	F(1651200000, P_APSS_PLL_EARLY, 1, 0, 0),
	F(1843200000, P_APSS_PLL_EARLY, 1, 0, 0),
	F(1920000000, P_APSS_PLL_EARLY, 1, 0, 0),
	F(2208000000UL, P_APSS_PLL_EARLY, 1, 0, 0),
	{ }
};

struct clk_rcg2 apcs_alias0_clk_src = {
	.cmd_rcgr = 0x0050,
	.freq_tbl = ftbl_apcs_alias0_clk_src,
	.hid_width = 5,
	.parent_map = parents_apcs_alias0_clk_src_map,
	.clkr.hw.init = &(struct clk_init_data){
		.name = "apcs_alias0_clk_src",
		.parent_data = parents_apcs_alias0_clk_src,
		.num_parents = ARRAY_SIZE(parents_apcs_alias0_clk_src),
		.ops = &clk_rcg2_ops,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_branch apcs_alias0_core_clk = {
	.halt_reg = 0x0058,
	.halt_bit = 31,
	.clkr = {
		.enable_reg = 0x0058,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "apcs_alias0_core_clk",
			.parent_hws = (const struct clk_hw *[]){
				&apcs_alias0_clk_src.clkr.hw },
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT |
				CLK_IS_CRITICAL,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_regmap *apss_ipq8074_clks[] = {
	[APSS_PLL_EARLY] = &apss_pll_early.clkr,
	[APSS_PLL] = &apss_pll.clkr,
	[APCS_ALIAS0_CLK_SRC] = &apcs_alias0_clk_src.clkr,
	[APCS_ALIAS0_CORE_CLK] = &apcs_alias0_core_clk.clkr,
};

static const struct regmap_config apss_ipq8074_regmap_config = {
	.reg_bits       = 32,
	.reg_stride     = 4,
	.val_bits       = 32,
	.max_register   = 0x5ffc,
	.fast_io	= true,
};

static const struct qcom_cc_desc apss_ipq8074_desc = {
	.config = &apss_ipq8074_regmap_config,
	.clks = apss_ipq8074_clks,
	.num_clks = ARRAY_SIZE(apss_ipq8074_clks),
};

static int apss_ipq8074_probe(struct platform_device *pdev)
{
	struct regmap *regmap;

	regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!regmap)
		return -ENODEV;

	return qcom_cc_really_probe(pdev, &apss_ipq8074_desc, regmap);
}

static struct platform_driver apss_ipq8074_driver = {
	.probe = apss_ipq8074_probe,
	.driver = {
		.name   = "qcom-apss-ipq8074-clk",
	},
};

module_platform_driver(apss_ipq8074_driver);

MODULE_DESCRIPTION("Qualcomm IPQ8074 APSS clock driver");
MODULE_LICENSE("GPL");
