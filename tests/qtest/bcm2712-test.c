/*
 * BCM2712 board wiring tests.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"

/* Independent hardware addresses, not imported from the implementation. */
#define UART10  UINT64_C(0x107d001000)
#define GICD    UINT64_C(0x107fff9000)
#define GICC    UINT64_C(0x107fffa000)
#define UART_IRQ 153

static void test_uart_irq(gconstpointer machine)
{
    QTestState *qts = qtest_initf("-machine %s -serial null", (char *)machine);

    /* Enable and route UART10's SPI to CPU0. */
    qtest_writel(qts, GICD, 1);
    qtest_writel(qts, GICC, 1);
    qtest_writel(qts, GICC + 4, 0xff);
    qtest_writeb(qts, GICD + 0x800 + UART_IRQ, 1);
    qtest_writel(qts, GICD + 0x100 + (UART_IRQ / 32) * 4,
                 1U << (UART_IRQ % 32));
    g_assert_cmphex(qtest_readl(qts, GICC + 0xc), ==, 1023);

    /* A transmitted character raises the PL011 TX interrupt. */
    qtest_writel(qts, UART10 + 0x30, 0x301);
    qtest_writel(qts, UART10 + 0x38, 0x20);
    qtest_writel(qts, UART10, 'Q');
    g_assert_cmphex(qtest_readl(qts, UART10 + 0x40), ==, 0x20);
    g_assert_cmphex(qtest_readl(qts, GICC + 0xc), ==, UART_IRQ);

    /* Masking the source and completing the IRQ leaves no pending IRQ. */
    qtest_writel(qts, UART10 + 0x38, 0);
    qtest_writel(qts, GICC + 0x10, UART_IRQ);
    g_assert_cmphex(qtest_readl(qts, GICC + 0xc), ==, 1023);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, UART10 + 0x38), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GICD), ==, 0);
    qtest_quit(qts);
}

static void test_ram(void)
{
    QTestState *qts = qtest_init("-machine raspi500 -serial null");
    const uint64_t addresses[] = {
        0x1000, UINT64_C(0x100001000), UINT64_C(0x1fffffff8),
    };

    /* Distinct patterns detect a missing or aliased upper RAM region. */
    for (unsigned i = 0; i < G_N_ELEMENTS(addresses); i++) {
        qtest_writeq(qts, addresses[i], UINT64_C(0x123456789abcdef0) + i);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(addresses); i++) {
        g_assert_cmphex(qtest_readq(qts, addresses[i]), ==,
                        UINT64_C(0x123456789abcdef0) + i);
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("/bcm2712/raspi5b/uart-irq", "raspi5b", test_uart_irq);
    qtest_add_data_func("/bcm2712/raspi500/uart-irq", "raspi500",
                        test_uart_irq);
    qtest_add_func("/bcm2712/raspi500/ram", test_ram);
    return g_test_run();
}
