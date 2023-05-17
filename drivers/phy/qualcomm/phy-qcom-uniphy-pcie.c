// SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause

#include <linux/module.h>
#include <linux/of_device.h>

struct uniphy_pcie {
	struct device *dev;
	void __iomem *base;
};

static int uniphy_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct uniphy_pcie *uniphy;

	uniphy = devm_kzalloc(dev, sizeof(*uniphy), GFP_KERNEL);
	if (!uniphy)
		return -ENOMEM;

	uniphy->dev = dev;
	uniphy->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(uniphy->base))
		return PTR_ERR(uniphy->base);

	dev_info(dev, "base: %p", uniphy->base);

	return 0;
}

static const struct of_device_id uniphy_pcie_of_match_table[] = {
	{ .compatible = "qcom,ipq5018-uniphy-pcie-phy", },
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
