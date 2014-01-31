#include "mic_common.h"

/* vnet/mic_shutdown/hvc/virtio */
#define VNET_SBOX_INT_IDX	0
#define MIC_SHT_SBOX_INT_IDX	1
#define HVC_SBOX_INT_IDX	2
#define VIRTIO_SBOX_INT_IDX	3
#define PM_SBOX_INT_IDX		4

#define MIC_BSP_INTERRUPT_VECTOR 229	// Host->Card(bootstrap) Interrupt Vector#
/*
 * Current usage of MIC interrupts:
 * APICICR1 - mic shutdown interrupt
 * APCICR0 - rest
 *
 * Planned Usage:
 * SCIF - rdmasrs
 * vnet/hvc/virtio - APICICR0
 * mic shutdown interrupt - APICICR1
 */
static void __mic_send_intr(mic_ctx_t *mic_ctx, int i)
{
	uint32_t apicicr_low;
	uint64_t apic_icr_offset = SBOX_APICICR0 + i * 8;

	apicicr_low = SBOX_READ(mic_ctx->mmio.va, apic_icr_offset);
	/* for KNC we need to make sure we "hit" the send_icr bit (13) */
	if (mic_ctx->bi_family == FAMILY_KNC)
		apicicr_low = (apicicr_low | (1 << 13));

	/* MIC card only triggers when we write the lower part of the
	 * address (upper bits)
	 */
	SBOX_WRITE(apicicr_low, mic_ctx->mmio.va, apic_icr_offset);
}

static inline void mic_send_vnet_intr(mic_ctx_t *mic_ctx)
{
	__mic_send_intr(mic_ctx, VNET_SBOX_INT_IDX);
}

static inline void mic_send_hvc_intr(mic_ctx_t *mic_ctx)
{
	__mic_send_intr(mic_ctx, HVC_SBOX_INT_IDX);
}

static inline void mic_send_scif_intr(mic_ctx_t *mic_ctx)
{
	__mic_send_intr(mic_ctx, 0);
}

static inline void mic_send_virtio_intr(mic_ctx_t *mic_ctx)
{
	__mic_send_intr(mic_ctx, VIRTIO_SBOX_INT_IDX);
}

static inline void mic_send_sht_intr(mic_ctx_t *mic_ctx)
{
	__mic_send_intr(mic_ctx, 1);
}

static inline void mic_send_pm_intr(mic_ctx_t *mic_ctx)
{
	__mic_send_intr(mic_ctx, PM_SBOX_INT_IDX);
}

static inline void mic_send_bootstrap_intr(mic_ctx_t *mic_ctx)
{
	uint32_t apicicr_low;
	uint64_t apic_icr_offset = SBOX_APICICR7;
	int vector = MIC_BSP_INTERRUPT_VECTOR;

	if (mic_ctx->bi_family == FAMILY_ABR){
		apicicr_low = vector;
	} else {
		/* for KNC we need to make sure we "hit" the send_icr bit (13) */
		apicicr_low = (vector | (1 << 13));
	}

	SBOX_WRITE(mic_ctx->apic_id, mic_ctx->mmio.va, apic_icr_offset + 4);
	// MIC card only triggers when we write the lower part of the address (upper bits)
	SBOX_WRITE(apicicr_low, mic_ctx->mmio.va, apic_icr_offset);
}
