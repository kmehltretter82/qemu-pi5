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
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
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
    uint64_t fixed_ram_size;
};

static void *raspi5_get_dtb(const struct arm_boot_info *info, int *size)
{
    Raspi5MachineState *s = container_of(info, Raspi5MachineState, bootinfo);
    Raspi5MachineClass *rmc = RASPI5_BASE_MACHINE_GET_CLASS(s);
    void *fdt = create_device_tree(size);
    uint32_t gic_phandle = qemu_fdt_alloc_phandle(fdt);
    uint32_t uartclk_phandle = qemu_fdt_alloc_phandle(fdt);
    uint32_t sdclk_phandle = qemu_fdt_alloc_phandle(fdt);
    const char uart_compat[] = "arm,pl011\0arm,primecell";
    const char clock_names[] = "uartclk\0apb_pclk";
    const char psci_compat[] = "arm,psci-1.0\0arm,psci-0.2";
    const char sd_compat[] = "brcm,bcm2712-sdhci\0brcm,sdhci-brcmstb";
    const char sd_reg_names[] = "host\0cfg";
    const char *gic = "/interrupt-controller@107fff9000";
    const char *uart = "/serial@107d001000";
    const char *sd = "/mmc@1000fff000";
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
    qemu_fdt_setprop_string(fdt, "/aliases", "mmc0", sd);

    qemu_fdt_add_subnode(fdt, "/memory@0");
    qemu_fdt_setprop_string(fdt, "/memory@0", "device_type", "memory");
    qemu_fdt_setprop_sized_cells(fdt, "/memory@0", "reg",
                                2, 0, 2, info->ram_size);

    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0);
    {
        const char *l3 = "/cpus/l3-cache";
        uint32_t l3_phandle = qemu_fdt_alloc_phandle(fdt);
        uint32_t cpu_phandles[BCM2712_NCPUS];

        /* Match Linux's bcm2712.dtsi: private L2s and a shared external L3. */
        qemu_fdt_add_subnode(fdt, l3);
        qemu_fdt_setprop_string(fdt, l3, "compatible", "cache");
        qemu_fdt_setprop_cell(fdt, l3, "phandle", l3_phandle);
        qemu_fdt_setprop_cell(fdt, l3, "cache-size", 0x200000);
        qemu_fdt_setprop_cell(fdt, l3, "cache-line-size", 64);
        qemu_fdt_setprop_cell(fdt, l3, "cache-sets", 2048);
        qemu_fdt_setprop_cell(fdt, l3, "cache-level", 3);
        qemu_fdt_setprop(fdt, l3, "cache-unified", NULL, 0);

        for (int i = BCM2712_NCPUS - 1; i >= 0; i--) {
            g_autofree char *node = g_strdup_printf("/cpus/cpu@%x", i << 8);
            g_autofree char *l2 = g_strdup_printf("%s/l2-cache", node);
            uint32_t l2_phandle = qemu_fdt_alloc_phandle(fdt);

            cpu_phandles[i] = qemu_fdt_alloc_phandle(fdt);

            qemu_fdt_add_subnode(fdt, node);
            qemu_fdt_setprop_string(fdt, node, "device_type", "cpu");
            qemu_fdt_setprop_string(fdt, node, "compatible", "arm,cortex-a76");
            qemu_fdt_setprop_cell(fdt, node, "reg", i << 8);
            qemu_fdt_setprop_cell(fdt, node, "phandle", cpu_phandles[i]);
            qemu_fdt_setprop_string(fdt, node, "enable-method", "psci");
            qemu_fdt_setprop_cell(fdt, node, "d-cache-size", 0x10000);
            qemu_fdt_setprop_cell(fdt, node, "d-cache-line-size", 64);
            qemu_fdt_setprop_cell(fdt, node, "d-cache-sets", 256);
            qemu_fdt_setprop_cell(fdt, node, "i-cache-size", 0x10000);
            qemu_fdt_setprop_cell(fdt, node, "i-cache-line-size", 64);
            qemu_fdt_setprop_cell(fdt, node, "i-cache-sets", 256);
            qemu_fdt_setprop_cell(fdt, node, "next-level-cache", l2_phandle);

            qemu_fdt_add_subnode(fdt, l2);
            qemu_fdt_setprop_string(fdt, l2, "compatible", "cache");
            qemu_fdt_setprop_cell(fdt, l2, "phandle", l2_phandle);
            qemu_fdt_setprop_cell(fdt, l2, "cache-size", 0x80000);
            qemu_fdt_setprop_cell(fdt, l2, "cache-line-size", 64);
            qemu_fdt_setprop_cell(fdt, l2, "cache-sets", 1024);
            qemu_fdt_setprop_cell(fdt, l2, "cache-level", 2);
            qemu_fdt_setprop(fdt, l2, "cache-unified", NULL, 0);
            qemu_fdt_setprop_cell(fdt, l2, "next-level-cache", l3_phandle);
        }

        if (arm_feature(&s->soc.cpu[0].env, ARM_FEATURE_PMU)) {
            const char *pmu = "/arm-pmu";

            /* The order must match the CPU affinity of SPI16 through SPI19. */
            qemu_fdt_add_subnode(fdt, pmu);
            qemu_fdt_setprop_string(fdt, pmu, "compatible",
                                    "arm,cortex-a76-pmu");
            qemu_fdt_setprop_cells(fdt, pmu, "interrupts",
                                   0, BCM2712_PMU_SPI_BASE, 4,
                                   0, BCM2712_PMU_SPI_BASE + 1, 4,
                                   0, BCM2712_PMU_SPI_BASE + 2, 4,
                                   0, BCM2712_PMU_SPI_BASE + 3, 4);
            qemu_fdt_setprop_cells(fdt, pmu, "interrupt-affinity",
                                   cpu_phandles[0], cpu_phandles[1],
                                   cpu_phandles[2], cpu_phandles[3]);
        }
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

    qemu_fdt_add_subnode(fdt, "/sd-clock");
    qemu_fdt_setprop_string(fdt, "/sd-clock", "compatible", "fixed-clock");
    qemu_fdt_setprop_cell(fdt, "/sd-clock", "#clock-cells", 0);
    qemu_fdt_setprop_cell(fdt, "/sd-clock", "clock-frequency",
                           BCM2712_SD_CLOCK);
    qemu_fdt_setprop_cell(fdt, "/sd-clock", "phandle", sdclk_phandle);
    qemu_fdt_add_subnode(fdt, sd);
    qemu_fdt_setprop(fdt, sd, "compatible", sd_compat, sizeof(sd_compat));
    qemu_fdt_setprop_sized_cells(fdt, sd, "reg",
                                2, BCM2712_SDIO1, 2, 0x260,
                                2, BCM2712_SDIO1_CFG, 2, 0x200);
    qemu_fdt_setprop(fdt, sd, "reg-names", sd_reg_names, sizeof(sd_reg_names));
    qemu_fdt_setprop_cells(fdt, sd, "interrupts", 0, BCM2712_SDIO1_SPI, 4);
    qemu_fdt_setprop_cell(fdt, sd, "clocks", sdclk_phandle);
    qemu_fdt_setprop_string(fdt, sd, "clock-names", "sw_sdio");
    qemu_fdt_setprop_cell(fdt, sd, "bus-width", 4);
    qemu_fdt_setprop_cell(fdt, sd, "max-frequency", 50000000);
    qemu_fdt_setprop(fdt, sd, "sd-highspeed", NULL, 0);
    /* The modeled slot is wired directly, without a pin multiplexer. */
    qemu_fdt_setprop_string(fdt, sd, "pinctrl-names", "default");
    qemu_fdt_setprop(fdt, sd, "pinctrl-0", NULL, 0);
    return fdt;
}

static void raspi5_init(MachineState *machine)
{
    Raspi5MachineState *s = RASPI5_BASE_MACHINE(machine);
    Raspi5MachineClass *rmc = RASPI5_BASE_MACHINE_GET_CLASS(machine);
    uint64_t ram = machine->ram_size;
    DriveInfo *di;

    if (rmc->fixed_ram_size && ram != rmc->fixed_ram_size) {
        error_report("%s requires %" PRIu64 " GiB RAM",
                     MACHINE_GET_CLASS(machine)->name,
                     rmc->fixed_ram_size / GiB);
        exit(1);
    }
    if (ram < GiB || ram > 16 * GiB || (ram & (ram - 1))) {
        error_report("%s requires 1, 2, 4, 8 or 16 GiB RAM",
                     MACHINE_GET_CLASS(machine)->name);
        exit(1);
    }
    if (machine->firmware) {
        error_report("Pi 5 firmware boot is not implemented; use -kernel");
        exit(1);
    }

    memory_region_add_subregion(get_system_memory(), 0, machine->ram);
    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_BCM2712);
    sysbus_realize(SYS_BUS_DEVICE(&s->soc), &error_fatal);

    /* Leave an empty slot unless a legacy SD backend is supplied. */
    di = drive_get(IF_SD, 0, 0);
    if (di) {
        DeviceState *card = qdev_new(TYPE_SD_CARD);

        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(di),
                                &error_fatal);
        qdev_realize_and_unref(card, s->soc.sdio1.bus, &error_fatal);
    }

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
    mc->block_default_type = IF_SD;
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
    rmc->fixed_ram_size = 8 * GiB;
}

static void raspi500p_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    Raspi5MachineClass *rmc = RASPI5_BASE_MACHINE_CLASS(oc);

    mc->desc = "Raspberry Pi 500+ (experimental core platform)";
    mc->default_ram_size = 16 * GiB;
    rmc->compatible = "raspberrypi,500\0brcm,bcm2712";
    rmc->compatible_size = sizeof("raspberrypi,500\0brcm,bcm2712");
    rmc->model = "Raspberry Pi 500+ (QEMU experimental)";
    rmc->fixed_ram_size = 16 * GiB;
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
        .name = MACHINE_TYPE_NAME("raspi500p"),
        .parent = TYPE_RASPI5_BASE_MACHINE,
        .class_init = raspi500p_class_init,
        .interfaces = aarch64_machine_interfaces,
    }, {
        .name = MACHINE_TYPE_NAME("raspi500"),
        .parent = TYPE_RASPI5_BASE_MACHINE,
        .class_init = raspi500_class_init,
        .interfaces = aarch64_machine_interfaces,
    },
};

DEFINE_TYPES(raspi5_types)
