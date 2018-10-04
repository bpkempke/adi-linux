/*
 * Analog Devices AXI-DMAC Engine test module
 *
 * Copyright (C) 2018 Analog Devices, Inc. All rights reserved.
 *
 * Based on Xilinx AXI DMA Test Client
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched/task.h>

#define EXPECTED_DEFAULT_DATA	"01234567890abcedfghijklmnopqrstuvwxyz"

static DECLARE_WAIT_QUEUE_HEAD(thread_wait_start_run);

static int param_set_int_set_wake(const char *val, const struct kernel_param *kp)
{
	int rv = param_set_int(val, kp);
	wake_up(&thread_wait_start_run);
	return rv;
}
#define param_check_int_set_wake param_check_int

const struct kernel_param_ops param_ops_int_set_wake = {
	.set = param_set_int_set_wake,
	.get = param_get_int,
};

static int sg_cnt = 4;
module_param(sg_cnt, int, 0644);
MODULE_PARM_DESC(sg_cnt, "Scatter-Gather list size");

static char *expected_data = EXPECTED_DEFAULT_DATA;
module_param(expected_data, charp, 0644);
MODULE_PARM_DESC(expected_data, "Data expected for the DMA to read from the FIFO");

static int expected_residue = 0;
module_param(expected_residue, int, 0644);
MODULE_PARM_DESC(expected_residue, "Expected residue value after DMA has finished");

static int start_run = 0;
module_param(start_run, int_set_wake, 0644);
MODULE_PARM_DESC(start_run, "Flag to start a DMA transaction");

struct dmatest_slave_thread {
	struct device *dev;

	struct completion cmp;

	struct list_head node;
	struct task_struct *task;
	struct dma_chan *chan;

	enum dma_transaction_type type;
	bool done;
};

struct dmatest_chan {
	struct list_head node;
	struct dma_chan *chan;
	struct list_head threads;
};

/*
 * These are protected by dma_list_mutex since they're only used by
 * the DMA filter function callback
 */
static LIST_HEAD(dmatest_channels);

static void dmatest_slave_callback_result(void *p,
		const struct dmaengine_result *r)
{
	struct dmatest_slave_thread *thread = p;
	dev_info(thread->dev, "DMA engine result: code %u, residue %u\n",
		 r->result, r->residue);
	complete(&thread->cmp);
}

/* Runs inside the thread
 */
static int dmatest_run_single_test(void *data1, int sg_cnt)
{
	struct dmatest_slave_thread *thread = data1;
	const char *thread_name = current->comm;
	struct dma_async_tx_descriptor *txd = NULL;
	struct dma_device *dmadev;
	struct dma_chan *chan;
	struct device *dev = thread->dev;
	int buf_len, per_sg_len, off, i;
	enum dma_status status;
	u8 *data, align = 0;
	struct scatterlist sg[sg_cnt];
	unsigned long tx_tmo = msecs_to_jiffies(30000);
	enum dma_ctrl_flags flags;
	dma_cookie_t cookie;
	int ret;

	chan = thread->chan;
	dmadev = chan->device;
	align = dmadev->copy_align;

	buf_len = 1024 * 1024;
	per_sg_len = buf_len / sg_cnt;
	buf_len = per_sg_len * sg_cnt;

	data = kzalloc(buf_len, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	sg_init_table(sg, sg_cnt);
	for (off = 0, i = 0; i < sg_cnt; i++) {
		sg_set_buf(&sg[i], data + off, per_sg_len);
		off += per_sg_len;
	}
	dma_map_sg(dmadev->dev, sg, sg_cnt, DMA_FROM_DEVICE);

	flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;

	txd = dmaengine_prep_slave_sg(chan, sg, sg_cnt, DMA_DEV_TO_MEM, flags);
	if (!txd) {
		dev_err(dev, "Error preparing descriptor\n");
		ret = -1;
		goto out;
	}

	init_completion(&thread->cmp);
	txd->callback_result = dmatest_slave_callback_result;
	txd->callback_param = thread;
	cookie = txd->tx_submit(txd);

	ret = dma_submit_error(cookie);
	if (dma_submit_error(cookie)) {
		dev_err(dev, "Got error on cookie: %d\n", ret);
		goto out;
	}

	dma_async_issue_pending(chan);

	tx_tmo = wait_for_completion_timeout(&thread->cmp, tx_tmo);

	status = dma_async_is_tx_complete(chan, cookie, NULL, NULL);

	if (tx_tmo == 0) {
		dev_warn(dev, "%s: test timed out\n", thread_name);
		ret = -ETIMEDOUT;
		goto out;
	} else if (status != DMA_COMPLETE) {
		dev_warn(dev, "%s: tx got completion callback, ",
			 thread_name);
		dev_warn(dev, "but status is \'%s\'\n",
			 status == DMA_ERROR ? "error" :  "in progress");
		ret = -1;
		goto out;
	}

	dev_info(dev, "Got via DMA: '%s'\n", (char *)data);

	ret = 0;
out:
	dma_unmap_sg(dmadev->dev, sg, sg_cnt, DMA_FROM_DEVICE);
	kfree(data);

	return ret;
}

/* Function for slave transfers
 */
static int dmatest_slave_func(void *data)
{
	struct dmatest_slave_thread *thread = data;
	const char *thread_name = current->comm;
	unsigned int total_tests = 0;
	unsigned int failed_tests = 0;
	struct device *dev = thread->dev;
	int ret;

	smp_rmb();
	set_user_nice(current, 10);

	while (!kthread_should_stop()) {
		wait_event(thread_wait_start_run, start_run);

		if (!expected_data || !strlen(expected_data)) {
			dev_notice(dev, "Empty expected data, stopping...\n");
			start_run = 0;
			msleep(100);
			continue;
		}

		ret = dmatest_run_single_test(data, sg_cnt);
		if (ret < 0)
			failed_tests++;
		total_tests++;

		start_run = 0;
	}

	dev_notice(dev, "%s: terminating after %u tests, %u failures\n",
		   thread_name, total_tests, failed_tests);

	thread->done = true;

	return 0;
}

static void dmatest_cleanup_channel(struct device *dev,
		struct dmatest_chan *dtc)
{
	struct dmatest_slave_thread *thread;
	struct dmatest_slave_thread *_thread;
	int ret;

	list_for_each_entry_safe(thread, _thread, &dtc->threads, node) {
		ret = kthread_stop(thread->task);
		dev_dbg(dev, "dmatest: thread %s exited with status %d\n",
			thread->task->comm, ret);
		list_del(&thread->node);
		put_task_struct(thread->task);
		kfree(thread);
	}
	kfree(dtc);
}

static int dmatest_add_slave_thread(struct device *dev,
		struct dmatest_chan *dtc)
{
	struct dmatest_slave_thread *thread;
	struct dma_chan *chan = dtc->chan;

	thread = kzalloc(sizeof(struct dmatest_slave_thread), GFP_KERNEL);
	if (!thread) {
		dev_warn(dev, "No memory for slave thread %s-%s\n",
			 dev_name(dev), dma_chan_name(chan));
		return -ENOMEM;
	}

	thread->chan = chan;
	thread->dev = dev;
	thread->type = (enum dma_transaction_type)DMA_SLAVE;
	smp_wmb();
	thread->task = kthread_run(dmatest_slave_func, thread, "%s-%s",
		dev_name(dev), dma_chan_name(chan));
	if (IS_ERR(thread->task)) {
		dev_warn(dev, "Failed to run thread %s-%s\n",
			 dev_name(dev), dma_chan_name(chan));
		kfree(thread);
		return PTR_ERR(thread->task);
	}

	/* srcbuf and dstbuf are allocated by the thread itself */
	get_task_struct(thread->task);
	list_add_tail(&thread->node, &dtc->threads);

	return 1;
}

static int dmatest_add_slave_channel(struct device *dev, struct dma_chan *chan)
{
	struct dmatest_chan *dtc;
	unsigned int thread_count = 0;
	int ret;

	dtc = kmalloc(sizeof(struct dmatest_chan), GFP_KERNEL);
	if (!dtc) {
		dev_warn(dev, "No memory for %s\n", dma_chan_name(chan));
		return -ENOMEM;
	}

	dtc->chan = chan;
	INIT_LIST_HEAD(&dtc->threads);

	ret = dmatest_add_slave_thread(dev, dtc);
	if (ret <= 0)
		return ret;
	thread_count += ret;

	dev_info(dev, "Started %u thread(s) using %s\n",
		 thread_count, dma_chan_name(chan));

	list_add_tail(&dtc->node, &dmatest_channels);

	return 0;
}

static int adi_axi_dmac_test_probe(struct platform_device *pdev)
{
	struct dma_chan *chan;
	int err;

	chan = dma_request_slave_channel(&pdev->dev, "axidma0");
	if (IS_ERR(chan)) {
		dev_err(&pdev->dev, "No channel with name 'axidma0' found\n");
		return PTR_ERR(chan);
	}

	err = dmatest_add_slave_channel(&pdev->dev, chan);
	if (err) {
		dev_err(&pdev->dev, "Unable to add channel\n");
		goto free_chan;
	}

	return 0;

free_chan:
	dma_release_channel(chan);

	return err;
}

static int adi_axi_dmac_test_remove(struct platform_device *pdev)
{
	struct dmatest_chan *dtc, *_dtc;
	struct dma_chan *chan;

	list_for_each_entry_safe(dtc, _dtc, &dmatest_channels, node) {
		list_del(&dtc->node);
		chan = dtc->chan;
		dmatest_cleanup_channel(&pdev->dev, dtc);
		dev_info(&pdev->dev,"dropped channel %s\n",
			 dma_chan_name(chan));
		dmaengine_terminate_all(chan);
		dma_release_channel(chan);
	}
	return 0;
}

static const struct of_device_id adi_axi_dmac_test_of_ids[] = {
	{ .compatible = "adi,axi-dmac-test-1.00.a",},
	{}
};

static struct platform_driver adi_axi_dmac_test_driver = {
	.driver = {
		.name = "adi_axi_dmac_test",
		.owner = THIS_MODULE,
		.of_match_table = adi_axi_dmac_test_of_ids,
	},
	.probe = adi_axi_dmac_test_probe,
	.remove = adi_axi_dmac_test_remove,
};

static int __init adi_axi_dmac_init(void)
{
	return platform_driver_register(&adi_axi_dmac_test_driver);

}
late_initcall(adi_axi_dmac_init);

static void __exit adi_axi_dmac_exit(void)
{
	platform_driver_unregister(&adi_axi_dmac_test_driver);
}
module_exit(adi_axi_dmac_exit)

MODULE_AUTHOR("Analog Devices, Inc.");
MODULE_DESCRIPTION("Analog Devices AXI DMAC Test Client");
MODULE_LICENSE("GPL v2");
