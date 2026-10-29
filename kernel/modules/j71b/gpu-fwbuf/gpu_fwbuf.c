// SPDX-License-Identifier: GPL-2.0-only
/*
 * gpu-001 experiment only: one physically contiguous, uncached buffer that
 * userspace fills through debugfs, so a coprocessor can be pointed at it.
 *
 *   /sys/kernel/debug/gpu_fwbuf/data   write/read bytes (seekable)
 *   /sys/kernel/debug/gpu_fwbuf/phys   physical (DMA) address and size
 */
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/platform_device.h>

static unsigned long size = SZ_1M;
module_param(size, ulong, 0444);
MODULE_PARM_DESC(size, "buffer size in bytes (power of two keeps natural alignment)");

static struct platform_device *pdev;
static struct dentry *dir;
static void *cpu;
static dma_addr_t dma;

static ssize_t data_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{
	return simple_read_from_buffer(buf, len, pos, cpu, size);
}

static ssize_t data_write(struct file *file, const char __user *buf, size_t len, loff_t *pos)
{
	return simple_write_to_buffer(cpu, size, pos, buf, len);
}

static const struct file_operations data_fops = {
	.owner = THIS_MODULE,
	.read = data_read,
	.write = data_write,
	.llseek = default_llseek,
};

static int phys_show(struct seq_file *s, void *unused)
{
	seq_printf(s, "%pad %#lx\n", &dma, size);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(phys);

static int __init gpu_fwbuf_init(void)
{
	int ret;

	pdev = platform_device_register_simple("gpu-fwbuf", -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	ret = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		goto unregister;
	cpu = dma_alloc_coherent(&pdev->dev, size, &dma, GFP_KERNEL);
	if (!cpu) {
		ret = -ENOMEM;
		goto unregister;
	}
	dir = debugfs_create_dir("gpu_fwbuf", NULL);
	debugfs_create_file("data", 0600, dir, NULL, &data_fops);
	debugfs_create_file("phys", 0400, dir, NULL, &phys_fops);
	pr_info("gpu_fwbuf: %pad + %#lx\n", &dma, size);
	return 0;

unregister:
	platform_device_unregister(pdev);
	return ret;
}

static void __exit gpu_fwbuf_exit(void)
{
	debugfs_remove(dir);
	dma_free_coherent(&pdev->dev, size, cpu, dma);
	platform_device_unregister(pdev);
}

module_init(gpu_fwbuf_init);
module_exit(gpu_fwbuf_exit);
MODULE_DESCRIPTION("gpu-001 experiment: contiguous buffer for coprocessor firmware");
MODULE_LICENSE("GPL");
