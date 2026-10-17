/*
 * Experimental BCM2712 SDIO2 Wi-Fi wiring and transport tests.
 * These test a synthetic endpoint, not firmware execution or RF behavior.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "libqos/sdhci-cmd.h"
#include "qobject/qdict.h"

/* Independent hardware addresses and identities, not implementation headers. */
#define SDIO1       UINT64_C(0x1000fff000)
#define SDIO2       UINT64_C(0x1001100000)
#define SDIO2_CFG   UINT64_C(0x1001100400)
#define GICD        UINT64_C(0x107fff9000)
#define GICC        UINT64_C(0x107fffa000)
#define STORAGE_IRQ 305
#define WIFI_IRQ    306
#define SD_CAPS     UINT64_C(0x000000001568c881)
#define SD_STATUS   0x30
#define SD_ENABLE   0x34
#define SD_SIGNAL   0x38
#define SD_CARD_IRQ 0x100
#define SD_ADMA_ERR 0x54
#define SD_ADMA_PTR 0x58
#define SD_CARD_PRESENT (1U << 16)

#define IOEX        0x02
#define IORX        0x03
#define INTEN       0x04
#define INTPEND     0x05
#define SBADDRLOW   0x1000a
#define SBADDRMID   0x1000b
#define SBADDRHIGH  0x1000c
#define CHIP_ID     0x18000000
#define INTSTATUS   0x18004020
#define HOSTINTMASK 0x18004024
#define FRAME_IND   (1U << 6)
#define CONTROL_LEN 48
#define GUARD_SIZE  32

static QTestState *wifi_init_extra(const char *machine, bool high_ram,
                                  const char *extra)
{
    return qtest_initf(
        "-machine %s,sdio-wifi=on %s -serial null "
        "-nic none -global cyw43455-sdio.mac=02:00:00:00:00:01 %s",
        machine, high_ram && !strcmp(machine, "raspi5b") ? "-m 8G" : "",
        extra);
}

static QTestState *wifi_init(const char *machine, bool high_ram)
{
    return wifi_init_extra(machine, high_ram, "");
}

static void command(QTestState *qts, unsigned cmd, uint32_t argument)
{
    sdhci_cmd_regs(qts, SDIO2, 0, 0, argument, 0,
                   (cmd << 8) | SDHC_CMD_RESPONSE);
    g_assert_cmphex(qtest_readl(qts, SDIO2 + SD_STATUS) >> 16, ==, 0);
}

static uint8_t cmd52(QTestState *qts, bool write, unsigned function,
                     uint32_t address, uint8_t value)
{
    uint32_t argument = (function << 28) | (address << 9) | value;
    uint32_t response;

    if (write) {
        argument |= (1U << 31) | (1U << 27); /* Write with read-after-write. */
    }
    command(qts, 52, argument);
    response = qtest_readl(qts, SDIO2 + SDHC_RSPREG0);
    g_assert_cmphex(response & ~0xffU, ==, 0);
    return response;
}

static void select_wifi(QTestState *qts)
{
    qtest_writeb(qts, SDIO2 + SDHC_SWRST, SDHC_RESET_ALL);
    qtest_writew(qts, SDIO2 + SDHC_CLKCON, 7);
    /* Software reset disables status latching; opt in before asserting it. */
    qtest_writel(qts, SDIO2 + SD_ENABLE, 0xffff013f);
    command(qts, 5, 0);
    g_assert_cmphex(qtest_readl(qts, SDIO2 + SDHC_RSPREG0), ==, 0xb0ff8000);
    command(qts, 3, 0);
    g_assert_cmphex(qtest_readl(qts, SDIO2 + SDHC_RSPREG0), ==, 0x10000);
    command(qts, 7, 0x10000);
    g_assert_cmphex(cmd52(qts, true, 0, IOEX, 0x0e), ==, 0x0e);
    g_assert_cmphex(cmd52(qts, false, 0, IORX, 0), ==, 0x0e);
}

static uint32_t cmd53_argument(bool read, unsigned function,
                               uint32_t address, bool increment, size_t len)
{
    g_assert_cmpuint(len, >, 0);
    g_assert_cmpuint(len, <=, 512);
    return (read ? 0 : 1U << 31) | (function << 28) |
           (increment ? 1U << 26 : 0) | (address << 9) | (len & 0x1ff);
}

static void pio_transfer(QTestState *qts, bool read, unsigned function,
                         uint32_t address, bool increment,
                         uint8_t *data, size_t len)
{
    g_assert_cmpuint(len % 4, ==, 0);
    qtest_writeb(qts, SDIO2 + 0x28, 0); /* Explicitly leave DMA mode. */
    qtest_writel(qts, SDIO2 + SD_STATUS, UINT32_MAX);
    sdhci_cmd_regs(qts, SDIO2, len, 1,
                   cmd53_argument(read, function, address, increment, len),
                   SDHC_TRNS_BLK_CNT_EN | (read ? SDHC_TRNS_READ : 0),
                   (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    for (size_t i = 0; i < len; i += 4) {
        if (read) {
            stl_le_p(data + i, qtest_readl(qts, SDIO2 + SDHC_BDATA));
        } else {
            qtest_writel(qts, SDIO2 + SDHC_BDATA, ldl_le_p(data + i));
        }
    }
    g_assert_cmphex(qtest_readl(qts, SDIO2 + SD_STATUS) & 0xffff0003, ==, 3);
}

static uint32_t backplane(QTestState *qts, bool read, uint32_t address,
                          uint32_t value)
{
    uint32_t window = address & ~0x7fffU;
    uint8_t data[4];

    g_assert_cmphex(cmd52(qts, true, 1, SBADDRLOW, window >> 8), ==,
                    (window >> 8) & 0xff);
    g_assert_cmphex(cmd52(qts, true, 1, SBADDRMID, window >> 16), ==,
                    (window >> 16) & 0xff);
    g_assert_cmphex(cmd52(qts, true, 1, SBADDRHIGH, window >> 24), ==,
                    window >> 24);
    stl_le_p(data, value);
    pio_transfer(qts, read, 1, 0x8000 | (address & 0x7fff), true,
                  data, sizeof(data));
    return ldl_le_p(data);
}

static void test_default_absent(gconstpointer opaque)
{
    const char *machine = opaque;
    QTestState *qts = qtest_initf("-machine %s -S -serial null -nic none",
                                  machine);
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': {"
        " 'path': '/machine/soc/sdio2', 'property': 'realized' } }");

    g_assert_true(qdict_haskey(response, "error"));
    qobject_unref(response);
    g_assert_cmphex(qtest_readq(qts, SDIO1 + SDHC_CAPAB), ==, SD_CAPS);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SDHC_PRNSTS) & SD_CARD_PRESENT,
                    ==, 0);
    qtest_quit(qts);
}

static void test_identity(gconstpointer opaque)
{
    QTestState *qts = wifi_init(opaque, false);

    g_assert_cmphex(qtest_readq(qts, SDIO2 + SDHC_CAPAB), ==, SD_CAPS);
    g_assert_cmphex(qtest_readw(qts, SDIO2 + SDHC_HCVER) & 0xff, ==, 2);
    g_assert_cmphex(qtest_readl(qts, SDIO2 + SDHC_PRNSTS) & SD_CARD_PRESENT,
                    ==, SD_CARD_PRESENT);
    g_assert_cmphex(qtest_readl(qts, SDIO1 + SDHC_PRNSTS) & SD_CARD_PRESENT,
                    ==, 0);
    /* Independently addressable cfg block, not an alias of storage cfg. */
    qtest_writel(qts, SDIO2_CFG + 0x44, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, SDIO2_CFG + 0x44), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, UINT64_C(0x1000fff444)), ==, 0);
    select_wifi(qts);
    g_assert_cmphex(cmd52(qts, false, 0, 0, 0), ==, 0x32);
    for (unsigned function = 0; function <= 3; function++) {
        uint32_t fbr = function * 0x100;
        uint32_t cis = cmd52(qts, false, 0, fbr + 9, 0) |
                       (cmd52(qts, false, 0, fbr + 10, 0) << 8) |
                       (cmd52(qts, false, 0, fbr + 11, 0) << 16);
        const uint8_t manfid[] = { 0x20, 4, 0xd0, 2, 0x45, 0x43 };

        g_assert_cmphex(cis, ==, 0x1000 + function * 0x100);
        for (unsigned i = 0; i < sizeof(manfid); i++) {
            g_assert_cmphex(cmd52(qts, false, 0, cis + i, 0), ==, manfid[i]);
        }
        if (function) {
            g_assert_cmphex(cmd52(qts, true, function, 0x400,
                                  0x30 + function), ==, 0x30 + function);
            g_assert_cmphex(cmd52(qts, false, function, 0x400, 0), ==,
                            0x30 + function);
        }
    }
    g_assert_cmphex(backplane(qts, true, CHIP_ID, 0), ==, 0x15264345);
    g_assert_cmphex(backplane(qts, true, 0x180000fc, 0), ==, 0x18110000);
    qtest_quit(qts);
}

static void build_control(uint8_t frame[CONTROL_LEN])
{
    memset(frame, 0, CONTROL_LEN);
    stw_le_p(frame, CONTROL_LEN);
    stw_le_p(frame + 2, (uint16_t)~CONTROL_LEN);
    frame[4] = 1;
    frame[7] = 12;
    stl_le_p(frame + 12, 262); /* BCDC GET_VAR. */
    stl_le_p(frame + 16, 20);
    stl_le_p(frame + 20, 0x12340000); /* Request ID. */
    memcpy(frame + 28, "cur_etheraddr", sizeof("cur_etheraddr"));
}

static void assert_control(const uint8_t frame[CONTROL_LEN])
{
    const uint8_t mac[] = { 2, 0, 0, 0, 0, 1 };

    g_assert_cmpuint(lduw_le_p(frame), ==, CONTROL_LEN);
    g_assert_cmphex(lduw_le_p(frame + 2), ==, (uint16_t)~CONTROL_LEN);
    g_assert_cmphex(frame[5] & 0xf, ==, 0);
    g_assert_cmpuint(frame[7], ==, 12);
    g_assert_cmpuint(ldl_le_p(frame + 12), ==, 262);
    g_assert_cmpuint(ldl_le_p(frame + 16), ==, 20);
    g_assert_cmphex(ldl_le_p(frame + 20), ==, 0x12340000);
    g_assert_cmphex(ldl_le_p(frame + 24), ==, 0);
    g_assert_cmpmem(frame + 28, sizeof(mac), mac, sizeof(mac));
    for (unsigned i = 28 + sizeof(mac); i < CONTROL_LEN; i++) {
        g_assert_cmphex(frame[i], ==, 0);
    }
}

static void enable_irq(QTestState *qts, unsigned irq)
{
    qtest_writel(qts, GICD, 1);
    qtest_writel(qts, GICC, 1);
    qtest_writel(qts, GICC + 4, 0xff);
    qtest_writeb(qts, GICD + 0x800 + irq, 1);
    qtest_writel(qts, GICD + 0x100 + (irq / 32) * 4, 1U << (irq % 32));
}

static void test_pio_control_irq(gconstpointer opaque)
{
    QTestState *qts = wifi_init(opaque, false);
    uint8_t frame[CONTROL_LEN];

    select_wifi(qts);
    enable_irq(qts, STORAGE_IRQ);
    enable_irq(qts, WIFI_IRQ);
    cmd52(qts, true, 0, INTEN, 3);
    qtest_writew(qts, SDIO2 + SD_ENABLE, 0x13f);
    qtest_writew(qts, SDIO2 + SD_SIGNAL, SD_CARD_IRQ);
    build_control(frame);
    pio_transfer(qts, false, 2, 0, false, frame, sizeof(frame));
    /* Masked at the endpoint, but the response and mailbox remain pending. */
    g_assert_cmphex(backplane(qts, true, INTSTATUS, 0), ==, FRAME_IND);
    g_assert_cmphex(cmd52(qts, false, 0, INTPEND, 0), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GICC + 0x18), ==, 1023);
    backplane(qts, false, HOSTINTMASK, FRAME_IND);
    g_assert_cmphex(cmd52(qts, false, 0, INTPEND, 0), ==, 2);
    g_assert_cmphex(qtest_readw(qts, SDIO2 + SD_STATUS) & SD_CARD_IRQ, ==,
                    SD_CARD_IRQ);
    /* Hardware SPI274 is architectural IRQ306, never storage IRQ305. */
    g_assert_cmphex(qtest_readl(qts, GICC + 0x18), ==, WIFI_IRQ);
    g_assert_cmphex(qtest_readl(qts, GICD + 0x200 + 9 * 4) & (1U << 17),
                    ==, 0);
    /* Host W1C cannot acknowledge the endpoint. */
    qtest_writew(qts, SDIO2 + SD_STATUS, SD_CARD_IRQ);
    g_assert_cmphex(qtest_readw(qts, SDIO2 + SD_STATUS) & SD_CARD_IRQ, ==,
                    SD_CARD_IRQ);

    /* Host status and signal masks are independent of the endpoint mailbox. */
    qtest_writew(qts, SDIO2 + SD_ENABLE, 0x3f);
    g_assert_cmphex(qtest_readw(qts, SDIO2 + SD_STATUS) & SD_CARD_IRQ, ==, 0);
    g_assert_cmphex(cmd52(qts, false, 0, INTPEND, 0), ==, 2);
    g_assert_cmphex(qtest_readl(qts, GICC + 0x18), ==, 1023);
    qtest_writew(qts, SDIO2 + SD_ENABLE, 0x13f);
    g_assert_cmphex(qtest_readw(qts, SDIO2 + SD_STATUS) & SD_CARD_IRQ, ==,
                    SD_CARD_IRQ);
    g_assert_cmphex(qtest_readl(qts, GICC + 0x18), ==, WIFI_IRQ);
    qtest_writew(qts, SDIO2 + SD_SIGNAL, 0);
    g_assert_cmphex(qtest_readw(qts, SDIO2 + SD_STATUS) & SD_CARD_IRQ, ==,
                    SD_CARD_IRQ);
    g_assert_cmphex(qtest_readl(qts, GICC + 0x18), ==, 1023);
    qtest_writew(qts, SDIO2 + SD_SIGNAL, SD_CARD_IRQ);
    g_assert_cmphex(qtest_readl(qts, GICC + 0x18), ==, WIFI_IRQ);

    /* Resetting only the host must retain the endpoint's pending response. */
    qtest_writeb(qts, SDIO2 + SDHC_SWRST, SDHC_RESET_ALL);
    g_assert_cmphex(qtest_readl(qts, SDIO2 + SD_STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SDIO2 + SD_ENABLE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SDIO2 + SD_SIGNAL), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GICC + 0x18), ==, 1023);
    qtest_writew(qts, SDIO2 + SDHC_CLKCON, 7);
    qtest_writel(qts, SDIO2 + SD_ENABLE, 0xffff013f);
    g_assert_cmphex(qtest_readw(qts, SDIO2 + SD_STATUS) & SD_CARD_IRQ, ==,
                    SD_CARD_IRQ);
    qtest_writew(qts, SDIO2 + SD_SIGNAL, SD_CARD_IRQ);
    g_assert_cmphex(qtest_readl(qts, GICC + 0x18), ==, WIFI_IRQ);
    g_assert_cmphex(cmd52(qts, false, 0, INTPEND, 0), ==, 2);
    pio_transfer(qts, true, 2, 0, false, frame, sizeof(frame));
    assert_control(frame);
    g_assert_cmphex(backplane(qts, true, INTSTATUS, 0), ==, FRAME_IND);
    backplane(qts, false, HOSTINTMASK, 0);
    g_assert_cmphex(qtest_readl(qts, GICC + 0x18), ==, 1023);
    backplane(qts, false, HOSTINTMASK, FRAME_IND);
    g_assert_cmphex(qtest_readl(qts, GICC + 0x0c), ==, WIFI_IRQ);
    backplane(qts, false, INTSTATUS, FRAME_IND);
    g_assert_cmphex(cmd52(qts, false, 0, INTPEND, 0), ==, 0);
    g_assert_cmphex(qtest_readw(qts, SDIO2 + SD_STATUS) & SD_CARD_IRQ, ==, 0);
    qtest_writel(qts, GICC + 0x10, WIFI_IRQ);
    g_assert_cmphex(qtest_readl(qts, GICC + 0x18), ==, 1023);
    qtest_quit(qts);
}

static void adma_transfer(QTestState *qts, bool read, unsigned function,
                          uint32_t address, bool increment,
                          uint8_t *data, unsigned len)
{
    const uint64_t table = UINT64_C(0x18001fff8);
    const uint64_t buffers[] = {
        UINT64_C(0x10002ffc0), UINT64_C(0x140030000),
    };
    uint8_t descriptors[24] = {0};
    uint8_t actual[512], scratch[256], guard[GUARD_SIZE];
    unsigned half = len / 2;

    g_assert_cmpuint(half % 4, ==, 0);
    memset(guard, 0xd3, sizeof(guard));
    /* Invalid low table and low buffer poison catch either truncation. */
    qtest_memset(qts, table & UINT32_MAX, 0, sizeof(descriptors));
    qtest_memwrite(qts, table - GUARD_SIZE, guard, sizeof(guard));
    qtest_memwrite(qts, table + sizeof(descriptors), guard, sizeof(guard));
    for (unsigned i = 0; i < G_N_ELEMENTS(buffers); i++) {
        uint64_t buffer = buffers[i];

        stw_le_p(descriptors + i * 12, i == 1 ? 0x23 : 0x21);
        stw_le_p(descriptors + i * 12 + 2, half);
        stq_le_p(descriptors + i * 12 + 4, buffer);
        qtest_memwrite(qts, buffer - GUARD_SIZE, guard, sizeof(guard));
        qtest_memwrite(qts, buffer + half, guard, sizeof(guard));
        qtest_memset(qts, buffer & UINT32_MAX, 0xa5, half);
        if (read) {
            qtest_memset(qts, buffer, 0xda, half);
        } else {
            qtest_memwrite(qts, buffer, data + i * half, half);
        }
    }
    qtest_memwrite(qts, table, descriptors, sizeof(descriptors));
    qtest_writeb(qts, SDIO2 + 0x28, 0x18); /* ADMA2-64, not SDMA/ADMA32. */
    g_assert_cmphex(qtest_readb(qts, SDIO2 + 0x28) & 0x18, ==, 0x18);
    qtest_writel(qts, SDIO2 + SD_ADMA_PTR, (uint32_t)table);
    qtest_writel(qts, SDIO2 + SD_ADMA_PTR + 4, table >> 32);
    g_assert_cmphex(qtest_readq(qts, SDIO2 + SD_ADMA_PTR), ==, table);
    qtest_writel(qts, SDIO2 + SD_STATUS, UINT32_MAX);
    sdhci_cmd_regs(qts, SDIO2, len, 1,
                   cmd53_argument(read, function, address, increment, len),
                   3 | (read ? SDHC_TRNS_READ : 0),
                   (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    for (unsigned i = 0; i < 100 &&
         !(qtest_readw(qts, SDIO2 + SD_STATUS) & 2); i++) {
        qtest_clock_step(qts, 1000000);
    }
    g_assert_cmphex(qtest_readl(qts, SDIO2 + SD_STATUS) & 0xffff0003, ==, 3);
    g_assert_cmphex(qtest_readb(qts, SDIO2 + SD_ADMA_ERR), ==, 0);
    g_assert_cmphex(qtest_readw(qts, SDIO2 + SDHC_BLKCNT), ==, 0);
    for (unsigned i = 0; i < G_N_ELEMENTS(buffers); i++) {
        qtest_memread(qts, buffers[i], actual + i * half, half);
        qtest_memread(qts, buffers[i] - GUARD_SIZE, scratch, GUARD_SIZE);
        g_assert_cmpmem(scratch, GUARD_SIZE, guard, sizeof(guard));
        qtest_memread(qts, buffers[i] + half, scratch, GUARD_SIZE);
        g_assert_cmpmem(scratch, GUARD_SIZE, guard, sizeof(guard));
        qtest_memread(qts, buffers[i] & UINT32_MAX, scratch, half);
        for (unsigned j = 0; j < half; j++) {
            g_assert_cmphex(scratch[j], ==, 0xa5);
        }
    }
    if (read) {
        memcpy(data, actual, len);
    } else {
        g_assert_cmpmem(actual, len, data, len);
    }
    qtest_memread(qts, table - GUARD_SIZE, actual, GUARD_SIZE);
    g_assert_cmpmem(actual, GUARD_SIZE, guard, sizeof(guard));
    qtest_memread(qts, table + sizeof(descriptors), actual, GUARD_SIZE);
    g_assert_cmpmem(actual, GUARD_SIZE, guard, sizeof(guard));
    qtest_memread(qts, table, actual, sizeof(descriptors));
    g_assert_cmpmem(actual, sizeof(descriptors), descriptors,
                    sizeof(descriptors));
    qtest_memread(qts, table & UINT32_MAX, actual, sizeof(descriptors));
    for (unsigned i = 0; i < sizeof(descriptors); i++) {
        g_assert_cmphex(actual[i], ==, 0);
    }
}

static void test_adma64(gconstpointer opaque)
{
    QTestState *qts = wifi_init(opaque, true);
    uint8_t source[512], actual[512], control[CONTROL_LEN];

    select_wifi(qts);
    for (unsigned i = 0; i < sizeof(source); i++) {
        source[i] = i * 31 + 17;
    }
    adma_transfer(qts, false, 1, 0x400, true, source, sizeof(source));
    adma_transfer(qts, true, 1, 0x400, true, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));
    /* Function2's fixed-address FIFO also traverses high-address DMA. */
    build_control(control);
    adma_transfer(qts, false, 2, 0, false, control, sizeof(control));
    g_assert_cmphex(backplane(qts, true, INTSTATUS, 0), ==, FRAME_IND);
    adma_transfer(qts, true, 2, 0, false, control, sizeof(control));
    assert_control(control);
    backplane(qts, false, INTSTATUS, FRAME_IND);
    qtest_quit(qts);
}

static void test_fragmented_control_reset(gconstpointer opaque)
{
    QTestState *qts = wifi_init(opaque, false);
    uint8_t frame[CONTROL_LEN];

    select_wifi(qts);
    build_control(frame);
    pio_transfer(qts, false, 2, 0, false, frame, 8);
    g_assert_cmphex(backplane(qts, true, INTSTATUS, 0), ==, 0);
    pio_transfer(qts, false, 2, 0, false, frame + 8, CONTROL_LEN - 8);
    g_assert_cmphex(backplane(qts, true, INTSTATUS, 0), ==, FRAME_IND);
    pio_transfer(qts, true, 2, 0, false, frame, sizeof(frame));
    assert_control(frame);
    backplane(qts, false, INTSTATUS, FRAME_IND);

    /* Endpoint reset discards a partial SDPCM request, unlike host reset. */
    build_control(frame);
    pio_transfer(qts, false, 2, 0, false, frame, 8);
    sdhci_cmd_regs(qts, SDIO2, 0, 0, 0, 0, 0); /* CMD0 has no response. */
    select_wifi(qts);
    pio_transfer(qts, false, 2, 0, false, frame + 8, CONTROL_LEN - 8);
    g_assert_cmphex(backplane(qts, true, INTSTATUS, 0), ==, 0);
    build_control(frame);
    pio_transfer(qts, false, 2, 0, false, frame, sizeof(frame));
    g_assert_cmphex(backplane(qts, true, INTSTATUS, 0), ==, FRAME_IND);
    pio_transfer(qts, true, 2, 0, false, frame, sizeof(frame));
    assert_control(frame);
    backplane(qts, false, INTSTATUS, FRAME_IND);
    qtest_quit(qts);
}

#ifndef _WIN32
static void wait_migration(QTestState *qts, bool expect_failure)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    while (g_get_monotonic_time() < deadline) {
        g_autoptr(QDict) response = qtest_qmp(
            qts, "{ 'execute': 'query-migrate' }");
        QDict *result = qdict_get_qdict(response, "return");
        const char *status = qdict_get_str(result, "status");

        if (g_str_equal(status, "completed")) {
            g_assert_false(expect_failure);
            return;
        }
        if (g_str_equal(status, "failed")) {
            const char *description = qdict_get_str(result, "error-desc");

            g_assert_true(expect_failure);
            /* Require endpoint load validation, not a transport failure. */
            g_assert_nonnull(strstr(description, "cyw43455-sdio"));
            return;
        }
        g_assert_false(g_str_equal(status, "cancelled"));
        g_usleep(1000);
    }
    g_error("timed out waiting for BCM2712 Wi-Fi migration");
}

static void migrate_file(QTestState *source, QTestState *destination,
                         bool expect_failure)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("bcm2712-wifi-XXXXXX", &error);
    g_autofree char *state_path = NULL;
    g_autofree char *uri = NULL;

    g_assert_no_error(error);
    g_assert_nonnull(directory);
    state_path = g_build_filename(directory, "state", NULL);
    uri = g_strdup_printf("file:%s", state_path);
    qtest_qmp_assert_success(source, "{ 'execute': 'stop' }");
    qtest_qmp_assert_success(source,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    wait_migration(source, false);
    qtest_qmp_assert_success(destination,
        "{ 'execute': 'migrate-incoming', 'arguments': { "
        "'uri': %s, 'exit-on-error': false } }", uri);
    wait_migration(destination, expect_failure);
    g_assert_cmpint(unlink(state_path), ==, 0);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_pending_response_migration(void)
{
    QTestState *source = wifi_init("raspi500", false);
    QTestState *destination;
    uint8_t frame[CONTROL_LEN];

    select_wifi(source);
    enable_irq(source, WIFI_IRQ);
    cmd52(source, true, 0, INTEN, 3);
    cmd52(source, true, 3, 0x400, 0x3c);
    backplane(source, false, HOSTINTMASK, FRAME_IND);
    qtest_writew(source, SDIO2 + SD_SIGNAL, SD_CARD_IRQ);
    build_control(frame);
    pio_transfer(source, false, 2, 0, false, frame, sizeof(frame));
    g_assert_cmphex(qtest_readl(source, GICC + 0x18), ==, WIFI_IRQ);
    destination = wifi_init_extra("raspi500", false, "-incoming defer");
    migrate_file(source, destination, false);

    g_assert_cmphex(cmd52(destination, false, 3, 0x400, 0), ==, 0x3c);
    g_assert_cmphex(backplane(destination, true, CHIP_ID, 0), ==, 0x15264345);
    g_assert_cmphex(cmd52(destination, false, 0, INTPEND, 0), ==, 2);
    g_assert_cmphex(qtest_readw(destination, SDIO2 + SD_STATUS) & SD_CARD_IRQ,
                    ==, SD_CARD_IRQ);
    g_assert_cmphex(qtest_readl(destination, GICC + 0x18), ==, WIFI_IRQ);
    pio_transfer(destination, true, 2, 0, false, frame, sizeof(frame));
    assert_control(frame);
    backplane(destination, false, INTSTATUS, FRAME_IND);
    g_assert_cmphex(qtest_readl(destination, GICC + 0x18), ==, 1023);
    qtest_quit(source);
    qtest_quit(destination);
}

static uint32_t fixture_profile(QTestState *qts)
{
    g_autoptr(QDict) response = qtest_qmp(
        qts, "{ 'execute': 'qom-get', 'arguments': { "
        "'path': '/machine/peripheral/wifi-fixture', "
        "'property': 'hardware-profile' } }");

    return qdict_get_int(response, "return");
}

static void test_profile_mismatch_migration(void)
{
    /*
     * Use the otherwise-empty storage bus as a model-only fixture.
     * Real Pi5 boards always select the Pi5 identity on their separate SDIO2.
     */
    const char *options = "-machine raspi5b -m 1G -serial null -nic none "
        "-device cyw43455-sdio,id=wifi-fixture,bus=sd-bus,"
        "mac=02:00:00:00:00:01,hardware-profile=%u %s";
    QTestState *source = qtest_initf(options, 1U, "");
    QTestState *destination = qtest_initf(options, 0U, "-incoming defer");

    g_assert_cmpuint(fixture_profile(source), ==, 1);
    g_assert_cmpuint(fixture_profile(destination), ==, 0);
    migrate_file(source, destination, true);
    /* The stream must not silently overwrite the destination identity. */
    g_assert_cmpuint(fixture_profile(destination), ==, 0);
    qtest_quit(source);
    qtest_quit(destination);
}
#endif

int main(int argc, char **argv)
{
    const char *machines[] = { "raspi5b", "raspi500", "raspi500p" };

    g_test_init(&argc, &argv, NULL);
    for (unsigned i = 0; i < G_N_ELEMENTS(machines); i++) {
        const char *machine = machines[i];
        g_autofree char *absent = g_strdup_printf(
            "/bcm2712/%s/wifi/default-absent", machine);
        g_autofree char *identity = g_strdup_printf(
            "/bcm2712/%s/wifi/identity", machine);
        g_autofree char *pio = g_strdup_printf(
            "/bcm2712/%s/wifi/pio-control-irq", machine);
        g_autofree char *adma = g_strdup_printf(
            "/bcm2712/%s/wifi/adma64", machine);
        g_autofree char *fragmented = g_strdup_printf(
            "/bcm2712/%s/wifi/fragmented-control-reset", machine);

        qtest_add_data_func(absent, machine, test_default_absent);
        qtest_add_data_func(identity, machine, test_identity);
        qtest_add_data_func(pio, machine, test_pio_control_irq);
        qtest_add_data_func(adma, machine, test_adma64);
        qtest_add_data_func(fragmented, machine, test_fragmented_control_reset);
    }
#ifndef _WIN32
    qtest_add_func("/bcm2712/raspi500/wifi/pending-response-migration",
                   test_pending_response_migration);
    qtest_add_func("/bcm2712/wifi/model-only-profile-mismatch-migration",
                   test_profile_mismatch_migration);
#endif
    return g_test_run();
}
