/* Kernel driver for Communication Controller FA sniffer.
 *
 * Copyright (C) 2010-2012  Michael Abbott, Diamond Light Source Ltd.
 *
 * The FA sniffer driver is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * The FA sniffer driver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * Contact:
 *      Dr. Michael Abbott,
 *      Diamond Light Source Ltd,
 *      Diamond House,
 *      Chilton,
 *      Didcot,
 *      Oxfordshire,
 *      OX11 0DE
 *      michael.abbott@diamond.ac.uk
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
#include <linux/slab.h>
#include <linux/delay.h>

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
static int fa_entry_count = 256;    // Default transfer size

module_param(fa_block_shift,  int, S_IRUGO);
module_param(fa_buffer_count, int, S_IRUGO);
module_param(fa_entry_count, int, S_IRUGO);


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
    TEST_(IS_ERR_OR_NULL(ptr), rc = PTR_ERR(ptr)?:-ENOMEM, target, message)



/* We specify the size of a single FA block as a power of 2 (because we're
 * going to allocate the block with __get_free_page(). */
#define FA_BLOCK_SIZE       (1 << fa_block_shift)




/*****************************************************************************/
/*                      FA Sniffer Hardware Definitions                      */
/*****************************************************************************/


/* Xilinx vendor id: currently just a Xilinx development card. */
#define XILINX_VID      0x10EE
#define XILINX_DID      0x0007
/* CERN SPEC board. */
#define SPEC_VID        0x10DC
#define SPEC_DID        0x018D


/* CERN SPEC Bar 4 definitions. */
#define GN4124_BAR              0x4
#define R_CLK_CSR               0x808
#define R_INT_CFG0              0x820
#define R_GPIO_DIR_MODE         0xA04
#define R_GPIO_INT_MASK_CLR     0xA18
#define R_GPIO_INT_MASK_SET     0xA1C
#define R_GPIO_INT_STATUS       0xA20
#define R_GPIO_INT_VALUE        0xA28
#define CLK_CSR_DIVOT_MASK      0x3F0
#define INT_CFG0_GPIO           15
#define GPIO_INT_SRC            8

/* CERN SPEC Bar 0 definitions. */
#define LCLK_LOCKED             0x10000
#define CLK_READ_SELECT         0x10004
#define CLK_READ_VAL            0x10008

/* Target clock frequency. */
#define MIN_CC_CLK_TICKS        8700
#define MAX_CC_CLK_TICKS        8706


/* For the development board we only need a small BAR0, but for the SPEC board
 * we have entries at high addresses so need a large BAR. */
#define BAR0_LEN_XILINX         4096
#define BAR0_LEN_SPEC           0x20000
#define BAR4_LEN                4096



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
    struct x5pcie_dma_registers __iomem *regs;
    void __iomem *bar4;     // Only present on CERN SPEC board
    int tlp_size;           // Max length of single PCI DMA transfer (in bytes)
};


static int code2size(int bCode)
{
    bCode = bCode & 0x7;
    if (bCode > 0x05)
        return 0;
    else
        return 128 << bCode;
}

static int DMAGetMaxPacketSize(struct x5pcie_dma_registers __iomem *regs)
{
    /* Read encoded max payload sizes */
    int dltrsstat = readl(&regs->dltrsstat);
    /* Convert encoded max payload sizes into byte count */
    /* bits [2:0] : Capability maximum payload size for the device */
    int wMaxCapPayload = code2size(dltrsstat);
    /* bits [10:8] : Programmed maximum payload size for the device */
    int wMaxProgPayload = code2size(dltrsstat >> 8);

    return wMaxCapPayload < wMaxProgPayload ?
        wMaxCapPayload : wMaxProgPayload;
}

static int get_spec_clocks(void __iomem *bar0)
{
    /* Check that the clock is locked. */
    int status = readl(bar0 + LCLK_LOCKED);
    if ((status & 1) == 0)
    {
        printk(KERN_ERR "Local clock not locked\n");
        return -EIO;
    }

    /* Verify communication controller clock frequency. */
    writel(0, bar0 + CLK_READ_SELECT);
    int clock = readl(bar0 + CLK_READ_VAL);
    if (clock < MIN_CC_CLK_TICKS || MAX_CC_CLK_TICKS < clock)
    {
        printk(KERN_ERR "CC clock out of range: %d\n", clock);
        return -EIO;
    }
    return 0;
}

static int setup_spec_lclk(struct fa_sniffer_hw *hw)
{
    /* Set up GN4121 clock controller to set local clock to 100 MHz. */
    writel(0xE001F07C, hw->bar4 + R_CLK_CSR);
    /* Need to wait up to 15ms for clock to settle. */
    msleep(15);

    return get_spec_clocks(hw->regs);
}

static void setup_spec_interrupts(void __iomem *bar4)
{
    // Set interrupt line from FPGA (GPIO8) as input
    writel(1<<GPIO_INT_SRC, bar4 + R_GPIO_DIR_MODE);
    // Set interrupt mask for all GPIO except for GPIO8
    writel(~(1<<GPIO_INT_SRC), bar4 + R_GPIO_INT_MASK_SET);
    // Make sure the interrupt mask is cleared for GPIO8
    writel(1<<GPIO_INT_SRC, bar4 + R_GPIO_INT_MASK_CLR);
    // Interrupt on rising edge of GPIO8
    writel(1<<GPIO_INT_SRC, bar4 + R_GPIO_INT_VALUE);
    // GPIO as interrupt 0 source
    writel(1<<INT_CFG0_GPIO, bar4 + R_INT_CFG0);
}


static int initialise_fa_hw(
    struct pci_dev *pdev, struct fa_sniffer_hw **hw, bool is_spec_board)
{
    int rc = 0;
    *hw = kmalloc(sizeof(*hw), GFP_KERNEL);
    TEST_PTR(rc, *hw, no_hw, "Cannot allocate fa hardware");

    struct x5pcie_dma_registers *regs =
        pci_iomap(pdev, 0, is_spec_board ? BAR0_LEN_SPEC : BAR0_LEN_XILINX);
    TEST_PTR(rc, regs, no_bar, "Cannot find registers");
    int ver = readl(&regs->dcsr);
    printk(KERN_INFO "FA sniffer firmware v%d.%02x.%d (%08x)\n",
        (ver >> 12) & 0xf, (ver >> 4) & 0xff, ver & 0xf, ver);
    TEST_(ver == 0, rc = -EIO, no_fpga, "FPGA image not loaded");

    (*hw)->regs = regs;
    if (is_spec_board)
        (*hw)->tlp_size = 128;
    else
        (*hw)->tlp_size = DMAGetMaxPacketSize(regs);

    /* Only pick up bar 4 registers from SPEC board. */
    if (is_spec_board)
    {
        void __iomem *bar4 = pci_iomap(pdev, 4, BAR4_LEN);
        TEST_PTR(rc, bar4, no_bar4, "Cannot find bar 4");
        (*hw)->bar4 = bar4;

        rc = setup_spec_lclk(*hw);
        if (rc < 0) goto clock_error;

        setup_spec_interrupts(bar4);
    }
    else
        (*hw)->bar4 = NULL;

    /* Now restart the communication controller: needed at present to work
     * around a controller defect. */
    writel(0, &regs->ccfaicfgval);
    readl(&regs->dcsr);             // Force sequencing of writes!
    writel(8, &regs->ccfaicfgval);

    return rc;

clock_error:
    pci_iounmap(pdev, (*hw)->bar4);
no_bar4:
no_fpga:
    pci_iounmap(pdev, (*hw)->regs);
no_bar:
    kfree(*hw);
no_hw:
    return rc;
}


static void release_fa_hw(struct pci_dev *pdev, struct fa_sniffer_hw *hw)
{
    pci_iounmap(pdev, hw->regs);
    if (hw->bar4)
        pci_iounmap(pdev, hw->bar4);
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

    /* Ensure bottom word written before top word, and serialise writes to help
     * with initialisation. */
    writel(bottom_word, &hw->regs->wdmatlpa);
    readl(&hw->regs->dcsr);
    writel(top_word,    &hw->regs->wdmatlps);
}


/* Prepares FA Sniffer card to perform DMA.  frame_count is the number of CC
 * frames that will be captured into each DMA buffer. */
static void prepare_dma(struct fa_sniffer_hw *hw, int fa_entry_count)
{
    int fa_frame_size = fa_entry_count * FA_ENTRY_SIZE;

    // Memory Write TLP Count (for one frame), in bytes
    writel(fa_frame_size / hw->tlp_size, &hw->regs->wdmatlpc);
    // Buffer length in terms of number of frames
    writel(FA_BLOCK_SIZE / fa_frame_size, &hw->regs->wdmatlpp);

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
 * together with the DMA transfer status in bits 3:0 thus:
 *  ...1    Last data transfer was complete, transfer count is zero
 *  ...0    Last data transfer incomplete, transfer count in top three bytes
 *  000.    DMA still in progress, expect another interrupt
 *  xxx.    DMA halted, no more interrupts will occur, reason code one of:
 *      1    No valid DMA address (typically due to isr buffer overrun)
 *      2    Explicit user stop request
 *      4    Communication controller timed out (no data available on network)
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
    int fa_entry_count;         // FA entries in one DMA transfer

    /* Circular buffer for interface from DMA to userspace read. */
    struct fa_block {
        void *block;            // Block address
        dma_addr_t dma;         // Associated dma address
        enum fa_block_state state;  // free / in dma / has data
        uint64_t timestamp;     // Timestamp of completion
    } buffers[];
};

struct fa_sniffer_open {
    struct fa_sniffer *fa_sniffer;  // Associated device data
    wait_queue_head_t wait_queue;   // Communicates from ISR to read() method
    /* We only allow one read() call at a time.  This allows lock free handling
     * which simplifies waiting considerably. */
    unsigned long read_active;

    /* Device status.  There are two DMA buffers allocated to the device, one
     * currently being read, the next queued to be read.  We will be
     * interrupted when the current buffer is switched. */
    struct completion isr_done; // Completion of all interrupts
    int isr_block_index;        // Block currently being read into by DMA
    bool buffer_overrun;        // Set if buffer overrun has occurred

    /* Reader status. */
    bool running;               // Set by ISR, read by reader
    int read_block_index;       // Index of next block in buffers[] to read
    int read_offset;            // Offset into current block to read.
    uint64_t timestamp;         // Timestamp of last block read
    int residue;                // Bytes remaining in last block read
};



static inline int step_index(int ix, int step)
{
    ix += step;
    if (ix >= fa_buffer_count)
        ix -= fa_buffer_count;
    return ix;
}


/* Advances one block around the buffer, making the newly filled block available
 * for readout and setting the next block up for DMA.  If the interrupt status
 * reports that we're stopped we don't pass the next block through to the
 * device, but it's still marked for DMA to maintain the buffer invariant (two
 * blocks directly after isr_block_index are DMA). */
static void advance_fa_buffer(
    struct fa_sniffer_open *open, int status,
    struct timeval *tv, struct fa_block *fresh_block)
{
    struct fa_sniffer_hw *hw = open->fa_sniffer->hw;
    struct pci_dev *pdev = open->fa_sniffer->pdev;
    int filled_ix = open->isr_block_index;
    struct fa_block *filled_block = &open->fa_sniffer->buffers[filled_ix];

    filled_block->timestamp = (uint64_t) tv->tv_sec * 1000000 + tv->tv_usec;
    pci_dma_sync_single_for_cpu(pdev,
        filled_block->dma, FA_BLOCK_SIZE, DMA_FROM_DEVICE);
    smp_wmb();  // Guards DMA transfer for block we've just read
    filled_block->state = fa_block_data;

    smp_rmb();  // Guards copy_to_user for free block.
    if ((status & FA_STATUS_STOPPED) == 0)
    {
        /* Alas on our target system (2.6.18) this function seems to do
         * nothing whatsoever.  Hopefully we'll have a working
         * implementation one day... */
        pci_dma_sync_single_for_device(pdev,
            fresh_block->dma, FA_BLOCK_SIZE, DMA_FROM_DEVICE);
        set_dma_buffer(hw, fresh_block->dma);
    }
    fresh_block->state = fa_block_dma;
    open->isr_block_index = step_index(filled_ix, 1);
}


/* This ISR maintains the invariant that the two blocks at isr_block_index and
 * directly after are fa_block_dma and all the rest are either data or free. */
static irqreturn_t fa_sniffer_isr(int irq, void *dev_id
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
    , struct pt_regs *pt_regs
#endif
    )
{
    /* Capture the interrupt time stamp as soon as possible so that it is
     * consistent.  Costs us a memory barrier, so it goes... */
    struct timeval tv;
    do_gettimeofday(&tv);

    struct fa_sniffer_open *open = dev_id;
    struct fa_sniffer_hw *hw = open->fa_sniffer->hw;

    /* Only on SPEC board we can get unexpected interrupts, so make sure there
     * is a reason for this interrupt. */
    if (hw->bar4  &&  readl(hw->bar4 + R_GPIO_INT_STATUS) == 0)
        return IRQ_NONE;

    int status = fa_hw_status(hw);
    open->fa_sniffer->last_interrupt = status;
    if (status & FA_STATUS_DATA_OK)
    {
        struct fa_block *fresh_block =
            &open->fa_sniffer->buffers[step_index(open->isr_block_index, 2)];
        if (fresh_block->state == fa_block_free)
            advance_fa_buffer(open, status, &tv, fresh_block);
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

    if (status & FA_STATUS_STOPPED)
    {
        /* This is the last interrupt.  Let the reader know that there's nothing
         * more coming, and let stop_sniffer() know that DMA is over and clean
         * up can complete. */
        open->running = false;
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
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,13,0)
    reinit_completion(&open->isr_done);
#else
    INIT_COMPLETION(open->isr_done);
#endif
    open->buffer_overrun = false;
    open->running = true;

    /* Prepare two buffers for dma.  We use the current isr index so that this
     * can be done concurrently with read.  There is an invariant that the two
     * blocks pointed to by isr_block_index are in state fa_block_dma.  We now
     * just send these buffers to the hardware and go! */
    struct fa_sniffer *fa_sniffer = open->fa_sniffer;
    int ix0 = open->isr_block_index;
    int ix1 = step_index(ix0, 1);
    prepare_dma(fa_sniffer->hw, fa_sniffer->fa_entry_count);
    set_dma_buffer(fa_sniffer->hw, fa_sniffer->buffers[ix0].dma);
    start_fa_hw(fa_sniffer->hw);
    set_dma_buffer(fa_sniffer->hw, fa_sniffer->buffers[ix1].dma);
}


/* Ensures the sniffer is stopped. */
static void stop_sniffer(struct fa_sniffer_open *open)
{
    struct fa_sniffer *fa_sniffer = open->fa_sniffer;
    stop_fa_hw(fa_sniffer->hw);
    /* This wait must not be interruptible, as associated pages cannot be
     * safely released until the last ISR has been received.  If we've not had a
     * response within a second then I guess we're not getting one... */
    if (wait_for_completion_timeout(&open->isr_done, HZ) == 0)
    {
        /* Oh dear, we are in real trouble.  The completion interrupt never
         * happened, which means we've no idea what the hardware is up to any
         * more.  All we can do is log a panicy report. */
        printk(KERN_EMERG "The FA sniffer completion interrupt was not seen\n");
        printk(KERN_EMERG "Kernel consistency is now unpredictable\n");
        printk(KERN_EMERG "Reboot the system as a matter of urgency\n");
    }
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
    open->read_active = 0;
    init_completion(&open->isr_done);
    /* Initial state for ISR -> read() communication. */
    open->isr_block_index = 0;
    open->read_block_index = 0;
    open->read_offset = 0;
    open->timestamp = 0;
    open->residue = 0;
    int i;
    fa_sniffer->buffers[0].state = fa_block_dma;
    fa_sniffer->buffers[1].state = fa_block_dma;
    for (i = 2; i < fa_buffer_count; i ++)
        fa_sniffer->buffers[i].state = fa_block_free;
    /* The remaining state is initialised by start_sniffer(). */

    /* Set up the interrupt routine and start things off. */
    rc = request_irq(
        pdev->irq, fa_sniffer_isr, IRQF_SHARED, "fa_sniffer", open);
    TEST_RC(rc, no_irq, "Unable to request irq");

    /* Ready to go. */
    start_sniffer(open);

    return 0;

no_irq:
    kfree(open);
no_open:
    test_and_clear_bit(0, &fa_sniffer->open_flag);
    return rc;
}


static int fa_sniffer_release(struct inode *inode, struct file *file)
{
    struct fa_sniffer_open *open = file->private_data;
    struct fa_sniffer *fa_sniffer = open->fa_sniffer;
    struct pci_dev *pdev = fa_sniffer->pdev;

    stop_sniffer(open);

    free_irq(pdev->irq, open);

    kfree(open);

    /* Do this last to let somebody else use this device. */
    test_and_clear_bit(0, &fa_sniffer->open_flag);
    return 0;
}


static ssize_t fa_sniffer_read(
    struct file *file, char __user *buf, size_t count, loff_t *f_pos)
{
    struct fa_sniffer_open *open = file->private_data;

    /* Check we're the only reader at this time. */
    if (test_and_set_bit(0, &open->read_active))
        return -EBUSY;

    ssize_t copied = 0;
    while (count > 0)
    {
        /* Wait for data to arrive in the current block.  We can be
         * interrupted by a process signal, or can detect end of input, due
         * to either buffer overrun or communication controller timeout. */
        struct fa_block *block =
            &open->fa_sniffer->buffers[open->read_block_index];
        int rc = wait_event_interruptible_timeout(open->wait_queue,
            block->state == fa_block_data  ||  !open->running, HZ);
        if (rc == 0)
        {
            /* Oh crap, we timed out.  This is not good, not good at all.
             * Basically means the hardware is upset, or something else equally
             * bad is going on. */
            printk(KERN_ALERT "fa_sniffer read timed out.\n");
            copied = -EIO;      // Could be ETIME, but something is badly wrong
            break;
        }
        else if (rc < 0)
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

        /* Record timestamp and residue of current read. */
        open->timestamp = block->timestamp;
        open->residue = FA_BLOCK_SIZE - open->read_offset;

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

    test_and_clear_bit(0, &open->read_active);
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
    /* Seem to be harmless to send this repeatedly. */
    if (open->running)
        stop_fa_hw(open->fa_sniffer->hw);
    return 0;
}


#define COPY_TO_USER(result, value) \
    (copy_to_user(result, &(value), sizeof(value)) == 0 ? 0 : -EFAULT)

/* Interrogate detailed sniffer status. */
static long read_fa_status(struct fa_sniffer_open *open, void __user *result)
{
    struct fa_sniffer *fa_sniffer = open->fa_sniffer;
    struct x5pcie_dma_registers __iomem *regs = fa_sniffer->hw->regs;

    long linkstatus = readl(&regs->linkstatus);
    struct fa_status status = {
        .status = linkstatus & 3,
        .partner = (linkstatus >> 8) & 0x3FF,
        .last_interrupt = fa_sniffer->last_interrupt,
        .frame_errors = readl(&regs->frameerrcnt),
        .soft_errors = readl(&regs->softerrcnt),
        .hard_errors = readl(&regs->harderrcnt),
        .running = open->running,
        .overrun = open->buffer_overrun,
    };
    return COPY_TO_USER(result, status);
}


static long read_fa_timestamp(struct fa_sniffer_open *open, void __user *result)
{
    struct fa_timestamp timestamp = {
        .timestamp = open->timestamp,
        .residue   = open->residue
    };
    return COPY_TO_USER(result, timestamp);
}


static long set_fa_entry_count(struct fa_sniffer_open *open, void __user *value)
{
    uint32_t new_count;
    if (copy_from_user(&new_count, value, sizeof(uint32_t)) == 0)
    {
        /* New count must be a multiple of the TLP count and a power of 2. */
        int tlp_size = open->fa_sniffer->hw->tlp_size;
        if (0 < new_count  &&  new_count <= 1024  &&
            (new_count & -new_count) == new_count  &&
            (FA_ENTRY_SIZE * new_count) % tlp_size == 0)
        {
            printk(KERN_INFO "fa_sniffer: setting fa_entry_count %u\n",
                new_count);
            open->fa_sniffer->fa_entry_count = new_count;
            return 0;
        }
        else
        {
            printk(KERN_ERR "fa_sniffer: invalid fa_entry_count %u (min %d)\n",
                new_count, (int) (tlp_size / FA_ENTRY_SIZE));
            return -EINVAL;
        }
    }
    else
        return -EFAULT;
}


static long fa_sniffer_ioctl(
    struct file *file, unsigned int cmd, unsigned long arg)
{
    struct fa_sniffer_open *open = file->private_data;
    switch (cmd)
    {
        case FASNIF_IOCTL_GET_VERSION:
            return FASNIF_IOCTL_VERSION;
        case FASNIF_IOCTL_RESTART:
            return restart_sniffer(open);
        case FASNIF_IOCTL_HALT:
            return halt_sniffer(open);
        case FASNIF_IOCTL_GET_STATUS:
            return read_fa_status(open, (void __user *) arg);
        case FASNIF_IOCTL_GET_TIMESTAMP:
            return read_fa_timestamp(open, (void __user *) arg);
        case FASNIF_IOCTL_GET_ENTRY_COUNT:
            return open->fa_sniffer->fa_entry_count;
        case FASNIF_IOCTL_SET_ENTRY_COUNT:
            return set_fa_entry_count(open, (void __user *) arg);
        default:
            return -ENOTTY;
    }
}


static struct file_operations fa_sniffer_fops = {
    .owner   = THIS_MODULE,
    .open    = fa_sniffer_open,
    .release = fa_sniffer_release,
    .read    = fa_sniffer_read,
    .unlocked_ioctl = fa_sniffer_ioctl,
    .compat_ioctl = fa_sniffer_ioctl,
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
/*                              Sysfs Device Nodes                           */
/*****************************************************************************/

static inline struct fa_sniffer *get_fa_sniffer(struct device *dev)
{
    return pci_get_drvdata(container_of(dev, struct pci_dev, dev));
}

#define READ_REG(dev, reg)      (readl(&get_fa_sniffer(dev)->hw->regs->reg))

#define DECLARE_ATTR(name, expr) \
    static ssize_t name##_show( \
        struct device *dev, struct device_attribute *attr, char *buf) \
    { \
        return sprintf(buf, "%u\n", (expr)); \
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
    unsigned int ver = READ_REG(dev, dcsr);
    return sprintf(buf, "v%d.%02x.%d\n",
        (ver >> 12) & 0xf, (ver >> 4) & 0xff, ver & 0xf);
}

static ssize_t api_version_show(
    struct device *dev, struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", FASNIF_IOCTL_VERSION);
}

static ssize_t fa_entry_count_show(
    struct device *dev, struct device_attribute *attr, char *buf)
{
    struct fa_sniffer *fa_sniffer = get_fa_sniffer(dev);
    return sprintf(buf, "%d\n", fa_sniffer->fa_entry_count);
}

static struct device_attribute attributes[] = {
    __ATTR_RO(firmware),
    __ATTR_RO(last_interrupt),
    __ATTR_RO(link_status),
    __ATTR_RO(link_partner),
    __ATTR_RO(frame_errors),
    __ATTR_RO(soft_errors),
    __ATTR_RO(hard_errors),
    __ATTR_RO(api_version),
    __ATTR_RO(fa_entry_count),
};


static int fa_sysfs_create(struct pci_dev *pdev)
{
    int rc = 0, i;
    for (i = 0; i < (int) ARRAY_SIZE(attributes); i ++)
    {
        rc = device_create_file(&pdev->dev, &attributes[i]);
        TEST_RC(rc, no_attr, "Unable to create attr");
    }
    return 0;

    do {
        device_remove_file(&pdev->dev, &attributes[i]);
no_attr:
        i --;
    } while (i >= 0);
    return rc;
}


static void fa_sysfs_remove(struct pci_dev *pdev)
{
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(attributes); i ++)
        device_remove_file(&pdev->dev, &attributes[i]);
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


static int fa_sniffer_enable(struct pci_dev *pdev, bool is_spec_board)
{
    int rc = pci_enable_device(pdev);
    TEST_RC(rc, no_device, "Unable to enable device");

    rc = pci_request_regions(pdev, "fa_sniffer");
    TEST_RC(rc, no_regions, "Unable to reserve resources");

    rc = pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
    TEST_RC(rc, no_msi, "Unable to set 32-bit DMA");

    pci_set_master(pdev);

    /* For reasons beyond our understanding, if we call pci_enable_msi on the
     * SPEC board things go horribly wrong: all the interrupts generate "no IRQ
     * handler" messages. */
    if (!is_spec_board)
    {
        rc = pci_enable_msi(pdev);       // Enable message based interrupts
        TEST_RC(rc, no_msi, "Unable to enable MSI");
    }

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

static void fa_sniffer_disable(struct pci_dev *pdev, bool is_spec_board)
{
    if (!is_spec_board)
        pci_disable_msi(pdev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,29)
    pci_clear_master(pdev);
#endif
    pci_release_regions(pdev);
    pci_disable_device(pdev);
}


static int fa_sniffer_probe(
    struct pci_dev *pdev, const struct pci_device_id *id)
{
    unsigned int minor;
    int rc = get_free_minor(&minor);
    TEST_RC(rc, no_minor, "Unable to allocate minor device number");

    bool is_spec_board = id->vendor == SPEC_VID  &&  id->device == SPEC_DID;
    rc = fa_sniffer_enable(pdev, is_spec_board);
    if (rc < 0)     goto no_sniffer;

    struct fa_sniffer *fa_sniffer = kmalloc(
        sizeof(*fa_sniffer) + fa_buffer_count * sizeof(struct fa_block),
        GFP_KERNEL);
    TEST_PTR(rc, fa_sniffer, no_memory, "Unable to allocate memory");

    fa_sniffer->open_flag = 0;
    fa_sniffer->pdev = pdev;
    fa_sniffer->last_interrupt = 0;
    fa_sniffer->fa_entry_count = fa_entry_count;
    pci_set_drvdata(pdev, fa_sniffer);

    rc = initialise_fa_hw(pdev, &fa_sniffer->hw, is_spec_board);
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

    rc = fa_sysfs_create(pdev);
    if (rc < 0)     goto no_attr;

    printk(KERN_INFO "fa_sniffer%d installed\n", minor);
    return 0;

no_attr:
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
    fa_sniffer_disable(pdev, is_spec_board);
no_sniffer:
    release_minor(minor);
no_minor:
    return rc;
}


static void fa_sniffer_remove(struct pci_dev *pdev)
{
    struct fa_sniffer *fa_sniffer = pci_get_drvdata(pdev);
    unsigned int minor = MINOR(fa_sniffer->cdev.dev);
    bool is_spec_board = fa_sniffer->hw->bar4 != NULL;

    fa_sysfs_remove(pdev);
    device_destroy(fa_sniffer_class, fa_sniffer->cdev.dev);
    cdev_del(&fa_sniffer->cdev);
    release_fa_buffers(pdev, fa_sniffer);
    release_fa_hw(pdev, fa_sniffer->hw);
    kfree(fa_sniffer);
    fa_sniffer_disable(pdev, is_spec_board);
    release_minor(minor);

    printk(KERN_INFO "fa_sniffer%d removed\n", minor);
}


static const struct pci_device_id fa_sniffer_ids[] = {
    { PCI_DEVICE(XILINX_VID, XILINX_DID) },
    { PCI_DEVICE(SPEC_VID, SPEC_DID) },
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
