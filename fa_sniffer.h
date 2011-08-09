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

/* Each frame consists of 256 (X,Y) position pairs stored in sequence, making a
 * total of 2048 bytes for a single FA frame. */
#define FA_ENTRY_SIZE   (sizeof(struct fa_entry))
#define FA_ENTRY_COUNT  256
#define FA_FRAME_SIZE   (FA_ENTRY_COUNT * FA_ENTRY_SIZE)

/* Type for an entire row representing a single FA frame. */
struct fa_row { struct fa_entry row[FA_ENTRY_COUNT]; };


/* ioctl definitions. */

#define FASNIF_IOCTL_VERSION        1

/* Returns ioctl interface version number.  Just a sanity check. */
#define FASNIF_IOCTL_GET_VERSION    _IO('C', 0)
/* Restarts reading after eof on read(), subsequent calls to read() will succeed
 * if data is available. */
#define FASNIF_IOCTL_RESTART        _IO('C', 1)
/* Halts transfer if in progress.  Intended for debug use. */
#define FASNIF_IOCTL_HALT           _IO('C', 2)

/* Interrogates detailed status of FA sniffer. */
struct fa_status {
    unsigned int status;            // Hardware link status
    unsigned int partner;           // Associated link partner
    unsigned int last_interrupt;    // Status word from last interrupt
    unsigned int frame_errors;      // Hardware counts of communication errors
    unsigned int soft_errors;       //  accumulated since hardware initialised
    unsigned int hard_errors;
    bool running;                   // True if connection currently active
    bool overrun;                   // True if a buffer overrun occurred
};
#define FASNIF_IOCTL_GET_STATUS   _IOR('R', 1, struct fa_status)
