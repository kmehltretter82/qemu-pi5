/*
 * BCM2712 per-core PMU SPI routing, using real AArch64 counter overflow.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"

/* Hardware values independent of the board implementation. */
#define PMU_SPI         16
#define PMU_INTID       (32 + PMU_SPI)
#define NCPUS           4
#define WORKER          0x1000
#define MAILBOX         0x10000
#define MAILBOX_STRIDE  0x40
#define PSCI_RESULTS    0x10200
#define SPURIOUS        1023

enum MailboxOffset {
    COMMAND = 0, DONE = 4, COUNTER = 8, OVERFLOW = 12, INTEN = 16,
    HPPIR = 20, PMCR = 24, IAR = 28, READY = 32, CNTEN = 36,
    CURRENT_EL = 40, AFF1 = 44,
};

enum Command {
    DO_OVERFLOW = 1, ENABLE_IRQ, MASK_IRQ, ACK_CLEAR, REPEAT_OVERFLOW, SAMPLE,
};

/*
 * Embedded, stackless AArch64 guest: no cross compiler or asset at test time.
 * No-kernel boot starts CPU0 at PC0/EL2; PSCI CPU_ON64 starts the other cores
 * at WORKER/EL2 with the core index in x0. DAIF stays masked: workers inspect
 * their banked GICC_HPPIR, then explicitly read IAR and clear/EOI the IRQ.
 * The host commands one core at a time and checks all four CPU interfaces.
 *
 * x19 holds the mailbox, x20 GICC; w8 is the current command/sequence and
 * w9 the last completed one. PMEVTYPER0 selects SW_INCR (event0), with NSH
 * set to include EL2. MDCR_EL2.HPMN=6 assigns all six counters to PMCR.E.
 * PMSWINC increments counter0 from 0xffffffff to zero: PMOVSSET is not used.
 * Comments give the assembly for every instruction, including branch targets.
 */
static const uint32_t boot_code[] = {
    0xd5034fdf, /* msr daifset, #15 */
    0xd2804014, /* mov x20, #0x200 */
    0xf2a00034, /* movk x20, #1, lsl #16: PSCI_RESULTS */
    0xd2920015, /* mov x21, #0x9000 */
    0xf2affff5, /* movk x21, #0x7fff, lsl #16 */
    0xf2c00215, /* movk x21, #0x10, lsl #32: GICD */
    0xb94002a1, /* ldr w1, [x21]: reset GICD_CTLR */
    0xb9000281, /* str w1, [x20] */
    0xb94106a1, /* ldr w1, [x21, #0x104]: reset GICD_ISENABLER1 */
    0xb9000681, /* str w1, [x20, #4] */
    0x52800021, /* mov w1, #1 */
    0xb90002a1, /* str w1, [x21]: enable Group0 */
    0x52804021, /* mov w1, #0x201 */
    0x72a10081, /* movk w1, #0x804, lsl #16: target masks 1/2/4/8 */
    0xb90832a1, /* str w1, [x21, #0x830]: GICD_ITARGETSR12 */
    0x52a001e1, /* mov w1, #0xf0000: SPI16..19 */
    0xb90106a1, /* str w1, [x21, #0x104]: GICD_ISENABLER1 */
    0xd2800033, /* mov x19, #1 */
    0xd2800060, /* boot_next: mov x0, #3 */
    0xf2b88000, /* movk x0, #0xc400, lsl #16: CPU_ON64 */
    0xd378de61, /* lsl x1, x19, #8: target MPIDR Aff1 */
    0xd2820002, /* mov x2, #0x1000: worker entry */
    0xaa1303e3, /* mov x3, x19: context_id */
    0xd4000003, /* smc #0 */
    0xf8337a80, /* str x0, [x20, x19, lsl #3]: PSCI result */
    0x91000673, /* add x19, x19, #1 */
    0xf100127f, /* cmp x19, #4 */
    0x54fffee1, /* b.ne boot_next */
    0xd2800000, /* mov x0, #0: CPU0 context */
    0xd2820002, /* mov x2, #0x1000 */
    0xd61f0040, /* br x2 */
};

static const uint32_t worker_code[] = {
    0xd5034fdf, /* msr daifset, #15 */
    0xd2a00033, /* mov x19, #0x10000 */
    0x8b001a73, /* add x19, x19, x0, lsl #6: mailbox */
    0xd2940014, /* mov x20, #0xa000 */
    0xf2affff4, /* movk x20, #0x7fff, lsl #16 */
    0xf2c00214, /* movk x20, #0x10, lsl #32: GICC */
    0xd53be801, /* mrs x1, pmevcntr0_el0: reset snapshot */
    0xb9000a61, /* str w1, [x19, #8] */
    0xd53b9c61, /* mrs x1, pmovsclr_el0 */
    0xb9000e61, /* str w1, [x19, #12] */
    0xd5389e21, /* mrs x1, pmintenset_el1 */
    0xb9001261, /* str w1, [x19, #16] */
    0xd53b9c01, /* mrs x1, pmcr_el0 */
    0xb9001a61, /* str w1, [x19, #24] */
    0xd53b9c21, /* mrs x1, pmcntenset_el0 */
    0xb9002661, /* str w1, [x19, #36] */
    0xd5384241, /* mrs x1, CurrentEL */
    0xb9002a61, /* str w1, [x19, #40] */
    0xd53800a1, /* mrs x1, mpidr_el1 */
    0xd3483c21, /* ubfx x1, x1, #8, #8 */
    0xb9002e61, /* str w1, [x19, #44] */
    0x52807fe1, /* mov w1, #1023 */
    0xb9001e61, /* str w1, [x19, #28]: last acknowledged IAR */
    0xd28000c1, /* mov x1, #6 */
    0xd51c1121, /* msr mdcr_el2, x1 */
    0xd2a10001, /* mov x1, #0x08000000: NSH + SW_INCR */
    0xd51bec01, /* msr pmevtyper0_el0, x1 */
    0x52800021, /* mov w1, #1 */
    0xb9000281, /* str w1, [x20]: GICC_CTLR */
    0x52801fe1, /* mov w1, #255 */
    0xb9000681, /* str w1, [x20, #4]: GICC_PMR */
    0x52800009, /* mov w9, #0 */
    0x52800021, /* mov w1, #1 */
    0xd5033fbf, /* dmb sy */
    0xb9002261, /* str w1, [x19, #32]: ready */
    0xd503203f, /* poll: yield: let single-thread TCG schedule other cores */
    0xb9400268, /* ldr w8, [x19] */
    0x6b09011f, /* cmp w8, w9 */
    0x54ffffa0, /* b.eq poll */
    0x12001d0a, /* and w10, w8, #255: opcode */
    0xd2800021, /* mov x1, #1: counter0 mask */
    0x7100055f, /* cmp w10, #DO_OVERFLOW */
    0x54000140, /* b.eq overflow */
    0x7100095f, /* cmp w10, #ENABLE_IRQ */
    0x540001e0, /* b.eq enable */
    0x71000d5f, /* cmp w10, #MASK_IRQ */
    0x540001e0, /* b.eq mask */
    0x7100115f, /* cmp w10, #ACK_CLEAR */
    0x540001e0, /* b.eq acknowledge */
    0x7100155f, /* cmp w10, #REPEAT_OVERFLOW */
    0x54000040, /* b.eq overflow */
    0x14000010, /* b snapshot: SAMPLE */
    0x12800002, /* overflow: mov w2, #-1 (zero-extends to 0xffffffff) */
    0xd51be802, /* msr pmevcntr0_el0, x2 */
    0xd51b9c21, /* msr pmcntenset_el0, x1 */
    0xd51b9c01, /* msr pmcr_el0, x1 */
    0xd5033fdf, /* isb */
    0xd51b9c81, /* msr pmswinc_el0, x1 */
    0x14000009, /* b snapshot */
    0xd5189e21, /* enable: msr pmintenset_el1, x1 */
    0x14000007, /* b snapshot */
    0xd5189e41, /* mask: msr pmintenclr_el1, x1 */
    0x14000005, /* b snapshot */
    0xb9400e82, /* acknowledge: ldr w2, [x20, #12]: GICC_IAR */
    0xb9001e62, /* str w2, [x19, #28] */
    0xd51b9c61, /* msr pmovsclr_el0, x1: clear level before EOI */
    0xb9001282, /* str w2, [x20, #16]: GICC_EOIR */
    0xd5033fdf, /* snapshot: isb */
    0xd53be801, /* mrs x1, pmevcntr0_el0 */
    0xb9000a61, /* str w1, [x19, #8] */
    0xd53b9c61, /* mrs x1, pmovsclr_el0 */
    0xb9000e61, /* str w1, [x19, #12] */
    0xd5389e21, /* mrs x1, pmintenset_el1 */
    0xb9001261, /* str w1, [x19, #16] */
    0xb9401a81, /* ldr w1, [x20, #24]: banked GICC_HPPIR */
    0xb9001661, /* str w1, [x19, #20] */
    0xd53b9c01, /* mrs x1, pmcr_el0 */
    0xb9001a61, /* str w1, [x19, #24] */
    0xd53b9c21, /* mrs x1, pmcntenset_el0 */
    0xb9002661, /* str w1, [x19, #36] */
    0x2a0803e9, /* mov w9, w8 */
    0xd5033fbf, /* dmb sy */
    0xb9000668, /* str w8, [x19, #4]: completed command */
    0x17ffffd0, /* b poll */
};

typedef struct PMUTest {
    QTestState *qts;
    uint32_t sequence[NCPUS];
} PMUTest;

static uint64_t mailbox(unsigned cpu, unsigned offset)
{
    return MAILBOX + cpu * MAILBOX_STRIDE + offset;
}

static uint32_t result(PMUTest *t, unsigned cpu, unsigned offset)
{
    return qtest_readl(t->qts, mailbox(cpu, offset));
}

static void wait_result(PMUTest *t, unsigned cpu, unsigned offset,
                        uint32_t expected)
{
    int64_t deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

    do {
        if (result(t, cpu, offset) == expected) {
            return;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_error("CPU%u mailbox[%u] timed out: expected 0x%x, got 0x%x "
            "(ready=%u, done=0x%x, overflow=0x%x)", cpu, offset, expected,
            result(t, cpu, offset), result(t, cpu, READY),
            result(t, cpu, DONE), result(t, cpu, OVERFLOW));
}

static void command(PMUTest *t, unsigned cpu, enum Command op)
{
    uint32_t value = (++t->sequence[cpu] << 8) | op;

    qtest_writel(t->qts, mailbox(cpu, COMMAND), value);
    wait_result(t, cpu, DONE, value);
}

static void load_code(QTestState *qts, uint64_t address,
                      const uint32_t *code, size_t words)
{
    /* qtest_writel serializes little-endian words on every host. */
    for (size_t i = 0; i < words; i++) {
        qtest_writel(qts, address + i * 4, code[i]);
    }
}

static void start_guest(PMUTest *t)
{
    uint8_t empty[MAILBOX_STRIDE * NCPUS] = {0};

    memset(t->sequence, 0, sizeof(t->sequence));
    qtest_memwrite(t->qts, MAILBOX, empty, sizeof(empty));
    for (unsigned cpu = 1; cpu < NCPUS; cpu++) {
        qtest_writeq(t->qts, PSCI_RESULTS + cpu * 8, UINT64_MAX);
    }
    load_code(t->qts, 0, boot_code, G_N_ELEMENTS(boot_code));
    load_code(t->qts, WORKER, worker_code, G_N_ELEMENTS(worker_code));

    /* CPU0 initializes the distributor; each worker sets its banked GICC. */
    qtest_qmp_assert_success(t->qts, "{ 'execute': 'cont' }");

    for (unsigned cpu = 0; cpu < NCPUS; cpu++) {
        wait_result(t, cpu, READY, 1);
        if (cpu) {
            g_assert_cmphex(qtest_readq(t->qts, PSCI_RESULTS + cpu * 8), ==, 0);
        }
        g_assert_cmpuint(result(t, cpu, CURRENT_EL), ==, 8);
        g_assert_cmpuint(result(t, cpu, AFF1), ==, cpu);
        g_assert_cmphex(result(t, cpu, OVERFLOW), ==, 0);
        g_assert_cmphex(result(t, cpu, INTEN), ==, 0);
        g_assert_cmphex(result(t, cpu, CNTEN), ==, 0);
        g_assert_cmphex(result(t, cpu, PMCR) & 1, ==, 0);
        g_assert_cmpuint((result(t, cpu, PMCR) >> 11) & 31, ==, 6);
    }
    g_assert_cmphex(qtest_readl(t->qts, PSCI_RESULTS), ==, 0);
    g_assert_cmphex(qtest_readl(t->qts, PSCI_RESULTS + 4), ==, 0);
}

static void check_routing(PMUTest *t, int selected)
{
    for (unsigned cpu = 0; cpu < NCPUS; cpu++) {
        bool pending = selected == cpu;

        /* Workers mask DAIF and poll GICC; observe the GIC's IRQ outputs. */
        g_assert_cmpint(qtest_get_irq(t->qts, cpu), ==, pending);
        command(t, cpu, SAMPLE);
        g_assert_cmpuint(result(t, cpu, HPPIR), ==,
                         pending ? PMU_INTID + cpu : SPURIOUS);
    }
}

static void exercise_core(PMUTest *t, unsigned cpu)
{
    /* Overflow flags/counter advance while interrupt generation is masked. */
    command(t, cpu, DO_OVERFLOW);
    g_assert_cmphex(result(t, cpu, COUNTER), ==, 0);
    g_assert_cmphex(result(t, cpu, OVERFLOW), ==, 1);
    g_assert_cmphex(result(t, cpu, INTEN), ==, 0);
    check_routing(t, -1);

    command(t, cpu, ENABLE_IRQ);
    check_routing(t, cpu);
    command(t, cpu, MASK_IRQ);
    g_assert_cmphex(result(t, cpu, OVERFLOW), ==, 1);
    check_routing(t, -1);
    command(t, cpu, ENABLE_IRQ);
    check_routing(t, cpu);

    command(t, cpu, ACK_CLEAR);
    g_assert_cmpuint(result(t, cpu, IAR), ==, PMU_INTID + cpu);
    g_assert_cmphex(result(t, cpu, OVERFLOW), ==, 0);
    check_routing(t, -1);

    /* A second actual wrap must generate another interrupt after EOI. */
    command(t, cpu, REPEAT_OVERFLOW);
    g_assert_cmphex(result(t, cpu, COUNTER), ==, 0);
    g_assert_cmphex(result(t, cpu, OVERFLOW), ==, 1);
    check_routing(t, cpu);
    command(t, cpu, ACK_CLEAR);
    g_assert_cmpuint(result(t, cpu, IAR), ==, PMU_INTID + cpu);
    check_routing(t, -1);
}

static void test_pmu(gconstpointer machine)
{
    PMUTest t = {
        .qts = qtest_initf("-machine %s -accel tcg,thread=single -S "
                           "-serial null", (const char *)machine),
    };

    /* Input interception cannot represent this GIC's >256 GPIO inputs. */
    qtest_irq_intercept_out_named(t.qts, "/machine/soc/gic", "sysbus-irq");
    for (unsigned round = 0; round < 2; round++) {
        start_guest(&t);
        check_routing(&t, -1);
        for (unsigned cpu = 0; cpu < NCPUS; cpu++) {
            exercise_core(&t, cpu);
        }
        if (!round) {
            /* Reset with a real PMU overflow still asserting CPU3's SPI. */
            command(&t, NCPUS - 1, REPEAT_OVERFLOW);
            check_routing(&t, NCPUS - 1);
            qtest_qmp_assert_success(t.qts, "{ 'execute': 'stop' }");
            qtest_qmp_eventwait(t.qts, "STOP");
            qtest_system_reset(t.qts);
            for (unsigned cpu = 0; cpu < NCPUS; cpu++) {
                g_assert_false(qtest_get_irq(t.qts, cpu));
            }
            /* Reinject code: firmware DT ROM loading may overwrite PC0. */
        }
    }
    qtest_quit(t.qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (qtest_has_accel("tcg")) {
        qtest_add_data_func("/bcm2712/raspi5b/pmu/overflow", "raspi5b",
                            test_pmu);
        qtest_add_data_func("/bcm2712/raspi500/pmu/overflow", "raspi500",
                            test_pmu);
    }
    return g_test_run();
}
