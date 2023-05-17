// SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/reset.h>

#define CDR_CTRL_REG_1		0x80
#define CDR_CTRL_REG_2		0x84
#define CDR_CTRL_REG_3		0x88
#define CDR_CTRL_REG_4		0x8C
#define CDR_CTRL_REG_5		0x90
#define CDR_CTRL_REG_6		0x94
#define CDR_CTRL_REG_7		0x98
#define SSCG_CTRL_REG_1		0x9c
#define SSCG_CTRL_REG_2		0xa0
#define SSCG_CTRL_REG_3		0xa4
#define SSCG_CTRL_REG_4		0xa8
#define SSCG_CTRL_REG_5		0xac
#define SSCG_CTRL_REG_6		0xb0
#define PCS_INTERNAL_CONTROL_2	0x2d8

struct uniphy_phy_cfg {
	int lanes;

	/* resets to be requested */
	const char * const *reset_list;
	int num_resets;
};

struct uniphy_pcie {
	struct device *dev;
	void __iomem *base;
	const struct uniphy_phy_cfg *cfg;
	struct clk *pipe_clk;
	struct reset_control_bulk_data *resets;
	struct phy *phy;
};

/* list of resets required by phy */
static const char * const ipq5018_pciephy_reset_l[] = {
	"phy", "common",
};

static const struct uniphy_phy_cfg ipq5018_uniphy_gen2x1_pciephy_cfg = {
	.lanes	= 1,

	.reset_list		= ipq5018_pciephy_reset_l,
	.num_resets		= ARRAY_SIZE(ipq5018_pciephy_reset_l),
};

static const struct uniphy_phy_cfg ipq5018_uniphy_gen2x2_pciephy_cfg = {
	.lanes	= 2,

	.reset_list		= ipq5018_pciephy_reset_l,
	.num_resets		= ARRAY_SIZE(ipq5018_pciephy_reset_l),
};

static int uniphy_pcie_reset_init(struct uniphy_pcie *uniphy)
{
	const struct uniphy_phy_cfg *cfg = uniphy->cfg;
	struct device *dev = uniphy->dev;
	int i;
	int ret;

	uniphy->resets = devm_kcalloc(dev, cfg->num_resets,
				   sizeof(*uniphy->resets), GFP_KERNEL);
	if (!uniphy->resets)
		return -ENOMEM;

	for (i = 0; i < cfg->num_resets; i++)
		uniphy->resets[i].id = cfg->reset_list[i];

	ret = devm_reset_control_bulk_get_exclusive(dev, cfg->num_resets, uniphy->resets);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get resets\n");

	return 0;
}

static int uniphy_pcie_init(struct phy *phy)
{
	struct uniphy_pcie *uniphy = phy_get_drvdata(phy);
	const struct uniphy_phy_cfg *cfg = uniphy->cfg;
	int ret;

	ret = reset_control_bulk_assert(cfg->num_resets, uniphy->resets);
	if (ret) {
		dev_err(uniphy->dev, "reset assert failed\n");
		return ret;
	}

	usleep_range(100, 150);

	ret = reset_control_bulk_deassert(cfg->num_resets, uniphy->resets);
	if (ret) {
		dev_err(uniphy->dev, "reset deassert failed\n");
		return ret;
	}

	ret = clk_set_rate(uniphy->pipe_clk, 125000000);
	if (ret)
		return ret;

	usleep_range(5000, 5100);
	clk_prepare_enable(uniphy->pipe_clk);
	usleep_range(30, 50);

	return 0;
}

static void qca_uni_pcie_phy_init(struct phy *phy)
{
	struct uniphy_pcie *uniphy = phy_get_drvdata(phy);
	const struct uniphy_phy_cfg *cfg = uniphy->cfg;
	void __iomem *reg = uniphy->base;
	int loop = 0;

	while (loop < 2) {
		reg += (loop * 0x800);
		/*set frequency initial value*/
		writel(0x1cb9, reg + SSCG_CTRL_REG_4);
		writel(0x023a, reg + SSCG_CTRL_REG_5);
		/*set spectrum spread count*/
		writel(0xd360, reg + SSCG_CTRL_REG_3);
		/*set fstep*/
		writel(0x1, reg + SSCG_CTRL_REG_1);
		writel(0xeb, reg + SSCG_CTRL_REG_2);
		/*set FLOOP initial value*/
		writel(0x3f9, reg + CDR_CTRL_REG_4);
		writel(0x1c9, reg + CDR_CTRL_REG_5);
		/*set upper boundary level*/
		writel(0x419, reg + CDR_CTRL_REG_2);
		/*set fixed offset*/
		writel(0x200, reg + CDR_CTRL_REG_1);
		writel(0xf101, reg + PCS_INTERNAL_CONTROL_2);

		if (cfg->lanes == 2)
			loop += 1;
		else
			break;
	}
}

static int uniphy_pcie_phy_power_on(struct phy *phy)
{
	int ret;

	ret = uniphy_pcie_init(phy);
	if (ret)
		return ret;

	qca_uni_pcie_phy_init(phy);

	return ret;
}

static int uniphy_pcie_phy_power_off(struct phy *phy)
{
	struct uniphy_pcie *uniphy = phy_get_drvdata(phy);
	const struct uniphy_phy_cfg *cfg = uniphy->cfg;
	int ret;

	ret = reset_control_bulk_assert(cfg->num_resets, uniphy->resets);
	if (ret) {
		dev_err(uniphy->dev, "reset assert failed\n");
		return ret;
	}

	return 0;
}

static const struct phy_ops uniphy_pcie_phy_ops = {
	.power_on	= uniphy_pcie_phy_power_on,
	.power_off	= uniphy_pcie_phy_power_off,
	.owner          = THIS_MODULE,
};

static int uniphy_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *phy_provider;
	struct uniphy_pcie *uniphy;
	int ret;

	uniphy = devm_kzalloc(dev, sizeof(*uniphy), GFP_KERNEL);
	if (!uniphy)
		return -ENOMEM;

	uniphy->dev = dev;

	uniphy->cfg = of_device_get_match_data(dev);
	if (!uniphy->cfg)
		return -EINVAL;

	uniphy->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(uniphy->base))
		return PTR_ERR(uniphy->base);

	uniphy->pipe_clk = devm_clk_get(dev, "pipe");
	if (IS_ERR(uniphy->pipe_clk))
		return PTR_ERR(uniphy->pipe_clk);

	ret = uniphy_pcie_reset_init(uniphy);
	if (ret)
		return ret;

	uniphy->phy = devm_phy_create(dev, dev->of_node, &uniphy_pcie_phy_ops);
	if (IS_ERR(uniphy->phy)) {
		ret = PTR_ERR(uniphy->phy);
		dev_err(dev, "failed to create PHY: %d\n", ret);
		return ret;
	}

	phy_set_drvdata(uniphy->phy, uniphy);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct of_device_id uniphy_pcie_of_match_table[] = {
	{
		.compatible = "qcom,ipq5018-uniphy-gen2x1-pcie-phy",
		.data = &ipq5018_uniphy_gen2x1_pciephy_cfg,
	},
	{
		.compatible = "qcom,ipq5018-uniphy-gen2x2-pcie-phy",
		.data = &ipq5018_uniphy_gen2x2_pciephy_cfg,
	},
	{ },
};
MODULE_DEVICE_TABLE(of, uniphy_pcie_of_match_table);

static struct platform_driver uniphy_pcie_driver = {
	.probe		= uniphy_pcie_probe,
	.driver = {
		.name	= "qcom-uniphy-pcie-phy",
		.of_match_table = uniphy_pcie_of_match_table,
	},
};

module_platform_driver(uniphy_pcie_driver);

MODULE_DESCRIPTION("Qualcomm UNIPHY PCIe PHY driver");
MODULE_LICENSE("Dual BSD/GPL");
