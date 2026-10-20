/*
 * QTests for deterministic Cadence GEM transmit completion.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define GEM_BASE                    0xe000b000
#define GEM_NWCTRL                  (GEM_BASE + 0x00)
#define GEM_TXSTATUS                (GEM_BASE + 0x14)
#define GEM_TXQBASE                 (GEM_BASE + 0x1c)
#define GEM_ISR                     (GEM_BASE + 0x24)
#define GEM_IER                     (GEM_BASE + 0x28)

#define GEM_NWCTRL_ENABLE_TRANSMIT  (1U << 3)
#define GEM_NWCTRL_TRANSMIT_START   (1U << 9)
#define GEM_TXSTATUS_COMPLETE       (1U << 5)
#define GEM_ISR_XMIT_COMPLETE       (1U << 7)

#define GEM_TX_DESC_BASE            0x00010000
#define GEM_TX_BUFFER0              0x00011000
#define GEM_TX_BUFFER1              0x00012000
#define GEM_TX_DESC_USED            (1U << 31)
#define GEM_TX_DESC_WRAP            (1U << 30)
#define GEM_TX_DESC_LAST            (1U << 15)
#define GEM_TX_DESC_SIZE            8
#define GEM_TEST_PACKET_SIZE        64

#define GEM_TX_DELAY_US             1000
#define GEM_TX_DELAY_NS             (GEM_TX_DELAY_US * 1000)

#define GEM_QEMU_ARGS \
    "-machine xilinx-zynq-a9 -nic none"
#define GEM_DELAY_QEMU_ARGS \
    GEM_QEMU_ARGS \
    " -global cadence_gem.tx-completion-delay-us=" \
    stringify(GEM_TX_DELAY_US) \
    " -global cadence_gem.tx-completion-budget=1"
#define GEM_MIGRATION_DEST_QEMU_ARGS \
    GEM_QEMU_ARGS \
    " -global cadence_gem.tx-completion-delay-us=1000000" \
    " -global cadence_gem.tx-completion-budget=1"

static void gem_write_tx_descriptor(QTestState *qts, unsigned int index,
                                    uint32_t buffer, uint32_t flags)
{
    uint64_t desc = GEM_TX_DESC_BASE + index * GEM_TX_DESC_SIZE;

    qtest_writel(qts, desc, buffer);
    qtest_writel(qts, desc + 4, GEM_TEST_PACKET_SIZE | flags);
}

static uint32_t gem_read_tx_descriptor_control(QTestState *qts,
                                               unsigned int index)
{
    return qtest_readl(qts, GEM_TX_DESC_BASE +
                       index * GEM_TX_DESC_SIZE + 4);
}

static void gem_prepare_two_packet_ring(QTestState *qts)
{
    uint8_t packet[GEM_TEST_PACKET_SIZE] = {};

    qtest_memwrite(qts, GEM_TX_BUFFER0, packet, sizeof(packet));
    qtest_memwrite(qts, GEM_TX_BUFFER1, packet, sizeof(packet));
    gem_write_tx_descriptor(qts, 0, GEM_TX_BUFFER0, GEM_TX_DESC_LAST);
    gem_write_tx_descriptor(qts, 1, GEM_TX_BUFFER1,
                            GEM_TX_DESC_LAST | GEM_TX_DESC_WRAP);
    qtest_writel(qts, GEM_TXQBASE, GEM_TX_DESC_BASE);
    qtest_writel(qts, GEM_IER, GEM_ISR_XMIT_COMPLETE);
}

static void gem_kick_tx(QTestState *qts)
{
    qtest_writel(qts, GEM_NWCTRL, GEM_NWCTRL_ENABLE_TRANSMIT |
                 GEM_NWCTRL_TRANSMIT_START);
}

static void test_default_synchronous_completion(void)
{
    QTestState *qts = qtest_init(
        GEM_QEMU_ARGS " -global cadence_gem.tx-completion-budget=1");

    gem_prepare_two_packet_ring(qts);
    gem_kick_tx(qts);

    /*
     * The budget is deliberately nonzero: without an enabled delay it must
     * not change the legacy synchronous completion behavior.
     */
    g_assert_true(gem_read_tx_descriptor_control(qts, 0) &
                  GEM_TX_DESC_USED);
    g_assert_true(gem_read_tx_descriptor_control(qts, 1) &
                  GEM_TX_DESC_USED);
    g_assert_cmphex(qtest_readl(qts, GEM_TXSTATUS) &
                    GEM_TXSTATUS_COMPLETE, ==, GEM_TXSTATUS_COMPLETE);
    g_assert_cmphex(qtest_readl(qts, GEM_ISR) &
                    GEM_ISR_XMIT_COMPLETE, ==, GEM_ISR_XMIT_COMPLETE);

    qtest_quit(qts);
}

static void test_delayed_budget_and_reset(void)
{
    QTestState *qts = qtest_init(GEM_DELAY_QEMU_ARGS);

    gem_prepare_two_packet_ring(qts);
    gem_kick_tx(qts);

    g_assert_false(gem_read_tx_descriptor_control(qts, 0) &
                   GEM_TX_DESC_USED);
    g_assert_false(gem_read_tx_descriptor_control(qts, 1) &
                   GEM_TX_DESC_USED);
    g_assert_cmphex(qtest_readl(qts, GEM_TXSTATUS) &
                    GEM_TXSTATUS_COMPLETE, ==, 0);
    g_assert_cmphex(qtest_readl(qts, GEM_ISR), ==, 0);

    qtest_clock_step(qts, GEM_TX_DELAY_NS - 1);
    g_assert_false(gem_read_tx_descriptor_control(qts, 0) &
                   GEM_TX_DESC_USED);

    qtest_clock_step(qts, 1);
    g_assert_true(gem_read_tx_descriptor_control(qts, 0) &
                  GEM_TX_DESC_USED);
    g_assert_false(gem_read_tx_descriptor_control(qts, 1) &
                   GEM_TX_DESC_USED);
    g_assert_cmphex(qtest_readl(qts, GEM_TXQBASE), ==,
                    GEM_TX_DESC_BASE + GEM_TX_DESC_SIZE);
    g_assert_cmphex(qtest_readl(qts, GEM_ISR) &
                    GEM_ISR_XMIT_COMPLETE, ==, GEM_ISR_XMIT_COMPLETE);
    qtest_writel(qts, GEM_TXSTATUS, GEM_TXSTATUS_COMPLETE);

    qtest_clock_step(qts, GEM_TX_DELAY_NS);
    g_assert_true(gem_read_tx_descriptor_control(qts, 1) &
                  GEM_TX_DESC_USED);
    g_assert_cmphex(qtest_readl(qts, GEM_TXQBASE), ==, GEM_TX_DESC_BASE);
    g_assert_cmphex(qtest_readl(qts, GEM_ISR) &
                    GEM_ISR_XMIT_COMPLETE, ==, GEM_ISR_XMIT_COMPLETE);

    /* A reset must cancel a delayed completion that is already armed. */
    gem_write_tx_descriptor(qts, 0, GEM_TX_BUFFER0,
                            GEM_TX_DESC_LAST | GEM_TX_DESC_WRAP);
    qtest_writel(qts, GEM_TXQBASE, GEM_TX_DESC_BASE);
    qtest_writel(qts, GEM_TXSTATUS, GEM_TXSTATUS_COMPLETE);
    gem_kick_tx(qts);
    g_assert_false(gem_read_tx_descriptor_control(qts, 0) &
                   GEM_TX_DESC_USED);

    qtest_system_reset(qts);
    /*
     * Leave a descriptor ready and re-enable TX without TRANSMIT_START.
     * This makes a stale pre-reset timer observable: if reset failed to
     * cancel it, the callback would complete this descriptor.
     */
    gem_write_tx_descriptor(qts, 0, GEM_TX_BUFFER0,
                            GEM_TX_DESC_LAST | GEM_TX_DESC_WRAP);
    qtest_writel(qts, GEM_TXQBASE, GEM_TX_DESC_BASE);
    qtest_writel(qts, GEM_NWCTRL, GEM_NWCTRL_ENABLE_TRANSMIT);
    qtest_clock_step(qts, GEM_TX_DELAY_NS);
    g_assert_false(gem_read_tx_descriptor_control(qts, 0) &
                   GEM_TX_DESC_USED);
    g_assert_cmphex(qtest_readl(qts, GEM_TXSTATUS) &
                    GEM_TXSTATUS_COMPLETE, ==, 0);

    /* Disabling TX must cancel a completion that is already armed too. */
    gem_kick_tx(qts);
    qtest_writel(qts, GEM_NWCTRL, 0);
    qtest_writel(qts, GEM_NWCTRL, GEM_NWCTRL_ENABLE_TRANSMIT);
    qtest_clock_step(qts, GEM_TX_DELAY_NS);
    g_assert_false(gem_read_tx_descriptor_control(qts, 0) &
                   GEM_TX_DESC_USED);
    g_assert_cmphex(qtest_readl(qts, GEM_TXSTATUS) &
                    GEM_TXSTATUS_COMPLETE, ==, 0);

    qtest_quit(qts);
}

#ifndef _WIN32
static void gem_wait_for_migration(QTestState *qts)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    while (g_get_monotonic_time() < deadline) {
        g_autoptr(QDict) response = qtest_qmp(
            qts, "{ 'execute': 'query-migrate' }");
        QDict *result = qdict_get_qdict(response, "return");
        const char *status = qdict_get_str(result, "status");

        if (g_str_equal(status, "completed")) {
            return;
        }
        g_assert_false(g_str_equal(status, "failed"));
        g_assert_false(g_str_equal(status, "cancelled"));
        g_usleep(1000);
    }

    g_error("timed out waiting for Cadence GEM migration");
}

static void test_pending_completion_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *tmpdir = NULL;
    g_autofree char *state_path = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *destination_args = NULL;
    QTestState *source = qtest_init(GEM_DELAY_QEMU_ARGS);
    QTestState *destination;

    gem_prepare_two_packet_ring(source);
    gem_kick_tx(source);
    qtest_clock_step(source, 400 * 1000);
    g_assert_false(gem_read_tx_descriptor_control(source, 0) &
                   GEM_TX_DESC_USED);

    qtest_qmp_assert_success(source, "{ 'execute': 'stop' }");
    tmpdir = g_dir_make_tmp("cadence-gem-migration-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    state_path = g_build_filename(tmpdir, "state", NULL);
    uri = g_strdup_printf("file:%s", state_path);

    qtest_qmp_assert_success(source,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    gem_wait_for_migration(source);

    destination_args = g_strdup_printf(
        GEM_MIGRATION_DEST_QEMU_ARGS " -incoming %s", uri);
    destination = qtest_init(destination_args);
    gem_wait_for_migration(destination);
    qtest_qmp_assert_success(destination, "{ 'execute': 'cont' }");

    g_assert_false(gem_read_tx_descriptor_control(destination, 0) &
                   GEM_TX_DESC_USED);
    /*
     * The destination property is deliberately one second.  Completing
     * within two milliseconds proves the source deadline was migrated
     * rather than a fresh post-load timer being armed.
     */
    qtest_clock_step(destination, 2 * GEM_TX_DELAY_NS);
    g_assert_true(gem_read_tx_descriptor_control(destination, 0) &
                  GEM_TX_DESC_USED);
    g_assert_false(gem_read_tx_descriptor_control(destination, 1) &
                   GEM_TX_DESC_USED);

    qtest_quit(destination);
    qtest_quit(source);
    g_assert_cmpint(g_unlink(state_path), ==, 0);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

static void test_enabled_no_start_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *tmpdir = NULL;
    g_autofree char *state_path = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *destination_args = NULL;
    QTestState *source = qtest_init(GEM_DELAY_QEMU_ARGS);
    QTestState *destination;

    gem_prepare_two_packet_ring(source);
    qtest_writel(source, GEM_NWCTRL, GEM_NWCTRL_ENABLE_TRANSMIT);
    g_assert_false(gem_read_tx_descriptor_control(source, 0) &
                   GEM_TX_DESC_USED);

    qtest_qmp_assert_success(source, "{ 'execute': 'stop' }");
    tmpdir = g_dir_make_tmp("cadence-gem-no-start-migration-XXXXXX",
                            &error);
    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    state_path = g_build_filename(tmpdir, "state", NULL);
    uri = g_strdup_printf("file:%s", state_path);

    qtest_qmp_assert_success(source,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    gem_wait_for_migration(source);

    destination_args = g_strdup_printf(
        GEM_DELAY_QEMU_ARGS " -incoming %s", uri);
    destination = qtest_init(destination_args);
    gem_wait_for_migration(destination);
    qtest_qmp_assert_success(destination, "{ 'execute': 'cont' }");

    /* Migration must not synthesize a TRANSMIT_START command. */
    g_assert_false(gem_read_tx_descriptor_control(destination, 0) &
                   GEM_TX_DESC_USED);
    qtest_clock_step(destination, 2 * GEM_TX_DELAY_NS);
    g_assert_false(gem_read_tx_descriptor_control(destination, 0) &
                   GEM_TX_DESC_USED);
    g_assert_cmphex(qtest_readl(destination, GEM_TXSTATUS) &
                    GEM_TXSTATUS_COMPLETE, ==, 0);

    qtest_quit(destination);
    qtest_quit(source);
    g_assert_cmpint(g_unlink(state_path), ==, 0);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}
#endif

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/net/cadence-gem/default-synchronous",
                   test_default_synchronous_completion);
    qtest_add_func("/net/cadence-gem/delayed-budget-reset",
                   test_delayed_budget_and_reset);
#ifndef _WIN32
    qtest_add_func("/net/cadence-gem/pending-migration",
                   test_pending_completion_migration);
    qtest_add_func("/net/cadence-gem/enabled-no-start-migration",
                   test_enabled_no_start_migration);
#endif

    return g_test_run();
}
