/*
 * Synthetic CYW43455 single-BSS open AP tests, not RF/firmware/WPA tests.
 * Both hardware identities use a manual endpoint on the empty SDIO1 bus.
 * This does not claim Pi400 board integration in the Pi5 source tree.
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

#define HOST            UINT64_C(0x1000fff000)
#define SD_STATUS       0x30
#define SD_ENABLE       0x34
#define IOEX            0x02
#define IORX            0x03
#define INTPEND         0x05
#define ABORT           0x06
#define SBADDRLOW       0x1000a
#define SBADDRMID       0x1000b
#define SBADDRHIGH      0x1000c
#define CHIP_ID         0x18000000
#define INTSTATUS       0x18004020
#define FRAME_IND       (1U << 6)
#define C_UP            2
#define C_DOWN          3
#define C_SET_PROMISC   10
#define C_GET_INFRA     19
#define C_SET_INFRA     20
#define C_GET_SSID      25
#define C_SET_SSID      26
#define C_GET_REGULATORY 46
#define C_SET_REGULATORY 47
#define C_GET_BCNPRD    75
#define C_SET_BCNPRD    76
#define C_GET_DTIMPRD   77
#define C_SET_DTIMPRD   78
#define C_GET_REVINFO   98
#define C_GET_AP        117
#define C_SET_AP        118
#define C_GET_BSS_INFO  136
#define C_GET_BANDLIST  140
#define C_GET_ASSOCLIST 159
#define C_GET_VAR       262
#define C_SET_VAR       263
#define CH36_20         0xd024
#define LINK_EVENT      16
#define ASSOC_IND       8
#define DISASSOC_IND    12
#define FRAME_MAX       512
#define JOIN_LEN        52
#define EVENT_BASE      88
#define EVENT_MSG       40
#define ETH_LEN         60

static const uint8_t ap_mac[] = { 2, 0, 0, 0, 0, 1 };
static const uint8_t peer_mac[] = { 0x52, 0x55, 0x0a, 0, 2, 2 };
static const uint8_t other_mac[] = { 2, 0, 0, 0, 0, 9 };
static const char ap_ssid[] = "qtest-open-ap";

typedef struct APFixture {
    QTestState *qts;
    unsigned profile;
    uint16_t request_id;
    uint8_t sequence;
    int peer_fd;
} APFixture;

static void command(APFixture *f, unsigned cmd, uint32_t argument)
{
    sdhci_cmd_regs(f->qts, HOST, 0, 0, argument, 0,
                   (cmd << 8) | SDHC_CMD_RESPONSE);
    g_assert_cmphex(qtest_readl(f->qts, HOST + SD_STATUS) >> 16, ==, 0);
}

static uint8_t cmd52(APFixture *f, bool write, unsigned function,
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

static void select_endpoint(APFixture *f)
{
    qtest_writeb(f->qts, HOST + SDHC_SWRST, SDHC_RESET_ALL);
    qtest_writew(f->qts, HOST + SDHC_CLKCON, 7);
    qtest_writel(f->qts, HOST + SD_ENABLE, 0xffff013f);
    command(f, 5, 0);
    g_assert_cmphex(qtest_readl(f->qts, HOST + SDHC_RSPREG0), ==, 0xb0ff8000);
    command(f, 3, 0);
    g_assert_cmphex(qtest_readl(f->qts, HOST + SDHC_RSPREG0), ==, 0x10000);
    command(f, 7, 0x10000);
    g_assert_cmphex(cmd52(f, true, 0, IOEX, 0x0e), ==, 0x0e);
    g_assert_cmphex(cmd52(f, false, 0, IORX, 0), ==, 0x0e);
}

static void pio(APFixture *f, bool read, unsigned function,
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

static uint32_t backplane(APFixture *f, bool read, uint32_t address,
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

static bool frame_pending(APFixture *f)
{
    return backplane(f, true, INTSTATUS, 0) & FRAME_IND;
}

static void acknowledge(APFixture *f)
{
    backplane(f, false, INTSTATUS, FRAME_IND);
}

/* Read exactly one padded frame; leave W1C explicit for event-order tests. */
static size_t read_frame(APFixture *f, uint8_t frame[FRAME_MAX])
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

static uint16_t send_control(APFixture *f, unsigned cmd, bool set,
                             unsigned ifidx, const void *input,
                             size_t input_len, size_t output_len)
{
    uint8_t frame[FRAME_MAX] = { };
    size_t len = 28 + input_len;
    size_t data_len = MAX(input_len, output_len);
    uint16_t id = ++f->request_id;

    g_assert_cmpuint(len, <=, FRAME_MAX);
    g_assert_cmpuint(28 + data_len, <=, FRAME_MAX);
    stw_le_p(frame, len);
    stw_le_p(frame + 2, (uint16_t)~len);
    frame[4] = ++f->sequence;
    frame[7] = 12;
    stl_le_p(frame + 12, cmd);
    stl_le_p(frame + 16, data_len);
    stl_le_p(frame + 20, ((uint32_t)id << 16) | (ifidx << 12) |
                         (set ? 2 : 0));
    if (input_len) {
        memcpy(frame + 28, input, input_len);
    }
    pio(f, false, 2, 0, false, frame, (len + 3) & ~3U);
    return id;
}

static int32_t receive_control(APFixture *f, unsigned cmd, uint16_t id,
                                uint8_t *output, size_t output_len)
{
    uint8_t frame[FRAME_MAX];
    size_t len;
    uint32_t flags;
    int32_t status;

    /* Data TX may leave one header-only credit update ahead of the reply. */
    len = read_frame(f, frame);
    if (len == 12) {
        g_assert_cmpuint(frame[5] & 0xf, ==, 1);
        acknowledge(f);
        len = read_frame(f, frame);
    }
    g_assert_cmpuint(len, >=, 28);
    flags = ldl_le_p(frame + 20);
    status = ldl_le_p(frame + 24);

    g_assert_cmpuint(frame[5] & 0xf, ==, 0);
    g_assert_cmpuint(len, ==, 28 + ldl_le_p(frame + 16));
    g_assert_cmpuint(len, >=, 28 + output_len);
    g_assert_cmpuint(ldl_le_p(frame + 12), ==, cmd);
    g_assert_cmpuint(flags >> 16, ==, id);
    g_assert_cmpuint(flags & 1, ==, status < 0);
    if (output_len) {
        memcpy(output, frame + 28, output_len);
    }
    acknowledge(f);
    return status;
}

static int32_t control(APFixture *f, unsigned cmd, bool set,
                       const void *input, size_t input_len,
                       uint8_t *output, size_t output_len)
{
    uint16_t id = send_control(f, cmd, set, 0, input, input_len, output_len);

    return receive_control(f, cmd, id, output, output_len);
}

static int32_t set_u32(APFixture *f, unsigned cmd, uint32_t value)
{
    uint8_t data[4];

    stl_le_p(data, value);
    return control(f, cmd, true, data, sizeof(data), NULL, 0);
}

static uint32_t get_u32(APFixture *f, unsigned cmd)
{
    uint8_t data[4];

    g_assert_cmpint(control(f, cmd, false, NULL, 0, data, sizeof(data)), ==, 0);
    return ldl_le_p(data);
}

static int32_t iovar(APFixture *f, const char *name, bool set,
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

static int32_t set_iovar_u32(APFixture *f, const char *name, uint32_t value)
{
    uint8_t data[4];

    stl_le_p(data, value);
    return iovar(f, name, true, data, sizeof(data), NULL, 0);
}

static uint32_t get_iovar_u32(APFixture *f, const char *name)
{
    uint8_t data[4];

    g_assert_cmpint(iovar(f, name, false, NULL, 0, data, sizeof(data)), ==, 0);
    return ldl_le_p(data);
}

static void assert_identity(APFixture *f)
{
    g_autoptr(QDict) response = qtest_qmp(
        f->qts, "{ 'execute': 'qom-get', 'arguments': { "
        "'path': '/machine/peripheral/wifi-fixture', "
        "'property': 'hardware-profile' } }");
    uint16_t device = f->profile ? 0x4345 : 0xa9a6;
    uint8_t revision = f->profile ? 6 : 9;
    uint8_t revinfo[68];

    g_assert_cmpuint(qdict_get_int(response, "return"), ==, f->profile);
    for (unsigned function = 0; function <= 3; function++) {
        uint32_t fbr = function * 0x100;
        uint32_t cis = cmd52(f, false, 0, fbr + 9, 0) |
                       (cmd52(f, false, 0, fbr + 10, 0) << 8) |
                       (cmd52(f, false, 0, fbr + 11, 0) << 16);
        uint8_t expected[] = { 0x20, 4, 0xd0, 2, device, device >> 8 };

        g_assert_cmphex(cis, ==, 0x1000 + function * 0x100);
        for (size_t i = 0; i < sizeof(expected); i++) {
            g_assert_cmphex(cmd52(f, false, 0, cis + i, 0), ==, expected[i]);
        }
    }
    g_assert_cmphex(backplane(f, true, CHIP_ID, 0), ==,
                    0x15204345 | (revision << 16));
    g_assert_cmpint(control(f, C_GET_REVINFO, false, NULL, 0,
                            revinfo, sizeof(revinfo)), ==, 0);
    g_assert_cmphex(ldl_le_p(revinfo + 4), ==, device);
    g_assert_cmpuint(ldl_le_p(revinfo + 12), ==, revision);
    g_assert_cmpuint(ldl_le_p(revinfo + 44), ==, 4345);
}

static void fixture_init(APFixture *f, unsigned profile, bool network,
                          const char *extra)
{
    g_autofree char *net = NULL;

    memset(f, 0, sizeof(*f));
    f->profile = profile;
    f->peer_fd = -1;
    if (network) {
#ifndef _WIN32
        int pair[2];

        g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), ==, 0);
        /* QEMU must inherit only its own backend end, not our observer end. */
        g_assert_cmpint(fcntl(pair[0], F_SETFD, FD_CLOEXEC), ==, 0);
        net = g_strdup_printf("-netdev socket,id=peer,fd=%d", pair[1]);
        f->qts = qtest_initf(
            "-machine raspi5b -m 1G -serial null -nic none %s "
            "-device cyw43455-sdio,id=wifi-fixture,bus=sd-bus,netdev=peer,"
            "mac=02:00:00:00:00:01,hardware-profile=%u %s", net, profile,
            extra);
        close(pair[1]);
        f->peer_fd = pair[0];
#else
        g_assert_not_reached();
#endif
    } else {
        f->qts = qtest_initf(
            "-machine raspi5b -m 1G -serial null -nic none "
            "-device cyw43455-sdio,id=wifi-fixture,bus=sd-bus,"
            "mac=02:00:00:00:00:01,hardware-profile=%u %s", profile, extra);
    }
    if (!strstr(extra, "-incoming")) {
        select_endpoint(f);
        assert_identity(f);
    }
}

static void fixture_quit(APFixture *f)
{
    qtest_quit(f->qts);
#ifndef _WIN32
    if (f->peer_fd >= 0) {
        close(f->peer_fd);
    }
#endif
}

static void assert_ssid(APFixture *f, const void *ssid, size_t len)
{
    uint8_t data[36];

    g_assert_cmpint(control(f, C_GET_SSID, false, NULL, 0,
                            data, sizeof(data)), ==, 0);
    g_assert_cmpuint(ldl_le_p(data), ==, len);
    if (len) {
        g_assert_cmpmem(data + 4, len, ssid, len);
    }
    for (size_t i = 4 + len; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0);
    }
}

static void assert_peers(APFixture *f, unsigned count)
{
    uint8_t input[4], output[12];

    stl_le_p(input, 1); /* Caller supplies the list's capacity. */
    g_assert_cmpint(control(f, C_GET_ASSOCLIST, false,
                            input, sizeof(input), output, sizeof(output)),
                    ==, 0);
    g_assert_cmpuint(ldl_le_p(output), ==, count);
    if (count) {
        g_assert_cmpmem(output + 4, sizeof(peer_mac),
                        peer_mac, sizeof(peer_mac));
    } else {
        for (size_t i = 4; i < sizeof(output); i++) {
            g_assert_cmphex(output[i], ==, 0);
        }
    }
}

static void assert_local_bss(APFixture *f, const void *ssid, size_t ssid_len)
{
    uint8_t input[4], output[176];
    const uint8_t *bss = output + 4;
    const uint8_t *ie = bss + 128;

    stl_le_p(input, sizeof(output));
    g_assert_cmpint(control(f, C_GET_BSS_INFO, false,
                            input, sizeof(input), output, sizeof(output)),
                    ==, 0);
    g_assert_cmpuint(ldl_le_p(output), ==, sizeof(output));
    g_assert_cmpuint(ldl_le_p(bss), ==, 109);
    g_assert_cmpuint(ldl_le_p(bss + 4), ==, 128 + ssid_len + 7);
    g_assert_cmpmem(bss + 8, 6, ap_mac, 6);
    g_assert_cmpuint(lduw_le_p(bss + 14), ==, 100);
    g_assert_cmpuint(bss[18], ==, ssid_len);
    g_assert_cmpmem(bss + 19, ssid_len, ssid, ssid_len);
    g_assert_cmpuint(ldl_le_p(bss + 52), ==, 3);
    g_assert_cmpmem(bss + 56, 3, "\x8c\x98\xb0", 3);
    g_assert_cmphex(lduw_le_p(bss + 72), ==, CH36_20);
    g_assert_cmpuint(bss[76], ==, 1);
    g_assert_cmpuint(bss[81], ==, 0); /* No synthesized HT capability. */
    g_assert_cmpuint(bss[88], ==, 36);
    g_assert_cmpuint(lduw_le_p(bss + 116), ==, 128);
    g_assert_cmpuint(ldl_le_p(bss + 120), ==, ssid_len + 7);
    g_assert_cmpuint(ie[0], ==, 0);
    g_assert_cmpuint(ie[1], ==, ssid_len);
    g_assert_cmpmem(ie + 2, ssid_len, ssid, ssid_len);
    g_assert_cmpmem(ie + 2 + ssid_len, 5, "\x01\x03\x8c\x98\xb0", 5);
    /* No DS-channel IE on 5 GHz, and no padding masquerading as another IE. */
    for (size_t i = 4 + 128 + ssid_len + 7; i < sizeof(output); i++) {
        g_assert_cmphex(output[i], ==, 0);
    }
    stl_le_p(input, 4 + 128 + ssid_len + 6); /* One byte too short. */
    g_assert_cmpint(control(f, C_GET_BSS_INFO, false,
                            input, sizeof(input), output, sizeof(output)),
                    <, 0);
}

static void configure_ap(APFixture *f)
{
    g_assert_cmpint(control(f, C_DOWN, true, NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(set_iovar_u32(f, "apsta", 0), ==, 0);
    g_assert_cmpint(set_u32(f, C_SET_INFRA, 1), ==, 0);
    g_assert_cmpint(set_u32(f, C_SET_AP, 1), ==, 0);
    g_assert_cmpuint(get_u32(f, C_GET_AP), ==, 1);
    g_assert_cmpuint(get_u32(f, C_GET_INFRA), ==, 1);
    g_assert_cmpint(set_iovar_u32(f, "chanspec", CH36_20), ==, 0);
    g_assert_cmpint(control(f, C_UP, true, NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(set_iovar_u32(f, "auth", 0), ==, 0);
    g_assert_cmpint(set_iovar_u32(f, "wsec", 0), ==, 0);
    g_assert_cmpint(set_iovar_u32(f, "wpa_auth", 0), ==, 0);
    g_assert_cmpint(set_u32(f, C_SET_REGULATORY, 0), ==, 0);
    g_assert_cmpuint(get_u32(f, C_GET_REGULATORY), ==, 0);
    g_assert_cmpint(set_u32(f, C_SET_BCNPRD, 100), ==, 0);
    g_assert_cmpint(set_u32(f, C_SET_DTIMPRD, 1), ==, 0);
    assert_ssid(f, NULL, 0);
    assert_peers(f, 0);
    g_assert_false(frame_pending(f));
}

static uint16_t start_ap_send(APFixture *f, const void *ssid, size_t len)
{
    uint8_t join[JOIN_LEN] = { };

    g_assert_cmpuint(len, <=, 32);
    stl_le_p(join, len);
    memcpy(join + 4, ssid, len);
    return send_control(f, C_SET_SSID, true, 0, join, sizeof(join), 0);
}

static void start_ap(APFixture *f, const void *ssid, size_t len)
{
    uint16_t id = start_ap_send(f, ssid, len);

    g_assert_cmpint(receive_control(f, C_SET_SSID, id, NULL, 0), ==, 0);
}

static void assert_event(APFixture *f, unsigned type, uint16_t flags,
                          const uint8_t *address, const void *ssid,
                          size_t ssid_len)
{
    uint8_t frame[FRAME_MAX];
    size_t len = read_frame(f, frame);
    const uint8_t *event = frame + EVENT_MSG;
    size_t data_len = ssid_len ? 2 + ssid_len + 5 : 0;

    g_assert_cmpuint(len, ==, EVENT_BASE + data_len);
    g_assert_cmpuint(frame[5] & 0xf, ==, 1);
    g_assert_cmphex(frame[12], ==, 0x20); /* BCDC v2. */
    g_assert_cmpmem(frame + 16, sizeof(ap_mac), ap_mac, sizeof(ap_mac));
    g_assert_cmpmem(frame + 22, sizeof(peer_mac), peer_mac, sizeof(peer_mac));
    g_assert_cmphex(lduw_be_p(frame + 28), ==, 0x886c);
    g_assert_cmphex(lduw_be_p(frame + 30), ==, 0x8001);
    g_assert_cmpuint(lduw_be_p(frame + 32), ==, 48 + data_len);
    g_assert_cmpmem(frame + 35, 3, "\x00\x10\x18", 3);
    g_assert_cmpuint(lduw_be_p(frame + 38), ==, 1);
    g_assert_cmpuint(lduw_be_p(event), ==, 2);
    g_assert_cmphex(lduw_be_p(event + 2), ==, flags);
    g_assert_cmpuint(ldl_be_p(event + 4), ==, type);
    g_assert_cmpuint(ldl_be_p(event + 8), ==, 0); /* Success. */
    g_assert_cmpuint(ldl_be_p(event + 12), ==, 0); /* Reason. */
    g_assert_cmpuint(ldl_be_p(event + 20), ==, data_len);
    g_assert_cmpmem(event + 24, 6, address, 6);
    g_assert_cmpmem(event + 30, 6, "wlan0", 6);
    g_assert_cmpuint(event[46], ==, 0); /* ifidx */
    g_assert_cmpuint(event[47], ==, 0); /* bsscfgidx */
    if (ssid_len) {
        const uint8_t *ie = event + 48;

        g_assert_cmpuint(ie[0], ==, 0);
        g_assert_cmpuint(ie[1], ==, ssid_len);
        g_assert_cmpmem(ie + 2, ssid_len, ssid, ssid_len);
        g_assert_cmpmem(ie + 2 + ssid_len, 5, "\x01\x03\x8c\x98\xb0", 5);
    }
    acknowledge(f);
}

static void drain_started(APFixture *f, const void *ssid, size_t len)
{
    assert_event(f, LINK_EVENT, 1, ap_mac, NULL, 0);
    assert_event(f, ASSOC_IND, 0, peer_mac, ssid, len);
    /* Readback also retires any credit update from a pre-association TX. */
    assert_peers(f, 1);
    assert_ssid(f, ssid, len);
    assert_local_bss(f, ssid, len);
    g_assert_false(frame_pending(f));
}

static void drain_stopped(APFixture *f)
{
    assert_event(f, DISASSOC_IND, 0, peer_mac, NULL, 0);
    assert_event(f, LINK_EVENT, 0, ap_mac, NULL, 0);
    g_assert_false(frame_pending(f));
    assert_peers(f, 0);
    assert_ssid(f, NULL, 0);
}

static void assert_station_bss(APFixture *f, bool include_ies)
{
    static const uint8_t station_ies[] = {
        0, 13, 'q', 'e', 'm', 'u', '-', 'c', 'y', 'w', '4', '3', '4', '5', '5',
        1, 4, 0x82, 0x84, 0x8b, 0x96, 3, 1, 1,
    };
    uint8_t input[4], output[176], expected[176] = { };
    uint8_t *bss = expected + 4;
    size_t bss_len = include_ies ? 152 : 128;
    size_t minimum = 4 + bss_len;
    const size_t capacities[] = { minimum, sizeof(output) };

    /* Independent byte oracle: remote station, never the former local AP. */
    g_assert_cmpuint(sizeof(station_ies), ==, 24);
    stl_le_p(bss, 109);
    stl_le_p(bss + 4, bss_len);
    memcpy(bss + 8, peer_mac, sizeof(peer_mac));
    stw_le_p(bss + 14, 100);
    stw_le_p(bss + 16, 0x421); /* Open ESS, short preamble/slot; no privacy. */
    bss[18] = 13;
    memcpy(bss + 19, "qemu-cyw43455", 13);
    stl_le_p(bss + 52, 1);
    bss[56] = 144; /* Preserve the legacy station rate field byte-for-byte. */
    stw_le_p(bss + 72, 0x1001); /* Remote BSS: 2.4 GHz channel1/20MHz. */
    bss[76] = 1;
    stw_le_p(bss + 78, (uint16_t)-28);
    bss[80] = (uint8_t)-95;
    bss[81] = 1; /* Historical station HLE field, not new HT negotiation. */
    bss[88] = 1;
    stw_le_p(bss + 116, 128);
    stl_le_p(bss + 120, include_ies ? sizeof(station_ies) : 0);
    stw_le_p(bss + 124, 67);
    if (include_ies) {
        memcpy(bss + 128, station_ies, sizeof(station_ies));
    }
    for (size_t i = 0; i < G_N_ELEMENTS(capacities); i++) {
        stl_le_p(input, capacities[i]);
        stl_le_p(expected, capacities[i]);
        memset(output, 0xa5, sizeof(output));
        g_assert_cmpint(control(f, C_GET_BSS_INFO, false, input, sizeof(input),
                                output, sizeof(output)), ==, 0);
        /* Includes every reserved byte and zero tail, not only IE presence. */
        g_assert_cmpmem(output, sizeof(output), expected, sizeof(expected));
    }
    stl_le_p(input, minimum - 1);
    memset(output, 0xa5, sizeof(output));
    g_assert_cmpint(control(f, C_GET_BSS_INFO, false, input, sizeof(input),
                            output, sizeof(output)), ==, -24); /* BADLEN */
    for (size_t i = 0; i < sizeof(output); i++) {
        g_assert_cmphex(output[i], ==, 0);
    }
    /* A sufficient caller prefix cannot overrun a155-byte explicit reply. */
    stl_le_p(input, minimum);
    memset(output, 0xa5, sizeof(output));
    g_assert_cmpint(control(f, C_GET_BSS_INFO, false, input, sizeof(input),
                            output, minimum - 1), ==, -24);
    for (size_t i = 0; i < sizeof(output); i++) {
        g_assert_cmphex(output[i], ==, i < minimum - 1 ? 0 : 0xa5);
    }
}

static void test_station_bss_after_ap(gconstpointer opaque)
{
    APFixture f;
    uint8_t input[4], output[176];

    fixture_init(&f, GPOINTER_TO_UINT(opaque), false, "");
    /* Untouched compatibility association retains its128-byte/no-IE reply. */
    g_assert_cmpint(control(&f, C_UP, true, NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(set_u32(&f, C_SET_PROMISC, 1), ==, 0);
    assert_event(&f, 0, 1, peer_mac, NULL, 0); /* Legacy SET_SSID only. */
    g_assert_false(frame_pending(&f));
    assert_station_bss(&f, false);
    g_assert_cmpint(control(&f, C_DOWN, true, NULL, 0, NULL, 0), ==, 0);
    assert_event(&f, LINK_EVENT, 0, peer_mac, NULL, 0);
    configure_ap(&f);
    start_ap(&f, ap_ssid, strlen(ap_ssid));
    drain_started(&f, ap_ssid, strlen(ap_ssid));
    g_assert_cmpint(set_u32(&f, C_SET_AP, 0), ==, 0);
    drain_stopped(&f);
    g_assert_cmpint(control(&f, C_UP, true, NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(set_u32(&f, C_SET_PROMISC, 1), ==, 0);
    g_assert_false(frame_pending(&f)); /* No compatibility fallback after AP. */
    stl_le_p(input, sizeof(output));
    memset(output, 0xa5, sizeof(output));
    g_assert_cmpint(control(&f, C_GET_BSS_INFO, false, input, sizeof(input),
                            output, sizeof(output)), ==, -28); /* NOTASSOC */
    for (size_t i = 0; i < sizeof(output); i++) {
        g_assert_cmphex(output[i], ==, 0); /* No stale local AP BSS/SSID. */
    }
    assert_ssid(&f, NULL, 0);
    /* Only a new explicit station join authorizes the remote BSS response. */
    start_ap(&f, "qemu-cyw43455", 13);
    assert_event(&f, LINK_EVENT, 1, peer_mac, NULL, 0);
    assert_event(&f, 0, 1, peer_mac, NULL, 0);
    assert_ssid(&f, "qemu-cyw43455", 13);
    g_assert_cmphex(get_iovar_u32(&f, "chanspec"), ==, 0x1001);
    assert_station_bss(&f, true);
    g_assert_false(frame_pending(&f));
    fixture_quit(&f);
}

static void test_configuration(gconstpointer opaque)
{
    APFixture f;
    uint8_t data[136], input[4];
    unsigned profile = GPOINTER_TO_UINT(opaque);

    fixture_init(&f, profile, false, "");
    g_assert_cmpuint(get_u32(&f, C_GET_AP), ==, 0);
    g_assert_cmpint(set_iovar_u32(&f, "mfp", 2), ==, 0);
    g_assert_cmpuint(get_iovar_u32(&f, "mfp"), ==, 2);
    configure_ap(&f);
    g_assert_cmpuint(get_iovar_u32(&f, "mfp"), ==, 0);
    g_assert_cmpuint(get_iovar_u32(&f, "chanspec"), ==, CH36_20);
    /* The modeled 20/40 MHz chanspecs require brcmfmac HT support. */
    g_assert_cmpuint(get_iovar_u32(&f, "nmode"), ==, 1);
    g_assert_cmpuint(get_iovar_u32(&f, "vhtmode"), ==, 0);
    g_assert_cmpuint(get_iovar_u32(&f, "mimo_bw_cap"), ==, 0);
    g_assert_cmpint(control(&f, C_GET_BANDLIST, false, NULL, 0,
                            data, 12), ==, 0);
    g_assert_cmpuint(ldl_le_p(data), ==, 2);
    g_assert_cmpuint(ldl_le_p(data + 4), ==, 2); /* 2.4 GHz */
    g_assert_cmpuint(ldl_le_p(data + 8), ==, 1); /* 5 GHz */
    stl_le_p(input, 1); /* 5 GHz */
    g_assert_cmpint(iovar(&f, "bw_cap", false, input, 4, data, 4), ==, 0);
    g_assert_cmphex(ldl_le_p(data), ==, 1); /* Only 20 MHz. */
    stw_le_p(input, 0xd000); /* 5 GHz / 20 MHz list, no channel. */
    g_assert_cmpint(iovar(&f, "chanspecs", false, input, 2, data, 8), ==, 0);
    g_assert_cmpuint(ldl_le_p(data), ==, 1);
    g_assert_cmphex(ldl_le_p(data + 4), ==, CH36_20);
    stw_le_p(input, 0);
    g_assert_cmpint(iovar(&f, "chanspecs", false, input, 2,
                          data, sizeof(data)), ==, 0);
    g_assert_cmpuint(ldl_le_p(data), ==, 32);
    unsigned five_ghz = 0;
    for (unsigned i = 0; i < 32; i++) {
        uint32_t spec = ldl_le_p(data + 4 + 4 * i);

        if (spec & 0xc000) {
            g_assert_cmphex(spec, ==, CH36_20);
            five_ghz++;
        }
    }
    g_assert_cmpuint(five_ghz, ==, 1);
    g_assert_cmpint(set_iovar_u32(&f, "closednet", 1), ==, 0);
    g_assert_cmpuint(get_iovar_u32(&f, "closednet"), ==, 1);
    g_assert_cmpint(set_iovar_u32(&f, "closednet", 0), ==, 0);
    g_assert_cmpuint(get_iovar_u32(&f, "closednet"), ==, 0);
    fixture_quit(&f);
}

static void test_negative_configuration(gconstpointer opaque)
{
    APFixture f;
    uint8_t join[JOIN_LEN] = { }, data[12] = { };
    uint16_t id;
    const char *unsupported[] = { "apsta", "mbss", "p2p_disc", "p2p",
                                 "tlv", "wlfc_mode", "ampdu_hostreorder" };
    const uint32_t invalid_channels[] = { 0xd026, 0xd034, 0xd826, 0xe02a,
                                         0x1803 };

    fixture_init(&f, GPOINTER_TO_UINT(opaque), false, "");
    /* UP, even without association, is not a valid AP mode-change state. */
    g_assert_cmpint(control(&f, C_UP, true, NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(set_u32(&f, C_SET_AP, 1), <, 0);
    g_assert_cmpuint(get_u32(&f, C_GET_AP), ==, 0);
    g_assert_cmpint(control(&f, C_DOWN, true, NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(set_u32(&f, C_SET_INFRA, 1), ==, 0);
    g_assert_cmpint(set_u32(&f, C_SET_AP, 1), ==, 0);
    g_assert_cmpint(control(&f, C_UP, true, NULL, 0, NULL, 0), <, 0);
    stl_le_p(join, 1);
    join[4] = 'x';
    g_assert_cmpint(control(&f, C_SET_SSID, true, join, sizeof(join),
                            NULL, 0), <, 0); /* No channel and still DOWN. */
    g_assert_cmpint(set_iovar_u32(&f, "chanspec", CH36_20), ==, 0);
    g_assert_cmpint(control(&f, C_SET_SSID, true, join, sizeof(join),
                            NULL, 0), <, 0); /* Channel set, but still DOWN. */
    configure_ap(&f);
    for (size_t i = 0; i < G_N_ELEMENTS(unsupported); i++) {
        g_assert_cmpint(set_iovar_u32(&f, unsupported[i], 1), <, 0);
    }
    g_assert_cmpint(set_iovar_u32(&f, "wpa_auth", 4), <, 0);
    g_assert_cmpint(set_iovar_u32(&f, "wsec", 4), <, 0);
    g_assert_cmpint(set_iovar_u32(&f, "auth", 1), <, 0);
    g_assert_cmpint(set_iovar_u32(&f, "mfp", 2), <, 0);
    g_assert_cmpuint(get_iovar_u32(&f, "mfp"), ==, 0);
    /* Nonempty security IE must not ACK as an open network. */
    g_assert_cmpint(iovar(&f, "wpaie", true, "\xdd\x02\x00\x00", 4,
                          NULL, 0), <, 0);
    for (size_t i = 0; i < G_N_ELEMENTS(invalid_channels); i++) {
        g_assert_cmpint(set_iovar_u32(&f, "chanspec", invalid_channels[i]),
                        <, 0);
        g_assert_cmpuint(get_iovar_u32(&f, "chanspec"), ==, CH36_20);
    }
    stl_le_p(data, 1); /* 5 GHz / 40 MHz is unsupported. */
    stl_le_p(data + 4, 3);
    g_assert_cmpint(iovar(&f, "bw_cap", true, data, 8, NULL, 0), <, 0);
    g_assert_cmpint(set_iovar_u32(&f, "closednet", 2), <, 0);
    g_assert_cmpint(set_u32(&f, C_SET_BCNPRD, 0), <, 0);
    g_assert_cmpint(set_u32(&f, C_SET_BCNPRD, 65536), <, 0);
    g_assert_cmpint(set_u32(&f, C_SET_DTIMPRD, 0), <, 0);
    g_assert_cmpint(set_u32(&f, C_SET_DTIMPRD, 256), <, 0);
    g_assert_cmpuint(get_u32(&f, C_GET_BCNPRD), ==, 100);
    g_assert_cmpuint(get_u32(&f, C_GET_DTIMPRD), ==, 1);
    g_assert_cmpint(set_u32(&f, C_SET_AP, 2), <, 0);
    g_assert_cmpint(set_u32(&f, C_SET_INFRA, 0), <, 0);
    g_assert_cmpint(control(&f, C_SET_AP, true, data, 3, NULL, 0), <, 0);
    stl_le_p(join, 1);
    join[4] = 'x';
    g_assert_cmpint(control(&f, C_SET_SSID, true, join, 36, NULL, 0), <, 0);
    join[36] = 1; /* Forbidden assoc channel/BSSID suffix. */
    g_assert_cmpint(control(&f, C_SET_SSID, true, join, sizeof(join),
                            NULL, 0), <, 0);
    join[36] = 0;
    stl_le_p(join, 33);
    g_assert_cmpint(control(&f, C_SET_SSID, true, join, sizeof(join),
                            NULL, 0), <, 0);
    stl_le_p(join, 1);
    id = send_control(&f, C_SET_SSID, true, 1, join, sizeof(join), 0);
    g_assert_cmpint(receive_control(&f, C_SET_SSID, id, NULL, 0), <, 0);
    /* bsscfg-prefixed forms must not create a second BSS. */
    stl_le_p(data, 1);
    stl_le_p(data + 4, 1);
    g_assert_cmpint(iovar(&f, "bsscfg:ap", true, data, 8, NULL, 0), <, 0);
    g_assert_cmpuint(get_u32(&f, C_GET_AP), ==, 1);
    assert_ssid(&f, NULL, 0);
    assert_peers(&f, 0);
    g_assert_false(frame_pending(&f));
    fixture_quit(&f);
}

static void test_ssid_boundaries(gconstpointer opaque)
{
    APFixture f;
    uint8_t ssid[32];

    fixture_init(&f, GPOINTER_TO_UINT(opaque), false, "");
    configure_ap(&f);
    /* All 32 bytes are significant; not C-string terminated. */
    for (size_t i = 0; i < sizeof(ssid); i++) {
        ssid[i] = 0x80 + i;
    }
    ssid[7] = 0; /* Embedded NUL, not an early C-string terminator. */
    start_ap(&f, ssid, sizeof(ssid));
    drain_started(&f, ssid, sizeof(ssid));
    start_ap(&f, ssid, 0);
    drain_stopped(&f);
    g_assert_cmpuint(get_u32(&f, C_GET_AP), ==, 1);
    start_ap(&f, "z", 1);
    drain_started(&f, "z", 1);
    fixture_quit(&f);
}

static void test_pending_reset(gconstpointer opaque)
{
    APFixture f;
    uint8_t frame[4] = { 0xff, 0xff, 0xff, 0xff };

    fixture_init(&f, GPOINTER_TO_UINT(opaque), false, "");
    configure_ap(&f);
    start_ap(&f, ap_ssid, strlen(ap_ssid));
    /* The LINK event is unread and ASSOC_IND is still pending. */
    g_assert_true(frame_pending(&f));
    cmd52(&f, true, 0, ABORT, 8);
    select_endpoint(&f);
    g_assert_false(frame_pending(&f));
    g_assert_cmphex(cmd52(&f, false, 0, INTPEND, 0), ==, 0);
    pio(&f, true, 2, 0, false, frame, sizeof(frame));
    g_assert_cmphex(ldl_le_p(frame), ==, 0);
    g_assert_cmpuint(get_u32(&f, C_GET_AP), ==, 0);
    assert_ssid(&f, NULL, 0);
    assert_peers(&f, 0);
    assert_identity(&f);
    configure_ap(&f);
    start_ap(&f, ap_ssid, strlen(ap_ssid));
    drain_started(&f, ap_ssid, strlen(ap_ssid));
    fixture_quit(&f);
}

static void test_stop_pending_events(gconstpointer opaque)
{
    APFixture f;
    uint16_t start_id, stop_id;

    fixture_init(&f, GPOINTER_TO_UINT(opaque), false, "");
    configure_ap(&f);
    /* Stop before either AP event is serialized: CDC replies have priority. */
    start_id = start_ap_send(&f, ap_ssid, strlen(ap_ssid));
    stop_id = send_control(&f, C_DOWN, true, 0, NULL, 0, 0);
    g_assert_cmpint(receive_control(&f, C_SET_SSID, start_id, NULL, 0), ==, 0);
    g_assert_cmpint(receive_control(&f, C_DOWN, stop_id, NULL, 0), ==, 0);
    assert_event(&f, LINK_EVENT, 0, ap_mac, NULL, 0);
    g_assert_false(frame_pending(&f));
    assert_peers(&f, 0);
    assert_ssid(&f, NULL, 0);
    configure_ap(&f);
    /* Already-serialized LINK survives, but queued ASSOC must be cancelled. */
    start_ap(&f, ap_ssid, strlen(ap_ssid));
    stop_id = send_control(&f, C_DOWN, true, 0, NULL, 0, 0);
    assert_event(&f, LINK_EVENT, 1, ap_mac, NULL, 0);
    g_assert_cmpint(receive_control(&f, C_DOWN, stop_id, NULL, 0), ==, 0);
    assert_event(&f, LINK_EVENT, 0, ap_mac, NULL, 0);
    g_assert_false(frame_pending(&f));
    assert_peers(&f, 0);
    assert_ssid(&f, NULL, 0);
    fixture_quit(&f);
}

#ifndef _WIN32
static void socket_io(int fd, bool read, void *data, size_t length)
{
    uint8_t *p = data;
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

static void make_ethernet(uint8_t packet[ETH_LEN], const uint8_t *dest,
                           const uint8_t *source, uint8_t cookie)
{
    memcpy(packet, dest, 6);
    memcpy(packet + 6, source, 6);
    stw_be_p(packet + 12, 0x88b5); /* Experimental EtherType: no ARP offload. */
    for (size_t i = 14; i < ETH_LEN; i++) {
        packet[i] = cookie + i;
    }
}

static void send_data(APFixture *f, const uint8_t packet[ETH_LEN])
{
    uint8_t frame[16 + ETH_LEN] = { };

    stw_le_p(frame, sizeof(frame));
    stw_le_p(frame + 2, (uint16_t)~sizeof(frame));
    frame[4] = ++f->sequence;
    frame[5] = 2;
    frame[7] = 12;
    frame[12] = 0x20;
    memcpy(frame + 16, packet, ETH_LEN);
    pio(f, false, 2, 0, false, frame, sizeof(frame));
}

static void send_invalid_data(APFixture *f, const uint8_t packet[ETH_LEN],
                               bool nonzero_ifidx)
{
    uint8_t frame[20 + ETH_LEN] = { };
    size_t len = sizeof(frame) - (nonzero_ifidx ? 4 : 0);
    size_t ethernet_offset = nonzero_ifidx ? 16 : 20;

    stw_le_p(frame, len);
    stw_le_p(frame + 2, (uint16_t)~len);
    frame[4] = ++f->sequence;
    frame[5] = 2;
    frame[7] = 12;
    frame[12] = 0x20;
    frame[14] = nonzero_ifidx ? 1 : 0;
    frame[15] = nonzero_ifidx ? 0 : 1; /* Four bytes of FWS signal/TLV. */
    memcpy(frame + ethernet_offset, packet, ETH_LEN);
    pio(f, false, 2, 0, false, frame, len);
}

static void expect_backend(APFixture *f, const uint8_t packet[ETH_LEN])
{
    uint8_t actual[ETH_LEN];
    uint32_t length;

    socket_io(f->peer_fd, true, &length, sizeof(length));
    g_assert_cmpuint(ntohl(length), ==, ETH_LEN);
    socket_io(f->peer_fd, true, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), packet, ETH_LEN);
}

static void assert_backend_empty(APFixture *f)
{
    struct pollfd pollfd = { .fd = f->peer_fd, .events = POLLIN };

    /* This checks the actual backend, not a synthetic packet counter. */
    g_assert_cmpint(poll(&pollfd, 1, 20), ==, 0);
}

static void inject_backend(APFixture *f, uint8_t packet[ETH_LEN])
{
    uint32_t length = htonl(ETH_LEN);

    socket_io(f->peer_fd, false, &length, sizeof(length));
    socket_io(f->peer_fd, false, packet, ETH_LEN);
}

static void expect_rx_data(APFixture *f, const uint8_t packet[ETH_LEN])
{
    int64_t deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    uint8_t frame[FRAME_MAX];

    while (!frame_pending(f)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    g_assert_cmpuint(read_frame(f, frame), ==, 16 + ETH_LEN);
    g_assert_cmpuint(frame[5] & 0xf, ==, 2);
    g_assert_cmphex(frame[12], ==, 0x20);
    g_assert_cmpmem(frame + 16, ETH_LEN, packet, ETH_LEN);
    acknowledge(f);
}

static void assert_no_rx_data(APFixture *f)
{
    int64_t deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_MILLISECOND;

    do {
        g_assert_false(frame_pending(f));
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
}

static void hold_pending_event(APFixture *f)
{
    int64_t deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_MILLISECOND;

    /* Let backend input run while the unread LINK still blocks association. */
    do {
        g_assert_true(frame_pending(f));
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
}

static void clear_credit(APFixture *f)
{
    /* Control replies retire credits; do not confuse a header-only update. */
    g_assert_cmpuint(get_u32(f, C_GET_AP), ==, 1);
    g_assert_false(frame_pending(f));
}

static void test_peer_data(gconstpointer opaque)
{
    APFixture f;
    uint8_t packet[ETH_LEN];

    fixture_init(&f, GPOINTER_TO_UINT(opaque), true, "");
    configure_ap(&f);
    make_ethernet(packet, peer_mac, ap_mac, 0x10);
    send_data(&f, packet);
    assert_backend_empty(&f); /* Configured+UP but not started. */
    clear_credit(&f);
    start_ap(&f, ap_ssid, strlen(ap_ssid));
    send_data(&f, packet);
    assert_backend_empty(&f); /* LINK pending, peer not yet associated. */
    make_ethernet(packet, ap_mac, peer_mac, 0x11);
    inject_backend(&f, packet); /* Must be dropped, not replayed after ASSOC. */
    hold_pending_event(&f);
    drain_started(&f, ap_ssid, strlen(ap_ssid));
    assert_no_rx_data(&f);
    make_ethernet(packet, peer_mac, ap_mac, 0x10);
    send_data(&f, packet);
    expect_backend(&f, packet);
    assert_backend_empty(&f); /* Exactly one forward. */
    clear_credit(&f);
    send_invalid_data(&f, packet, true);
    assert_backend_empty(&f); /* No second data-interface/BSS is modeled. */
    clear_credit(&f);
    send_invalid_data(&f, packet, false);
    assert_backend_empty(&f); /* SDPCM credits are not proprietary FWS. */
    clear_credit(&f);
    make_ethernet(packet, (const uint8_t *)"\xff\xff\xff\xff\xff\xff",
                  ap_mac, 0x18);
    send_data(&f, packet);
    expect_backend(&f, packet);
    clear_credit(&f);
    make_ethernet(packet, other_mac, ap_mac, 0x20);
    send_data(&f, packet);
    assert_backend_empty(&f); /* Unassociated unicast destination. */
    clear_credit(&f);
    make_ethernet(packet, ap_mac, other_mac, 0x30);
    inject_backend(&f, packet);
    assert_no_rx_data(&f); /* Unassociated source may not enter AP RX. */
    make_ethernet(packet, ap_mac, peer_mac, 0x40);
    inject_backend(&f, packet);
    expect_rx_data(&f, packet);
    assert_no_rx_data(&f);
    g_assert_cmpint(set_iovar_u32(&f, "closednet", 1), ==, 0);
    g_assert_cmpuint(get_iovar_u32(&f, "closednet"), ==, 1);
    assert_ssid(&f, ap_ssid, strlen(ap_ssid));
    assert_peers(&f, 1); /* Hidden SSID is not a second association/start. */
    make_ethernet(packet, peer_mac, ap_mac, 0x50);
    send_data(&f, packet);
    expect_backend(&f, packet);
    clear_credit(&f);
    fixture_quit(&f);
}

static void test_stop_no_station_fallback(gconstpointer opaque)
{
    APFixture f;
    uint8_t packet[ETH_LEN];

    fixture_init(&f, GPOINTER_TO_UINT(opaque), true, "");
    configure_ap(&f);
    start_ap(&f, ap_ssid, strlen(ap_ssid));
    drain_started(&f, ap_ssid, strlen(ap_ssid));
    g_assert_cmpint(set_u32(&f, C_SET_AP, 0), ==, 0);
    drain_stopped(&f);
    g_assert_cmpuint(get_u32(&f, C_GET_AP), ==, 0);
    g_assert_cmpint(control(&f, C_UP, true, NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(set_u32(&f, C_SET_INFRA, 1), ==, 0);
    g_assert_cmpuint(get_u32(&f, C_GET_INFRA), ==, 1);
    g_assert_false(frame_pending(&f));
    make_ethernet(packet, peer_mac, ap_mac, 0x60);
    send_data(&f, packet);
    assert_backend_empty(&f);
    /* A control response prevents delayed TX-credit events contaminating RX. */
    g_assert_cmpuint(get_u32(&f, C_GET_AP), ==, 0);
    make_ethernet(packet, ap_mac, peer_mac, 0x70);
    inject_backend(&f, packet);
    assert_no_rx_data(&f);
    /* A fresh explicit station join is the only way out of the stopped role. */
    start_ap(&f, "qemu-cyw43455", strlen("qemu-cyw43455"));
    assert_event(&f, LINK_EVENT, 1, peer_mac, NULL, 0);
    assert_event(&f, 0, 1, peer_mac, NULL, 0); /* SET_SSID station event. */
    make_ethernet(packet, peer_mac, ap_mac, 0x80);
    send_data(&f, packet);
    expect_backend(&f, packet);
    fixture_quit(&f);
}

static void test_down_and_zero_stop(gconstpointer opaque)
{
    APFixture f;
    uint8_t packet[ETH_LEN];

    fixture_init(&f, GPOINTER_TO_UINT(opaque), true, "");
    configure_ap(&f);
    for (unsigned zero_ssid = 0; zero_ssid <= 1; zero_ssid++) {
        start_ap(&f, ap_ssid, strlen(ap_ssid));
        drain_started(&f, ap_ssid, strlen(ap_ssid));
        if (zero_ssid) {
            start_ap(&f, ap_ssid, 0);
        } else {
            g_assert_cmpint(control(&f, C_DOWN, true, NULL, 0, NULL, 0),
                            ==, 0);
        }
        drain_stopped(&f);
        make_ethernet(packet, peer_mac, ap_mac, 0x90 + zero_ssid);
        send_data(&f, packet);
        assert_backend_empty(&f);
        clear_credit(&f);
        g_assert_cmpint(control(&f, C_UP, true, NULL, 0, NULL, 0), ==, 0);
        g_assert_false(frame_pending(&f));
        send_data(&f, packet);
        assert_backend_empty(&f); /* UP alone cannot restart the AP. */
        clear_credit(&f);
    }
    fixture_quit(&f);
}

static void wait_migration(QTestState *qts, bool failure)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    while (g_get_monotonic_time() < deadline) {
        g_autoptr(QDict) response = qtest_qmp(qts,
                                            "{ 'execute': 'query-migrate' }");
        QDict *result = qdict_get_qdict(response, "return");
        const char *status = qdict_get_str(result, "status");

        if (!strcmp(status, "completed")) {
            g_assert_false(failure);
            return;
        }
        if (!strcmp(status, "failed")) {
            g_assert_true(failure);
            g_assert_nonnull(strstr(qdict_get_str(result, "error-desc"),
                                    "cyw43455-sdio"));
            return;
        }
        g_assert_cmpstr(status, !=, "cancelled");
        g_usleep(1000);
    }
    g_error("timed out waiting for synthetic AP migration");
}

static void migrate(APFixture *source, APFixture *destination, bool failure)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("cyw43455-ap-XXXXXX", &error);
    g_autofree char *state = NULL;
    g_autofree char *uri = NULL;

    g_assert_no_error(error);
    g_assert_nonnull(directory);
    state = g_build_filename(directory, "state", NULL);
    uri = g_strdup_printf("file:%s", state);
    qtest_qmp_assert_success(source->qts, "{ 'execute': 'stop' }");
    qtest_qmp_assert_success(source->qts,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    wait_migration(source->qts, false);
    qtest_qmp_assert_success(destination->qts,
        "{ 'execute': 'migrate-incoming', 'arguments': { "
        "'uri': %s, 'exit-on-error': false } }", uri);
    wait_migration(destination->qts, failure);
    g_assert_cmpint(unlink(state), ==, 0);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_pending_ap_migration(gconstpointer opaque)
{
    APFixture source, destination;
    unsigned profile = GPOINTER_TO_UINT(opaque);
    uint16_t id;

    fixture_init(&source, profile, false, "");
    configure_ap(&source);
    /* Migrate unread CDC reply and both queued AP events, not just carrier. */
    id = start_ap_send(&source, ap_ssid, strlen(ap_ssid));
    fixture_init(&destination, profile, false, "-incoming defer");
    migrate(&source, &destination, false);
    /* These host variables represent the same guest's migrated CDC state. */
    destination.request_id = source.request_id;
    destination.sequence = source.sequence;
    g_assert_cmpint(receive_control(&destination, C_SET_SSID, id, NULL, 0),
                    ==, 0);
    drain_started(&destination, ap_ssid, strlen(ap_ssid));
    assert_identity(&destination);
    g_assert_cmpuint(get_u32(&destination, C_GET_AP), ==, 1);
    g_assert_cmpuint(get_u32(&destination, C_GET_INFRA), ==, 1);
    g_assert_cmpuint(get_iovar_u32(&destination, "chanspec"), ==, CH36_20);
    g_assert_cmpint(control(&destination, C_DOWN, true, NULL, 0, NULL, 0),
                    ==, 0);
    drain_stopped(&destination);
    fixture_quit(&source);
    fixture_quit(&destination);
}

static void test_ap_profile_mismatch(gconstpointer opaque)
{
    APFixture source, destination;
    unsigned profile = GPOINTER_TO_UINT(opaque);
    g_autoptr(QDict) response = NULL;

    fixture_init(&source, profile, false, "");
    configure_ap(&source);
    start_ap_send(&source, ap_ssid, strlen(ap_ssid));
    fixture_init(&destination, !profile, false, "-incoming defer");
    migrate(&source, &destination, true);
    response = qtest_qmp(destination.qts,
        "{ 'execute': 'qom-get', 'arguments': { "
        "'path': '/machine/peripheral/wifi-fixture', "
        "'property': 'hardware-profile' } }");
    g_assert_cmpuint(qdict_get_int(response, "return"), ==, !profile);
    fixture_quit(&source);
    fixture_quit(&destination);
}
#endif

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    for (unsigned profile = 0; profile <= 1; profile++) {
        struct {
            const char *name;
            GTestDataFunc function;
        } tests[] = {
            { "configuration-5ghz20", test_configuration },
            { "unsupported-and-malformed", test_negative_configuration },
            { "ssid-boundaries", test_ssid_boundaries },
            { "pending-reset", test_pending_reset },
            { "stop-pending-events", test_stop_pending_events },
            { "station-bss-after-ap", test_station_bss_after_ap },
#ifndef _WIN32
            { "peer-events-and-ethernet", test_peer_data },
            { "stop-no-station-fallback", test_stop_no_station_fallback },
            { "down-and-zero-ssid-stop", test_down_and_zero_stop },
            { "pending-ap-migration", test_pending_ap_migration },
            { "profile-mismatch-migration", test_ap_profile_mismatch },
#endif
        };

        for (size_t i = 0; i < G_N_ELEMENTS(tests); i++) {
            g_autofree char *name = g_strdup_printf(
                "/cyw43455/model-profile%u/open-ap/%s", profile,
                tests[i].name);

            qtest_add_data_func(name, GUINT_TO_POINTER(profile),
                                tests[i].function);
        }
    }
    return g_test_run();
}
