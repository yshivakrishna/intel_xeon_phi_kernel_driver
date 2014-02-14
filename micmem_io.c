/*
 * Copyright (C) 2014 PathScale Inc. All Rights Reserved.
 */
 
/* Functions that have to do with keeping the kernel-side state of data
 * structures.
 */

#include "mic/micmem.h"
#include "mic/micmem_io.h"
#include <linux/slab.h>
#include <linux/fs.h>

#ifdef CONFIG_MK1OM

#include <linux/mutex.h>

#define err_page_align(var, name) ({ \
	if (!IS_ALIGNED(var, PAGE_SIZE)) { \
		printk(KERN_ERR name " not on page size boundary.\n"); \
		return -EINVAL; \
	} \
})

/* FIXME: replace one coarse lock with fine-grained locks, at least to get full
 * duplex and multiple simultaneous devices support.
 */
static DEFINE_MUTEX(ioctl_lock);

static struct micmem_range_entry*
micmem_find_range_item(struct mic_fd_data *fd_data, uint32_t bdnum,
		void *uvaddr)
{
	struct list_head *head = &(fd_data->range_list);
	struct list_head *cur;
	struct micmem_range_entry *range_item;

	list_for_each(cur, head) {
		range_item = list_entry(cur, struct micmem_range_entry, list);
		if ((range_item->bdnum == bdnum) &&
				(range_item->uvaddr == uvaddr)) {
			return range_item;
		}
	}
	return 0;
}

/**
 * micmem_find_pinned_item:
 * Finds a pinning corresponding to given (addr, length) memory area.
 *
 * If @length == 0, then finds the exact address entry.
 */
static struct micmem_pinned_entry*
micmem_find_pinned_item(struct mic_fd_data *fd_data, void *uvaddr,
		uint64_t length)
{
	struct list_head *head = &(fd_data->pinned_list);
	struct list_head *cur;
	struct micmem_pinned_entry *pinned_item;
	uint64_t size;

	list_for_each(cur, head) {
		pinned_item = list_entry(cur, struct micmem_pinned_entry, list);
		size = pinned_item->pinned_pages->nr_pages << PAGE_SHIFT;
		if (length == 0) {
			if (pinned_item->uvaddr == uvaddr)
				return pinned_item;
		} else if ((pinned_item->uvaddr + size >= uvaddr + length) &&
				(pinned_item->uvaddr <= uvaddr)) {
			return pinned_item;
		}
	}
	return 0;
}

static struct dma_mem_range*
micmem_find_dma_range(struct mic_fd_data *fd_data, uint32_t bdnum, void *uvaddr)
{
	struct micmem_range_entry* range_item;
	range_item = micmem_find_range_item(fd_data, bdnum, uvaddr);
	if (!range_item)
		return 0;

	return range_item->mem_range;
}

static int micmem_cleanup_mappings(struct mic_fd_data *fd_data, uint32_t bdnum)
{
	struct micmem_ctx *mem_ctx = fd_data->mem_ctx[bdnum];
	struct micmem_range_entry* range_item;
	struct list_head *head = &(fd_data->range_list);
	struct list_head *cur;
	struct list_head *tmp;

	list_for_each_safe(cur, tmp, head) {
		range_item = list_entry(cur, struct micmem_range_entry, list);
		if (range_item->bdnum == bdnum) {
			micmem_unmap_range(mem_ctx->mic_ctx,
				range_item->mem_range);
			list_del(&(range_item->list));
			kfree(range_item);
		}
	}
	return 0;
}

static int micmem_cleanup_pinnings(struct mic_fd_data *fd_data)
{
	struct micmem_pinned_entry* pinned_item;
	struct list_head *head = &(fd_data->pinned_list);
	struct list_head *cur;
	struct list_head *tmp;

	/* The whole list needs to be removed, therefore we don't care about
	 * unlinking ranges properly.
	 */

	list_for_each_safe(cur, tmp, head) {
		pinned_item = list_entry(cur, struct micmem_pinned_entry, list);
		micmem_unpin_range(pinned_item->pinned_pages);
		kfree(pinned_item);
	}

	/* No need to reinitialize the list; this function only runs when fd is
	 * being closed. */
	return 0;
}

/**
 * __micmem_opendev:
 * Initializes device context for DMA access and binds it to fd. Only 1 context
 * (i.e. 1 device) allowed per open file.
 */
int __micmem_opendev(struct mic_fd_data *fd_data, uint32_t bdnum)
{
	int status;
	struct micmem_ctx *mem_ctx;
	mic_ctx_t *mic_ctx;

	if (fd_data->mem_ctx[bdnum]) {
		printk(KERN_ERR "Device is already open.\n");
		return -EBUSY;
	}

	mic_ctx = get_per_dev_ctx(bdnum);
	if (!mic_ctx) {
		printk(KERN_ERR "IOCTL error: null mic context\n");
		return -EFAULT;
	}

	mem_ctx = kmalloc(sizeof(*mem_ctx), GFP_KERNEL);
	if (!mem_ctx)
		return -ENOMEM;

	if ((status = micmem_get_mem_ctx(mic_ctx, mem_ctx))) {
		kfree(mem_ctx);
		return status;
	}

	fd_data->mem_ctx[bdnum] = mem_ctx;
	return status;
}

/**
 * __micmem_closedev:
 * Deinitializes device context and unbinds it from fd.
 */
int __micmem_closedev(struct mic_fd_data *fd_data, uint32_t bdnum)
{
	struct micmem_ctx *mem_ctx = fd_data->mem_ctx[bdnum];
	if (!mem_ctx) {
		printk(KERN_ERR "Device not open.\n");
		return -EINVAL;
	}
	micmem_cleanup_mappings(fd_data, bdnum);
	fd_data->mem_ctx[bdnum] = NULL;
	micmem_destroy_mem_ctx(mem_ctx);
	kfree(mem_ctx);
	return 0;
}

int __micmem_pin_range(struct mic_fd_data *fd_data, void *uvaddr, uint64_t size)
{
	struct micmem_pinned_entry *pinned_item;
	struct list_head *phead = &(fd_data->pinned_list);
	int status;

	err_page_align(size, "Size");
	err_page_align((uint64_t)uvaddr, "Data beginning");

	if (!(pinned_item = kmalloc(sizeof(*pinned_item), GFP_KERNEL)))
		return -ENOMEM;

	pinned_item->uvaddr = uvaddr;

	if ((status = micmem_pin_range(uvaddr, size,
			&(pinned_item->pinned_pages)))) {
		kfree(pinned_item);
		return status;
	}

	list_add(&pinned_item->list, phead);
	return 0;
}

int __micmem_map_range(struct mic_fd_data *fd_data, uint32_t bdnum,
		void *uvaddr, uint64_t size)
{
	struct micmem_ctx *mem_ctx = fd_data->mem_ctx[bdnum];
	struct micmem_range_entry *range_item;
	struct list_head *head = &(fd_data->range_list);
	struct micmem_pinned_entry *pinned_item;
	struct scif_pinned_pages *pinned_pages;
	int status;
	uint64_t offset;

	if (!mem_ctx) {
		printk(KERN_ERR "Device not open.\n");
		return -EINVAL;
	}

	err_page_align(size, "Size");
	err_page_align((uint64_t)uvaddr, "Range beginning");

	if (!(pinned_item = micmem_find_pinned_item(fd_data, uvaddr, size))) {
		printk(KERN_ERR "Range was not previously pinned.\n");
		return -EINVAL;
	}
	pinned_pages = pinned_item->pinned_pages;
	offset = (uint64_t)(uvaddr - pinned_item->uvaddr);

	range_item = kmalloc(sizeof(*range_item), GFP_KERNEL);
	if (!range_item)
		return -ENOMEM;

	range_item->bdnum = bdnum;
	range_item->uvaddr = uvaddr;

	if ((status = micmem_map_range(mem_ctx->mic_ctx, pinned_pages, offset,
			size, &(range_item->mem_range)))) {
		kfree(range_item);
		return status;
	}

	list_add(&range_item->list, head);
	return 0;
}

int __micmem_unmap_range(struct mic_fd_data *fd_data, uint32_t bdnum,
		void *uvaddr)
{
	struct micmem_ctx *mem_ctx = fd_data->mem_ctx[bdnum];
	struct micmem_range_entry *range_item;

	/* No need to check for open device explicitly, find will fail if it's
	 * not open because closing dev unmaps all its mappings. */
	range_item = micmem_find_range_item(fd_data, bdnum, uvaddr);
	if (!range_item) {
		printk(KERN_ERR "Memory not mapped\n");
		return -EINVAL;
	}

	micmem_unmap_range(mem_ctx->mic_ctx, range_item->mem_range);
	list_del(&(range_item->list));
	kfree(range_item);
	return 0;
}

int __micmem_unpin_range(struct mic_fd_data *fd_data, void *uvaddr)
{
	struct micmem_pinned_entry *pinned_item;

	pinned_item = micmem_find_pinned_item(fd_data, uvaddr, 0);
	if (!pinned_item) {
		printk(KERN_ERR "Memory not pinned\n");
		return -EINVAL;
	}

	micmem_unpin_range(pinned_item->pinned_pages);
	list_del(&(pinned_item->list));
	kfree(pinned_item);
	return 0;
}

/**
 * __micmem_dev2host:
 * Wrapper around micmem_dev2host call.
 *
 * @fd_data:	private file descriptor data
 * @bdnum:	device number to transfer from
 * @dest:	user virtual address to previously registered destination
 * @dest_offset:	offset into destination range (must be multiple of page
 * 			size)
 * @source_dev:	physical address of source device memory
 * @size:	transfer size
 * @flags:	flags passed to micmem_dev2host
 */
int __micmem_dev2host(struct mic_fd_data *fd_data, uint32_t bdnum, void *dest,
		uint64_t dest_offset, uint64_t source_dev, uint64_t size,
		int flags)
{
	struct micmem_ctx *mem_ctx = fd_data->mem_ctx[bdnum];
	struct dma_mem_range *dest_range;

	if (!capable(CAP_SYS_ADMIN)) {
		printk(KERN_ERR "Cannot execute unless sysadmin\n");
		return -EPERM;
	}

	/* No need to check if device was open, no mapping will be present if
	 * it's not. */
	dest_range = micmem_find_dma_range(fd_data, bdnum, dest);
	if (dest_range == NULL) {
		printk(KERN_ERR "Address not registered\n");
		return -EINVAL;
	}

	return micmem_dev2host(mem_ctx, dest_range, dest_offset, source_dev,
			size, flags);
}

/**
 * __micmem_host2dev:
 * Wrapper around micmem_host2dev call.
 *
 * @fd_data:	private file descriptor data
 * @bdnum:	number of the destination device
 * @dest_dev:	physical address of source device memory
 * @src:	user virtual address to previously registered source range
 * @src_offset:	offset into source range (must be multiple of page size)
 * @size:	transfer size
 * @flags:	flags passed to micmem_host2dev
 */
int __micmem_host2dev(struct mic_fd_data *fd_data, uint32_t bdnum,
		uint64_t dest_dev, void *src, uint64_t src_offset,
		uint64_t size, int flags)
{
	struct micmem_ctx *mem_ctx = fd_data->mem_ctx[bdnum];
	struct dma_mem_range *src_range;

	/* No need to check if device was open, no mapping will be present if
	 * it's not. */
	src_range = micmem_find_dma_range(fd_data, bdnum, src);
	if (src_range == NULL) {
		printk(KERN_ERR "Address not registered\n");
		return -EINVAL;
	}

	return micmem_host2dev(mem_ctx, dest_dev, src_range, src_offset, size,
		flags);
}

/* Part of ioctl function which executes within a critical section */
static int micmem_ioctl_inner(struct file *filp, uint32_t cmd, uint64_t arg)
{
	int status = 0;
	struct mic_fd_data *fd_data = (struct mic_fd_data *)filp->private_data;
	void __user *argp = (void __user *)arg;

	switch(cmd) {
	case IOCTL_MICMEM_OPENDEV:
	{
		uint32_t bdnum = 0;

		if (copy_from_user(&bdnum, argp, sizeof(uint32_t))) {
			return -EFAULT;
		}

		if (bdnum >= (uint32_t)mic_data.dd_numdevs) {
			printk(KERN_ERR "IOCTL error: given board num is invalid\n");
			return -ENODEV;
		}

		BUG_ON(!fd_data);
		return __micmem_opendev(fd_data, bdnum);
	}
	case IOCTL_MICMEM_CLOSEDEV:
	{
		uint32_t bdnum = 0;

		if (copy_from_user(&bdnum, argp, sizeof(uint32_t))) {
			return -EFAULT;
		}

		return __micmem_closedev(fd_data, bdnum);
	}
	case IOCTL_MICMEM_PINMEM:
	{
		struct ctrlioctl_micmem_pinmem args = {0};
		if (copy_from_user(&args, argp,
				sizeof(struct ctrlioctl_micmem_pinmem)))
			return -EFAULT;

		return __micmem_pin_range(fd_data, args.addr, args.size);
	}
	case IOCTL_MICMEM_UNPINMEM:
	{
		void *addr;

		if (copy_from_user(&addr, argp, sizeof(void*)))
			return -EFAULT;

		return __micmem_unpin_range(fd_data, addr);
	}
	case IOCTL_MICMEM_MAPRANGE:
	{
		struct ctrlioctl_micmem_maprange args = {0};
		if (copy_from_user(&args, argp,
				sizeof(struct ctrlioctl_micmem_maprange)))
			return -EFAULT;

		return __micmem_map_range(fd_data, args.bdnum, args.addr,
			args.size);
	}
	case IOCTL_MICMEM_UNMAPRANGE:
	{
		struct ctrlioctl_micmem_unmaprange args = {0};
		if (copy_from_user(&args, argp,
				sizeof(struct ctrlioctl_micmem_unmaprange)))
			return -EFAULT;

		return __micmem_unmap_range(fd_data, args.bdnum, args.addr);
	}
	case IOCTL_MICMEM_DEV2HOST:
	{
		struct ctrlioctl_micmem_dev2host args = {0};

		/* dev2host and host2dev are distinct in their capability to
		 * affect other processes: they access memory space of the
		 * device, which is global across the whole system. Therefore
		 * only explicitly privileged users should be able to access it.
		 *
		 * TODO: Maybe this should be implemented as device permissions
		 * instead combined with some "micmem" group?
		 */
		if (!capable(CAP_SYS_ADMIN)) {
			printk(KERN_ERR "Cannot execute unless sysadmin\n");
			return -EPERM;
		}

		if (copy_from_user(&args, argp,
				sizeof(struct ctrlioctl_micmem_dev2host))) {
			return -EFAULT;
		}

		status = __micmem_dev2host(fd_data, args.bdnum, args.dest,
				args.dest_offset, args.source_dev, args.size,
				args.flags);
		if (status)
			printk(KERN_ERR "IOCTL error: failed to complete IOCTL\n");
		return status;
	}
	case IOCTL_MICMEM_HOST2DEV:
	{
		struct ctrlioctl_micmem_host2dev args = {0};

		/* host2dev can affect other processes running on the system,
		 * see note for dev2host. */
		if (!capable(CAP_SYS_ADMIN)) {
			printk(KERN_ERR "Cannot execute unless sysadmin\n");
			return -EPERM;
		}

		if (copy_from_user(&args, argp,
				sizeof(struct ctrlioctl_micmem_host2dev))) {
			return -EFAULT;
		}

		status = __micmem_host2dev(fd_data, args.bdnum, args.dest_dev,
				args.src, args.src_offset, args.size,
				args.flags);
		if (status)
			printk(KERN_ERR "IOCTL error: failed to complete IOCTL\n");
		return status;
	}
	default:
		status = -EINVAL;
		break;
	}
	return status;
}
#endif /* CONFIG_MK1OM */

/* Base ioctl function */
int micmem_ioctl(struct file *filp, uint32_t cmd, uint64_t arg)
{
	int status = 0;
#ifdef CONFIG_MK1OM
	switch (cmd) {
	case IOCTL_MICMEM_OPENDEV:
	case IOCTL_MICMEM_CLOSEDEV:
	case IOCTL_MICMEM_PINMEM:
	case IOCTL_MICMEM_UNPINMEM:
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
#else
	printk("Invalid IOCTL");
	status = -EINVAL;
#endif
	return status;
}


int micmem_fdopen(struct file *filp)
{
	struct mic_fd_data *fd_data;

	fd_data = kzalloc(sizeof(*fd_data), GFP_KERNEL);
	if (!fd_data)
		return -ENOMEM;
#ifdef CONFIG_MK1OM
	INIT_LIST_HEAD(&(fd_data->range_list));
	INIT_LIST_HEAD(&(fd_data->pinned_list));
#endif /* CONFIG_MK1OM */
	filp->private_data = (void*)fd_data;
	return 0;
}

int micmem_fdclose(struct file *filp)
{
	struct mic_fd_data *fd_data = filp->private_data;
	int i;
	int err;
	BUG_ON(!fd_data);
#ifdef CONFIG_MK1OM
	for (i = 0; i < MAX_BOARD_SUPPORTED; i++) {
		if (fd_data->mem_ctx[i]) {
			if ((err = __micmem_closedev(fd_data, i))) {
				printk(KERN_ERR "Did not cleanly close device %d", i);
				BUG();
			}
		}
	}
	micmem_cleanup_pinnings(fd_data);
#endif /* CONFIG_MK1OM */
	kfree(fd_data);
	return 0;
}
