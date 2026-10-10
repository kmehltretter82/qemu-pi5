/*
 * BCM2712 SDHCI host and configuration block.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_SD_BCM2712_SDHCI_H
#define HW_SD_BCM2712_SDHCI_H

#include "hw/sd/sdhci.h"

#define TYPE_BCM2712_SDHCI "bcm2712-sdhci"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2712SDHCIState, BCM2712_SDHCI)

#define BCM2712_SD_CLOCK 200000000

struct BCM2712SDHCIState {
    SysBusDevice parent_obj;
    SDHCIState sdhci;
    MemoryRegion host;
    MemoryRegion cfg;
    BusState *bus;
    uint32_t cfg_ctrl;
    uint32_t pin_sel;
    uint32_t cq_cap;
    uint32_t max_50mhz;
    uint32_t vendor_ctrl;
};

#endif
