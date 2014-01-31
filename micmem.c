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
 
#include "mic/micmem.h"
 
#define DMA_TO  (5 * HZ)

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
static inline int do_chunk_dma(struct dma_channel *ch, uint64_t src_pa,
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

static inline int wait_for_dma(struct dma_channel *ch, int cookie,
		unsigned long start) {
	while (1 != poll_dma_completion(cookie, ch)) {
		cpu_relax();
		if (time_after(jiffies, start + DMA_TO)) {
			printk("DMA timed out\n");
			return -EBUSY;
		}
	}
	return 0;
}

/**
 * do_dev2host:
 * Synchronously transfer memory from device to host.
 *
 * @card_pa:    source address on the device. Must be aligned to PAGE_SIZE.
 * @mem_range:  destination memory range
 * @offset: offset inside @mem_range. TODO: not implemented, must be set to 0
 * @size:   size inside @mem_rangs. TODO: currently ignored
 *
 * Returns 0 on success.
*/
static int do_dev2host(struct micmem_ctx *mem_ctx,
		uint64_t card_pa, struct dma_mem_range *mem_range,
		uint64_t offset, uint64_t size)
{
	int result, cookie;
	int i;
	unsigned long ts;
	struct dma_channel *ch = mem_ctx->h2d_ch;
	
	if (offset != 0)
		return -EINVAL; // TODO: not implemented

	result = request_dma_channel(ch);
	if (unlikely(result)) {
		return result;
	}
	/* do dma and keep polling for completion */
	for (i = 0; i < mem_range->nr_contig_chunks - 1; i++) {
		result = do_chunk_dma(ch, card_pa, mem_range->dma_addr[i],
				mem_range->num_pages[i] << PAGE_SHIFT, NULL);
		card_pa += mem_range->num_pages[i] << PAGE_SHIFT;
		if (unlikely(result < 0)) {
			free_dma_channel(ch);
			return result;
		}
	}
	/* last iteration will be used for polling. i == nr_contig_chunks - 1 */
	result = do_chunk_dma(ch, card_pa, mem_range->dma_addr[i],
			mem_range->num_pages[i] << PAGE_SHIFT, &cookie);
	free_dma_channel(ch);
	if (unlikely(result < 0))
		return result;

	ts = jiffies;
	if (unlikely(result = wait_for_dma(ch, cookie, ts)))
		return result;

	return 0;
}

/**
 * do_host2dev:
 * Synchronously transfer memory from host to device.
 *
 * @mem_range:  source memory range
 * @offset: offset inside @mem_range. TODO: not implemented, must be set to 0
 * @card_pa:    destination address on the device. Must be aligned to PAGE_SIZE.
 * @size:   size inside @mem_rangs. TODO: currently ignored
 *
 * Returns 0 on success.
*/
static int do_host2dev(struct micmem_ctx *mem_ctx,
		struct dma_mem_range *mem_range, uint64_t offset,
		uint64_t card_pa, uint64_t size)
{
	int result, cookie;
	int i;
	unsigned long ts;
	struct dma_channel *ch = mem_ctx->d2h_ch;
	
	if (offset != 0)
		return -EINVAL; // TODO: not implemented

	result = request_dma_channel(ch);
	if (unlikely(result)) {
		return result;
	}
	/* do dma and keep polling for completion */
	for (i = 0; i < mem_range->nr_contig_chunks - 1; i++) {
		result = do_chunk_dma(ch, mem_range->dma_addr[i], card_pa,
				mem_range->num_pages[i] << PAGE_SHIFT, NULL);
		card_pa += mem_range->num_pages[i] << PAGE_SHIFT;
		if (unlikely(result < 0)) {
			free_dma_channel(ch);
			return result;
		}
	}
	/* last iteration will be used for polling. i == nr_contig_chunks - 1 */
	result = do_chunk_dma(ch, mem_range->dma_addr[i], card_pa,
			mem_range->num_pages[i] << PAGE_SHIFT, &cookie);
	free_dma_channel(ch);
	if (unlikely(result < 0))
		return result;

	ts = jiffies;
	if (unlikely(result = wait_for_dma(ch, cookie, ts)))
		return result;
		
	return 0;
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
 * Altered copy of map_virt_into_aperture.
 */
static int
do_map_virt_into_aperture(mic_ctx_t *mic_ctx, phys_addr_t *out_offset,
		void *local, size_t size)
{
	int bid;
	struct pci_dev *hwdev = mic_ctx->bi_pdev;

	bid = mic_ctx->bi_id; // TODO: is this really board id?
	hwdev = mic_ctx->bi_pdev;
	*out_offset = mic_map_single(bid, hwdev, local, size);
	if (mic_map_error(*out_offset)) {
		*out_offset = 0;
		return -ENOMEM;
	}

	return 0;
}


/*
 * Unmaps the host memory described by dev PA @local and @size from the device
 * aperture.
 */
static inline void
do_unmap_from_aperture(mic_ctx_t *mic_ctx, phys_addr_t local, size_t size)
{
	mic_ctx_unmap_single(mic_ctx, local, size);
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
		struct scif_pinned_pages *pinned_pages,
		struct dma_mem_range **out_mem_range)
{
	int j, i, err = 0;
	int nr_pages = pinned_pages->nr_pages;
	int nr_contig_chunks = pinned_pages->nr_contig_chunks;
	struct dma_mem_range *mem_range;
	
	might_sleep();
	
	if (!(mem_range = scif_zalloc(sizeof(*mem_range))))
		return -ENOMEM;
	if (!(mem_range->dma_addr = scif_zalloc(nr_contig_chunks *
			sizeof(*(mem_range->dma_addr))))) {
		err = -ENOMEM;
		goto error_free_range;
	}

	if (!(mem_range->num_pages = scif_zalloc(nr_contig_chunks *
			sizeof(*(mem_range->num_pages))))) {
		err = -ENOMEM;
		goto error_free_range;
	}
	mem_range->pinned_pages = pinned_pages;
	mem_range->nr_contig_chunks = nr_contig_chunks;
	mem_range->size = pinned_pages->nr_pages << PAGE_SHIFT;

	for (j = 0, i = 0; j < nr_contig_chunks; j++, i += nr_pages) {
		nr_pages = pinned_pages->num_pages[i];
		err = do_map_virt_into_aperture(mic_ctx,
			&mem_range->dma_addr[j],
			phys_to_virt(page_to_phys(pinned_pages->pages[i])),
			((size_t)nr_pages) << PAGE_SHIFT);
		if (err)
			goto error_free_range;
		mem_range->num_pages[j] = nr_pages;
	}
	*out_mem_range = mem_range;
	return 0;
	
error_free_range:
	if (mem_range->num_pages)
		scif_free(mem_range->num_pages,
			nr_contig_chunks * sizeof(*(mem_range->num_pages)));
	if (mem_range->dma_addr)
		scif_free(mem_range->dma_addr,
			nr_contig_chunks * sizeof(*(mem_range->dma_addr)));
	scif_free(mem_range, sizeof(*mem_range));
	return err;
}

/**
 * do_unmap_range_pages:
 * Unmaps memory range from device apertue/PCI and frees @mem_range
 */
static int do_unmap_range_pages(mic_ctx_t *mic_ctx,
		struct dma_mem_range *mem_range)
{
	int j;
	might_sleep();

	for (j = 0; j < mem_range->nr_contig_chunks; j++) {
		if (mem_range->dma_addr[j]) {
			do_unmap_from_aperture(mic_ctx, mem_range->dma_addr[j],
				mem_range->num_pages[j] << PAGE_SHIFT);
		}
	}
	scif_free(mem_range->dma_addr, mem_range->nr_contig_chunks *
			sizeof(*(mem_range->dma_addr)));
	scif_free(mem_range->num_pages, mem_range->nr_contig_chunks *
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

	status = micpm_get_reference(mic_ctx, true);
	if (status)
		return status;
	
	/* FIXME: Figure out initialization
	if ((status = open_dma_device(mic_ctx->bi_id + 1,
				   mic_ctx->mmio.va + HOST_SBOX_BASE_ADDRESS,
				   &mic_ctx->dma_handle)))
		goto put_ref;
	*/
	
	status = do_reserve_dma_chan(mic_ctx, &d2h_ch);
	if (status)
		goto close_dev;

	status = do_reserve_dma_chan(mic_ctx, &h2d_ch);
	if (status)
		goto close_dev;

	mem_ctx->mic_ctx = mic_ctx;
	mem_ctx->d2h_ch = d2h_ch;
	mem_ctx->h2d_ch = h2d_ch;
	return 0;
	
close_dev:
	/* FIXME: init
	close_dma_device(mic_ctx->bi_id + 1, &mic_ctx->dma_handle);
put_ref:
	*/
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
	/* FIXME: init
	close_dma_device(mic_ctx->bi_id + 1,
					 &mic_ctx->dma_handle);
	*/
	micpm_put_reference(mic_ctx);
	/* XXX: Does the memory channel need to be "unreserved"? Or is the
	 * "reserve" function needed only in order to fill in dma_chan struct
	 * for later use?
	 */
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
int micmem_map_range(mic_ctx_t *mic_ctx, void *host_mem, uint64_t len,
		struct dma_mem_range **out_range)
{
	struct scif_pinned_pages *pinned_pages;
	int status;
	uint64_t aligned_len = ALIGN(len, PAGE_SIZE);
	
	if ((status = scif_pin_pages((void *)((uint64_t)host_mem & PAGE_MASK),
			aligned_len, SCIF_PROT_READ | SCIF_PROT_WRITE, 0,
			&pinned_pages)))
		return status;

	if ((status = do_map_range_pages(mic_ctx, pinned_pages, out_range))) {
		scif_unpin_pages(pinned_pages);
		return status;
	}
	return 0;
}

/**
 * micmem_unmap_range:
 * Unmaps and unpins host memory mapped with descriptor @mem_range.
 */
void micmem_unmap_range(mic_ctx_t *mic_ctx, struct dma_mem_range *mem_range)
{
	struct scif_pinned_pages* pinned_pages = mem_range->pinned_pages;
	do_unmap_range_pages(mic_ctx, mem_range);
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
 */
int micmem_dev2host(struct micmem_ctx *mem_ctx,
		struct dma_mem_range *dest_mem_range, uint64_t range_offset,
		uint64_t source_dev, uint64_t size)
{
	if (range_offset + size > dest_mem_range->size) {
		printk(KERN_ERR "Transfer exceeds specified memory range\n");
		return -EINVAL;
	}

	return do_dev2host(mem_ctx, source_dev, dest_mem_range, range_offset,
			size);
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
 */
int micmem_host2dev(struct micmem_ctx *mem_ctx, uint64_t dest_dev,
		struct dma_mem_range *src_mem_range, uint64_t range_offset,
		uint64_t size)
{
	if (range_offset + size > src_mem_range->size) {
		printk(KERN_ERR "Transfer exceeds specified memory range\n");
		return -EINVAL;
	}

	return do_host2dev(mem_ctx, src_mem_range, range_offset, dest_dev,
			size);
}
