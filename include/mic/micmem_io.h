/*
 * Copyright (C) 2014 PathScale Inc. All Rights Reserved.
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
	struct file *filp;  /* data used for fasync state (see ioctl.c) */
	struct micmem_ctx *mem_ctx; /* currently open device */
	struct list_head range_list;   /* micmem_range_entry attached to open dev */
};

/** struct micmem_range_entry:
 * data structure associating a user-visible address (key) to
 * struct dma_mem_range instances
 * TODO: convert to a hash table?
 */
struct micmem_range_entry {
	void *uvaddr;
	struct dma_mem_range *mem_range;
	struct list_head list;
};

int __micmem_opendev(struct mic_fd_data *fd_data, mic_ctx_t *mic_ctx);
void __micmem_closedev(struct mic_fd_data *fd_data);
int __micmem_map_range(struct mic_fd_data *fd_data, void *uvaddr, uint64_t size);
int __micmem_unmap_range(struct mic_fd_data *fd_data, void *uvaddr);
int __micmem_dev2host(struct mic_fd_data *fd_data, void *dest, uint64_t dest_offset, uint64_t source_dev, uint64_t size);
int __micmem_host2dev(struct mic_fd_data *fd_data, uint64_t dest_dev, void *src, uint64_t src_offset, uint64_t size);

int micmem_ioctl(struct file *filp, uint32_t cmd, uint64_t arg);
		
int micmem_fdopen(struct file *filp);
int micmem_fdclose(struct file *filp);

#else /* !__KERNEL__ */

#include <inttypes.h>

#endif /* __KERNEL__ */
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
 * Takes the board number as the sole argument
 */
#define IOCTL_MICMEM_OPENDEV    _IOW('c', 17, int)
/**
 * IOCTL_MICMEM_CLOSEDEV:
 * Unbinds the fd from the device if bound.
 */
#define IOCTL_MICMEM_CLOSEDEV   _IO('c', 18)
/**
 * IOCTL_MICMEM_MAPRANGE:
 * Pins a memory region and maps it to device, allowing for DMA transfers on it.
 * Memory regions may not overlap.
 */
#define IOCTL_MICMEM_MAPRANGE   _IOW('c', 19, struct ctrlioctl_micmem_maprange)
/**
 * IOCTL_MICMEM_UNMAPRANGE:
 * Unmaps memory from device and unpins it.
 * If the pointer hasn't been mapped with IOCTL_MICMEM_MAPRANGE, returns EINVAL.
 *
 * Takes a pointer to a previously mapped memory region as the sole argument.
 */
#define IOCTL_MICMEM_UNMAPRANGE _IOW('c', 20, void*)

/**
 * struct ctrlioctl_micmem_dev2host:
 *
 * \param dest	            Previously mapped destination buffer
 * \param dest_offset       Byte offset into the destination buffer where data
 *                          will be stored
 * \param source_dev        Device physical address of the data to be
 *                          transferred
 * \param size		        Size of transfer
 *
 * All parameters must be multiple of page size (4096B)
 */
struct ctrlioctl_micmem_dev2host {
	void *dest;
	uint64_t dest_offset;
	uint64_t source_dev;
	uint64_t size;
};

/**
 * struct ctrlioctl_micmem_host2dev:
 *
 * \param src	            Previously mapped source buffer
 * \param src_offset        Byte offset into the source buffer where data
 *                          is stored
 * \param dest_dev          Device physical address for the data to be stored
 * \param size		        Size of transfer
 *
 * All parameters must be multiple of page size (4096B)
 */
struct ctrlioctl_micmem_host2dev {
	void *src;
	uint64_t src_offset;
	uint64_t dest_dev;
	uint64_t size;
};

/**
 * struct ctrlioctl_micmem_maprange:
 *
 * \param addr              user host address of the memory range to be mapped
 * \param size              size of the memory range
 */
struct ctrlioctl_micmem_maprange {
	void *addr;
	uint64_t size;
};

#endif
