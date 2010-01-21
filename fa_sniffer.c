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


MODULE_AUTHOR("Michael Abbott, Diamond Light Source Ltd.");
MODULE_DESCRIPTION("Driver for PCIe Fast Acquisition Sniffer");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.0-dev");


/* If test is true then do on_error, print message and goto target. */
#define TEST_(test, on_error, target, message) \
    do if (test) \
    { \
        on_error; \
        printk(KERN_ERR "fa_sniffer: " message "\n"); \
        goto target; \
    } while (0)

/* If rc is an error code (< 0) then print message and goto target. */
#define TEST_RC(rc, target, message) \
    TEST_((rc) < 0, , target, message)

/* If ptr indicates an error then assign an error code to rc, print message
 * and goto target. */
#define TEST_PTR(rc, ptr, target, message) \
    TEST_(IS_ERR(ptr), rc = PTR_ERR(ptr), target, message)







/*****************************************************************************/
/*                      FA Sniffer Hardware Definitions                      */
/*****************************************************************************/

/* A single FA frame is 256 X positions followed by 256 Y positions, each
 * position being a 4 byte integer. */
#define FA_FRAME_SIZE   2048


/* Xilinx vendor id. */
#define XILINX_VID      0x10EE
#define XILINX_DID      0x0007


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

struct fa_sniffer_hw {
    struct x5pcie_dma_registers * regs;
    int wSize;              
};

#define BAR0_LEN    8192


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

    return wMaxCapPayload < wMaxProgPayload ?
        wMaxCapPayload : wMaxProgPayload;
}

/* Returns the requested block of bar registers. */
static void * get_bar(struct pci_dev *dev, int bar, resource_size_t min_size)
{
    if (pci_resource_len(dev, bar) < min_size)
        return ERR_PTR(-EINVAL);
    return pci_iomap(dev, bar, min_size);
}



static int initialise_fa_hw(struct pci_dev *pdev, struct fa_sniffer_hw **hw)
{
    int rc = 0;
    *hw = kmalloc(sizeof(*hw), GFP_KERNEL);
    TEST_PTR(rc, *hw, no_hw, "Cannot allocate fa hardware");

    struct x5pcie_dma_registers *regs = get_bar(pdev, 0, BAR0_LEN);
    TEST_PTR(rc, regs, no_bar, "Cannot find registers");
    (*hw)->regs = regs;
    (*hw)->wSize = DMAGetMaxPacketSize(regs) / 4;

    printk(KERN_ERR "FA version %08x\n", regs->dcsr);

    /* Now restart the communication controller: needed at present to work
     * around a controller defect. */
    writel(0, &regs->ccfaicfgval);
    writel(8, &regs->ccfaicfgval);
    
    return rc;

no_bar:
    kfree(*hw);
no_hw:
    return rc;
}


static void release_fa_hw(struct pci_dev *pdev, struct fa_sniffer_hw *hw)
{
    pci_iounmap(pdev, hw->regs);
    kfree(hw);
}



static void set_dma_buffer(
    struct fa_sniffer_hw *hw, bool a_or_b, dma_addr_t buffer)
{
    /* Get Maximum TLP size and compute how many TLPs are required for one
     * frame of 2048 bytes */
    int bTrafficClass = 0;  // Default Memory Write TLP Traffic Class
    int fEnable64bit = 1;   // Enable 64b Memroy Write TLP Generation

    u32 top_word =
        (hw->wSize & 0x1FFF) | ((bTrafficClass & 0x7) << 16) |
        ((fEnable64bit & 1) << 19) |
        ((u32) (buffer >> 32) << 24);
    u32 bottom_word = (u32) buffer;

    printk(KERN_ERR "set_dma_buffer %p, %d, %llx\n", hw, a_or_b, buffer);
    
    if (a_or_b) {
        writel(bottom_word, &hw->regs->wdmatlpa);
        writel(top_word,    &hw->regs->wdmatlps);
    } else {
        writel(bottom_word, &hw->regs->rdmatlpa);
        writel(top_word,    &hw->regs->rdmatlps);
    }
}


/* Prepares FA Sniffer card to perform DMA. */
static void prepare_dma(
    struct fa_sniffer_hw *hw,
    u32 frame_count, u32 swCount,
    dma_addr_t dma_addrA, dma_addr_t dma_addrB)
{
    /* Get Maximum TLP size and compute how many TLPs are required for one
     * frame of 2048 bytes */
    int wCount = FA_FRAME_SIZE / hw->wSize;
    
    set_dma_buffer(hw, true, dma_addrA);
    set_dma_buffer(hw, true, dma_addrB);

    // Memory Write TLP Count (for one frame)
    writel(wCount, &hw->regs->wdmatlpc);

    // Switch count, and
    // Buffer length in terms of number of 2K frames
    writel((swCount << 16) | frame_count, &hw->regs->wdmatlpp);

    // Assert Initiator Reset
    writel(1, &hw->regs->dcsr);
    writel(0, &hw->regs->dcsr);
}


static void fa_int_ack(struct fa_sniffer_hw *hw)
{
    hw->regs->ccfaiirqclr = 0x100;
}

static void stop_fa_hw(struct fa_sniffer_hw *hw)
{
    writel(0, &hw->regs->ddmacr);
    mmiowb();                   // Not sure we need this
}

static void start_fa_hw(struct fa_sniffer_hw *hw)
{
    writel(1, &hw->regs->ddmacr);
}



/*****************************************************************************/
/*                       Character Device Interface                          */
/*****************************************************************************/

#define FA_BUFFER_COUNT     5
#define FA_BLOCK_FRAMES     64
#define FA_BLOCK_SIZE       (FA_BLOCK_FRAMES * 2048)


enum fa_block_state {
    fa_block_free,      // Not in use
    fa_block_dma,       // Allocated to DMA
    fa_block_data       // Contains useful data.
};


/* Device specific data. */
struct fa_sniffer {
    struct cdev cdev;           // Character device
    struct pci_dev *pdev;       // Parent PCI device
    struct fa_sniffer_hw *hw;   // Hardware resources
    struct class *class;        // Device class
    unsigned long open_flag;    // Interlock: only one open at a time!
};

struct fa_sniffer_open {
    struct fa_sniffer *fa_sniffer;
    wait_queue_head_t wait_queue;

    struct fa_block {
        void * block;
        dma_addr_t dma;
        enum fa_block_state state;
    } buffers[FA_BUFFER_COUNT];

    /* Device status.  There are two DMA buffers allocated to the device, one
     * currently being read, the next queued to be read.  We will be
     * interrupted when the current buffer is switched. */
//    spinlock_t isr_lock;
    bool a_or_b;                // Used to track the current buffer
    int isr_block_index;        // Block currently being read into by DMA
    
    /* Reader status. */
    bool overrun_detected;      // Set by ISR, read by reader
    int read_block_index;       // Index of next block in buffers[] to read
    int read_offset;            // Offset into current block to read.
};




static inline int step_index(int ix, int step)
{
    ix += step;
    if (ix >= FA_BUFFER_COUNT)
        ix -= FA_BUFFER_COUNT;
    return ix;
}


static irqreturn_t fa_sniffer_irq(
    int irq, void * dev_id, struct pt_regs *pt_regs)
{
    struct fa_sniffer_open *open = dev_id;
    struct pci_dev *pdev = open->fa_sniffer->pdev;
    struct fa_sniffer_hw *hw = open->fa_sniffer->hw;

    printk(KERN_ERR "Got FA interrupt: %p\n", open);
    /* Acknowledge the interrupt. */
    fa_int_ack(hw);


//     unsigned long flags;
//     spin_lock_irqsave(&fa_sniffer->isr_lock, flags);
//         
//     spin_unlock_irqrestore(&fa_sniffer->isr_lock, flags);


    int filled_ix = open->isr_block_index;
    int fresh_ix = step_index(filled_ix, 2);

    printk(KERN_ERR "ix: %d, %d\n", filled_ix, fresh_ix);
    printk(KERN_ERR "about to pci_dma_sync_single_for_cpu(%p, %llx,...)\n",
        pdev, open->buffers[filled_ix].dma);
    pci_dma_sync_single_for_cpu(pdev, 
        open->buffers[filled_ix].dma, FA_BLOCK_SIZE, DMA_FROM_DEVICE);
    open->buffers[filled_ix].state = fa_block_data;

    TEST_(open->buffers[fresh_ix].state != fa_block_free, ,
        data_overrun, "Data buffer overrun in IRQ\n");
    printk(KERN_ERR "about to pci_dma_sync_single_for_device(%p, %llx,...)\n",
        pdev, open->buffers[fresh_ix].dma);
    pci_dma_sync_single_for_device(pdev, 
        open->buffers[fresh_ix].dma, FA_BLOCK_SIZE, DMA_FROM_DEVICE);
    set_dma_buffer(hw,
        open->a_or_b, open->buffers[fresh_ix].dma);
    open->buffers[fresh_ix].state = fa_block_dma;

    /* Move through the circular buffer. */
    open->isr_block_index = step_index(filled_ix, 1);
    open->a_or_b = ! open->a_or_b;

    wake_up_interruptible(&open->wait_queue);
    return IRQ_HANDLED;


data_overrun:
    /* Whoops.  Better stop the hardware right away! */
    stop_fa_hw(hw);
    open->overrun_detected = true;

    /* Wake up the user. */
    wake_up_interruptible(&open->wait_queue);
    return IRQ_HANDLED;
}


static int fa_sniffer_open(struct inode *inode, struct file *file)
{
    struct fa_sniffer *fa_sniffer =
        container_of(inode->i_cdev, struct fa_sniffer, cdev);
    struct pci_dev *pdev = fa_sniffer->pdev;

    if (test_and_set_bit(0, &fa_sniffer->open_flag))
        /* No good, the device is already open. */
        return -EINVAL;

    int rc = 0;
    struct fa_sniffer_open *open = kmalloc(sizeof(*open), GFP_KERNEL);
    TEST_PTR(rc, open, no_open, "Unable to allocate open structure");

    init_waitqueue_head(&open->wait_queue);
    open->a_or_b = true;
    open->isr_block_index = 0;
    open->overrun_detected = false;
    open->read_block_index = 0;
    open->read_offset = 0;
    open->fa_sniffer = fa_sniffer;
    file->private_data = open;

    /* Prepare the circular buffer. */
    int blk;
    for (blk = 0; blk < FA_BUFFER_COUNT; blk++) {
        struct fa_block *block = &open->buffers[blk];
        /* We ask for "cache cold" pages just to optimise things, as these
         * pages won't be read without DMA first. */
        block->block = kmalloc(FA_BLOCK_SIZE, GFP_KERNEL | __GFP_COLD);
// Should we use __get_free_pages() instead here?
        TEST_PTR(rc, block->block, no_block, "Unable to allocate buffer");

        /* Map each block for DMA. */
        block->dma = pci_map_single(
            pdev, block->block, FA_BLOCK_SIZE, DMA_FROM_DEVICE);
        TEST_(pci_dma_mapping_error(block->dma),
            rc = -EIO, no_dma_map, "Unable to map DMA block");
        block->state = fa_block_free;
    }

    /* Prepare the initial hardware DMA buffers. */
    open->buffers[0].state = fa_block_dma;
    open->buffers[1].state = fa_block_dma;
    prepare_dma(fa_sniffer->hw, FA_BLOCK_FRAMES, 2, // -1,
        open->buffers[0].dma, open->buffers[1].dma);

    /* Set up the interrupt routine.  This should not trigger until we call
     * start_fa_hw(). */
    rc = request_irq(
        pdev->irq, fa_sniffer_irq,
        IRQF_SHARED, "fa_sniffer", open);
    TEST_RC(rc, no_irq, "Unable to request irq");
    start_fa_hw(fa_sniffer->hw);

    return 0;

no_irq:
   
    /* Release circular buffer resources.  Rather tricky interlocking with
     * allocation loop above so that we release precisely those resources we
     * allocated, in reverse order. */
    do {
        blk -= 1;
        pci_unmap_single(pdev, open->buffers[blk].dma,
            FA_BLOCK_SIZE, DMA_FROM_DEVICE);
no_dma_map:
        kfree(open->buffers[blk].block);
no_block:
        ;
    } while (blk > 0);

    kfree(open);
no_open:
    return rc;
}


static int fa_sniffer_release(struct inode *inode, struct file *file)
{
    struct fa_sniffer *fa_sniffer =
        container_of(inode->i_cdev, struct fa_sniffer, cdev);
    struct pci_dev *pdev = fa_sniffer->pdev;
    struct fa_sniffer_open *open = file->private_data;

    stop_fa_hw(fa_sniffer->hw);
    free_irq(pdev->irq, open);
    int blk;
    for (blk = 0; blk < FA_BUFFER_COUNT; blk++) {
        pci_unmap_single(pdev, open->buffers[blk].dma,
            FA_BLOCK_SIZE, DMA_FROM_DEVICE);
        kfree(open->buffers[blk].block);
    }
    kfree(open);

    /* Do this last to let somebody else use this device. */
    test_and_clear_bit(0, &fa_sniffer->open_flag);
    return 0;
}


static ssize_t fa_sniffer_read(
    struct file *file, char *buf, size_t count, loff_t *f_pos)
{
    struct fa_sniffer_open *open = file->private_data;
    size_t copied = 0;
    int rc = 0;
    while (count > 0) {
        /* Wait for data to arrive in the current block. */
        struct fa_block * block =
            & open->buffers[open->read_block_index];
        rc = wait_event_interruptible(open->wait_queue,
            block->state == fa_block_data  ||  open->overrun_detected);
        TEST_RC(rc, failed, "read interrupted");
        TEST_(block->state != fa_block_data,
            rc = -EIO, failed, "No data available");

        /* Copy as much data as needed and available out of the current block,
         * and advance all our buffers and pointers. */
        size_t read_offset = open->read_offset;
        size_t copy_count = FA_BLOCK_SIZE - read_offset;
        if (copy_count > count)  copy_count = count;
        TEST_(
            copy_to_user(buf,
                (char *) block->block + read_offset, copy_count) != 0,
            rc = -EFAULT, failed, "Unable to copy data to user");

        copied += copy_count;
        count -= copy_count;
        buf += copy_count;
        open->read_offset += copy_count;

        /* If the current block has been consumed then move on to the next
         * block. */
        if (open->read_offset >= FA_BLOCK_SIZE) {
            block->state = fa_block_free;
            open->read_offset = 0;
            open->read_block_index =
                step_index(open->read_block_index, 1);
        }
    }

    return copied;
    
failed:
    return rc;
}


static struct file_operations fa_sniffer_fops = {
    .owner   = THIS_MODULE,
    .open    = fa_sniffer_open,
    .release = fa_sniffer_release,
    .read    = fa_sniffer_read
};



/*****************************************************************************/
/*                       Device and Module Initialisation                    */
/*****************************************************************************/


static int fa_sniffer_enable(struct pci_dev *pdev)
{
    int rc = pci_enable_device(pdev);
    TEST_RC(rc, no_device, "Unable to enable device");
    rc = pci_request_regions(pdev, "fa_sniffer");
    TEST_RC(rc, no_regions, "Unable to reserve resources");

    rc = pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
    TEST_RC(rc, no_msi, "Unable to set 32-bit DMA");

    pci_set_master(pdev);

    rc = pci_enable_msi(pdev);       // Enable message based interrupts
    TEST_RC(rc, no_msi, "Unable to enable MSI");

    return 0;

no_msi:
    pci_release_regions(pdev);
no_regions:
    pci_disable_device(pdev);
no_device:
    return rc;
}

static void fa_sniffer_disable(struct pci_dev *pdev)
{
    pci_disable_msi(pdev);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
}


static int __devinit fa_sniffer_probe(
    struct pci_dev *pdev, const struct pci_device_id *id)
{
    int rc = 0;
    struct fa_sniffer *fa_sniffer = kmalloc(sizeof(*fa_sniffer), GFP_KERNEL);
    TEST_PTR(rc, fa_sniffer, no_memory, "Unable to allocate memory");

    fa_sniffer->open_flag = 0;
    fa_sniffer->pdev = pdev;
    pci_set_drvdata(pdev, fa_sniffer);

    rc = fa_sniffer_enable(pdev);
    if (rc < 0)     goto no_sniffer;
    rc = initialise_fa_hw(pdev, &fa_sniffer->hw);
    if (rc < 0)     goto no_hw;

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
    release_fa_hw(pdev, fa_sniffer->hw);
no_hw:
    fa_sniffer_disable(pdev);
no_sniffer:
    kfree(fa_sniffer);
no_memory:
    return rc;
}


static void __devexit fa_sniffer_remove(struct pci_dev *pdev)
{
    struct fa_sniffer *fa_sniffer = pci_get_drvdata(pdev);
    dev_t devt = fa_sniffer->cdev.dev;

    device_destroy(fa_sniffer->class, devt);
    class_destroy(fa_sniffer->class);
    cdev_del(&fa_sniffer->cdev);
    unregister_chrdev_region(devt, 1);
    release_fa_hw(pdev, fa_sniffer->hw);
    fa_sniffer_disable(pdev);
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
