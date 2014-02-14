/*
 * Copyright (C) 2014 PathScale Inc. All Rights Reserved.
 */

/* micmem_io is a layer for accessing micmem using an IOCTL abstraction. This
 * layer performs most input validation and ensures thread-safety.
 */

#ifndef __MICMEM_IO_H__
#define __MICMEM_IO_H__

#ifdef __KERNEL__

#include "mic/micmem.h"
#include <linux/list.h>

/** struct mic_fd_data:
 * structure containing data for /dev/mic/ctrl
 */
struct mic_fd_data {
	struct file *filp;	/* data used for fasync state (see ioctl.c) */
#ifdef CONFIG_MK1OM
	/* currently open devices */
	struct micmem_ctx *mem_ctx[MAX_BOARD_SUPPORTED];

	/* list of micmem_range_entry attached to open dev */
	struct list_head range_list;
	/* list of micmem_pinned_entry attached to open dev */
	struct list_head pinned_list;
#endif /* CONFIG_MK1OM */
};

#ifdef CONFIG_MK1OM
/** struct micmem_range_entry:
 * data structure associating a user-visible address (key) to
 * struct dma_mem_range instances
 * TODO: convert to a hash table? what's the typical working set size?
 */
struct micmem_range_entry {
	uint32_t bdnum;
	void *uvaddr;
	struct dma_mem_range *mem_range;
	struct list_head list;
};

/** struct micmem_pinned_entry:
 * data structure keeping track of memory pinnings by associating a user-visible
 * address (key) to struct dma_mem_range instances.
 *
 * Lookups on this structure will always take O(n) time, but that's fine since
 * it's only accessed during mapping operations or deinitialization and not any
 * time-critical calls.
 */
struct micmem_pinned_entry {
	void *uvaddr;
	struct scif_pinned_pages *pinned_pages;
	struct list_head list;
};

int __micmem_opendev(struct mic_fd_data *fd_data, uint32_t bdnum);
int __micmem_closedev(struct mic_fd_data *fd_data, uint32_t bdnum);
int __micmem_map_range(struct mic_fd_data *fd_data, uint32_t bdnum, void *uvaddr, uint64_t size);
int __micmem_unmap_range(struct mic_fd_data *fd_data, uint32_t bdnum, void *uvaddr);
int __micmem_dev2host(struct mic_fd_data *fd_data, uint32_t bdnum, void *dest, uint64_t dest_offset, uint64_t source_dev, uint64_t size, int flags);
int __micmem_host2dev(struct mic_fd_data *fd_data, uint32_t bdnum, uint64_t dest_dev, void *src, uint64_t src_offset, uint64_t size, int flags);

#endif /* CONFIG_MK1OM */

int micmem_ioctl(struct file *filp, uint32_t cmd, uint64_t arg);
		
int micmem_fdopen(struct file *filp);
int micmem_fdclose(struct file *filp);

#else /* !__KERNEL__ */

#include <inttypes.h>
#include "micmem_const.h"

#endif /* __KERNEL__ */


#if !defined(__KERNEL__) || defined(CONFIG_MK1OM)

/* IOCTL interface */

/**
 * IOCTL_MICMEM_HOST2DEV:
 * Performs a synchronous transfer from a buffer in the calling process to
 * device physical memory.
 */
#define IOCTL_MICMEM_HOST2DEV   _IOW('c', 15, struct ctrlioctl_micmem_host2dev)
/**
 * IOCTL_MICMEM_DEV2HOST:
 * Performs a synchronous transfer from device physical memory to a buffer in
 * the calling process.
 */
#define IOCTL_MICMEM_DEV2HOST   _IOW('c', 16, struct ctrlioctl_micmem_dev2host)
/**
 * IOCTL_MICMEM_OPENDEV:
 * Binds the fd to a specified device for further DMA operations.
 *
 * Takes the board number as the sole argument.
 */
#define IOCTL_MICMEM_OPENDEV    _IOW('c', 17, uint32_t)
/**
 * IOCTL_MICMEM_CLOSEDEV:
 * Unbinds the fd from the device if bound.
 * 
 * Takes the board number as the sole argument.
 */
#define IOCTL_MICMEM_CLOSEDEV   _IOW('c', 18, uint32_t)

/**
 * IOCTL_MICMEM_PINRANGE:
 * Pins a memory region for further mapping it to device.
 * 
 * No device needs to be open at the moment of this call.
 * Memory regions may not overlap.
 */
#define IOCTL_MICMEM_PINMEM	_IOW('c', 19, struct ctrlioctl_micmem_pinmem)
/**
 * IOCTL_MICMEM_UNPINMEM:
 * Unpins a memory region. A device doesn't have to be open at the moment of
 * this call.
 *
 * Takes a pointer to a previously pinned memory area. If the pointer hasn't
 * been pinned with IOCTL_MICMEM_PINMEM, returns EINVAL.
 */
#define IOCTL_MICMEM_UNPINMEM	_IOW('c', 19, void*)
/**
 * IOCTL_MICMEM_MAPRANGE:
 * Maps a memory region to currently selected device, allowing for DMA transfers
 * to it. The memory region must be previousky registered with micmem_pinmem.
 * Memory regions may not overlap.
 */
#define IOCTL_MICMEM_MAPRANGE	_IOW('c', 20, struct ctrlioctl_micmem_maprange)
/**
 * IOCTL_MICMEM_UNMAPRANGE:
 * Unmaps memory from device.
 * If the pointer hasn't been mapped with IOCTL_MICMEM_MAPRANGE, returns EINVAL.
 */
#define IOCTL_MICMEM_UNMAPRANGE	_IOW('c', 21, \
		struct ctrlioctl_micmem_unmaprange)

/**
 * struct ctrlioctl_micmem_dev2host:
 *
 * \param bdnum	Device number
 * \param dest	Previously mapped destination buffer
 * \param dest_offset	Byte offset into the destination buffer where data will
 *			be stored
 * \param source_dev	Device physical address of the data to be transferred
 * \param size	Size of transfer
 * \param flags	Flags determining the number of channels to use: [MICMEM_AUTO
 *		MICMEM_SINGLE MICMEM_DUAL]
 *
 * All parameters must be multiple of page size (4096B)
 */
struct ctrlioctl_micmem_dev2host {
	uint32_t bdnum;
	void *dest;
	uint64_t dest_offset;
	uint64_t source_dev;
	uint64_t size;
	int flags;
};

/**
 * struct ctrlioctl_micmem_host2dev:
 *
 * \param bdnum	Device number
 * \param src	Previously mapped source buffer
 * \param src_offset	Byte offset into the source buffer where data is stored
 * \param dest_dev	Device physical address for the data to be stored
 * \param size	Size of transfer
 * \param flags	Flags determining the number of channels to use: [MICMEM_AUTO
 *		MICMEM_SINGLE MICMEM_DUAL]
 *
 * All parameters must be multiple of page size (4096B)
 */
struct ctrlioctl_micmem_host2dev {
	uint32_t bdnum;
	void *src;
	uint64_t src_offset;
	uint64_t dest_dev;
	uint64_t size;
	int flags;
};

/**
 * struct ctrlioctl_micmem_pinmem:
 *
 * \param addr	User host address of memory range to pin
 * \param size	Size of memory range
 *
 * All parameters must be multiple of page size (4096B)
 */
struct ctrlioctl_micmem_pinmem {
	void *addr;
	uint64_t size;
};

/**
 * struct ctrlioctl_micmem_maprange:
 *
 * \param bdnum	Device identifier
 * \param addr	User host address of the memory range to be mapped
 * \param size	Size of the memory range
 *
 * addr and size parameters must be multiple of page size (4096B)
 */
struct ctrlioctl_micmem_maprange {
	uint32_t bdnum;
	void *addr;
	uint64_t size;
};

/**
 * struct ctrlioctl_micmem_unmaprange:
 *
 * \param bdnim	Device identifier
 * \param addr	Pointer to a previously mapped memory region. Must be multiple
 * 		of page size (4096B)
 */
struct ctrlioctl_micmem_unmaprange {
	uint32_t bdnum;
	void *addr;
};

#endif /* !__KERNEL__ || CONFIG_MK1OM */

#endif /* __MICMEM_IO_H__ */
