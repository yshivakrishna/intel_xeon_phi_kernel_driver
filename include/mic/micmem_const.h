/*
 * Copyright (C) 2014 PathScale Inc. All Rights Reserved.
 */

/* Constants that are used in kernel mode (in micmem library) portion and are
 * part of the public interface that is exposed to userland.
 */

#define MICMEM_AUTO	0	/* Use appropriate channels count */
#define MICMEM_SINGLE	1	/* Use one channel */
#define MICMEM_DUAL	2	/* Use two channels simultaneously */
