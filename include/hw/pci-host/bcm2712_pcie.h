/*
 * Broadcom BCM2712 PCIe host controller.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_PCI_HOST_BCM2712_PCIE_H
#define HW_PCI_HOST_BCM2712_PCIE_H

#include "hw/core/sysbus.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pci_host.h"

#define TYPE_BCM2712_PCIE_HOST "bcm2712-pcie-host"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2712PCIeHost, BCM2712_PCIE_HOST)

#define TYPE_BCM2712_PCIE_ROOT "bcm2712-pcie-root"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2712PCIeRoot, BCM2712_PCIE_ROOT)

#define TYPE_BCM2712_PCIE_ROOT_BUS "bcm2712-pcie-root-bus"

#define BCM2712_PCIE_NUM_OUTBOUND 4
#define BCM2712_PCIE_NUM_INBOUND 10
#define BCM2712_PCIE_REG_SIZE 0x9310

typedef struct BCM2712PCIeOutbound {
    MemoryRegion alias;
} BCM2712PCIeOutbound;

typedef struct BCM2712PCIeInbound {
    MemoryRegion system_alias;
    MemoryRegion msi_alias;
} BCM2712PCIeInbound;

struct BCM2712PCIeRoot {
    PCIBridge parent_obj;
};

struct BCM2712PCIeHost {
    PCIHostState parent_obj;

    BCM2712PCIeRoot root;
    MemoryRegion regs_mr;
    uint8_t regs[BCM2712_PCIE_REG_SIZE];
    uint32_t ext_cfg_index;

    struct {
        AddressSpace dma_as;
        MemoryRegion dma_root;
        MemoryRegion memory;
        MemoryRegion io;
        qemu_irq intx[4];
        qemu_irq controller_irq;
    } pci;

    BCM2712PCIeOutbound outbound[BCM2712_PCIE_NUM_OUTBOUND];
    bool outbound_programmed[BCM2712_PCIE_NUM_OUTBOUND];
    BCM2712PCIeInbound inbound[BCM2712_PCIE_NUM_INBOUND];
    MemoryRegion *msi_region;
    hwaddr msi_cpu_addr;
};

void bcm2712_pcie_set_msi_region(BCM2712PCIeHost *s, MemoryRegion *region,
                                 hwaddr cpu_addr);

#endif
