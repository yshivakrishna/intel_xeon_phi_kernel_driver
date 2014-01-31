Intel Xeon PHI kernel driver with new fast PCI copies interface

This driver adds new DMA functionality to Intel's driver.
The new set of ioctls will allow for transferring data between the host and a MIC device with little overhead, and irrespective of software running on the device side.

New ioctls are found in include/mic/micmem_io.h


Special thanks to our hardware sponsor aeoncomputing.com for providing us access to hardware during our development.


Current features
================

- Memory pinning and mapping
- DMA transfers


Future features
===============

- Make new ioctls fully independent of MPSS and device uOS
- Improve speed by using two DMA channels
- Make the code thread-friendly by adding fine-grained locking


Starting
========

The module has only been tested on m1om devices. It doesn't contain checks against l1om.
Apart from replacing the module, mpss needs to be running (this is TODO).
    /etc/init.d/mpss stop
    rmmod mic
    insmod mic
    /etc/init.d/mpss start
    

User-mode interface
===================

The patch is entirely based on ioctls, piggybacking on /dev/mic/ctrl.
ioctls are described in detail in include/mic/micmem_io.h. This file should be included from the program that's going to use micmem.

All ioctls return 0 on success. Most commands dealing with memory assume that memory is aligned to page size (4096B).
However, micmem detects if host memory is using huge pages and can improve transfer speed by reducing number of dma transfers, therefore it's recommended to align all memory to 2MiB boundaries (e.g. using posix_memalign()).

The model of micmem assumes that a single open fd can be assigned to at most 1 device. To use multiple devices simultaneously, /dev/mic/ctrl must be opened multiple times.
The first command before any DMA actions is selecting the device using IOCTL_MICMEM_OPENDEV. The last command should be IOCTL_MICMEM_CLOSEDEV.

The transfer commands IOCTL_MICMEM_HOST2DEV and IOCTL_MICMEM_DEV2HOST can only operate on host memory ranges previously mapped using IOCTL_MICMEM_MAPRANGE. Memory can be unmapped with IOCTL_MICMEM_UNMAPRANGE.

All operations are synchronous.



Based on Intel kmod version 2.1.6720-19
