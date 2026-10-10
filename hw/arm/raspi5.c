/*
 * Experimental Raspberry Pi 5 / 500 direct-kernel-boot machines.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/bcm2712.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "system/address-spaces.h"
#include "system/device_tree.h"

#define TYPE_RASPI5_BASE_MACHINE MACHINE_TYPE_NAME("raspi5-base")
OBJECT_DECLARE_TYPE(Raspi5MachineState, Raspi5MachineClass, RASPI5_BASE_MACHINE)

struct Raspi5MachineState {
    MachineState parent_obj;
    BCM2712State soc;
    struct arm_boot_info bootinfo;
};

struct Raspi5MachineClass {
    MachineClass parent_class;
    const char *compatible;
    size_t compatible_size;
    const char *model;
    bool pi500;
};

static void *raspi5_get_dtb(const struct arm_boot_info *info, int *size)
{
    Raspi5MachineState *s = container_of(info, Raspi5MachineState, bootinfo);
    Raspi5MachineClass *rmc = RASPI5_BASE_MACHINE_GET_CLASS(s);
    void *fdt = create_device_tree(size);
    uint32_t gic_phandle = qemu_fdt_alloc_phandle(fdt);
    uint32_t uartclk_phandle = qemu_fdt_alloc_phandle(fdt);
    const char uart_compat[] = "arm,pl011\0arm,primecell";
    const char clock_names[] = "uartclk\0apb_pclk";
    const char psci_compat[] = "arm,psci-1.0\0arm,psci-0.2";
    const char *gic = "/interrupt-controller@107fff9000";
    const char *uart = "/serial@107d001000";
    /* This is deliberately a minimal DT, describing only modeled devices. */
    qemu_fdt_setprop(fdt, "/", "compatible", rmc->compatible,
                     rmc->compatible_size);
    qemu_fdt_setprop_string(fdt, "/", "model", rmc->model);
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/", "interrupt-parent", gic_phandle);
    qemu_fdt_add_subnode(fdt, "/chosen");
    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path", uart);
    qemu_fdt_add_subnode(fdt, "/aliases");
    qemu_fdt_setprop_string(fdt, "/aliases", "serial0", uart);

    qemu_fdt_add_subnode(fdt, "/memory@0");
    qemu_fdt_setprop_string(fdt, "/memory@0", "device_type", "memory");
    qemu_fdt_setprop_sized_cells(fdt, "/memory@0", "reg",
                                2, 0, 2, info->ram_size);

    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0);
    for (int i = BCM2712_NCPUS - 1; i >= 0; i--) {
        g_autofree char *node = g_strdup_printf("/cpus/cpu@%x", i << 8);

        qemu_fdt_add_subnode(fdt, node);
        qemu_fdt_setprop_string(fdt, node, "device_type", "cpu");
        qemu_fdt_setprop_string(fdt, node, "compatible", "arm,cortex-a76");
        qemu_fdt_setprop_cell(fdt, node, "reg", i << 8);
        qemu_fdt_setprop_string(fdt, node, "enable-method", "psci");
    }

    qemu_fdt_add_subnode(fdt, "/psci");
    qemu_fdt_setprop(fdt, "/psci", "compatible", psci_compat,
                     sizeof(psci_compat));
    qemu_fdt_setprop_string(fdt, "/psci", "method", "smc");

    qemu_fdt_add_subnode(fdt, gic);
    qemu_fdt_setprop_string(fdt, gic, "compatible", "arm,gic-400");
    qemu_fdt_setprop_cell(fdt, gic, "phandle", gic_phandle);
    qemu_fdt_setprop_cell(fdt, gic, "#interrupt-cells", 3);
    qemu_fdt_setprop_cell(fdt, gic, "#address-cells", 0);
    qemu_fdt_setprop(fdt, gic, "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_sized_cells(fdt, gic, "reg",
                                2, BCM2712_GIC_DIST, 2, 0x1000,
                                2, BCM2712_GIC_CPU, 2, 0x2000,
                                2, BCM2712_GIC_HYP, 2, 0x2000,
                                2, BCM2712_GIC_VCPU, 2, 0x2000);
    qemu_fdt_setprop_cells(fdt, gic, "interrupts", 1, 9, 0xf04);

    qemu_fdt_add_subnode(fdt, "/timer");
    qemu_fdt_setprop_string(fdt, "/timer", "compatible", "arm,armv8-timer");
    qemu_fdt_setprop_cells(fdt, "/timer", "interrupts",
                           1, 13, 0xf08, 1, 14, 0xf08,
                           1, 11, 0xf08, 1, 10, 0xf08);

    qemu_fdt_add_subnode(fdt, "/uart-clock");
    qemu_fdt_setprop_string(fdt, "/uart-clock", "compatible", "fixed-clock");
    qemu_fdt_setprop_cell(fdt, "/uart-clock", "#clock-cells", 0);
    qemu_fdt_setprop_cell(fdt, "/uart-clock", "clock-frequency",
                           BCM2712_UART_CLOCK);
    qemu_fdt_setprop_cell(fdt, "/uart-clock", "phandle", uartclk_phandle);
    qemu_fdt_add_subnode(fdt, uart);
    qemu_fdt_setprop(fdt, uart, "compatible", uart_compat, sizeof(uart_compat));
    qemu_fdt_setprop_sized_cells(fdt, uart, "reg",
                                2, BCM2712_UART10, 2, 0x1000);
    qemu_fdt_setprop_cells(fdt, uart, "interrupts", 0, BCM2712_UART10_SPI, 4);
    qemu_fdt_setprop_cells(fdt, uart, "clocks", uartclk_phandle,
                           uartclk_phandle);
    qemu_fdt_setprop(fdt, uart, "clock-names", clock_names,
                     sizeof(clock_names));
    return fdt;
}

static void raspi5_init(MachineState *machine)
{
    Raspi5MachineState *s = RASPI5_BASE_MACHINE(machine);
    Raspi5MachineClass *rmc = RASPI5_BASE_MACHINE_GET_CLASS(machine);
    uint64_t ram = machine->ram_size;

    if (ram < GiB || ram > 16 * GiB || (ram & (ram - 1)) ||
        (rmc->pi500 && ram != 8 * GiB)) {
        error_report("%s requires %s RAM", MACHINE_GET_CLASS(machine)->name,
                     rmc->pi500 ? "8 GiB" : "1, 2, 4, 8 or 16 GiB");
        exit(1);
    }
    if (machine->firmware) {
        error_report("Pi 5 firmware boot is not implemented; use -kernel");
        exit(1);
    }

    memory_region_add_subregion(get_system_memory(), 0, machine->ram);
    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_BCM2712);
    sysbus_realize(SYS_BUS_DEVICE(&s->soc), &error_fatal);

    s->bootinfo.ram_size = ram;
    s->bootinfo.board_id = -1;
    s->bootinfo.loader_start = 0;
    s->bootinfo.get_dtb = raspi5_get_dtb;
    s->bootinfo.psci_conduit = QEMU_PSCI_CONDUIT_SMC;
    arm_load_kernel(&s->soc.cpu[0], machine, &s->bootinfo);
}

static void raspi5_base_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a76"), NULL
    };

    mc->init = raspi5_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a76");
    mc->valid_cpu_types = valid_cpu_types;
    mc->min_cpus = mc->max_cpus = mc->default_cpus = BCM2712_NCPUS;
    mc->default_ram_id = "bcm2712.ram";
    mc->no_parallel = true;
    mc->no_floppy = true;
    mc->no_cdrom = true;
}

static void raspi5b_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    Raspi5MachineClass *rmc = RASPI5_BASE_MACHINE_CLASS(oc);

    mc->desc = "Raspberry Pi 5B (experimental core platform)";
    mc->alias = "raspi5";
    mc->default_ram_size = 2 * GiB;
    rmc->compatible = "raspberrypi,5-model-b\0brcm,bcm2712";
    rmc->compatible_size = sizeof("raspberrypi,5-model-b\0brcm,bcm2712");
    rmc->model = "Raspberry Pi 5 Model B (QEMU experimental)";
}

static void raspi500_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    Raspi5MachineClass *rmc = RASPI5_BASE_MACHINE_CLASS(oc);

    mc->desc = "Raspberry Pi 500 (experimental core platform)";
    mc->default_ram_size = 8 * GiB;
    rmc->compatible = "raspberrypi,500\0brcm,bcm2712";
    rmc->compatible_size = sizeof("raspberrypi,500\0brcm,bcm2712");
    rmc->model = "Raspberry Pi 500 (QEMU experimental)";
    rmc->pi500 = true;
}

static const TypeInfo raspi5_types[] = {
    {
        .name = TYPE_RASPI5_BASE_MACHINE,
        .parent = TYPE_MACHINE,
        .abstract = true,
        .instance_size = sizeof(Raspi5MachineState),
        .class_size = sizeof(Raspi5MachineClass),
        .class_init = raspi5_base_class_init,
    }, {
        .name = MACHINE_TYPE_NAME("raspi5b"),
        .parent = TYPE_RASPI5_BASE_MACHINE,
        .class_init = raspi5b_class_init,
        .interfaces = aarch64_machine_interfaces,
    }, {
        .name = MACHINE_TYPE_NAME("raspi500"),
        .parent = TYPE_RASPI5_BASE_MACHINE,
        .class_init = raspi500_class_init,
        .interfaces = aarch64_machine_interfaces,
    },
};

DEFINE_TYPES(raspi5_types)
