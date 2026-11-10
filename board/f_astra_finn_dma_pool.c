// SPDX-License-Identifier: GPL-2.0
// Private coherent CMA pool. This driver never accesses FPGA registers.
// region_base/region_bytes (TrustZone T1b): the pool is a no-map reserved region of the device tree instead (outside
// Linux memory, so the DDR sections the PL may reach hold the pool only); mapped write-combining as the coherent pool.
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#if defined(F_ASTRA_MODELS_POOL)
/* multi-model harness: a FINN loop model's DDR region (weights, intermediate frames) plus input chunks */
#define INPUT_ARENA_BYTES (72U * 1024U * 1024U)
#define POOL_NAME "f_astra_models_dma_pool"
#elif defined(F_ASTRA_MODEL_PAIR_POOL)
#define INPUT_ARENA_BYTES (32U * 1024U * 1024U)
#define POOL_NAME "f_astra_model_pair_dma_pool"
#else
#define INPUT_ARENA_BYTES (2U * 1024U * 1024U)
#define POOL_NAME "f_astra_finn_dma_pool"
#endif
#define POOL_SIZE (INPUT_ARENA_BYTES + 65536U)
struct pool_info { __u64 dma_address; __u32 bytes; __u32 output_offset; };
#define POOL_INFO _IOR('F', 1, struct pool_info)
static struct platform_device *pool_device;
static void *pool_cpu;
static dma_addr_t pool_dma;
static unsigned long region_base, region_bytes;
module_param(region_base, ulong, 0444);
MODULE_PARM_DESC(region_base, "physical base of a no-map reserved region holding the pool (0: CMA)");
module_param(region_bytes, ulong, 0444);
MODULE_PARM_DESC(region_bytes, "bytes of that region");
static DEFINE_MUTEX(pool_lock);
static int pool_open(struct inode *inode, struct file *file)
{
    if (!mutex_trylock(&pool_lock)) return -EBUSY;
    return 0;
}
static int pool_release(struct inode *inode, struct file *file)
{
    mutex_unlock(&pool_lock);
    return 0;
}
static long pool_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct pool_info info = { .dma_address = pool_dma, .bytes = POOL_SIZE,
                              .output_offset = INPUT_ARENA_BYTES };
    if (cmd != POOL_INFO) return -ENOTTY;
    return copy_to_user((void __user *)arg, &info, sizeof(info)) ? -EFAULT : 0;
}
static int pool_mmap(struct file *file, struct vm_area_struct *vma)
{
    if (vma->vm_pgoff || vma->vm_end-vma->vm_start != POOL_SIZE) return -EINVAL;
    if (region_base) {
        vma->vm_page_prot = pgprot_dmacoherent(vma->vm_page_prot);
        return remap_pfn_range(vma, vma->vm_start, region_base >> PAGE_SHIFT, POOL_SIZE, vma->vm_page_prot);
    }
    return dma_mmap_coherent(&pool_device->dev, vma, pool_cpu, pool_dma, POOL_SIZE);
}
static const struct file_operations pool_fops = {
    .owner=THIS_MODULE, .open=pool_open, .release=pool_release,
    .unlocked_ioctl=pool_ioctl, .mmap=pool_mmap,
};
static struct miscdevice pool_misc = {
    .minor=MISC_DYNAMIC_MINOR, .name=POOL_NAME,
    .fops=&pool_fops, .mode=0600,
};
static int __init pool_init(void)
{
    int ret;
    pool_device=platform_device_register_simple("f-astra-finn-pool",-1,NULL,0);
    if (IS_ERR(pool_device)) return PTR_ERR(pool_device);
    ret=dma_coerce_mask_and_coherent(&pool_device->dev,DMA_BIT_MASK(32));
    if (ret) goto fail_device;
    if (region_base) {
        void *p;
        // the region must hold the pool, lie in the 1 GiB DDR and be outside Linux memory (no-map)
        if ((region_base & ~PAGE_MASK) || region_bytes < POOL_SIZE || region_base < 0x00100000UL
            || region_base + region_bytes > 0x40000000UL || pfn_valid(PFN_DOWN(region_base))
            || pfn_valid(PFN_DOWN(region_base + region_bytes - 1))) {
            ret=-EINVAL; goto fail_device;
        }
        p=memremap(region_base,POOL_SIZE,MEMREMAP_WC);
        if (!p) { ret=-ENOMEM; goto fail_device; }
        memset(p,0,POOL_SIZE);
        memunmap(p);
        pool_dma=region_base;
        ret=misc_register(&pool_misc);
        if (!ret) return 0;
        goto fail_device;
    }
    pool_cpu=dma_alloc_coherent(&pool_device->dev,POOL_SIZE,&pool_dma,GFP_KERNEL);
    if (!pool_cpu) { ret=-ENOMEM; goto fail_device; }
    // The fixed Trenz handoff has 1 GiB DDR, never DMA to Linux-reserved guessed RAM.
    if (pool_dma < 0x00100000ULL || pool_dma+POOL_SIZE > 0x40000000ULL) {
        ret=-ERANGE; goto fail_pool;
    }
    ret=misc_register(&pool_misc);
    if (!ret) return 0;
fail_pool:
    dma_free_coherent(&pool_device->dev,POOL_SIZE,pool_cpu,pool_dma);
fail_device:
    platform_device_unregister(pool_device);
    return ret;
}
static void __exit pool_exit(void)
{
    misc_deregister(&pool_misc);
    if (!region_base) dma_free_coherent(&pool_device->dev,POOL_SIZE,pool_cpu,pool_dma);
    platform_device_unregister(pool_device);
}
module_init(pool_init); module_exit(pool_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("F-ASTRA fixed INT8 Model A private coherent DMA pool, no register access");
