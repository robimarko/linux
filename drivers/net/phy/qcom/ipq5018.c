#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/phy.h>
#include <linux/reset.h>

#include "qcom.h"

#define IPQ5018_PHY_ID			0x004dd0c0

#define TX_RX_CLK_RATE			125000000 /* 125M */

#define IPQ5018_PHY_FIFO_CONTROL	0x19
#define  IPQ5018_PHY_FIFO_RESET		GENMASK(1, 0)

#define IPQ5018_PHY_DEBUG_EDAC			0x4380
#define IPQ5018_PHY_MMD1_MDAC			0x8100
#define  IPQ5018_PHY_DAC_MASK			GENMASK(15,8)

struct ipq5018_phy {
	int num_clks;
	struct clk_bulk_data *clks;
	struct reset_control *rst;

	struct clk_hw *clk_rx, *clk_tx;
	struct clk_hw_onecell_data *clk_data;

	u32 mdac;
	u32 edac;
};

static int ipq5018_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct ipq5018_phy *priv;
	u32 mdac, edac = 0;
	char name[64];
	int ret, cnt;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to allocate priv\n");

	cnt = of_property_count_u32_elems(dev->of_node, "qcom,dac");
	if (cnt == 2) {
		ret = of_property_read_u32_index(dev->of_node, "qcom,dac", 0, &mdac);
		if (!ret)
			priv->mdac = mdac;

		ret = of_property_read_u32_index(dev->of_node, "qcom,dac", 1, &edac);
		if (!ret)
			priv->edac = edac;
	}

	priv->num_clks = devm_clk_bulk_get_all(dev, &priv->clks);
	if (priv->num_clks < 0)
		return dev_err_probe(dev, priv->num_clks,
				     "failed to acquire clocks\n");

	ret = clk_bulk_prepare_enable(priv->num_clks, priv->clks);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to enable clocks\n");

	priv->rst = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR_OR_NULL(priv->rst))
		return dev_err_probe(dev, PTR_ERR(priv->rst),
				     "failed to acquire reset\n");

	ret = reset_control_reset(priv->rst);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to reset\n");

	snprintf(name, sizeof(name), "%s#rx", dev_name(dev));
	priv->clk_rx = clk_hw_register_fixed_rate(dev, name, NULL, 0,
						  TX_RX_CLK_RATE);
	if (IS_ERR_OR_NULL(priv->clk_rx))
		return dev_err_probe(dev, PTR_ERR(priv->clk_rx),
				     "failed to register rx clock\n");

	snprintf(name, sizeof(name), "%s#tx", dev_name(dev));
	priv->clk_tx = clk_hw_register_fixed_rate(dev, name, NULL, 0,
						  TX_RX_CLK_RATE);
	if (IS_ERR_OR_NULL(priv->clk_tx))
		return dev_err_probe(dev, PTR_ERR(priv->clk_tx),
				     "failed to register tx clock\n");

	priv->clk_data = devm_kzalloc(dev,
				      struct_size(priv->clk_data, hws, 2),
				      GFP_KERNEL);
	if (!priv->clk_data)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to allocate clk_data\n");

	priv->clk_data->num = 2;
	priv->clk_data->hws[0] = priv->clk_rx;
	priv->clk_data->hws[1] = priv->clk_tx;
	ret = of_clk_add_hw_provider(dev->of_node, of_clk_hw_onecell_get,
				     priv->clk_data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "fail to register clock provider\n");

	phydev->priv = priv;

	return 0;
}

static int ipq5018_soft_reset(struct phy_device *phydev)
{
	int ret;

	ret = phy_modify(phydev, IPQ5018_PHY_FIFO_CONTROL,
			 IPQ5018_PHY_FIFO_RESET, 0);
	if (ret < 0)
		return ret;

	msleep(50);

	ret = phy_modify(phydev, IPQ5018_PHY_FIFO_CONTROL,
			 IPQ5018_PHY_FIFO_RESET, IPQ5018_PHY_FIFO_RESET);
	if (ret < 0)
		return ret;

	return 0;
}

static int ipq5018_cable_test_start(struct phy_device *phydev)
{
	/* we do all the (time consuming) work later */
	return 0;
}

static int ipq5018_config_init(struct phy_device *phydev)
{
	struct ipq5018_phy *priv = phydev->priv;
	int ret;

	/* setting mdac in MMD1 */
	if (priv->mdac) {
		ret = phy_modify_mmd(phydev, MDIO_MMD_PMAPMD, IPQ5018_PHY_MMD1_MDAC,
							IPQ5018_PHY_DAC_MASK, priv->mdac);
		if (ret)
			return ret;
	}

	/* setting edac in debug register */
	if (priv->edac)
		return at803x_debug_reg_mask(phydev, IPQ5018_PHY_DEBUG_EDAC,
							IPQ5018_PHY_DAC_MASK, priv->edac);

	return 0;
}

static struct phy_driver ipq5018_internal_phy_driver[] = {
	{
		PHY_ID_MATCH_EXACT(IPQ5018_PHY_ID),
		.name		= "Qualcomm IPQ5018 internal PHY",
		.flags		= PHY_IS_INTERNAL | PHY_POLL_CABLE_TEST,
		.probe		= ipq5018_probe,
		.config_init	= ipq5018_config_init,
		.soft_reset	= ipq5018_soft_reset,
		.read_status	= at803x_read_status,
		.config_intr	= at803x_config_intr,
		.handle_interrupt = at803x_handle_interrupt,
		.cable_test_start	= ipq5018_cable_test_start,
		.cable_test_get_status	= qca808x_cable_test_get_status,
	},
};
module_phy_driver(ipq5018_internal_phy_driver);

static struct mdio_device_id __maybe_unused ipq5018_internal_phy_ids[] = {
	{ PHY_ID_MATCH_EXACT(IPQ5018_PHY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(mdio, ipq5018_internal_phy_ids);

MODULE_DESCRIPTION("Qualcomm IPQ5018 internal PHY driver");
MODULE_AUTHOR("Ziyang Huang <hzyitc@outlook.com>");
