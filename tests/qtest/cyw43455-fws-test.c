/*
 * Private synthetic CYW43455 firmware-signalling wire tests.
 * These exercise BCDC tags, bounded credits and completion records, not RF or
 * proprietary firmware execution. The endpoint is on the empty SDIO1 bus;
 * inherited bcm2712-wifi-test separately covers the real board's SDIO2 wiring.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "libqos/sdhci-cmd.h"
#include "qobject/qdict.h"
#ifndef _WIN32
#include <poll.h>
#endif

/* Independent addresses and wire constants; no model implementation header. */
#define HOST            UINT64_C(0x1000fff000)
#define SD_STATUS       0x30
#define SD_ENABLE       0x34
#define IOEX            0x02
#define IORX            0x03
#define ABORT           0x06
#define SBADDRLOW       0x1000a
#define SBADDRMID       0x1000b
#define SBADDRHIGH      0x1000c
#define CHIP_ID         0x18000000
#define INTSTATUS       0x18004020
#define FRAME_IND       (1U << 6)
#define C_GET_VAR       262
#define C_SET_VAR       263
#define EVENT_CREDIT_MAP 74
#define EVENT_BCMC_SUPPORT 127
#define FWS_TXSTATUS    4
#define FWS_PKTTAG      5
#define FWS_CREDITBACK  11
#define FWS_FILLER      255
#define FWS_FLAGS       0x4f
#define FRAME_MAX       512
#define ETH_LEN         60

static const uint8_t guest_mac[] = { 2, 0, 0, 0, 0, 1 };
static const uint8_t peer_mac[] = { 0x52, 0x55, 0x0a, 0, 2, 2 };

typedef struct FWSFixture {
    QTestState *qts;
    unsigned mode;
    unsigned profile;
    uint16_t request_id;
    uint8_t sequence;
    int peer_fd;
} FWSFixture;

static void command(FWSFixture *f, unsigned cmd, uint32_t argument)
{
    sdhci_cmd_regs(f->qts, HOST, 0, 0, argument, 0,
                   (cmd << 8) | SDHC_CMD_RESPONSE);
    g_assert_cmphex(qtest_readl(f->qts, HOST + SD_STATUS) >> 16, ==, 0);
}

static uint8_t cmd52(FWSFixture *f, bool write, unsigned function,
                     uint32_t address, uint8_t value)
{
    uint32_t argument = (function << 28) | (address << 9) | value;
    uint32_t response;

    if (write) {
        argument |= (1U << 31) | (1U << 27);
    }
    command(f, 52, argument);
    response = qtest_readl(f->qts, HOST + SDHC_RSPREG0);
    g_assert_cmphex(response & ~0xffU, ==, 0);
    return response;
}

static void select_endpoint(FWSFixture *f)
{
    qtest_writeb(f->qts, HOST + SDHC_SWRST, SDHC_RESET_ALL);
    qtest_writew(f->qts, HOST + SDHC_CLKCON, 7);
    qtest_writel(f->qts, HOST + SD_ENABLE, 0xffff013f);
    command(f, 5, 0);
    command(f, 3, 0);
    command(f, 7, 0x10000);
    g_assert_cmphex(cmd52(f, true, 0, IOEX, 0x0e), ==, 0x0e);
    g_assert_cmphex(cmd52(f, false, 0, IORX, 0), ==, 0x0e);
}

static void pio(FWSFixture *f, bool read, unsigned function,
                uint32_t address, bool increment, uint8_t *data, size_t len)
{
    uint32_t argument;

    g_assert_cmpuint(len, >, 0);
    g_assert_cmpuint(len, <=, FRAME_MAX);
    g_assert_cmpuint(len % 4, ==, 0);
    argument = (read ? 0 : 1U << 31) | (function << 28) |
               (increment ? 1U << 26 : 0) | (address << 9) | (len & 0x1ff);
    qtest_writeb(f->qts, HOST + 0x28, 0);
    qtest_writel(f->qts, HOST + SD_STATUS, UINT32_MAX);
    sdhci_cmd_regs(f->qts, HOST, len, 1, argument,
                   SDHC_TRNS_BLK_CNT_EN | (read ? SDHC_TRNS_READ : 0),
                   (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    for (size_t i = 0; i < len; i += 4) {
        if (read) {
            stl_le_p(data + i, qtest_readl(f->qts, HOST + SDHC_BDATA));
        } else {
            qtest_writel(f->qts, HOST + SDHC_BDATA, ldl_le_p(data + i));
        }
    }
    g_assert_cmphex(qtest_readl(f->qts, HOST + SD_STATUS) & 0xffff0003, ==, 3);
}

static uint32_t backplane(FWSFixture *f, bool read, uint32_t address,
                          uint32_t value)
{
    uint32_t window = address & ~0x7fffU;
    uint8_t data[4];

    cmd52(f, true, 1, SBADDRLOW, window >> 8);
    cmd52(f, true, 1, SBADDRMID, window >> 16);
    cmd52(f, true, 1, SBADDRHIGH, window >> 24);
    stl_le_p(data, value);
    pio(f, read, 1, 0x8000 | (address & 0x7fff), true, data, sizeof(data));
    return ldl_le_p(data);
}

static bool frame_pending(FWSFixture *f)
{
    return backplane(f, true, INTSTATUS, 0) & FRAME_IND;
}

static void acknowledge(FWSFixture *f)
{
    backplane(f, false, INTSTATUS, FRAME_IND);
}

static size_t read_frame(FWSFixture *f, uint8_t frame[FRAME_MAX])
{
    size_t len, stored;

    g_assert_true(frame_pending(f));
    memset(frame, 0, FRAME_MAX);
    pio(f, true, 2, 0, false, frame, 4);
    len = lduw_le_p(frame);
    g_assert_cmpuint(len, >=, 12);
    g_assert_cmpuint(len, <=, FRAME_MAX);
    g_assert_cmphex(lduw_le_p(frame + 2), ==, (uint16_t)~len);
    stored = (len + 3) & ~3U;
    if (stored > 4) {
        pio(f, true, 2, 0, false, frame + 4, stored - 4);
    }
    g_assert_cmpuint(frame[7], ==, 12);
    for (size_t i = len; i < stored; i++) {
        g_assert_cmphex(frame[i], ==, 0);
    }
    return len;
}

static uint16_t send_control(FWSFixture *f, uint32_t cmd, bool set,
                             const void *input, size_t input_len,
                             size_t output_len)
{
    uint8_t frame[FRAME_MAX] = { };
    size_t data_len = MAX(input_len, output_len);
    size_t len = 28 + input_len;
    uint16_t id = ++f->request_id;

    g_assert_cmpuint(28 + data_len, <=, FRAME_MAX);
    stw_le_p(frame, len);
    stw_le_p(frame + 2, (uint16_t)~len);
    frame[4] = ++f->sequence;
    frame[7] = 12;
    stl_le_p(frame + 12, cmd);
    stl_le_p(frame + 16, data_len);
    stl_le_p(frame + 20, ((uint32_t)id << 16) | (set ? 2 : 0));
    if (input_len) {
        memcpy(frame + 28, input, input_len);
    }
    pio(f, false, 2, 0, false, frame, (len + 3) & ~3U);
    return id;
}

static int32_t receive_control(FWSFixture *f, uint32_t cmd, uint16_t id,
                               uint8_t *output, size_t output_len)
{
    uint8_t frame[FRAME_MAX];
    size_t len;
    int32_t status;

    len = read_frame(f, frame);
    g_assert_cmpuint(frame[5] & 0xf, ==, 0);
    g_assert_cmpuint(len, ==, 28 + ldl_le_p(frame + 16));
    g_assert_cmpuint(ldl_le_p(frame + 12), ==, cmd);
    g_assert_cmpuint((uint32_t)ldl_le_p(frame + 20) >> 16, ==, id);
    status = ldl_le_p(frame + 24);
    g_assert_cmpuint(ldl_le_p(frame + 20) & 1, ==, status < 0);
    g_assert_cmpuint(len, >=, 28 + output_len);
    if (output_len) {
        memcpy(output, frame + 28, output_len);
    }
    acknowledge(f);
    return status;
}

static int32_t control(FWSFixture *f, uint32_t cmd, bool set,
                       const void *input, size_t input_len,
                       uint8_t *output, size_t output_len)
{
    uint16_t id = send_control(f, cmd, set, input, input_len, output_len);

    return receive_control(f, cmd, id, output, output_len);
}

static int32_t iovar(FWSFixture *f, const char *name, bool set,
                     const void *input, size_t input_len,
                     uint8_t *output, size_t output_len)
{
    uint8_t data[FRAME_MAX];
    size_t name_len = strlen(name) + 1;

    g_assert_cmpuint(name_len + input_len, <=, sizeof(data));
    memcpy(data, name, name_len);
    if (input_len) {
        memcpy(data + name_len, input, input_len);
    }
    return control(f, set ? C_SET_VAR : C_GET_VAR, set,
                   data, name_len + input_len, output, output_len);
}

static int32_t set_u32(FWSFixture *f, const char *name, uint32_t value)
{
    uint8_t bytes[4];

    stl_le_p(bytes, value);
    return iovar(f, name, true, bytes, sizeof(bytes), NULL, 0);
}

static uint32_t get_u32(FWSFixture *f, const char *name)
{
    uint8_t bytes[4];

    g_assert_cmpint(iovar(f, name, false, NULL, 0, bytes, sizeof(bytes)), ==, 0);
    return ldl_le_p(bytes);
}

#ifndef _WIN32
static void socket_io(int fd, bool read, void *buffer, size_t length)
{
    uint8_t *p = buffer;
    int64_t deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;

    while (length) {
        struct pollfd pollfd = { .fd = fd, .events = read ? POLLIN : POLLOUT };
        int64_t remaining = deadline - g_get_monotonic_time();
        ssize_t done;
        int result;

        g_assert_cmpint(remaining, >, 0);
        result = poll(&pollfd, 1, (remaining + 999) / 1000);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        g_assert_cmpint(result, ==, 1);
        g_assert_cmphex(pollfd.revents & (POLLERR | POLLHUP | POLLNVAL), ==, 0);
        done = read ? recv(fd, p, length, 0) : send(fd, p, length, 0);
        if (done < 0 && errno == EINTR) {
            continue;
        }
        g_assert_cmpint(done, >, 0);
        p += done;
        length -= done;
    }
}

static void make_ethernet(uint8_t packet[ETH_LEN], bool incoming, uint8_t cookie)
{
    memcpy(packet, incoming ? guest_mac : peer_mac, 6);
    memcpy(packet + 6, incoming ? peer_mac : guest_mac, 6);
    stw_be_p(packet + 12, 0x88b5);
    for (size_t i = 14; i < ETH_LEN; i++) {
        packet[i] = cookie + i;
    }
}

static void expect_backend(FWSFixture *f, const uint8_t packet[ETH_LEN])
{
    uint8_t actual[ETH_LEN];
    uint32_t length;

    socket_io(f->peer_fd, true, &length, sizeof(length));
    g_assert_cmpuint(ntohl(length), ==, ETH_LEN);
    socket_io(f->peer_fd, true, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), packet, ETH_LEN);
}

static void assert_backend_empty(FWSFixture *f)
{
    struct pollfd pollfd = { .fd = f->peer_fd, .events = POLLIN };

    g_assert_cmpint(poll(&pollfd, 1, 20), ==, 0);
}

static void fixture_init_full(FWSFixture *f, unsigned context, bool enabled,
                               const char *extra, unsigned credit_delay_ms)
{
    int pair[2];

    memset(f, 0, sizeof(*f));
    f->mode = context & 3;
    f->profile = context >> 2;
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), ==, 0);
    g_assert_cmpint(fcntl(pair[0], F_SETFD, FD_CLOEXEC), ==, 0);
    f->qts = qtest_initf(
        "-machine raspi5b -m 1G -serial null -nic none "
        "-netdev socket,id=peer,fd=%d "
        "-device cyw43455-sdio,id=wifi-fws,bus=sd-bus,netdev=peer,"
        "mac=02:00:00:00:00:01,hardware-profile=%u,auto-associate=false,"
        "fws-enabled=%s,fws-credit-budget=2,fws-startup-delay-ms=10,"
        "fws-status-delay-ms=20,fws-credit-delay-ms=%u,"
        "tx-credit-delay-ms=1000,tx-credit-window=64,"
        "rx-glom-max-frames=2,rx-glom-delay-ms=5 %s",
        pair[1], f->profile, enabled ? "true" : "false", credit_delay_ms, extra);
    close(pair[1]);
    f->peer_fd = pair[0];
    select_endpoint(f);
    g_assert_cmphex(backplane(f, true, CHIP_ID, 0), ==,
                    f->profile ? 0x15264345 : 0x15294345);
}

static void fixture_init_extra(FWSFixture *f, unsigned context, bool enabled,
                                const char *extra)
{
    fixture_init_full(f, context, enabled, extra, 30);
}

static void fixture_init(FWSFixture *f, unsigned context, bool enabled)
{
    fixture_init_extra(f, context, enabled, "");
}

static void fixture_quit(FWSFixture *f)
{
    qtest_quit(f->qts);
    close(f->peer_fd);
}

static void step_ms(FWSFixture *f, unsigned ms)
{
    qtest_clock_step(f->qts, (int64_t)ms * 1000000);
}

static void event_mask(FWSFixture *f, bool credit_map, bool bcmc_support)
{
    uint8_t mask[16] = { };

    if (credit_map) {
        mask[EVENT_CREDIT_MAP / 8] = 1U << (EVENT_CREDIT_MAP % 8);
    }
    if (bcmc_support) {
        mask[EVENT_BCMC_SUPPORT / 8] = 1U << (EVENT_BCMC_SUPPORT % 8);
    }
    g_assert_cmpint(iovar(f, "event_msgs", true, mask, sizeof(mask),
                          NULL, 0), ==, 0);
}

static void credit_mask(FWSFixture *f, bool enabled)
{
    event_mask(f, enabled, enabled);
}

static void assert_bcmc_support(FWSFixture *f)
{
    uint8_t frame[FRAME_MAX];

    g_assert_cmpuint(read_frame(f, frame), ==, 88);
    g_assert_cmpuint(frame[5] & 0xf, ==, 1);
    g_assert_cmpmem(frame + 12, 4, "\x20\x00\x00\x00", 4);
    g_assert_cmphex(lduw_be_p(frame + 28), ==, 0x886c);
    g_assert_cmpuint(ldl_be_p(frame + 44), ==, EVENT_BCMC_SUPPORT);
    g_assert_cmpuint(ldl_be_p(frame + 48), ==, 0);
    g_assert_cmpuint(ldl_be_p(frame + 60), ==, 0);
    acknowledge(f);
}

static void assert_credit_map(FWSFixture *f)
{
    static const uint8_t credits[] = { 2, 2, 2, 2, 2, 0 };
    uint8_t frame[FRAME_MAX];

    g_assert_cmpuint(read_frame(f, frame), ==, 94);
    g_assert_cmpuint(frame[5] & 0xf, ==, 1);
    g_assert_cmpmem(frame + 12, 4, "\x20\x00\x00\x00", 4);
    g_assert_cmphex(lduw_be_p(frame + 28), ==, 0x886c);
    g_assert_cmphex(lduw_be_p(frame + 30), ==, 0x8001);
    g_assert_cmpmem(frame + 35, 3, "\x00\x10\x18", 3);
    g_assert_cmpuint(lduw_be_p(frame + 38), ==, 1);
    g_assert_cmpuint(lduw_be_p(frame + 40), ==, 2);
    g_assert_cmpuint(ldl_be_p(frame + 44), ==, EVENT_CREDIT_MAP);
    g_assert_cmpuint(ldl_be_p(frame + 48), ==, 0);
    g_assert_cmpuint(ldl_be_p(frame + 60), ==, sizeof(credits));
    g_assert_cmpmem(frame + 88, sizeof(credits), credits, sizeof(credits));
    acknowledge(f);
}

static void activate(FWSFixture *f)
{
    g_assert_cmpint(set_u32(f, "tlv", FWS_FLAGS), ==, 0);
    credit_mask(f, true);
    g_assert_false(frame_pending(f));
    step_ms(f, 9);
    g_assert_false(frame_pending(f));
    step_ms(f, 1);
    /* Conservative private-model ordering: enable BCMC checks before fc_active. */
    assert_bcmc_support(f);
    assert_credit_map(f);
    g_assert_false(frame_pending(f));
}

static uint32_t tag(unsigned fifo, unsigned slot, uint8_t freerun)
{
    /* Generation1, PKTFROMHOST1; independently encode all identity fields. */
    return UINT32_C(0x88000000) | (fifo << 24) | (slot << 8) | freerun;
}

static void send_signals(FWSFixture *f, const uint8_t *signals,
                         size_t signals_len, unsigned fifo,
                         const uint8_t *packet)
{
    static const uint8_t priority[] = { 1, 0, 4, 6, 0 };
    uint8_t frame[FRAME_MAX] = { };
    size_t len = 16 + signals_len + (packet ? ETH_LEN : 0);

    g_assert_cmpuint(signals_len % 4, ==, 0);
    stw_le_p(frame, len);
    stw_le_p(frame + 2, (uint16_t)~len);
    frame[4] = ++f->sequence;
    frame[5] = 2;
    frame[7] = 12;
    frame[12] = 0x20;
    frame[13] = fifo < G_N_ELEMENTS(priority) ? priority[fifo] : 0;
    frame[15] = signals_len / 4;
    memcpy(frame + 16, signals, signals_len);
    if (packet) {
        memcpy(frame + 16 + signals_len, packet, ETH_LEN);
    }
    pio(f, false, 2, 0, false, frame, (len + 3) & ~3U);
}

static void send_tagged(FWSFixture *f, uint32_t identity,
                        const uint8_t packet[ETH_LEN])
{
    uint8_t signals[8] = { FWS_PKTTAG, 4, 0, 0, 0, 0,
                           FWS_FILLER, FWS_FILLER };

    stl_le_p(signals + 2, identity);
    send_signals(f, signals, sizeof(signals), (identity >> 24) & 7, packet);
}

static void assert_status_payload(const uint8_t *frame, uint32_t identity,
                                   unsigned flags)
{
    g_assert_cmpuint(frame[5] & 0xf, ==, 2);
    g_assert_cmpmem(frame + 12, 4, "\x20\x00\x00\x02", 4);
    g_assert_cmphex(frame[16], ==, FWS_TXSTATUS);
    g_assert_cmpuint(frame[17], ==, 4);
    /* FIFO/hanger/generation/freerun survive; only terminal flags replace host. */
    g_assert_cmphex((uint32_t)ldl_le_p(frame + 18), ==,
                    (identity & ~UINT32_C(0x78000000)) | (flags << 27));
    g_assert_cmphex(frame[22], ==, FWS_FILLER);
    g_assert_cmphex(frame[23], ==, FWS_FILLER);
}

static void assert_status_flags(FWSFixture *f, uint32_t identity, unsigned flags)
{
    uint8_t frame[FRAME_MAX];

    g_assert_cmpuint(read_frame(f, frame), ==, 24);
    assert_status_payload(frame, identity, flags);
    acknowledge(f);
}

static void assert_status(FWSFixture *f, uint32_t identity)
{
    assert_status_flags(f, identity, 0);
}

static void assert_creditback(FWSFixture *f, const uint8_t counts[6])
{
    uint8_t frame[FRAME_MAX];

    g_assert_cmpuint(read_frame(f, frame), ==, 24);
    g_assert_cmpuint(frame[5] & 0xf, ==, 2);
    g_assert_cmpmem(frame + 12, 4, "\x20\x00\x00\x02", 4);
    g_assert_cmphex(frame[16], ==, FWS_CREDITBACK);
    g_assert_cmpuint(frame[17], ==, 6);
    g_assert_cmpmem(frame + 18, 6, counts, 6);
    acknowledge(f);
}

static void assert_event_mask_lengths(FWSFixture *f)
{
    static const unsigned lengths[] = { 16, 25, 27, 32 };
    uint8_t mask[33], expected[32], readback[32];

    for (unsigned variant = 0; variant < G_N_ELEMENTS(lengths); variant++) {
        for (unsigned index = 0; index < sizeof(mask); index++) {
            mask[index] = 1 + variant + 7 * index;
        }
        mask[EVENT_CREDIT_MAP / 8] &= ~(1U << (EVENT_CREDIT_MAP % 8));
        mask[EVENT_BCMC_SUPPORT / 8] &= ~(1U << (EVENT_BCMC_SUPPORT % 8));
        memset(expected, 0, sizeof(expected));
        memcpy(expected, mask, lengths[variant]);
        g_assert_cmpint(iovar(f, "event_msgs", true, mask, lengths[variant],
                              NULL, 0), ==, 0);
        memset(readback, 0xa5, sizeof(readback));
        g_assert_cmpint(iovar(f, "event_msgs", false, NULL, 0,
                              readback, sizeof(readback)), ==, 0);
        g_assert_cmpmem(readback, sizeof(readback), expected, sizeof(expected));
        g_assert_cmpint(iovar(f, "event_msgs", false, NULL, 0,
                              readback, 16), ==, 0);
        g_assert_cmpmem(readback, 16, expected, 16);
    }
    for (unsigned invalid = 0; invalid < 2; invalid++) {
        unsigned length = invalid ? 33 : 15;

        memset(mask, 0xff, sizeof(mask));
        g_assert_cmpint(iovar(f, "event_msgs", true, mask, length,
                              NULL, 0), ==, -24); /* BADLEN, no mutation. */
        g_assert_cmpint(iovar(f, "event_msgs", false, NULL, 0,
                              readback, sizeof(readback)), ==, 0);
        g_assert_cmpmem(readback, sizeof(readback), expected, sizeof(expected));
    }
}

static void test_startup(gconstpointer opaque)
{
    FWSFixture f;
    uint8_t zero_mask[16] = { };
    uint8_t readback[16];
    uint8_t packet[ETH_LEN];
    uint32_t identity = tag(1, 33, 0x44);
    const uint8_t returned[] = { 0, 1, 0, 0, 0, 0 };

    fixture_init(&f, GPOINTER_TO_UINT(opaque), true);
    g_assert_cmpuint(get_u32(&f, "tlv"), ==, 0);
    g_assert_cmpuint(get_u32(&f, "wlfc_mode"), ==, 0);
    g_assert_cmpint(set_u32(&f, "wlfc_mode", 8), <, 0);
    assert_event_mask_lengths(&f);
    g_assert_cmpint(set_u32(&f, "tlv", FWS_FLAGS), ==, 0);
    credit_mask(&f, false);
    step_ms(&f, 20);
    g_assert_false(frame_pending(&f)); /* No event74 before driver enables it. */
    g_assert_cmpint(iovar(&f, "event_msgs", false, NULL, 0,
                          readback, sizeof(readback)), ==, 0);
    g_assert_cmpmem(readback, sizeof(readback), zero_mask, sizeof(zero_mask));
    event_mask(&f, true, false);
    step_ms(&f, 20);
    g_assert_false(frame_pending(&f)); /* Missing127 must not activate FWS. */
    make_ethernet(packet, false, 0x11);
    send_tagged(&f, identity, packet);
    assert_backend_empty(&f);
    credit_mask(&f, true);
    step_ms(&f, 9);
    g_assert_false(frame_pending(&f));
    step_ms(&f, 1);
    assert_bcmc_support(&f);
    assert_credit_map(&f);
    credit_mask(&f, true);
    step_ms(&f, 20);
    g_assert_false(frame_pending(&f)); /* Rewriting mask must not mint credit. */
    g_assert_cmpuint(get_u32(&f, "tlv"), ==, FWS_FLAGS);
    send_tagged(&f, identity, packet); /* Pre-activation drop owns no hanger. */
    expect_backend(&f, packet);
    step_ms(&f, 20);
    assert_status(&f, identity);
    step_ms(&f, 30);
    assert_creditback(&f, returned);
    fixture_quit(&f);
}

static void test_default_disabled(gconstpointer opaque)
{
    FWSFixture f;
    g_autoptr(QDict) response = NULL;

    fixture_init(&f, GPOINTER_TO_UINT(opaque), false);
    response = qtest_qmp(f.qts,
        "{ 'execute': 'qom-get', 'arguments': { "
        "'path': '/machine/peripheral/wifi-fws', "
        "'property': 'fws-status-batch-max' } }");
    g_assert_nonnull(response);
    g_assert_false(qdict_haskey(response, "error"));
    g_assert_true(qdict_haskey(response, "return"));
    g_assert_cmpuint(qdict_get_int(response, "return"), ==, 1);
    g_assert_cmpint(set_u32(&f, "tlv", FWS_FLAGS), <, 0);
    credit_mask(&f, true);
    step_ms(&f, 100);
    g_assert_false(frame_pending(&f));
    fixture_quit(&f);
}

static void test_status_and_host_mode(gconstpointer opaque)
{
    FWSFixture f;
    uint8_t packet[ETH_LEN];
    unsigned host_credit = 2;
    uint32_t first = tag(1, 0x123, 0x45);
    uint32_t second = tag(1, 0x234, 0x67);
    uint32_t resumed = tag(1, 0x345, 0x89);
    uint8_t returned[6] = { };

    fixture_init(&f, GPOINTER_TO_UINT(opaque), true);
    activate(&f);
    make_ethernet(packet, false, 0x20);
    send_tagged(&f, first, packet);
    expect_backend(&f, packet);
    host_credit--;
    send_tagged(&f, second, packet);
    expect_backend(&f, packet);
    host_credit--;
    g_assert_cmpuint(host_credit, ==, 0);
    /* Host ledger waits; another AC may still legally lend firmware credit. */
    step_ms(&f, 19);
    g_assert_false(frame_pending(&f));
    step_ms(&f, 1);
    assert_status(&f, first);
    assert_status(&f, second);
    g_assert_false(frame_pending(&f));
    if (f.mode == 1) {
        /* Mode1 ignores explicit CREDITBACK: terminal status returns credits. */
        host_credit += 2;
        /* Hanger/freerun wrap may legitimately reuse the complete old tag. */
        send_tagged(&f, first, packet);
        expect_backend(&f, packet); /* Must work before delayed CREDITBACK. */
        host_credit--;
        step_ms(&f, 20);
        assert_status(&f, first);
        host_credit++;
        step_ms(&f, 9);
        g_assert_false(frame_pending(&f));
        step_ms(&f, 1);
        returned[1] = 3;
        assert_creditback(&f, returned);
    } else {
        /* Mode2 status retires a hanger but does not refill the host ledger. */
        g_assert_cmpuint(host_credit, ==, 0);
        step_ms(&f, 29);
        g_assert_false(frame_pending(&f));
        step_ms(&f, 1);
        returned[1] = 2;
        assert_creditback(&f, returned);
        host_credit += returned[1];
        send_tagged(&f, resumed, packet);
        expect_backend(&f, packet);
        host_credit--;
        step_ms(&f, 20);
        assert_status(&f, resumed);
        g_assert_cmpuint(host_credit, ==, 1);
        step_ms(&f, 30);
        returned[1] = 1;
        assert_creditback(&f, returned);
        host_credit += returned[1];
    }
    g_assert_cmpuint(host_credit, ==, 2);
    g_assert_false(frame_pending(&f));
    assert_backend_empty(&f);
    fixture_quit(&f);
}

static void test_ac_borrowing(gconstpointer opaque)
{
    FWSFixture f;
    uint8_t packet[ETH_LEN];
    uint8_t returned[6] = { 0, 0, 0, 8, 0, 0 };
    uint32_t identities[8];
    uint32_t resumed = tag(3, 1080, 0xf0);

    fixture_init(&f, GPOINTER_TO_UINT(opaque), true);
    activate(&f);
    make_ethernet(packet, false, 0xe0);
    /* VO borrows the lower ACs' six credits; the separate BCMC two stay put. */
    for (unsigned index = 0; index < G_N_ELEMENTS(identities); index++) {
        identities[index] = tag(3, 1000 + index, 0xd0 + index);
        send_tagged(&f, identities[index], packet);
        expect_backend(&f, packet);
    }
    send_tagged(&f, resumed, packet);
    assert_backend_empty(&f);
    step_ms(&f, 20);
    for (unsigned index = 0; index < G_N_ELEMENTS(identities); index++) {
        assert_status(&f, identities[index]);
    }
    step_ms(&f, 30);
    assert_creditback(&f, returned);
    send_tagged(&f, resumed, packet);
    expect_backend(&f, packet);
    step_ms(&f, 20);
    assert_status(&f, resumed);
    step_ms(&f, 30);
    returned[3] = 1;
    assert_creditback(&f, returned);
    g_assert_false(frame_pending(&f));
    fixture_quit(&f);
}

static void assert_interface(FWSFixture *f, bool open)
{
    uint8_t frame[FRAME_MAX];

    g_assert_cmpuint(read_frame(f, frame), ==, 20);
    g_assert_cmpuint(frame[5] & 0xf, ==, 2);
    g_assert_cmpmem(frame + 12, 4, "\x20\x00\x00\x01", 4);
    g_assert_cmpuint(frame[16], ==, open ? 9 : 10);
    g_assert_cmpuint(frame[17], ==, 1);
    g_assert_cmpuint(frame[18], ==, 0); /* Primary interface. */
    g_assert_cmpuint(frame[19], ==, FWS_FILLER);
    acknowledge(f);
}

static void test_interface_close_open(gconstpointer opaque)
{
    FWSFixture f;
    uint8_t packet[ETH_LEN];
    uint32_t closed_tag = tag(1, 2000, 0x11);
    uint32_t open_tag = tag(1, 2001, 0x22);
    const uint8_t returned[] = { 0, 1, 0, 0, 0, 0 };

    fixture_init(&f, GPOINTER_TO_UINT(opaque), true);
    activate(&f);
    g_assert_cmpint(control(&f, 3, true, NULL, 0, NULL, 0), ==, 0); /* C_DOWN */
    assert_interface(&f, false);
    make_ethernet(packet, false, 0x90);
    send_tagged(&f, closed_tag, packet);
    assert_backend_empty(&f);
    g_assert_cmpint(control(&f, 2, true, NULL, 0, NULL, 0), ==, -16);
    g_assert_false(frame_pending(&f)); /* BUSY cannot retire a host hanger. */
    step_ms(&f, 20);
    assert_status_flags(&f, closed_tag, 3); /* FW_TOSSED, exactly this hanger. */
    g_assert_cmpint(control(&f, 2, true, NULL, 0, NULL, 0), ==, -16);
    step_ms(&f, 30);
    assert_creditback(&f, returned);
    g_assert_cmpint(control(&f, 2, true, NULL, 0, NULL, 0), ==, 0); /* C_UP */
    assert_interface(&f, true);
    send_tagged(&f, open_tag, packet);
    expect_backend(&f, packet);
    step_ms(&f, 20);
    assert_status(&f, open_tag);
    step_ms(&f, 30);
    assert_creditback(&f, returned);
    g_assert_false(frame_pending(&f));
    fixture_quit(&f);
}

static void test_interface_immediate_close_open(gconstpointer opaque)
{
    FWSFixture f;
    uint16_t down_id, up_id;
    int32_t down_status, up_status;

    fixture_init(&f, GPOINTER_TO_UINT(opaque), true);
    activate(&f);

    /*
     * Model the brcmfmac AP-stop sequence: C_UP may already be queued while
     * the successful C_DOWN reply still owns RX, so the FWS close indication
     * cannot have reached the host.  Both controls and both ordered interface
     * notifications must survive that overlap.
     */
    down_id = send_control(&f, 3, true, NULL, 0, 0); /* C_DOWN */
    up_id = send_control(&f, 2, true, NULL, 0, 0);   /* C_UP */
    down_status = receive_control(&f, 3, down_id, NULL, 0);
    up_status = receive_control(&f, 2, up_id, NULL, 0);
    g_test_message("overlapped C_DOWN status=%d, C_UP status=%d",
                   (int)down_status, (int)up_status);
    g_assert_cmpint(down_status, ==, 0);
    g_assert_cmpint(up_status, ==, 0);
    assert_interface(&f, false);
    assert_interface(&f, true);
    g_assert_false(frame_pending(&f));
    fixture_quit(&f);
}

static void test_malformed_and_duplicate(gconstpointer opaque)
{
    FWSFixture f;
    uint8_t packet[ETH_LEN];
    uint8_t signals[20], completion[24];
    uint32_t identity = tag(1, 123, 0x51);
    const uint8_t returned[] = { 0, 1, 0, 0, 0, 0 };
    const uint8_t pending[] = { FWS_PKTTAG, 4, 0, 0, 0, 0,
                                12, 2, 0, 1, FWS_FILLER, FWS_FILLER };

    fixture_init(&f, GPOINTER_TO_UINT(opaque), true);
    activate(&f);
    make_ethernet(packet, false, 0x40);
    for (unsigned malformed = 0; malformed < 10; malformed++) {
        size_t length = 8;

        memset(signals, FWS_FILLER, sizeof(signals));
        signals[0] = FWS_PKTTAG;
        signals[1] = 4;
        stl_le_p(signals + 2, identity);
        switch (malformed) {
        case 0:
            memset(signals, FWS_FILLER, sizeof(signals)); /* No PKTTAG. */
            break;
        case 1:
            signals[1] = 3; /* PKTTAG has exactly four identity bytes. */
            break;
        case 2:
            signals[1] = 9; /* Claimed TLV extends beyond BCDC offset. */
            break;
        case 3:
            memcpy(signals + 6, signals, 6); /* Duplicate PKTTAG TLV. */
            length = 12;
            break;
        case 4:
            stl_le_p(signals + 2, tag(5, 123, 0x51)); /* Unsupported FIFO. */
            break;
        case 5:
            stl_le_p(signals + 2, tag(1, 3072, 0x51)); /* Hanger overflow. */
            break;
        case 6:
            stl_le_p(signals + 2, identity & ~UINT32_C(0x78000000));
            break; /* Missing PKTFROMHOST flag. */
        case 7:
            signals[6] = 12;
            signals[7] = 2;
            signals[8] = 0;
            signals[9] = 1;
            memcpy(signals + 10, signals + 6, 4); /* Duplicate bitmap. */
            length = 16;
            break;
        case 8:
            signals[6] = 12;
            signals[7] = 1; /* Pending bitmap has exactly two bytes. */
            signals[8] = 0;
            length = 12;
            break;
        case 9:
            signals[0] = 12;
            signals[1] = 2;
            signals[2] = 0;
            signals[3] = 1; /* A bitmap alone does not own an Ethernet tag. */
            length = 4;
            break;
        default:
            g_assert_not_reached();
        }
        send_signals(&f, signals, length, 1, packet);
        assert_backend_empty(&f);
    }
    /* A host TIM bitmap without Ethernet must not manufacture a completion. */
    send_signals(&f, pending, sizeof(pending), 1, NULL);
    step_ms(&f, 25);
    g_assert_false(frame_pending(&f));
    send_tagged(&f, identity, packet);
    expect_backend(&f, packet);
    send_tagged(&f, identity, packet); /* Exact duplicate while hanger pending. */
    assert_backend_empty(&f);
    step_ms(&f, 20);
    g_assert_true(frame_pending(&f));
    /* Serializing a status does not hand off a partly unread host hanger. */
    pio(&f, true, 2, 0, false, completion, 4);
    g_assert_cmpuint(lduw_le_p(completion), ==, sizeof(completion));
    g_assert_cmphex(lduw_le_p(completion + 2), ==,
                    (uint16_t)~sizeof(completion));
    send_tagged(&f, identity, packet);
    assert_backend_empty(&f);
    pio(&f, true, 2, 0, false, completion + 4, sizeof(completion) - 4);
    assert_status_payload(completion, identity, 0);
    acknowledge(&f);
    g_assert_false(frame_pending(&f));
    step_ms(&f, 30);
    assert_creditback(&f, returned);
    g_assert_false(frame_pending(&f));
    assert_backend_empty(&f);
    /* Ordinary Linux packets may piggyback a primary-interface TIM bitmap. */
    identity = tag(1, 124, 0x52);
    memcpy(signals, pending, sizeof(pending));
    stl_le_p(signals + 2, identity);
    send_signals(&f, signals, sizeof(pending), 1, packet);
    expect_backend(&f, packet);
    step_ms(&f, 20);
    assert_status(&f, identity);
    step_ms(&f, 30);
    assert_creditback(&f, returned);
    g_assert_false(frame_pending(&f));
    fixture_quit(&f);
}

static void test_only_migratable(gconstpointer opaque)
{
    FWSFixture f;
    uint8_t packet[ETH_LEN], mask[16] = { }, readback[32];

    fixture_init_extra(&f, GPOINTER_TO_UINT(opaque), true, "-only-migratable");
    mask[EVENT_CREDIT_MAP / 8] = 1U << (EVENT_CREDIT_MAP % 8);
    mask[EVENT_BCMC_SUPPORT / 8] = 1U << (EVENT_BCMC_SUPPORT % 8);
    g_assert_cmpint(iovar(&f, "event_msgs", true, mask, sizeof(mask),
                          NULL, 0), ==, -16);
    memset(readback, 0xa5, sizeof(readback));
    g_assert_cmpint(iovar(&f, "event_msgs", false, NULL, 0,
                          readback, sizeof(readback)), ==, 0);
    for (unsigned index = 0; index < sizeof(readback); index++) {
        g_assert_cmpuint(readback[index], ==, 0);
    }
    g_assert_cmpint(set_u32(&f, "tlv", FWS_FLAGS), ==, -16);
    g_assert_cmpuint(get_u32(&f, "tlv"), ==, 0);
    step_ms(&f, 100);
    g_assert_false(frame_pending(&f));
    make_ethernet(packet, false, 0x33);
    send_tagged(&f, tag(1, 55, 0x66), packet);
    assert_backend_empty(&f);
    fixture_quit(&f);
}

static void test_notification_pressure(gconstpointer opaque)
{
    FWSFixture f;
    uint8_t packet[ETH_LEN];
    uint32_t identities[10];
    const uint8_t returned[] = { 2, 2, 2, 2, 2, 0 };

    fixture_init(&f, GPOINTER_TO_UINT(opaque), true);
    activate(&f);
    make_ethernet(packet, false, 0x60);
    for (unsigned fifo = 0; fifo < 5; fifo++) {
        for (unsigned index = 0; index < 2; index++) {
            unsigned position = 2 * fifo + index;

            identities[position] = tag(fifo, 100 + position, 0x70 + position);
            send_tagged(&f, identities[position], packet);
            expect_backend(&f, packet);
        }
    }
    /* Every advertised FIFO is now exhausted; control/SDPCM ACKs are not FWS. */
    for (unsigned index = 0; index < 16; index++) {
        send_tagged(&f, tag(index % 5, 500 + index, index), packet);
    }
    assert_backend_empty(&f);
    step_ms(&f, 20);
    g_assert_true(frame_pending(&f));
    /* Leave the oldest notification unread while attacking a later hanger. */
    send_tagged(&f, identities[9], packet);
    send_tagged(&f, tag(4, 900, 0x90), packet);
    assert_backend_empty(&f);
    for (unsigned index = 0; index < G_N_ELEMENTS(identities); index++) {
        assert_status(&f, identities[index]);
    }
    g_assert_false(frame_pending(&f));
    step_ms(&f, 30);
    assert_creditback(&f, returned);
    g_assert_false(frame_pending(&f));
    assert_backend_empty(&f);
    fixture_quit(&f);
}

static void test_reset_pending(gconstpointer opaque)
{
    FWSFixture f;
    uint8_t packet[ETH_LEN];
    uint32_t identity = tag(1, 77, 0xa1);
    const uint8_t returned[] = { 0, 1, 0, 0, 0, 0 };

    fixture_init(&f, GPOINTER_TO_UINT(opaque), true);
    activate(&f);
    make_ethernet(packet, false, 0x80);
    send_tagged(&f, identity, packet);
    expect_backend(&f, packet);
    step_ms(&f, 20);
    g_assert_true(frame_pending(&f)); /* Unread status plus future CREDITBACK. */
    cmd52(&f, true, 0, ABORT, 8);
    select_endpoint(&f);
    step_ms(&f, 100);
    g_assert_false(frame_pending(&f));
    g_assert_cmpuint(get_u32(&f, "tlv"), ==, 0);
    activate(&f);
    send_tagged(&f, identity, packet); /* Reset retired the old hanger. */
    expect_backend(&f, packet);
    step_ms(&f, 20);
    assert_status(&f, identity);
    step_ms(&f, 30);
    assert_creditback(&f, returned);
    g_assert_false(frame_pending(&f));
    fixture_quit(&f);
}

static void inject_backend(FWSFixture *f, uint8_t packet[ETH_LEN])
{
    uint32_t length = htonl(ETH_LEN);

    socket_io(f->peer_fd, false, &length, sizeof(length));
    socket_io(f->peer_fd, false, packet, ETH_LEN);
}

static void test_glom_notification_order(gconstpointer opaque)
{
    FWSFixture f;
    uint8_t outgoing[ETH_LEN], incoming[2][ETH_LEN];
    uint8_t frame[FRAME_MAX];
    uint32_t identity = tag(1, 99, 0xb1);
    const uint8_t returned[] = { 0, 1, 0, 0, 0, 0 };
    int64_t deadline;

    fixture_init(&f, GPOINTER_TO_UINT(opaque), true);
    /* Enable receive before FWS activation, with no synthetic association. */
    g_assert_cmpint(control(&f, 2, true, NULL, 0, NULL, 0), ==, 0); /* C_UP */
    activate(&f);
    make_ethernet(outgoing, false, 0xa0);
    send_tagged(&f, identity, outgoing);
    expect_backend(&f, outgoing);
    for (unsigned index = 0; index < 2; index++) {
        make_ethernet(incoming[index], true, 0xc0 + index);
        inject_backend(&f, incoming[index]);
    }
    deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while (!frame_pending(&f)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    /* The glom descriptor is committed before the status timer expires. */
    step_ms(&f, 20);
    g_assert_cmpuint(read_frame(&f, frame), ==, 16);
    g_assert_cmphex(frame[5], ==, 0x83);
    g_assert_cmpuint(lduw_le_p(frame + 12), ==, 88);
    g_assert_cmpuint(lduw_le_p(frame + 14), ==, 76);
    /* Do not ACK between a committed descriptor and its superframe. */
    /* A committed descriptor's block-padded superframe must remain adjacent. */
    g_assert_true(frame_pending(&f));
    pio(&f, true, 2, 0, false, frame, sizeof(frame));
    g_assert_cmpuint(lduw_le_p(frame), ==, 164);
    g_assert_cmphex(lduw_le_p(frame + 2), ==, (uint16_t)~164);
    g_assert_cmpuint(frame[5] & 0xf, ==, 3);
    g_assert_cmpuint(frame[7], ==, 12);
    for (unsigned index = 0; index < 2; index++) {
        unsigned offset = 12 + 76 * index;

        g_assert_cmpuint(lduw_le_p(frame + offset), ==, 76);
        g_assert_cmphex(lduw_le_p(frame + offset + 2), ==, (uint16_t)~76);
        g_assert_cmpuint(frame[offset + 5] & 0xf, ==, 2);
        g_assert_cmpuint(frame[offset + 7], ==, 12);
        g_assert_cmpmem(frame + offset + 16, ETH_LEN, incoming[index], ETH_LEN);
    }
    for (unsigned index = 164; index < sizeof(frame); index++) {
        g_assert_cmphex(frame[index], ==, 0);
    }
    acknowledge(&f);
    assert_status(&f, identity); /* No FWS notification split descriptor/super. */
    step_ms(&f, 30);
    assert_creditback(&f, returned);
    g_assert_false(frame_pending(&f));
    fixture_quit(&f);
}

static void assert_status_batch(const uint8_t *frame, const uint32_t *tags,
                                unsigned count)
{
    unsigned signal_len = (6 * count + 3) & ~3U;

    g_assert_cmpuint(lduw_le_p(frame), ==, 16 + signal_len);
    g_assert_cmphex(lduw_le_p(frame + 2), ==, (uint16_t)~(16 + signal_len));
    g_assert_cmpuint(frame[5] & 0xf, ==, 2);
    g_assert_cmpuint(frame[7], ==, 12);
    g_assert_cmpmem(frame + 12, 3, "\x20\x00\x00", 3);
    g_assert_cmpuint(frame[15], ==, signal_len / 4);
    for (unsigned index = 0; index < count; index++) {
        unsigned offset = 16 + 6 * index;

        g_assert_cmpuint(frame[offset], ==, FWS_TXSTATUS);
        g_assert_cmpuint(frame[offset + 1], ==, 4);
        g_assert_cmphex((uint32_t)ldl_le_p(frame + offset + 2), ==,
                        tags[index] & ~UINT32_C(0x78000000));
    }
    for (unsigned index = 6 * count; index < signal_len; index++) {
        g_assert_cmpuint(frame[16 + index], ==, FWS_FILLER);
    }
}

static void test_batch_handoff(gconstpointer opaque)
{
    FWSFixture f;
    uint8_t packet[ETH_LEN], frame[FRAME_MAX], returned[6] = { };
    uint32_t tags[4];
    uint32_t extra = tag(4, 450, 0x71);
    unsigned host_credit = 4; /* BE2 plus legitimate BK2 borrowing. */

    fixture_init_extra(&f, GPOINTER_TO_UINT(opaque), true,
                       "-global cyw43455-sdio.fws-status-batch-max=4");
    activate(&f);
    make_ethernet(packet, false, 0x31);
    for (unsigned index = 0; index < G_N_ELEMENTS(tags); index++) {
        tags[index] = tag(1, 400 + index, 0x40 + index);
        send_tagged(&f, tags[index], packet);
        expect_backend(&f, packet);
        host_credit--;
    }
    step_ms(&f, 19);
    g_assert_false(frame_pending(&f));
    step_ms(&f, 1);
    pio(&f, true, 2, 0, false, frame, 36);
    g_assert_cmpuint(lduw_le_p(frame), ==, 40);
    /* Even the first three complete TLVs retain ownership until frame end. */
    for (unsigned index = 0; index < G_N_ELEMENTS(tags); index++) {
        send_tagged(&f, tags[index], packet);
        assert_backend_empty(&f);
    }
    /* A distinct BCMC hanger may coexist with all four staged statuses. */
    send_tagged(&f, extra, packet);
    expect_backend(&f, packet);
    step_ms(&f, 30);
    /* No explicit credits may be generated by a partially read batch. */
    pio(&f, true, 2, 0, false, frame + 36, 4);
    assert_status_batch(frame, tags, G_N_ELEMENTS(tags));
    acknowledge(&f);
    assert_status(&f, extra); /* Accept/status traces count queued+staged. */
    g_assert_false(frame_pending(&f));
    g_assert_cmpint(set_u32(&f, "tlv", 0), ==, -16);
    g_assert_cmpint(control(&f, 3, true, NULL, 0, NULL, 0), ==, -16);
    if (f.mode == 1) {
        host_credit += 4;
    }
    g_assert_cmpuint(host_credit, ==, f.mode == 1 ? 4 : 0);
    if (f.mode == 2) {
        step_ms(&f, 29);
        g_assert_false(frame_pending(&f));
        step_ms(&f, 1);
        returned[1] = 4;
        returned[4] = 1;
        assert_creditback(&f, returned);
        host_credit += returned[1];
        returned[4] = 0;
    }
    /* Every full tag may be reused; mode1 does so before late CREDITBACK. */
    for (unsigned index = 0; index < G_N_ELEMENTS(tags); index++) {
        send_tagged(&f, tags[index], packet);
        expect_backend(&f, packet);
        host_credit--;
    }
    step_ms(&f, 20);
    g_assert_cmpuint(read_frame(&f, frame), ==, 40);
    assert_status_batch(frame, tags, G_N_ELEMENTS(tags));
    acknowledge(&f);
    if (f.mode == 1) {
        host_credit += 4;
        step_ms(&f, 9);
        g_assert_false(frame_pending(&f));
        step_ms(&f, 1);
        returned[1] = 8;
        returned[4] = 1;
        assert_creditback(&f, returned);
    } else {
        step_ms(&f, 30);
        returned[1] = 4;
        assert_creditback(&f, returned);
        host_credit += returned[1];
    }
    g_assert_cmpuint(host_credit, ==, 4);
    g_assert_false(frame_pending(&f));
    fixture_quit(&f);
}

static void test_batch_eight(gconstpointer opaque)
{
    FWSFixture f;
    uint8_t packet[ETH_LEN], frame[FRAME_MAX];
    uint8_t returned[] = { 0, 0, 0, 8, 0, 0 };
    uint32_t tags[8], extra = tag(3, 600, 0x70);

    fixture_init_extra(&f, GPOINTER_TO_UINT(opaque), true,
                       "-global cyw43455-sdio.fws-status-batch-max=8");
    activate(&f);
    make_ethernet(packet, false, 0x51);
    for (unsigned index = 0; index < G_N_ELEMENTS(tags); index++) {
        tags[index] = tag(3, 500 + index, 0x50 + index);
        send_tagged(&f, tags[index], packet); /* VO borrows all lower ACs. */
        expect_backend(&f, packet);
    }
    step_ms(&f, 20);
    pio(&f, true, 2, 0, false, frame, 60);
    g_assert_cmpuint(lduw_le_p(frame), ==, 64);
    send_tagged(&f, extra, packet); /* No internal AC capacity retired early. */
    assert_backend_empty(&f);
    pio(&f, true, 2, 0, false, frame + 60, 4);
    assert_status_batch(frame, tags, G_N_ELEMENTS(tags));
    acknowledge(&f);
    if (f.mode == 1) {
        send_tagged(&f, extra, packet);
        expect_backend(&f, packet);
        step_ms(&f, 20);
        assert_status(&f, extra); /* Short batch preserves ordinary padding. */
        step_ms(&f, 10);
        returned[3] = 9;
    } else {
        step_ms(&f, 29);
        g_assert_false(frame_pending(&f));
        step_ms(&f, 1);
    }
    assert_creditback(&f, returned);
    g_assert_false(frame_pending(&f));
    fixture_quit(&f);
}

static void test_batch_reset(gconstpointer opaque)
{
    FWSFixture f;
    uint8_t packet[ETH_LEN], frame[FRAME_MAX];
    uint32_t tags[4];
    const uint8_t returned[] = { 0, 3, 0, 0, 0, 0 };

    fixture_init_extra(&f, GPOINTER_TO_UINT(opaque), true,
                       "-global cyw43455-sdio.fws-status-batch-max=4");
    activate(&f);
    make_ethernet(packet, false, 0x61);
    for (unsigned index = 0; index < G_N_ELEMENTS(tags); index++) {
        tags[index] = tag(1, 700 + index, 0x60 + index);
        send_tagged(&f, tags[index], packet);
        expect_backend(&f, packet);
    }
    step_ms(&f, 20);
    pio(&f, true, 2, 0, false, frame, 36);
    cmd52(&f, true, 0, ABORT, 8); /* Cold reset, partly unread four-tag batch. */
    select_endpoint(&f);
    step_ms(&f, 100);
    g_assert_false(frame_pending(&f));
    g_assert_cmpuint(get_u32(&f, "tlv"), ==, 0);
    activate(&f);
    for (unsigned index = 0; index < 3; index++) {
        send_tagged(&f, tags[index], packet);
        expect_backend(&f, packet);
    }
    step_ms(&f, 20);
    g_assert_cmpuint(read_frame(&f, frame), ==, 36);
    assert_status_batch(frame, tags, 3); /* Exact two final filler bytes. */
    acknowledge(&f);
    step_ms(&f, 30);
    assert_creditback(&f, returned); /* No old reset-batch credit survives. */
    g_assert_false(frame_pending(&f));
    fixture_quit(&f);
}

static void test_batch_credit_pressure(gconstpointer opaque)
{
    FWSFixture f;
    uint8_t packet[ETH_LEN], frame[FRAME_MAX], returned[6] = { };
    uint32_t tags[4];
    unsigned sent = 0;

    fixture_init_full(&f, GPOINTER_TO_UINT(opaque), true,
                      "-global cyw43455-sdio.fws-status-batch-max=4", 1000);
    activate(&f);
    make_ethernet(packet, false, 0x81);
    /* Firmware-side accumulation stress, not a mode2 host-credit policy. */
    for (unsigned round = 0; round < 16; round++) {
        unsigned count = round == 15 ? 3 : 4;

        for (unsigned index = 0; index < count; index++) {
            tags[index] = tag(1, 800 + sent, sent);
            send_tagged(&f, tags[index], packet);
            expect_backend(&f, packet);
            sent++;
        }
        step_ms(&f, 20);
        g_assert_cmpuint(read_frame(&f, frame), ==, count == 4 ? 40 : 36);
        assert_status_batch(frame, tags, count);
        acknowledge(&f);
        /* Refresh only SDPCM credits; do not return any FWS FIFO credit. */
        g_assert_cmpuint(get_u32(&f, "tlv"), ==, FWS_FLAGS);
    }
    g_assert_cmpuint(sent, ==, 63);
    for (unsigned index = 0; index < 2; index++) {
        tags[index] = tag(1, 800 + sent, sent);
        send_tagged(&f, tags[index], packet);
        expect_backend(&f, packet);
        sent++;
    }
    step_ms(&f, 20);
    g_assert_cmpuint(read_frame(&f, frame), ==, 24);
    assert_status_batch(frame, tags, 1); /* 63 + selected1 is exactly64. */
    acknowledge(&f);
    g_assert_false(frame_pending(&f)); /* Second due tag must stay queued. */
    /* First full read was at30ms; its1000ms credit deadline is unchanged. */
    step_ms(&f, 679);
    g_assert_false(frame_pending(&f));
    step_ms(&f, 1);
    returned[1] = 64;
    assert_creditback(&f, returned);
    assert_status(&f, tags[1]); /* Credit emission unblocks the saved prefix. */
    g_assert_cmpuint(get_u32(&f, "tlv"), ==, FWS_FLAGS);
    step_ms(&f, 999);
    g_assert_false(frame_pending(&f));
    step_ms(&f, 1);
    returned[1] = 1;
    assert_creditback(&f, returned);
    g_assert_false(frame_pending(&f));
    fixture_quit(&f);
}
#endif

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!qtest_has_machine("raspi5b")) {
        return 0;
    }
#ifndef _WIN32
    static const struct {
        const char *name;
        GTestDataFunc test;
    } tests[] = {
        { "default-disabled", test_default_disabled },
        { "only-migratable-refuses-negotiation", test_only_migratable },
        { "event-mask-delayed-startup", test_startup },
        { "tag-status-credit-host-ledger", test_status_and_host_mode },
        { "ac-credit-borrowing", test_ac_borrowing },
        { "interface-close-open", test_interface_close_open },
        { "interface-immediate-close-open",
          test_interface_immediate_close_open },
        { "malformed-duplicate-fifo-hanger", test_malformed_and_duplicate },
        { "notification-pressure", test_notification_pressure },
        { "pending-reset", test_reset_pending },
        { "glom-notification-order", test_glom_notification_order },
        { "batch-four-full-frame-handoff", test_batch_handoff },
        { "batch-eight-upper-bound", test_batch_eight },
        { "batch-partial-reset-padding", test_batch_reset },
        { "batch-pending-credit64-pressure", test_batch_credit_pressure },
    };

    for (unsigned profile = 0; profile <= 1; profile++) {
        for (unsigned mode = 1; mode <= 2; mode++) {
            for (unsigned index = 0; index < G_N_ELEMENTS(tests); index++) {
                g_autofree char *path = g_strdup_printf(
                    "/cyw43455/model-profile%u/fws%u/%s", profile, mode,
                    tests[index].name);

                qtest_add_data_func(path, GUINT_TO_POINTER((profile << 2) | mode),
                                    tests[index].test);
            }
        }
    }
#endif
    return g_test_run();
}
