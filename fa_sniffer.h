/* User space definitions for the fa_sniffer device driver.
 *
 * Copyright (C) 2010  Michael Abbott, Diamond Light Source Ltd.
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
 */

struct fa_entry { int32_t x, y; };

/* Each frame consists of fa_entry_count (X,Y) position pairs stored in
 * sequence, making a total of 2048/4096/8192 bytes for a single FA frame
 * depending on the configured value of fa_entry_count (can be 256/512/1024). */
#define FA_ENTRY_SIZE   (sizeof(struct fa_entry))

#define MAX_FA_ENTRY_COUNT          1024    // 10 bit FA ID in protocol

/* Type for an entire row representing a single FA frame.  Actual size will
 * depend on configuration. */
struct fa_row { struct fa_entry row[0]; };


/* ioctl definitions. */

#define FASNIF_IOCTL_VERSION        2

/* Returns ioctl interface version number.  Just a sanity check. */
#define FASNIF_IOCTL_GET_VERSION    _IO('C', 0)
/* Restarts reading after eof on read(), subsequent calls to read() will succeed
 * if data is available. */
#define FASNIF_IOCTL_RESTART        _IO('C', 1)
/* Halts transfer if in progress.  Intended for debug use. */
#define FASNIF_IOCTL_HALT           _IO('C', 2)

/* Interrogates detailed status of FA sniffer. */
struct fa_status {
    uint32_t status;                // Hardware link status
    uint32_t partner;               // Associated link partner
    uint32_t last_interrupt;        // Status word from last interrupt
    uint32_t frame_errors;          // Hardware counts of communication errors
    uint32_t soft_errors;           //  accumulated since hardware initialised
    uint32_t hard_errors;
    uint8_t running;                // True if connection currently active
    uint8_t overrun;                // True if a buffer overrun occurred
} __attribute__((packed));
#define FASNIF_IOCTL_GET_STATUS     _IOR('R', 1, struct fa_status)

/* Retrieve timestamp associated with last read.  If reside is non zero then the
 * true timestamp of the last point must be computed by projecting backwards
 * using an estimate of sample interval not provided by this driver.  */
struct fa_timestamp {
    uint64_t timestamp;             // Block completion timestamp
    uint32_t residue;               // Residue of block not read
} __attribute__((packed));
#define FASNIF_IOCTL_GET_TIMESTAMP  _IOR('R', 2, struct fa_timestamp)

/* Interrogates the current fa_entry_count. */
#define FASNIF_IOCTL_GET_ENTRY_COUNT _IO('R', 3)
/* Sets the fa_entry_count.  Note that the device will need to be closed and
 * reopened for the change to take effect. */
#define FASNIF_IOCTL_SET_ENTRY_COUNT _IOW('C', 3, uint32_t)
