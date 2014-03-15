/*
 * Copyright (C) 2014 PathScale Inc. All Rights Reserved.
 */

/* Micmem is a collection of functions allowing fast DMA access to card memory.
 * They are used best without full MPSS stack running, and without an OS present
 * on the device side. By design, these functions reserve several DMA channels
 * for their exclusive use. Using this functionality together with a full MPSS
 * stack may lead to exhaustion of available DMA channels.
 * This code has been tested to work on Knight's Corner devices only.
 * Not threadsafe.
 */

#ifdef CONFIG_MK1OM

#include "mic/micmem.h"
#include "mic/mic_sbox_md.h"

#define DMA_TO  (5 * HZ)
#define SBOX_OFFSET	0x10000

typedef enum dma_dir {
	DEV2HOST,
	HOST2DEV
} dma_dir_t;

/**
 * do_chunk_dma:
 * Requests transfer of a single contiguous chunk of memory via DMA.
 *
 * @card_pa, @host_pa and @size must be aligned to PAGE_SIZE.
 * @ch must be pre-acquired.
 * If @cookie is not NULL, its value will be filled with a DMA cookie suitable
 * for poll_dma_completion call.
 *
 * Returns 0 on success.
 */
static int do_chunk_dma(struct dma_channel *ch, uint64_t src_pa,
		uint64_t dst_pa, uint64_t size, int *cookie)
{
	/* This function is explicitly inlined because it's performance critical
	 * and used from multiple places. */
	int flags = 0;
	int result;
	if (cookie)
		flags |= DO_DMA_POLLING;

	result = do_dma(ch, flags, src_pa, dst_pa, size, NULL);
	if (unlikely(0 > result)) {
		printk("Error programming the dma descriptor\n");
		return result;
	}
	if (cookie)
		*cookie = result;
	return 0;
}

/* helper function to make do_xfer more readable.
 * This piece of code assumes dma_addr_t <= uint64_t to comply with do_dma
 * requirements. */
static int do_chunk_dma_dir(struct dma_channel *ch, uint64_t dev_pa,
		dma_addr_t host_pa, uint64_t size, int *cookie,
		dma_dir_t direction)
{
	uint64_t src;
	uint64_t dst;
	if (direction == HOST2DEV) {
		src = (uint64_t)host_pa;
		dst = dev_pa;
	} else {
		src = dev_pa;
		dst = (uint64_t)host_pa;
	}
	return do_chunk_dma(ch, src, dst, size, cookie);
}

static inline int wait_for_dma(struct dma_channel *ch, int cookie,
		unsigned long start)
{
	while (1 != poll_dma_completion(cookie, ch)) {
		cpu_relax();
		if (time_after(jiffies, start + DMA_TO)) {
			printk(KERN_ERR "DMA timed out\n");
			return -EBUSY;
		}
	}
	return 0;
}

static void find_1st_chunk(struct dma_mem_range *mem_range, uint64_t offset,
		uint64_t *out_chunk_idx, uint64_t *out_chunk_offset,
		uint64_t *out_chunk_size)
{
	int i;
	uint64_t chunk_size = 0;
	uint64_t chunk_offset = offset;
	for (i = 0; i < mem_range->nr_contig_chunks; i++) {
		chunk_size = mem_range->num_pages[i] << PAGE_SHIFT;
		if (chunk_size > chunk_offset) {
			*out_chunk_idx = i;
			break;
		}
		chunk_offset -= chunk_size;
	}
	*out_chunk_offset = chunk_offset;
	*out_chunk_size = chunk_size;
	/* out_chunk_idx stays the same if loop finishes normally; this means
	 * transfer was requested starting exactly at the end of mem range and
	 * is invalid anyway.
	 */
}

static int xfer_single_finish(struct dma_channel *ch, dma_addr_t chunk_pa,
		uint64_t chunk_offset, uint64_t card_pa,
		uint64_t remaining_size, dma_dir_t direction)
{
	int result;
	int cookie;
	unsigned long ts;
	result = do_chunk_dma_dir(ch, card_pa, chunk_pa + chunk_offset,
			remaining_size, &cookie, direction);
	free_dma_channel(ch);
	if (unlikely(result < 0))
		return result;

	ts = jiffies;
	if (unlikely(result = wait_for_dma(ch, cookie, ts)))
		return result;

	return 0;
}

/**
 * do_xfer_single:
 * Synchronously transfer memory in requested direction.
 *
 * @mem_ctx:	memory context to use for the transfer
 * @card_pa:	address on the device. Must be aligned to PAGE_SIZE.
 * @mem_range:	memory range on host
 * @offset:	offset inside @mem_range
 * @size:	size inside @mem_rangs
 * @direction:	direction of transfer
 *
 * Returns 0 on success.
 */
static int do_xfer_single(struct micmem_ctx *mem_ctx, uint64_t card_pa,
		struct dma_mem_range *mem_range, uint64_t offset, uint64_t size,
		dma_dir_t direction)
{
	int result;
	/* i iterates chunk count, which is defined as int64_t in
	 * scif_pinned_pages.nr_contig_chunks */
	uint64_t i;
	struct dma_channel *ch;
	uint64_t chunk_offset;
	uint64_t chunk_idx = 0;
	uint64_t chunk_size;
	uint64_t chunk_remaining;
	uint64_t remaining_size = size;

	/* Channel could be passed in some struct chan_info to gain a bit of
	 * flexibility and not keep function prototypes the same as
	 * multi-channel transfer. Better not increase complexity for such a
	 * corner case -- hardcoded channels. */
	if (direction == HOST2DEV)
		ch = mem_ctx->h2d_ch;
	else
		ch = mem_ctx->d2h_ch;

	result = request_dma_channel(ch);
	if (unlikely(result))
		return result;

	find_1st_chunk(mem_range, offset, &chunk_idx, &chunk_offset,
			&chunk_size);

	if (chunk_offset + remaining_size <= chunk_size)
		goto last_chunk;

	/* do dma for inside chunks
	 * XXX: is it worth the effort to place anything outside this loop to
	 * optimize for speed? the loop/wait should be IO-bound and dma is async
	 * anyway... */
	for (i = chunk_idx; i < mem_range->nr_contig_chunks - 1; i++) {
		chunk_size = mem_range->num_pages[i] << PAGE_SHIFT;
		chunk_remaining = chunk_size - chunk_offset;
		if (chunk_remaining >= remaining_size)
			break;

		result = do_chunk_dma_dir(ch, card_pa,
				mem_range->dma_addr[i] + chunk_offset,
				chunk_remaining, NULL, direction);

		if (unlikely(result < 0)) {
			free_dma_channel(ch);
			return result;
		}
		card_pa += chunk_remaining;
		remaining_size -= chunk_remaining;
		chunk_offset = 0;
	}
	chunk_idx = i;

last_chunk:
	/* Last dma request will be used for polling. DMA requests are stored in
	 * a queue, so the only thing lost are errors from previous requests. */
	return xfer_single_finish(ch, mem_range->dma_addr[chunk_idx],
		chunk_offset, card_pa, remaining_size, direction);
}

/** xfer_dual_finish:
 * helper function performing the last 2 dma requests of dual-channel transfer
 *
 * @chunk_idx:	index of the 1st of the 2 chunks to be transferred
 */
static int xfer_dual_finish(struct dma_channel *ch, struct dma_channel *ch2,
		struct dma_mem_range *mem_range, uint64_t chunk_idx,
		uint64_t chunk_offset, uint64_t remaining, uint64_t card_pa,
		dma_dir_t direction)
{
	int cookie, cookie2;
	unsigned long ts;
	int result;
	uint64_t chunk_remaining;

	chunk_remaining = (mem_range->num_pages[chunk_idx] << PAGE_SHIFT) - \
		chunk_offset;
	result = do_chunk_dma_dir(ch, card_pa,
			mem_range->dma_addr[chunk_idx] + chunk_offset,
			chunk_remaining, &cookie, direction);
	free_dma_channel(ch);
	if (unlikely(result < 0)) {
		free_dma_channel(ch2);
		return result;
	}
	remaining -= chunk_remaining;
	card_pa += chunk_remaining;

	result = do_chunk_dma_dir(ch2, card_pa,
			mem_range->dma_addr[chunk_idx + 1], remaining, &cookie2,
			direction);
	free_dma_channel(ch2);
	if (unlikely(result < 0))
		return result;

	/* If transfer on the 1st channel times out, there's no need to wait for
	 * the 2nd channel. 1st channel is left blocked for an indefinite time
	 * in this case, 2nd channel may stay blocked as well. */
	ts = jiffies;
	if (unlikely(result = wait_for_dma(ch, cookie, ts)))
		return result;

	if (unlikely(result = wait_for_dma(ch2, cookie, ts)))
		return result;

	return 0;
}

/**
 * do_xfer_dual:
 * Synchronously transfer memory in requested direction, using two dma channels.
 *
 * @mem_ctx:	memory context to use for the transfer
 * @card_pa:	address on the device. Must be aligned to PAGE_SIZE.
 * @mem_range:	memory range on host
 * @offset:	offset inside @mem_range
 * @size:	size inside @mem_rangs
 * @direction:	direction of transfer
 *
 * Returns 0 on success.
 */
static int do_xfer_dual(struct micmem_ctx *mem_ctx, uint64_t card_pa,
		struct dma_mem_range *mem_range, uint64_t offset, uint64_t size,
		dma_dir_t direction)
{
	int result;
	/* i iterates chunk count, which is defined as int64_t in
	 * scif_pinned_pages.nr_contig_chunks */
	uint64_t i;
	struct dma_channel *ch, *ch2;
	struct dma_channel *cur_ch;
	uint64_t chunk_offset;
	uint64_t chunk_idx = 0;
	uint64_t chunk_size;
	uint64_t next_chunk_size;
	uint64_t chunk_remaining;
	uint64_t remaining_size = size;

	/* Channel could be passed in some struct chan_info to gain a bit of
	 * flexibility and not keep function prototypes the same as
	 * multi-channel transfer. Better not increase complexity for such a
	 * corner case -- hardcoded channels. */
	if (direction == HOST2DEV) {
		ch = mem_ctx->h2d_ch;
		ch2 = mem_ctx->h2d_ch2;
	} else {
		ch = mem_ctx->d2h_ch;
		ch2 = mem_ctx->d2h_ch2;
	}

	result = request_dma_channel(ch);
	if (unlikely(result))
		return result;

	result = request_dma_channel(ch2);
	if (unlikely(result)) {
		free_dma_channel(ch);
		return result;
	}

	find_1st_chunk(mem_range, offset, &chunk_idx, &chunk_offset,
			&chunk_size);

	if (chunk_offset + remaining_size <= chunk_size) {
		free_dma_channel(ch2);
		/* TODO: Split large transfers into pairs of smaller ones */
		return xfer_single_finish(ch, mem_range->dma_addr[chunk_idx],
			chunk_offset, card_pa, remaining_size, direction);
	}
	/* At least 2 chunks guaranteed to be in need of transferring thanks to
	 * the above check. */

	chunk_remaining = chunk_size - chunk_offset;
	/* do dma for inside chunks, analyzing chunks in pairs
	 * XXX: is it worth the effort to place anything outside this loop to
	 * optimize for speed? the loop/wait should be IO-bound and dma is async
	 * anyway... */
	for (i = chunk_idx; i < mem_range->nr_contig_chunks - 2; i++) {
		next_chunk_size = mem_range->num_pages[i + 1] << PAGE_SHIFT;
		if (chunk_remaining + next_chunk_size >= remaining_size)
			break;

		if (i % 2)
			cur_ch = ch;
		else
			cur_ch = ch2;

		result = do_chunk_dma_dir(cur_ch, card_pa,
				mem_range->dma_addr[i] + chunk_offset,
				chunk_remaining, NULL, direction);

		if (unlikely(result < 0)) {
			free_dma_channel(ch);
			free_dma_channel(ch2);
			return result;
		}

		card_pa += chunk_remaining;
		remaining_size -= chunk_remaining;

		chunk_offset = 0;
		chunk_remaining = next_chunk_size;
		chunk_size = next_chunk_size;
	}
	chunk_idx = i;

	/* Last dma requests will be used for polling. DMA requests are stored
	 * in a queue, so the only thing lost are errors from previous requests.
	 */
	return xfer_dual_finish(ch, ch2, mem_range, chunk_idx, chunk_offset,
		remaining_size, card_pa, direction);
}

/**
 * do_xfer:
 * Chooses the number of channels to use for actual transfer based on @flags
 * value, performs bounds checking.
 */
static inline int do_xfer(struct micmem_ctx *mem_ctx, uint64_t card_pa,
		struct dma_mem_range *mem_range, uint64_t offset, uint64_t size,
		dma_dir_t direction, int flags)
{
	if (offset + size > mem_range->size) {
		printk(KERN_ERR "Transfer exceeds specified memory range:" \
			"requested %llxb @%llx, ends at %llx.\n",
			(long long unsigned int)size,
			(long long unsigned int)offset,
			(long long unsigned int)mem_range->size);
		return -EINVAL;
	}

	if (flags == MICMEM_SINGLE)
		return do_xfer_single(mem_ctx, card_pa, mem_range, offset, size,
			direction);
	else if (flags == MICMEM_DUAL)
		return do_xfer_dual(mem_ctx, card_pa, mem_range, offset, size,
			direction);
	else if (flags == MICMEM_AUTO)
		return do_xfer_single(mem_ctx, card_pa, mem_range, offset, size,
			direction); // FIXME: dual has no speed advantage
	else
		return -EINVAL;
}

/**
 * do_reserve_dma_chan:
 *
 * Reserves a DMA channel for a particular device.
 */
static int do_reserve_dma_chan(mic_ctx_t *mic_ctx, struct dma_channel **chan)
{
	int err = 0;
	unsigned long ts = jiffies;
	while (true) {
		if (!(err = allocate_dma_channel(mic_ctx->dma_handle, chan)))
			break;
		schedule();
		if (time_after(jiffies,
			ts + NODE_ALIVE_TIMEOUT)) {
			return -EBUSY;
		}
	}
	mic_dma_thread_free_chan(*chan);
	return err;
}


/**
 * do_map_virt_into_aperture:
 *
 * Maps the memory described by @local VA and @size to the device aperture and
 * returns the corresponding device physical address in @out_offset.
 */
static int
do_map_virt_into_aperture(mic_ctx_t *mic_ctx, phys_addr_t *out_offset,
		void *virt_addr, size_t size)
{
	int bid;
	struct pci_dev *hwdev = mic_ctx->bi_pdev;

	bid = mic_ctx->bi_id;
	hwdev = mic_ctx->bi_pdev;
	*out_offset = mic_map_single(bid, hwdev, virt_addr, size);
	if (mic_map_error(*out_offset)) {
		*out_offset = 0;
		return -ENOMEM;
	}

	return 0;
}


/**
 * do_unmap_from_aperture:
 *
 * Unmaps the host memory described by dev PA @local and @size from the device
 * aperture.
 */
static inline void
do_unmap_from_aperture(mic_ctx_t *mic_ctx, phys_addr_t local, size_t size)
{
	mic_ctx_unmap_single(mic_ctx, local, size);
}

/* forward declaration */
static int do_unmap_range_pages(mic_ctx_t *mic_ctx,
		struct dma_mem_range *mem_range);

/* helper: appends chunk to the end of a dma_mem_range */
static inline int range_add_chunk(mic_ctx_t *mic_ctx,
		struct dma_mem_range *mem_range, phys_addr_t addr, int pages)
{
	int err;
	int idx = mem_range->nr_contig_chunks;
	err = do_map_virt_into_aperture(mic_ctx, &mem_range->dma_addr[idx],
		phys_to_virt(addr), (size_t)pages << PAGE_SHIFT);
	if (err)
		return err;
	mem_range->num_pages[idx] = pages;
	mem_range->nr_contig_chunks++;
	return 0;
}

/**
 * init_coalesce_range_pages:
 *
 * Fills in mem_range structure with physical addresses from its pinned_pages,
 * coalescing arrays of contiguous chunks into single ones along the way.
 *
 * @mic_ctx:	context to device
 * @mem_range:	range to initialize
 * @offset:	offset into pinned area associated with @mem_range
 * @len:	length of mapped area
 */
static int init_coalesce_range_pages(mic_ctx_t *mic_ctx,
		struct dma_mem_range *mem_range, uint64_t offset, uint64_t len)
{
	struct scif_pinned_pages *pinned_pages = mem_range->pinned_pages;
	int err;
	int i, j;
	int nr_pages;
	phys_addr_t cur_addr;
	int first_page = offset >> PAGE_SHIFT;
	int last_page = first_page + (len >> PAGE_SHIFT);

	if (last_page > pinned_pages->nr_pages)
		return -EINVAL;

	mem_range->size = len;

	for (j = 0, i = 0; j < pinned_pages->nr_contig_chunks;
			j++, i += nr_pages) {
		cur_addr = page_to_phys(pinned_pages->pages[i]);
		nr_pages = pinned_pages->num_pages[i];

		if (first_page >= i + nr_pages)
			continue;

		if (first_page > i)
			cur_addr += (first_page - i) << PAGE_SHIFT;
		if (last_page < i + nr_pages)
			nr_pages = last_page - i;
		err = range_add_chunk(mic_ctx, mem_range, cur_addr, nr_pages);
		if (err)
			return err;
		if (last_page == i + nr_pages)
			break;
	}
	return 0;
}

/**
 * do_map_range_pages:
 *
 * Map pinned pages into the device aperture/PCI.
 * Also compute physical addresses required for DMA.
 *
 * @out_mem_range is allocated and filled in in this function.
 */
static int do_map_range_pages(mic_ctx_t *mic_ctx,
		struct scif_pinned_pages *pinned_pages, uint64_t offset,
		uint64_t len, struct dma_mem_range **out_mem_range)
{
	int err = 0;
	int max_contig_chunks = pinned_pages->nr_contig_chunks;
	struct dma_mem_range *mem_range;

	might_sleep();

	if (!(mem_range = scif_zalloc(sizeof(*mem_range))))
		return -ENOMEM;

	/* dma_addr and num_pages are indexed up to mem_range->nr_contig_chunks,
	 * but allocated using pinned_pages->nr_contig_chunks as the upper limit
	 * on number of chunks which is calculated while coalescing.
	 */
	if (!(mem_range->dma_addr = scif_zalloc(max_contig_chunks *
			sizeof(*(mem_range->dma_addr))))) {
		err = -ENOMEM;
		goto error_free_range;
	}
	if (!(mem_range->num_pages = scif_zalloc(max_contig_chunks *
			sizeof(*(mem_range->num_pages))))) {
		err = -ENOMEM;
		goto error_free_range;
	}

	mem_range->pinned_pages = pinned_pages;

	if ((err = init_coalesce_range_pages(mic_ctx, mem_range, offset, len))) {
		do_unmap_range_pages(mic_ctx, mem_range);
		return err;
	}

	*out_mem_range = mem_range;
	return 0;

error_free_range:
	if (mem_range->num_pages)
		scif_free(mem_range->num_pages,
			max_contig_chunks * sizeof(*(mem_range->num_pages)));
	if (mem_range->dma_addr)
		scif_free(mem_range->dma_addr,
			max_contig_chunks * sizeof(*(mem_range->dma_addr)));
	scif_free(mem_range, sizeof(*mem_range));
	return err;
}

/**
 * do_unmap_range_pages:
 * Unmaps memory range from device apertue/PCI and frees @mem_range.
 */
static int do_unmap_range_pages(mic_ctx_t *mic_ctx,
		struct dma_mem_range *mem_range)
{
	int j;
	might_sleep();

	for (j = 0; j < mem_range->nr_contig_chunks; j++) {
		/* check for presence of chunk in not fully initialized range */
		if (mem_range->dma_addr[j]) {
			do_unmap_from_aperture(mic_ctx, mem_range->dma_addr[j],
				mem_range->num_pages[j] << PAGE_SHIFT);
		}
	}

	/* dma_addr and num_pages were allocated using
	 * pinned_pages->nr_contig_chunks, before mem_range->nr_contig_chunks
	 * was known. */
	scif_free(mem_range->dma_addr,
			mem_range->pinned_pages->nr_contig_chunks *
			sizeof(*(mem_range->dma_addr)));
	scif_free(mem_range->num_pages,
			mem_range->pinned_pages->nr_contig_chunks *
			sizeof(*(mem_range->num_pages)));
	scif_free(mem_range, sizeof(*mem_range));
	return 0;
}

/**
 * micmem_get_mem_ctx:
 * Initializes device and fills in memory context for a device given its
 * @mic_ctx
 */
int micmem_get_mem_ctx(mic_ctx_t *mic_ctx, struct micmem_ctx *mem_ctx)
{
	int status;
	struct dma_channel *d2h_ch;
	struct dma_channel *h2d_ch;
	struct dma_channel *d2h_ch2;
	struct dma_channel *h2d_ch2;

	status = micpm_get_reference(mic_ctx, true);
	if (status)
		return status;

	/* FIXME: Assuming uOS has been booted, DCR was reset and must be restored
	 * before using DMA.
	 */
	/* Change ownership of channels 0-5 to host and enable 0-6.
	 * TODO: why doesn't regular boot enable all?
	 */
	// mic_sbox_write_mmio(mic_ctx->mmio.va, SBOX_OFFSET + SBOX_DCR, 0x00001555);
	if ((status = open_dma_device(mic_ctx->bi_id + 1,
			mic_ctx->mmio.va + HOST_SBOX_BASE_ADDRESS,
			&mic_ctx->dma_handle)))
		goto put_ref;

	status = do_reserve_dma_chan(mic_ctx, &d2h_ch);
	if (status)
		goto close_dev;

	status = do_reserve_dma_chan(mic_ctx, &h2d_ch);
	if (status)
		goto close_dev;

	status = do_reserve_dma_chan(mic_ctx, &d2h_ch2);
	if (status)
		goto close_dev;

	status = do_reserve_dma_chan(mic_ctx, &h2d_ch2);
	if (status)
		goto close_dev;

	mem_ctx->d2h_ch2 = d2h_ch2;
	mem_ctx->h2d_ch2 = h2d_ch2;

	mem_ctx->d2h_ch = d2h_ch;
	mem_ctx->h2d_ch = h2d_ch;
	mem_ctx->mic_ctx = mic_ctx;
	return 0;

close_dev:
	// FIXME: deinit
	close_dma_device(mic_ctx->bi_id + 1, &mic_ctx->dma_handle);
put_ref:
	micpm_put_reference(mic_ctx);
	return status;
}

/**
 * micmem_destroy_mem_ctx:
 * Invalidates the memory context.
 *
 * @mem_ctx contents are not altered in any way, it needs to be freed manually.
 */
void micmem_destroy_mem_ctx(struct micmem_ctx *mem_ctx)
{
	mic_ctx_t *mic_ctx = mem_ctx->mic_ctx;
	/* FIXME: deinit -- the deinit sequence is not documented. Reset magic or
	power cycling may be required.
	*/
	printk(KERN_ERR "Card released, reboot may be required\n");
	/*close_dma_device(mic_ctx->bi_id + 1,
					 &mic_ctx->dma_handle);
	*/
	micpm_put_reference(mic_ctx);
	/* XXX: Does the memory channel need to be "unreserved"? Or is the
	 * "reserve" function needed only in order to fill in dma_chan struct
	 * for later use?
	 */
}

/**
 * micmem_pin_range:
 * Pins memory range in physical memory.
 *
 * Parameters must be page-aligned.
 */
int micmem_pin_range(void *host_vm, uint64_t len,
		struct scif_pinned_pages **pinned_pages)
{
	return scif_pin_pages(host_vm, len, SCIF_PROT_READ | SCIF_PROT_WRITE, 0,
		pinned_pages);
}

/**
 * micmem_map_range:
 * Prepares host memory range to use with DMA.
 *
 * Virtual memory given by @host_mem and @len is pinned and mapped to device
 * described by @mic_ctx.
 * Resulting memory descriptor is returned in @out_range.
 * TODO: put mic_ctx_t into dma_mem_range when mapping to the particular device?
 */
int micmem_map_range(mic_ctx_t *mic_ctx,
		struct scif_pinned_pages *pinned_pages, uint64_t offset,
		uint64_t len, struct dma_mem_range **out_range)
{
	if (offset + len > pinned_pages->nr_pages << PAGE_SHIFT) {
		printk(KERN_ERR "Mapping request exceeds pinned range size.\n");
		return -EINVAL;
	}

	return do_map_range_pages(mic_ctx, pinned_pages, offset, len,
		out_range);
}

/**
 * micmem_unmap_range:
 * Unmaps and unpins host memory mapped with descriptor @mem_range.
 */
void micmem_unmap_range(mic_ctx_t *mic_ctx, struct dma_mem_range *mem_range)
{
	do_unmap_range_pages(mic_ctx, mem_range);
}

void micmem_unpin_range(struct scif_pinned_pages *pinned_pages)
{
	scif_unpin_pages(pinned_pages); /* TODO: print out value if bugs out */
}

/**
 * micmem_dev2host:
 * Transfers memory from host to device.
 *
 * @mem_ctx: device context to transfer from
 * @dest_mem_range: memory range mapped to the same device as mem_ctx
 * @range_offset: offset inside the memory range to transfer to
 * @source_dev: device physical address used as source
 * @size: transfer size
 * @flags: determines the number of channels to use for this transfer
 */
int micmem_dev2host(struct micmem_ctx *mem_ctx,
		struct dma_mem_range *dest_mem_range, uint64_t range_offset,
		uint64_t source_dev, uint64_t size, int flags)
{
	return do_xfer(mem_ctx, source_dev, dest_mem_range, range_offset, size,
		DEV2HOST, flags);
}

/**
 * micmem_host2dev:
 * Transfers memory from host to device.
 *
 * @mem_ctx: device context to transfer to
 * @dest_dev: device physical address used as destination
 * @src_mem_range: memory range mapped to the same device as mem_ctx
 * @range_offset: offset inside the memory range to transfer from
 * @size: transfer size
 * @flags: determines the number of channels to use for this transfer
 */
int micmem_host2dev(struct micmem_ctx *mem_ctx, uint64_t dest_dev,
		struct dma_mem_range *src_mem_range, uint64_t range_offset,
		uint64_t size, int flags)
{
	return do_xfer(mem_ctx, dest_dev, src_mem_range, range_offset, size,
		HOST2DEV, flags);
}

#endif /* CONFIG_MK1OM */
