/*
 * Broadcom BCM2712 PCIe host controller.
 *
 * This models the address translations used by PCIe1 on BCM2712.  Linux
 * programs the same outbound and inbound windows as physical hardware;
 * endpoint DMA therefore enters a private PCI address space instead of
 * bypassing the controller through system memory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "migration/vmstate.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pcie_port.h"
#include "hw/pci-host/bcm2712_pcie.h"
#include "system/address-spaces.h"
#include "trace.h"

#define PCIE_RC_CFG_PRIV1_ID_VAL3               0x043c
#define PCIE_RC_CFG_PRIV1_LINK_CAPABILITY       0x04dc

#define PCIE_RC_DL_MDIO_WR_DATA                 0x1104
#define  MDIO_DATA_DONE                         BIT(31)

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO        0x400c
#define PCIE_MISC_RC_BAR1_CONFIG_LO             0x402c
#define PCIE_MISC_PCIE_CTRL                     0x4064
#define  PCIE_MISC_PCIE_CTRL_PERSTB             BIT(2)
#define PCIE_MISC_PCIE_STATUS                   0x4068
#define  PCIE_MISC_PCIE_STATUS_PORT             BIT(7)
#define  PCIE_MISC_PCIE_STATUS_DL_ACTIVE        BIT(5)
#define  PCIE_MISC_PCIE_STATUS_PHYLINKUP        BIT(4)
#define PCIE_MISC_REVISION                      0x406c
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT 0x4070
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI   0x4080
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI  0x4084
#define PCIE_MISC_UBUS_BAR1_CONFIG_REMAP        0x40ac
#define PCIE_MISC_RC_BAR4_CONFIG_LO             0x40d4
#define PCIE_MISC_UBUS_BAR4_CONFIG_REMAP        0x410c
#define PCIE_EXT_CFG_DATA                       0x8000
#define PCIE_EXT_CFG_INDEX                      0x9000
#define PCIE_RGR1_SW_INIT_1                     0x9210
#define  PCIE_RGR1_SW_INIT_GENERIC              BIT(1)

#define PCIE_CAP_OFFSET                         0x00ac
#define PCIE_LINK_SPEED_GEN2                    2
#define PCIE_LINK_WIDTH_X1                      1

#define OUTBOUND_HIGH_MASK                      0x000000ff
#define INBOUND_SIZE_MASK                       0x1f
#define INBOUND_ACCESS_ENABLE                   BIT(0)

static uint32_t bcm2712_pcie_raw_readl(BCM2712PCIeHost *s, hwaddr addr)
{
    return ldl_le_p(&s->regs[addr]);
}

static void bcm2712_pcie_raw_writel(BCM2712PCIeHost *s, hwaddr addr,
                                    uint32_t value)
{
    stl_le_p(&s->regs[addr], value);
}

static hwaddr bcm2712_pcie_inbound_bar(unsigned int win)
{
    unsigned int bar = win + 1;

    return bar <= 3 ? PCIE_MISC_RC_BAR1_CONFIG_LO + 8 * (bar - 1) :
                      PCIE_MISC_RC_BAR4_CONFIG_LO + 8 * (bar - 4);
}

static hwaddr bcm2712_pcie_inbound_remap(unsigned int win)
{
    unsigned int bar = win + 1;

    return bar <= 3 ? PCIE_MISC_UBUS_BAR1_CONFIG_REMAP + 8 * (bar - 1) :
                      PCIE_MISC_UBUS_BAR4_CONFIG_REMAP + 8 * (bar - 4);
}

static uint64_t bcm2712_pcie_inbound_size(uint32_t code)
{
    if (code >= 1 && code <= 21) {
        return UINT64_C(1) << (code + 15);
    }
    if (code >= 28 && code <= 31) {
        return UINT64_C(1) << (code - 16);
    }
    return 0;
}

static void bcm2712_pcie_update_outbound(BCM2712PCIeHost *s,
                                         unsigned int win)
{
    BCM2712PCIeOutbound *window = &s->outbound[win];
    hwaddr pcie_reg = PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO + win * 8;
    hwaddr range_reg = PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT + win * 4;
    hwaddr base_hi_reg = PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI + win * 8;
    hwaddr limit_hi_reg = PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI + win * 8;
    uint32_t range = bcm2712_pcie_raw_readl(s, range_reg);
    uint64_t pcie_base = bcm2712_pcie_raw_readl(s, pcie_reg) |
                         (uint64_t)bcm2712_pcie_raw_readl(s, pcie_reg + 4)
                         << 32;
    uint64_t cpu_mb = extract32(range, 4, 12) |
                      (uint64_t)(bcm2712_pcie_raw_readl(s, base_hi_reg) &
                                 OUTBOUND_HIGH_MASK) << 12;
    uint64_t limit_mb = extract32(range, 20, 12) |
                        (uint64_t)(bcm2712_pcie_raw_readl(s, limit_hi_reg) &
                                   OUTBOUND_HIGH_MASK) << 12;
    uint64_t cpu_base = cpu_mb << 20;
    uint64_t size;

    memory_region_transaction_begin();
    memory_region_set_enabled(&window->alias, false);
    if (s->outbound_programmed[win] && limit_mb >= cpu_mb) {
        size = (limit_mb - cpu_mb + 1) << 20;
        memory_region_set_alias_offset(&window->alias, pcie_base);
        memory_region_set_size(&window->alias, size);
        memory_region_set_address(&window->alias, cpu_base);
        memory_region_set_enabled(&window->alias, true);
    }
    memory_region_transaction_commit();
    trace_bcm2712_pcie_outbound(win, cpu_base, pcie_base,
                                s->outbound_programmed[win] &&
                                limit_mb >= cpu_mb ?
                                (limit_mb - cpu_mb + 1) << 20 : 0);
}

static void bcm2712_pcie_update_inbound(BCM2712PCIeHost *s,
                                        unsigned int win)
{
    BCM2712PCIeInbound *window = &s->inbound[win];
    hwaddr bar_reg = bcm2712_pcie_inbound_bar(win);
    hwaddr remap_reg = bcm2712_pcie_inbound_remap(win);
    uint32_t bar_lo = bcm2712_pcie_raw_readl(s, bar_reg);
    uint32_t remap_lo = bcm2712_pcie_raw_readl(s, remap_reg);
    uint64_t pci_base = (bar_lo & ~(uint64_t)INBOUND_SIZE_MASK) |
                        (uint64_t)bcm2712_pcie_raw_readl(s, bar_reg + 4)
                        << 32;
    uint64_t cpu_base = (remap_lo & ~UINT64_C(0xfff)) |
                        (uint64_t)bcm2712_pcie_raw_readl(s, remap_reg + 4)
                        << 32;
    uint64_t size = bcm2712_pcie_inbound_size(bar_lo & INBOUND_SIZE_MASK);
    MemoryRegion *alias;
    bool is_msi = false;

    memory_region_transaction_begin();
    memory_region_set_enabled(&window->system_alias, false);
    memory_region_set_enabled(&window->msi_alias, false);
    if (!(remap_lo & INBOUND_ACCESS_ENABLE) || !size) {
        memory_region_transaction_commit();
        trace_bcm2712_pcie_inbound(win, pci_base, cpu_base, 0, false);
        return;
    }

    if (cpu_base == s->msi_cpu_addr &&
        size <= memory_region_size(s->msi_region)) {
        alias = &window->msi_alias;
        is_msi = true;
        memory_region_set_alias_offset(alias, 0);
    } else {
        alias = &window->system_alias;
        memory_region_set_alias_offset(alias, cpu_base);
    }
    memory_region_set_size(alias, size);
    memory_region_set_address(alias, pci_base);
    memory_region_set_enabled(alias, true);
    memory_region_transaction_commit();
    trace_bcm2712_pcie_inbound(win, pci_base, cpu_base, size, is_msi);
}

static void bcm2712_pcie_update_windows(BCM2712PCIeHost *s, hwaddr addr)
{
    for (unsigned int i = 0; i < BCM2712_PCIE_NUM_OUTBOUND; i++) {
        hwaddr pcie = PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO + i * 8;
        hwaddr range = PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT + i * 4;
        hwaddr base_hi = PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI + i * 8;
        hwaddr limit_hi = PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI + i * 8;

        if (addr == range) {
            s->outbound_programmed[i] = true;
        }
        if (addr == pcie || addr == pcie + 4 || addr == range ||
            addr == base_hi || addr == limit_hi) {
            bcm2712_pcie_update_outbound(s, i);
        }
    }

    for (unsigned int i = 0; i < BCM2712_PCIE_NUM_INBOUND; i++) {
        hwaddr bar = bcm2712_pcie_inbound_bar(i);
        hwaddr remap = bcm2712_pcie_inbound_remap(i);

        if (addr == bar || addr == bar + 4 || addr == remap ||
            addr == remap + 4) {
            bcm2712_pcie_update_inbound(s, i);
        }
    }
}

static bool bcm2712_pcie_has_endpoint(BCM2712PCIeHost *s)
{
    PCIBus *bus = pci_bridge_get_sec_bus(PCI_BRIDGE(&s->root));

    for (unsigned int i = 0; i < ARRAY_SIZE(bus->devices); i++) {
        if (bus->devices[i]) {
            return true;
        }
    }
    return false;
}

static bool bcm2712_pcie_link_up(BCM2712PCIeHost *s)
{
    return !(bcm2712_pcie_raw_readl(s, PCIE_RGR1_SW_INIT_1) &
             PCIE_RGR1_SW_INIT_GENERIC) &&
           (bcm2712_pcie_raw_readl(s, PCIE_MISC_PCIE_CTRL) &
            PCIE_MISC_PCIE_CTRL_PERSTB) &&
           bcm2712_pcie_has_endpoint(s);
}

static uint64_t bcm2712_pcie_ext_config_read(BCM2712PCIeHost *s,
                                             hwaddr addr, unsigned int size)
{
    PCIHostState *host = PCI_HOST_BRIDGE(s);
    unsigned int bus = extract32(s->ext_cfg_index, 20, 8);
    unsigned int devfn = extract32(s->ext_cfg_index, 12, 8);
    PCIDevice *dev = pci_find_device(host->bus, bus, devfn);

    if (!dev) {
        return UINT64_MAX;
    }
    return pci_host_config_read_common(dev, addr - PCIE_EXT_CFG_DATA,
                                       pci_config_size(dev), size);
}

static void bcm2712_pcie_ext_config_write(BCM2712PCIeHost *s, hwaddr addr,
                                          uint64_t value, unsigned int size)
{
    PCIHostState *host = PCI_HOST_BRIDGE(s);
    unsigned int bus = extract32(s->ext_cfg_index, 20, 8);
    unsigned int devfn = extract32(s->ext_cfg_index, 12, 8);
    PCIDevice *dev = pci_find_device(host->bus, bus, devfn);

    if (dev) {
        pci_host_config_write_common(dev, addr - PCIE_EXT_CFG_DATA,
                                     pci_config_size(dev), value, size);
    }
}

static uint64_t bcm2712_pcie_reg_read(void *opaque, hwaddr addr,
                                      unsigned int size)
{
    BCM2712PCIeHost *s = opaque;
    PCIDevice *root = PCI_DEVICE(&s->root);

    if (addr < 0x1000) {
        if (addr <= PCIE_CAP_OFFSET + PCI_EXP_LNKSTA &&
            addr + size > PCIE_CAP_OFFSET + PCI_EXP_LNKSTA) {
            uint16_t status = PCI_EXP_LNKSTA_DLLLA |
                              (PCIE_LINK_WIDTH_X1 << 4) |
                              PCIE_LINK_SPEED_GEN2;

            if (!bcm2712_pcie_link_up(s)) {
                status &= ~PCI_EXP_LNKSTA_DLLLA;
            }
            pci_set_word(root->config + PCIE_CAP_OFFSET + PCI_EXP_LNKSTA,
                         status);
        }
        return pci_host_config_read_common(root, addr, pci_config_size(root),
                                           size);
    }
    if (addr >= PCIE_EXT_CFG_DATA && addr < PCIE_EXT_CFG_DATA + 0x1000) {
        return bcm2712_pcie_ext_config_read(s, addr, size);
    }
    if (addr == PCIE_EXT_CFG_INDEX && size == 4) {
        return s->ext_cfg_index;
    }
    if (addr == PCIE_MISC_PCIE_STATUS && size == 4) {
        uint32_t value = PCIE_MISC_PCIE_STATUS_PORT;

        if (bcm2712_pcie_link_up(s)) {
            value |= PCIE_MISC_PCIE_STATUS_DL_ACTIVE |
                     PCIE_MISC_PCIE_STATUS_PHYLINKUP;
        }
        return value;
    }
    if (addr + size > BCM2712_PCIE_REG_SIZE) {
        return UINT64_MAX;
    }
    return ldn_le_p(&s->regs[addr], size);
}

static void bcm2712_pcie_reg_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned int size)
{
    BCM2712PCIeHost *s = opaque;
    PCIDevice *root = PCI_DEVICE(&s->root);

    if (addr < 0x1000) {
        pci_host_config_write_common(root, addr, pci_config_size(root), value,
                                     size);
        return;
    }
    if (addr >= PCIE_EXT_CFG_DATA && addr < PCIE_EXT_CFG_DATA + 0x1000) {
        bcm2712_pcie_ext_config_write(s, addr, value, size);
        return;
    }
    if (addr == PCIE_EXT_CFG_INDEX && size == 4) {
        s->ext_cfg_index = value;
        return;
    }
    if (addr + size > BCM2712_PCIE_REG_SIZE) {
        return;
    }

    stn_le_p(&s->regs[addr], size, value);
    if (addr == PCIE_RC_DL_MDIO_WR_DATA && size == 4) {
        bcm2712_pcie_raw_writel(s, addr, value & ~MDIO_DATA_DONE);
    }
    if (size == 4) {
        bcm2712_pcie_update_windows(s, addr);
    }
}

static const MemoryRegionOps bcm2712_pcie_reg_ops = {
    .read = bcm2712_pcie_reg_read,
    .write = bcm2712_pcie_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void bcm2712_pcie_set_irq(void *opaque, int irq_num, int level)
{
    BCM2712PCIeHost *s = opaque;

    qemu_set_irq(s->pci.intx[irq_num], level);
}

static AddressSpace *bcm2712_pcie_dma_address_space(PCIBus *bus, void *opaque,
                                                    int devfn)
{
    BCM2712PCIeHost *s = opaque;

    return &s->pci.dma_as;
}

static const PCIIOMMUOps bcm2712_pcie_iommu_ops = {
    .get_address_space = bcm2712_pcie_dma_address_space,
};

static const char *bcm2712_pcie_root_bus_path(PCIHostState *host,
                                              PCIBus *rootbus)
{
    return "0001:00";
}

static void bcm2712_pcie_root_bus_class_init(ObjectClass *oc,
                                             const void *data)
{
    BusClass *bc = BUS_CLASS(oc);

    bc->max_dev = 1;
}

static BCM2712PCIeHost *bcm2712_pcie_root_to_host(BCM2712PCIeRoot *root)
{
    BusState *bus = qdev_get_parent_bus(DEVICE(root));

    return BCM2712_PCIE_HOST(bus->parent);
}

static void bcm2712_pcie_root_realize(PCIDevice *dev, Error **errp)
{
    BCM2712PCIeRoot *root = BCM2712_PCIE_ROOT(dev);
    BCM2712PCIeHost *host = bcm2712_pcie_root_to_host(root);
    PCIBridge *bridge = PCI_BRIDGE(dev);

    bridge->bus_name = "pcie1";
    pci_set_word(dev->config + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    pci_config_set_interrupt_pin(dev->config, 1);
    pci_bridge_initfn(dev, TYPE_PCIE_BUS);
    pcie_port_init_reg(dev);
    if (pcie_cap_init(dev, PCIE_CAP_OFFSET, PCI_EXP_TYPE_ROOT_PORT, 0,
                      errp) < 0) {
        return;
    }

    pci_set_long(dev->config + PCIE_RC_CFG_PRIV1_ID_VAL3, 0x060400);
    pci_set_long(dev->wmask + PCIE_RC_CFG_PRIV1_ID_VAL3, 0x00ffffff);
    pci_set_long(dev->config + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY, 0x12);
    pci_set_long(dev->wmask + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY, 0x1ff);
    pci_set_long(dev->config + PCIE_CAP_OFFSET + PCI_EXP_LNKCAP,
                 PCI_EXP_LNKCAP_DLLLARC | (PCIE_LINK_WIDTH_X1 << 4) |
                 PCIE_LINK_SPEED_GEN2);
    pci_set_word(dev->config + PCIE_CAP_OFFSET + PCI_EXP_LNKSTA,
                 PCI_EXP_LNKSTA_DLLLA | (PCIE_LINK_WIDTH_X1 << 4) |
                 PCIE_LINK_SPEED_GEN2);

    /* The controller starts with the endpoint held in fundamental reset. */
    bcm2712_pcie_raw_writel(host, PCIE_MISC_PCIE_CTRL, 0);
}

static const VMStateDescription vmstate_bcm2712_pcie_root = {
    .name = TYPE_BCM2712_PCIE_ROOT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, PCIBridge),
        VMSTATE_END_OF_LIST(),
    },
};

static void bcm2712_pcie_root_class_init(ObjectClass *oc, const void *data)
{
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    pc->vendor_id = 0x14e4;
    pc->device_id = 0x2712;
    pc->revision = 0x30;
    pc->class_id = PCI_CLASS_BRIDGE_PCI;
    pc->realize = bcm2712_pcie_root_realize;
    pc->exit = pci_bridge_exitfn;
    pc->config_write = pci_bridge_write_config;
    device_class_set_legacy_reset(dc, pci_bridge_reset);
    dc->user_creatable = false;
    dc->vmsd = &vmstate_bcm2712_pcie_root;
}

static void bcm2712_pcie_disable_windows(BCM2712PCIeHost *s)
{
    memory_region_transaction_begin();
    for (unsigned int i = 0; i < BCM2712_PCIE_NUM_OUTBOUND; i++) {
        memory_region_set_enabled(&s->outbound[i].alias, false);
    }
    for (unsigned int i = 0; i < BCM2712_PCIE_NUM_INBOUND; i++) {
        memory_region_set_enabled(&s->inbound[i].system_alias, false);
        memory_region_set_enabled(&s->inbound[i].msi_alias, false);
    }
    memory_region_transaction_commit();
}

static void bcm2712_pcie_reset(DeviceState *dev)
{
    BCM2712PCIeHost *s = BCM2712_PCIE_HOST(dev);

    bcm2712_pcie_disable_windows(s);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->outbound_programmed, 0, sizeof(s->outbound_programmed));
    s->ext_cfg_index = 0;
    bcm2712_pcie_raw_writel(s, PCIE_MISC_REVISION, 0x0303);
}

static int bcm2712_pcie_post_load(void *opaque, int version_id)
{
    BCM2712PCIeHost *s = opaque;

    for (unsigned int i = 0; i < BCM2712_PCIE_NUM_OUTBOUND; i++) {
        bcm2712_pcie_update_outbound(s, i);
    }
    for (unsigned int i = 0; i < BCM2712_PCIE_NUM_INBOUND; i++) {
        bcm2712_pcie_update_inbound(s, i);
    }
    return 0;
}

static const VMStateDescription vmstate_bcm2712_pcie_host = {
    .name = TYPE_BCM2712_PCIE_HOST,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = bcm2712_pcie_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, BCM2712PCIeHost, BCM2712_PCIE_REG_SIZE),
        VMSTATE_UINT32(ext_cfg_index, BCM2712PCIeHost),
        VMSTATE_BOOL_ARRAY(outbound_programmed, BCM2712PCIeHost,
                           BCM2712_PCIE_NUM_OUTBOUND),
        VMSTATE_STRUCT(root, BCM2712PCIeHost, 1,
                       vmstate_bcm2712_pcie_root, BCM2712PCIeRoot),
        VMSTATE_END_OF_LIST(),
    },
};

static void bcm2712_pcie_host_realize(DeviceState *dev, Error **errp)
{
    BCM2712PCIeHost *s = BCM2712_PCIE_HOST(dev);
    PCIHostState *host = PCI_HOST_BRIDGE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    MemoryRegion *system_memory = get_system_memory();

    if (!s->msi_region) {
        error_setg(errp, "BCM2712 PCIe host requires an MSI message region");
        return;
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(s->pci.intx); i++) {
        sysbus_init_irq(sbd, &s->pci.intx[i]);
    }
    sysbus_init_irq(sbd, &s->pci.controller_irq);

    memory_region_init_io(&s->regs_mr, OBJECT(s), &bcm2712_pcie_reg_ops, s,
                          "bcm2712-pcie-regs", BCM2712_PCIE_REG_SIZE);
    sysbus_init_mmio(sbd, &s->regs_mr);
    memory_region_init(&s->pci.io, OBJECT(s), "bcm2712-pcie-io", 0x10000);
    memory_region_init(&s->pci.memory, OBJECT(s), "bcm2712-pcie-memory",
                       UINT64_MAX);

    host->bus = pci_register_root_bus(dev, "bcm2712-pcie-root",
                                      bcm2712_pcie_set_irq,
                                      pci_swizzle_map_irq_fn, s,
                                      &s->pci.memory, &s->pci.io, 0, 4,
                                      TYPE_BCM2712_PCIE_ROOT_BUS);
    host->bus->flags |= PCI_BUS_EXTENDED_CONFIG_SPACE;

    memory_region_init(&s->pci.dma_root, OBJECT(s), "bcm2712-pcie-dma",
                       UINT64_MAX);
    address_space_init(&s->pci.dma_as, &s->pci.dma_root,
                       "bcm2712-pcie-dma");
    pci_setup_iommu(host->bus, &bcm2712_pcie_iommu_ops, s);

    for (unsigned int i = 0; i < BCM2712_PCIE_NUM_OUTBOUND; i++) {
        g_autofree char *name = g_strdup_printf("bcm2712-pcie-outbound%u", i);

        memory_region_init_alias(&s->outbound[i].alias, OBJECT(s), name,
                                 &s->pci.memory, 0, 1);
        memory_region_add_subregion_overlap(system_memory, 0,
                                             &s->outbound[i].alias, -1);
        memory_region_set_enabled(&s->outbound[i].alias, false);
    }
    for (unsigned int i = 0; i < BCM2712_PCIE_NUM_INBOUND; i++) {
        g_autofree char *system_name =
            g_strdup_printf("bcm2712-pcie-inbound%u-system", i);
        g_autofree char *msi_name =
            g_strdup_printf("bcm2712-pcie-inbound%u-msi", i);

        memory_region_init_alias(&s->inbound[i].system_alias, OBJECT(s),
                                 system_name, system_memory, 0, 1);
        memory_region_init_alias(&s->inbound[i].msi_alias, OBJECT(s),
                                 msi_name, s->msi_region, 0, 1);
        memory_region_add_subregion_overlap(&s->pci.dma_root, 0,
                  &s->inbound[i].system_alias, -1);
        memory_region_add_subregion_overlap(&s->pci.dma_root, 0,
                  &s->inbound[i].msi_alias, 0);
        memory_region_set_enabled(&s->inbound[i].system_alias, false);
        memory_region_set_enabled(&s->inbound[i].msi_alias, false);
    }

    qdev_realize(DEVICE(&s->root), BUS(host->bus), errp);
}

static void bcm2712_pcie_host_init(Object *obj)
{
    BCM2712PCIeHost *s = BCM2712_PCIE_HOST(obj);

    object_initialize_child(obj, "root", &s->root, TYPE_BCM2712_PCIE_ROOT);
    qdev_prop_set_int32(DEVICE(&s->root), "addr", PCI_DEVFN(0, 0));
    qdev_prop_set_bit(DEVICE(&s->root), "multifunction", false);
}

static void bcm2712_pcie_host_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(oc);

    hc->root_bus_path = bcm2712_pcie_root_bus_path;
    dc->realize = bcm2712_pcie_host_realize;
    dc->fw_name = "pci";
    dc->vmsd = &vmstate_bcm2712_pcie_host;
    device_class_set_legacy_reset(dc, bcm2712_pcie_reset);
}

void bcm2712_pcie_set_msi_region(BCM2712PCIeHost *s, MemoryRegion *region,
                                 hwaddr cpu_addr)
{
    s->msi_region = region;
    s->msi_cpu_addr = cpu_addr;
}

static const TypeInfo bcm2712_pcie_types[] = {
    {
        .name = TYPE_BCM2712_PCIE_ROOT_BUS,
        .parent = TYPE_PCIE_BUS,
        .instance_size = sizeof(PCIBus),
        .class_init = bcm2712_pcie_root_bus_class_init,
    }, {
        .name = TYPE_BCM2712_PCIE_HOST,
        .parent = TYPE_PCI_HOST_BRIDGE,
        .instance_size = sizeof(BCM2712PCIeHost),
        .instance_init = bcm2712_pcie_host_init,
        .class_init = bcm2712_pcie_host_class_init,
    }, {
        .name = TYPE_BCM2712_PCIE_ROOT,
        .parent = TYPE_PCI_BRIDGE,
        .instance_size = sizeof(BCM2712PCIeRoot),
        .class_init = bcm2712_pcie_root_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(bcm2712_pcie_types)
