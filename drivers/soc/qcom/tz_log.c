/*
 * Copyright (c) 2015-2017, 2020 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/debugfs.h> /* this is for DebugFS libraries */
#include <linux/fs.h>
#include <linux/dma-mapping.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/slab.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
#include <linux/threads.h>
#include <linux/of_device.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/sizes.h>

#define SMMU_DISABLE_NONE  0x0 /* SMMU Stage2 Enabled */
#define SMMU_DISABLE_S2    0x1 /* SMMU Stage2 bypass */
#define SMMU_DISABLE_ALL   0x2 /* SMMU Disabled */

#define HVC_DIAG_RING_OFFSET		2
#define HVC_DIAG_LOG_POS_INFO_OFFSET	3
#define TZ_LEGACY_RING_OFFSET		7
#define TZ_LEGACY_LOG_POS_INFO_OFFSET	522
#define TZ_LEGACY_BUF_LEN		0x1000

static unsigned int paniconaccessviolation = 0;
module_param(paniconaccessviolation, uint, 0644);
MODULE_PARM_DESC(paniconaccessviolation, "Panic on Access Violation detected: 0,1");

static char *smmu_state;

/**
 * struct tzbsp_log_pos_t - log position structure
 * @wrap: ring buffer wrap-around ctr
 * @offset: ring buffer current position
 */
struct tzbsp_log_pos_t {
	uint16_t wrap;
	uint16_t offset;
};

/**
 * struct tz_hvc_log_struct - TZ / HVC log info structure
 * @debugfs_dir: qti_debug_logs debugfs directory
 * @ker_buf: kernel buffer shared with TZ to get the diag log
 * @copy_buf: kernel buffer used to copy the diag log
 * @copy_len: length of the diag log that has been copied into the buffer
 * @tz_ring_off: offset in tz log buffer that contains the ring start offset
 * @tz_log_pos_info_off: offset in tz log buffer that contains log position info
 * @hvc_ring_off: offset in hvc log buffer that contains the ring start offset
 * @hvc_log_pos_info_off: offset in hvc log buffer that contains log position info
 * @buf_len: kernel buffer length
 * @lock: mutex lock for synchronization
 * @tz_kpss: boolean to handle ipq806x which has different diag log structure
 */
struct tz_hvc_log_struct {
	struct dentry *debugfs_dir;
	char *ker_buf;
	char *copy_buf;
	int copy_len;
	uint32_t tz_ring_off;
	uint32_t tz_log_pos_info_off;
	uint32_t hvc_ring_off;
	uint32_t hvc_log_pos_info_off;
	int buf_len;
	struct mutex lock;
	bool tz_kpss;
};

static int tz_hvc_log_open(struct inode *inode, struct file *file)
{
	struct tz_hvc_log_struct *tz_hvc_log;
	char *ker_buf;
	char *copy_buf;
	uint32_t buf_len;
	uint32_t ring_off;
	uint32_t log_pos_info_off;

	uint32_t *diag_buf;
	uint16_t ring;
	struct tzbsp_log_pos_t *log;
	uint16_t offset;
	uint16_t wrap;
	int ret;

	file->private_data = inode->i_private;

	tz_hvc_log = file->private_data;
	mutex_lock(&tz_hvc_log->lock);

	ker_buf = tz_hvc_log->ker_buf;
	copy_buf = tz_hvc_log->copy_buf;
	buf_len = tz_hvc_log->buf_len;

	if (!strncmp(file->f_path.dentry->d_iname, "tz_log", sizeof("tz_log"))) {
		/* SCM call to TZ to get the tz log */
		ret = qti_scm_tz_log(ker_buf, buf_len);
		if (ret != 0) {
			pr_err("Error in getting tz log\n");
			mutex_unlock(&tz_hvc_log->lock);
			return ret;
		}

		ring_off = tz_hvc_log->tz_ring_off;
		log_pos_info_off = tz_hvc_log->tz_log_pos_info_off;
	} else {
		/* SCM call to TZ to get the hvc log */
		ret = qti_scm_hvc_log(ker_buf, buf_len);
		if (ret != 0) {
			pr_err("Error in getting hvc log\n");
			mutex_unlock(&tz_hvc_log->lock);
			return ret;
		}

		ring_off = tz_hvc_log->hvc_ring_off;
		log_pos_info_off = tz_hvc_log->hvc_log_pos_info_off;
	}

	diag_buf = (uint32_t *) ker_buf;
	ring = diag_buf[ring_off];
	log = (struct tzbsp_log_pos_t *) &diag_buf[log_pos_info_off];
	offset = log->offset;
	wrap = log->wrap;

	/* To support IPQ806x platform */
	if (tz_hvc_log->tz_kpss) {
		offset = buf_len - ring;
		wrap = 0;
	}

	if (wrap != 0) {
		/* since ring wrap occurred, log starts at the offset position
		 * and offset will be in the middle of the ring.
		 * ring buffer - [ <second half of log> $ <first half of log> ]
		 * $ - represents current position of the log start i.e. offset
		 */
		memcpy(copy_buf, (ker_buf + ring + offset),
				(buf_len - ring - offset));
		memcpy(copy_buf + (buf_len - ring - offset),
				(ker_buf + ring), offset);
		tz_hvc_log->copy_len = (buf_len - offset - ring)
			+ offset;
	} else {
		/* since there is no ring wrap condition, log starts at the
		 * start of the ring and offset will be the end of the log.
		 */
		memcpy(copy_buf, (ker_buf + ring), offset);
		tz_hvc_log->copy_len = offset;
	}

	return 0;
}

static ssize_t tz_hvc_log_read(struct file *fp, char __user *user_buffer,
				size_t count, loff_t *position)
{
	struct tz_hvc_log_struct *tz_hvc_log;

	tz_hvc_log = fp->private_data;

	return simple_read_from_buffer(user_buffer, count,
					position, tz_hvc_log->copy_buf,
					tz_hvc_log->copy_len);
}

static int tz_hvc_log_release(struct inode *inode, struct file *file)
{
	struct tz_hvc_log_struct *tz_hvc_log;

	tz_hvc_log = file->private_data;
	mutex_unlock(&tz_hvc_log->lock);

	return 0;
}

static const struct file_operations fops_tz_hvc_log = {
	.open = tz_hvc_log_open,
	.read = tz_hvc_log_read,
	.release = tz_hvc_log_release,
};

static ssize_t tz_smmu_state_read(struct file *fp, char __user *user_buffer,
				size_t count, loff_t *position)
{
	return simple_read_from_buffer(user_buffer, count, position,
				smmu_state, strlen(smmu_state));
}

static const struct file_operations fops_tz_smmu_state = {
	.read = tz_smmu_state_read,
};

static irqreturn_t tzerr_irq(int irq, void *data)
{
	if (paniconaccessviolation) {
		panic("WARN: Access Violation!!!");
	} else {
		pr_emerg_ratelimited("WARN: Access Violation!!!, "
			"Run \"cat /sys/kernel/debug/qti_debug_logs/tz_log\" "
			"for more details \n");
	}
	return IRQ_HANDLED;
}

static int qti_tzlog_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct tz_hvc_log_struct *tz_hvc_log;
	struct dentry *fileret;
	struct page *page_buf;
	bool tz_legacy_scm = false;
	int ret = 0;
	int irq;

	tz_hvc_log = (struct tz_hvc_log_struct *)
			kzalloc(sizeof(struct tz_hvc_log_struct), GFP_KERNEL);
	if (tz_hvc_log == NULL) {
		dev_err(&pdev->dev, "unable to get tzlog memory\n");
		return -ENOMEM;
	}

	tz_hvc_log->tz_kpss = of_property_read_bool(np, "qti,tz_kpss");

	tz_legacy_scm = false;

	ret = of_property_read_u32(np, "qti,tz-diag-buf-size",
				   &(tz_hvc_log->buf_len));
	if (ret < 0) {
		dev_err(&pdev->dev, "unable to get diag-buf-size property\n");
		goto free_mem;
	}

	ret = of_property_read_u32(np, "qti,tz-ring-off",
				   &(tz_hvc_log->tz_ring_off));
	if (ret < 0) {
		dev_err(&pdev->dev, "unable to get ring-off property\n");
		goto free_mem;
	}

	ret = of_property_read_u32(np, "qti,tz-log-pos-info-off",
				   &(tz_hvc_log->tz_log_pos_info_off));
	if (ret < 0) {
		dev_err(&pdev->dev, "unable to get log-pos-info-off property\n");
		goto free_mem;
	}

	tz_hvc_log->hvc_ring_off = HVC_DIAG_RING_OFFSET;
	tz_hvc_log->hvc_log_pos_info_off = HVC_DIAG_LOG_POS_INFO_OFFSET;

	/* To support TZ 2.10 */
	if (tz_legacy_scm) {
		tz_hvc_log->tz_ring_off = TZ_LEGACY_RING_OFFSET;
		tz_hvc_log->tz_log_pos_info_off = TZ_LEGACY_LOG_POS_INFO_OFFSET;
		tz_hvc_log->buf_len = TZ_LEGACY_BUF_LEN;
	}

	page_buf = alloc_pages(GFP_KERNEL,
					get_order(tz_hvc_log->buf_len));
	if (page_buf == NULL) {
		dev_err(&pdev->dev, "unable to get data buffer memory\n");
		ret = -ENOMEM;
		goto free_mem;
	}

	tz_hvc_log->ker_buf = page_address(page_buf);

	page_buf = alloc_pages(GFP_KERNEL,
					get_order(tz_hvc_log->buf_len));
	if (page_buf == NULL) {
		dev_err(&pdev->dev, "unable to get copy buffer memory\n");
		ret = -ENOMEM;
		goto free_mem;
	}

	tz_hvc_log->copy_buf = page_address(page_buf);

	mutex_init(&tz_hvc_log->lock);

	tz_hvc_log->debugfs_dir = debugfs_create_dir("qti_debug_logs", NULL);
	if (IS_ERR_OR_NULL(tz_hvc_log->debugfs_dir)) {
		dev_err(&pdev->dev, "unable to create debugfs\n");
		ret = -EIO;
		goto free_mem;
	}

	fileret = debugfs_create_file("tz_log", 0444,  tz_hvc_log->debugfs_dir,
					tz_hvc_log, &fops_tz_hvc_log);
	if (IS_ERR_OR_NULL(fileret)) {
		dev_err(&pdev->dev, "unable to create tz_log debugfs\n");
		ret = -EIO;
		goto remove_debugfs;
	}

	if (!tz_legacy_scm && of_property_read_bool(np, "qti,hvc-enabled")) {
		fileret = debugfs_create_file("hvc_log", 0444,
			tz_hvc_log->debugfs_dir, tz_hvc_log, &fops_tz_hvc_log);
		if (IS_ERR_OR_NULL(fileret)) {
			dev_err(&pdev->dev, "can't create hvc_log debugfs\n");
			ret = -EIO;
			goto remove_debugfs;
		}
	}

	if (of_property_read_bool(np, "qti,get-smmu-state")) {
		ret = qti_scm_get_smmustate();
		switch(ret) {
			case SMMU_DISABLE_NONE:
				smmu_state = "SMMU Stage2 Enabled\n";
				break;
			case SMMU_DISABLE_S2:
				smmu_state = "SMMU Stage2 Bypass\n";
				break;
			case SMMU_DISABLE_ALL:
				smmu_state = "SMMU is Disabled\n";
				break;
			default:
				smmu_state = "Can't detect SMMU State\n";
		}
		dev_info(&pdev->dev, "TZ SMMU State: %s", smmu_state);

		fileret = debugfs_create_file("tz_smmu_state", 0444,
			tz_hvc_log->debugfs_dir, NULL, &fops_tz_smmu_state);
		if (IS_ERR_OR_NULL(fileret)) {
			dev_err(&pdev->dev, "can't create tz_smmu_state\n");
			ret = -EIO;
			goto remove_debugfs;
		}
	}

	irq = platform_get_irq(pdev, 0);
	if (irq > 0) {
		ret = devm_request_irq(&pdev->dev, irq, tzerr_irq,
				       IRQF_ONESHOT, "tzerror", NULL);
		if (ret) {
			dev_err(&pdev->dev, "failed requesting IRQ: %d\n", ret);
			return ret;
		}
	}

	platform_set_drvdata(pdev, tz_hvc_log);

	if (paniconaccessviolation) {
		dev_info(&pdev->dev, "TZ Log : Will panic on Access Violation, as paniconaccessviolation is set\n");
	} else {
		dev_info(&pdev->dev, "TZ Log : Will warn on Access Violation, as paniconaccessviolation is not set\n");
	}

	return 0;

remove_debugfs:
	debugfs_remove_recursive(tz_hvc_log->debugfs_dir);
free_mem:
	if (tz_hvc_log->copy_buf)
		__free_pages(virt_to_page(tz_hvc_log->copy_buf),
				get_order(tz_hvc_log->buf_len));

	if (tz_hvc_log->ker_buf)
		__free_pages(virt_to_page(tz_hvc_log->ker_buf),
				get_order(tz_hvc_log->buf_len));

	kfree(tz_hvc_log);

	return ret;
}

static int qti_tzlog_remove(struct platform_device *pdev)
{
	struct tz_hvc_log_struct *tz_hvc_log = platform_get_drvdata(pdev);

	/* removing the directory recursively */
	debugfs_remove_recursive(tz_hvc_log->debugfs_dir);

	if (tz_hvc_log->copy_buf)
		__free_pages(virt_to_page(tz_hvc_log->copy_buf),
				get_order(tz_hvc_log->buf_len));

	if (tz_hvc_log->ker_buf)
		__free_pages(virt_to_page(tz_hvc_log->ker_buf),
				get_order(tz_hvc_log->buf_len));

	kfree(tz_hvc_log);

	return 0;
}

static const struct of_device_id qti_tzlog_of_match[] = {
	{ .compatible = "qti,tzlog" },
	{}
};
MODULE_DEVICE_TABLE(of, qti_tzlog_of_match);

static struct platform_driver qti_tzlog_driver = {
	.probe = qti_tzlog_probe,
	.remove = qti_tzlog_remove,
	.driver  = {
		.name  = "qti_tzlog",
		.of_match_table = qti_tzlog_of_match,
	},
};

module_platform_driver(qti_tzlog_driver);
