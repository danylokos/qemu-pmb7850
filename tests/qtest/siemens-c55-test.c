/*
 * Siemens C55 machine and M58LW064D qtests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "libqtest.h"

#define FLASH_NATIVE 0x800000
#define FLASH_BLOCK  0x020000
#define CLOCK_PERIOD_1SEC (1000000000ULL << 32)
#define CLOCK_PERIOD_FROM_HZ(hz) (CLOCK_PERIOD_1SEC / (hz))
#define ADC_COMMAND 0xe062
#define ADC_RESULT_HI 0xef2c
#define ADC_RESULT_LO 0xef2e
#define SSC0TB  0xf0b0
#define SSC0BR  0xf0b4
#define SSC0CON 0xffb2
#define P3      0xffc4
#define DP3     0xffc6
#define P7      0xffd0
#define DP7     0xffd2

static QTestState *c55_start(const char *extra)
{
    return qtest_initf("-M siemens-c55 -display none -serial null "
                       "-global m58lw064d.factory-uid="
                       "000000004079A7E9 %s", extra ?: "");
}

static uint64_t qom_get_u64(QTestState *qts, const char *path,
                            const char *property)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':%s}}", path, property);
    uint64_t value;

    g_assert_false(qdict_haskey(response, "error"));
    value = qdict_get_uint(response, "return");
    qobject_unref(response);
    return value;
}

static void flash_array_mode(QTestState *qts)
{
    qtest_writeb(qts, FLASH_NATIVE, 0xff);
}

static void flash_program_byte(QTestState *qts, uint32_t offset,
                               uint8_t value)
{
    qtest_writeb(qts, FLASH_NATIVE + offset, 0x40);
    qtest_writeb(qts, FLASH_NATIVE + offset, value);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0x0080);
    flash_array_mode(qts);
}

static void test_machine_flash_views_and_signature(void)
{
    QTestState *qts = c55_start(NULL);

    qtest_writeb(qts, 0, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0xffff);

    qtest_writeb(qts, FLASH_NATIVE, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0x0020);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 2), ==, 0x0017);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 4), ==, 0x0000);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x100), ==, 0x00fe);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x102), ==, 0x0000);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x104), ==, 0x0000);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x106), ==, 0x7940);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x108), ==, 0xe9a7);
    g_assert_cmphex(qtest_readw(qts, 0), ==, 0xffff);

    qtest_writeb(qts, FLASH_NATIVE, 0x98);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x20), ==, 'Q');
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x22), ==, 'R');
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x24), ==, 'Y');
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x4e), ==, 0x17);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x58), ==, 1);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x5a), ==, 63);
    qtest_quit(qts);
}

static void test_machine_cpu_and_clock(void)
{
    QTestState *qts = c55_start(NULL);
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-list','arguments':{'path':'/machine/soc'}}");
    QList *properties = qdict_get_qlist(response, "return");
    QListEntry *entry;
    bool found_cpu = false;

    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *property = qobject_to(QDict, qlist_entry_obj(entry));

        if (!strcmp(qdict_get_str(property, "name"), "cpu")) {
            g_assert_cmpstr(qdict_get_str(property, "type"), ==,
                            "child<c166s-v1-c166-cpu>");
            found_cpu = true;
        }
    }
    g_assert_true(found_cpu);
    qobject_unref(response);

    response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/sysclk',"
        "'property':'qtest-clock-period'}}");
    g_assert_false(qdict_haskey(response, "error"));
    g_assert_cmpuint(qdict_get_int(response, "return"), ==,
                     CLOCK_PERIOD_FROM_HZ(26000000));
    qobject_unref(response);
    qtest_quit(qts);
}

static void test_flash_program_status_and_reset(void)
{
    QTestState *qts = c55_start(NULL);

    flash_program_byte(qts, 0x100, 0x0f);
    g_assert_cmphex(qtest_readb(qts, 0x100), ==, 0x0f);
    g_assert_cmphex(qtest_readb(qts, FLASH_NATIVE + 0x100), ==, 0x0f);

    qtest_writeb(qts, FLASH_NATIVE + 0x100, 0x10);
    qtest_writeb(qts, FLASH_NATIVE + 0x100, 0xff);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0x0090);
    qtest_writeb(qts, FLASH_NATIVE, 0x50);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0x0080);
    flash_array_mode(qts);
    g_assert_cmphex(qtest_readb(qts, FLASH_NATIVE + 0x100), ==, 0x0f);

    qtest_writeb(qts, FLASH_NATIVE + 0x110, 0x40);
    qtest_writew(qts, FLASH_NATIVE + 0x110, 0xa55a);
    flash_array_mode(qts);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x110), ==, 0xa55a);

    qtest_writeb(qts, FLASH_NATIVE + 0x110, 0x40);
    qtest_writew(qts, FLASH_NATIVE + 0x110, 0xffff);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0x0090);
    qtest_writeb(qts, FLASH_NATIVE, 0x50);
    flash_array_mode(qts);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x110), ==, 0xa55a);

    qtest_writeb(qts, FLASH_NATIVE, 0x90);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, FLASH_NATIVE + 0x100), ==, 0x0f);
    qtest_quit(qts);
}

static void test_startup_power_hold(void)
{
    QTestState *qts = c55_start(NULL);

    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0);
    qtest_writew(qts, 0xef1c, 0x003d);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000d);
    qtest_writew(qts, 0xfd12, 0x0100);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000d);
    qtest_writew(qts, 0xef1c, 0x003d);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000f);
    qtest_writew(qts, 0xfd12, 0);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000f);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0);
    qtest_quit(qts);
}

static void input_key(QTestState *qts, const char *qcode, bool down)
{
    QDict *response;

    if (down) {
        response = qtest_qmp(qts,
            "{'execute':'input-send-event','arguments':{'events':["
            "{'type':'key','data':{'down':true,'key':"
            "{'type':'qcode','data':%s}}}]}}", qcode);
    } else {
        response = qtest_qmp(qts,
            "{'execute':'input-send-event','arguments':{'events':["
            "{'type':'key','data':{'down':false,'key':"
            "{'type':'qcode','data':%s}}}]}}", qcode);
    }

    g_assert_false(qdict_haskey(response, "error"));
    qobject_unref(response);
}

static void test_runtime_keypad_soft_left(void)
{
    QTestState *qts = c55_start(NULL);

    qtest_writew(qts, 0xfd12, 0x0100);
    input_key(qts, "f1", true);
    g_assert_cmphex(qtest_readw(qts, 0xf16e) & 0x80, ==, 0x80);
    qtest_writew(qts, 0xef1c, 0x00ed);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000e);
    qtest_writew(qts, 0xf16e, 0);
    input_key(qts, "f1", false);
    g_assert_cmphex(qtest_readw(qts, 0xf16e) & 0x80, ==, 0x80);
    qtest_writew(qts, 0xef1c, 0x00ed);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000f);
    qtest_quit(qts);
}

static uint32_t qom_get_u32(QTestState *qts, const char *path,
                            const char *property)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':%s}}", path, property);
    uint32_t value;

    g_assert_false(qdict_haskey(response, "error"));
    value = qdict_get_int(response, "return");
    qobject_unref(response);
    return value;
}

static void qom_set_u32(QTestState *qts, const char *path,
                        const char *property, uint32_t value)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':%s,'value':%u}}", path, property, value);

    g_assert_false(qdict_haskey(response, "error"));
    qobject_unref(response);
}

static void test_supervised_keypad_properties(void)
{
    QTestState *qts = c55_start(NULL);
    QDict *response;
    const uint32_t soft_left = 1u << 14;

    qtest_writew(qts, 0xfd12, 0x0100);
    qom_set_u32(qts, "/machine/keypad", "ui-buttons", soft_left);
    g_assert_cmphex(qom_get_u32(qts, "/machine/keypad", "ui-buttons"),
                    ==, soft_left);
    qtest_writew(qts, 0xef1c, 0x00ed);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000e);
    g_assert_cmphex(qom_get_u32(qts, "/machine/keypad", "sampled-buttons"),
                    ==, soft_left);

    input_key(qts, "f1", true);
    qom_set_u32(qts, "/machine/keypad", "ui-buttons", 0);
    qtest_writew(qts, 0xef1c, 0x00ed);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000e);
    input_key(qts, "f1", false);
    qtest_writew(qts, 0xef1c, 0x00ed);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000f);

    response = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/keypad',"
        "'property':'ui-buttons','value':262144}}");
    g_assert_true(qdict_haskey(response, "error"));
    qobject_unref(response);

    qom_set_u32(qts, "/machine/keypad", "ui-buttons", soft_left);
    qtest_system_reset(qts);
    g_assert_cmphex(qom_get_u32(qts, "/machine/keypad", "ui-buttons"),
                    ==, 0);
    g_assert_cmphex(qom_get_u32(qts, "/machine/keypad", "sampled-buttons"),
                    ==, 0);
    qtest_quit(qts);
}

static void test_supervised_statistics_and_display(void)
{
    QTestState *qts = c55_start(NULL);
    QDict *response;

    g_assert_cmpuint(qom_get_u32(qts, "/machine", "pc"), ==, 0);
    response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'guest-instructions'}}");
    g_assert_false(qdict_haskey(response, "error"));
    g_assert_cmpuint(qdict_get_int(response, "return"), ==, 0);
    qobject_unref(response);
    response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'timing-ticks'}}");
    g_assert_false(qdict_haskey(response, "error"));
    g_assert_cmpuint(qdict_get_int(response, "return"), ==, 0);
    qobject_unref(response);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "scale"), ==, 1);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "panel-width"),
                     ==, 101);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "panel-height"),
                     ==, 64);
    qtest_quit(qts);
}

static void xbus_charger_query(QTestState *qts)
{
    qtest_writew(qts, 0xec10, 0x1704);
    qtest_writew(qts, 0xec14, 2);
    qtest_writew(qts, 0xec80, 0x0162);
    qtest_writew(qts, 0xec82, 2);
    qtest_writew(qts, 0xec84, 0x0163);
    qtest_writew(qts, 0xec12, 1);
    qtest_clock_step(qts, 32);
}

static void test_battery_and_charger_properties(void)
{
    QTestState *qts = c55_start(NULL);
    QDict *response;

    qtest_writew(qts, ADC_COMMAND, 0x0801);
    g_assert_cmphex(qtest_readw(qts, ADC_RESULT_HI), ==, 0);
    g_assert_cmphex(qtest_readw(qts, ADC_RESULT_LO), ==, 0xf91d);

    response = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'battery-level','value':50}}");
    g_assert_false(qdict_haskey(response, "error"));
    qobject_unref(response);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/battery", "level"), ==, 50);
    qtest_writew(qts, ADC_COMMAND, 0x0801);
    g_assert_cmphex(qtest_readw(qts, ADC_RESULT_LO), ==, 0x0145);
    qtest_writew(qts, ADC_COMMAND, 0x0803);
    g_assert_cmphex(qtest_readw(qts, ADC_RESULT_LO), ==, 0);

    response = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'battery-level','value':101}}");
    g_assert_true(qdict_haskey(response, "error"));
    qobject_unref(response);

    response = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'battery-charging','value':true}}");
    g_assert_false(qdict_haskey(response, "error"));
    qobject_unref(response);
    xbus_charger_query(qts);
    g_assert_cmphex(qtest_readw(qts, 0xec16), ==, 0x0010);

    qtest_system_reset(qts);
    g_assert_cmpuint(qom_get_u32(qts, "/machine", "battery-level"), ==, 50);
    qtest_writew(qts, ADC_COMMAND, 0x0801);
    g_assert_cmphex(qtest_readw(qts, ADC_RESULT_LO), ==, 0x0145);
    xbus_charger_query(qts);
    g_assert_cmphex(qtest_readw(qts, 0xec16), ==, 0x0010);
    qtest_quit(qts);
}

static void ssc_send(QTestState *qts, uint16_t value)
{
    qtest_writew(qts, SSC0TB, value);
    qtest_clock_step(qts, 100);
}

static void test_display_wiring_and_frame(void)
{
    QTestState *qts = c55_start(NULL);
    QDict *response;
    uint64_t memory_before = qom_get_u64(qts, "/machine/display",
                                         "memory-digest");

    qtest_writew(qts, DP3, 1 << 11);
    qtest_writew(qts, DP7, (1 << 5) | (1 << 6) | (1 << 7));
    qtest_writew(qts, P3, 1 << 11); /* SCE inactive */
    qtest_writew(qts, SSC0BR, 0);
    qtest_writew(qts, SSC0CON, 0xc0d7); /* master, MSB first, 8-bit */
    qtest_writew(qts, P3, 0);       /* SCE active */
    qtest_writew(qts, P7, 0);       /* command */
    ssc_send(qts, 0x20);            /* normal instruction set, powered */
    ssc_send(qts, 0x0c);            /* normal display */
    qtest_writew(qts, P7, 1 << 6);  /* data */
    ssc_send(qts, 0xff);
    qtest_writew(qts, P3, 1 << 11); /* close raw transaction, keep sweep */

    response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/display',"
        "'property':'frame-count'}}");
    g_assert_false(qdict_haskey(response, "error"));
    g_assert_cmpuint(qdict_get_int(response, "return"), ==, 0);
    qobject_unref(response);

    qtest_writew(qts, P3, 0);       /* exercise partial-byte SSI framing */
    qtest_writew(qts, SSC0CON, 0);
    qtest_writew(qts, SSC0CON, 0xc0d3); /* master, MSB first, 4-bit */
    qtest_writew(qts, P7, 0);
    ssc_send(qts, 0x2);
    ssc_send(qts, 0x0);             /* function set 0x20 */
    ssc_send(qts, 0x0);
    ssc_send(qts, 0xc);             /* display control 0x0c */
    qtest_writew(qts, P7, 1 << 6);
    ssc_send(qts, 0xf);
    ssc_send(qts, 0xf);             /* data 0xff */
    qtest_writew(qts, P3, 1 << 11);

    response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/display',"
        "'property':'frame-count'}}");
    g_assert_false(qdict_haskey(response, "error"));
    g_assert_cmpuint(qdict_get_int(response, "return"), ==, 0);
    qobject_unref(response);
    g_assert_cmphex(qom_get_u64(qts, "/machine/display", "memory-digest"),
                    !=, memory_before);
    qtest_quit(qts);
}

static QTestState *pcf8813_sweep_start(void)
{
    QTestState *qts = c55_start(NULL);

    qtest_writew(qts, DP3, 1 << 11);
    qtest_writew(qts, DP7, (1 << 5) | (1 << 6) | (1 << 7));
    qtest_writew(qts, P3, 1 << 11);
    qtest_writew(qts, SSC0BR, 0);
    qtest_writew(qts, SSC0CON, 0xc0d7);
    qtest_writew(qts, P3, 0);
    qtest_writew(qts, P7, 0);
    ssc_send(qts, 0x20);
    ssc_send(qts, 0x0c);
    qtest_writew(qts, P3, 1 << 11);
    return qts;
}

static void pcf8813_start_bank(QTestState *qts, unsigned x, unsigned bank)
{
    qtest_writew(qts, P3, 0);
    qtest_writew(qts, P7, 0);
    ssc_send(qts, 0x40 | bank);
    ssc_send(qts, 0x80 | x);
    qtest_writew(qts, P7, 1 << 6);
}

static void pcf8813_write_bank(QTestState *qts, unsigned x, unsigned bank,
                               unsigned bytes, uint8_t value)
{
    pcf8813_start_bank(qts, x, bank);
    for (unsigned i = 0; i < bytes; i++) {
        ssc_send(qts, value);
    }
    qtest_writew(qts, P3, 1 << 11);
}

static void test_pcf8813_atomic_sweep(void)
{
    QTestState *qts = pcf8813_sweep_start();
    uint64_t initial_memory = qom_get_u64(qts, "/machine/display",
                                          "memory-digest");
    uint64_t initial_frame = qom_get_u64(qts, "/machine/display",
                                         "frame-digest");

    for (unsigned bank = 0; bank < 7; bank++) {
        pcf8813_write_bank(qts, 1, bank, 102, 0xff);
        g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                         ==, 0);
    }
    g_assert_cmphex(qom_get_u64(qts, "/machine/display", "memory-digest"),
                    !=, initial_memory);
    g_assert_cmphex(qom_get_u64(qts, "/machine/display", "frame-digest"),
                    ==, initial_frame);
    pcf8813_write_bank(qts, 1, 7, 102, 0xff);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 1);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display",
                                 "transaction-count"), ==, 8);
    qtest_quit(qts);
}

static void test_pcf8813_sweep_fallbacks(void)
{
    QTestState *qts = pcf8813_sweep_start();

    pcf8813_write_bank(qts, 0, 0, 102, 0x11);
    pcf8813_start_bank(qts, 1, 1);
    ssc_send(qts, 0x22);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 1);
    qtest_writew(qts, P3, 1 << 11);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 1);

    pcf8813_write_bank(qts, 0, 0, 102, 0x33);
    pcf8813_write_bank(qts, 0, 1, 101, 0x44);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 2);

    pcf8813_write_bank(qts, 0, 0, 102, 0x55);
    pcf8813_start_bank(qts, 0, 0);
    ssc_send(qts, 0x66);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 4);
    for (unsigned i = 1; i < 102; i++) {
        ssc_send(qts, 0x66);
    }
    qtest_writew(qts, P3, 1 << 11);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 4);

    qtest_system_reset(qts);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 4);
    qtest_writew(qts, DP3, 1 << 11);
    qtest_writew(qts, DP7, (1 << 5) | (1 << 6) | (1 << 7));
    qtest_writew(qts, P3, 1 << 11);
    qtest_writew(qts, SSC0BR, 0);
    qtest_writew(qts, SSC0CON, 0xc0d7);
    pcf8813_write_bank(qts, 0, 1, 102, 0x77);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 4);
    qtest_quit(qts);
}

static void test_pcf8813_fragmented_sweep(void)
{
    QTestState *qts = pcf8813_sweep_start();
    uint64_t blank = qom_get_u64(qts, "/machine/display",
                                  "presentation-digest");
    uint64_t complete;
    uint64_t updates;

    /* Split every bank into three CS transactions, keeping its address. */
    for (unsigned bank = 0; bank < 8; bank++) {
        pcf8813_start_bank(qts, 0, bank);
        for (unsigned x = 0; x < 102; x++) {
            ssc_send(qts, 0xff);
            g_assert_cmpuint(qom_get_u32(qts, "/machine/display",
                                         "frame-count"),
                             ==, bank == 7 && x == 101);
            if (bank != 7 || x != 101) {
                g_assert_cmphex(qom_get_u64(qts, "/machine/display",
                                            "presentation-digest"), ==, blank);
            }
            if (x == 33 || x == 67) {
                qtest_writew(qts, P3, 1 << 11);
                qtest_writew(qts, P3, 0);
            }
        }
        qtest_writew(qts, P3, 1 << 11);
    }
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display",
                                 "transaction-count"), ==, 24);
    complete = qom_get_u64(qts, "/machine/display", "presentation-digest");
    g_assert_cmphex(complete, !=, blank);

    pcf8813_write_bank(qts, 1, 0, 1, 0);
    g_assert_cmphex(qom_get_u64(qts, "/machine/display",
                                "presentation-digest"), ==, complete);
    qtest_writew(qts, P3, 0);
    qtest_writew(qts, P7, 0);
    updates = qom_get_u64(qts, "/machine/display", "display-update-count");
    ssc_send(qts, 0x0d); /* Invert the presented image, not pending RAM. */
    g_assert_cmpuint(qom_get_u64(qts, "/machine/display",
                                 "display-update-count"), ==, updates + 1);
    ssc_send(qts, 0x0d);
    g_assert_cmpuint(qom_get_u64(qts, "/machine/display",
                                 "display-update-count"), ==, updates + 1);
    g_assert_cmphex(qom_get_u64(qts, "/machine/display",
                                "presentation-digest"), !=, complete);
    ssc_send(qts, 0x0c);
    ssc_send(qts, 0x12); /* Power control is immediate too. */
    g_assert_cmphex(qom_get_u64(qts, "/machine/display",
                                "presentation-digest"), ==, blank);
    ssc_send(qts, 0x13);
    g_assert_cmphex(qom_get_u64(qts, "/machine/display",
                                "presentation-digest"), ==, complete);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 1);

    /* A real addressing-mode change publishes before changing the mode. */
    ssc_send(qts, 0x22);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 2);
    g_assert_cmphex(qom_get_u64(qts, "/machine/display",
                                "presentation-digest"), !=, complete);
    pcf8813_write_bank(qts, 1, 0, 1, 0xff);
    qtest_system_reset(qts);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 2);
    qtest_quit(qts);
}

static void test_flash_erase_and_write_buffer(void)
{
    QTestState *qts = c55_start(NULL);
    uint32_t first = FLASH_BLOCK + 0x20;
    uint32_t second = 2 * FLASH_BLOCK + 0x20;

    flash_program_byte(qts, first, 0x12);
    flash_program_byte(qts, second, 0x34);
    qtest_writeb(qts, FLASH_NATIVE + first, 0x20);
    qtest_writeb(qts, FLASH_NATIVE + first, 0xd0);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0x0080);
    flash_array_mode(qts);
    g_assert_cmphex(qtest_readb(qts, FLASH_NATIVE + first), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, FLASH_NATIVE + second), ==, 0x34);

    qtest_writeb(qts, FLASH_NATIVE + 0x200, 0xe8);
    qtest_writew(qts, FLASH_NATIVE + 0x200, 2);
    qtest_writew(qts, FLASH_NATIVE + 0x200, 0x1234);
    qtest_writew(qts, FLASH_NATIVE + 0x202, 0x5678);
    qtest_writew(qts, FLASH_NATIVE + 0x204, 0x9abc);
    qtest_writew(qts, FLASH_NATIVE + 0x204, 0x00d0);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0x0080);
    flash_array_mode(qts);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x200), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x202), ==, 0x5678);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x204), ==, 0x9abc);
    qtest_quit(qts);
}

static void test_sram_folding_reset_seed_and_pin(void)
{
    QTestState *qts = c55_start(NULL);

    g_assert_cmphex(qtest_readw(qts, 0xf13c), ==, 0x0002);
    g_assert_cmphex(qtest_readw(qts, 0xffd0) & 8, ==, 8);

    /* Canonical firmware geometry: 2 MiB CS2 window, 512 KiB SRAM folded. */
    qtest_writew(qts, 0xfe1a, 0x0009);
    qtest_writew(qts, 0xff16, 1 << 10);
    qtest_writew(qts, 0x100020, 0x5aa5);
    g_assert_cmphex(qtest_readw(qts, 0x000020), ==, 0x5aa5);
    g_assert_cmphex(qtest_readw(qts, 0x080020), ==, 0x5aa5);
    g_assert_cmphex(qtest_readw(qts, 0x180020), ==, 0x5aa5);
    qtest_writew(qts, 0x180022, 0xc33c);
    g_assert_cmphex(qtest_readw(qts, 0x100022), ==, 0xc33c);

    qtest_set_irq_in(qts, "/machine/soc", "p7-in", 3, 0);
    g_assert_cmphex(qtest_readw(qts, 0xffd0) & 8, ==, 0);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readw(qts, 0xf13c), ==, 0x0002);
    g_assert_cmphex(qtest_readw(qts, 0xffd0) & 8, ==, 0);
    qtest_quit(qts);
}

static void test_product_xbus_contract(void)
{
    QTestState *qts = c55_start(NULL);

    g_assert_cmphex(qtest_readw(qts, 0xe800), ==, 0x1202);
    qtest_writew(qts, 0xe806, 0x0055);
    qtest_writeb(qts, 0xef3a, 1);
    g_assert_cmphex(qtest_readw(qts, 0xe806), ==, 0x8055);
    qtest_writeb(qts, 0xe806, 0xaa);
    g_assert_cmphex(qtest_readw(qts, 0xe806), ==, 0x00aa);

    qtest_writew(qts, 0xec10, 0x1704);
    qtest_writew(qts, 0xf140, 0x0051);
    qtest_writew(qts, 0xec14, 2);
    qtest_writew(qts, 0xec80, 0x0162);
    qtest_writew(qts, 0xec82, 2);
    qtest_writew(qts, 0xec84, 0x0163);
    qtest_writew(qts, 0xec12, 1);
    g_assert_cmphex(qtest_readw(qts, 0xec12), ==, 2);
    g_assert_cmphex(qtest_readw(qts, 0xec16), ==, 0);
    g_assert_cmphex(qtest_readw(qts, 0xf140), ==, 0x0051);
    qtest_clock_step(qts, 31);
    g_assert_cmphex(qtest_readw(qts, 0xec12), ==, 2);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readw(qts, 0xec12), ==, 4);
    g_assert_cmphex(qtest_readw(qts, 0xf140), ==, 0x00d1);

    qtest_writew(qts, 0xf140, 0x0051);
    qtest_writew(qts, 0xec82, 0);
    qtest_writew(qts, 0xec12, 1);
    g_assert_cmphex(qtest_readw(qts, 0xec12), ==, 2);
    g_assert_cmphex(qtest_readw(qts, 0xec16), ==, 1);
    qtest_writeb(qts, 0xec12, 0);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readw(qts, 0xec12), ==, 0);
    g_assert_cmphex(qtest_readw(qts, 0xf140), ==, 0x0051);
    qtest_quit(qts);
}

static void test_xbus_completion_mode(const void *opaque)
{
    uint16_t irq_mode = GPOINTER_TO_UINT(opaque);
    QTestState *qts = c55_start(NULL);
    const uint64_t delays[] = { 31, 1, 1, 1967, 1000000 };

    /* S55's transaction 16, with acknowledgement delayed past preemption. */
    qtest_writew(qts, 0xec10, 0x1700);
    qtest_writew(qts, 0xf140, 0x0051);
    qtest_writew(qts, 0xec14, 2);
    qtest_writew(qts, 0xec80, 0x0162);
    qtest_writew(qts, 0xec82, 0x000a);
    qtest_writew(qts, 0xec84, 8);
    qtest_writew(qts, 0xec12, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(delays); i++) {
        qtest_clock_step(qts, delays[i]);
        g_assert_cmphex(qtest_readw(qts, 0xec12), ==, 2);
        g_assert_cmphex(qtest_readw(qts, 0xec16), ==, 0x10);
        g_assert_cmphex(qtest_readw(qts, 0xf140), ==, 0x51);
    }

    /* Mode is sampled, not live. Overlap cannot replace the active result. */
    qtest_writeb(qts, 0xec10, irq_mode);
    qtest_writew(qts, 0xec82, 0);
    qtest_writew(qts, 0xec84, 0x0163);
    qtest_writeb(qts, 0xec12, 3);
    qtest_writeb(qts, 0xec13, 0x80);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readw(qts, 0xec12), ==, 0x8002);
    g_assert_cmphex(qtest_readw(qts, 0xec16), ==, 0x10);
    g_assert_cmphex(qtest_readw(qts, 0xf140), ==, 0x51);

    /* Same write acknowledges and submits the next request, now queued. */
    qtest_writew(qts, 0xf140, 0x0011); /* IE masked, IR must still latch. */
    qtest_writew(qts, 0xec12, 1);
    g_assert_cmphex(qtest_readw(qts, 0xec16), ==, 1);
    qtest_writew(qts, 0xec10, 0x1700); /* Cannot cancel the latched IRQ mode. */
    qtest_clock_step(qts, 31);
    g_assert_cmphex(qtest_readw(qts, 0xec12), ==, 2);
    g_assert_cmphex(qtest_readw(qts, 0xf140), ==, 0x11);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readw(qts, 0xec12), ==, 4);
    g_assert_cmphex(qtest_readw(qts, 0xf140), ==, 0x91);

    /* A synchronous acknowledgement must preserve an unrelated pending IR. */
    qtest_writew(qts, 0xec12, 1);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readw(qts, 0xec12), ==, 2);
    qtest_writeb(qts, 0xec12, 0);
    g_assert_cmphex(qtest_readw(qts, 0xf140), ==, 0x91);
    qtest_writew(qts, 0xf140, 0x0051);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readw(qts, 0xec12), ==, 0);
    g_assert_cmphex(qtest_readw(qts, 0xf140), ==, 0x51);

    /* Early guest acknowledgement and reset both cancel queued delivery. */
    qtest_writew(qts, 0xec10, 0x1700 | irq_mode);
    qtest_writew(qts, 0xec12, 1);
    qtest_writeb(qts, 0xec12, 0);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readw(qts, 0xf140), ==, 0x51);
    qtest_writew(qts, 0xec12, 1);
    qtest_system_reset(qts);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readw(qts, 0xec12), ==, 0);
    g_assert_cmphex(qtest_readw(qts, 0xf140) & 0x80, ==, 0);
    qtest_quit(qts);
}

static void test_asc0_attachment(void)
{
    QTestState *qts = qtest_init(
        "-M siemens-c55 -display none "
        "-chardev null,id=asc0 -serial chardev:asc0");
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/soc',"
        "'property':'serial0'}}");

    g_assert_cmpstr(qdict_get_str(response, "return"), ==, "asc0");
    qobject_unref(response);
    qtest_quit(qts);
}

static void fill_file(int fd, uint8_t value, size_t size)
{
    g_autofree uint8_t *buffer = g_malloc0(64 * KiB);

    memset(buffer, value, 64 * KiB);
    while (size) {
        size_t chunk = MIN(size, 64 * KiB);

        g_assert_cmpint(write(fd, buffer, chunk), ==, chunk);
        size -= chunk;
    }
    g_assert_cmpint(lseek(fd, 0, SEEK_SET), ==, 0);
}

static void assert_qemu_rejects(const char *const arguments[])
{
    g_auto(GStrv) argv = NULL;
    g_autofree char *stdout_text = NULL;
    g_autofree char *stderr_text = NULL;
    GError *error = NULL;
    size_t count = 0;
    int status;

    while (arguments[count]) {
        count++;
    }
    argv = g_new0(char *, count + 1);
    for (size_t i = 0; i < count; i++) {
        argv[i] = g_strdup(arguments[i]);
    }

    g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
                               NULL, &stdout_text, &stderr_text, &status,
                               &error));
    g_assert_no_error(error);
    g_assert_false(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_flash_property_validation(void)
{
    const char *qemu = qtest_qemu_binary(NULL);
    g_autofree char *path = NULL;
    g_autofree char *drive = NULL;
    int fd = g_file_open_tmp("c55-m58-invalid-XXXXXX", &path, NULL);
    const char *const bad_uid_argv[] = {
        qemu, "-M", "siemens-c55", "-display", "none",
        "-global", "m58lw064d.factory-uid=not-a-uid", NULL,
    };
    const char *bad_size_argv[8] = {
        qemu, "-M", "siemens-c55", "-display", "none", "-drive",
    };

    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, 4 * MiB);
    close(fd);
    drive = g_strdup_printf("if=pflash,format=raw,file=%s", path);
    bad_size_argv[6] = drive;
    assert_qemu_rejects(bad_uid_argv);
    assert_qemu_rejects(bad_size_argv);
    unlink(path);
}

static void test_flash_backend_persistence(void)
{
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    int fd = g_file_open_tmp("c55-m58-XXXXXX", &path, NULL);
    uint8_t value;
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, 8 * MiB);
    close(fd);
    args = g_strdup_printf("-drive if=pflash,format=raw,file=%s", path);
    qts = c55_start(args);
    flash_program_byte(qts, 0x300, 0x5a);
    qtest_quit(qts);
    fd = open(path, O_RDONLY);
    g_assert_cmpint(pread(fd, &value, 1, 0x300), ==, 1);
    g_assert_cmphex(value, ==, 0x5a);
    close(fd);
    unlink(path);
}

static void test_flash_snapshot_preserves_base(void)
{
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    int fd = g_file_open_tmp("c55-m58-snapshot-XXXXXX", &path, NULL);
    uint8_t value;
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, 8 * MiB);
    close(fd);
    args = g_strdup_printf("-drive if=pflash,format=raw,file=%s -snapshot",
                           path);
    qts = c55_start(args);
    flash_program_byte(qts, 0x300, 0x5a);
    qtest_quit(qts);
    fd = open(path, O_RDONLY);
    g_assert_cmpint(pread(fd, &value, 1, 0x300), ==, 1);
    g_assert_cmphex(value, ==, 0xff);
    close(fd);
    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/c55/flash/views-signature",
                   test_machine_flash_views_and_signature);
    qtest_add_func("/c55/board/cpu-clock", test_machine_cpu_and_clock);
    qtest_add_func("/c55/flash/program-status-reset",
                   test_flash_program_status_and_reset);
    qtest_add_func("/c55/flash/erase-buffer",
                   test_flash_erase_and_write_buffer);
    qtest_add_func("/c55/flash/persistence", test_flash_backend_persistence);
    qtest_add_func("/c55/flash/snapshot", test_flash_snapshot_preserves_base);
    qtest_add_func("/c55/flash/property-validation",
                   test_flash_property_validation);
    qtest_add_func("/c55/board/sram-reset-pin",
                   test_sram_folding_reset_seed_and_pin);
    qtest_add_func("/c55/board/product-xbus", test_product_xbus_contract);
    qtest_add_data_func("/c55/board/xbus-queued-mode", GUINT_TO_POINTER(1),
                        test_xbus_completion_mode);
    qtest_add_data_func("/c55/board/xbus-legacy-mode", GUINT_TO_POINTER(4),
                        test_xbus_completion_mode);
    qtest_add_func("/c55/board/startup-power-hold", test_startup_power_hold);
    qtest_add_func("/c55/board/runtime-soft-left",
                   test_runtime_keypad_soft_left);
    qtest_add_func("/c55/board/supervised-keypad",
                   test_supervised_keypad_properties);
    qtest_add_func("/c55/board/supervised-statistics-display",
                   test_supervised_statistics_and_display);
    qtest_add_func("/c55/board/battery-charger",
                   test_battery_and_charger_properties);
    qtest_add_func("/c55/board/display-frame",
                   test_display_wiring_and_frame);
    qtest_add_func("/c55/board/display-atomic-sweep",
                   test_pcf8813_atomic_sweep);
    qtest_add_func("/c55/board/display-sweep-fallbacks",
                   test_pcf8813_sweep_fallbacks);
    qtest_add_func("/c55/board/display-fragmented-sweep",
                   test_pcf8813_fragmented_sweep);
    qtest_add_func("/c55/board/asc0", test_asc0_attachment);
    return g_test_run();
}
