/* Kernel driver for Communication Controller FA sniffer.
 *
 * Copyright (C) 2010  Michael Abbott, Diamond Light Source Ltd.
 *
 * The FA sniffer card captures a stream of Fast Acquisition frames from the
 * FA network and writes them to memory using PCIe DMA transfer.
 * A new frame arrives every 100 microseconds, and the sniffer has no control
 * over this data stream.
 *
 * The driver here endeavours to capture every frame arriving after a file
 * open call on the FA sniffer device. */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/sched.h>

#include "fa_sniffer.h"

#define _S(x)   #x
#define S(x)    _S(x)

MODULE_AUTHOR("Michael Abbott, Diamond Light Source Ltd.");
MODULE_DESCRIPTION("Driver for PCIe Fast Acquisition Sniffer");
MODULE_LICENSE("GPL");
MODULE_VERSION(S(VERSION));

/* Module parameters. */
#define MIN_FA_BUFFER_COUNT     3
static int fa_block_shift = 19;     // Size of FA block buffer as power of 2
static int fa_buffer_count = 5;     // Number of FA block buffers

module_param(fa_block_shift,  int, S_IRUGO);
module_param(fa_buffer_count, int, S_IRUGO);



/* If test is true then do on_error, print message and goto target. */
#define TEST_(test, on_error, target, message) \
    do if (test) { \
        on_error; \
        printk(KERN_ERR "fa_sniffer: " message "\n"); \
        goto target; \
    } while (0)

/* If rc is an error code (< 0) then print message and goto target. */
#define TEST_RC(rc, target, message) \
    TEST_((rc) < 0, , target, message)

/* If ptr indicates an error then assign the associated error code to rc, print
 * message and goto target.  If ptr is in fact NULL we return -ENOMEM. */
#define TEST_PTR(rc, ptr, target, message) \
    TEST_(IS_ERR(ptr), rc = PTR_ERR(ptr)?:-ENOMEM, target, message)





/*****************************************************************************/
/*                      FA Sniffer Hardware Definitions                      */
/*****************************************************************************/


/* Xilinx vendor id: currently just a Xilinx development card. */
#define XILINX_VID      0x10EE
#define XILINX_DID      0x0007


/* Register map for FA sniffer PCIe interface. */
struct x5pcie_dma_registers {
    u32 dcsr;         /* 0x00 device control status register*/
    u32 ddmacr;       /* 0x04 device DMA control status register */
    u32 wdmatlpa;     /* 0x08 write DMA TLP address */
    u32 wdmatlps;     /* 0x0C write DMA TLP Size */
    u32 wdmatlpc;     /* 0x10 write DMA TLP count */
    u32 wdmatlpp;     /* 0x14 write DMA pattern */
    u32 rdmatlpp;     /* 0x18 read DMA expected pattern */
    u32 rdmatlpa;     /* 0x1C read DMA TLP address*/
    u32 rdmatlps;     /* 0x20 read DMA TLP size*/
    u32 rdmatlpc;     /* 0x24 read DMA TLP count*/
    u32 wdmaperf;     /* 0x28 write DMA performace*/
    u32 rdmaperf;     /* 0x2C read DMA performace*/
    u32 rdmastat;     /* 0x30 read DMA status*/
    u32 nrdcomp;      /* 0x34 Number of Read Completion*/
    u32 rcompdsizw;   /* 0x38 Read Completion Data Size */
    u32 dlwstat;      /* 0x3C Device Link Width Status*/
    u32 dltrsstat;    /* 0x40 Device Link Transaction Size Status */
    u32 dmisccont;    /* 0x44 Device Miscellaneous Control */
    u32 ccfaiirqclr;  /* 0x48 CC FAI interrupt clear register */
    u32 dummy[13];    /* 0x4C-0x7F Reserved Address Space */
    u32 ccfaicfgval;  /* Ox80 CC FAI configuration register */
    u32 wdmastatus;   /* 0x84 WDMA status register */
    u32 linkstatus;   /* 0x88 Link status register */
    u32 frameerrcnt;  /* 0x8C Frame error count */
    u32 softerrcnt;   /* 0x90 Soft error count */
    u32 harderrcnt;   /* 0x94 Hard error count */
};

struct fa_sniffer_hw {
    struct x5pcie_dma_registers *regs;
    int tlp_size;   // Max length of single PCI DMA transfer (in bytes)
};

#define BAR0_LEN    1024


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
    else
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
    (*hw)->tlp_size = DMAGetMaxPacketSize(regs);

    int ver = readl(&regs->dcsr);
    printk(KERN_INFO "FA sniffer firmware v%d.%02x.%d\n",
        (ver >> 12) & 0xf, (ver >> 4) & 0xff, ver & 0xf);

    /* Now restart the communication controller: needed at present to work
     * around a controller defect. */
    writel(0, &regs->ccfaicfgval);
    readl(&regs->dcsr);             // Force sequencing of writes!
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


static void set_dma_buffer(struct fa_sniffer_hw *hw, dma_addr_t buffer)
{
    /* Get Maximum TLP size and compute how many TLPs are required for one
     * frame of 2048 bytes */
    u32 bTrafficClass = 0;  // Default Memory Write TLP Traffic Class
#if defined(CONFIG_X86_64) || defined(CONFIG_HIGHMEM64G)
    u32 top_address = (u32) (buffer >> 32);
#else
    u32 top_address = 0;
#endif
    bool fEnable64bit = top_address != 0;

    /* Format of wdmatlps (in bits):
     *  31:24   Bits 39:32 of the DMA address
     *  23:20   (unused)
     *  19      Emable 64 bit addresses
     *  18:16   (unused)
     *  15:13   Traffic class (0 => default memory write)
     *  12:0    Number of 32 bit transfers in one TLP. */
    u32 top_word =
        (top_address << 24) |
        ((fEnable64bit & 1) << 19) |
        ((bTrafficClass & 0x7) << 16) |
        ((hw->tlp_size / 4) & 0x1FFF);
    u32 bottom_word = (u32) buffer;

    /* Ensure bottom word written befor top word, and serialise writes to help
     * with initialisation. */
    writel(bottom_word, &hw->regs->wdmatlpa);
    readl(&hw->regs->dcsr);
    writel(top_word,    &hw->regs->wdmatlps);
}


/* Prepares FA Sniffer card to perform DMA.  frame_count is the number of CC
 * frames that will be captured into each DMA buffer. */
static void prepare_dma(struct fa_sniffer_hw *hw, int frame_count)
{
    BUILD_BUG_ON(FA_FRAME_SIZE != 2048);    // Frame size determined by hardware

    // Memory Write TLP Count (for one frame), in bytes
    writel(FA_FRAME_SIZE / hw->tlp_size, &hw->regs->wdmatlpc);
    // Buffer length in terms of number of 2K frames
    writel(frame_count, &hw->regs->wdmatlpp);

    // Assert Initiator Reset
    writel(1, &hw->regs->dcsr);
    readl(&hw->regs->dcsr);
    writel(0, &hw->regs->dcsr);
}


/* Enable FA acquisition DMA. */

static void start_fa_hw(struct fa_sniffer_hw *hw)
{
    /* Before starting perform a register readback to ensure that all
     * preceding PCI writes to this device have completed: rather important,
     * actually! */
    readl(&hw->regs->dcsr);

    /* Format of ddmacr (in bits):
     *  6       Don't snoop caches during DMA
     *  5       Relaxed ordering on DMA write
     *  1       Stop DMA
     *  0       Write DMA start. */
//    u32 control = (1 << 6) | (1 << 5) | (1 << 0);
    /* Unfortunately it would seem that, at least on 2.6.18, explicit DMA
     * cache synchronisation just plain doesn't work.  So leave DMA cache
     * snooping on, but at least we can allow relaxed transfer ordering. */
    u32 control = (1 << 5) | (1 << 0);
    writel(control, &hw->regs->ddmacr);
    /* Ensure further writes now come after start. */
    readl(&hw->regs->dcsr);
}

/* Stop DMA transfers as soon as possible, at the very least after the
 * current frame.  There will be one further interrupt. */
static void stop_fa_hw(struct fa_sniffer_hw *hw)
{
    writel(2, &hw->regs->ddmacr);
}

/* Read status associated with latest interrupt.  Returns current frame count
 * in bottom byte, returns DMA transfer status in bits 3:0 thus:
 *  ...1    Buffer finished, new DMA in progress (normal condition)
 *  000.    DMA still in progress
 *  xxx.    DMA halted, reason code one of:
 *      1    No valid DMA address
 *      2    Explicit user stop request
 *      4    Communication controller timed out
 *
 * The device guarantees that the last interrupt generated in response to a
 * sequence of DMA transfers will have a non zero STOPPED code and that this
 * interrupt will arrive in a timely way.  Thus we can safely synchronise on
 * the stop code for cleaning up resources used by hardware. */
#define FA_STATUS_DATA_OK       0x1     // DMA still in progress
#define FA_STATUS_STOPPED       0xE     // If non zero, DMA halted
static int fa_hw_status(struct fa_sniffer_hw *hw)
{
    return readl(&hw->regs->wdmastatus);
}



/*****************************************************************************/
/*                       Character Device Interface                          */
/*****************************************************************************/

/* The character device interface provides a very simple streaming API: open
 * /dev/fa_sniffer and read blocks continuously to access the data stream.
 * If reads are not fast enough then overrun is detected and read() will
 * eventually fail (with EIO).
 *
 * A circular buffer of DMA buffers is managed by the driver.  At any instant
 * two of the buffers are assigned to the hardware (one actively being
 * transferred into, one configured for the next DMA transfer).  Each
 * transfer generates an interrupt: the first buffer is then handed over to
 * the reader, and a fresh DMA buffer is configured for transfer.
 *
 * Buffers transition through the following sequence of states:
 *
 *  +-> fa_block_free       Block is currently unassigned
 *  |       |
 *  |       | ISR assigns block to hardware
 *  |       v
 *  |   fa_block_dma        Block is assigned to hardware for DMA
 *  |       |
 *  |       | ISR marks block as complete
 *  |       v
 *  |   fa_block_data       Block contains valid data to be read
 *  |       |
 *  |       | read() completes, marks block as free
 *  +-------+
 */

/* We specify the size of a single FA block as a power of 2 (because we're
 * going to allocate the block with __get_free_page(). */
#define FA_BLOCK_SIZE       (1 << fa_block_shift)
#define FA_BLOCK_FRAMES     (FA_BLOCK_SIZE / FA_FRAME_SIZE)


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
    unsigned long open_flag;    // Interlock: only one open at a time!
    int last_interrupt;         // Status word from last interrupt

    /* Circular buffer for interface from DMA to userspace read. */
    struct fa_block {
        void *block;
        dma_addr_t dma;
        enum fa_block_state state;
    } buffers[];
};

struct fa_sniffer_open {
    struct fa_sniffer *fa_sniffer;  // Associated device data
    wait_queue_head_t wait_queue;   // Communicates from ISR to read() method
    /* Some notes on this mutex.  We only need to serialise access to the open
     * as we only allow one open at a time to exist, this is enforced via the
     * fa_sniffer:open_flag.  We still need to serialise access as the caller
     * may be multi-threaded, and we need to protect the open() and ioctl()
     * methods. */
    struct mutex mutex;         // Serialises access to this open

    /* Device status.  There are two DMA buffers allocated to the device, one
     * currently being read, the next queued to be read.  We will be
     * interrupted when the current buffer is switched. */
    struct completion isr_done; // Completion of all interrupts
    int isr_block_index;        // Block currently being read into by DMA
    bool buffer_overrun;        // Set if buffer overrun has occurred

    /* Reader status. */
    bool stopped;               // Set by ISR, read by reader
    int read_block_index;       // Index of next block in buffers[] to read
    int read_offset;            // Offset into current block to read.
};



static inline int step_index(int ix, int step)
{
    ix += step;
    if (ix >= fa_buffer_count)
        ix -= fa_buffer_count;
    return ix;
}


static irqreturn_t fa_sniffer_isr(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,19)
    int irq, void *dev_id)
#else
    int irq, void *dev_id, struct pt_regs *pt_regs)
#endif
{
    struct fa_sniffer_open *open = dev_id;
    struct fa_sniffer_hw *hw = open->fa_sniffer->hw;
    struct pci_dev *pdev = open->fa_sniffer->pdev;
    int filled_ix = open->isr_block_index;
    int status = fa_hw_status(hw);

    open->isr_block_index = step_index(filled_ix, 1);
    open->fa_sniffer->last_interrupt = status;
    if (status & FA_STATUS_DATA_OK)
    {
        /* Normal DMA complete interrupt, data in hand is ready: set up the
         * next transfer and let the read know that there's data to read. */
        struct fa_block *filled_block = &open->fa_sniffer->buffers[filled_ix];
        pci_dma_sync_single_for_cpu(pdev,
            filled_block->dma, FA_BLOCK_SIZE, DMA_FROM_DEVICE);
        smp_wmb();  // Guards DMA transfer for block we've just read
        filled_block->state = fa_block_data;
    }

    if ((status & FA_STATUS_STOPPED) == 0)
    {
        /* DMA transfer still in progress.  Set up a new DMA buffer. */
        int fresh_ix = step_index(filled_ix, 2);
        struct fa_block *fresh_block = &open->fa_sniffer->buffers[fresh_ix];

        if (fresh_block->state == fa_block_free)
        {
            smp_rmb();  // Guards copy_to_user for free block.
            /* Alas on our target system (2.6.18) this function seems to do
             * nothing whatsoever.  Hopefully we'll have a working
             * implementation one day... */
            pci_dma_sync_single_for_device(pdev,
                fresh_block->dma, FA_BLOCK_SIZE, DMA_FROM_DEVICE);
            set_dma_buffer(hw, fresh_block->dma);
            fresh_block->state = fa_block_dma;
        }
        else
        {
            /* Whoops: the next buffer isn't free.  Never mind.  The hardware
             * will stop as soon as the current block is full and we'll get a
             * STOPPED interrupt.  Let the reader consume the current block
             * first. */
            open->buffer_overrun = true;
            printk(KERN_DEBUG
                "fa_sniffer: Data buffer overrun in IRQ (%08x)\n", status);
        }
    }
    else
    {
        /* This is the last interrupt.  Let the reader know that there's
         * nothing more coming, and let fa_sniffer_release() know that DMA is
         * over and clean up can complete. */
        open->stopped = true;
        complete(&open->isr_done);
    }

    /* Wake up any pending reads. */
    wake_up_interruptible(&open->wait_queue);
    return IRQ_HANDLED;
}


/* Used to start or restart the sniffer.  The hardware must not be running and
 * there must be no pending completion when this is called. */
static void start_sniffer(struct fa_sniffer_open *open)
{
    /* Reset all the dynamic state. */
    INIT_COMPLETION(open->isr_done);
    open->isr_block_index = 0;
    open->buffer_overrun = false;
    open->stopped = false;
    open->read_block_index = 0;
    open->read_offset = 0;

    /* Reset the circular buffer into a standard state.  The first two buffers
     * will be handed straight to the hardware, mark the rest as free. */
    struct fa_sniffer *fa_sniffer = open->fa_sniffer;
    fa_sniffer->buffers[0].state = fa_block_dma;
    fa_sniffer->buffers[1].state = fa_block_dma;
    int i;
    for (i = 2; i < fa_buffer_count; i ++)
        fa_sniffer->buffers[i].state = fa_block_free;

    /* Prepare the initial hardware DMA buffers and go! */
    prepare_dma(fa_sniffer->hw, FA_BLOCK_FRAMES);
    set_dma_buffer(fa_sniffer->hw, fa_sniffer->buffers[0].dma);
    start_fa_hw(fa_sniffer->hw);
    set_dma_buffer(fa_sniffer->hw, fa_sniffer->buffers[1].dma);
}


/* Ensures the sniffer is stopped. */
static void stop_sniffer(struct fa_sniffer_open *open)
{
    struct fa_sniffer *fa_sniffer = open->fa_sniffer;
    stop_fa_hw(fa_sniffer->hw);
    /* This wait must not be interruptible, as associated pages cannot be
     * safely released until the last ISR has been received. */
    // Strictly speaking we ought to use a timed wait anyhow for resistance
    // against hardware errors.  On the other hand that's a losing battle...
    wait_for_completion(&open->isr_done);
}


static int fa_sniffer_open(struct inode *inode, struct file *file)
{
    struct fa_sniffer *fa_sniffer =
        container_of(inode->i_cdev, struct fa_sniffer, cdev);
    struct pci_dev *pdev = fa_sniffer->pdev;

    if (test_and_set_bit(0, &fa_sniffer->open_flag))
        /* No good, the device is already open.  This approach (only one open on
         * the device at a time) is practical enough and means we only need to
         * protect the open itself against concurrent access. */
        return -EBUSY;

    int rc = 0;
    struct fa_sniffer_open *open = kmalloc(sizeof(*open), GFP_KERNEL);
    TEST_PTR(rc, open, no_open, "Unable to allocate open structure");
    file->private_data = open;

    open->fa_sniffer = fa_sniffer;
    init_waitqueue_head(&open->wait_queue);
    mutex_init(&open->mutex);
    init_completion(&open->isr_done);
    /* The remaining state is initialised by start_sniffer(). */

    /* Set up the interrupt routine and start things off. */
    rc = request_irq(
        pdev->irq, fa_sniffer_isr, IRQF_SHARED, "fa_sniffer", open);
    TEST_RC(rc, no_irq, "Unable to request irq");

    /* Ready to go. */
    start_sniffer(open);

    return 0;

no_irq:
    mutex_destroy(&open->mutex);
    kfree(open);
no_open:
    return rc;
}


static int fa_sniffer_release(struct inode *inode, struct file *file)
{
    struct fa_sniffer_open *open = file->private_data;
    struct fa_sniffer *fa_sniffer = open->fa_sniffer;
    struct pci_dev *pdev = fa_sniffer->pdev;

    stop_sniffer(open);

    free_irq(pdev->irq, open);

    mutex_destroy(&open->mutex);
    kfree(open);

    /* Do this last to let somebody else use this device. */
    test_and_clear_bit(0, &fa_sniffer->open_flag);
    return 0;
}


static ssize_t fa_sniffer_read(
    struct file *file, char *buf, size_t count, loff_t *f_pos)
{
    struct fa_sniffer_open *open = file->private_data;

    ssize_t copied = mutex_lock_interruptible(&open->mutex);
    if (copied != 0)
        return copied;

    while (count > 0)
    {
        /* Wait for data to arrive in the current block.  We can be
         * interrupted by a process signal, or can detect end of input, due
         * to either buffer overrun or communication controller timeout. */
        struct fa_block *block =
            &open->fa_sniffer->buffers[open->read_block_index];
        int rc = wait_event_interruptible(open->wait_queue,
            block->state == fa_block_data  ||  open->stopped);
        if (rc < 0)
        {
            /* If the wait was interrupted and we just return -EINTR we will
             * effectively lose all the data that's already been copied.  The
             * best thing is to only return -EINTR if nothing's been copied so
             * far. */
            if (copied == 0)
                copied = rc;    // Interrupted, return interrupt code
            break;
        }
        if (block->state != fa_block_data)
            break;              // Device stopped, return what's been copied

        /* Copy as much data as needed and available out of the current block,
         * and advance all our buffers and pointers. */
        smp_rmb();  // Guards DMA transfer for new data block
        size_t read_offset = open->read_offset;
        size_t copy_count = FA_BLOCK_SIZE - read_offset;
        if (copy_count > count)  copy_count = count;
        copy_count -= copy_to_user(
            buf, (char *) block->block + read_offset, copy_count);
        if (copy_count == 0)
        {
            /* As for the interrupted case, if we want to avoid losing data we
             * can't report failure unless we've copied nothing. */
            if (copied == 0)
                copied = -EFAULT;   // copy_to_user failed
            break;
        }

        copied += copy_count;
        count -= copy_count;
        buf += copy_count;
        open->read_offset += copy_count;

        /* If the current block has been consumed then move on to the next
         * block, marking this block as free for the interrupt routine. */
        if (open->read_offset >= FA_BLOCK_SIZE)
        {
            open->read_offset = 0;
            open->read_block_index = step_index(open->read_block_index, 1);
            smp_wmb();  // Guards copy_to_user for block we're freeing
            block->state = fa_block_free;
        }
    }

    mutex_unlock(&open->mutex);
    return copied;
}


/* Force a restart of the sniffer.  If it has stopped naturally it will resume,
 * otherwise a hardware restart is forced. */
static long restart_sniffer(struct fa_sniffer_open *open)
{
    stop_sniffer(open);
    start_sniffer(open);
    return 0;
}


/* Force a halt to the sniffer.  Debug use only. */
static long halt_sniffer(struct fa_sniffer_open *open)
{
    //!!!!
    /* Need to check with Isa whether it's safe to repeatedly call this. */
    if (!open->stopped)
        stop_fa_hw(open->fa_sniffer->hw);
    return 0;
}


/* Used to compute an estimate of the number of buffered frames.  This is not to
 * be relied on as we have to read isr_block_index, which belongs to the
 * interrupt routine exclusively. */
static int blocks_available(struct fa_sniffer_open *open)
{
    int blocks =
        (open->read_block_index - open->isr_block_index) % fa_buffer_count;
    /* If we've suffered a buffer overrun and blocks is zero it's entirely
     * possible that actually *all* the blocks are available instead. */
    enum fa_block_state read_state =
        open->fa_sniffer->buffers[open->read_block_index].state;
    if (blocks == 0  &&  open->buffer_overrun  &&  read_state == fa_block_data)
        blocks = fa_buffer_count;
    return blocks;
}

/* Interrogate detailed sniffer status. */
static long read_fa_status(struct fa_sniffer_open *open, void *result)
{
    struct fa_status status;
    struct fa_sniffer *fa_sniffer = open->fa_sniffer;
    struct x5pcie_dma_registers *regs = fa_sniffer->hw->regs;

    long linkstatus = readl(&regs->linkstatus);
    status.status = linkstatus & 3;
    status.partner = (linkstatus >> 8) & 0x3FF;
    status.last_interrupt = fa_sniffer->last_interrupt;
    status.frame_errors = readl(&regs->frameerrcnt);
    status.soft_errors = readl(&regs->softerrcnt);
    status.hard_errors = readl(&regs->harderrcnt);
    status.running = !open->stopped;
    status.overrun = open->buffer_overrun;
    status.available =
        FA_BLOCK_SIZE * blocks_available(open) - open->read_offset;

    if (copy_to_user(result, &status, sizeof(struct fa_status)) == 0)
        return 0;
    else
        return -EFAULT;
}


static long fa_sniffer_ioctl(
    struct file *file, unsigned int cmd, unsigned long arg)
{
    struct fa_sniffer_open *open = file->private_data;
    long result = mutex_lock_interruptible(&open->mutex);
    if (result != 0)
        return result;

    switch (cmd)
    {
        case FASNIF_IOCTL_GET_VERSION:
            result = FASNIF_IOCTL_VERSION;
            break;
        case FASNIF_IOCTL_RESTART:
            result = restart_sniffer(open);
            break;
        case FASNIF_IOCTL_HALT:
            result = halt_sniffer(open);
            break;
        case FASNIF_IOCTL_GET_STATUS:
            result = read_fa_status(open, (void *) arg);
            break;
        default:
            result = -ENOTTY;
            break;
    }

    mutex_unlock(&open->mutex);
    return result;
}


static struct file_operations fa_sniffer_fops = {
    .owner   = THIS_MODULE,
    .open    = fa_sniffer_open,
    .release = fa_sniffer_release,
    .read    = fa_sniffer_read,
    .unlocked_ioctl = fa_sniffer_ioctl,
};


/*****************************************************************************/
/*                          Circular Buffer Management                       */
/*****************************************************************************/


static int allocate_fa_buffers(
    struct pci_dev *pdev, struct fa_sniffer *fa_sniffer)
{
    int rc;

    /* Prepare the circular buffer. */
    int blk;
    for (blk = 0; blk < fa_buffer_count; blk++)
    {
        struct fa_block *block = &fa_sniffer->buffers[blk];
        /* We ask for "cache cold" pages just to optimise things, as these pages
         * won't be read without DMA first.  We allocate free pages (rather than
         * using kmalloc) as this appears to be a better match to our
         * application.
         *    Seems that this one returns 0 rather than an error pointer on
         * failure. */
        block->block = (void *) __get_free_pages(
            GFP_KERNEL | __GFP_COLD, fa_block_shift - PAGE_SHIFT);
        TEST_((unsigned long) block->block == 0, rc = -ENOMEM,
            no_block, "Unable to allocate buffer");

        /* Map each block for DMA. */
        block->dma = pci_map_single(
            pdev, block->block, FA_BLOCK_SIZE, DMA_FROM_DEVICE);
        TEST_(pci_dma_mapping_error(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
                    pdev,
#endif
                    block->dma),
            rc = -EIO, no_dma_map, "Unable to map DMA block");
        block->state = fa_block_free;
    }

    return 0;


    /* Release circular buffer resources on error.  Rather tricky interaction
     * with allocation loop above so that we release precisely those resources
     * we allocated, in reverse order. */
    do {
        blk -= 1;
        pci_unmap_single(pdev, fa_sniffer->buffers[blk].dma,
            FA_BLOCK_SIZE, DMA_FROM_DEVICE);
no_dma_map:
        free_pages((unsigned long) fa_sniffer->buffers[blk].block,
            fa_block_shift - PAGE_SHIFT);
no_block:
        ;
    } while (blk > 0);
    return rc;
}


static void release_fa_buffers(
    struct pci_dev *pdev, struct fa_sniffer *fa_sniffer)
{
    int blk;
    for (blk = 0; blk < fa_buffer_count; blk++)
    {
        pci_unmap_single(pdev, fa_sniffer->buffers[blk].dma,
            FA_BLOCK_SIZE, DMA_FROM_DEVICE);
        free_pages((unsigned long) fa_sniffer->buffers[blk].block,
            fa_block_shift - PAGE_SHIFT);
    }
}


/*****************************************************************************/
/*                       Device and Module Initialisation                    */
/*****************************************************************************/

#define FA_SNIFFER_MAX_MINORS   32

static struct class *fa_sniffer_class;
static unsigned int fa_sniffer_major;
static unsigned long fa_sniffer_minors;  /* Bit mask of allocated minors */


static int get_free_minor(unsigned int *minor)
{
    int bit;
    for (bit = 0; bit < FA_SNIFFER_MAX_MINORS; bit ++)
    {
        if (test_and_set_bit(bit, &fa_sniffer_minors) == 0)
        {
            *minor = bit;
            return 0;
        }
    }
    return -EIO;
}

static void release_minor(unsigned int minor)
{
    test_and_clear_bit(minor, &fa_sniffer_minors);
}


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
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,29)
    pci_clear_master(pdev);
#endif
    pci_release_regions(pdev);
no_regions:
    pci_disable_device(pdev);
no_device:
    return rc;
}

static void fa_sniffer_disable(struct pci_dev *pdev)
{
    pci_disable_msi(pdev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,29)
    pci_clear_master(pdev);
#endif
    pci_release_regions(pdev);
    pci_disable_device(pdev);
}


static inline struct fa_sniffer * get_fa_sniffer(struct device *dev)
{
    return pci_get_drvdata(container_of(dev, struct pci_dev, dev));
}

#define READ_REG(dev, reg)      (readl(&get_fa_sniffer(dev)->hw->regs->reg))

#define DECLARE_ATTR(name, expr) \
    static ssize_t name##_show( \
        struct device *dev, struct device_attribute *attr, char *buf) \
    { \
        return sprintf(buf, "%d\n", (expr)); \
    }

DECLARE_ATTR(last_interrupt, get_fa_sniffer(dev)->last_interrupt)
DECLARE_ATTR(link_status,    READ_REG(dev, linkstatus) & 3)
DECLARE_ATTR(link_partner,   (READ_REG(dev, linkstatus) >> 8) & 0x3FF)
DECLARE_ATTR(frame_errors,   READ_REG(dev, frameerrcnt))
DECLARE_ATTR(soft_errors,    READ_REG(dev, softerrcnt))
DECLARE_ATTR(hard_errors,    READ_REG(dev, harderrcnt))

static ssize_t firmware_show(
    struct device *dev, struct device_attribute *attr, char *buf)
{
    int ver = READ_REG(dev, dcsr);
    return sprintf(buf, "v%d.%02x.%d\n",
        (ver >> 12) & 0xf, (ver >> 4) & 0xff, ver & 0xf);
}

static struct device_attribute attributes[] = {
    __ATTR_RO(firmware),
    __ATTR_RO(last_interrupt),
    __ATTR_RO(link_status),
    __ATTR_RO(link_partner),
    __ATTR_RO(frame_errors),
    __ATTR_RO(soft_errors),
    __ATTR_RO(hard_errors),
};


static int __devinit fa_sniffer_probe(
    struct pci_dev *pdev, const struct pci_device_id *id)
{
    unsigned int minor;
    int rc = get_free_minor(&minor);
    TEST_RC(rc, no_minor, "Unable to allocate minor device number");

    rc = fa_sniffer_enable(pdev);
    if (rc < 0)     goto no_sniffer;

    struct fa_sniffer *fa_sniffer = kmalloc(
        sizeof(*fa_sniffer) + fa_buffer_count * sizeof(struct fa_block),
        GFP_KERNEL);
    TEST_PTR(rc, fa_sniffer, no_memory, "Unable to allocate memory");

    fa_sniffer->open_flag = 0;
    fa_sniffer->pdev = pdev;
    fa_sniffer->last_interrupt = 0;
    pci_set_drvdata(pdev, fa_sniffer);

    rc = initialise_fa_hw(pdev, &fa_sniffer->hw);
    if (rc < 0)     goto no_hw;

    rc = allocate_fa_buffers(pdev, fa_sniffer);
    if (rc < 0)     goto no_buffers;

    cdev_init(&fa_sniffer->cdev, &fa_sniffer_fops);
    fa_sniffer->cdev.owner = THIS_MODULE;
    rc = cdev_add(&fa_sniffer->cdev, MKDEV(fa_sniffer_major, minor), 1);
    TEST_RC(rc, no_cdev, "Unable to register device");

    struct device *dev = device_create(
        fa_sniffer_class, &pdev->dev,
        MKDEV(fa_sniffer_major, minor),
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
        NULL,
#endif
        "fa_sniffer%d", minor);
    TEST_PTR(rc, dev, no_device, "Unable to create device");

    int i;
    for (i = 0; i < (int) ARRAY_SIZE(attributes); i ++)
    {
        rc = device_create_file(&pdev->dev, &attributes[i]);
        TEST_RC(rc, no_attr, "Unable to create attr");
    }

    printk(KERN_INFO "fa_sniffer%d installed\n", minor);
    return 0;


    do {
        device_remove_file(&pdev->dev, &attributes[i]);
no_attr:
        i --;
    } while (i >= 0);

    device_destroy(fa_sniffer_class, MKDEV(fa_sniffer_major, minor));
no_device:
    cdev_del(&fa_sniffer->cdev);
no_cdev:
    release_fa_buffers(pdev, fa_sniffer);
no_buffers:
    release_fa_hw(pdev, fa_sniffer->hw);
no_hw:
    kfree(fa_sniffer);
no_memory:
    fa_sniffer_disable(pdev);
no_sniffer:
    release_minor(minor);
no_minor:
    return rc;
}


static void __devexit fa_sniffer_remove(struct pci_dev *pdev)
{
    struct fa_sniffer *fa_sniffer = pci_get_drvdata(pdev);
    unsigned int minor = MINOR(fa_sniffer->cdev.dev);

    unsigned i;
    for (i = 0; i < ARRAY_SIZE(attributes); i ++)
        device_remove_file(&pdev->dev, &attributes[i]);
    device_destroy(fa_sniffer_class, fa_sniffer->cdev.dev);
    cdev_del(&fa_sniffer->cdev);
    release_fa_buffers(pdev, fa_sniffer);
    release_fa_hw(pdev, fa_sniffer->hw);
    kfree(fa_sniffer);
    fa_sniffer_disable(pdev);
    release_minor(minor);

    printk(KERN_INFO "fa_sniffer%d removed\n", minor);
}


static const struct pci_device_id fa_sniffer_ids[] = {
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
    int rc = 0;
    /* First validate the module parameters. */
    TEST_(fa_block_shift < PAGE_SHIFT, rc = -EINVAL, bad_param,
        "fa_block_shift too small");
    TEST_(fa_buffer_count < MIN_FA_BUFFER_COUNT, rc = -EINVAL, bad_param,
        "fa_buffer_count too small");

    fa_sniffer_class = class_create(THIS_MODULE, "fa_sniffer");
    TEST_PTR(rc, fa_sniffer_class, no_class, "Unable to create class");

    dev_t dev;
    rc = alloc_chrdev_region(&dev, 0, FA_SNIFFER_MAX_MINORS, "fa_sniffer");
    TEST_RC(rc, no_chrdev, "Unable to allocate device");
    fa_sniffer_major = MAJOR(dev);
    fa_sniffer_minors = 0;

    rc = pci_register_driver(&fa_sniffer_driver);
    TEST_RC(rc, no_driver, "Unable to register driver");
    printk(KERN_INFO "Installed FA sniffer module\n");
    return rc;

no_driver:
    unregister_chrdev_region(fa_sniffer_major, FA_SNIFFER_MAX_MINORS);
no_chrdev:
    class_destroy(fa_sniffer_class);
no_class:
bad_param:
    return rc;
}

static void __exit fa_sniffer_exit(void)
{
    pci_unregister_driver(&fa_sniffer_driver);
    unregister_chrdev_region(fa_sniffer_major, FA_SNIFFER_MAX_MINORS);
    class_destroy(fa_sniffer_class);
    printk(KERN_INFO "Removed FA sniffer module\n");
}


module_init(fa_sniffer_init);
module_exit(fa_sniffer_exit);
