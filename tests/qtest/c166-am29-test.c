/*
 * C166 AMD AM29LV NOR flash qtests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "c166-nor-trace.h"

#define FLASH_TYPE "am29lv640mh"
#define FLASH128_TYPE "am29lv128mh"
#define FLASH_SIZE (8 * MiB)
#define FLASH128_SIZE (16 * MiB)
#define FLASH_SECTOR (64 * KiB)
#define FLASH_NATIVE 0x800000
#define CLOCK_TIMEOUT_NS ((UINT64_C(8) * UINT64_C(1000000000)) / 26000000)
#define CLOCK_ERASE_NS ((UINT64_C(80000) * UINT64_C(1000000000)) / 26000000)
#define INSTRUCTION_TIMEOUT_NS UINT64_C(8)
#define INSTRUCTION_ERASE_NS UINT64_C(80000)
#define ERASE_NS CLOCK_ERASE_NS

static char *factory_secsi(void)
{
    GString *text = g_string_new("A0A1A2A3A4A5A6A7A8A9AAABACADAEAF");

    for (unsigned i = 16; i < 256; i++) {
        g_string_append(text, "FF");
    }
    return g_string_free(text, false);
}

static QTestState *am29_start_type(const char *type, const char *extra)
{
    g_autofree char *secsi = factory_secsi();

    return qtest_initf("-M pmb7850-test,flash-type=%s"
                       " -display none -global %s.factory-secsi=%s %s",
                       type, type, secsi, extra ?: "");
}

static QTestState *am29_start(const char *extra)
{
    return am29_start_type(FLASH_TYPE, extra);
}

static QTestState *am29_start_timing(bool instruction_timing)
{
    return am29_start(instruction_timing ?
        "-global " FLASH_TYPE ".instruction-timing=true" : NULL);
}

static void unlock_command_at(QTestState *qts, uint32_t offset,
                              uint8_t command)
{
    qtest_writeb(qts, FLASH_NATIVE + 0xaaa, 0xaa);
    qtest_writeb(qts, FLASH_NATIVE + 0x555, 0x55);
    qtest_writeb(qts, FLASH_NATIVE + offset, command);
}

static void unlock_command(QTestState *qts, uint8_t command)
{
    unlock_command_at(qts, 0xaaa, command);
}

static void array_mode(QTestState *qts)
{
    qtest_writeb(qts, FLASH_NATIVE, 0xf0);
}

static void program_word(QTestState *qts, uint32_t offset, uint16_t value)
{
    unlock_command(qts, 0xa0);
    qtest_writew(qts, FLASH_NATIVE + offset, value);
}

static void sector_erase(QTestState *qts, uint32_t offset)
{
    unlock_command(qts, 0x80);
    qtest_writeb(qts, FLASH_NATIVE + 0xaaa, 0xaa);
    qtest_writeb(qts, FLASH_NATIVE + 0x555, 0x55);
    qtest_writeb(qts, FLASH_NATIVE + offset, 0x30);
}

static void unlock128_command_at(QTestState *qts, uint32_t offset,
                                 uint8_t command)
{
    qtest_writeb(qts, 0xaaa, 0xaa);
    qtest_writeb(qts, 0x555, 0x55);
    qtest_writeb(qts, offset, command);
}

static void unlock128_command(QTestState *qts, uint8_t command)
{
    unlock128_command_at(qts, 0xaaa, command);
}

static void program128_word(QTestState *qts, uint32_t offset, uint16_t value)
{
    unlock128_command(qts, 0xa0);
    qtest_writew(qts, offset, value);
}

static void sector128_erase(QTestState *qts, uint32_t offset)
{
    unlock128_command(qts, 0x80);
    qtest_writeb(qts, 0xaaa, 0xaa);
    qtest_writeb(qts, 0x555, 0x55);
    qtest_writeb(qts, offset, 0x30);
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

static void test_views_identity_protection_and_secsi(void)
{
    QTestState *qts = am29_start(NULL);

    qtest_writeb(qts, 0xaaa, 0xaa);
    qtest_writeb(qts, 0x555, 0x55);
    qtest_writeb(qts, 0xaaa, 0x90);
    g_assert_cmphex(qtest_readw(qts, 0), ==, 0xffff);

    unlock_command(qts, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0x0001);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 2), ==, 0x227e);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x1c), ==, 0x220c);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x1e), ==, 0x2201);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 4), ==, 1);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 8), ==, 1);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 0x40004), ==, 0);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + 6), ==, 0x0098);
    g_assert_cmphex(qtest_readw(qts, 0), ==, 0xffff);

    array_mode(qts);
    unlock_command(qts, 0x88);
    for (unsigned i = 0; i < 16; i++) {
        g_assert_cmphex(qtest_readb(qts, FLASH_NATIVE + i), ==, 0xa0 + i);
    }
    g_assert_cmphex(qtest_readb(qts, FLASH_NATIVE + 16), ==, 0xff);
    unlock_command(qts, 0x90);
    qtest_writeb(qts, FLASH_NATIVE, 0x00);
    g_assert_cmphex(qtest_readb(qts, FLASH_NATIVE), ==, 0xff);
    qtest_quit(qts);
}

static void test_program_buffer_erase_suspend_and_reset(void)
{
    g_autofree char *trace_path = NULL;
    g_autofree char *trace_args = nor_trace_arguments(&trace_path);
    QTestState *qts = am29_start(trace_args);
    const char *const expected[] = {
        "chip=0 offset=0x40020 size=2 operation=program value=0x1234",
        "chip=0 offset=0x7ffffe size=2 operation=program value=0xa55a",
        "chip=0 offset=0x40020 size=2 operation=program value=0x1030",
        "chip=0 offset=0x40020 size=2 operation=program value=0x0",
        "chip=0 offset=0x40022 size=2 operation=program value=0x403",
        "chip=0 offset=0x40024 size=2 operation=program value=0x605",
        "chip=0 offset=0x50020 size=2 operation=program value=0x5678",
        "chip=0 offset=0x50020 size=2 operation=program value=0x1230",
        "chip=0 offset=0x40000 size=65536 operation=erase value=0x0",
    };
    uint32_t target = 4 * FLASH_SECTOR + 0x20;
    uint32_t control = 5 * FLASH_SECTOR + 0x20;

    program_word(qts, target, 0x1234);
    program_word(qts, FLASH_SIZE - 2, 0xa55a);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE + FLASH_SIZE - 2),
                    ==, 0xa55a);
    program_word(qts, target, 0xf0f0);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0x1030);

    program_word(qts, 0x100, 0x0000);
    g_assert_cmphex(qtest_readw(qts, 0x100), ==, 0xffff);

    unlock_command_at(qts, target, 0x25);
    qtest_writew(qts, FLASH_NATIVE + target, 2);
    qtest_writew(qts, FLASH_NATIVE + target, 0x0201);
    qtest_writew(qts, FLASH_NATIVE + target + 2, 0x0403);
    qtest_writew(qts, FLASH_NATIVE + target + 4, 0x0605);
    qtest_writeb(qts, FLASH_NATIVE + target, 0x29);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0x0000);
    g_assert_cmphex(qtest_readw(qts, target + 2), ==, 0x0403);
    g_assert_cmphex(qtest_readw(qts, target + 4), ==, 0x0605);

    program_word(qts, control, 0x5678);
    sector_erase(qts, target);
    uint8_t first = qtest_readb(qts, FLASH_NATIVE + target);
    uint8_t second = qtest_readb(qts, FLASH_NATIVE + target);
    g_assert_cmphex((first ^ second) & 0x44, ==, 0x44);
    qtest_writeb(qts, FLASH_NATIVE, 0xb0);
    g_assert_cmphex(qtest_readw(qts, control), ==, 0x5678);
    first = qtest_readb(qts, FLASH_NATIVE + target);
    second = qtest_readb(qts, FLASH_NATIVE + target);
    g_assert_cmphex((first ^ second) & 0x04, ==, 0x04);
    g_assert_cmphex(first & 0xc0, ==, 0x80);
    g_assert_cmphex(second & 0xc0, ==, 0x80);

    unlock_command(qts, 0x90);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0x0001);
    array_mode(qts);
    first = qtest_readb(qts, FLASH_NATIVE + target);
    second = qtest_readb(qts, FLASH_NATIVE + target);
    g_assert_cmphex((first ^ second) & 0x04, ==, 0x04);

    unlock_command(qts, 0x88);
    g_assert_cmphex(qtest_readw(qts, FLASH_NATIVE), ==, 0xffff);
    program_word(qts, control, 0x1234);
    g_assert_cmphex(qtest_readw(qts, control), ==, 0x1230);
    program_word(qts, target, 0xffff);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0x0000);

    qtest_clock_step(qts, ERASE_NS);
    g_assert_cmphex(qtest_readw(qts, control), ==, 0x1230);
    nor_trace_assert(trace_path, expected, G_N_ELEMENTS(expected) - 1);
    qtest_writeb(qts, FLASH_NATIVE + target, 0x30);
    qtest_clock_step(qts, ERASE_NS);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, control), ==, 0x1230);

    sector_erase(qts, control);
    qtest_system_reset(qts);
    qtest_clock_step(qts, ERASE_NS);
    g_assert_cmphex(qtest_readw(qts, control), ==, 0x1230);
    g_assert_cmphex(qtest_readw(qts, FLASH_SIZE - 2), ==, 0xa55a);
    qtest_quit(qts);
    nor_trace_assert(trace_path, expected, G_N_ELEMENTS(expected));
    unlink(trace_path);
}

static void assert_timing_domain(bool instruction_timing,
                                 uint64_t timeout_ns, uint64_t erase_ns)
{
    QTestState *qts = am29_start_timing(instruction_timing);
    uint32_t target = 4 * FLASH_SECTOR + 0x20;
    uint8_t first;
    uint8_t second;

    program_word(qts, target, 0x0000);
    sector_erase(qts, target);
    g_assert_cmphex(qtest_readb(qts, FLASH_NATIVE + target) & 0x08, ==, 0);
    qtest_clock_step(qts, timeout_ns - 1);
    g_assert_cmphex(qtest_readb(qts, FLASH_NATIVE + target) & 0x08, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, FLASH_NATIVE + target) & 0x08, ==, 0x08);

    qtest_writeb(qts, FLASH_NATIVE, 0xb0);
    first = qtest_readb(qts, FLASH_NATIVE + target);
    second = qtest_readb(qts, FLASH_NATIVE + target);
    g_assert_cmphex((first ^ second) & 0x04, ==, 0x04);
    g_assert_cmphex(first & 0xc0, ==, 0x80);
    g_assert_cmphex(second & 0xc0, ==, 0x80);
    qtest_clock_step(qts, erase_ns);
    first = qtest_readb(qts, FLASH_NATIVE + target);
    second = qtest_readb(qts, FLASH_NATIVE + target);
    g_assert_cmphex((first ^ second) & 0x04, ==, 0x04);

    qtest_writeb(qts, FLASH_NATIVE + target, 0x30);
    qtest_clock_step(qts, erase_ns - timeout_ns - 1);
    g_assert_cmphex(qtest_readb(qts, FLASH_NATIVE + target) & 0x80, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0xffff);
    qtest_quit(qts);
}

static void test_clock_timed_deadlines(void)
{
    assert_timing_domain(false, CLOCK_TIMEOUT_NS, CLOCK_ERASE_NS);
}

static void test_instruction_timed_deadlines(void)
{
    assert_timing_domain(true, INSTRUCTION_TIMEOUT_NS,
                         INSTRUCTION_ERASE_NS);
}

static void test_validation_persistence_and_snapshot(void)
{
    const char *qemu = qtest_qemu_binary(NULL);
    g_autofree char *path = NULL;
    g_autofree char *drive = NULL;
    g_autofree char *args = NULL;
    int fd = g_file_open_tmp("c166-am29-XXXXXX", &path, NULL);
    const char *const bad_secsi_argv[] = {
        qemu, "-M", "pmb7850-test,flash-type=" FLASH_TYPE,
        "-display", "none", "-global",
        FLASH_TYPE ".factory-secsi=not-secsi", NULL,
    };
    const char *bad_size_argv[8] = {
        qemu, "-M", "pmb7850-test,flash-type=" FLASH_TYPE,
        "-display", "none", "-drive",
    };
    uint8_t value;
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, FLASH_SIZE);
    close(fd);
    drive = g_strdup_printf("if=pflash,format=raw,file=%s", path);
    bad_size_argv[6] = drive;
    assert_qemu_rejects(bad_secsi_argv);

    fd = open(path, O_WRONLY | O_TRUNC);
    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, 4 * MiB);
    close(fd);
    assert_qemu_rejects(bad_size_argv);

    fd = open(path, O_WRONLY | O_TRUNC);
    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, FLASH_SIZE);
    close(fd);
    args = g_strdup_printf("-drive if=pflash,format=raw,file=%s", path);
    qts = am29_start(args);
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
    qts = am29_start(args);
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
    QTestState *qts = am29_start_type(FLASH128_TYPE, NULL);
    uint32_t target = 0x10020;
    uint32_t control = 0x20020;

    unlock128_command(qts, 0x90);
    g_assert_cmphex(qtest_readw(qts, 0), ==, 0x0001);
    g_assert_cmphex(qtest_readw(qts, 2), ==, 0x227e);
    g_assert_cmphex(qtest_readw(qts, 0x1c), ==, 0x2212);
    g_assert_cmphex(qtest_readw(qts, 0x1e), ==, 0x2200);
    g_assert_cmphex(qtest_readw(qts, 4), ==, 0);
    g_assert_cmphex(qtest_readw(qts, 8 * MiB + 4), ==, 1);
    qtest_writeb(qts, 0, 0xf0);

    unlock128_command(qts, 0x88);
    g_assert_cmphex(qtest_readb(qts, 0), ==, 0xa0);
    g_assert_cmphex(qtest_readb(qts, 15), ==, 0xaf);
    unlock128_command(qts, 0x90);
    qtest_writeb(qts, 0, 0x00);

    program128_word(qts, target, 0x1234);
    program128_word(qts, FLASH128_SIZE - 2, 0xa55a);
    program128_word(qts, 8 * MiB + 0x100, 0x0000);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, FLASH128_SIZE - 2), ==, 0xa55a);
    g_assert_cmphex(qtest_readw(qts, 8 * MiB + 0x100), ==, 0xffff);

    unlock128_command_at(qts, target, 0x25);
    qtest_writew(qts, target, 1);
    qtest_writew(qts, target, 0x0201);
    qtest_writew(qts, target + 2, 0x0403);
    qtest_writeb(qts, target, 0x29);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0x0200);
    g_assert_cmphex(qtest_readw(qts, target + 2), ==, 0x0403);

    program128_word(qts, control, 0x5678);
    sector128_erase(qts, target);
    qtest_clock_step(qts, ERASE_NS);
    g_assert_cmphex(qtest_readw(qts, target), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, control), ==, 0x5678);

    unlock128_command(qts, 0x90);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readw(qts, control), ==, 0x5678);
    g_assert_cmphex(qtest_readw(qts, FLASH128_SIZE - 2), ==, 0xa55a);
    qtest_quit(qts);
}

static void test_128_validation_persistence_and_snapshot(void)
{
    const char *qemu = qtest_qemu_binary(NULL);
    g_autofree char *path = NULL;
    g_autofree char *drive = NULL;
    g_autofree char *args = NULL;
    int fd = g_file_open_tmp("c166-am29-128-XXXXXX", &path, NULL);
    const char *const bad_secsi_argv[] = {
        qemu, "-M", "pmb7850-test,flash-type=" FLASH128_TYPE,
        "-display", "none", "-global",
        FLASH128_TYPE ".factory-secsi=not-secsi", NULL,
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
    assert_qemu_rejects(bad_secsi_argv);
    assert_qemu_rejects(bad_size_argv);

    fd = open(path, O_WRONLY | O_TRUNC);
    g_assert_cmpint(fd, >=, 0);
    fill_file(fd, 0xff, FLASH128_SIZE);
    close(fd);
    args = g_strdup_printf("-drive if=pflash,format=raw,file=%s", path);
    qts = am29_start_type(FLASH128_TYPE, args);
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
    qts = am29_start_type(FLASH128_TYPE, args);
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
    qtest_add_func("/am29lv640/views-identity-protection-secsi",
                   test_views_identity_protection_and_secsi);
    qtest_add_func("/am29lv640/program-buffer-erase-suspend-reset",
                   test_program_buffer_erase_suspend_and_reset);
    qtest_add_func("/am29lv640/clock-timed-deadlines",
                   test_clock_timed_deadlines);
    qtest_add_func("/am29lv640/instruction-timed-deadlines",
                   test_instruction_timed_deadlines);
    qtest_add_func("/am29lv640/validation-persistence-snapshot",
                   test_validation_persistence_and_snapshot);
    qtest_add_func("/am29lv128/geometry-identity-commands",
                   test_128_geometry_identity_and_commands);
    qtest_add_func("/am29lv128/validation-persistence-snapshot",
                   test_128_validation_persistence_and_snapshot);
    return g_test_run();
}
