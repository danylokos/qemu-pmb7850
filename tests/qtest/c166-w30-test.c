/*
 * C166 Intel W30-family NOR flash qtests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "c166-nor-trace.h"

#define FLASH_TYPE     "w30-64mbit-top"
#define FLASH128_TYPE  "w30-128mbit-top"
#define FLASH_SIZE     (8 * MiB)
#define FLASH128_SIZE  (16 * MiB)
#define FLASH_NATIVE   0x800000
#define PARTITION_SIZE (512 * KiB)
#define PARAMETER_BASE 0x7f0000
#define PARAMETER_SIZE (8 * KiB)
#define FACTORY_UID    "0001020304050607"
#define PROGRAM_NS     ((UINT64_C(8) * UINT64_C(1000000000)) / 26000000)
#define ERASE_NS       ((UINT64_C(80000) * UINT64_C(1000000000)) / 26000000)

static QTestState *w30_start_type(const char *type, const char *extra)
{
    return qtest_initf("-M pmb7850-test,flash-type=%s"
                       " -display none -global %s.factory-uid=" FACTORY_UID
                       " %s", type, type, extra ?: "");
}

static QTestState *w30_start(const char *extra)
{
    return w30_start_type(FLASH_TYPE, extra);
}

static void command(QTestState *qts, uint32_t offset, uint8_t value)
{
    qtest_writeb(qts, FLASH_NATIVE + offset, value);
}

static void unlock_block(QTestState *qts, uint32_t offset)
{
    command(qts, offset, 0x60);
    command(qts, offset, 0xd0);
}

static void program_word(QTestState *qts, uint32_t offset, uint16_t value)
{
    unlock_block(qts, offset);
    command(qts, offset, 0x40);
    qtest_writew(qts, FLASH_NATIVE + offset, value);
    qtest_clock_step(qts, MAX(PROGRAM_NS, 1));
}

static void start_erase(QTestState *qts, uint32_t offset)
{
    unlock_block(qts, offset);
    command(qts, offset, 0x20);
    command(qts, offset, 0xd0);
}

static void command128(QTestState *qts, uint32_t offset, uint8_t value)
{
    qtest_writeb(qts, offset, value);
}

static void unlock128_block(QTestState *qts, uint32_t offset)
{
    command128(qts, offset, 0x60);
    command128(qts, offset, 0xd0);
}

static void program128_word(QTestState *qts, uint32_t offset, uint16_t value)
{
    unlock128_block(qts, offset);
    command128(qts, offset, 0x40);
    qtest_writew(qts, offset, value);
    qtest_clock_step(qts, MAX(PROGRAM_NS, 1));
    command128(qts, offset, 0xff);
}

static void start128_erase(QTestState *qts, uint32_t offset)
{
    unlock128_block(qts, offset);
    command128(qts, offset, 0x20);
    command128(qts, offset, 0xd0);
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

static void test_views_identity_cfi_locks_and_configuration(void)
{
    QTestState *qts = w30_start(NULL);

    qtest_writeb(qts, 0x390000, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x390000), ==, 0xffff);

    command(qts, 0x390000, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x390000), ==, 0x0089);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x390002), ==, 0x8854);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x390004), ==, 1);
    g_assert_cmphex(qtest_readw(qts, 0x390000), ==, 0xffff);

    command(qts, 0x380000, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x380102), ==, 0x0100);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x380104), ==, 0x0302);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x380106), ==, 0x0504);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x380108), ==, 0x0706);

    unlock_block(qts, 0x390000);
    command(qts, 0x390000, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x390004), ==, 0);
    command(qts, 0x390000, 0x60);
    command(qts, 0x390000, 0x2f);
    command(qts, 0x390000, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x390004), ==, 3);
    unlock_block(qts, 0x390000);
    command(qts, 0x390000, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x390004), ==, 2);

    command(qts, 0x400000, 0x98);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x400020), ==, 'Q');
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x400022), ==, 'R');
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x400024), ==, 'Y');
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x40004e), ==, 0x17);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x400058), ==, 0x11);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x4000a4), ==, 0x0f);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x4000ae), ==, 0x0f);

    command(qts, 0x2468, 0x60);
    command(qts, 0x2468, 0x03);
    command(qts, 0, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x0a), ==, 0x1234);
    qtest_system_reset(qts);
    command(qts, 0, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x0a), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 4), ==, 1);
    qtest_quit(qts);
}

static void test_program_erase_suspend_and_reset(void)
{
    g_autofree char *trace_path = NULL;
    g_autofree char *trace_args = nor_trace_arguments(&trace_path);
    QTestState *qts = w30_start(trace_args);
    const char *const expected[] = {
        "chip=0 offset=0x390020 size=2 operation=program value=0x1234",
        "chip=0 offset=0x7ffffe size=2 operation=program value=0xa55a",
        "chip=0 offset=0x390020 size=2 operation=program value=0x1030",
        "chip=0 offset=0x38faf2 size=2 operation=program value=0x10",
        "chip=0 offset=0x390000 size=65536 operation=erase value=0x0",
        "chip=0 offset=0x7f4000 size=2 operation=program value=0x56",
        "chip=0 offset=0x7f1ba4 size=2 operation=program value=0x34",
        "chip=0 offset=0x780100 size=2 operation=program value=0x5678",
        "chip=0 offset=0x7f4000 size=8192 operation=erase value=0x0",
    };
    uint32_t target = 0x390020;
    uint32_t control = 0x38faf2;
    uint32_t parameter_target = PARAMETER_BASE + 2 * PARAMETER_SIZE;
    uint32_t parameter_control = PARAMETER_BASE + 0x1ba4;

    program_word(qts, target, 0x1234);
    program_word(qts, FLASH_SIZE - 2, 0xa55a);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, FLASH_SIZE - 2), ==, 0xa55a);
    program_word(qts, target, 0xf0f0);
    command(qts, target, 0x70);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + target), ==, 0x0090);
    command(qts, target, 0x50);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0x1030);

    program_word(qts, control, 0x0010);
    start_erase(qts, target);
    command(qts, target, 0x98);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + target), ==, 0);
    command(qts, 0, 0x70);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0x0001);
    command(qts, target, 0x70);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + target), ==, 0);
    nor_trace_assert(trace_path, expected, 4);
    qtest_clock_step(qts, ERASE_NS);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, control), ==, 0x0010);

    program_word(qts, parameter_target, 0x0056);
    program_word(qts, parameter_control, 0x0034);
    start_erase(qts, parameter_target);
    command(qts, parameter_target, 0xb0);
    command(qts, 0x780000, 0x70);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x780000) & 0x00c1,
                    ==, 0x00c0);
    program_word(qts, 0x780100, 0x5678);
    g_assert_cmphex(qtest_readw(qts, 0x780100), ==, 0x5678);
    command(qts, 0x10000, 0xd0);
    qtest_clock_step(qts, ERASE_NS);
    g_assert_cmphex(qtest_readw(qts, parameter_target), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, parameter_control), ==, 0x0034);

    unlock_block(qts, 0);
    command(qts, 0, 0x40);
    qtest_writew(qts, FLASH_NATIVE, 0x0000);
    command(qts, 0x10000, 0xb0);
    command(qts, 0, 0xb0);
    qtest_system_reset(qts);
    qtest_clock_step(qts, MAX(PROGRAM_NS, 1));
    g_assert_cmphex(qtest_readw(qts, 0), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, FLASH_SIZE - 2), ==, 0xa55a);
    command(qts, 0, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 4), ==, 1);
    qtest_quit(qts);
    nor_trace_assert(trace_path, expected, G_N_ELEMENTS(expected));
    unlink(trace_path);
}

static void test_protection_programming(void)
{
    g_autofree char *trace_path = NULL;
    g_autofree char *trace_args = nor_trace_arguments(&trace_path);
    QTestState *qts = w30_start(trace_args);

    command(qts, 0x10a, 0xc0);
    qtest_writew(qts, FLASH_NATIVE + 0x10a, 0x1234);
    qtest_clock_step(qts, MAX(PROGRAM_NS, 1));
    command(qts, 0, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x10a), ==, 0x1234);

    command(qts, 0x10a, 0xc0);
    qtest_writew(qts, FLASH_NATIVE + 0x10a, 0xffff);
    qtest_clock_step(qts, MAX(PROGRAM_NS, 1));
    command(qts, 0x10a, 0x70);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x10a), ==, 0x0090);
    command(qts, 0x10a, 0x50);

    command(qts, 0x100, 0xc0);
    qtest_writew(qts, FLASH_NATIVE + 0x100, 0xfffd);
    qtest_clock_step(qts, MAX(PROGRAM_NS, 1));
    command(qts, 0, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x100), ==, 0xfffc);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x102), ==, 0x0100);

    command(qts, PARTITION_SIZE + 0x10a, 0xc0);
    qtest_writew(qts, FLASH_NATIVE + PARTITION_SIZE + 0x10a, 0x0000);
    command(qts, PARTITION_SIZE, 0x70);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + PARTITION_SIZE),
                    ==, 0x0090);
    qtest_system_reset(qts);
    command(qts, 0, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x100), ==, 0xfffc);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x10a), ==, 0x1234);
    qtest_quit(qts);
    nor_trace_assert(trace_path, NULL, 0);
    unlink(trace_path);
}

static void test_validation_persistence_and_snapshot(void)
{
    const char *qemu = qtest_qemu_binary(NULL);
    g_autofree char *path = NULL;
    g_autofree char *drive = NULL;
    g_autofree char *args = NULL;
    int fd = g_file_open_tmp("c166-w30-XXXXXX", &path, NULL);
    const char *const bad_uid_argv[] = {
        qemu, "-M", "pmb7850-test,flash-type=" FLASH_TYPE,
        "-display", "none", "-global",
        FLASH_TYPE ".factory-uid=not-a-uid", NULL,
    };
    const char *bad_size_argv[8] = {
        qemu, "-M", "pmb7850-test,flash-type=" FLASH_TYPE,
        "-display", "none", "-drive",
    };
    uint8_t value;
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, 4 * MiB);
    close(fd);
    drive = g_strdup_printf("if=pflash,format=raw,file=%s", path);
    bad_size_argv[6] = drive;
    assert_qemu_rejects(bad_uid_argv);
    assert_qemu_rejects(bad_size_argv);

    fd = open(path, O_WRONLY | O_TRUNC);
    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, FLASH_SIZE);
    close(fd);
    args = g_strdup_printf("-drive if=pflash,format=raw,file=%s", path);
    qts = w30_start(args);
    program_word(qts, FLASH_SIZE - 2, 0xa55a);
    qtest_quit(qts);
    fd = open(path, O_RDONLY);
    g_assert_cmpint(pread(fd, &value, 1, FLASH_SIZE - 2), ==, 1);
    g_assert_cmphex(value, ==, 0x5a);
    close(fd);

    fd = open(path, O_WRONLY | O_TRUNC);
    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, FLASH_SIZE);
    close(fd);
    g_free(args);
    args = g_strdup_printf("-drive if=pflash,format=raw,file=%s -snapshot",
                           path);
    qts = w30_start(args);
    program_word(qts, FLASH_SIZE - 2, 0xa55a);
    qtest_quit(qts);
    fd = open(path, O_RDONLY);
    g_assert_cmpint(pread(fd, &value, 1, FLASH_SIZE - 2), ==, 1);
    g_assert_cmphex(value, ==, 0xff);
    close(fd);
    unlink(path);
}

static void test_128_geometry_identity_and_commands(void)
{
    QTestState *qts = w30_start_type(FLASH128_TYPE, NULL);
    uint32_t target = 0x7d0020;
    uint32_t parameter_target = 0xff4000;
    uint32_t parameter_control = 0xff1ba4;

    command128(qts, 0x7d0000, 0x90);
    g_assert_cmphex(qtest_readw(qts, 0x7d0000), ==, 0x0089);
    g_assert_cmphex(qtest_readw(qts, 0x7d0002), ==, 0x8856);
    g_assert_cmphex(qtest_readw(qts, 0x7d0004), ==, 1);
    g_assert_cmphex(qtest_readw(qts, 0x7d0100), ==, 0xfffe);
    g_assert_cmphex(qtest_readw(qts, 0x7d0102), ==, 0x0100);
    command128(qts, 0xff0000, 0x90);
    g_assert_cmphex(qtest_readw(qts, 0xff0100), ==, 0xfffe);
    g_assert_cmphex(qtest_readw(qts, 0xff010a), ==, 0xffff);

    command128(qts, 0x7d0000, 0x98);
    g_assert_cmphex(qtest_readw(qts, 0x780020), ==, 'Q');
    g_assert_cmphex(qtest_readw(qts, 0x780022), ==, 'R');
    g_assert_cmphex(qtest_readw(qts, 0x780024), ==, 'Y');
    g_assert_cmphex(qtest_readw(qts, 0x78004e), ==, 0x18);
    g_assert_cmphex(qtest_readw(qts, 0x780058), ==, 0x21);
    g_assert_cmphex(qtest_readw(qts, 0x7800a4), ==, 0x1f);
    g_assert_cmphex(qtest_readw(qts, 0x7800ae), ==, 0x1f);

    program128_word(qts, target, 0x01fc);
    program128_word(qts, FLASH128_SIZE - 2, 0xa55a);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0x01fc);
    g_assert_cmphex(qtest_readw(qts, FLASH128_SIZE - 2), ==, 0xa55a);

    program128_word(qts, parameter_target, 0x0056);
    program128_word(qts, parameter_control, 0x0034);
    start128_erase(qts, parameter_target);
    qtest_clock_step(qts, ERASE_NS);
    command128(qts, parameter_target, 0xff);
    g_assert_cmphex(qtest_readw(qts, parameter_target), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, parameter_control), ==, 0x0034);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0x01fc);
    g_assert_cmphex(qtest_readw(qts, FLASH128_SIZE - 2), ==, 0xa55a);
    command128(qts, 0x7d0000, 0x90);
    g_assert_cmphex(qtest_readw(qts, 0x7d0004), ==, 1);
    qtest_quit(qts);
}

static void test_128_validation_persistence_and_snapshot(void)
{
    const char *qemu = qtest_qemu_binary(NULL);
    g_autofree char *path = NULL;
    g_autofree char *drive = NULL;
    g_autofree char *args = NULL;
    int fd = g_file_open_tmp("c166-w30-128-XXXXXX", &path, NULL);
    const char *const bad_uid_argv[] = {
        qemu, "-M", "pmb7850-test,flash-type=" FLASH128_TYPE,
        "-display", "none", "-global",
        FLASH128_TYPE ".factory-uid=not-a-uid", NULL,
    };
    const char *bad_size_argv[8] = {
        qemu, "-M", "pmb7850-test,flash-type=" FLASH128_TYPE,
        "-display", "none", "-drive",
    };
    uint8_t value;
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, FLASH_SIZE);
    close(fd);
    drive = g_strdup_printf("if=pflash,format=raw,file=%s", path);
    bad_size_argv[6] = drive;
    assert_qemu_rejects(bad_uid_argv);
    assert_qemu_rejects(bad_size_argv);

    fd = open(path, O_WRONLY | O_TRUNC);
    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, FLASH128_SIZE);
    close(fd);
    args = g_strdup_printf("-drive if=pflash,format=raw,file=%s", path);
    qts = w30_start_type(FLASH128_TYPE, args);
    program128_word(qts, FLASH128_SIZE - 2, 0xa55a);
    qtest_quit(qts);
    fd = open(path, O_RDONLY);
    g_assert_cmpint(pread(fd, &value, 1, FLASH128_SIZE - 2), ==, 1);
    g_assert_cmphex(value, ==, 0x5a);
    close(fd);

    fd = open(path, O_WRONLY | O_TRUNC);
    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, FLASH128_SIZE);
    close(fd);
    g_free(args);
    args = g_strdup_printf("-drive if=pflash,format=raw,file=%s -snapshot",
                           path);
    qts = w30_start_type(FLASH128_TYPE, args);
    program128_word(qts, FLASH128_SIZE - 2, 0xa55a);
    qtest_quit(qts);
    fd = open(path, O_RDONLY);
    g_assert_cmpint(pread(fd, &value, 1, FLASH128_SIZE - 2), ==, 1);
    g_assert_cmphex(value, ==, 0xff);
    close(fd);
    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/w30-64/views-identity-cfi-locks-configuration",
                   test_views_identity_cfi_locks_and_configuration);
    qtest_add_func("/w30-64/program-erase-suspend-reset",
                   test_program_erase_suspend_and_reset);
    qtest_add_func("/w30-64/protection-programming",
                   test_protection_programming);
    qtest_add_func("/w30-64/validation-persistence-snapshot",
                   test_validation_persistence_and_snapshot);
    qtest_add_func("/w30-128/geometry-identity-commands",
                   test_128_geometry_identity_and_commands);
    qtest_add_func("/w30-128/validation-persistence-snapshot",
                   test_128_validation_persistence_and_snapshot);
    return g_test_run();
}
