/*
 * Reusable Siemens x55 board-component qtests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"
#include "libqtest.h"

#define ADDRSEL2 0xfe1a
#define BUSCON2  0xff16
#define SSC0TB   0xf0b0
#define SSC0RB   0xf0b2
#define SSC0BR   0xf0b4
#define SSC0CON  0xffb2
#define P3       0xffc4
#define DP3      0xffc6
#define P6       0xffcc
#define DP6      0xffce
#define P7       0xffd0
#define DP7      0xffd2
#define P8       0xffd4
#define DP8      0xffd6
#define CCM0     0xff52
#define T8       0xf052
#define T78CON   0xff20
#define CC23     0xfe6e
#define CCM5     0xff24
#define IRQ55IC  0xf16e

static void qom_set_u32(QTestState *qts, const char *path,
                        const char *property, uint32_t value)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':%s,'value':%u}}", path, property, value);

    g_assert_false(qdict_haskey(response, "error"));
    qobject_unref(response);
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

static bool qom_get_bool(QTestState *qts, const char *path,
                         const char *property)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':%s}}", path, property);
    bool value;

    g_assert_false(qdict_haskey(response, "error"));
    value = qdict_get_bool(response, "return");
    qobject_unref(response);
    return value;
}

static char *qom_get_string(QTestState *qts, const char *path,
                            const char *property)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':%s}}", path, property);
    char *value;

    g_assert_false(qdict_haskey(response, "error"));
    value = g_strdup(qdict_get_str(response, "return"));
    qobject_unref(response);
    return value;
}

static char *qom_child_type(QTestState *qts, const char *path,
                            const char *child)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-list','arguments':{'path':%s}}", path);
    QList *entries;
    QListEntry *entry;
    char *result = NULL;

    g_assert_false(qdict_haskey(response, "error"));
    entries = qdict_get_qlist(response, "return");
    QLIST_FOREACH_ENTRY(entries, entry) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(entry));

        if (!strcmp(qdict_get_str(item, "name"), child)) {
            result = g_strdup(qdict_get_str(item, "type"));
            break;
        }
    }
    qobject_unref(response);
    return result;
}

static QTestState *x55_ram_start(const char *type)
{
    return qtest_initf("-M pmb7850-test,ram-type=%s -display none", type);
}

static void map_cs2(QTestState *qts)
{
    qtest_writew(qts, ADDRSEL2, 0x0009);
    qtest_writew(qts, BUSCON2, 1 << 10);
}

static void test_external_ram_geometries(void)
{
    QTestState *qts;

    qts = x55_ram_start("sram-256kx16");
    map_cs2(qts);
    qtest_writew(qts, 0x000020, 0x5aa5);
    g_assert_cmphex(qtest_readw(qts, 0x080020), ==, 0x5aa5);
    g_assert_cmphex(qtest_readw(qts, 0x180020), ==, 0x5aa5);
    qtest_system_reset(qts);
    map_cs2(qts);
    g_assert_cmphex(qtest_readw(qts, 0x100020), ==, 0x5aa5);
    qtest_quit(qts);

    qts = x55_ram_start("sram-512kx16");
    map_cs2(qts);
    qtest_writew(qts, 0x000020, 0x1234);
    qtest_writew(qts, 0x080020, 0x5678);
    g_assert_cmphex(qtest_readw(qts, 0x100020), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, 0x180020), ==, 0x5678);
    qtest_quit(qts);

    qts = x55_ram_start("psram-1024kx16");
    map_cs2(qts);
    qtest_writew(qts, 0x000020, 0x1111);
    qtest_writew(qts, 0x080020, 0x2222);
    qtest_writew(qts, 0x100020, 0x3333);
    qtest_writew(qts, 0x180020, 0x4444);
    g_assert_cmphex(qtest_readw(qts, 0x000020), ==, 0x1111);
    g_assert_cmphex(qtest_readw(qts, 0x080020), ==, 0x2222);
    g_assert_cmphex(qtest_readw(qts, 0x100020), ==, 0x3333);
    g_assert_cmphex(qtest_readw(qts, 0x180020), ==, 0x4444);
    qtest_quit(qts);
}

static void test_color_keypad_matrix(void)
{
    QTestState *qts = qtest_init(
        "-M pmb7850-test,keypad-profile=color -display none");

    qtest_writew(qts, DP3, 0);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000d);

    qtest_writew(qts, 0xfd1c, 0x0080);
    qom_set_u32(qts, "/machine/keypad", "ui-buttons", 1);
    qtest_writew(qts, DP3, 0);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000f);
    qtest_writew(qts, DP3, 1);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000e);
    g_assert_cmphex(qom_get_u32(qts, "/machine/keypad", "sampled-buttons"),
                    ==, 1);

    qom_set_u32(qts, "/machine/keypad", "ui-buttons", 0);
    qtest_writew(qts, DP3, 0x003d);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0xff0f);
    qtest_writew(qts, DP3, 0);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000f);
    qtest_writew(qts, DP3, 0x003d);
    qtest_writew(qts, DP3, 0);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0xff0f);

    qtest_system_reset(qts);
    g_assert_cmphex(qom_get_u32(qts, "/machine/keypad", "ui-buttons"),
                    ==, 0);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0);
    qtest_quit(qts);
}

static void test_sl55_direct_release(void)
{
    const uint32_t soft_left = 1u << 16;
    QTestState *qts = qtest_init("-M siemens-sl55 -display none");

    qtest_writew(qts, 0xef1c, 0x003d);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000d);
    qom_set_u32(qts, "/machine/keypad", "ui-buttons", soft_left);

    /* A command-write trace preview must not sample the pressed key. */
    qtest_writew(qts, 0xef1c, 0x003d);
    g_assert_cmphex(qom_get_u32(qts, "/machine/keypad", "sampled-buttons"),
                    ==, 0);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x0007);
    g_assert_cmphex(qom_get_u32(qts, "/machine/keypad", "sampled-buttons"),
                    ==, soft_left);

    qom_set_u32(qts, "/machine/keypad", "ui-buttons", 0);

    /* Release activity is visible for one scan, then returns to idle. */
    qtest_writew(qts, 0xef1c, 0x003d);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000f);
    qtest_writew(qts, 0xef1c, 0x003d);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0xff0f);

    qom_set_u32(qts, "/machine/keypad", "ui-buttons", soft_left);
    qtest_writew(qts, 0xef1c, 0x003d);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x0007);
    qtest_quit(qts);
}

static void test_zero_command_startup_keypad(void)
{
    QTestState *qts = qtest_init("-M siemens-a60 -display none");

    /*
     * A60/A62/A65/C60 issue an explicit zero-valued EF1C startup scan.
     * A reset-state read is empty, while the write must return the configured
     * held power-key value until both bytes have been consumed.
     */
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0);
    qtest_writew(qts, 0xef1c, 0);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000d);
    qtest_writew(qts, 0xfd1c, 0x0080);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000d);
    qtest_writew(qts, 0xef1c, 0);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0xff0f);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0);
    qtest_quit(qts);
}

static void test_all_board_keypad_profiles(void)
{
    static const struct {
        const char *machine;
        uint16_t scan;
        uint32_t power_button;
        uint32_t ack_addr;
        uint16_t ack_mask;
    } cases[] = {
        { "a52", 0x003d, 17, 0xfd0e, 1 << 11 },
        { "a55", 0x003d, 17, 0xfd0e, 1 << 11 },
        { "c55", 0x003d, 17, 0xfd12, 1 << 8 },
        { "a60", 0, 19, 0xfd1c, 1 << 7 },
        { "a62", 0, 19, 0xfd1a, 1 << 9 },
        { "a65", 0, 19, 0xfd1c, 1 << 9 },
        { "c60", 0, 19, 0xfd1c, 1 << 7 },
        { "cf62", 0, 19, 0xfd20, 1 << 11 },
        { "m55", 0, 19, 0xfd1e, 1 << 4 },
        { "mc60", 0, 17, 0xfd24, 1 << 13 },
        { "s55", 0, 19, 0xfd24, 1 << 11 },
        { "sl55", 0x003d, 19, 0xfd1c, 1 << 14 },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        const uint32_t power = 1u << cases[i].power_button;
        QTestState *qts = qtest_initf("-M siemens-%s -display none",
                                      cases[i].machine);

        /* Firmware-private RAM must not acknowledge an unread result. */
        qtest_writew(qts, cases[i].ack_addr, cases[i].ack_mask);
        qtest_writeb(qts, 0xef1d, 0);
        g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0);
        qtest_writew(qts, 0xef1c, cases[i].scan);
        g_assert_cmphex(qtest_readb(qts, 0xef1a), ==, 0x0d);
        g_assert_cmphex(qtest_readb(qts, 0xef1a), ==, 0x0d);
        {
            g_autofree char *peek = qtest_hmp(qts, "x /1hx 0xef1a");
            g_assert_nonnull(strstr(peek, "000d"));
        }
        /* Command-register reads and replacement reset partial consumption. */
        g_assert_cmphex(qtest_readw(qts, 0xef1c), ==, cases[i].scan);
        qtest_writew(qts, 0xef1c, cases[i].scan);
        g_assert_cmphex(qtest_readb(qts, 0xef1b), ==, 0);
        qtest_writew(qts, 0xef1c, cases[i].scan);
        g_assert_cmphex(qtest_readb(qts, 0xef1a), ==, 0x0d);
        g_assert_cmphex(qtest_readb(qts, 0xef1b), ==, 0);
        /* Reading is not a new scan; retain the already published result. */
        g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000d);
        qtest_writew(qts, 0xef1c, cases[i].scan);
        g_assert_cmphex(qtest_readw(qts, 0xef1a), !=, 0x000d);

        qtest_system_reset(qts);
        qtest_writeb(qts, 0xef1c, cases[i].scan);
        g_assert_cmphex(qtest_readb(qts, 0xef1a), ==, 0x0d);
        qtest_writeb(qts, 0xef1d, 0);
        g_assert_cmphex(qtest_readb(qts, 0xef1b), ==, 0);
        qtest_writeb(qts, 0xef1c, cases[i].scan);
        g_assert_cmphex(qtest_readw(qts, 0xef1a), !=, 0x000d);

        qtest_system_reset(qts);
        qom_set_u32(qts, "/machine/keypad", "ui-buttons", power);
        qtest_writew(qts, 0xef1c, cases[i].scan);
        g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000d);
        qtest_writew(qts, cases[i].ack_addr, cases[i].ack_mask);
        qtest_writew(qts, 0xef1c, cases[i].scan);
        g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000d);
        g_assert_cmphex(qom_get_u32(qts, "/machine/keypad",
                                    "sampled-buttons") & power, ==, power);
        qtest_quit(qts);
    }
}

static void test_ui_release_diagnostic(void)
{
    QTestState *qts = qtest_init("-M siemens-c55 -display none");
    const char *const phases[] = {"queued", "sampled", "cancelled", "forced"};
    const char *const invalid[] = {
        "32 queued", "-1 queued", "0 invalid", "0 queued extra", "0", "",
    };

    qom_set_u32(qts, "/machine/keypad", "ui-buttons", 1);
    for (unsigned i = 0; i < ARRAY_SIZE(phases); i++) {
        g_autofree char *value = g_strdup_printf("0 %s", phases[i]);
        QDict *response = qtest_qmp(qts,
            "{'execute':'qom-set','arguments':{'path':'/machine/keypad',"
            "'property':'ui-release-event','value':%s}}", value);

        g_assert_false(qdict_haskey(response, "error"));
        qobject_unref(response);
        g_assert_cmpuint(qom_get_u32(qts, "/machine/keypad", "ui-buttons"),
                         ==, 1);
        g_assert_cmpuint(qom_get_u32(qts, "/machine/keypad", "sampled-buttons"),
                         ==, 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        QDict *response = qtest_qmp(qts,
            "{'execute':'qom-set','arguments':{'path':'/machine/keypad',"
            "'property':'ui-release-event','value':%s}}", invalid[i]);

        g_assert_true(qdict_haskey(response, "error"));
        qobject_unref(response);
    }
    const char *const service_records[] = {
        "input_owner key changed 1 1 owner 3 18446744073709551615",
        "serial_history close subscription 3 1 tail 3 42",
        "keypad_deferred_release sampled key_index 0 0",
    };
    for (unsigned i = 0; i < ARRAY_SIZE(service_records); i++) {
        QDict *response = qtest_qmp(qts,
            "{'execute':'qom-set','arguments':{'path':'/machine',"
            "'property':'host-service-event','value':%s}}", service_records[i]);
        g_assert_false(qdict_haskey(response, "error"));
        qobject_unref(response);
        g_assert_cmpuint(qom_get_u32(qts, "/machine/keypad",
                                    "ui-buttons"), ==, 1);
        g_assert_cmpuint(qom_get_u32(qts, "/machine/keypad",
                                    "sampled-buttons"), ==, 0);
    }
    const char *const invalid_records[] = {
        "", "guest write", "input_owner bad\nrecord",
        "serial_history bad\"record",
    };
    for (unsigned i = 0; i < ARRAY_SIZE(invalid_records); i++) {
        QDict *response = qtest_qmp(qts,
            "{'execute':'qom-set','arguments':{'path':'/machine',"
            "'property':'host-service-event','value':%s}}", invalid_records[i]);
        g_assert_true(qdict_haskey(response, "error"));
        qobject_unref(response);
    }
    /* Host diagnostics must not consume the tagged startup result either. */
    qtest_writew(qts, 0xef1c, 0x003d);
    g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000d);
    qtest_quit(qts);
}

static void test_all_board_matrix_startup(void)
{
    static const char *const boards[] = {
        "a60", "a62", "a65", "c60", "cf62", "m55", "mc60", "s55", "sl55",
    };

    for (unsigned i = 0; i < ARRAY_SIZE(boards); i++) {
        QTestState *qts = qtest_initf("-M siemens-%s -display none", boards[i]);

        qtest_writew(qts, DP3, 0);
        g_assert_cmphex(qtest_readb(qts, 0xef1a), ==, 0x0d);
        qtest_writew(qts, DP3, 0);
        g_assert_cmphex(qtest_readb(qts, 0xef1b), ==, 0);
        /* A second replacement must still contain the implicit press. */
        qtest_writew(qts, DP3, 0);
        g_assert_cmphex(qtest_readw(qts, 0xef1a), ==, 0x000d);
        qtest_writew(qts, DP3, 0x003d);
        qtest_writew(qts, DP3, 0);
        g_assert_cmphex(qtest_readw(qts, 0xef1a), !=, 0x000d);
        qtest_quit(qts);
    }
}

static void test_a60_low_flash_commands(void)
{
    static const char *const models[] = { "a60", "a62" };
    const uint32_t target = 0x050020;

    for (unsigned i = 0; i < ARRAY_SIZE(models); i++) {
        QTestState *qts = qtest_initf("-M siemens-%s -display none",
                                      models[i]);

        /*
         * Both 8 MiB AM29 boards expose commands in the low flash window.
         * Their firmware performs buffered EEPROM writes there after EINIT.
         */
        qtest_writeb(qts, 0x000aaa, 0xaa);
        qtest_writeb(qts, 0x000555, 0x55);
        qtest_writeb(qts, target, 0x25);
        qtest_writew(qts, target, 0);
        qtest_writew(qts, target, 0x1234);
        qtest_writeb(qts, target, 0x29);
        g_assert_cmphex(qtest_readw(qts, target), ==, 0x1234);
        qtest_quit(qts);
    }
}

static char *create_pattern_flash(size_t size, const uint16_t *words,
                                  size_t word_count)
{
    g_autoptr(GError) error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("c166-x55-aperture-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    for (size_t i = 0; i < word_count; i++) {
        uint8_t bytes[] = { words[i], words[i] >> 8 };

        g_assert_cmpint(pwrite(fd, bytes, sizeof(bytes), i * 4 * MiB + 0x100),
                        ==, sizeof(bytes));
    }
    close(fd);
    return path;
}

static void test_fast_fallback_apertures(void)
{
    static const uint16_t words4[] = { 0x1101 };
    static const uint16_t words8[] = { 0x2201, 0x2202 };
    static const uint16_t words16[] = { 0x3301, 0x3302, 0x3303, 0x3304 };
    g_autofree char *flash4 = create_pattern_flash(4 * MiB, words4,
                                                   ARRAY_SIZE(words4));
    g_autofree char *flash8 = create_pattern_flash(8 * MiB, words8,
                                                   ARRAY_SIZE(words8));
    g_autofree char *flash16 = create_pattern_flash(16 * MiB, words16,
                                                     ARRAY_SIZE(words16));
    QTestState *qts;

    /* The 4 MiB A52 boot view and CS0 fallback both fold four times. */
    qts = qtest_initf(
        "-M siemens-a52 -drive if=pflash,format=raw,file=%s,snapshot=on "
        "-display none", flash4);
    for (unsigned address = 0x100; address < 16 * MiB; address += 4 * MiB) {
        g_assert_cmphex(qtest_readw(qts, address), ==, words4[0]);
    }
    qtest_quit(qts);

    /* An 8 MiB single flash repeats once through the CS0 fallback. */
    qts = qtest_initf(
        "-M siemens-a55 -drive if=pflash,format=raw,file=%s,snapshot=on "
        "-display none", flash8);
    for (unsigned address = 0x100; address < 16 * MiB; address += 4 * MiB) {
        g_assert_cmphex(qtest_readw(qts, address), ==,
                        words8[(address / (4 * MiB)) & 1]);
    }
    qtest_quit(qts);

    /* A 16 MiB device keeps all four quarters distinct. */
    qts = qtest_initf(
        "-M siemens-c60 -drive if=pflash,format=raw,file=%s,snapshot=on "
        "-display none", flash16);
    for (unsigned address = 0x100; address < 16 * MiB; address += 4 * MiB) {
        g_assert_cmphex(qtest_readw(qts, address), ==,
                        words16[address / (4 * MiB)]);
    }
    qtest_quit(qts);

    /* Dual-flash boot composition selects chip 1 in the second 4 MiB. */
    qts = qtest_initf(
        "-M siemens-s55 "
        "-drive if=pflash,unit=0,format=raw,file=%s,snapshot=on "
        "-drive if=pflash,unit=1,format=raw,file=%s,snapshot=on "
        "-display none", flash8, flash4);
    g_assert_cmphex(qtest_readw(qts, 0x000100), ==, words8[0]);
    g_assert_cmphex(qtest_readw(qts, 0x400100), ==, words4[0]);
    g_assert_cmphex(qtest_readw(qts, 0x800100), ==, words8[0]);
    g_assert_cmphex(qtest_readw(qts, 0xc00100), ==, words8[1]);
    qtest_quit(qts);

    unlink(flash4);
    unlink(flash8);
    unlink(flash16);
}

static void test_irq55_source_routing(void)
{
    static const struct {
        const char *machine;
        bool cc23;
    } cases[] = {
        { "a52", true }, { "a55", true }, { "a60", false },
        { "a62", false }, { "a65", false }, { "c60", false },
        { "cf62", false }, { "m55", true }, { "mc60", false },
        { "s55", true }, { "sl55", true },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        QTestState *qts = qtest_initf("-M siemens-%s -display none",
                                      cases[i].machine);

        g_assert_cmpint(qom_get_bool(qts, "/machine/soc",
                                     "irq55-cc23-connected"), ==,
                        cases[i].cc23);

        qtest_writew(qts, IRQ55IC, 0);
        qom_set_u32(qts, "/machine/keypad", "ui-buttons", 1);
        g_assert_true(qtest_readw(qts, IRQ55IC) & 0x80);
        qtest_quit(qts);
    }

    for (unsigned connected = 0; connected < 2; connected++) {
        QTestState *qts = qtest_initf(
            "-M pmb7850-test -global pmb7850-soc.irq55-cc23-connected=%s "
            "-display none", connected ? "on" : "off");

        qtest_writew(qts, IRQ55IC, 0);
        qtest_writew(qts, T8, 0);
        qtest_writew(qts, CC23, 1);
        qtest_writew(qts, CCM5, 0xc000); /* mode 4 on T8 */
        qtest_writew(qts, T78CON, 1 << 14);
        qtest_clock_step(qts, 1000);
        g_assert_cmpint(!!(qtest_readw(qts, IRQ55IC) & 0x80), ==,
                        connected);
        qtest_quit(qts);
    }
}

static void test_board_flash_protection_and_mirroring(void)
{
    static const struct {
        const char *machine;
        uint32_t start;
        uint32_t size;
    } flash[] = {
        { "a60", 0, 256 * 1024 }, { "a62", 0, 0 },
        { "a65", 8 * 1024 * 1024, 256 * 1024 },
        { "c60", 8 * 1024 * 1024, 256 * 1024 },
        { "cf62", 8 * 1024 * 1024, 256 * 1024 },
        { "m55", 8 * 1024 * 1024, 256 * 1024 },
        { "mc60", 8 * 1024 * 1024, 256 * 1024 },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(flash); i++) {
        QTestState *qts = qtest_initf("-M siemens-%s -display none",
                                      flash[i].machine);
        g_assert_cmphex(qom_get_u32(qts, "/machine/flash",
                                    "protected-start"), ==, flash[i].start);
        g_assert_cmphex(qom_get_u32(qts, "/machine/flash",
                                    "protected-size"), ==, flash[i].size);
        qtest_quit(qts);
    }

    for (unsigned i = 0; i < 2; i++) {
        const char *machine = i ? "a55" : "a52";
        QTestState *qts = qtest_initf("-M siemens-%s -display none",
                                      machine);
        g_assert_true(qom_get_bool(qts, "/machine/display",
                                   "panel-mirror-y"));
        qtest_quit(qts);
    }
}

static void x55_ssc_send(QTestState *qts, uint16_t value);

static void test_a60_display_dc_wiring(void)
{
    QTestState *qts = qtest_init("-M siemens-a60 -display none");

    uint64_t memory_before = qom_get_u64(qts, "/machine/display",
                                         "memory-digest");

    qtest_writew(qts, DP3, 1 << 11);
    qtest_writew(qts, DP8, 1 << 13);
    qtest_writew(qts, P3, 1 << 11);
    qtest_writew(qts, SSC0BR, 0);
    qtest_writew(qts, SSC0CON, 0xc0d7);
    qtest_writew(qts, P3, 0);
    qtest_writew(qts, P8, 1 << 13); /* HM17 command-high */
    x55_ssc_send(qts, 0xa1);
    qtest_writew(qts, P8, 0);       /* HM17 data-low */
    x55_ssc_send(qts, 0xff);
    x55_ssc_send(qts, 0xff);
    qtest_writew(qts, P3, 1 << 11);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 0);
    g_assert_cmphex(qom_get_u64(qts, "/machine/display", "memory-digest"),
                    !=, memory_before);
    qtest_quit(qts);
}

static void x55_ssc_send(QTestState *qts, uint16_t value)
{
    qtest_writew(qts, SSC0TB, value);
    qtest_clock_step(qts, 2000);
}

static void test_hm17_model(const char *type, bool rgb444)
{
    QTestState *qts = qtest_initf(
        "-M pmb7850-test,display-type=%s -display none", type);

    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "panel-width"),
                     ==, 101);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "panel-height"),
                     ==, 80);
    g_assert_false(qom_get_bool(qts, "/machine/display", "reset-asserted"));

    qtest_writew(qts, DP3, 1 << 11);
    qtest_writew(qts, DP7, 1 << 6);
    qtest_writew(qts, P3, 1 << 11);
    qtest_writew(qts, SSC0BR, 0);
    qtest_writew(qts, SSC0CON, 0xc0d7);
    qtest_writew(qts, P3, 0);
    qtest_writew(qts, P7, 1 << 6); /* HM17 command-high */
    x55_ssc_send(qts, 0xa1); /* increment X */
    x55_ssc_send(qts, 0x81); /* display on */
    qtest_writew(qts, P7, 0);      /* HM17 data-low */
    x55_ssc_send(qts, 0xff);
    if (rgb444) {
        x55_ssc_send(qts, 0xff);
    }
    qtest_writew(qts, P3, 1 << 11);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 0);

    qtest_writew(qts, CCM0, 5); /* compare mode 1 drives reset low */
    g_assert_true(qom_get_bool(qts, "/machine/display", "reset-asserted"));
    qtest_writew(qts, CCM0, 4); /* capture mode releases to idle-high */
    g_assert_false(qom_get_bool(qts, "/machine/display", "reset-asserted"));

    qtest_system_reset(qts);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 0);
    g_assert_false(qom_get_bool(qts, "/machine/display", "reset-asserted"));
    qtest_quit(qts);
}

static void test_mc60_ssc_unselected_receive(void)
{
    QTestState *qts = qtest_init("-M siemens-mc60 -display none");

    /* SW13's camera poll uses SSC0 while the display is deselected. */
    qtest_writew(qts, P3, 1u << 11);
    qtest_writew(qts, DP3, 1u << 11);
    qtest_writew(qts, SSC0BR, 2);
    qtest_writew(qts, SSC0CON, 0xc637);
    qtest_writew(qts, SSC0TB, 0x40);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readw(qts, SSC0RB), ==, 0xff);
    qtest_writew(qts, SSC0TB, 0xff);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readw(qts, SSC0RB), ==, 0xff);
    qtest_quit(qts);
}

static void test_hm17cm4096(void)
{
    test_hm17_model("hm17cm4096", true);
}

static void test_hm17cm256(void)
{
    test_hm17_model("hm17cm256", false);
}

static void x55_ssc_bits(QTestState *qts, unsigned bits);
static QTestState *x55_color_lcd_start(const char *type);

static void hm17cm256_start_transaction(QTestState *qts, unsigned x,
                                        unsigned y)
{
    qtest_writew(qts, P3, 0);
    qtest_writew(qts, P7, 1 << 6);
    x55_ssc_send(qts, x & 0xf);
    x55_ssc_send(qts, 0x10 | (x >> 4));
    x55_ssc_send(qts, 0x20 | (y & 0xf));
    x55_ssc_send(qts, 0x30 | (y >> 4));
    x55_ssc_send(qts, 0xa1);
    qtest_writew(qts, P7, 0);
}

static void hm17cm256_write_row(QTestState *qts, unsigned x, unsigned y,
                                unsigned bytes, uint8_t value)
{
    hm17cm256_start_transaction(qts, x, y);
    for (unsigned i = 0; i < bytes; i++) {
        x55_ssc_send(qts, value);
    }
    qtest_writew(qts, P3, 1 << 11);
}

static QTestState *hm17cm256_sweep_start(void)
{
    QTestState *qts = x55_color_lcd_start("hm17cm256");

    qtest_writew(qts, P7, 1 << 6);
    x55_ssc_send(qts, 0x81);
    qtest_writew(qts, P3, 1 << 11);
    return qts;
}

static void test_hm17cm256_atomic_sweeps(void)
{
    QTestState *qts = hm17cm256_sweep_start();
    uint64_t initial_memory = qom_get_u64(qts, "/machine/display",
                                          "memory-digest");
    uint64_t initial_frame = qom_get_u64(qts, "/machine/display",
                                         "frame-digest");

    for (unsigned y = 0; y < 79; y++) {
        hm17cm256_write_row(qts, 0, y, 101, 0xff);
        g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                         ==, 0);
    }
    g_assert_cmphex(qom_get_u64(qts, "/machine/display", "memory-digest"),
                    !=, initial_memory);
    g_assert_cmphex(qom_get_u64(qts, "/machine/display", "frame-digest"),
                    ==, initial_frame);
    hm17cm256_write_row(qts, 0, 79, 101, 0xff);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 1);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display",
                                 "transaction-count"), ==, 80);

    for (unsigned y = 0; y < 79; y++) {
        hm17cm256_write_row(qts, 16, y, 32, 0x00);
        g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                         ==, 1);
    }
    hm17cm256_write_row(qts, 16, 79, 32, 0x00);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 2);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display",
                                 "transaction-count"), ==, 160);
    qtest_quit(qts);
}

static void test_hm17cm256_sweep_fallbacks(void)
{
    QTestState *qts = hm17cm256_sweep_start();

    hm17cm256_write_row(qts, 0, 0, 32, 0x11);
    hm17cm256_start_transaction(qts, 1, 1);
    x55_ssc_send(qts, 0x22);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 1);
    qtest_writew(qts, P3, 1 << 11);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 1);

    hm17cm256_write_row(qts, 0, 0, 32, 0x33);
    hm17cm256_write_row(qts, 0, 1, 31, 0x44);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 2);

    hm17cm256_write_row(qts, 0, 0, 32, 0x55);
    hm17cm256_start_transaction(qts, 0, 0);
    x55_ssc_send(qts, 0x66);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 4);
    for (unsigned i = 1; i < 32; i++) {
        x55_ssc_send(qts, 0x66);
    }
    qtest_writew(qts, P3, 1 << 11);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 4);

    qtest_system_reset(qts);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 4);
    qtest_writew(qts, DP3, 1 << 11);
    qtest_writew(qts, DP7, 1 << 6);
    qtest_writew(qts, P3, 1 << 11);
    qtest_writew(qts, SSC0BR, 0);
    x55_ssc_bits(qts, 8);
    hm17cm256_write_row(qts, 0, 1, 32, 0x77);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 4);
    qtest_quit(qts);
}

static void x55_ssc_bits(QTestState *qts, unsigned bits)
{
    qtest_writew(qts, SSC0CON, 0);
    qtest_writew(qts, SSC0CON, 0xc0d0 | (bits - 1));
}

static QTestState *x55_color_lcd_start(const char *type)
{
    QTestState *qts = qtest_initf(
        "-M pmb7850-test,display-type=%s -display none", type);

    qtest_writew(qts, DP3, 1 << 11);
    qtest_writew(qts, DP7, 1 << 6);
    qtest_writew(qts, P3, 1 << 11);
    qtest_writew(qts, SSC0BR, 0);
    x55_ssc_bits(qts, 8);
    qtest_writew(qts, P3, 0);
    return qts;
}

enum SweepLCD { SWEEP_HM256, SWEEP_HM4096, SWEEP_PCF8833, SWEEP_S6 };

static void sweep_command(QTestState *qts, enum SweepLCD model, uint8_t value)
{
    x55_ssc_bits(qts, 8);
    qtest_writew(qts, P7, model <= SWEEP_HM4096 ? 1 << 6 : 0);
    x55_ssc_send(qts, value);
}

static void sweep_window(QTestState *qts, enum SweepLCD model)
{
    if (model <= SWEEP_HM4096) {
        unsigned start = model == SWEEP_HM4096 ? 48 : 13;
        unsigned end = start + (model == SWEEP_HM4096 ? 4 : 2) - 1;
        const uint8_t commands[] = {
            0xf0, 0xab, start & 15, 0x10 | (start >> 4), 0x20, 0x30, 0xf5,
            end & 15, 0x10 | (end >> 4), 0x21, 0x30, 0xf0,
        };
        for (unsigned i = 0; i < ARRAY_SIZE(commands); i++) {
            sweep_command(qts, model, commands[i]);
        }
    } else if (model == SWEEP_S6) {
        const uint8_t commands[] = {0x43, 1, 2, 0x42, 0, 1};
        for (unsigned i = 0; i < ARRAY_SIZE(commands); i++) {
            sweep_command(qts, model, commands[i]);
        }
    } else {
        sweep_command(qts, model, 0x2a);
        qtest_writew(qts, P7, 1 << 6);
        x55_ssc_send(qts, 1);
        x55_ssc_send(qts, 2);
        sweep_command(qts, model, 0x2b);
        qtest_writew(qts, P7, 1 << 6);
        x55_ssc_send(qts, 0);
        x55_ssc_send(qts, 1);
        sweep_command(qts, model, 0x2c);
    }
}

static void sweep_pixel(QTestState *qts, enum SweepLCD model, bool white)
{
    unsigned bits = model <= SWEEP_HM4096 ? 8 :
                    model == SWEEP_S6 ? 16 : 12;
    x55_ssc_bits(qts, bits);
    qtest_writew(qts, P7, model <= SWEEP_HM4096 ? 0 : 1 << 6);
    x55_ssc_send(qts, white ? (1u << bits) - 1 : 0);
    if (model == SWEEP_HM4096) {
        x55_ssc_send(qts, white ? 0xff : 0);
    }
}

static void test_fragmented_color_windows(void)
{
    const char *const types[] = {
        "hm17cm256", "hm17cm4096", "pcf8833-compatible", "s6b33bx-compatible",
    };

    for (unsigned model = 0; model < ARRAY_SIZE(types); model++) {
        QTestState *qts = x55_color_lcd_start(types[model]);
        uint64_t blank, complete;

        if (model != SWEEP_S6) {
            sweep_command(qts, model, model <= SWEEP_HM4096 ? 0x81 : 0x11);
        }
        sweep_window(qts, model);
        blank = qom_get_u64(qts, "/machine/display", "presentation-digest");

        for (unsigned pixel = 0; pixel < 4; pixel++) {
            sweep_pixel(qts, model, true);
            g_assert_cmpuint(qom_get_u32(qts, "/machine/display",
                                         "frame-count"),
                             ==, pixel == 3);
            if (pixel < 3) {
                g_assert_cmphex(qom_get_u64(qts, "/machine/display",
                                            "presentation-digest"), ==, blank);
            }
            qtest_writew(qts, P3, 1 << 11);
            qtest_writew(qts, P3, 0);
        }
        complete = qom_get_u64(qts, "/machine/display", "presentation-digest");
        g_assert_cmphex(complete, !=, blank);

        if (model != SWEEP_S6) {
            uint64_t updates = qom_get_u64(qts, "/machine/display",
                                           "display-update-count");

            sweep_command(qts, model, model <= SWEEP_HM4096 ? 0x98 : 0x21);
            g_assert_cmpuint(qom_get_u64(qts, "/machine/display",
                                         "display-update-count"),
                             ==, updates + 1);
            sweep_command(qts, model, model <= SWEEP_HM4096 ? 0x98 : 0x21);
            g_assert_cmpuint(qom_get_u64(qts, "/machine/display",
                                         "display-update-count"),
                             ==, updates + 1);
            g_assert_cmphex(qom_get_u64(qts, "/machine/display",
                                        "presentation-digest"), !=, complete);
            sweep_command(qts, model, model <= SWEEP_HM4096 ? 0x90 : 0x20);
            g_assert_cmphex(qom_get_u64(qts, "/machine/display",
                                        "presentation-digest"), ==, complete);
            g_assert_cmpuint(qom_get_u32(qts, "/machine/display",
                                         "frame-count"), ==, 1);
        }
        sweep_window(qts, model);
        sweep_pixel(qts, model, false);
        g_assert_cmphex(qom_get_u64(qts, "/machine/display",
                                    "presentation-digest"), ==, complete);
        sweep_window(qts, model);
        sweep_pixel(qts, model, true);
        g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                         ==, 2);
        /* Restart publishes the old black pixel before the new white one. */
        g_assert_cmphex(qom_get_u64(qts, "/machine/display",
                                    "presentation-digest"), !=, complete);
        qtest_system_reset(qts);
        g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                         ==, 2);
        qtest_quit(qts);
    }
}

static void test_hm17cm4096_packed_sweep(void)
{
    QTestState *qts = x55_color_lcd_start("hm17cm4096");
    /* Three bytes encode two pixels; the window spans two such rows. */
    const uint8_t commands[] = {
        0x81, 0xf4, 0x98, 0xf0, 0xab,
        0x04, 0x12, 0x20, 0x30, 0xf5, 0x06, 0x12, 0x21, 0x30, 0xf0,
    };
    uint64_t blank;

    for (unsigned i = 0; i < ARRAY_SIZE(commands); i++) {
        sweep_command(qts, SWEEP_HM4096, commands[i]);
    }
    blank = qom_get_u64(qts, "/machine/display", "presentation-digest");
    qtest_writew(qts, P7, 0);
    for (unsigned byte = 0; byte < 6; byte++) {
        x55_ssc_send(qts, 0xff);
        g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                         ==, byte == 5);
        if (byte < 5) {
            g_assert_cmphex(qom_get_u64(qts, "/machine/display",
                                        "presentation-digest"), ==, blank);
        }
        if (byte == 2) {
            qtest_writew(qts, P3, 1 << 11);
            qtest_writew(qts, P3, 0);
        }
    }
    g_assert_cmphex(qom_get_u64(qts, "/machine/display",
                                "presentation-digest"), !=, blank);
    /* A packing-mode change closes an existing, partly filled candidate. */
    for (unsigned byte = 0; byte < 3; byte++) {
        x55_ssc_send(qts, 0);
    }
    sweep_command(qts, SWEEP_HM4096, 0xf4);
    sweep_command(qts, SWEEP_HM4096, 0x90);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 2);
    qtest_quit(qts);
}

static void test_s6b33bx_compatible(void)
{
    const uint64_t left_edge_digest = UINT64_C(0x96b16230de260bba);
    const uint64_t right_edge_digest = UINT64_C(0x9fbab66811454e58);
    QTestState *qts = x55_color_lcd_start("s6b33bx-compatible");

    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "panel-width"),
                     ==, 130);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "panel-height"),
                     ==, 130);
    qtest_writew(qts, P7, 0);
    x55_ssc_bits(qts, 16);
    x55_ssc_send(qts, 0x43); /* commands require 8-bit frames */
    x55_ssc_bits(qts, 8);
    x55_ssc_send(qts, 0x43);
    x55_ssc_send(qts, 0);
    x55_ssc_send(qts, 0);
    x55_ssc_send(qts, 0x42);
    x55_ssc_send(qts, 64);
    x55_ssc_send(qts, 64);
    qtest_writew(qts, P7, 1 << 6);
    x55_ssc_send(qts, 0xf8); /* pixels require 16-bit frames */
    x55_ssc_bits(qts, 16);
    x55_ssc_send(qts, 0xf800);
    qtest_writew(qts, P3, 1 << 11);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 1);
    uint64_t digest = qom_get_u64(qts, "/machine/display", "frame-digest");

    g_assert_cmphex(digest, ==, left_edge_digest);
    g_assert_cmphex(digest, !=, right_edge_digest);
    qtest_quit(qts);
}

static void test_pcf8833_compatible(void)
{
    const uint64_t red_pixel_digest = UINT64_C(0x22d29f66dd2b8b62);
    QTestState *qts = x55_color_lcd_start("pcf8833-compatible");

    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "panel-width"),
                     ==, 101);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "panel-height"),
                     ==, 80);
    qtest_writew(qts, P7, 0);
    x55_ssc_bits(qts, 16);
    x55_ssc_send(qts, 0x11); /* controls require 8-bit frames */
    x55_ssc_bits(qts, 8);
    x55_ssc_send(qts, 0x11); /* sleep out */
    x55_ssc_send(qts, 0x2a);
    qtest_writew(qts, P7, 1 << 6);
    x55_ssc_send(qts, 1);
    x55_ssc_send(qts, 1);
    qtest_writew(qts, P7, 0);
    x55_ssc_send(qts, 0x2b);
    qtest_writew(qts, P7, 1 << 6);
    x55_ssc_send(qts, 2);
    x55_ssc_send(qts, 2);
    qtest_writew(qts, P7, 0);
    x55_ssc_send(qts, 0x2c);
    qtest_writew(qts, P7, 1 << 6);
    x55_ssc_bits(qts, 16);
    x55_ssc_send(qts, 0xf00); /* pixels require 12-bit frames */
    x55_ssc_bits(qts, 12);
    x55_ssc_send(qts, 0xf00);
    qtest_writew(qts, P3, 1 << 11);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 1);
    g_assert_cmphex(qom_get_u64(qts, "/machine/display", "frame-digest"),
                    ==, red_pixel_digest);
    qtest_writew(qts, CCM0, 5);
    g_assert_true(qom_get_bool(qts, "/machine/display", "reset-asserted"));
    qtest_writew(qts, CCM0, 4);
    g_assert_false(qom_get_bool(qts, "/machine/display", "reset-asserted"));
    qtest_quit(qts);
}

static void test_pcf8833_viewport(void)
{
    QTestState *qts = x55_color_lcd_start("pcf8833-compatible");

    sweep_command(qts, SWEEP_PCF8833, 0x11);
    sweep_command(qts, SWEEP_PCF8833, 0x2a);
    qtest_writew(qts, P7, 1 << 6);
    x55_ssc_send(qts, 1);
    x55_ssc_send(qts, 101);
    sweep_command(qts, SWEEP_PCF8833, 0x2b);
    qtest_writew(qts, P7, 1 << 6);
    x55_ssc_send(qts, 0);
    x55_ssc_send(qts, 80); /* inclusive off-screen row */
    sweep_command(qts, SWEEP_PCF8833, 0x2c);
    for (unsigned i = 0; i < 101 * 80 - 1; i++) {
        sweep_pixel(qts, SWEEP_PCF8833, true);
    }
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 0);
    sweep_pixel(qts, SWEEP_PCF8833, true);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 1);
    /* Publication needs neither another command nor deselection. */
    for (unsigned i = 0; i < 100; i++) {
        sweep_pixel(qts, SWEEP_PCF8833, false);
    }
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 1);
    sweep_pixel(qts, SWEEP_PCF8833, false);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "frame-count"),
                     ==, 2); /* the physical window still wraps inclusively */
    qtest_quit(qts);
}

#define TWI_SCL (1 << 12)
#define TWI_SDA (1 << 13)

static void twi_lines(QTestState *qts, bool scl_low, bool sda_low)
{
    qtest_writew(qts, P6, 0);
    qtest_writew(qts, DP6, (scl_low ? TWI_SCL : 0) |
                              (sda_low ? TWI_SDA : 0));
}

static void twi_start(QTestState *qts)
{
    twi_lines(qts, false, false);
    twi_lines(qts, false, true);
}

static void twi_stop(QTestState *qts)
{
    twi_lines(qts, true, true);
    twi_lines(qts, false, true);
    twi_lines(qts, false, false);
}

static bool twi_send_byte(QTestState *qts, uint8_t value)
{
    for (unsigned bit = 0; bit < 8; bit++) {
        bool low = !(value & (0x80 >> bit));

        twi_lines(qts, true, low);
        twi_lines(qts, false, low);
        twi_lines(qts, true, low);
    }
    twi_lines(qts, true, false);
    twi_lines(qts, false, false);
    bool ack = !(qtest_readw(qts, P6) & TWI_SDA);
    twi_lines(qts, true, false);
    return ack;
}

static uint8_t twi_read_byte(QTestState *qts)
{
    uint8_t value = 0;

    for (unsigned bit = 0; bit < 8; bit++) {
        twi_lines(qts, true, false);
        twi_lines(qts, false, false);
        value = (value << 1) | !!(qtest_readw(qts, P6) & TWI_SDA);
        twi_lines(qts, true, false);
    }
    twi_lines(qts, true, false);
    twi_lines(qts, false, false); /* NACK */
    twi_lines(qts, true, false);
    return value;
}

static uint8_t twi_read_register(QTestState *qts, uint8_t reg)
{
    twi_start(qts);
    g_assert_true(twi_send_byte(qts, 0x31 << 1));
    g_assert_true(twi_send_byte(qts, reg));
    twi_start(qts);
    g_assert_true(twi_send_byte(qts, (0x31 << 1) | 1));
    uint8_t value = twi_read_byte(qts);
    twi_stop(qts);
    return value;
}

static void test_twi_register_file(void)
{
    QTestState *qts = qtest_init(
        "-M pmb7850-test,twi-profile=mc60 -display none");

    twi_start(qts);
    g_assert_true(twi_send_byte(qts, 0x31 << 1));
    g_assert_true(twi_send_byte(qts, 1));
    g_assert_true(twi_send_byte(qts, 0xa5));
    twi_stop(qts);
    g_assert_cmphex(twi_read_register(qts, 1), ==, 0xa5);
    twi_start(qts);
    g_assert_true(twi_send_byte(qts, 0x31 << 1));
    g_assert_false(twi_send_byte(qts, 32));
    twi_stop(qts);
    g_assert_cmpuint(qom_get_u32(qts, "/machine/twi", "register-writes"),
                     ==, 1);
    qtest_system_reset(qts);
    g_assert_cmphex(twi_read_register(qts, 1), ==, 0xa5);
    qtest_quit(qts);

    qts = qtest_init("-M pmb7850-test,twi-profile=cf62 -display none");
    twi_start(qts);
    g_assert_true(twi_send_byte(qts, 0x31 << 1));
    g_assert_true(twi_send_byte(qts, 0x37));
    twi_stop(qts);
    qtest_quit(qts);
}

typedef struct BoardExpectation {
    const char *name;
    const char *flash_type;
    const char *ram_type;
    const char *display_type;
    uint16_t xbus_device_id;
    uint16_t panel_width;
    uint16_t panel_height;
    uint16_t panel_origin_x;
    bool panel_mirror_x;
    bool panel_mirror_y;
    bool secondary_flash;
    bool twi;
} BoardExpectation;

static void test_production_board_matrix(void)
{
    static const BoardExpectation boards[] = {
        { "a52", "child<m58-compatible-32mbit>", "child<sram-256kx16>",
          "child<pcf8813>", 0x1202, 101, 64, 1, true, true,
          false, false },
        { "a55", "child<m58lw064d>", "child<sram-256kx16>",
          "child<pcf8813>", 0x1202, 101, 64, 1, true, true,
          false, false },
        { "c55", "child<m58lw064d>", "child<sram-256kx16>",
          "child<pcf8813>", 0x1202, 101, 64, 1, true, true,
          false, false },
        { "a60", "child<am29lv640mh>", "child<psram-1024kx16>",
          "child<hm17cm4096>", 0x1203, 101, 80, 24, true },
        { "a62", "child<am29lv640mh>", "child<psram-1024kx16>",
          "child<hm17cm4096>", 0x1203, 101, 80, 24, true },
        { "a65", "child<am29lv128mh>", "child<psram-1024kx16>",
          "child<hm17cm4096>", 0x1203, 101, 80, 24, true },
        { "c60", "child<am29lv128mh>", "child<psram-1024kx16>",
          "child<hm17cm4096>", 0x1203, 101, 80, 24, true },
        { "cf62", "child<am29lv128mh>", "child<psram-1024kx16>",
          "child<s6b33bx-compatible>", 0x1203, 130, 130, 0, false,
          false, false, true },
        { "m55", "child<am29lv128mh>", "child<psram-1024kx16>",
          "child<hm17cm4096>", 0x1203, 101, 80, 24, true },
        { "mc60", "child<am29lv128mh>", "child<psram-1024kx16>",
          "child<hm17cm4096>", 0x1203, 101, 80, 24, true,
          false, false, true },
        { "s55", "child<w30-64mbit-top>", "child<sram-512kx16>",
          "child<hm17cm256>", 0x1202, 101, 80, 13, true,
          false, true },
        { "sl55", "child<w30-64mbit-top>", "child<sram-512kx16>",
          "child<pcf8833-compatible>", 0x1203, 101, 80, 1, true,
          false, true },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(boards); i++) {
        const BoardExpectation *board = &boards[i];
        QTestState *qts = qtest_initf(
            "-M siemens-%s -display none -serial null", board->name);
        g_autofree char *flash = qom_child_type(qts, "/machine", "flash");
        g_autofree char *display = qom_child_type(qts, "/machine", "display");
        g_autofree char *ram = qom_child_type(qts, "/machine", "ram");
        g_autofree char *keypad = qom_child_type(qts, "/machine", "keypad");
        g_autofree char *xbus = qom_child_type(qts, "/machine", "product-xbus");
        g_autofree char *secondary = qom_child_type(
            qts, "/machine", "flash-secondary");
        g_autofree char *twi = qom_child_type(qts, "/machine", "twi");
        g_autofree char *battery = qom_child_type(qts, "/machine", "battery");

        g_assert_cmpstr(flash, ==, board->flash_type);
        g_assert_cmpstr(ram, ==, board->ram_type);
        g_assert_cmpstr(display, ==, board->display_type);
        g_assert_cmpstr(keypad, ==, "child<siemens-keypad>");
        g_assert_cmpstr(xbus, ==, "child<siemens-xbus-endpoint>");
        g_assert_cmphex(qom_get_u32(qts, "/machine/product-xbus",
                                    "device-id"), ==,
                        board->xbus_device_id);
        g_assert_cmphex(qom_get_u32(qts, "/machine/product-xbus",
                                    "irq-vector"), ==, 0x50);
        g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "panel-width"),
                         ==, board->panel_width);
        g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "panel-height"),
                         ==, board->panel_height);
        g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "panel-origin-x"),
                         ==, board->panel_origin_x);
        g_assert_cmpint(qom_get_bool(qts, "/machine/display",
                                    "panel-mirror-x"), ==,
                        board->panel_mirror_x);
        g_assert_cmpint(qom_get_bool(qts, "/machine/display",
                                    "panel-mirror-y"), ==,
                        board->panel_mirror_y);
        if (!strcmp(board->name, "cf62") || !strcmp(board->name, "sl55")) {
            g_assert_cmpuint(qom_get_u32(qts, "/machine/display", "scale"),
                             ==, 1);
            g_assert_true(qom_get_bool(qts, "/machine/display",
                                      "reset-idle-level"));
            g_assert_cmpuint(qom_get_u32(qts, "/machine/display",
                                         "frame-count"), ==, 0);
            g_assert_false(qom_get_bool(qts, "/machine/display",
                                       "reset-asserted"));
        }
        g_assert_cmphex(qtest_readw(qts, 0xf13c), ==, 0x0002);
        g_assert_cmpint(secondary != NULL, ==, board->secondary_flash);
        g_assert_cmpint(twi != NULL, ==, board->twi);
        g_assert_cmpstr(battery, ==, "child<siemens-x55-battery>");
        qtest_quit(qts);
    }
}

static void test_all_board_battery_routes(void)
{
    static const struct {
        const char *name;
        unsigned word;
    } boards[] = {
        { "a52", 1 }, { "a55", 1 }, { "a60", 0 }, { "a62", 0 },
        { "a65", 0 }, { "c55", 1 }, { "c60", 0 }, { "cf62", 0 },
        { "m55", 0 }, { "mc60", 0 }, { "s55", 1 }, { "sl55", 0 },
    };
    /* C55 SW24 bucket-policy vectors, with generated block-67 endpoints. */
    static const struct {
        unsigned level;
        int millivolts;
        int16_t raw;
    } points[] = {
        { 0, 3649, 1610 },
        { 1, 3659, 1549 },
        { 2, 3669, 1488 },
        { 3, 3679, 1427 },
        { 4, 3689, 1366 },
        { 5, 3699, 1304 },
        { 10, 3754, 968 },
        { 15, 3774, 845 },
        { 20, 3789, 754 },
        { 25, 3804, 662 },
        { 30, 3814, 600 },
        { 35, 3824, 539 },
        { 40, 3834, 478 },
        { 45, 3849, 386 },
        { 50, 3859, 325 },
        { 55, 3879, 203 },
        { 60, 3894, 111 },
        { 65, 3919, -42 },
        { 70, 3939, -165 },
        { 75, 3969, -348 },
        { 80, 3999, -532 },
        { 85, 4029, -715 },
        { 90, 4059, -899 },
        { 95, 4104, -1175 },
        { 100, 4200, -1763 },
    };

    for (unsigned b = 0; b < ARRAY_SIZE(boards); b++) {
        QTestState *qts = qtest_initf(
            "-M siemens-%s -display none -serial null", boards[b].name);
        unsigned result = 0xef2c + 2 * boards[b].word;
        unsigned unused = 0xef2c + 2 * (1 - boards[b].word);

        for (unsigned i = 0; i < ARRAY_SIZE(points); i++) {
            qom_set_u32(qts, "/machine", "battery-level", points[i].level);
            qtest_writew(qts, unused, 0xbeef);
            qtest_writew(qts, 0xe062, 0x0801);
            g_assert_cmphex(qtest_readw(qts, result), ==,
                            (uint16_t)points[i].raw);
            g_assert_cmphex(qtest_readw(qts, unused), ==, 0);
            /* Firmware's signed affine conversion must not cross upward. */
            int millivolts = 4178 - (points[i].raw + 1628) * 1001 / 6127;
            g_assert_cmpint(millivolts, ==, points[i].millivolts);
        }
        /* High-byte writes do not launch; the low-byte store does. */
        qtest_writew(qts, result, 0x1234);
        qtest_writeb(qts, 0xe063, 0x08);
        g_assert_cmphex(qtest_readw(qts, result), ==, 0x1234);
        qtest_writeb(qts, 0xe062, 1);
        g_assert_cmphex(qtest_readw(qts, result), ==, 0xf91d);
        qtest_writew(qts, 0xe062, 0x0811);
        g_assert_cmphex(qtest_readw(qts, result), ==, 0);
        qtest_writew(qts, 0xe062, 0x0803);
        g_assert_cmphex(qtest_readw(qts, result), ==, 0);
        qtest_writew(qts, 0xe062, 0x0801);
        qtest_writew(qts, 0xe062, 0x0003);
        g_assert_cmphex(qtest_readw(qts, result), ==, 0xf91d);

        for (unsigned charging = 0; charging < 2; charging++) {
            QDict *response = qtest_qmp(qts,
                "{'execute':'qom-set','arguments':{'path':'/machine',"
                "'property':'battery-charging','value':%i}}", charging);
            g_assert_false(qdict_haskey(response, "error"));
            qobject_unref(response);
            qtest_writew(qts, 0xec14, 2);
            qtest_writew(qts, 0xec80, 0x0162);
            qtest_writew(qts, 0xec82, 2);
            qtest_writew(qts, 0xec84, 0x0163);
            qtest_writew(qts, 0xec10, 0x1704);
            qtest_writew(qts, 0xec12, 1);
            qtest_clock_step(qts, 32);
            g_assert_cmphex(qtest_readw(qts, 0xec16), ==,
                            charging ? 0x0010 : 0);
        }
        qtest_system_reset(qts);
        qtest_writew(qts, 0xe062, 0x0801);
        g_assert_cmphex(qtest_readw(qts, result), ==, 0xf91d);
        g_assert_cmphex(qtest_readw(qts, unused), ==, 0);
        g_assert_true(qom_get_bool(qts, "/machine", "battery-charging"));
        qtest_quit(qts);
    }
}

static void test_c55_normalized_surface(void)
{
    const char *qemu = qtest_qemu_binary(NULL);
    const char *const bad_flash_argv[] = {
        qemu, "-M", "siemens-c55,flash-type=am29lv640mh",
        "-display", "none", NULL,
    };
    g_autofree char *stdout_text = NULL;
    g_autofree char *stderr_text = NULL;
    g_autoptr(GError) error = NULL;
    int status;
    QTestState *qts = qtest_init(
        "-M siemens-c55,flash-type=m58lw064d,"
        "secondary-flash-type=m58-compatible-32mbit "
        "-display none -serial null");
    g_autofree char *ram = qom_child_type(qts, "/machine", "ram");
    g_autofree char *old_ram = qom_child_type(qts, "/machine", "sram");
    g_autofree char *flash_type = qom_get_string(
        qts, "/machine", "flash-type");
    g_autofree char *secondary_type = qom_get_string(
        qts, "/machine", "secondary-flash-type");

    g_assert_cmpstr(ram, ==, "child<sram-256kx16>");
    g_assert_null(old_ram);
    g_assert_cmpstr(flash_type, ==, "m58lw064d");
    g_assert_cmpstr(secondary_type, ==, "m58-compatible-32mbit");
    qtest_quit(qts);

    g_assert_true(g_spawn_sync(NULL, (char **)bad_flash_argv, NULL,
                               G_SPAWN_SEARCH_PATH, NULL, NULL,
                               &stdout_text, &stderr_text, &status, &error));
    g_assert_no_error(error);
    g_assert_false(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    g_assert_nonnull(strstr(stderr_text,
        "c55 does not permit flash type 'am29lv640mh'"));
}

static void test_production_board_inputs_and_alternates(void)
{
    QTestState *qts = qtest_init(
        "-M siemens-cf62,flash-type=w30-128mbit-top "
        "-display none -serial null");
    g_autofree char *flash = qom_child_type(qts, "/machine", "flash");

    g_assert_cmpstr(flash, ==, "child<w30-128mbit-top>");
    qtest_writew(qts, DP3, 0);
    g_assert_cmphex(qtest_readw(qts, P3) & 0x0402, ==, 0x0402);
    qtest_quit(qts);

    qts = qtest_init("-M siemens-a65,flash-type=w30-128mbit-top "
                     "-display none -serial null");
    g_free(flash);
    flash = qom_child_type(qts, "/machine", "flash");
    g_assert_cmpstr(flash, ==, "child<w30-128mbit-top>");
    qtest_quit(qts);

    qts = qtest_init("-M siemens-c60,flash-type=w30-128mbit-top "
                     "-display none -serial null");
    g_free(flash);
    flash = qom_child_type(qts, "/machine", "flash");
    g_assert_cmpstr(flash, ==, "child<w30-128mbit-top>");
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/x55/ram/geometries", test_external_ram_geometries);
    qtest_add_func("/x55/keypad/color-matrix", test_color_keypad_matrix);
    qtest_add_func("/x55/keypad/sl55-direct-release",
                   test_sl55_direct_release);
    qtest_add_func("/x55/keypad/zero-command-startup",
                   test_zero_command_startup_keypad);
    qtest_add_func("/x55/keypad/all-board-profiles",
                   test_all_board_keypad_profiles);
    qtest_add_func("/x55/keypad/all-board-matrix-startup",
                   test_all_board_matrix_startup);
    qtest_add_func("/x55/keypad/ui-release-diagnostic",
                   test_ui_release_diagnostic);
    qtest_add_func("/x55/flash/a60-low-command-window",
                   test_a60_low_flash_commands);
    qtest_add_func("/x55/flash/fast-fallback-apertures",
                   test_fast_fallback_apertures);
    qtest_add_func("/x55/flash/protection-and-panel-mirroring",
                   test_board_flash_protection_and_mirroring);
    qtest_add_func("/x55/irq55/source-routing", test_irq55_source_routing);
    qtest_add_func("/x55/display/a60-dc-wiring",
                   test_a60_display_dc_wiring);
    qtest_add_func("/x55/display/hm17cm4096", test_hm17cm4096);
    qtest_add_func("/x55/mc60/ssc-unselected-receive",
                   test_mc60_ssc_unselected_receive);
    qtest_add_func("/x55/display/hm17cm256", test_hm17cm256);
    qtest_add_func("/x55/display/hm17cm256-atomic-sweeps",
                   test_hm17cm256_atomic_sweeps);
    qtest_add_func("/x55/display/hm17cm256-sweep-fallbacks",
                   test_hm17cm256_sweep_fallbacks);
    qtest_add_func("/x55/display/s6b33bx-compatible",
                   test_s6b33bx_compatible);
    qtest_add_func("/x55/display/pcf8833-viewport", test_pcf8833_viewport);
    qtest_add_func("/x55/display/pcf8833-compatible",
                   test_pcf8833_compatible);
    qtest_add_func("/x55/display/fragmented-windows",
                   test_fragmented_color_windows);
    qtest_add_func("/x55/display/hm17cm4096-packed-sweep",
                   test_hm17cm4096_packed_sweep);
    qtest_add_func("/x55/twi/register-file", test_twi_register_file);
    qtest_add_func("/x55/boards/matrix", test_production_board_matrix);
    qtest_add_func("/x55/battery/all-board-routes",
                   test_all_board_battery_routes);
    qtest_add_func("/x55/boards/inputs-and-alternates",
                   test_production_board_inputs_and_alternates);
    qtest_add_func("/x55/boards/c55-normalized-surface",
                   test_c55_normalized_surface);
    return g_test_run();
}
