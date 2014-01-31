/*
 * Copyright (C) 2014 PathScale Inc. All Rights Reserved.
 */


#include "mic/micmem.h"
#include "mic/micmem_io.h"
#include <linux/mutex.h>

/* Functions that have to do with keeping the kernel-side state of data
 * structures.
 * FIXME: replace one coarse lock with fine-grained locks, at least to get full
 * duplex.
 */

static DEFINE_MUTEX(ioctl_lock);

static struct micmem_range_entry*
micmem_find_range_item(struct mic_fd_data *fd_data, void *uvaddr)
{
	struct list_head *head = &(fd_data->range_list);
	struct list_head *cur;
	struct micmem_range_entry *range_item;
	
	list_for_each(cur, head) {
		range_item = list_entry(cur, struct micmem_range_entry, list);
		if (range_item->uvaddr == uvaddr) {
			return range_item;
		}
	}
	return 0;
}

static struct dma_mem_range*
micmem_find_dma_range(struct mic_fd_data *fd_data, void *uvaddr)
{
	struct micmem_range_entry* range_item;
	range_item = micmem_find_range_item(fd_data, uvaddr);
	if (!range_item)
		return 0;
	
	return range_item->mem_range;
}

static int micmem_cleanup_ranges(struct mic_fd_data *fd_data)
{
	struct micmem_ctx *mem_ctx = fd_data->mem_ctx;
	struct micmem_range_entry* range_item;
	struct list_head *head = &(fd_data->range_list);
	struct list_head *cur;
	struct list_head *tmp;
	
	/* The whole list needs to be removed, therefore we don't care about
	 * unlinking ranges properly.
	 */
	
	list_for_each_safe(cur, tmp, head) {
		range_item = list_entry(cur, struct micmem_range_entry, list);
		micmem_unmap_range(mem_ctx->mic_ctx, range_item->mem_range);
		kfree(range_item);
	}
	
	/* After freeing items, the (empty) list is in an inconsistent state;
	 * need to reinitialize it.
	 */
	INIT_LIST_HEAD(head);
	return 0;
}

/**
 * __micmem_opendev:
 * Initializes device context for DMA access and binds it to fd. Only 1 context
 * (i.e. 1 device) allowed per open file.
 */
int __micmem_opendev(struct mic_fd_data *fd_data, mic_ctx_t *mic_ctx)
{
	int status;
	struct micmem_ctx *mem_ctx;
	
	mem_ctx = kmalloc(sizeof(*mem_ctx), GFP_KERNEL);
	if (!mem_ctx)
		return -ENOMEM;
		
	if ((status = micmem_get_mem_ctx(mic_ctx, mem_ctx))) {
		kfree(mem_ctx);
		return status;
	}

	/* XXX: wouldn't that need a lock? */
	fd_data->mem_ctx = mem_ctx;
	return status;
}

/**
 * __micmem_closedev:
 * Deinitializes device context and unbinds it from fd.
 */
void __micmem_closedev(struct mic_fd_data *fd_data)
{
	micmem_cleanup_ranges(fd_data);
	micmem_destroy_mem_ctx(fd_data->mem_ctx);
	kfree(fd_data->mem_ctx);
	fd_data->mem_ctx = NULL;
}

int __micmem_map_range(struct mic_fd_data *fd_data, void *uvaddr, uint64_t size)
{
	struct micmem_ctx *mem_ctx = fd_data->mem_ctx;
	struct micmem_range_entry *range_item;
	struct list_head *head = &(fd_data->range_list);
	int status = 0;
	
	/* Do not check if memory ranges are overlapping or if multiple ones
	 * have the same address: micmem_map_range will fail to pin them if this
	 * is the case.
	 */
	
	range_item = kmalloc(sizeof(*range_item), GFP_KERNEL);
	if (!range_item)
		return -ENOMEM;
	
	range_item->uvaddr = uvaddr;
	
	if ((status = micmem_map_range(mem_ctx->mic_ctx, uvaddr, size,
			&(range_item->mem_range)))) {
		kfree(range_item);
		return status;
	}

	list_add(&range_item->list, head);
	return 0;
}

int __micmem_unmap_range(struct mic_fd_data *fd_data, void *uvaddr)
{
	struct micmem_ctx *mem_ctx = fd_data->mem_ctx;
	struct micmem_range_entry *range_item;

	range_item = micmem_find_range_item(fd_data, uvaddr);
	if (!range_item)
		return -EINVAL;
		
	micmem_unmap_range(mem_ctx->mic_ctx, range_item->mem_range);
	list_del(&(range_item->list));
	kfree(range_item);
	return 0;
}

/**
 * __micmem_dev2host:
 * Wrapper around micmem_dev2host call.
 *
 * @fd_data: private file descriptor data
 * @dest: user virtual address to previously registered destination
 * @dest_offset: offset into destination range (must be multiple of page size)
 * @source_dev: physical address of source device memory
 * @size: transfer size
 */
int __micmem_dev2host(struct mic_fd_data *fd_data, void *dest,
		uint64_t dest_offset, uint64_t source_dev, uint64_t size)
{
	struct micmem_ctx *mem_ctx = fd_data->mem_ctx;
	struct dma_mem_range *dest_range;
	
	if (!capable(CAP_SYS_ADMIN)) {
		printk(KERN_ERR "Cannot execute unless sysadmin\n");
		return -EPERM;
	}

	if (dest == NULL) {
		return -EINVAL;
	}

	dest_range = micmem_find_dma_range(fd_data, dest);
	if (dest_range == NULL) {
		printk(KERN_ERR "Address not registered\n");
		return -EINVAL;
	}

	return micmem_dev2host(mem_ctx, dest_range, dest_offset, source_dev,
			size);
}

/**
 * __micmem_host2dev:
 * Wrapper around micmem_host2dev call.
 *
 * @fd_data: private file descriptor data
 * @dest_dev: physical address of source device memory
 * @src: user virtual address to previously registered source range
 * @src_offset: offset into source range (must be multiple of page size)
 * @size: transfer size
 */
int __micmem_host2dev(struct mic_fd_data *fd_data, uint64_t dest_dev,
		void *src, uint64_t src_offset, uint64_t size)
{
	struct micmem_ctx *mem_ctx = fd_data->mem_ctx;
	struct dma_mem_range *src_range;
	
	if (!capable(CAP_SYS_ADMIN)) {
		printk(KERN_ERR "Cannot execute unless sysadmin\n");
		return -EPERM;
	}

	if (src == NULL) {
		return -EINVAL;
	}

	src_range = micmem_find_dma_range(fd_data, src);
	if (src_range == NULL) {
		printk(KERN_ERR "Address not registered\n");
		return -EINVAL;
	}

	return micmem_host2dev(mem_ctx, dest_dev, src_range, src_offset, size);
}

int micmem_fdopen(struct file *filp)
{
	struct mic_fd_data *fd_data;
	
	fd_data = kzalloc(sizeof(*fd_data), GFP_KERNEL);
	if (!fd_data)
		return -ENOMEM;
	
	INIT_LIST_HEAD(&(fd_data->range_list));
	filp->private_data = (void*)fd_data;
	return 0;
}

int micmem_fdclose(struct file *filp)
{
	struct mic_fd_data *fd_data = filp->private_data;
	BUG_ON(!fd_data);

	if (fd_data->mem_ctx)
		__micmem_closedev(fd_data);
	
	kfree(fd_data);
	return 0;
}

/* Part of ioctl function which executes within a critical section */
static int micmem_ioctl_inner(struct file *filp, uint32_t cmd, uint64_t arg)
{
	int status = 0;
	struct mic_fd_data *fd_data = (struct mic_fd_data *)filp->private_data;
	void __user *argp = (void __user *)arg;
	
	switch(cmd) {
	/* select device to use with this fd */
	case IOCTL_MICMEM_OPENDEV:
	{
		uint32_t bdnum = 0;
		mic_ctx_t *mic_ctx;
		
		BUG_ON(!fd_data);
		if (fd_data->mem_ctx) {
			printk(KERN_ERR "IOCTL error: device was already selected. Free it first\n");
			return -EINVAL;
		}
		if (copy_from_user(&bdnum, argp, sizeof(uint32_t))) {
			return -EFAULT;
		}
		
		if (bdnum >= (uint32_t)mic_data.dd_numdevs) {
			printk(KERN_ERR "IOCTL error: given board num is invalid\n");
			return -EINVAL;
		}
		
		mic_ctx = get_per_dev_ctx(bdnum);
		if (!mic_ctx) {
			printk(KERN_ERR "IOCTL error: null mic context\n");
			return -EFAULT;
		}
		
		return __micmem_opendev(fd_data, mic_ctx);
	}
	case IOCTL_MICMEM_CLOSEDEV:
	{
		if (!fd_data->mem_ctx) {
			printk(KERN_ERR "IOCTL error: No device was selected.\n");
			return -EINVAL;
		}
		
		__micmem_closedev(fd_data);
		return 0;
	}
	case IOCTL_MICMEM_MAPRANGE:
	{
		struct ctrlioctl_micmem_maprange args = {0};
		
		if (!fd_data->mem_ctx) {
			printk(KERN_ERR "IOCTL error: no device was selected.\n");
			return -EINVAL;
		}
		
		if (copy_from_user(&args, argp,
				sizeof(struct ctrlioctl_micmem_maprange))) {
			return -EFAULT;
		}
		
		status = __micmem_map_range(fd_data, args.addr, args.size);
		return status;
	}
	case IOCTL_MICMEM_UNMAPRANGE:
	{
		void *addr;
		
		if (!fd_data->mem_ctx) {
			printk(KERN_ERR "IOCTL error: no device was selected.\n");
			return -EINVAL;
		}
		
		if (copy_from_user(&addr, argp, sizeof(void*))) {
			return -EFAULT;
		}
		
		status = __micmem_unmap_range(fd_data, addr);
		return status;
	}
	case IOCTL_MICMEM_DEV2HOST:
	{
		struct ctrlioctl_micmem_dev2host args = {0};

		if (!fd_data->mem_ctx) {
			printk(KERN_ERR "IOCTL error: No device was selected.\n");
			return -EINVAL;
		}
		
		if (copy_from_user(&args, argp,
				sizeof(struct ctrlioctl_micmem_dev2host))) {
			return -EFAULT;
		}

		status = __micmem_dev2host(fd_data, args.dest, args.dest_offset,
				args.source_dev, args.size);
		if (status) {
			printk(KERN_ERR "IOCTL error: failed to complete IOCTL\n");
			return status;
		}

		break;
	}
	case IOCTL_MICMEM_HOST2DEV:
	{
		struct ctrlioctl_micmem_host2dev args = {0};

		if (!fd_data->mem_ctx) {
			printk(KERN_ERR "IOCTL error: No device was selected.\n");
			return -EINVAL;
		}
		
		if (copy_from_user(&args, argp,
				sizeof(struct ctrlioctl_micmem_host2dev))) {
			return -EFAULT;
		}

		status = __micmem_host2dev(fd_data, args.dest_dev, args.src,
				args.src_offset, args.size);
		if (status) {
			printk(KERN_ERR "IOCTL error: failed to complete IOCTL\n");
			return status;
		}

		break;
	}
	default:
		status = -EINVAL;
		break;
	}
	return status;
}


/* Base ioctl function */
int micmem_ioctl(struct file *filp, uint32_t cmd, uint64_t arg)
{
	int status = 0;
	
	switch (cmd) {
	case IOCTL_MICMEM_OPENDEV:
	case IOCTL_MICMEM_CLOSEDEV:
	case IOCTL_MICMEM_MAPRANGE:
	case IOCTL_MICMEM_UNMAPRANGE:
	case IOCTL_MICMEM_DEV2HOST:
	case IOCTL_MICMEM_HOST2DEV:
	{
		mutex_lock(&ioctl_lock);
		status = micmem_ioctl_inner(filp, cmd, arg);
		mutex_unlock(&ioctl_lock);
		break;
	}
	default:
		printk("Invalid IOCTL");
		status = -EINVAL;
		break;
	}
	return status;
}
