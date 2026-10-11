/*
 * Broadcom BCM2712 MSI-X Interrupt Peripheral.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_INTC_BCM2712_MIP_H
#define HW_INTC_BCM2712_MIP_H

#include "hw/core/sysbus.h"

#define TYPE_BCM2712_MIP "bcm2712-mip"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2712MIPState, BCM2712_MIP)

#define BCM2712_MIP_MAX_IRQS 64

struct BCM2712MIPState {
    SysBusDevice parent_obj;

    MemoryRegion control;
    MemoryRegion message;
    qemu_irq irq[BCM2712_MIP_MAX_IRQS];

    uint32_t num_irqs;
    uint32_t offset;
    uint32_t config[2];
    uint32_t mask_host[2];
    uint32_t mask_vpu[2];
    uint32_t status_host[2];
    uint32_t status_vpu[2];
};

#endif
