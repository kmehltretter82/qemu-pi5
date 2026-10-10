/*
 * Experimental BCM2712 SDHCI host and configuration block.
 * The configuration registers follow Linux's sdhci-brcmstb driver.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "hw/sd/bcm2712_sdhci.h"
#include "sdhci-internal.h"

#define CFG_CTRL         0x000
#define CFG_PIN_SEL      0x044
#define CFG_CQ_CAP       0x04c
#define CFG_MAX_50MHZ    0x1ac
#define CFG_CD_TEST_EN   BIT(31)
#define CFG_CD_TEST_LEV  BIT(30)
#define HOST_VENDOR     0x078

/*
 * 200 MHz base, 1 MHz timeout, 512-byte blocks, high speed, SDMA/ADMA2,
 * 64-bit DMA, 3.3 V and 1.8 V. UHS speed modes and CQE are not advertised.
 * These are the capabilities of the implemented subset, not a hardware dump.
 */
#define BCM2712_SD_CAPS UINT64_C(0x000000001568c881)

static void bcm2712_sdhci_update_cd(BCM2712SDHCIState *s)
{
    bool present;

    if (s->cfg_ctrl & CFG_CD_TEST_EN) {
        present = !(s->cfg_ctrl & CFG_CD_TEST_LEV);
    } else {
        present = sdbus_get_inserted(&s->sdhci.sdbus);
    }
    s->sdhci.prnsts &= ~(SDHC_CARD_PRESENT | SDHC_CARD_DETECT);
    if (present) {
        s->sdhci.prnsts |= SDHC_CARD_PRESENT | SDHC_CARD_DETECT;
    }
}

static uint64_t bcm2712_sdhci_host_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    BCM2712SDHCIState *s = opaque;

    bcm2712_sdhci_update_cd(s);
    if ((addr & ~3) == HOST_VENDOR) {
        return extract32(s->vendor_ctrl, (addr & 3) * 8, size * 8);
    }
    if (addr < SDHC_REGISTERS_MAP_SIZE) {
        return s->sdhci.io_ops->read(&s->sdhci, addr, size);
    }
    qemu_log_mask(LOG_UNIMP, "%s: host read at 0x%" HWADDR_PRIx "\n",
                  TYPE_BCM2712_SDHCI, addr);
    return 0;
}

static void bcm2712_sdhci_host_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    BCM2712SDHCIState *s = opaque;

    bcm2712_sdhci_update_cd(s);
    if ((addr & ~3) == HOST_VENDOR) {
        s->vendor_ctrl = deposit32(s->vendor_ctrl, (addr & 3) * 8,
                                   size * 8, value);
    } else if (addr < SDHC_REGISTERS_MAP_SIZE) {
        s->sdhci.io_ops->write(&s->sdhci, addr, value, size);
    } else {
        qemu_log_mask(LOG_UNIMP, "%s: host write at 0x%" HWADDR_PRIx "\n",
                      TYPE_BCM2712_SDHCI, addr);
    }
    /* SDHCI software reset samples the physical bus; retain the override. */
    bcm2712_sdhci_update_cd(s);
}

static uint32_t *bcm2712_sdhci_cfg_reg(BCM2712SDHCIState *s, hwaddr addr)
{
    switch (addr) {
    case CFG_CTRL:
        return &s->cfg_ctrl;
    case CFG_PIN_SEL:
        return &s->pin_sel;
    case CFG_CQ_CAP:
        return &s->cq_cap;
    case CFG_MAX_50MHZ:
        return &s->max_50mhz;
    default:
        return NULL;
    }
}

static uint64_t bcm2712_sdhci_cfg_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    BCM2712SDHCIState *s = opaque;
    uint32_t *reg = bcm2712_sdhci_cfg_reg(s, addr);

    if (reg) {
        return *reg;
    }
    qemu_log_mask(LOG_UNIMP, "%s: cfg read at 0x%" HWADDR_PRIx "\n",
                  TYPE_BCM2712_SDHCI, addr);
    return 0;
}

static void bcm2712_sdhci_cfg_write(void *opaque, hwaddr addr,
                                   uint64_t value, unsigned size)
{
    BCM2712SDHCIState *s = opaque;
    uint32_t *reg = bcm2712_sdhci_cfg_reg(s, addr);

    if (reg) {
        *reg = value;
        bcm2712_sdhci_update_cd(s);
    } else {
        qemu_log_mask(LOG_UNIMP, "%s: cfg write at 0x%" HWADDR_PRIx "\n",
                      TYPE_BCM2712_SDHCI, addr);
    }
}

static const MemoryRegionOps bcm2712_sdhci_host_ops = {
    .read = bcm2712_sdhci_host_read,
    .write = bcm2712_sdhci_host_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static const MemoryRegionOps bcm2712_sdhci_cfg_ops = {
    .read = bcm2712_sdhci_cfg_read,
    .write = bcm2712_sdhci_cfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void bcm2712_sdhci_reset(DeviceState *dev)
{
    BCM2712SDHCIState *s = BCM2712_SDHCI(dev);

    s->cfg_ctrl = 0;
    s->pin_sel = 0;
    s->cq_cap = 0;
    s->max_50mhz = 0;
    s->vendor_ctrl = 0;
}

static void bcm2712_sdhci_init(Object *obj)
{
    BCM2712SDHCIState *s = BCM2712_SDHCI(obj);

    object_initialize_child(obj, "sdhci", &s->sdhci, TYPE_SYSBUS_SDHCI);
}

static void bcm2712_sdhci_realize(DeviceState *dev, Error **errp)
{
    BCM2712SDHCIState *s = BCM2712_SDHCI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    SysBusDevice *child = SYS_BUS_DEVICE(&s->sdhci);

    qdev_prop_set_uint8(DEVICE(child), "sd-spec-version", 3);
    qdev_prop_set_uint8(DEVICE(child), "uhs", UHS_I);
    qdev_prop_set_uint64(DEVICE(child), "capareg", BCM2712_SD_CAPS);
    if (!sysbus_realize(child, errp)) {
        return;
    }
    memory_region_init_io(&s->host, OBJECT(s), &bcm2712_sdhci_host_ops,
                          s, "bcm2712-sdhci-host", 0x260);
    memory_region_init_io(&s->cfg, OBJECT(s), &bcm2712_sdhci_cfg_ops,
                          s, "bcm2712-sdhci-cfg", 0x200);
    sysbus_init_mmio(sbd, &s->host);
    sysbus_init_mmio(sbd, &s->cfg);
    sysbus_pass_irq(sbd, child);
    s->bus = qdev_get_child_bus(DEVICE(child), "sd-bus");
}

static const VMStateDescription vmstate_bcm2712_sdhci = {
    .name = TYPE_BCM2712_SDHCI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cfg_ctrl, BCM2712SDHCIState),
        VMSTATE_UINT32(pin_sel, BCM2712SDHCIState),
        VMSTATE_UINT32(cq_cap, BCM2712SDHCIState),
        VMSTATE_UINT32(max_50mhz, BCM2712SDHCIState),
        VMSTATE_UINT32(vendor_ctrl, BCM2712SDHCIState),
        VMSTATE_END_OF_LIST(),
    },
};

static void bcm2712_sdhci_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = bcm2712_sdhci_realize;
    dc->user_creatable = false;
    dc->vmsd = &vmstate_bcm2712_sdhci;
    device_class_set_legacy_reset(dc, bcm2712_sdhci_reset);
}

static const TypeInfo bcm2712_sdhci_type = {
    .name = TYPE_BCM2712_SDHCI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2712SDHCIState),
    .instance_init = bcm2712_sdhci_init,
    .class_init = bcm2712_sdhci_class_init,
};

static void bcm2712_sdhci_register_types(void)
{
    type_register_static(&bcm2712_sdhci_type);
}

type_init(bcm2712_sdhci_register_types)
