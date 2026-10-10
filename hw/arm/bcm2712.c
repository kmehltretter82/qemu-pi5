/*
 * Experimental BCM2712 platform: CPU cluster, GIC, debug UART and SDIO1.
 * Register addresses and interrupts follow Linux's bcm2712.dtsi.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/bcm2712.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "system/system.h"
#include "target/arm/gtimer.h"

static qemu_irq bcm2712_ppi(BCM2712State *s, unsigned cpu, unsigned intid)
{
    return qdev_get_gpio_in(DEVICE(&s->gic),
                           BCM2712_GIC_SPIS + cpu * GIC_INTERNAL + intid);
}

static void bcm2712_init(Object *obj)
{
    BCM2712State *s = BCM2712(obj);

    for (unsigned i = 0; i < BCM2712_NCPUS; i++) {
        g_autofree char *name = g_strdup_printf("cpu%u", i);

        object_initialize_child(obj, name, &s->cpu[i],
                                ARM_CPU_TYPE_NAME("cortex-a76"));
    }
    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GIC);
    object_initialize_child(obj, "uart10", &s->uart10, TYPE_PL011);
    object_initialize_child(obj, "sdio1", &s->sdio1, TYPE_BCM2712_SDHCI);
}

static void bcm2712_realize(DeviceState *dev, Error **errp)
{
    BCM2712State *s = BCM2712(dev);
    DeviceState *gic = DEVICE(&s->gic);
    SysBusDevice *gicbus = SYS_BUS_DEVICE(gic);
    DeviceState *uart = DEVICE(&s->uart10);

    for (unsigned i = 0; i < BCM2712_NCPUS; i++) {
        Object *cpu = OBJECT(&s->cpu[i]);

        /* Each core occupies a different Aff1, as on the physical BCM2712. */
        object_property_set_int(cpu, "mp-affinity", i << 8, &error_abort);
        object_property_set_int(cpu, "cntfrq", BCM2712_COUNTER_HZ,
                                &error_abort);
        object_property_set_bool(cpu, "has_el3", false, &error_abort);
        object_property_set_bool(cpu, "start-powered-off", i != 0,
                                 &error_abort);
        if (!qdev_realize(DEVICE(cpu), NULL, errp)) {
            return;
        }
    }

    qdev_prop_set_uint32(gic, "revision", 2);
    qdev_prop_set_uint32(gic, "num-cpu", BCM2712_NCPUS);
    qdev_prop_set_uint32(gic, "num-irq", BCM2712_GIC_IRQS);
    qdev_prop_set_uint32(gic, "num-priority-bits", 5);
    qdev_prop_set_bit(gic, "has-virtualization-extensions", true);
    if (!sysbus_realize(gicbus, errp)) {
        return;
    }
    sysbus_mmio_map(gicbus, 0, BCM2712_GIC_DIST);
    sysbus_mmio_map(gicbus, 1, BCM2712_GIC_CPU);
    sysbus_mmio_map(gicbus, 2, BCM2712_GIC_HYP);
    sysbus_mmio_map(gicbus, 3, BCM2712_GIC_VCPU);
    for (unsigned i = 0; i < BCM2712_NCPUS; i++) {
        DeviceState *cpu = DEVICE(&s->cpu[i]);

        sysbus_mmio_map(gicbus, 4 + BCM2712_NCPUS + i,
                        BCM2712_GIC_HYP + 0x1000 + i * 0x200);
        sysbus_connect_irq(gicbus, i, qdev_get_gpio_in(cpu, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbus, i + BCM2712_NCPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbus, i + 2 * BCM2712_NCPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbus, i + 3 * BCM2712_NCPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_VFIQ));
        sysbus_connect_irq(gicbus, i + 4 * BCM2712_NCPUS,
                           bcm2712_ppi(s, i, 25));
        qdev_connect_gpio_out(cpu, GTIMER_PHYS, bcm2712_ppi(s, i, 30));
        qdev_connect_gpio_out(cpu, GTIMER_VIRT, bcm2712_ppi(s, i, 27));
        qdev_connect_gpio_out(cpu, GTIMER_HYP, bcm2712_ppi(s, i, 26));
        qdev_connect_gpio_out(cpu, GTIMER_SEC, bcm2712_ppi(s, i, 29));
        /* BCM2712 has a separate PMU SPI for each core, not a shared PPI. */
        qdev_connect_gpio_out_named(cpu, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(gic,
                                        BCM2712_PMU_SPI_BASE + i));
    }

    qdev_prop_set_chr(uart, "chardev", serial_hd(0));
    clock_set_hz(qdev_get_clock_in(uart, "clk"), BCM2712_UART_CLOCK);
    if (!sysbus_realize(SYS_BUS_DEVICE(uart), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0, BCM2712_UART10);
    sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0,
                       qdev_get_gpio_in(gic, BCM2712_UART10_SPI));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sdio1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sdio1), 0, BCM2712_SDIO1);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sdio1), 1, BCM2712_SDIO1_CFG);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sdio1), 0,
                       qdev_get_gpio_in(gic, BCM2712_SDIO1_SPI));
}

static void bcm2712_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = bcm2712_realize;
    dc->user_creatable = false;
}

static const TypeInfo bcm2712_type = {
    .name = TYPE_BCM2712,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2712State),
    .instance_init = bcm2712_init,
    .class_init = bcm2712_class_init,
};

static void bcm2712_register_types(void)
{
    type_register_static(&bcm2712_type);
}

type_init(bcm2712_register_types)
