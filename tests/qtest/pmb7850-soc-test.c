/*
 * PMB7850 reusable SoC qtests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/c166/pmb7850/capcom.h"
#include "qobject/qdict.h"

#define SYSCON  0xff12
#define T3      0xfe42
#define T3CON   0xff42
#define T3IC    0xff62
#define S0TBUF  0xfeb0
#define S0BG    0xfeb4
#define S0TIC   0xff6c
#define S0CON   0xffb0
#define SSC0TB  0xf0b0
#define SSC0RB  0xf0b2
#define SSC0BR  0xf0b4
#define SSC0TIC 0xff72
#define SSC0RIC 0xff74
#define SSC0EIC 0xff76
#define SSC0CON 0xffb2
#define ADC_COMMAND 0xe062
#define ADC_RESULT_HI 0xef2c
#define ADC_RESULT_LO 0xef2e
#define T0      0xfe50
#define T0REL   0xfe54
#define T01CON  0xff50
#define CC0     0xfe80
#define CCM0    0xff52
#define CC0IC   0xff78
#define T0IC    0xff9c
#define TDMA_TOP     0xe022
#define TDMA_IRQ36   0xe024
#define TDMA_IRQ37   0xe026
#define TDMA_COUNTER 0xe034
#define TDMA_CONTROL 0xe060
#define TDMA_IRQ36IC 0xf186
#define TDMA_IRQ37IC 0xf18e
#define SIM_CTRL     0xef50
#define SIM_STATUS   0xef54
#define SIM_RX       0xef5a
#define SIM_BYTE_IC  0xf184
#define SIM_STATUS_IC 0xf18c
#define SIM_TX       0xef58
#define SIM_RESULT_HI 0xef60
#define SIM_RESULT_LO 0xef62
#define PECC3        0xfec6
#define PECSN3       0xfed6
#define SRCP3        0xfcec
#define DSTP3        0xfcee
#define PECC_BWT     (1 << 8)

static QTestState *pmb_start(void)
{
    return qtest_init("-M pmb7850-test -display none");
}

static QTestState *pmb_start_sim(void)
{
    return qtest_init(
        "-M pmb7850-test -global pmb7850-soc.sim-profile=deterministic "
        "-display none");
}

static uint64_t sim_qom_get_u64(QTestState *qts, const char *property)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/soc',"
        "'property':%s}}", property);
    uint64_t value;

    g_assert_false(qdict_haskey(response, "error"));
    value = qdict_get_int(response, "return");
    qobject_unref(response);
    return value;
}

static bool sim_qom_get_bool(QTestState *qts, const char *property)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/soc',"
        "'property':%s}}", property);
    bool value;

    g_assert_false(qdict_haskey(response, "error"));
    value = qdict_get_bool(response, "return");
    qobject_unref(response);
    return value;
}

static QTestState *sim_ready(void);
static void sim_send(QTestState *qts, const uint8_t *bytes, size_t size);

static void test_sim_default_off_and_atr(void)
{
    QTestState *qts = pmb_start();

    g_assert_false(sim_qom_get_bool(qts, "sim-attached"));
    qtest_writew(qts, SIM_CTRL, 1 << 13);
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(qtest_readw(qts, SIM_STATUS), ==, 0);
    g_assert_cmphex(qtest_readw(qts, SIM_STATUS_IC), ==, 0);
    qtest_quit(qts);

    qts = pmb_start_sim();
    g_assert_true(sim_qom_get_bool(qts, "sim-attached"));
    qtest_writew(qts, SIM_CTRL, 1 << 13);
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(qtest_readw(qts, SIM_STATUS), ==, 2);
    g_assert_cmphex(qtest_readw(qts, SIM_RX), ==, 3);
    g_assert_true(qtest_readw(qts, SIM_STATUS_IC) & (1 << 7));
    qtest_writew(qts, SIM_STATUS_IC, 0);
    qtest_clock_step(qts, 20000);
    g_assert_true(qtest_readw(qts, SIM_BYTE_IC) & (1 << 7));
    g_assert_cmphex(qtest_readb(qts, SIM_RX), ==, 0x90);
    qtest_writew(qts, SIM_BYTE_IC, 0);
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(qtest_readb(qts, SIM_RX), ==, 0x11);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readw(qts, SIM_STATUS), ==, 0);
    qtest_writew(qts, SIM_CTRL, 1 << 13);
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(qtest_readw(qts, SIM_STATUS), ==, 2);
    qtest_quit(qts);
}

static void test_sim_blocked_event_resumption(void)
{
    static const uint8_t command[] = { 0xa0, 0xf2, 0, 0, 2 };
    QTestState *qts = pmb_start_sim();
    uint64_t events;

    /* A due activation waits without repeated work while its IRQ is pending. */
    qtest_writew(qts, SIM_STATUS_IC, 1 << 7);
    qtest_writew(qts, SIM_CTRL, 1 << 13);
    qtest_clock_step(qts, 20000);
    g_assert_cmpuint(sim_qom_get_u64(qts, "sim-event-count"), ==, 0);
    qtest_clock_step(qts, 20000);
    g_assert_cmpuint(sim_qom_get_u64(qts, "sim-event-count"), ==, 0);

    /* Clearing the IRQ rearms the overdue event exactly once. */
    qtest_writew(qts, SIM_STATUS_IC, 0);
    qtest_clock_step(qts, 20000);
    g_assert_cmpuint(sim_qom_get_u64(qts, "sim-event-count"), ==, 1);
    g_assert_true(qtest_readw(qts, SIM_STATUS_IC) & (1 << 7));
    qtest_clock_step(qts, 20000);
    g_assert_cmpuint(sim_qom_get_u64(qts, "sim-event-count"), ==, 1);
    qtest_quit(qts);

    qts = sim_ready();
    events = sim_qom_get_u64(qts, "sim-event-count");
    sim_send(qts, command, sizeof(command));

    /* The procedure byte requires a PEC direction before it can advance. */
    qtest_clock_step(qts, 20000);
    g_assert_cmpuint(sim_qom_get_u64(qts, "sim-event-count"), ==, events);
    qtest_writew(qts, SRCP3, SIM_RX);
    qtest_writew(qts, DSTP3, 0xe500);
    qtest_writew(qts, PECSN3, 0);
    qtest_writew(qts, PECC3, PECC_BWT | 2);
    qtest_clock_step(qts, 20000);
    g_assert_cmpuint(sim_qom_get_u64(qts, "sim-event-count"), ==,
                     events + 2);
    g_assert_true(qtest_readw(qts, SIM_BYTE_IC) & (1 << 7));

    /* A loaded RX byte remains unique until consumption and IRQ release. */
    qtest_clock_step(qts, 20000);
    g_assert_cmpuint(sim_qom_get_u64(qts, "sim-event-count"), ==,
                     events + 2);
    (void)qtest_readb(qts, SIM_RX);
    qtest_writew(qts, SIM_BYTE_IC, 0);
    qtest_clock_step(qts, 20000);
    g_assert_cmpuint(sim_qom_get_u64(qts, "sim-event-count"), ==,
                     events + 3);
    g_assert_true(qtest_readw(qts, SIM_BYTE_IC) & (1 << 7));
    qtest_quit(qts);
}

static QTestState *sim_ready(void)
{
    static const uint8_t atr[] = { 0x90, 0x11, 0 };
    QTestState *qts = pmb_start_sim();

    qtest_writew(qts, SIM_CTRL, 1 << 13);
    qtest_clock_step(qts, 20000);
    qtest_writew(qts, SIM_STATUS_IC, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(atr); i++) {
        qtest_clock_step(qts, 20000);
        g_assert_true(qtest_readw(qts, SIM_BYTE_IC) & (1 << 7));
        g_assert_cmphex(qtest_readb(qts, SIM_RX), ==, atr[i]);
        qtest_writew(qts, SIM_BYTE_IC, 0);
    }
    qtest_clock_step(qts, 20000);
    return qts;
}

static void sim_send(QTestState *qts, const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        qtest_writeb(qts, SIM_TX, bytes[i]);
    }
}

static void sim_select_id(QTestState *qts, uint16_t id)
{
    static const uint8_t header[] = { 0xa0, 0xa4, 0, 0, 2 };
    uint8_t data[] = { id >> 8, id };

    sim_send(qts, header, sizeof(header));
    qtest_writew(qts, DSTP3, SIM_TX);
    qtest_writew(qts, PECSN3, 0);
    qtest_writew(qts, PECC3, PECC_BWT | 2);
    qtest_clock_step(qts, 20000);
    sim_send(qts, data, sizeof(data));
    qtest_writew(qts, PECC3, PECC_BWT);
    qtest_writew(qts, SIM_BYTE_IC, 0);
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(qtest_readw(qts, SIM_RESULT_HI), ==, 0x009f);
    g_assert_cmphex(qtest_readw(qts, SIM_RESULT_LO), ==, 0x000f);
    g_assert_cmphex(qtest_readw(qts, SIM_STATUS), ==, 8);
    g_assert_true(qtest_readw(qts, SIM_STATUS_IC) & (1 << 7));
}

static void sim_select_df_gsm(QTestState *qts)
{
    sim_select_id(qts, 0x7f20);
}

static void test_sim_select_get_response(void)
{
    static const uint8_t get_response[] = { 0xa0, 0xc0, 0, 0, 15 };
    QTestState *qts = sim_ready();
    uint8_t response[15];

    sim_select_df_gsm(qts);
    qtest_writew(qts, SIM_STATUS_IC, 0);
    sim_send(qts, get_response, sizeof(get_response));
    qtest_writew(qts, SRCP3, SIM_RX);
    qtest_writew(qts, DSTP3, 0xe200);
    qtest_writew(qts, PECSN3, 0);
    qtest_writew(qts, PECC3, PECC_BWT | 15);
    qtest_clock_step(qts, 20000); /* consume procedure byte */
    for (unsigned i = 0; i < ARRAY_SIZE(response); i++) {
        qtest_clock_step(qts, 20000);
        g_assert_true(qtest_readw(qts, SIM_BYTE_IC) & (1 << 7));
        response[i] = qtest_readb(qts, SIM_RX);
        qtest_writew(qts, SIM_BYTE_IC, 0);
    }
    g_assert_cmphex(response[4], ==, 0x7f);
    g_assert_cmphex(response[5], ==, 0x20);
    g_assert_cmphex(response[6], ==, 2);
    g_assert_cmphex(response[13], ==, 0x93);
    qtest_writew(qts, PECC3, PECC_BWT);
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(qtest_readw(qts, SIM_RESULT_HI), ==, 0x0090);
    g_assert_cmphex(qtest_readw(qts, SIM_RESULT_LO), ==, 0);
    g_assert_cmpuint(sim_qom_get_u64(qts, "sim-apdu-count"), ==, 2);
    g_assert_cmpuint(sim_qom_get_u64(qts, "sim-byte-count"), ==, 32);
    qtest_quit(qts);
}

static void test_sim_status_profile(void)
{
    static const uint8_t status[] = { 0xa0, 0xf2, 0, 0, 20 };
    QTestState *qts = sim_ready();
    uint8_t response[20];

    sim_select_df_gsm(qts);
    qtest_writew(qts, SIM_STATUS_IC, 0);
    sim_send(qts, status, sizeof(status));
    qtest_writew(qts, SRCP3, SIM_RX);
    qtest_writew(qts, DSTP3, 0xe300);
    qtest_writew(qts, PECSN3, 0);
    qtest_writew(qts, PECC3, PECC_BWT | 20);
    qtest_clock_step(qts, 20000); /* consume procedure byte */
    for (unsigned i = 0; i < ARRAY_SIZE(response); i++) {
        qtest_clock_step(qts, 20000);
        g_assert_true(qtest_readw(qts, SIM_BYTE_IC) & (1 << 7));
        response[i] = qtest_readb(qts, SIM_RX);
        qtest_writew(qts, SIM_BYTE_IC, 0);
    }
    g_assert_cmphex(response[4], ==, 0x7f);
    g_assert_cmphex(response[5], ==, 0x20);
    g_assert_cmphex(response[6], ==, 2);
    g_assert_cmphex(response[13], ==, 0x93);
    g_assert_cmphex(response[16], ==, 2);
    g_assert_cmphex(response[18], ==, 0x83);
    g_assert_cmphex(response[19], ==, 0x8a);
    qtest_writew(qts, PECC3, PECC_BWT);
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(qtest_readw(qts, SIM_RESULT_HI), ==, 0x0090);
    g_assert_cmphex(qtest_readw(qts, SIM_RESULT_LO), ==, 0);
    qtest_quit(qts);
}

static void test_sim_update_read_binary(void)
{
    static const uint8_t update[] = { 0xa0, 0xd6, 0, 0, 1 };
    static const uint8_t read[] = { 0xa0, 0xb0, 0, 0, 1 };
    QTestState *qts = sim_ready();

    sim_select_df_gsm(qts);
    qtest_writew(qts, SIM_STATUS_IC, 0);
    sim_select_id(qts, 0x6f05);
    qtest_writew(qts, SIM_STATUS_IC, 0);

    sim_send(qts, update, sizeof(update));
    qtest_writew(qts, DSTP3, SIM_TX);
    qtest_writew(qts, PECSN3, 0);
    qtest_writew(qts, PECC3, PECC_BWT | 1);
    qtest_clock_step(qts, 20000); /* consume procedure byte */
    qtest_writeb(qts, SIM_TX, 0x42);
    qtest_writew(qts, PECC3, PECC_BWT);
    qtest_writew(qts, SIM_BYTE_IC, 0);
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(qtest_readw(qts, SIM_RESULT_HI), ==, 0x0090);
    g_assert_cmphex(qtest_readw(qts, SIM_RESULT_LO), ==, 0);
    qtest_writew(qts, SIM_STATUS_IC, 0);

    sim_send(qts, read, sizeof(read));
    qtest_writew(qts, SRCP3, SIM_RX);
    qtest_writew(qts, DSTP3, 0xe400);
    qtest_writew(qts, PECC3, PECC_BWT | 1);
    qtest_clock_step(qts, 20000); /* consume procedure byte */
    qtest_clock_step(qts, 20000);
    g_assert_true(qtest_readw(qts, SIM_BYTE_IC) & (1 << 7));
    g_assert_cmphex(qtest_readb(qts, SIM_RX), ==, 0x42);
    qtest_writew(qts, SIM_BYTE_IC, 0);
    qtest_writew(qts, PECC3, PECC_BWT);
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(qtest_readw(qts, SIM_RESULT_HI), ==, 0x0090);
    g_assert_cmphex(qtest_readw(qts, SIM_RESULT_LO), ==, 0);
    qtest_quit(qts);
}

static void test_reset_and_register_map(void)
{
    QTestState *qts = pmb_start();

    g_assert_cmphex(qtest_readw(qts, 0xfe00), ==, 0);
    g_assert_cmphex(qtest_readw(qts, 0xfe02), ==, 1);
    g_assert_cmphex(qtest_readw(qts, 0xfe04), ==, 2);
    g_assert_cmphex(qtest_readw(qts, 0xfe06), ==, 3);
    g_assert_cmphex(qtest_readw(qts, 0xfe08), ==, 0); /* read-only CSP */
    g_assert_cmphex(qtest_readw(qts, 0xfe10), ==, 0xfc00);
    g_assert_cmphex(qtest_readw(qts, 0xfe12), ==, 0xfc00);

    qtest_writew(qts, 0xfe00, 0xffff);
    qtest_writew(qts, 0xfe08, 0xffff);
    g_assert_cmphex(qtest_readw(qts, 0xfe00), ==, 0x03ff);
    g_assert_cmphex(qtest_readw(qts, 0xfe08), ==, 0);

    qtest_writew(qts, 0xfc00, 0x7856);
    qtest_writew(qts, 0xe100, 0x3412);
    g_assert_cmphex(qtest_readw(qts, 0xfc00), ==, 0x7856);
    g_assert_cmphex(qtest_readw(qts, 0xe100), ==, 0x3412);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readw(qts, 0xfc00), ==, 0);
    g_assert_cmphex(qtest_readw(qts, 0xe100), ==, 0);
    qtest_quit(qts);
}

static void test_memory_ownership(void)
{
    QTestState *qts = pmb_start();

    qtest_writew(qts, 0x2000, 0x1111); /* low boot view */
    g_assert_cmphex(qtest_readw(qts, 0x2000), ==, 0x1111);

    qtest_writew(qts, SYSCON, 1 << 10); /* ROMEN */
    qtest_writew(qts, 0x2000, 0x2222); /* local memory now wins */
    g_assert_cmphex(qtest_readw(qts, 0x2000), ==, 0x2222);

    qtest_writew(qts, SYSCON, 0);
    g_assert_cmphex(qtest_readw(qts, 0x2000), ==, 0x1111);
    qtest_writew(qts, SYSCON, 1 << 10);
    g_assert_cmphex(qtest_readw(qts, 0x2000), ==, 0x2222);

    /* ROMS1 remaps the same physical 256 KiB array, not a second RAM. */
    qtest_writew(qts, SYSCON, (1 << 10) | (1 << 12));
    qtest_writew(qts, 0x10000, 0x3333);
    qtest_writew(qts, 0x18000, 0x4444);
    qtest_writew(qts, SYSCON, 1 << 10);
    g_assert_cmphex(qtest_readw(qts, 0), ==, 0x3333);
    g_assert_cmphex(qtest_readw(qts, 0x18000), ==, 0x4444);
    qtest_quit(qts);
}

static void test_ports_and_interrupt_registers(void)
{
    QTestState *qts = pmb_start();

    qtest_irq_intercept_out_named(qts, "/machine/soc", "p3-out");
    qtest_set_irq_in(qts, "/machine/soc", "p3-in", 5, 1);
    g_assert_cmphex(qtest_readw(qts, 0xffc4) & (1 << 5), ==, 1 << 5);
    qtest_writew(qts, 0xffc6, 1 << 5);
    qtest_writew(qts, 0xffc4, 1 << 5);
    g_assert_true(qtest_get_irq(qts, 5));
    qtest_writew(qts, 0xffc4, 0);
    g_assert_false(qtest_get_irq(qts, 5));

    /* Board calibration is external; PMB owns command/result transport. */
    qtest_set_irq_in(qts, "/machine/soc", "adc-raw", 3, -1234);
    qtest_writew(qts, ADC_COMMAND, 0x0803);
    g_assert_cmphex(qtest_readw(qts, ADC_RESULT_HI), ==, 0);
    g_assert_cmphex(qtest_readw(qts, ADC_RESULT_LO), ==, 0xfb2e);
    qtest_writew(qts, 0xff9a, 0x00fc); /* ADEIC: IE, IR, ILVL=15 */
    g_assert_cmphex(qtest_readw(qts, 0xff9a), ==, 0x00fc);
    qtest_quit(qts);
}

static void test_eop_subnodes(void)
{
    QTestState *qts = pmb_start();

    /* A pending subnode asserts EOPIC when its adjacent enable bit is set. */
    qtest_writew(qts, 0xffa8, 0x0002);
    g_assert_cmphex(qtest_readw(qts, 0xf180) & 0x80, ==, 0);
    qtest_writew(qts, 0xffa8, 0x0003);
    g_assert_cmphex(qtest_readw(qts, 0xf180) & 0x80, ==, 0x80);
    qtest_writew(qts, 0xffa8, 0x0001);
    g_assert_cmphex(qtest_readw(qts, 0xf180) & 0x80, ==, 0);
    qtest_quit(qts);
}

static void test_external_bus_windows(void)
{
    QTestState *qts = pmb_start();

    /* ADDRSEL1: 0x200000..0x3fffff, target offset 0x123. */
    qtest_writew(qts, 0xfe18, 0x2009);
    qtest_writew(qts, 0xff14, 1 << 10);
    qtest_writew(qts, 0x200122, 0xbeef);
    g_assert_cmphex(qtest_readw(qts, 0x200122), ==, 0xbeef);
    qtest_writew(qts, 0xff14, 0);
    g_assert_cmphex(qtest_readw(qts, 0x122), ==, 0xbeef);

    /* XBUS5: 0x600000..0x6fffff, target offset 0x456. */
    qtest_writew(qts, 0xf01c, 0x6008);
    qtest_writew(qts, 0xf11c, 1 << 10);
    qtest_writew(qts, 0xf024, 1 << 4);
    qtest_writew(qts, SYSCON, 1 << 2);
    qtest_writew(qts, 0x600456, 0xcafe);
    g_assert_cmphex(qtest_readw(qts, 0x600456), ==, 0xcafe);
    qtest_writew(qts, SYSCON, 0);
    g_assert_cmphex(qtest_readw(qts, 0x456), ==, 0xcafe);
    qtest_quit(qts);
}

static void test_external_bus_priority(void)
{
    QTestState *qts = pmb_start();

    /* Distinct physical offsets make the selected window observable. */
    qtest_writew(qts, 0x000020, 0x1111);
    qtest_writew(qts, 0x100020, 0x2222);

    /* CS4 maps 0x100000..0x1fffff to target offset zero. */
    qtest_writew(qts, 0xfe1e, 0x1008);
    qtest_writew(qts, 0xff1a, 1 << 10);
    g_assert_cmphex(qtest_readw(qts, 0x100020), ==, 0x1111);

    /* CS2 has higher ADDRSEL priority and maps the same address at 0x100020. */
    qtest_writew(qts, 0xfe1a, 0x0009);
    qtest_writew(qts, 0xff16, 1 << 10);
    g_assert_cmphex(qtest_readw(qts, 0x100020), ==, 0x2222);

    /* Every active XBUS window wins over programmable chip selects. */
    qtest_writew(qts, 0xf01c, 0x1008);
    qtest_writew(qts, 0xf11c, 1 << 10);
    qtest_writew(qts, 0xf024, 1 << 4);
    qtest_writew(qts, SYSCON, 1 << 2);
    g_assert_cmphex(qtest_readw(qts, 0x100020), ==, 0x1111);

    qtest_writew(qts, SYSCON, 0);
    g_assert_cmphex(qtest_readw(qts, 0x100020), ==, 0x2222);
    qtest_writew(qts, 0xff16, 0);
    g_assert_cmphex(qtest_readw(qts, 0x100020), ==, 0x1111);
    qtest_quit(qts);
}

static void test_external_bus_trace_fallback(void)
{
    g_autofree char *trace_path = g_strdup_printf(
        "/tmp/pmb7850-bus-trace-%u.log", (unsigned)getpid());
    g_autofree char *trace = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;

    unlink(trace_path);
    qts = qtest_initf(
        "-M pmb7850-test -display none "
        "-trace enable=pmb7850_bus -D %s", trace_path);
    qtest_writew(qts, 0xfe18, 0x2009);
    qtest_writew(qts, 0xff14, 1 << 10);
    qtest_writew(qts, 0x200020, 0xa55a);
    g_assert_cmphex(qtest_readw(qts, 0x200020), ==, 0xa55a);
    qtest_writew(qts, 0x800020, 0x5aa5);
    g_assert_cmphex(qtest_readw(qts, 0x800020), ==, 0x5aa5);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, &error));
    g_assert_nonnull(strstr(trace,
        "pmb7850_bus write=1 addr=0x200020 size=2 value=0xa55a "
        "target=chip-select offset=0x20"));
    g_assert_nonnull(strstr(trace,
        "pmb7850_bus write=0 addr=0x200020 size=2 value=0x0 "
        "target=chip-select offset=0x20"));
    g_assert_nonnull(strstr(trace,
        "pmb7850_bus write=1 addr=0x800020 size=2 value=0x5aa5 "
        "target=boot offset=0x800020"));
    unlink(trace_path);
}

static void test_clocked_blocks(void)
{
    QTestState *qts = pmb_start();

    qtest_writew(qts, T3, 0xfffe);
    qtest_writew(qts, T3CON, 1 << 6);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readw(qts, T3IC) & 0x80, ==, 0x80);

    qtest_writew(qts, T0, 0xfffe);
    qtest_writew(qts, T0REL, 0x1234);
    qtest_writew(qts, CC0, 0xffff);
    qtest_writew(qts, CCM0, 5); /* channel 0 compare mode 1, T0 */
    qtest_writew(qts, T01CON, 1 << 6);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readw(qts, CC0IC) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readw(qts, T0IC) & 0x80, ==, 0x80);

    qtest_writew(qts, S0BG, 0);
    qtest_writew(qts, S0CON, 0x8011); /* run, receive, async mode 1 */
    qtest_writew(qts, S0TBUF, 0x5a);
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(qtest_readw(qts, S0TIC) & 0x80, ==, 0x80);

    qtest_writew(qts, SSC0BR, 0);
    qtest_writew(qts, SSC0CON, 0xc207); /* master, enable, receive, 8 bits */
    qtest_writew(qts, SSC0TB, 0xa5);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readw(qts, SSC0TIC) & 0x80, ==, 0x80);
    qtest_quit(qts);
}

static void test_bulk_timer_edges(void)
{
    QTestState *qts = pmb_start();

    qtest_irq_intercept_out_named(qts, "/machine/soc", "capcom-out");
    qtest_writew(qts, T0, 0xfffe);
    qtest_writew(qts, T0REL, 0x1234);
    qtest_writew(qts, CC0, 0xffff);
    qtest_writew(qts, CCM0, 5); /* compare IRQ and output toggle, T0 */
    qtest_writew(qts, T01CON, 1 << 6);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readw(qts, T0), ==, 0x1235);
    g_assert_cmphex(qtest_readw(qts, CC0IC) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readw(qts, T0IC) & 0x80, ==, 0x80);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_writew(qts, TDMA_TOP, 3);
    qtest_writew(qts, TDMA_COUNTER, 0);
    qtest_writew(qts, TDMA_IRQ36, 2);
    qtest_writew(qts, TDMA_IRQ37, 0);
    qtest_writew(qts, TDMA_CONTROL, 1 << 1);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readw(qts, TDMA_COUNTER), ==, 2);
    g_assert_cmphex(qtest_readw(qts, TDMA_IRQ36IC) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readw(qts, TDMA_IRQ37IC) & 0x80, ==, 0);
    qtest_writew(qts, TDMA_IRQ36IC, 0);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readw(qts, TDMA_COUNTER), ==, 0);
    g_assert_cmphex(qtest_readw(qts, TDMA_IRQ37IC) & 0x80, ==, 0x80);
    qtest_quit(qts);
}

static void test_capcom_matches(void)
{
    static const uint16_t values[] = {0, 1, 0x1234, 0xff7f, 0xfffe, 0xffff};
    static const uint64_t counts[] = {0, 1, 2, 127, 128, 129, 65536, 196613};

    for (unsigned v = 0; v < ARRAY_SIZE(values); v++) {
        for (unsigned r = 0; r < ARRAY_SIZE(values); r++) {
            for (unsigned c = 0; c < ARRAY_SIZE(values); c++) {
                for (unsigned n = 0; n < ARRAY_SIZE(counts); n++) {
                    uint16_t counter = values[v];
                    uint64_t matches = 0;
                    bool level = false;
                    uint64_t bulk;

                    for (uint64_t step = 0; step < counts[n]; step++) {
                        counter = counter == 0xffff ? values[r] : counter + 1;
                        if (counter == values[c]) {
                            matches++;
                            level = !level;
                        }
                    }
                    bulk = pmb_capcom_matches(values[v], values[r],
                                              values[c], counts[n]);
                    g_assert_cmpuint(bulk, ==, matches);
                    g_assert_cmpint(bulk & 1, ==, level);
                }
            }
        }
    }
}

/* An unreachable compare must not assert IR after later reload cycles. */
static void test_capcom_unreachable(void)
{
    QTestState *qts = pmb_start();

    qtest_writew(qts, T0, 0xffff);
    qtest_writew(qts, T0REL, 0xffff);
    qtest_writew(qts, CC0, 0xfffe);
    qtest_writew(qts, CCM0, 4);
    qtest_writew(qts, T01CON, 1 << 6);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readw(qts, T0), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, T0IC) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readw(qts, CC0IC) & 0x80, ==, 0);
    qtest_writew(qts, CC0, 0xffff);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readw(qts, CC0IC) & 0x80, ==, 0x80);
    qtest_writew(qts, CC0IC, 0);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readw(qts, CC0IC) & 0x80, ==, 0x80);
    qtest_quit(qts);
}

static void test_capcom_capture_between_callbacks(void)
{
    QTestState *qts = pmb_start();

    qtest_writew(qts, T0, 0xfff0);
    qtest_writew(qts, T0REL, 0xfff0);
    qtest_writew(qts, CCM0, 1); /* Capture on rising edge, T0. */
    qtest_writew(qts, T01CON, 1 << 6);
    /* 13 ticks at 26 MHz, before the 1000 ns periodic callback. */
    qtest_clock_step(qts, 500);
    qtest_set_irq_in(qts, "/machine/soc", "capcom-in", 0, 1);
    g_assert_cmphex(qtest_readw(qts, CC0), ==, 0xfff1);
    g_assert_cmphex(qtest_readw(qts, CC0IC), ==, 0x80);
    qtest_writew(qts, CC0IC, 0);
    qtest_clock_step(qts, 250);
    /* The P6.0 route shares the same capture synchronization contract. */
    qtest_set_irq_in(qts, "/machine/soc", "p6-in", 0, 1);
    g_assert_cmphex(qtest_readw(qts, CC0), ==, 0xfff2);
    g_assert_cmphex(qtest_readw(qts, CC0IC), ==, 0x80);
    qtest_quit(qts);
}

static void test_capcom_output_drive_state(void)
{
    QTestState *qts = pmb_start();

    qtest_irq_intercept_out_named(qts, "/machine/soc", "capcom-driven");
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_writew(qts, CCM0, 5);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writew(qts, CCM0, 4);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_writew(qts, CCM0, 7);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_system_reset(qts);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void test_timer_trace_fallback(void)
{
    g_autofree char *trace_path = g_strdup_printf(
        "/tmp/pmb7850-timer-trace-%u.log", (unsigned)getpid());
    g_autofree char *trace = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    unsigned rows = 0;

    unlink(trace_path);
    qts = qtest_initf(
        "-M pmb7850-test -display none "
        "-trace enable=pmb7850_timer -D %s", trace_path);
    qtest_writew(qts, T3, 0xfffe);
    qtest_writew(qts, T3CON, 1 << 6);
    qtest_clock_step(qts, 1000);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, &error));
    for (const char *row = trace; (row = strstr(row, "pmb7850_timer"));
         row++) {
        rows++;
    }
    /* GPT tracing retains its canonical one-row-per-synchronization shape. */
    g_assert_cmpuint(rows, ==, 1);
    unlink(trace_path);
}

static void test_tdma_irq_trace_bulk(void)
{
    g_autofree char *trace_path = g_strdup_printf(
        "/tmp/pmb7850-tdma-irq-trace-%u.log", (unsigned)getpid());
    g_autofree char *trace = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    unsigned rows = 0;

    unlink(trace_path);
    qts = qtest_initf(
        "-M pmb7850-test -display none "
        "-trace enable=pmb7850_tdma_irq -D %s", trace_path);
    qtest_writew(qts, TDMA_TOP, 3);
    qtest_writew(qts, TDMA_COUNTER, 0);
    qtest_writew(qts, TDMA_IRQ36, 2);
    qtest_writew(qts, TDMA_IRQ37, 0);
    qtest_writew(qts, TDMA_CONTROL, 1 << 1);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readw(qts, TDMA_COUNTER), ==, 0);
    g_assert_cmphex(qtest_readw(qts, TDMA_IRQ36IC) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readw(qts, TDMA_IRQ37IC) & 0x80, ==, 0x80);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, &error));
    for (const char *row = trace; (row = strstr(row, "pmb7850_tdma_irq"));
         row++) {
        rows++;
    }
    g_assert_cmpuint(rows, ==, 2);
    unlink(trace_path);
}

static void test_ssc_master_modes_and_pipeline(void)
{
    QTestState *qts = pmb_start();

    qtest_writew(qts, SSC0BR, 0);
    qtest_writew(qts, SSC0CON, 0xc297); /* receive, loopback, MSB, 8-bit */
    qtest_writew(qts, SSC0TB, 0x00a5);
    g_assert_cmphex(qtest_readw(qts, SSC0CON) & 0x1000, ==, 0x1000);
    g_assert_cmphex(qtest_readw(qts, SSC0TIC) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readw(qts, SSC0RIC) & 0x80, ==, 0);
    qtest_clock_step(qts, 616);
    g_assert_cmphex(qtest_readw(qts, SSC0RIC) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readw(qts, SSC0RB), ==, 0x00a5);
    g_assert_cmphex(qtest_readw(qts, SSC0CON) & 0x1000, ==, 0);

    qtest_writew(qts, SSC0TIC, 0);
    qtest_writew(qts, SSC0RIC, 0);
    qtest_writew(qts, SSC0TB, 0x0012);
    qtest_writew(qts, SSC0TB, 0x0034);
    qtest_writew(qts, SSC0TB, 0x0056);
    g_assert_cmphex(qtest_readw(qts, SSC0CON) & 0x0100, ==, 0x0100);
    g_assert_cmphex(qtest_readw(qts, SSC0EIC) & 0x80, ==, 0x80);
    qtest_writew(qts, SSC0CON, 0xc297); /* clear TE, retain REN config */
    g_assert_cmphex(qtest_readw(qts, SSC0CON) & 0x0300, ==, 0x0200);
    qtest_clock_step(qts, 616);
    g_assert_cmphex(qtest_readw(qts, SSC0RB), ==, 0x0012);
    g_assert_cmphex(qtest_readw(qts, SSC0CON) & 0x1000, ==, 0x1000);
    qtest_clock_step(qts, 616);
    g_assert_cmphex(qtest_readw(qts, SSC0RB), ==, 0x0034);
    g_assert_cmphex(qtest_readw(qts, SSC0CON) & 0x1000, ==, 0);

    qtest_writew(qts, SSC0EIC, 0);
    qtest_writew(qts, SSC0TB, 0x0077);
    qtest_clock_step(qts, 616);
    qtest_writew(qts, SSC0TB, 0x0088); /* leave 0x77 unread */
    qtest_clock_step(qts, 616);
    g_assert_cmphex(qtest_readw(qts, SSC0EIC) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readw(qts, SSC0RB), ==, 0x0088);

    qtest_writew(qts, SSC0CON, 0);       /* disable before reconfigure */
    qtest_writew(qts, SSC0CON, 0xc083);  /* loopback, LSB, 4-bit */
    qtest_writew(qts, SSC0TB, 0x00ad);
    qtest_clock_step(qts, 308);
    g_assert_cmphex(qtest_readw(qts, SSC0RB), ==, 0x000d);

    qtest_writew(qts, SSC0CON, 0);
    qtest_writew(qts, SSC0CON, 0xc09f);  /* loopback, MSB, 16-bit */
    qtest_writew(qts, SSC0TB, 0xa55a);
    qtest_clock_step(qts, 1231);
    g_assert_cmphex(qtest_readw(qts, SSC0RB), ==, 0xa55a);
    qtest_quit(qts);
}

static void test_ssc_unselected_receive(void)
{
    QTestState *qts = pmb_start();
    const unsigned widths[] = { 2, 4, 8, 12, 16 };

    /* Existing boards retain the SSI bus's default zero response. */
    qtest_writew(qts, SSC0CON, 0xc017);
    qtest_writew(qts, SSC0TB, 0xa5);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readw(qts, SSC0RB), ==, 0);
    qtest_quit(qts);

    qts = qtest_init("-M pmb7850-test -display none "
                    "-global pmb7850-soc.ssc0-unselected-rx-high=on "
                    "-device pcf8813,id=reply,bus=ssc0");
    qtest_set_irq_in(qts, "/machine/peripheral/reply", "ssi-gpio-cs", 0, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(widths); i++) {
        unsigned bits = widths[i];

        qtest_writew(qts, SSC0CON, 0);
        qtest_writew(qts, SSC0CON, 0xc010 | (bits - 1));
        qtest_writew(qts, SSC0TB, 0);
        g_assert_cmphex(qtest_readw(qts, SSC0RIC) & 0x80, ==, 0);
        qtest_clock_step(qts, 2000);
        g_assert_cmphex(qtest_readw(qts, SSC0RB), ==, (1u << bits) - 1);
        g_assert_cmphex(qtest_readw(qts, SSC0RIC) & 0x80, ==, 0x80);
        qtest_writew(qts, SSC0RIC, 0);
    }

    /*
     * The unconnected frame-start input leaves this slave at eight bits.
     * Preserve its selected 0x00ff reply, including the zero upper bits,
     * rather than replacing it with the master's 16-bit idle mask.
     */
    qtest_set_irq_in(qts, "/machine/peripheral/reply", "ssi-gpio-cs", 0, 0);
    qtest_writew(qts, SSC0CON, 0);
    qtest_writew(qts, SSC0CON, 0xc01f);
    qtest_writew(qts, SSC0TB, 0xa5);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readw(qts, SSC0RB), ==, 0xff);

    /* Loopback owns RX even with the board input calibrated high. */
    qtest_set_irq_in(qts, "/machine/peripheral/reply", "ssi-gpio-cs", 0, 1);
    qtest_writew(qts, SSC0CON, 0);
    qtest_writew(qts, SSC0CON, 0xc097);
    qtest_writew(qts, SSC0TB, 0);
    qtest_writew(qts, SSC0TB, 0xa5);
    qtest_clock_step(qts, 616);
    g_assert_cmphex(qtest_readw(qts, SSC0RB), ==, 0);
    qtest_clock_step(qts, 616);
    g_assert_cmphex(qtest_readw(qts, SSC0RB), ==, 0xa5);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/pmb7850/reset-register-map", test_reset_and_register_map);
    qtest_add_func("/pmb7850/memory-ownership", test_memory_ownership);
    qtest_add_func("/pmb7850/ports-interrupts",
                   test_ports_and_interrupt_registers);
    qtest_add_func("/pmb7850/eop-subnodes", test_eop_subnodes);
    qtest_add_func("/pmb7850/external-bus", test_external_bus_windows);
    qtest_add_func("/pmb7850/external-bus-priority",
                   test_external_bus_priority);
    qtest_add_func("/pmb7850/external-bus-trace-fallback",
                   test_external_bus_trace_fallback);
    qtest_add_func("/pmb7850/clocked-blocks", test_clocked_blocks);
    qtest_add_func("/pmb7850/bulk-timer-edges", test_bulk_timer_edges);
    qtest_add_func("/pmb7850/capcom-matches", test_capcom_matches);
    qtest_add_func("/pmb7850/capcom-unreachable", test_capcom_unreachable);
    qtest_add_func("/pmb7850/capcom-capture-between-callbacks",
                   test_capcom_capture_between_callbacks);
    qtest_add_func("/pmb7850/capcom-output-drive-state",
                   test_capcom_output_drive_state);
    qtest_add_func("/pmb7850/timer-trace-fallback",
                   test_timer_trace_fallback);
    qtest_add_func("/pmb7850/tdma-irq-trace-bulk",
                   test_tdma_irq_trace_bulk);
    qtest_add_func("/pmb7850/ssc-master-pipeline",
                   test_ssc_master_modes_and_pipeline);
    qtest_add_func("/pmb7850/ssc-unselected-receive",
                   test_ssc_unselected_receive);
    qtest_add_func("/pmb7850/sim/default-off-atr",
                   test_sim_default_off_and_atr);
    qtest_add_func("/pmb7850/sim/blocked-event-resumption",
                   test_sim_blocked_event_resumption);
    qtest_add_func("/pmb7850/sim/select-get-response",
                   test_sim_select_get_response);
    qtest_add_func("/pmb7850/sim/status-profile",
                   test_sim_status_profile);
    qtest_add_func("/pmb7850/sim/update-read-binary",
                   test_sim_update_read_binary);
    return g_test_run();
}
