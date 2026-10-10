/*
 * BCM2712 GIC-400 virtualization and CPU-specific alias tests.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"

/* Hardware addresses, independent of the board implementation. */
#define GICD        UINT64_C(0x107fff9000)
#define GICC        UINT64_C(0x107fffa000)
#define GICH        UINT64_C(0x107fffc000)
#define GICH_ALIAS  UINT64_C(0x107fffd000)
#define GICV        UINT64_C(0x107fffe000)
#define NCPUS       4
#define MAINT_IRQ   25

static uint64_t gich_alias(unsigned cpu)
{
    return GICH_ALIAS + cpu * 0x200;
}

static QTestState *gic_init(const char *machine)
{
    QTestState *qts = qtest_initf("-machine %s -serial null", machine);

    qtest_irq_intercept_out_named(qts, "/machine/soc/gic", "sysbus-irq");
    return qts;
}

static void test_aliases(gconstpointer machine)
{
    QTestState *qts = gic_init(machine);
    uint32_t vtr = qtest_readl(qts, GICH + 4);
    uint32_t hcr[NCPUS], vmcr[NCPUS], lr[NCPUS];

    /* GICH_VTR distinguishes these aliases from physical CPU interfaces. */
    g_assert_cmphex(vtr & 0x3f, ==, 3);
    for (unsigned i = 0; i < NCPUS; i++) {
        uint64_t alias = gich_alias(i);

        g_assert_cmphex(qtest_readl(qts, alias + 4), ==, vtr);
        hcr[i] = (i + 1) << 1; /* Leave virtual interrupt delivery disabled. */
        vmcr[i] = ((8 + i) << 27) | ((2 + i) << 21) |
                  ((3 + i) << 18) | 1;
        lr[i] = (1U << 28) | ((i + 1) << 23) | (64 + i);
        qtest_writel(qts, alias, hcr[i]);
        qtest_writel(qts, alias + 8, vmcr[i]);
        qtest_writel(qts, alias + 0x100, lr[i]);
        qtest_writel(qts, alias + 0xf0, 1U << i);
    }

    /* Each address selects its own CPU, regardless of the requesting CPU. */
    for (unsigned i = 0; i < NCPUS; i++) {
        uint64_t alias = gich_alias(i);

        g_assert_cmphex(qtest_readl(qts, alias), ==, hcr[i]);
        g_assert_cmphex(qtest_readl(qts, alias + 8), ==, vmcr[i]);
        g_assert_cmphex(qtest_readl(qts, alias + 0x100), ==, lr[i]);
        g_assert_cmphex(qtest_readl(qts, alias + 0xf0), ==, 1U << i);
    }
    /* qtest's banked accesses select CPU0. */
    g_assert_cmphex(qtest_readl(qts, GICH), ==, hcr[0]);
    g_assert_cmphex(qtest_readl(qts, GICH + 8), ==, vmcr[0]);
    g_assert_cmphex(qtest_readl(qts, GICH + 0x100), ==, lr[0]);
    g_assert_cmphex(qtest_readl(qts, GICC + 4), ==, 0);

    /* The GIC-400 implements five physical interrupt priority bits. */
    qtest_writel(qts, GICC + 4, 0xff);
    g_assert_cmphex(qtest_readl(qts, GICC + 4), ==, 0xf8);
    qtest_writeb(qts, GICD + 0x400 + 64, 0xff);
    g_assert_cmphex(qtest_readb(qts, GICD + 0x400 + 64), ==, 0xf8);

    qtest_system_reset(qts);
    for (unsigned i = 0; i < NCPUS; i++) {
        uint64_t alias = gich_alias(i);

        g_assert_cmphex(qtest_readl(qts, alias), ==, 0);
        g_assert_cmphex(qtest_readl(qts, alias + 0x100), ==, 0);
        g_assert_cmphex(qtest_readl(qts, alias + 0xf0), ==, 0);
        g_assert_cmphex(qtest_readl(qts, alias + 4), ==, vtr);
    }
    qtest_quit(qts);
}

static void test_virtual_irq(gconstpointer machine)
{
    QTestState *qts = gic_init(machine);

    for (unsigned i = 0; i < NCPUS; i++) {
        uint64_t alias = gich_alias(i);

        /* Enable Group0, unmask its priorities, and pend a software IRQ. */
        qtest_writel(qts, alias + 8, (31U << 27) | 1);
        qtest_writel(qts, alias + 0x100, (1U << 28) | (64 + i));
        qtest_writel(qts, alias, 1);
        for (unsigned cpu = 0; cpu < NCPUS; cpu++) {
            g_assert_cmpint(qtest_get_irq(qts, 2 * NCPUS + cpu), ==,
                            cpu == i);
            g_assert_false(qtest_get_irq(qts, cpu));
            g_assert_false(qtest_get_irq(qts, 3 * NCPUS + cpu));
        }
        qtest_writel(qts, alias + 0x100, 0);
        g_assert_false(qtest_get_irq(qts, 2 * NCPUS + i));
    }

    /* CPU0 can acknowledge and complete an interrupt through its GICV view. */
    qtest_writel(qts, gich_alias(0) + 0x100, (1U << 28) | 65);
    g_assert_true(qtest_get_irq(qts, 2 * NCPUS));
    g_assert_cmphex(qtest_readl(qts, GICV + 0xc), ==, 65);
    g_assert_false(qtest_get_irq(qts, 2 * NCPUS));
    g_assert_cmphex(qtest_readl(qts, GICH + 0x100) & (3U << 28), ==,
                    2U << 28);
    qtest_writel(qts, GICV + 0x10, 65);
    g_assert_cmphex(qtest_readl(qts, GICH + 0x100) & (3U << 28), ==, 0);
    qtest_quit(qts);
}

static void test_maintenance_irq(gconstpointer machine)
{
    QTestState *qts = gic_init(machine);

    for (unsigned i = 0; i < NCPUS; i++) {
        /* Empty list registers and HCR.UIE generate underflow maintenance. */
        qtest_writel(qts, gich_alias(i), 3);
        g_assert_cmphex(qtest_readl(qts, gich_alias(i) + 0x10), ==, 2);
        for (unsigned cpu = 0; cpu < NCPUS; cpu++) {
            g_assert_cmpint(qtest_get_irq(qts, 4 * NCPUS + cpu), ==,
                            cpu == i);
        }
        qtest_writel(qts, gich_alias(i), 0);
        g_assert_false(qtest_get_irq(qts, 4 * NCPUS + i));
    }

    /* Outgoing intercepts disconnect wires; use CPU0's input on a new VM. */
    qtest_quit(qts);
    qts = qtest_initf("-machine %s -serial null", (const char *)machine);
    qtest_irq_intercept_in(qts, "/machine/soc/cpu0");

    /* The maintenance output is wired back to CPU0's physical PPI25. */
    qtest_writel(qts, GICD, 1);
    qtest_writel(qts, GICC, 1);
    qtest_writel(qts, GICC + 4, 0xff);
    qtest_writel(qts, GICD + 0x100, 1U << MAINT_IRQ);
    qtest_writel(qts, gich_alias(0), 3);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(qtest_readl(qts, GICC + 0xc), ==, MAINT_IRQ);
    qtest_writel(qts, gich_alias(0), 0);
    qtest_writel(qts, GICC + 0x10, MAINT_IRQ);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(qtest_readl(qts, GICC + 0xc), ==, 1023);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("/bcm2712/raspi5b/gic/aliases", "raspi5b",
                        test_aliases);
    qtest_add_data_func("/bcm2712/raspi500/gic/aliases", "raspi500",
                        test_aliases);
    qtest_add_data_func("/bcm2712/raspi5b/gic/virtual-irq", "raspi5b",
                        test_virtual_irq);
    qtest_add_data_func("/bcm2712/raspi500/gic/virtual-irq", "raspi500",
                        test_virtual_irq);
    qtest_add_data_func("/bcm2712/raspi5b/gic/maintenance-irq", "raspi5b",
                        test_maintenance_irq);
    qtest_add_data_func("/bcm2712/raspi500/gic/maintenance-irq", "raspi500",
                        test_maintenance_irq);
    return g_test_run();
}
