/*
 * BCM2712 board wiring tests.
 * MMIO-only VMs keep dummy CPUs stopped; card tests run virtual timers.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

/* Independent hardware addresses, not imported from the implementation. */
#define UART10  UINT64_C(0x107d001000)
#define GICD    UINT64_C(0x107fff9000)
#define GICC    UINT64_C(0x107fffa000)
#define UART_IRQ 153
#define SDIO1   UINT64_C(0x1000fff000)
#define SD_CFG  UINT64_C(0x1000fff400)
#define SD_IRQ  305 /* GIC SPI 273. */
#define SD_CAPS UINT64_C(0x000000001568c881)
#define SD_SECTOR_SIZE 512
#define SD_CARD_SIZE (UINT64_C(4) * 1024 * 1024 * 1024)

#define SD_ARGUMENT  0x08
#define SD_COMMAND   0x0e
#define SD_RESPONSE  0x10
#define SD_DATA      0x20
#define SD_PRESENT   0x24
#define SD_CLOCK     0x2c
#define SD_RESET     0x2f
#define SD_STATUS    0x30
#define SD_ENABLE    0x34
#define SD_SIGNAL    0x38
#define SD_CAPAB     0x40
#define SD_ADMA_ERROR 0x54
#define SD_ADMA_ADDR 0x58
#define SD_VERSION   0xfe
#define SD_INSERTED  (1U << 16)
#define SD_FORCE_CD  (1U << 31)
#define SD_FORCE_ABSENT (1U << 30)

static void enable_gic_irq(QTestState *qts, unsigned irq)
{
    qtest_writel(qts, GICD, 1);
    qtest_writel(qts, GICC, 1);
    qtest_writel(qts, GICC + 4, 0xff);
    qtest_writeb(qts, GICD + 0x800 + irq, 1);
    qtest_writel(qts, GICD + 0x100 + (irq / 32) * 4, 1U << (irq % 32));
}

static void test_uart_irq(gconstpointer machine)
{
    QTestState *qts = qtest_initf("-machine %s -S -serial null",
                                (char *)machine);

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

static void test_ram(gconstpointer machine)
{
    QTestState *qts = qtest_initf("-machine %s -S -serial null",
                                (char *)machine);
    const uint64_t size = !strcmp(machine, "raspi500p") ?
                         UINT64_C(0x400000000) : UINT64_C(0x200000000);
    const uint64_t addresses[] = {
        0x1000, UINT64_C(0x100001000), size - 8,
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

static void test_sd_registers(gconstpointer machine)
{
    QTestState *qts = qtest_initf("-machine %s -S -serial null",
                                (char *)machine);
    const unsigned cfg_regs[] = {0, 0x44, 0x4c, 0x1ac};

    g_assert_cmphex(qtest_readw(qts, SDIO1 + SD_VERSION) & 0xff, ==, 2);
    g_assert_cmphex(qtest_readq(qts, SDIO1 + SD_CAPAB), ==, SD_CAPS);
    qtest_writeq(qts, SDIO1 + SD_CAPAB, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, SDIO1 + SD_CAPAB), ==, SD_CAPS);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SD_PRESENT) & SD_INSERTED, ==, 0);

    /* The driver can force card detection independently of the SD bus. */
    qtest_writel(qts, SD_CFG, SD_FORCE_CD);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SD_PRESENT) & SD_INSERTED, ==,
                    SD_INSERTED);

    /* Forcing detection cannot manufacture a response on an empty SD bus. */
    qtest_writeb(qts, SDIO1 + 0x29, 0xf);
    qtest_writew(qts, SDIO1 + SD_CLOCK, 5);
    qtest_writel(qts, SDIO1 + SD_ENABLE, 0x00010000);
    qtest_writel(qts, SDIO1 + SD_ARGUMENT, 0x1aa);
    qtest_writew(qts, SDIO1 + SD_COMMAND, (8 << 8) | 0x1a);
    g_assert_cmphex(qtest_readw(qts, SDIO1 + SD_STATUS), ==, 0x8000);
    g_assert_cmphex(qtest_readw(qts, SDIO1 + SD_STATUS + 2), ==, 1);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SD_RESPONSE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SD_PRESENT) & SD_INSERTED, ==,
                    SD_INSERTED);
    qtest_writel(qts, SDIO1 + SD_STATUS, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SD_STATUS), ==, 0);

    qtest_writel(qts, SD_CFG, SD_FORCE_CD | SD_FORCE_ABSENT);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SD_PRESENT) & SD_INSERTED, ==, 0);

    for (unsigned i = 0; i < G_N_ELEMENTS(cfg_regs); i++) {
        uint32_t value = 0x12345678 + i;

        qtest_writel(qts, SD_CFG + cfg_regs[i], value);
        g_assert_cmphex(qtest_readl(qts, SD_CFG + cfg_regs[i]), ==, value);
    }
    qtest_writel(qts, SDIO1 + SD_ENABLE, 0xffffffff);
    qtest_writeb(qts, SDIO1 + SD_RESET, 1);
    g_assert_cmphex(qtest_readb(qts, SDIO1 + SD_RESET), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SD_ENABLE), ==, 0);
    for (unsigned i = 0; i < G_N_ELEMENTS(cfg_regs); i++) {
        g_assert_cmphex(qtest_readl(qts, SD_CFG + cfg_regs[i]), ==,
                        0x12345678 + i);
    }

    /* A board reset includes the glue registers, unlike SDHCI SW_RESET. */
    qtest_writel(qts, SDIO1 + 0x78, 0x87654321);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + 0x78), ==, 0x87654321);
    qtest_system_reset(qts);
    for (unsigned i = 0; i < G_N_ELEMENTS(cfg_regs); i++) {
        g_assert_cmphex(qtest_readl(qts, SD_CFG + cfg_regs[i]), ==, 0);
    }
    g_assert_cmphex(qtest_readl(qts, SDIO1 + 0x78), ==, 0);
    g_assert_cmphex(qtest_readq(qts, SDIO1 + SD_CAPAB), ==, SD_CAPS);
    qtest_quit(qts);
}

static uint32_t sd_command(QTestState *qts, unsigned command,
                           uint32_t argument, unsigned flags)
{
    qtest_writel(qts, SDIO1 + SD_STATUS, 0xffffffff);
    qtest_writel(qts, SDIO1 + SD_ARGUMENT, argument);
    qtest_writew(qts, SDIO1 + SD_COMMAND, (command << 8) | flags);
    g_assert_cmphex(qtest_readw(qts, SDIO1 + SD_STATUS + 2), ==, 0);
    g_assert_cmphex(qtest_readw(qts, SDIO1 + SD_STATUS) & 1, ==, 1);
    return qtest_readl(qts, SDIO1 + SD_RESPONSE);
}

static uint32_t sd_card_init(QTestState *qts)
{
    uint32_t ocr = 0;
    uint32_t rca;

    qtest_writeb(qts, SDIO1 + SD_RESET, 1);
    qtest_writeb(qts, SDIO1 + 0x29, 0xf); /* Power on at 3.3 V. */
    qtest_writew(qts, SDIO1 + SD_CLOCK, 5);
    g_assert_cmphex(qtest_readw(qts, SDIO1 + SD_CLOCK) & 7, ==, 7);
    qtest_writel(qts, SDIO1 + SD_ENABLE, 0xffffffff);
    sd_command(qts, 0, 0, 0);
    g_assert_cmphex(sd_command(qts, 8, 0x1aa, 0x1a), ==, 0x1aa);

    /* An enquiry defers power-up until the card's virtual timer fires. */
    sd_command(qts, 55, 0, 0x1a);
    g_assert_cmphex(sd_command(qts, 41, 0, 2) & (1U << 31), ==, 0);
    qtest_clock_step(qts, 1000000);
    sd_command(qts, 55, 0, 0x1a);
    g_assert_cmphex(sd_command(qts, 41, 0, 2) & (1U << 31), ==, 1U << 31);

    for (unsigned i = 0; i < 100 && !(ocr & (1U << 31)); i++) {
        sd_command(qts, 55, 0, 0x1a);
        ocr = sd_command(qts, 41, 0x40ff8000, 2);
        qtest_clock_step(qts, 1000000);
    }
    /* Use an SDHC card so command arguments are sector numbers. */
    g_assert_cmphex(ocr & 0xc0000000, ==, 0xc0000000);
    sd_command(qts, 2, 0, 9);
    rca = sd_command(qts, 3, 0, 0x1a) & 0xffff0000;
    g_assert_cmphex(rca, !=, 0);
    sd_command(qts, 7, rca, 0x1b);
    return rca;
}

static void sd_read_sector(QTestState *qts, uint32_t sector, uint8_t *data)
{
    qtest_writew(qts, SDIO1 + 4, SD_SECTOR_SIZE);
    qtest_writew(qts, SDIO1 + 6, 1);
    qtest_writew(qts, SDIO1 + 0xc, 0x10); /* PIO, read, single block. */
    sd_command(qts, 17, sector, 0x3a);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SD_PRESENT) & 0x800, ==, 0x800);
    for (unsigned i = 0; i < SD_SECTOR_SIZE; i += 4) {
        uint32_t value = qtest_readl(qts, SDIO1 + SD_DATA);

        for (unsigned j = 0; j < 4; j++) {
            data[i + j] = value >> (8 * j);
        }
    }
    g_assert_cmphex(qtest_readw(qts, SDIO1 + SD_STATUS) & 2, ==, 2);
}

static void sd_write_sector(QTestState *qts, uint32_t sector,
                            const uint8_t *data)
{
    qtest_writew(qts, SDIO1 + 4, SD_SECTOR_SIZE);
    qtest_writew(qts, SDIO1 + 6, 1);
    qtest_writew(qts, SDIO1 + 0xc, 0); /* PIO, write, single block. */
    sd_command(qts, 24, sector, 0x3a);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SD_PRESENT) & 0x400, ==, 0x400);
    for (unsigned i = 0; i < SD_SECTOR_SIZE; i += 4) {
        uint32_t value = 0;

        for (unsigned j = 0; j < 4; j++) {
            value |= (uint32_t)data[i + j] << (8 * j);
        }
        qtest_writel(qts, SDIO1 + SD_DATA, value);
    }
    g_assert_cmphex(qtest_readw(qts, SDIO1 + SD_STATUS) & 2, ==, 2);
}

static void test_sd_card(gconstpointer machine)
{
    g_autofree char *path = NULL;
    g_autoptr(GError) error = NULL;
    uint8_t source[SD_SECTOR_SIZE];
    uint8_t written[SD_SECTOR_SIZE];
    uint8_t actual[SD_SECTOR_SIZE];
    QTestState *qts;
    uint32_t rca;
    int fd = g_file_open_tmp("bcm2712-sd-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, SD_CARD_SIZE), ==, 0);
    for (unsigned i = 0; i < SD_SECTOR_SIZE; i++) {
        source[i] = i * 17 + 13;
        written[i] = i * 31 + 7;
    }
    g_assert_cmpint(pwrite(fd, source, sizeof(source), 3 * SD_SECTOR_SIZE), ==,
                    sizeof(source));
    close(fd);
    qts = qtest_initf("-machine %s -serial null "
                      "-drive if=sd,file=%s,format=raw", (char *)machine, path);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SD_PRESENT) & SD_INSERTED, ==,
                    SD_INSERTED);
    qtest_writel(qts, SD_CFG, SD_FORCE_CD | SD_FORCE_ABSENT);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SD_PRESENT) & SD_INSERTED, ==, 0);
    qtest_writel(qts, SD_CFG, 0);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SD_PRESENT) & SD_INSERTED, ==,
                    SD_INSERTED);
    rca = sd_card_init(qts);

    /* A real SD command reaches the host IRQ and hardware's GIC SPI. */
    enable_gic_irq(qts, SD_IRQ);
    qtest_writel(qts, SDIO1 + SD_STATUS, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, GICC + 0xc), ==, 1023);
    qtest_writew(qts, SDIO1 + SD_SIGNAL, 1);
    sd_command(qts, 13, rca, 0x1a);
    g_assert_cmphex(qtest_readl(qts, GICC + 0xc), ==, SD_IRQ);
    qtest_writew(qts, SDIO1 + SD_STATUS, 1);
    qtest_writel(qts, GICC + 0x10, SD_IRQ);
    g_assert_cmphex(qtest_readl(qts, GICC + 0xc), ==, 1023);
    qtest_writew(qts, SDIO1 + SD_SIGNAL, 0);

    sd_read_sector(qts, 3, actual);
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));
    sd_write_sector(qts, 17, written);
    sd_read_sector(qts, 17, actual);
    g_assert_cmpmem(actual, sizeof(actual), written, sizeof(written));
    qtest_quit(qts);

    /* Compare the persistent backend, including an untouched source sector. */
    fd = open(path, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, actual, sizeof(actual), 17 * SD_SECTOR_SIZE), ==,
                    sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), written, sizeof(written));
    g_assert_cmpint(pread(fd, actual, sizeof(actual), 3 * SD_SECTOR_SIZE), ==,
                    sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));
    close(fd);
    unlink(path);
}

static void sd_adma_sector(QTestState *qts, uint32_t sector, bool read,
                           uint64_t table, const uint64_t buffers[2])
{
    uint8_t descriptors[2 * 12] = {0};

    /* SDHCI v3 ADMA2-64: LE16 attributes, LE16 length, LE64 address. */
    for (unsigned i = 0; i < 2; i++) {
        uint8_t *descriptor = descriptors + i * 12;

        stw_le_p(descriptor, i == 1 ? 0x23 : 0x21); /* Valid, Trans, End. */
        stw_le_p(descriptor + 2, SD_SECTOR_SIZE / 2);
        stq_le_p(descriptor + 4, buffers[i]);
    }
    qtest_memwrite(qts, table, descriptors, sizeof(descriptors));
    qtest_writeb(qts, SDIO1 + 0x28, 0x18); /* Select ADMA2-64. */
    g_assert_cmphex(qtest_readb(qts, SDIO1 + 0x28) & 0x18, ==, 0x18);
    qtest_writew(qts, SDIO1 + 4, SD_SECTOR_SIZE);
    qtest_writew(qts, SDIO1 + 6, 1);
    qtest_writel(qts, SDIO1 + SD_ADMA_ADDR, table);
    qtest_writel(qts, SDIO1 + SD_ADMA_ADDR + 4, table >> 32);
    g_assert_cmphex(qtest_readq(qts, SDIO1 + SD_ADMA_ADDR), ==, table);
    /* DMA, block count enabled, one block, read or write. */
    qtest_writew(qts, SDIO1 + 0xc, read ? 0x13 : 0x03);
    sd_command(qts, read ? 17 : 24, sector, 0x3a);
    g_assert_cmphex(qtest_readw(qts, SDIO1 + SD_STATUS), ==, 3);
    g_assert_cmphex(qtest_readb(qts, SDIO1 + SD_ADMA_ERROR), ==, 0);
    g_assert_cmphex(qtest_readw(qts, SDIO1 + 6), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SD_PRESENT) & 0xf06, ==, 0);
}

static void test_sd_adma64(void)
{
    const uint64_t table = UINT64_C(0x180010000);
    const uint64_t buffers[2] = {
        UINT64_C(0x100020000), UINT64_C(0x140030000),
    };
    g_autofree char *path = NULL;
    g_autoptr(GError) error = NULL;
    uint8_t source[SD_SECTOR_SIZE];
    uint8_t written[SD_SECTOR_SIZE];
    uint8_t actual[SD_SECTOR_SIZE];
    uint8_t poison[SD_SECTOR_SIZE / 2];
    QTestState *qts;
    int fd = g_file_open_tmp("bcm2712-adma-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, SD_CARD_SIZE), ==, 0);
    for (unsigned i = 0; i < SD_SECTOR_SIZE; i++) {
        source[i] = i * 43 + 19;
        written[i] = i * 29 + 5;
    }
    g_assert_cmpint(pwrite(fd, source, sizeof(source), 3 * SD_SECTOR_SIZE), ==,
                    sizeof(source));
    close(fd);
    qts = qtest_initf("-machine raspi500 -serial null "
                      "-drive if=sd,file=%s,format=raw", path);
    sd_card_init(qts);

    /* An invalid low table and distinct low buffers catch 32-bit truncation. */
    qtest_memset(qts, table & UINT32_MAX, 0, 24);
    memset(poison, 0xa5, sizeof(poison));
    for (unsigned i = 0; i < G_N_ELEMENTS(buffers); i++) {
        qtest_memwrite(qts, buffers[i] & UINT32_MAX, poison, sizeof(poison));
        qtest_memset(qts, buffers[i], 0, sizeof(poison));
    }
    sd_adma_sector(qts, 3, true, table, buffers);
    for (unsigned i = 0; i < G_N_ELEMENTS(buffers); i++) {
        qtest_memread(qts, buffers[i], actual + i * sizeof(poison),
                       sizeof(poison));
    }
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));

    for (unsigned i = 0; i < G_N_ELEMENTS(buffers); i++) {
        qtest_memwrite(qts, buffers[i], written + i * sizeof(poison),
                        sizeof(poison));
    }
    sd_adma_sector(qts, 17, false, table, buffers);
    for (unsigned i = 0; i < G_N_ELEMENTS(buffers); i++) {
        qtest_memset(qts, buffers[i], 0, sizeof(poison));
    }
    sd_adma_sector(qts, 17, true, table, buffers);
    for (unsigned i = 0; i < G_N_ELEMENTS(buffers); i++) {
        qtest_memread(qts, buffers[i], actual + i * sizeof(poison),
                       sizeof(poison));
    }
    g_assert_cmpmem(actual, sizeof(actual), written, sizeof(written));
    for (unsigned i = 0; i < G_N_ELEMENTS(buffers); i++) {
        qtest_memread(qts, buffers[i] & UINT32_MAX, actual, sizeof(poison));
        g_assert_cmpmem(actual, sizeof(poison), poison, sizeof(poison));
    }
    qtest_quit(qts);

    fd = open(path, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, actual, sizeof(actual), 17 * SD_SECTOR_SIZE), ==,
                    sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), written, sizeof(written));
    g_assert_cmpint(pread(fd, actual, sizeof(actual), 3 * SD_SECTOR_SIZE), ==,
                    sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));
    close(fd);
    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("/bcm2712/raspi5b/uart-irq", "raspi5b", test_uart_irq);
    qtest_add_data_func("/bcm2712/raspi500/uart-irq", "raspi500",
                        test_uart_irq);
    qtest_add_data_func("/bcm2712/raspi500/ram", "raspi500", test_ram);
    qtest_add_data_func("/bcm2712/raspi500p/ram", "raspi500p", test_ram);
    qtest_add_data_func("/bcm2712/raspi500p/uart-irq", "raspi500p",
                        test_uart_irq);
    qtest_add_data_func("/bcm2712/raspi5b/sd-registers", "raspi5b",
                        test_sd_registers);
    qtest_add_data_func("/bcm2712/raspi500/sd-registers", "raspi500",
                        test_sd_registers);
    qtest_add_data_func("/bcm2712/raspi500p/sd-registers", "raspi500p",
                        test_sd_registers);
    qtest_add_data_func("/bcm2712/raspi5b/sd-card", "raspi5b", test_sd_card);
    qtest_add_data_func("/bcm2712/raspi500/sd-card", "raspi500", test_sd_card);
    qtest_add_func("/bcm2712/raspi500/sd-adma64", test_sd_adma64);
    return g_test_run();
}
