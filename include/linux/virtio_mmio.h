/*
 * Virtio platform device driver
 *
 * Copyright 2011, ARM Ltd.
 *
 * Based on Virtio PCI driver by Anthony Liguori, copyright IBM Corp. 2007
 *
 * This header is BSD licensed so anyone can use the definitions to implement
 * compatible drivers/servers.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of IBM nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL IBM OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _LINUX_VIRTIO_MMIO_H
#define _LINUX_VIRTIO_MMIO_H

/*
 * Control registers
 */

/* Magic value ("virt" string) - Read Only */
#define VIRTIO_MMIO_MAGIC_VALUE		0x000

/* Virtio device version - Read Only */
#define VIRTIO_MMIO_VERSION		0x004

/* Virtio device ID - Read Only */
#define VIRTIO_MMIO_DEVICE_ID		0x008

/* Virtio vendor ID - Read Only */
#define VIRTIO_MMIO_VENDOR_ID		0x00c

/* Bitmask of the features supported by the host
 * (32 bits per set) - Read Only */
#define VIRTIO_MMIO_HOST_FEATURES	0x010

/* Host features set selector - Write Only */
#define VIRTIO_MMIO_HOST_FEATURES_SEL	0x014

/* Bitmask of features activated by the guest
 * (32 bits per set) - Write Only */
#define VIRTIO_MMIO_GUEST_FEATURES	0x020

/* Activated features set selector - Write Only */
#define VIRTIO_MMIO_GUEST_FEATURES_SEL	0x024

/* Guest's memory page size in bytes - Write Only */
#define VIRTIO_MMIO_GUEST_PAGE_SIZE	0x028

/* Queue selector - Write Only */
#define VIRTIO_MMIO_QUEUE_SEL		0x030

/* Maximum size of the currently selected queue - Read Only */
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034

/* Queue size for the currently selected queue - Write Only */
#define VIRTIO_MMIO_QUEUE_NUM		0x038

/* Used Ring alignment for the currently selected queue - Write Only */
#define VIRTIO_MMIO_QUEUE_ALIGN		0x03c

/* Guest's PFN for the currently selected queue - Read Write */
#define VIRTIO_MMIO_QUEUE_PFN		0x040

/* Queue notifier - Write Only */
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050

/* Interrupt status - Read Only */
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060

/* Interrupt acknowledge - Write Only */
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064

/* Device status register - Read Write */
#define VIRTIO_MMIO_STATUS		0x070

/* Shared memory set selector - Write only */
#define VIRTIO_MMIO_SHM_ID_SEL          0x0ac

/* Shared memory region length - Read only */
#define VIRTIO_MMIO_SHM_LEN_LOW         0x0b0
#define VIRTIO_MMIO_SHM_LEN_HIGH        0x0b4

/* Shared memory region base addr - Read only */
#define VIRTIO_MMIO_SHM_ADDR_LOW        0x0b8
#define VIRTIO_MMIO_SHM_ADDR_HIGH       0x0bc

/* MSI max vector number - Read only */
#define VIRTIO_MMIO_MSI_VEC_NUM         0x0c0

/* MSI state - Read only */
#define VIRTIO_MMIO_MSI_STATE           0x0c4

/* MSI command - Write only */
#define VIRTIO_MMIO_MSI_CMD             0x0c8

/* MSI vector select - Write only */
#define VIRTIO_MMIO_MSI_VEC_SEL         0x0d0

/* MSI address - Write only */
#define VIRTIO_MMIO_MSI_ADDR_LO         0x0d4
#define VIRTIO_MMIO_MSI_ADDR_HI         0x0d8

/* MSI data - Write only */
#define VIRTIO_MMIO_MSI_DATA            0x0dc

/* The config space is defined by each driver as
 * the per-driver configuration space - Read Write */
#define VIRTIO_MMIO_CONFIG		0x100

/*
 * Interrupt flags (re: interrupt status & acknowledge registers)
 */

#define VIRTIO_MMIO_INT_VRING		(1 << 0)
#define VIRTIO_MMIO_INT_CONFIG		(1 << 1)

/* MSI commands */

#define VIRTIO_MMIO_MSI_CMD_ENABLE      0x1
#define VIRTIO_MMIO_MSI_CMD_DISABLE     0x2
#define VIRTIO_MMIO_MSI_CMD_CONFIGURE   0x3
#define VIRTIO_MMIO_MSI_CMD_MASK        0x4
#define VIRTIO_MMIO_MSI_CMD_UNMASK      0x5
#define VIRTIO_MMIO_MSI_CMD_MAP_CONFIG  0x6
#define VIRTIO_MMIO_MSI_CMD_MAP_QUEUE   0x7

/* Vector value used to disable an MSI event */

#define VIRTIO_MMIO_MSI_NO_VECTOR       0xffffffff

/* MSI reserved feature bits */

#define VIRTIO_F_MMIO_NOTIFICATION      39 
#define VIRTIO_F_MMIO_MSI               40

/* MSI state */

#define VIRTIO_MMIO_MSI_ENABLED         (1 << 31)
#define VIRTIO_MMIO_MSI_SHARING         (1 << 30)

#endif
