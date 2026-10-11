/*
 * Broadcom BCM2712 MSI-X Interrupt Peripheral.
 *
 * The MIP turns the data of a write to its PCIe message aperture into a
 * GIC interrupt.  Linux configures all host outputs as edge triggered and
 * leaves them unmasked before assigning them to PCI devices.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/intc/bcm2712_mip.h"
#include "hw/pci/msi.h"
#include "trace.h"

#define MIP_INT_RAISE          0x00
#define MIP_INT_CLEAR          0x10
#define MIP_INT_CFGL_HOST      0x20
#define MIP_INT_CFGH_HOST      0x30
#define MIP_INT_MASKL_HOST     0x40
#define MIP_INT_MASKH_HOST     0x50
#define MIP_INT_MASKL_VPU      0x60
#define MIP_INT_MASKH_VPU      0x70
#define MIP_INT_STATUSL_HOST   0x80
#define MIP_INT_STATUSH_HOST   0x90
#define MIP_INT_STATUSL_VPU    0xa0
#define MIP_INT_STATUSH_VPU    0xb0

static bool bcm2712_mip_vector(BCM2712MIPState *s, uint32_t vector,
                               unsigned *bank, uint32_t *bit)
{
    if (vector < s->offset || vector >= s->offset + s->num_irqs) {
        return false;
    }

    *bank = vector / 32;
    *bit = BIT(vector % 32);
    return true;
}

static void bcm2712_mip_update_level(BCM2712MIPState *s, unsigned vector)
{
    unsigned bank = vector / 32;
    uint32_t bit = BIT(vector % 32);
    unsigned output = vector - s->offset;

    if (output >= s->num_irqs) {
        return;
    }

    qemu_set_irq(s->irq[output],
                 !(s->config[bank] & bit) &&
                 !!(s->status_host[bank] & bit & ~s->mask_host[bank]));
}

static void bcm2712_mip_raise(BCM2712MIPState *s, uint32_t vector)
{
    unsigned bank;
    uint32_t bit;

    if (!bcm2712_mip_vector(s, vector, &bank, &bit)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid MSI-X vector %u (valid %u..%u)\n",
                      TYPE_BCM2712_MIP, vector, s->offset,
                      s->offset + s->num_irqs - 1);
        return;
    }

    s->status_host[bank] |= bit;
    if (s->mask_host[bank] & bit) {
        return;
    }

    if (s->config[bank] & bit) {
        qemu_irq_pulse(s->irq[vector - s->offset]);
    } else {
        bcm2712_mip_update_level(s, vector);
    }
}

static void bcm2712_mip_clear(BCM2712MIPState *s, uint32_t vector)
{
    unsigned bank;
    uint32_t bit;

    if (!bcm2712_mip_vector(s, vector, &bank, &bit)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid clear vector %u (valid %u..%u)\n",
                      TYPE_BCM2712_MIP, vector, s->offset,
                      s->offset + s->num_irqs - 1);
        return;
    }

    s->status_host[bank] &= ~bit;
    bcm2712_mip_update_level(s, vector);
}

static uint64_t bcm2712_mip_control_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    BCM2712MIPState *s = opaque;

    switch (addr) {
    case MIP_INT_RAISE:
    case MIP_INT_CLEAR:
        return 0;
    case MIP_INT_CFGL_HOST:
        return s->config[0];
    case MIP_INT_CFGH_HOST:
        return s->config[1];
    case MIP_INT_MASKL_HOST:
        return s->mask_host[0];
    case MIP_INT_MASKH_HOST:
        return s->mask_host[1];
    case MIP_INT_MASKL_VPU:
        return s->mask_vpu[0];
    case MIP_INT_MASKH_VPU:
        return s->mask_vpu[1];
    case MIP_INT_STATUSL_HOST:
        return s->status_host[0];
    case MIP_INT_STATUSH_HOST:
        return s->status_host[1];
    case MIP_INT_STATUSL_VPU:
        return s->status_vpu[0];
    case MIP_INT_STATUSH_VPU:
        return s->status_vpu[1];
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read from unknown register 0x%" HWADDR_PRIx "\n",
                      TYPE_BCM2712_MIP, addr);
        return 0;
    }
}

static void bcm2712_mip_control_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size)
{
    BCM2712MIPState *s = opaque;
    unsigned bank;
    uint32_t old;

    switch (addr) {
    case MIP_INT_RAISE:
        bcm2712_mip_raise(s, value);
        return;
    case MIP_INT_CLEAR:
        bcm2712_mip_clear(s, value);
        return;
    case MIP_INT_CFGL_HOST:
    case MIP_INT_CFGH_HOST:
        bank = (addr - MIP_INT_CFGL_HOST) / 0x10;
        old = s->config[bank];
        s->config[bank] = value;
        for (unsigned bit = 0; bit < 32; bit++) {
            if ((old ^ value) & BIT(bit)) {
                bcm2712_mip_update_level(s, bank * 32 + bit);
            }
        }
        return;
    case MIP_INT_MASKL_HOST:
    case MIP_INT_MASKH_HOST:
        bank = (addr - MIP_INT_MASKL_HOST) / 0x10;
        old = s->mask_host[bank];
        s->mask_host[bank] = value;
        for (unsigned bit = 0; bit < 32; bit++) {
            uint32_t bitmask = BIT(bit);
            unsigned vector = bank * 32 + bit;

            if (!((old ^ value) & bitmask)) {
                continue;
            }
            if (!(value & bitmask) && (s->status_host[bank] & bitmask) &&
                (s->config[bank] & bitmask) &&
                vector >= s->offset &&
                vector < s->offset + s->num_irqs) {
                qemu_irq_pulse(s->irq[vector - s->offset]);
            } else {
                bcm2712_mip_update_level(s, vector);
            }
        }
        return;
    case MIP_INT_MASKL_VPU:
        s->mask_vpu[0] = value;
        return;
    case MIP_INT_MASKH_VPU:
        s->mask_vpu[1] = value;
        return;
    case MIP_INT_STATUSL_HOST:
    case MIP_INT_STATUSH_HOST:
    case MIP_INT_STATUSL_VPU:
    case MIP_INT_STATUSH_VPU:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only register 0x%" HWADDR_PRIx "\n",
                      TYPE_BCM2712_MIP, addr);
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to unknown register 0x%" HWADDR_PRIx "\n",
                      TYPE_BCM2712_MIP, addr);
    }
}

static const MemoryRegionOps bcm2712_mip_control_ops = {
    .read = bcm2712_mip_control_read,
    .write = bcm2712_mip_control_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static uint64_t bcm2712_mip_message_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: MSI-X message aperture is write-only\n",
                  TYPE_BCM2712_MIP);
    return 0;
}

static void bcm2712_mip_message_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size)
{
    trace_bcm2712_mip_message((uint32_t)value);
    bcm2712_mip_raise(opaque, value);
}

static const MemoryRegionOps bcm2712_mip_message_ops = {
    .read = bcm2712_mip_message_read,
    .write = bcm2712_mip_message_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void bcm2712_mip_reset(DeviceState *dev)
{
    BCM2712MIPState *s = BCM2712_MIP(dev);

    memset(s->config, 0, sizeof(s->config));
    memset(s->status_host, 0, sizeof(s->status_host));
    memset(s->status_vpu, 0, sizeof(s->status_vpu));
    memset(s->mask_host, 0xff, sizeof(s->mask_host));
    memset(s->mask_vpu, 0xff, sizeof(s->mask_vpu));
    for (unsigned i = 0; i < s->num_irqs; i++) {
        qemu_set_irq(s->irq[i], 0);
    }
}

static int bcm2712_mip_post_load(void *opaque, int version_id)
{
    BCM2712MIPState *s = opaque;

    for (unsigned i = 0; i < s->num_irqs; i++) {
        bcm2712_mip_update_level(s, s->offset + i);
    }
    return 0;
}

static const VMStateDescription vmstate_bcm2712_mip = {
    .name = TYPE_BCM2712_MIP,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = bcm2712_mip_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(config, BCM2712MIPState, 2),
        VMSTATE_UINT32_ARRAY(mask_host, BCM2712MIPState, 2),
        VMSTATE_UINT32_ARRAY(mask_vpu, BCM2712MIPState, 2),
        VMSTATE_UINT32_ARRAY(status_host, BCM2712MIPState, 2),
        VMSTATE_UINT32_ARRAY(status_vpu, BCM2712MIPState, 2),
        VMSTATE_END_OF_LIST(),
    },
};

static void bcm2712_mip_realize(DeviceState *dev, Error **errp)
{
    BCM2712MIPState *s = BCM2712_MIP(dev);

    if (!s->num_irqs || s->num_irqs > BCM2712_MIP_MAX_IRQS ||
        s->offset >= BCM2712_MIP_MAX_IRQS ||
        s->num_irqs > BCM2712_MIP_MAX_IRQS - s->offset) {
        error_setg(errp, "invalid vector range %u..%u", s->offset,
                   s->offset + s->num_irqs - 1);
        return;
    }
    for (unsigned i = 0; i < s->num_irqs; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }
    msi_nonbroken = true;
}

static void bcm2712_mip_init(Object *obj)
{
    BCM2712MIPState *s = BCM2712_MIP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->control, obj, &bcm2712_mip_control_ops, s,
                          "bcm2712-mip-control", 0xc0);
    memory_region_init_io(&s->message, obj, &bcm2712_mip_message_ops, s,
                          "bcm2712-mip-message", 0x1000);
    sysbus_init_mmio(sbd, &s->control);
    sysbus_init_mmio(sbd, &s->message);
}

static const Property bcm2712_mip_properties[] = {
    DEFINE_PROP_UINT32("num-irqs", BCM2712MIPState, num_irqs, 64),
    DEFINE_PROP_UINT32("offset", BCM2712MIPState, offset, 0),
};

static void bcm2712_mip_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = bcm2712_mip_realize;
    dc->vmsd = &vmstate_bcm2712_mip;
    device_class_set_props(dc, bcm2712_mip_properties);
    device_class_set_legacy_reset(dc, bcm2712_mip_reset);
}

static const TypeInfo bcm2712_mip_type = {
    .name = TYPE_BCM2712_MIP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2712MIPState),
    .instance_init = bcm2712_mip_init,
    .class_init = bcm2712_mip_class_init,
};

static void bcm2712_mip_register_types(void)
{
    type_register_static(&bcm2712_mip_type);
}

type_init(bcm2712_mip_register_types)
