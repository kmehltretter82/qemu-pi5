/*
 * Experimental BCM2712 core platform.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_BCM2712_H
#define HW_ARM_BCM2712_H

#include "hw/core/sysbus.h"
#include "hw/intc/arm_gic.h"
#include "hw/char/pl011.h"
#include "hw/sd/bcm2712_sdhci.h"
#include "target/arm/cpu.h"

#define TYPE_BCM2712 "bcm2712"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2712State, BCM2712)

#define BCM2712_NCPUS           4
#define BCM2712_GIC_IRQS        320
#define BCM2712_GIC_SPIS        (BCM2712_GIC_IRQS - GIC_INTERNAL)
#define BCM2712_PMU_SPI_BASE    16
#define BCM2712_GIC_DIST        UINT64_C(0x107fff9000)
#define BCM2712_GIC_CPU         UINT64_C(0x107fffa000)
#define BCM2712_GIC_HYP         UINT64_C(0x107fffc000)
#define BCM2712_GIC_VCPU        UINT64_C(0x107fffe000)
#define BCM2712_UART10          UINT64_C(0x107d001000)
#define BCM2712_UART10_SPI      121
#define BCM2712_UART_CLOCK      9216000
#define BCM2712_COUNTER_HZ      54000000
#define BCM2712_SDIO1           UINT64_C(0x1000fff000)
#define BCM2712_SDIO1_CFG       UINT64_C(0x1000fff400)
#define BCM2712_SDIO1_SPI       273

struct BCM2712State {
    SysBusDevice parent_obj;
    ARMCPU cpu[BCM2712_NCPUS];
    GICState gic;
    PL011State uart10;
    BCM2712SDHCIState sdio1;
};

#endif
