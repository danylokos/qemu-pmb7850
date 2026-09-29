/*
 * C166 M58-family NOR flash qtests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "c166-nor-trace.h"

#define FLASH_SIZE        (4 * MiB)
#define FLASH_BLOCK       (128 * KiB)
#define FLASH_NATIVE      0x800000
#define FLASH_NATIVE_FOLD 0xc00000
#define FLASH_TYPE        "m58-compatible-32mbit"
#define FACTORY_UID       "00000000C5B863B3"

static QTestState *m58_start(const char *extra)
{
    return qtest_initf("-M pmb7850-test,flash-type=" FLASH_TYPE
                       " -display none -global " FLASH_TYPE
                       ".factory-uid=" FACTORY_UID " %s", extra ?: "");
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

static void fill_file(int fd, uint8_t value, size_t size)
{
    g_autofree uint8_t *buffer = g_malloc(64 * KiB);

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

static void test_views_identity_cfi_and_folding(void)
{
    QTestState *qts = m58_start(NULL);

    qtest_writeb(qts, 0, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0xffff);

    qtest_writeb(qts, FLASH_NATIVE, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0x0020);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 2), ==, 0x0017);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x100), ==, 0x00fe);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x102), ==, 0x0000);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x104), ==, 0x0000);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x106), ==, 0xb8c5);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x108), ==, 0xb363);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE_FOLD), ==, 0x0020);
    g_assert_cmphex(qtest_readw(qts, 0), ==, 0xffff);

    qtest_writeb(qts, FLASH_NATIVE, 0x98);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x20), ==, 'Q');
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x22), ==, 'R');
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x24), ==, 'Y');
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x4e), ==, 0x16);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x58), ==, 1);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x5a), ==, 31);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x5c), ==, 0);
    flash_array_mode(qts);

    flash_program_byte(qts, 0x1000, 0x5a);
    g_assert_cmphex(qtest_readb(qts, 0x1000), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, FLASH_NATIVE_FOLD + 0x1000), ==, 0x5a);
    qtest_quit(qts);
}

static void test_program_erase_buffer_and_reset(void)
{
    g_autofree char *trace_path = NULL;
    g_autofree char *trace_args = nor_trace_arguments(&trace_path);
    QTestState *qts = m58_start(trace_args);
    const char *const expected[] = {
        "chip=0 offset=0x200 size=1 operation=program value=0x12",
        "chip=0 offset=0x3ffffe size=2 operation=program value=0xa55a",
        "chip=0 offset=0x200 size=1 operation=program value=0x12",
        "chip=0 offset=0x20020 size=1 operation=program value=0x34",
        "chip=0 offset=0x40020 size=1 operation=program value=0x56",
        "chip=0 offset=0x20000 size=131072 operation=erase value=0x0",
        "chip=0 offset=0x60020 size=2 operation=program value=0x1234",
        "chip=0 offset=0x60022 size=2 operation=program value=0x5678",
        "chip=0 offset=0x60024 size=2 operation=program value=0x9abc",
    };
    uint32_t final_word = FLASH_SIZE - 2;
    uint32_t erase_target = FLASH_BLOCK + 0x20;
    uint32_t erase_control = 2 * FLASH_BLOCK + 0x20;

    flash_program_byte(qts, 0x200, 0x12);
    qtest_writeb(qts, FLASH_NATIVE + final_word, 0x40);
    qtest_writew(qts, FLASH_NATIVE + final_word, 0xa55a);
    flash_array_mode(qts);
    g_assert_cmphex(qtest_readb(qts, 0x200), ==, 0x12);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + final_word), ==, 0xa55a);

    qtest_writeb(qts, FLASH_NATIVE + 0x200, 0x40);
    qtest_writeb(qts, FLASH_NATIVE + 0x200, 0xff);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0x0090);
    qtest_writeb(qts, FLASH_NATIVE, 0x50);
    flash_array_mode(qts);
    g_assert_cmphex(qtest_readb(qts, 0x200), ==, 0x12);

    flash_program_byte(qts, erase_target, 0x34);
    flash_program_byte(qts, erase_control, 0x56);
    qtest_writeb(qts, FLASH_NATIVE + erase_target, 0x20);
    qtest_writeb(qts, FLASH_NATIVE + erase_target, 0xd0);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0x0080);
    flash_array_mode(qts);
    g_assert_cmphex(qtest_readb(qts, erase_target), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, erase_control), ==, 0x56);

    qtest_writeb(qts, FLASH_NATIVE + 0x60020, 0xe8);
    qtest_writew(qts, FLASH_NATIVE + 0x60020, 2);
    qtest_writew(qts, FLASH_NATIVE + 0x60020, 0x1234);
    qtest_writew(qts, FLASH_NATIVE + 0x60022, 0x5678);
    qtest_writew(qts, FLASH_NATIVE + 0x60024, 0x9abc);
    qtest_writew(qts, FLASH_NATIVE + 0x60024, 0x00d0);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0x0080);
    flash_array_mode(qts);
    g_assert_cmphex(qtest_readw(qts, 0x60020), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, 0x60022), ==, 0x5678);
    g_assert_cmphex(qtest_readw(qts, 0x60024), ==, 0x9abc);

    qtest_writeb(qts, FLASH_NATIVE, 0x90);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, 0x200), ==, 0x12);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + final_word), ==, 0xa55a);
    qtest_quit(qts);
    nor_trace_assert(trace_path, expected, G_N_ELEMENTS(expected));
    unlink(trace_path);
}

static void test_property_validation(void)
{
    const char *qemu = qtest_qemu_binary(NULL);
    g_autofree char *path = NULL;
    g_autofree char *drive = NULL;
    int fd = g_file_open_tmp("c166-m58-invalid-XXXXXX", &path, NULL);
    const char *const bad_uid_argv[] = {
        qemu, "-M", "pmb7850-test,flash-type=" FLASH_TYPE,
        "-display", "none", "-global",
        FLASH_TYPE ".factory-uid=not-a-uid", NULL,
    };
    const char *bad_size_argv[8] = {
        qemu, "-M", "pmb7850-test,flash-type=" FLASH_TYPE,
        "-display", "none", "-drive",
    };

    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, 8 * MiB);
    close(fd);
    drive = g_strdup_printf("if=pflash,format=raw,file=%s", path);
    bad_size_argv[6] = drive;
    assert_qemu_rejects(bad_uid_argv);
    assert_qemu_rejects(bad_size_argv);
    unlink(path);
}

static void test_backend_persistence(void)
{
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    int fd = g_file_open_tmp("c166-m58-XXXXXX", &path, NULL);
    uint8_t value;
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, FLASH_SIZE);
    close(fd);
    args = g_strdup_printf("-drive if=pflash,format=raw,file=%s", path);
    qts = m58_start(args);
    flash_program_byte(qts, FLASH_SIZE - 1, 0x5a);
    qtest_quit(qts);
    fd = open(path, O_RDONLY);
    g_assert_cmpint(pread(fd, &value, 1, FLASH_SIZE - 1), ==, 1);
    g_assert_cmphex(value, ==, 0x5a);
    close(fd);
    unlink(path);
}

static void test_snapshot_preserves_source(void)
{
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    int fd = g_file_open_tmp("c166-m58-snapshot-XXXXXX", &path, NULL);
    uint8_t value;
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, FLASH_SIZE);
    close(fd);
    args = g_strdup_printf("-drive if=pflash,format=raw,file=%s -snapshot",
                           path);
    qts = m58_start(args);
    flash_program_byte(qts, FLASH_SIZE - 1, 0x5a);
    qtest_quit(qts);
    fd = open(path, O_RDONLY);
    g_assert_cmpint(pread(fd, &value, 1, FLASH_SIZE - 1), ==, 1);
    g_assert_cmphex(value, ==, 0xff);
    close(fd);
    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/m58-compatible/views-identity-cfi-folding",
                   test_views_identity_cfi_and_folding);
    qtest_add_func("/m58-compatible/program-erase-buffer-reset",
                   test_program_erase_buffer_and_reset);
    qtest_add_func("/m58-compatible/property-validation",
                   test_property_validation);
    qtest_add_func("/m58-compatible/persistence",
                   test_backend_persistence);
    qtest_add_func("/m58-compatible/snapshot",
                   test_snapshot_preserves_source);
    return g_test_run();
}
