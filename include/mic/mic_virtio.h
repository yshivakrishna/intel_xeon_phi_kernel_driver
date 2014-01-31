/*
  Structures which are passed from host to MIC card through
  uOS kernel command line option, virtio_addr.

  (C) Copyright 2012 Intel Corporation
  Author: Caz Yokoyama <Caz.Yokoyama@intel.com>
 */
#ifndef MIC_VIRTIO_H
#define MIC_VIRTIO_H

struct vb_shared {
	struct virtio_blk_config blk_config;
	uint32_t host_features;
	uint32_t client_features;
	bool update;
	struct vring vring;
};

struct mic_virtblk {
#ifdef HOST
	struct vb_shared vb_shared;
	void *vblk;  /* keep vblk in vhost for virtblk */
#else
	struct vb_shared *vb_shared;
	void *vdev;  /* keep vdev in virtio for virtblk */
#endif
};

uint64_t mic_vhost_pm_disconnect_node(uint64_t node_bitmask, enum disconn_type type);
void mic_vhost_blk_stop(bd_info_t *bd_info);

#endif // MIC_VIRTIO_H
