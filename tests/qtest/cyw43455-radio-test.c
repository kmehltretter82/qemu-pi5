/*
 * Downstream CYW43455 radio/firmware-boundary regressions for both Pi forks.
 * These exercise board SDIO wiring, not firmware execution or RF behavior.
 * AI-assisted downstream work; not for QEMU upstream submission.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "libqos/sdhci-cmd.h"
#include "qobject/qdict.h"

#define FRAME_MAX 512
#define INTSTATUS 0x18004020
#define FRAME_IND (1U << 6)
#define RAM_BASE 0x198000
#define RAM_SIZE (640 * 1024)
#define NS_MS INT64_C(1000000)

typedef struct Radio {
    QTestState *qts;
    uint64_t host;
    uint16_t request;
    uint8_t sequence;
} Radio;

static void command(Radio *r, unsigned cmd, uint32_t arg)
{
    sdhci_cmd_regs(r->qts, r->host, 0, 0, arg, 0,
                   (cmd << 8) | SDHC_CMD_RESPONSE);
    g_assert_cmphex(qtest_readl(r->qts, r->host + 0x30) >> 16, ==, 0);
}

static uint8_t cmd52(Radio *r, bool write, unsigned fn, uint32_t addr,
                     uint8_t value)
{
    uint32_t arg = (fn << 28) | (addr << 9) | value;
    uint32_t response;

    command(r, 52, arg | (write ? (1U << 31) | (1U << 27) : 0));
    response = qtest_readl(r->qts, r->host + SDHC_RSPREG0);
    g_assert_cmphex(response & ~0xffU, ==, 0);
    return response;
}

static Radio radio_init(const char *machine, uint16_t chanspec,
                        const char *binary_env, const char *extra)
{
    g_autofree char *args = g_strdup_printf(
        "-machine %s,sdio-wifi=on -serial null -nic none "
        "-global cyw43455-sdio.mac=02:00:00:00:00:01 "
        "-global cyw43455-sdio.ap-chanspec=%u %s",
        machine, chanspec, extra);
    Radio r = {
        .qts = qtest_init_ext(binary_env, args, NULL, true),
        .host = g_str_has_prefix(machine, "raspi4") ? 0xfe300000 :
                                                    UINT64_C(0x1001100000),
    };

    return r;
}

static void select_card(Radio *r)
{
    qtest_writeb(r->qts, r->host + SDHC_SWRST, SDHC_RESET_ALL);
    qtest_writew(r->qts, r->host + SDHC_CLKCON, 7);
    qtest_writel(r->qts, r->host + 0x34, 0xffff013f);
    command(r, 5, 0);
    g_assert_cmphex(qtest_readl(r->qts, r->host + SDHC_RSPREG0), ==,
                    0xb0ff8000);
    command(r, 3, 0);
    command(r, 7, 0x10000);
    g_assert_cmphex(cmd52(r, true, 0, 2, 0x0e), ==, 0x0e);
    g_assert_cmphex(cmd52(r, false, 0, 3, 0), ==, 0x0e);
}

static void pio(Radio *r, bool read, unsigned fn, uint32_t addr,
                bool increment, uint8_t *data, size_t length)
{
    uint32_t arg = (read ? 0 : 1U << 31) | (fn << 28) |
                   (increment ? 1U << 26 : 0) | (addr << 9) |
                   (length & 0x1ff);

    g_assert_cmpuint(length, >, 0);
    g_assert_cmpuint(length, <=, FRAME_MAX);
    g_assert_cmpuint(length % 4, ==, 0);
    qtest_writeb(r->qts, r->host + 0x28, 0);
    qtest_writel(r->qts, r->host + 0x30, UINT32_MAX);
    sdhci_cmd_regs(r->qts, r->host, length, 1, arg,
                   SDHC_TRNS_BLK_CNT_EN | (read ? SDHC_TRNS_READ : 0),
                   (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    for (size_t i = 0; i < length; i += 4) {
        if (read) {
            stl_le_p(data + i, qtest_readl(r->qts, r->host + SDHC_BDATA));
        } else {
            qtest_writel(r->qts, r->host + SDHC_BDATA, ldl_le_p(data + i));
        }
    }
    g_assert_cmphex(qtest_readl(r->qts, r->host + 0x30) & 0xffff0003, ==, 3);
}

static uint32_t backplane(Radio *r, bool read, uint32_t addr, uint32_t value)
{
    uint32_t window = addr & ~0x7fffU;
    uint8_t bytes[4];

    cmd52(r, true, 1, 0x1000a, window >> 8);
    cmd52(r, true, 1, 0x1000b, window >> 16);
    cmd52(r, true, 1, 0x1000c, window >> 24);
    stl_le_p(bytes, value);
    pio(r, read, 1, 0x8000 | (addr & 0x7fff), true, bytes, sizeof(bytes));
    return ldl_le_p(bytes);
}

static bool pending(Radio *r)
{
    return backplane(r, true, INTSTATUS, 0) & FRAME_IND;
}

static void ack(Radio *r)
{
    backplane(r, false, INTSTATUS, FRAME_IND);
}

static size_t read_frame(Radio *r, uint8_t frame[FRAME_MAX])
{
    size_t length, padded;

    g_assert_true(pending(r));
    memset(frame, 0xa5, FRAME_MAX);
    pio(r, true, 2, 0, false, frame, 4);
    length = lduw_le_p(frame);
    g_assert_cmpuint(length, >=, 12);
    g_assert_cmpuint(length, <=, FRAME_MAX);
    g_assert_cmphex(lduw_le_p(frame + 2), ==, (uint16_t)~length);
    padded = (length + 3) & ~3U;
    pio(r, true, 2, 0, false, frame + 4, padded - 4);
    g_assert_cmphex(frame[7], ==, 12);
    for (size_t i = length; i < padded; i++) {
        g_assert_cmphex(frame[i], ==, 0);
    }
    return length;
}

static void send_control(Radio *r, unsigned command, bool set,
                         const uint8_t *input, size_t input_len,
                         size_t output_len)
{
    uint8_t frame[FRAME_MAX] = { };
    size_t length = 28 + input_len;

    g_assert_cmpuint(28 + MAX(input_len, output_len), <=, FRAME_MAX);
    stw_le_p(frame, length);
    stw_le_p(frame + 2, (uint16_t)~length);
    frame[4] = ++r->sequence;
    frame[7] = 12;
    stl_le_p(frame + 12, command);
    stl_le_p(frame + 16, MAX(input_len, output_len));
    stl_le_p(frame + 20, ((uint32_t)++r->request << 16) | (set ? 2 : 0));
    if (input_len) {
        memcpy(frame + 28, input, input_len);
    }
    pio(r, false, 2, 0, false, frame, (length + 3) & ~3U);
}

static int32_t reply(Radio *r, unsigned command, uint8_t *output,
                      size_t output_len, bool acknowledge)
{
    uint8_t frame[FRAME_MAX];
    size_t length = read_frame(r, frame);
    int32_t status = ldl_le_p(frame + 24);

    g_assert_cmpuint(length, >=, 28 + output_len);
    g_assert_cmpuint(length, ==, 28 + ldl_le_p(frame + 16));
    g_assert_cmphex(frame[5] & 0xf, ==, 0);
    g_assert_cmpuint(ldl_le_p(frame + 12), ==, command);
    g_assert_cmpuint(ldl_le_p(frame + 20) >> 16, ==, r->request);
    g_assert_cmpuint(ldl_le_p(frame + 20) & 1, ==, status < 0);
    if (output_len) {
        memcpy(output, frame + 28, output_len);
    }
    if (acknowledge) {
        ack(r);
    }
    return status;
}

static int32_t iovar(Radio *r, const char *name, bool set,
                      const uint8_t *input, size_t input_len,
                      uint8_t *output, size_t output_len)
{
    uint8_t payload[FRAME_MAX];
    size_t name_len = strlen(name) + 1;

    g_assert_cmpuint(name_len + input_len, <=, sizeof(payload));
    memcpy(payload, name, name_len);
    if (input_len) {
        memcpy(payload + name_len, input, input_len);
    }
    send_control(r, set ? 263 : 262, set, payload, name_len + input_len,
                  output_len);
    return reply(r, set ? 263 : 262, output, output_len, true);
}

static void start_scan(Radio *r, uint16_t chanspec, uint16_t sync)
{
    uint8_t payload[6 + 8 + 72 + 4 + 36] = "escan";
    uint8_t *scan = payload + 6;
    uint8_t *params = scan + 8;

    stl_le_p(scan, 2);
    stw_le_p(scan + 4, 1); /* START */
    stw_le_p(scan + 6, sync);
    stw_le_p(params, 2);
    stw_le_p(params + 2, sizeof(payload) - 6 - 8);
    memset(params + 40, 0xff, 6);
    params[46] = 2; /* any BSS */
    for (size_t off = 52; off < 68; off += 4) {
        stl_le_p(params + off, UINT32_MAX);
    }
    stl_le_p(params + 68, (1U << 16) | 1);
    stw_le_p(params + 72, chanspec);
    stl_le_p(params + 76, 13);
    memcpy(params + 80, "qemu-cyw43455", 13);
    send_control(r, 263, true, payload, sizeof(payload), 0);
}

static void assert_scan(const uint8_t *frame, size_t length, uint16_t sync,
                         bool partial, bool five_ghz)
{
    static const uint8_t ies_2g[] = {
        0, 13, 'q', 'e', 'm', 'u', '-', 'c', 'y', 'w', '4', '3', '4', '5', '5',
        1, 4, 0x82, 0x84, 0x8b, 0x96, 3, 1, 1,
    };
    static const uint8_t ies_5g[] = {
        0, 13, 'q', 'e', 'm', 'u', '-', 'c', 'y', 'w', '4', '3', '4', '5', '5',
        1, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c,
    };
    const uint8_t *ies = five_ghz ? ies_5g : ies_2g;
    size_t ie_len = five_ghz ? sizeof(ies_5g) : sizeof(ies_2g);
    size_t data_len = partial ? 12 + 128 + ie_len : 0;
    const uint8_t *result = frame + 88;
    const uint8_t *bss = result + 12;

    g_assert_cmpuint(length, ==, 88 + data_len);
    g_assert_cmpuint(frame[5] & 0xf, ==, 1);
    g_assert_cmphex(lduw_be_p(frame + 28), ==, 0x886c);
    g_assert_cmpuint(ldl_be_p(frame + 44), ==, 69); /* ESCAN_RESULT */
    g_assert_cmpuint(ldl_be_p(frame + 48), ==, partial ? 8 : 0);
    g_assert_cmpuint(ldl_be_p(frame + 60), ==, data_len);
    if (!partial) {
        return;
    }
    g_assert_cmpuint(ldl_le_p(result), ==, data_len);
    g_assert_cmpuint(ldl_le_p(result + 4), ==, 1);
    g_assert_cmphex(lduw_le_p(result + 8), ==, sync);
    g_assert_cmpuint(lduw_le_p(result + 10), ==, 1);
    g_assert_cmpuint(ldl_le_p(bss), ==, 109);
    g_assert_cmpuint(ldl_le_p(bss + 4), ==, 128 + ie_len);
    g_assert_cmphex(lduw_le_p(bss + 16), ==, five_ghz ? 1 : 0x421);
    g_assert_cmpuint(ldl_le_p(bss + 52), ==, five_ghz ? 8 : 4);
    g_assert_cmpmem(bss + 56, five_ghz ? 8 : 4, ies + 17, five_ghz ? 8 : 4);
    g_assert_cmphex(lduw_le_p(bss + 72), ==, five_ghz ? 0xd024 : 0x1001);
    g_assert_cmpuint(bss[88], ==, five_ghz ? 36 : 1);
    g_assert_cmpuint(lduw_le_p(bss + 116), ==, 128);
    g_assert_cmpuint(ldl_le_p(bss + 120), ==, ie_len);
    g_assert_cmpmem(bss + 128, ie_len, ies, ie_len);
}

static void test_ram(gconstpointer machine)
{
    Radio r = radio_init(machine, 0x1001, NULL, "");
    const uint32_t offsets[] = { 0, 4, 0x7ffc, 0x8000, 0x3fffc, 0x40000,
                                 0x7fffc, 0x80000, RAM_SIZE - 4 };

    select_card(&r);
    for (unsigned i = 0; i < G_N_ELEMENTS(offsets); i++) {
        g_assert_cmphex(backplane(&r, true, RAM_BASE + offsets[i], 0), ==, 0);
        backplane(&r, false, RAM_BASE + offsets[i], 0x12340000 + i);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(offsets); i++) {
        g_assert_cmphex(backplane(&r, true, RAM_BASE + offsets[i], 0), ==,
                        0x12340000 + i);
    }
    /* Out-of-range writes must neither extend RAM nor alias its edges. */
    backplane(&r, false, RAM_BASE - 4, UINT32_MAX);
    backplane(&r, false, RAM_BASE + RAM_SIZE, UINT32_MAX);
    g_assert_cmphex(backplane(&r, true, RAM_BASE - 4, 0), ==, 0);
    g_assert_cmphex(backplane(&r, true, RAM_BASE + RAM_SIZE, 0), ==, 0);
    g_assert_cmphex(backplane(&r, true, RAM_BASE, 0), ==, 0x12340000);
    sdhci_cmd_regs(r.qts, r.host, 0, 0, 0, 0, 0); /* endpoint CMD0 reset */
    select_card(&r);
    for (unsigned i = 0; i < G_N_ELEMENTS(offsets); i++) {
        g_assert_cmphex(backplane(&r, true, RAM_BASE + offsets[i], 0), ==, 0);
    }
    qtest_quit(r.qts);
}

static const uint8_t country_de[12] = {
    'D', 'E', 0, 0, 13, 0, 0, 0, 'X', '2', 0, 0,
};

static void assert_country(Radio *r, const uint8_t expected[12])
{
    uint8_t actual[12];

    g_assert_cmpint(iovar(r, "country", false, NULL, 0,
                          actual, sizeof(actual)), ==, 0);
    g_assert_cmpmem(actual, sizeof(actual), expected, 12);
}

static void test_country(gconstpointer machine)
{
    Radio r = radio_init(machine, 0x1001, NULL, "");
    uint8_t malformed[12];

    select_card(&r);
    g_assert_cmpint(iovar(&r, "country", true, country_de, 12, NULL, 0), ==, 0);
    assert_country(&r, country_de);
    g_assert_cmpint(iovar(&r, "country", true, country_de, 11, NULL, 0), ==, -24);
    assert_country(&r, country_de);
    memcpy(malformed, country_de, 12);
    malformed[3] = 'A'; /* unterminated abbreviation */
    g_assert_cmpint(iovar(&r, "country", true, malformed, 12, NULL, 0), ==, -2);
    assert_country(&r, country_de);
    qtest_quit(r.qts);
}

static void scan_order(const char *machine, bool five_ghz)
{
    Radio r = radio_init(machine, five_ghz ? 0xd024 : 0x1001, NULL, "");
    uint8_t frame[FRAME_MAX];
    size_t length, padded;

    select_card(&r);
    start_scan(&r, five_ghz ? 0xd024 : 0x1001, 0x5a36);
    /* Neither request receipt nor full reply read starts an ACK-gated timer. */
    qtest_clock_step(r.qts, 100 * NS_MS);
    g_assert_cmpint(reply(&r, 263, NULL, 0, false), ==, 0);
    qtest_clock_step(r.qts, 100 * NS_MS);
    ack(&r);
    g_assert_false(pending(&r));
    qtest_clock_step(r.qts, 9 * NS_MS);
    g_assert_false(pending(&r));
    qtest_clock_step(r.qts, NS_MS);
    g_assert_true(pending(&r));

    /* An early W1C must not let a partial FIFO read start the next timer. */
    pio(&r, true, 2, 0, false, frame, 4);
    length = lduw_le_p(frame);
    g_assert_cmpuint(length, >=, 12);
    g_assert_cmpuint(length, <=, FRAME_MAX);
    g_assert_cmphex(lduw_le_p(frame + 2), ==, (uint16_t)~length);
    padded = (length + 3) & ~3U;
    ack(&r);
    qtest_clock_step(r.qts, 100 * NS_MS);
    pio(&r, true, 2, 0, false, frame + 4, padded - 4);
    assert_scan(frame, length, 0x5a36, true, five_ghz);
    for (size_t i = length; i < padded; i++) {
        g_assert_cmphex(frame[i], ==, 0);
    }
    g_assert_false(pending(&r));
    qtest_clock_step(r.qts, 9 * NS_MS);
    g_assert_false(pending(&r));
    qtest_clock_step(r.qts, NS_MS);
    length = read_frame(&r, frame);
    assert_scan(frame, length, 0, false, five_ghz);
    ack(&r);
    qtest_clock_step(r.qts, 100 * NS_MS);
    g_assert_false(pending(&r));
    qtest_quit(r.qts);
}

static void test_scan_2g(gconstpointer machine)
{
    scan_order(machine, false);
}

static void test_scan_5g(gconstpointer machine)
{
    scan_order(machine, true);
}

#ifndef _WIN32
static void wait_migration(QTestState *qts)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    while (g_get_monotonic_time() < deadline) {
        g_autoptr(QDict) response = qtest_qmp(qts,
            "{ 'execute': 'query-migrate' }");
        QDict *result = qdict_get_qdict(response, "return");
        const char *status = qdict_get_try_str(result, "status");

        g_assert_nonnull(status);
        if (!strcmp(status, "completed")) {
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpstr(status, !=, "cancelled");
        g_usleep(1000);
    }
    g_error("radio migration did not complete within 30 seconds");
}

static void migrate(Radio *source, Radio *dest)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("cyw43455-radio-XXXXXX", &error);
    g_autofree char *state = NULL;
    g_autofree char *uri = NULL;

    g_assert_no_error(error);
    g_assert_nonnull(dir);
    state = g_build_filename(dir, "state", NULL);
    uri = g_strdup_printf("file:%s", state);
    qtest_qmp_assert_success(source->qts, "{ 'execute': 'stop' }");
    qtest_qmp_assert_success(source->qts,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    wait_migration(source->qts);
    qtest_qmp_assert_success(dest->qts,
        "{ 'execute': 'migrate-incoming', 'arguments': { 'uri': %s } }", uri);
    wait_migration(dest->qts);
    qtest_qmp_assert_success(dest->qts, "{ 'execute': 'cont' }");
    dest->request = source->request;
    dest->sequence = source->sequence;
    g_assert_cmpint(unlink(state), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_scan_migration(gconstpointer machine)
{
    Radio source = radio_init(machine, 0xd024, NULL, "");
    Radio dest = radio_init(machine, 0xd024, NULL, "-incoming defer");
    uint8_t frame[FRAME_MAX];
    size_t length;
    int64_t source_time;

    select_card(&source);
    backplane(&source, false, RAM_BASE + RAM_SIZE - 4, 0x5a36abcd);
    g_assert_cmpint(iovar(&source, "country", true, country_de, 12,
                          NULL, 0), ==, 0);
    start_scan(&source, 0xd024, 0x7e36);
    g_assert_cmpint(reply(&source, 263, NULL, 0, true), ==, 0);
    source_time = qtest_clock_step(source.qts, 4 * NS_MS);
    g_assert_cmpint(source_time, ==, 4 * NS_MS);
    g_assert_false(pending(&source));
    /*
     * QTest's accelerator clock is process-local, not migration state.
     * Device timers migrate absolute deadlines.  Align the destination
     * before loading, retaining the exact 5 ms not-due / 6 ms due oracle.
     */
    g_assert_cmpint(qtest_clock_set(dest.qts, source_time), ==, source_time);
    migrate(&source, &dest);
    g_assert_cmphex(backplane(&dest, true, RAM_BASE + RAM_SIZE - 4, 0), ==,
                    0x5a36abcd);
    g_assert_false(pending(&dest));
    qtest_clock_step(dest.qts, 5 * NS_MS);
    g_assert_false(pending(&dest));
    qtest_clock_step(dest.qts, NS_MS);
    length = read_frame(&dest, frame);
    assert_scan(frame, length, 0x7e36, true, true);
    ack(&dest);
    qtest_clock_step(dest.qts, 10 * NS_MS);
    length = read_frame(&dest, frame);
    assert_scan(frame, length, 0, false, true);
    ack(&dest);
    assert_country(&dest, country_de);
    g_assert_false(pending(&dest));
    qtest_quit(source.qts);
    qtest_quit(dest.qts);
}

static void test_legacy_migration(void)
{
    static const uint8_t world[12] = {
        '0', '0', 0, 0, 0, 0, 0, 0, '0', '0', 0, 0,
    };
    Radio source = radio_init("raspi500", 0x1001,
                               "QTEST_QEMU_BINARY_LEGACY", "");
    Radio dest = radio_init("raspi500", 0x1001, NULL, "-incoming defer");
    uint8_t scalar[4], mac[6];

    select_card(&source);
    stl_le_p(scalar, 1);
    send_control(&source, 20, true, scalar, sizeof(scalar), 0); /* INFRA */
    g_assert_cmpint(reply(&source, 20, NULL, 0, true), ==, 0);
    send_control(&source, 262, false, (const uint8_t *)"cur_etheraddr",
                  sizeof("cur_etheraddr"), 6);
    /* Preserve a complete pending v30 reply, including its IRQ and identity. */
    migrate(&source, &dest);
    g_assert_cmpint(reply(&dest, 262, mac, sizeof(mac), true), ==, 0);
    g_assert_cmpmem(mac, sizeof(mac), "\x02\0\0\0\0\x01", 6);
    g_assert_cmphex(backplane(&dest, true, RAM_BASE, 0), ==, 0);
    assert_country(&dest, world);
    qtest_quit(source.qts);
    qtest_quit(dest.qts);
}
#endif

int main(int argc, char **argv)
{
    const char *machines[] = { "raspi400", "raspi4b", "raspi5b",
                               "raspi500", "raspi500p" };
    static const struct {
        const char *name;
        void (*test)(gconstpointer);
    } tests[] = {
        { "firmware-ram", test_ram },
        { "country-state", test_country },
        { "2g-frame-boundary-scan", test_scan_2g },
        { "5g-frame-boundary-scan", test_scan_5g },
#ifndef _WIN32
        { "5g-pending-scan-migration", test_scan_migration },
#endif
    };

    g_test_init(&argc, &argv, NULL);
    for (unsigned i = 0; i < G_N_ELEMENTS(machines); i++) {
        if (!qtest_has_machine(machines[i])) {
            continue;
        }
        for (unsigned j = 0; j < G_N_ELEMENTS(tests); j++) {
            g_autofree char *name = g_strdup_printf("/cyw43455-radio/%s/%s",
                                                    machines[i], tests[j].name);

            qtest_add_data_func(name, machines[i], tests[j].test);
        }
    }
#ifndef _WIN32
    if (qtest_has_machine("raspi500") && getenv("QTEST_QEMU_BINARY_LEGACY")) {
        qtest_add_func("/cyw43455-radio/raspi500/legacy-v30-migration",
                       test_legacy_migration);
    }
#endif
    return g_test_run();
}
