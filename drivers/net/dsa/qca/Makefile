# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_NET_DSA_AR9331)	+= ar9331.o
obj-$(CONFIG_NET_DSA_QCA8K)	+= qca8k.o
qca8k-y 			+= qca8k-common.o qca8k-8xxx.o
ifdef CONFIG_NET_DSA_QCA8K_LEDS_SUPPORT
qca8k-y				+= qca8k-leds.o
endif
obj-$(CONFIG_NET_DSA_QCOM_80211AX_PPE) += qca_ppe.o
qca_ppe-objs := qca_ppe_main.o qca_ppe_vlan.o qca_ppe_scheduler.o qca_ppe_debugfs.o qca_ppe_acl.o qca_ppe_flow.o qca_ppe_flow_offload.o
