#
#  Manycore Throughput Linux Driver
#  Copyright (c) 2010, Intel Corporation.
#
#  This program is free software; you can redistribute it and/or modify it
#  under the terms and conditions of the GNU General Public License,
#  version 2, as published by the Free Software Foundation.
#
#  This program is distributed in the hope it will be useful, but WITHOUT
#  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
#  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
#  more details.
#
#  You should have received a copy of the GNU General Public License along with
#  this program; if not, write to the Free Software Foundation, Inc.,
#  51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
#
#

include $(M)/version.mk

mic-DEBUG ?= 0

mic-cflags := $(KERNWARNFLAGS) -D__LINUX_GPL__ -D_MODULE_SCIF_ -DHOST
mic-cflags += -DUSE_VCONSOLE
mic-cflags += -DBUILD_VERSION=\"'$(VER_BUILD) ($(VER_BYWHOM))'\"
mic-cflags += -DHOST_REV=\"$(VER_SCIF)\"
mic-cflags += -DUOS_REV=\"$(VER_LINUX)\"
ifeq ($(MICARCH),l1om)
mic-cflags += -DCONFIG_ML1OM
else ifeq ($(MICARCH),k1om)
mic-cflags += -DCONFIG_MK1OM
else
$(error $$(MICARCH) == '$(MICARCH)' not supported, use l1om or k1om)
endif
mic-cflags += $(SPOOKY_MIC_CFLAGS)

ifeq ($(mic-DEBUG),1)
mic-cflags += -O0 -DDEBUG
ldflags-y += -O0
endif

mic-cflags += -I$(M) -I$(M)/include

obj-m += mic.o

# list of objects in alphabetic order
mic-objs :=
mic-objs += acptboot.o
mic-objs += linux.o
mic-objs += linsysfs.o
mic-objs += linvcons.o
mic-objs += linpm.o
mic-objs += linpsmi.o
mic-objs += micpsmi.o
mic-objs += linscif_host.o
mic-objs += micscif_sysfs.o
mic-objs += micscif_api.o
mic-objs += micscif_fd.o
mic-objs += micscif_intr.o
mic-objs += micscif_nodeqp.o
mic-objs += micscif_smpt.o
mic-objs += micscif_rb.o
mic-objs += micscif_va_node.o
mic-objs += micscif_va_gen.o
mic-objs += micscif_rma.o
mic-objs += micscif_rma_list.o
mic-objs += micscif_rma_dma.o
mic-objs += micscif_debug.o
mic-objs += micscif_ports.o
mic-objs += micveth_param.o
mic-objs += linvnet.o
mic-objs += micveth_dma.o
mic-objs += uos_download.o
mic-objs += ioctl.o
mic-objs += pm_ioctl.o
mic-objs += mic_dma_lib.o
mic-objs += mic_dma_md.o
mic-objs += micscif_pm.o
mic-objs += tools_support.o
mic-objs += micscif_select.o
mic-objs += micscif_gtt.o
mic-objs += micscif_nm.o
mic-objs += pm_pcstate.o
mic-objs += vhost/mic_vhost.o
mic-objs += vhost/mic_blk.o
mic-objs += vmcore.o

version-le = $(shell printf '%s\n' $(1) | sort -CV && echo t)
ifeq ($(call version-le, 2.6.23 $(KERNELRELEASE)),t)
ccflags-y += $(mic-cflags)
else
EXTRA_CFLAGS += $(mic-cflags)
endif
