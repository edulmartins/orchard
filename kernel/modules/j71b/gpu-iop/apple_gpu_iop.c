// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * gpu-001 experiment: boot the T8010 GPU coprocessor (gfx-kf, an AppleA7IOP
 * V2) with Apple's GFX firmware and bring up RTKit through the mainline
 * apple-rtkit core. No GPU functionality yet; the AGX endpoints are only
 * started and logged.
 *
 * The coprocessor has no DART. It sees one remapped window of physical
 * memory at IOP address 0, programmed in two register copies (Fender block
 * and IOP block). The firmware image runs from the start of that window;
 * RTKit buffers are carved from the rest of it.
 */
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>

/* "fender" block: GFX copy of the firmware window and the run bit */
#define GPU_FENDER_WINDOW		0x200c
#define GPU_FENDER_RUN			0x2048
#define GPU_FENDER_RUN_ENABLE		BIT(0)

/* "iop" block: AppleA7IOP V2 copy of the firmware window */
#define GPU_IOP_WINDOW			0x0008
#define GPU_IOP_WINDOW_ENABLE		0x0038

/* Window layout: phys lo/hi, IOP address lo/hi, size, zero; 4 bytes apart in
 * the Fender copy, 8 bytes apart in the IOP copy. */
#define GPU_WINDOW_SIZE			SZ_1M
#define GPU_FW_RUNTIME_SIZE		0x8f000
#define GPU_SCRATCH_SIZE		SZ_4M
#define GPU_EP_AGX_FIRMWARE		0x20
#define GPU_EP_AGX_DOORBELL		0x21

struct apple_gpu_iop {
	struct device *dev;
	void __iomem *fender;
	void __iomem *iop;
	struct apple_rtkit *rtk;
	void *mem;
	dma_addr_t mem_dma;
	size_t next;
	struct dentry *debugfs;
	/* Host memory the firmware maps by physical address (its own MMU) */
	void *scratch;
	dma_addr_t scratch_dma;
	void *crashlog;
	size_t crashlog_size;
};

static void apple_gpu_iop_set_window(struct apple_gpu_iop *gpu)
{
	u64 phys = gpu->mem_dma;
	static const u32 fields[] = { 0, 0, 0, 0, GPU_WINDOW_SIZE, 0 };
	int i;

	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		u32 value = fields[i];

		if (i == 0)
			value = lower_32_bits(phys);
		else if (i == 1)
			value = upper_32_bits(phys);
		writel(value, gpu->fender + GPU_FENDER_WINDOW + 4 * i);
		writel(value, gpu->iop + GPU_IOP_WINDOW + 8 * i);
	}
	writel(1, gpu->iop + GPU_IOP_WINDOW_ENABLE);
}

static int apple_gpu_iop_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_gpu_iop *gpu = cookie;
	size_t size = ALIGN(bfr->size, SZ_4K);

	if (bfr->iova) {
		/* Firmware-chosen address: must lie inside our window */
		if (bfr->iova + bfr->size > GPU_WINDOW_SIZE)
			return -EINVAL;
		bfr->buffer = gpu->mem + bfr->iova;
		bfr->is_mapped = true;
		return 0;
	}
	if (gpu->next + size > GPU_WINDOW_SIZE)
		return -ENOMEM;
	bfr->iova = gpu->next;
	bfr->buffer = gpu->mem + gpu->next;
	gpu->next += size;
	return 0;
}

static void apple_gpu_iop_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	/* Carved from the window; freed with it */
}

static void apple_gpu_iop_recv(void *cookie, u8 endpoint, u64 message)
{
	struct apple_gpu_iop *gpu = cookie;

	dev_info(gpu->dev, "ep %#x: %#016llx\n", endpoint, message);
}

static void apple_gpu_iop_crashed(void *cookie, const void *crashlog, size_t size)
{
	struct apple_gpu_iop *gpu = cookie;

	dev_err(gpu->dev, "firmware crashed (%zu byte crashlog)\n", size);
	kfree(gpu->crashlog);
	gpu->crashlog = crashlog ? kmemdup(crashlog, size, GFP_ATOMIC) : NULL;
	gpu->crashlog_size = gpu->crashlog ? size : 0;
	if (crashlog)
		print_hex_dump(KERN_ERR, "crashlog: ", DUMP_PREFIX_OFFSET, 16, 1,
			       crashlog, min_t(size_t, size, 512), true);
}

/*
 * Experiment interface (debugfs apple_gpu_iop/):
 *   mem       the whole window, read/write at IOP addresses
 *   send      write "<endpoint> <message>" to post an RTKit message
 *   crashlog  the last crashlog, raw
 */
static ssize_t mem_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{
	struct apple_gpu_iop *gpu = file->private_data;

	return simple_read_from_buffer(buf, len, pos, gpu->mem, GPU_WINDOW_SIZE);
}

static ssize_t mem_write(struct file *file, const char __user *buf, size_t len, loff_t *pos)
{
	struct apple_gpu_iop *gpu = file->private_data;

	return simple_write_to_buffer(gpu->mem, GPU_WINDOW_SIZE, pos, buf, len);
}

static const struct file_operations mem_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mem_read,
	.write = mem_write,
	.llseek = default_llseek,
};

static ssize_t send_write(struct file *file, const char __user *buf, size_t len, loff_t *pos)
{
	struct apple_gpu_iop *gpu = file->private_data;
	char line[64];
	unsigned int ep;
	u64 msg;
	int ret;

	if (len >= sizeof(line))
		return -EINVAL;
	if (copy_from_user(line, buf, len))
		return -EFAULT;
	line[len] = 0;
	if (sscanf(line, "%x %llx", &ep, &msg) != 2 || ep > 0xff)
		return -EINVAL;
	dev_info(gpu->dev, "tx ep %#x: %#016llx\n", ep, msg);
	ret = apple_rtkit_send_message(gpu->rtk, ep, msg, NULL, false);
	return ret ? ret : len;
}

static ssize_t scratch_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{
	struct apple_gpu_iop *gpu = file->private_data;

	return simple_read_from_buffer(buf, len, pos, gpu->scratch, GPU_SCRATCH_SIZE);
}

static ssize_t scratch_write(struct file *file, const char __user *buf, size_t len, loff_t *pos)
{
	struct apple_gpu_iop *gpu = file->private_data;

	return simple_write_to_buffer(gpu->scratch, GPU_SCRATCH_SIZE, pos, buf, len);
}

static const struct file_operations scratch_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = scratch_read,
	.write = scratch_write,
	.llseek = default_llseek,
};

static const struct file_operations send_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = send_write,
};

static ssize_t crashlog_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{
	struct apple_gpu_iop *gpu = file->private_data;

	return simple_read_from_buffer(buf, len, pos, gpu->crashlog, gpu->crashlog_size);
}

static const struct file_operations crashlog_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = crashlog_read,
};

static const struct apple_rtkit_ops apple_gpu_iop_rtkit_ops = {
	.crashed = apple_gpu_iop_crashed,
	.recv_message = apple_gpu_iop_recv,
	.shmem_setup = apple_gpu_iop_shmem_setup,
	.shmem_destroy = apple_gpu_iop_shmem_destroy,
};

static int apple_gpu_iop_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct firmware *fw;
	struct apple_gpu_iop *gpu;
	const char *name;
	int ret;

	gpu = devm_kzalloc(dev, sizeof(*gpu), GFP_KERNEL);
	if (!gpu)
		return -ENOMEM;
	gpu->dev = dev;
	platform_set_drvdata(pdev, gpu);

	gpu->fender = devm_platform_ioremap_resource_byname(pdev, "fender");
	if (IS_ERR(gpu->fender))
		return PTR_ERR(gpu->fender);
	gpu->iop = devm_platform_ioremap_resource_byname(pdev, "iop");
	if (IS_ERR(gpu->iop))
		return PTR_ERR(gpu->iop);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	gpu->mem = dmam_alloc_coherent(dev, GPU_WINDOW_SIZE, &gpu->mem_dma, GFP_KERNEL);
	if (!gpu->mem)
		return -ENOMEM;
	if (!IS_ALIGNED(gpu->mem_dma, SZ_16K))
		return dev_err_probe(dev, -EINVAL, "window %pad not 16K aligned\n", &gpu->mem_dma);
	gpu->next = GPU_FW_RUNTIME_SIZE;
	gpu->scratch = dmam_alloc_coherent(dev, GPU_SCRATCH_SIZE, &gpu->scratch_dma, GFP_KERNEL);
	if (!gpu->scratch)
		return -ENOMEM;

	ret = of_property_read_string(dev->of_node, "firmware-name", &name);
	if (ret)
		return dev_err_probe(dev, ret, "no firmware-name\n");
	ret = request_firmware(&fw, name, dev);
	if (ret)
		return dev_err_probe(dev, ret, "cannot load %s\n", name);
	if (fw->size > GPU_FW_RUNTIME_SIZE) {
		release_firmware(fw);
		return dev_err_probe(dev, -EINVAL, "%s too large\n", name);
	}
	memcpy(gpu->mem, fw->data, fw->size);
	release_firmware(fw);

	gpu->rtk = devm_apple_rtkit_init(dev, gpu, NULL, 0, &apple_gpu_iop_rtkit_ops);
	if (IS_ERR(gpu->rtk))
		return dev_err_probe(dev, PTR_ERR(gpu->rtk), "rtkit init failed\n");

	/* Powers ps_gfx; registers are inaccessible before this */
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	apple_gpu_iop_set_window(gpu);
	writel(readl(gpu->fender + GPU_FENDER_RUN) | GPU_FENDER_RUN_ENABLE,
	       gpu->fender + GPU_FENDER_RUN);

	ret = apple_rtkit_boot(gpu->rtk);
	if (ret) {
		dev_err(dev, "RTKit boot failed: %d\n", ret);
		goto err_stop;
	}
	dev_info(dev, "GFX firmware up, window %pad\n", &gpu->mem_dma);
	dev_info(dev, "scratch %pad + %#x\n", &gpu->scratch_dma, GPU_SCRATCH_SIZE);

	gpu->debugfs = debugfs_create_dir("apple_gpu_iop", NULL);
	debugfs_create_file("mem", 0600, gpu->debugfs, gpu, &mem_fops);
	debugfs_create_file("send", 0200, gpu->debugfs, gpu, &send_fops);
	debugfs_create_file("crashlog", 0400, gpu->debugfs, gpu, &crashlog_fops);
	debugfs_create_file("scratch", 0600, gpu->debugfs, gpu, &scratch_fops);

	ret = apple_rtkit_start_ep(gpu->rtk, GPU_EP_AGX_FIRMWARE);
	if (!ret)
		ret = apple_rtkit_start_ep(gpu->rtk, GPU_EP_AGX_DOORBELL);
	if (ret)
		dev_warn(dev, "starting AGX endpoints failed: %d\n", ret);
	return 0;

err_stop:
	writel(readl(gpu->fender + GPU_FENDER_RUN) & ~GPU_FENDER_RUN_ENABLE,
	       gpu->fender + GPU_FENDER_RUN);
	pm_runtime_put(dev);
	return ret;
}

static void apple_gpu_iop_remove(struct platform_device *pdev)
{
	struct apple_gpu_iop *gpu = platform_get_drvdata(pdev);

	debugfs_remove(gpu->debugfs);
	kfree(gpu->crashlog);
	if (apple_rtkit_is_running(gpu->rtk))
		apple_rtkit_shutdown(gpu->rtk);
	writel(readl(gpu->fender + GPU_FENDER_RUN) & ~GPU_FENDER_RUN_ENABLE,
	       gpu->fender + GPU_FENDER_RUN);
	pm_runtime_put(&pdev->dev);
}

static const struct of_device_id apple_gpu_iop_of_match[] = {
	{ .compatible = "apple,t8010-gpu" },
	{}
};
MODULE_DEVICE_TABLE(of, apple_gpu_iop_of_match);

static struct platform_driver apple_gpu_iop_driver = {
	.driver = {
		.name = "apple-gpu-iop",
		.of_match_table = apple_gpu_iop_of_match,
	},
	.probe = apple_gpu_iop_probe,
	.remove = apple_gpu_iop_remove,
};
module_platform_driver(apple_gpu_iop_driver);

MODULE_DESCRIPTION("gpu-001 experiment: T8010 GPU coprocessor RTKit bring-up");
MODULE_LICENSE("Dual MIT/GPL");
