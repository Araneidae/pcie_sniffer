#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>

#include <linux/string.h>   // memset, testing only.
#include <linux/delay.h>
#include <linux/completion.h>


/* Xilinx vendor id. */
#define XILINX_VID      0x10EE
#define XILINX_DID      0x0007


MODULE_AUTHOR("Michael Abbott, Diamond Light Source Ltd.");
MODULE_DESCRIPTION("Driver for PCIe Fast Acquisition Sniffer");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.0-dev");


/* Register map for FA sniffer PCIe interface. */
struct x5pcie_dma_registers {
    u32 dcsr;         /** 0x00 device control status register*/
    u32 ddmacr;       /** 0x04 device DMA control status register */
    u32 wdmatlpa;     /** 0x08 write DMA TLP address */
    u32 wdmatlps;     /** 0x0C write DMA TLP Size */
    u32 wdmatlpc;     /** 0x10 write DMA TLP count */
    u32 wdmatlpp;     /** 0x14 write DMA pattern */
    u32 rdmatlpp;     /** 0x18 read DMA expected pattern */
    u32 rdmatlpa;     /** 0x1C read DMA TLP address*/
    u32 rdmatlps;     /** 0x20 read DMA TLP size*/
    u32 rdmatlpc;     /** 0x24 read DMA TLP count*/
    u32 wdmaperf;     /** 0x28 write DMA performace*/
    u32 rdmaperf;     /** 0x2C read DMA performace*/
    u32 rdmastat;     /** 0x30 read DMA status*/
    u32 nrdcomp;      /** 0x34 Number of Read Completion*/
    u32 rcompdsizw;   /** 0x38 Read Completion Data Size */
    u32 dlwstat;      /** 0x3C Device Link Width Status*/
    u32 dltrsstat;    /** 0x40 Device Link Transaction Size Status */
    u32 dmisccont;    /** 0x44 Device Miscellaneous Control */
    u32 ccfaiirqclr;  /** 0x48 CC FAI interrupt clear register */
    u32 dummy[13];    /** 0x4C-0x80 Reserved Address Space */
    u32 ccfaicfgval;  /** Ox48 CC FAI configuration register */
    u32 wdmairqperf;  /** 0x50 WDMA Irq Timer */
};

#define BAR0_LEN    8192

/* Device specific data. */
struct fa_sniffer_data {
    struct cdev cdev;           // Character device
    struct class * class;       // Device class
    struct device * device;     // Parent device
    struct x5pcie_dma_registers * bar0;
};



DECLARE_COMPLETION(irq_done);

bool sw = false;



#define TEST_(test, rc, err, target, message) \
    do if (test) \
    { \
        rc = (err); \
        printk(KERN_ERR "fa_sniffer: " message ": %d\n", -rc); \
        goto target; \
    } while (0)

#define TEST_RC(rc, target, message) \
    TEST_((rc) < 0, rc, rc, target, message)

#define TEST_PTR(rc, ptr, target, message) \
    TEST_(IS_ERR(ptr) < 0, rc, PTR_ERR(ptr), target, message)


static int code2size(int bCode)
{
    bCode = bCode & 0x7;
    if (bCode > 0x05)
        return 0;
    else
        return 128 << bCode;
}

static int DMAGetMaxPacketSize(struct x5pcie_dma_registers *regs)
{
    /* Read encoded max payload sizes */
    int dltrsstat = regs->dltrsstat;
    /* Convert encoded max payload sizes into byte count */
    /* bits [2:0] : Capability maximum payload size for the device */
    int wMaxCapPayload = code2size(dltrsstat);
    /* bits [10:8] : Programmed maximum payload size for the device */
    int wMaxProgPayload = code2size(dltrsstat >> 8);

    if (wMaxCapPayload < wMaxProgPayload)
        return wMaxCapPayload;
    else
        return wMaxProgPayload;
}

/* Prepares FA Sniffer card to perform DMA. */
static void prepare_dma(
    struct x5pcie_dma_registers *regs,
    u32 frame_count, dma_addr_t dma_addrA, dma_addr_t dma_addrB, u32 swCount)
{
    /* Prepare DMA Registers */

    /* Get Maximum TLP size and compute how many TLPs are required for one
     * frame of 2048 bytes */
    int wSize = DMAGetMaxPacketSize(regs)/4; // converted to DWORDs
    int wCount = 2048/wSize;                 // # of TLPs required for a frame
    int bTrafficClass = 0;  // Default Memory Write TLP Traffic Class
    int fEnable64bit = 1;   // Enable 64b Memroy Write TLP Generation

    /* The register structure below may seem a bit awkward, but it is kept
     * like this to be register-compatible with Xilinx reference design
     * (xapp1052).  */

    // Prepare Buffer-A TLP Size & Upper Address
    u32 wtlps =
        (wSize & 0x1FFF) | ((bTrafficClass & 0x7) << 16) |
        ((fEnable64bit & 1) << 19) |
        ((u32) (dma_addrA >> 32) << 24);
    // Prepare Buffer-B TLP Size & Upper Address
    u32 rtlps =
        (wSize & 0x1FFF) | ((bTrafficClass & 0x7) << 16) |
        ((fEnable64bit & 1) << 19) |
        ((u32) (dma_addrB >> 32) << 24);

    // Write Buffer-A&B Lower DMA Address
    writel((u32)dma_addrA, &regs->wdmatlpa);
    writel((u32)dma_addrB, &regs->rdmatlpa);

    // Write Buffer-A&B Upper DMA Address
    writel(wtlps, &regs->wdmatlps);
    writel(rtlps, &regs->rdmatlps);

    // Memory Write TLP Count (for one frame)
    writel(wCount, &regs->wdmatlpc);

    // Switch count, and
    // Buffer length in terms of number of 2K frames
    writel((swCount << 16) | frame_count, &regs->wdmatlpp);

    // Assert Initiator Reset
    writel(1, &regs->dcsr);
    writel(0, &regs->dcsr);
}



static int fa_sniffer_open(struct inode *inode, struct file *file)
{
    struct fa_sniffer_data *fa_sniffer =
        container_of(inode->i_cdev, struct fa_sniffer_data, cdev);
    file->private_data = fa_sniffer;
    return 0;
}


static int fa_sniffer_release(struct inode *inode, struct file *file)
{
    return 0;
}


static ssize_t fa_sniffer_read(
    struct file *file, char *buf, size_t count, loff_t *f_pos)
{
    struct fa_sniffer_data *fa_sniffer = file->private_data;
    struct x5pcie_dma_registers *regs = fa_sniffer->bar0;

    INIT_COMPLETION(irq_done);

    /* Buffer size is set to max 128KB. It can hold 64 frames */
    size_t bufSize = 64*2048;
    /* Compute how many times buffers are going to be switched based on 
     * how many frames user wants to capture. */
    int swCount = count / bufSize;


    /* Buffer-A Allocation */
    void * bufferA = kmalloc(bufSize, GFP_KERNEL | __GFP_COLD);
    TEST_PTR(bufSize, bufferA, no_buffer, "oh dear oh dear oh dear");

    memset(bufferA, 0, bufSize);       // for the moment

    dma_addr_t dma_addrA = dma_map_single(
        fa_sniffer->device, bufferA, bufSize, DMA_FROM_DEVICE);
    TEST_(dma_addrA == 0, bufSize, -ENOMEM, no_dma_addr,
        "Unable to map DMA bufferA");

    /* Buffer-B Allocation */
    void * bufferB = kmalloc(bufSize, GFP_KERNEL | __GFP_COLD);
    TEST_PTR(bufSize, bufferB, no_buffer, "oh dear oh dear oh dear");

    memset(bufferB, 0, bufSize);       // for the moment

    dma_addr_t dma_addrB = dma_map_single(
        fa_sniffer->device, bufferB, bufSize, DMA_FROM_DEVICE);
    TEST_(dma_addrB == 0, bufSize, -ENOMEM, no_dma_addr,
        "Unable to map DMA bufferA");

    /* Re-start CC */
    writel(0x0, &regs->ccfaicfgval);
    writel(0x8, &regs->ccfaicfgval);

    /* Prepare DMA Registers */
    prepare_dma(regs, bufSize/2048, dma_addrA, dma_addrB, swCount);

    /* Start FA data acquisition */
    writel(1, &regs->ddmacr);

    int i;
    for (i = 0; i <= swCount; i++) {
        int rc = wait_for_completion_interruptible(&irq_done);
        if (rc) {
            count = rc;
            goto no_dma_addr;
        }

        void * buffer = sw ? bufferA : bufferB;
        size_t copy_count = count > bufSize ? bufSize : count;
        if (copy_to_user(buf, buffer, copy_count) != 0)
        {
            count = -EFAULT;
            goto no_dma_addr;
        }

        buf += copy_count;
    }

    printk(KERN_ERR "Buffers are copied.\n");


no_dma_addr:
    kfree(bufferA);

no_buffer:
    return count;

}

static struct file_operations fa_sniffer_fops = {
    .owner = THIS_MODULE,
    .open = fa_sniffer_open,
    .release = fa_sniffer_release,
    .read = fa_sniffer_read
};

static irqreturn_t fa_sniffer_irq(
    int irq, void * dev_id, struct pt_regs *pt_regs)
{
    struct fa_sniffer_data *fa_sniffer = dev_id;
    struct x5pcie_dma_registers *regs = fa_sniffer->bar0;
    regs->ccfaiirqclr = 0x100;
    sw = !sw;
    complete(&irq_done);
    return IRQ_HANDLED;
}


/* Returns the requested block of bar registers. */
static void * get_bar(struct pci_dev *dev, int bar, resource_size_t min_size)
{
    if (pci_resource_len(dev, bar) < min_size)
        return ERR_PTR(-EINVAL);
    return pci_iomap(dev, bar, min_size);
}


static int fa_sniffer_enable(
    struct pci_dev *dev, struct fa_sniffer_data *fa_sniffer)
{
    int rc = pci_enable_device(dev);
    TEST_RC(rc, no_device, "Unable to enable device");
    rc = pci_request_regions(dev, "fa_sniffer");
    TEST_RC(rc, no_regions, "Unable to reserve resources");

    pci_set_master(dev);

    /* Enable MSI (message based interrupts) and use the currently allocated
     * pci_dev irq. */
    rc = pci_enable_msi(dev);
    TEST_RC(rc, no_msi, "Unable to enable MSI");
    rc = request_irq(
        dev->irq, fa_sniffer_irq,
        IRQF_SHARED, "fa_sniffer", fa_sniffer);
    TEST_RC(rc, no_irq, "Unable to request irq");

    fa_sniffer->bar0 = get_bar(dev, 0, BAR0_LEN);
    TEST_PTR(rc, fa_sniffer->bar0, no_bar, "Cannot find registers");

    return 0;

no_bar:
    free_irq(dev->irq, fa_sniffer);
no_irq:

    pci_disable_msi(dev);
no_msi:

    pci_release_regions(dev);
no_regions:

    pci_disable_device(dev);
no_device:
    return rc;
}

static void fa_sniffer_disable(
    struct pci_dev *dev, struct fa_sniffer_data *fa_sniffer)
{
    free_irq(dev->irq, fa_sniffer);
    pci_disable_msi(dev);
    pci_iounmap(dev, fa_sniffer->bar0);
    pci_release_regions(dev);
    pci_disable_device(dev);
}


static int __devinit fa_sniffer_probe(
    struct pci_dev *dev, const struct pci_device_id *id)
{
    int rc = 0;
    struct fa_sniffer_data *fa_sniffer =
        kzalloc(sizeof(struct fa_sniffer_data), GFP_KERNEL);
    TEST_PTR(rc, fa_sniffer, no_memory, "Unable to allocate memory");

    fa_sniffer->device = &dev->dev;
    dev_set_drvdata(&dev->dev, fa_sniffer);

    rc = fa_sniffer_enable(dev, fa_sniffer);
    if (rc < 0)
        goto no_sniffer;

    dev_t devt;
    rc = alloc_chrdev_region(&devt, 0, 1, "fa_sniffer");
    TEST_RC(rc, no_chrdev, "Unable to allocate device");

    cdev_init(&fa_sniffer->cdev, &fa_sniffer_fops);
    fa_sniffer->cdev.owner = THIS_MODULE;
    rc = cdev_add(&fa_sniffer->cdev, devt, 1);
    TEST_RC(rc, no_cdev, "Unable to register device");

    fa_sniffer->class = class_create(THIS_MODULE, "fa_sniffer");
    TEST_PTR(rc, fa_sniffer->class, no_class, "Unable to create class");

    struct device * fa_sniffer_device = device_create(
        fa_sniffer->class, NULL, devt, "fa_sniffer");
    TEST_PTR(rc, fa_sniffer_device, no_device, "Unable to create device");

    printk(KERN_ERR "fa_sniffer loaded\n");
    return 0;

no_device:
    class_destroy(fa_sniffer->class);
no_class:
    cdev_del(&fa_sniffer->cdev);
no_cdev:
    unregister_chrdev_region(devt, 1);
no_chrdev:
    fa_sniffer_disable(dev, fa_sniffer);
no_sniffer:
    kfree(fa_sniffer);
no_memory:
    return rc;
}


static void __devexit fa_sniffer_remove(struct pci_dev *dev)
{
    struct fa_sniffer_data *fa_sniffer = dev_get_drvdata(&dev->dev);
    dev_t devt = fa_sniffer->cdev.dev;

    device_destroy(fa_sniffer->class, devt);
    class_destroy(fa_sniffer->class);
    cdev_del(&fa_sniffer->cdev);
    unregister_chrdev_region(devt, 1);
    fa_sniffer_disable(dev, fa_sniffer);
    kfree(fa_sniffer);
    printk(KERN_ERR "fa_sniffer unloaded\n");
}


static struct pci_device_id fa_sniffer_ids[] = {
    { PCI_DEVICE(XILINX_VID, XILINX_DID) },
    { 0 }
};

MODULE_DEVICE_TABLE(pci, fa_sniffer_ids);

static struct pci_driver fa_sniffer_driver = {
    .name = "fa_sniffer",
    .id_table = fa_sniffer_ids,
    .probe = fa_sniffer_probe,
    .remove = fa_sniffer_remove
};


static int __init fa_sniffer_init(void)
{
    return pci_register_driver(&fa_sniffer_driver);
}

static void __exit fa_sniffer_exit(void)
{
    pci_unregister_driver(&fa_sniffer_driver);
}


module_init(fa_sniffer_init);
module_exit(fa_sniffer_exit);
