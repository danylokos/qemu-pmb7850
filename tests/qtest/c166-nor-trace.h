/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef C166_NOR_TRACE_TEST_H
#define C166_NOR_TRACE_TEST_H

/*
 * Native trace records are flushed by QEMU's log backend. Reading after a
 * qtest acknowledgement also permits checking delayed-operation frontiers.
 */
static char *nor_trace_arguments(char **path)
{
    int fd = g_file_open_tmp("c166-nor-trace-XXXXXX", path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    return g_strdup_printf("-trace enable=x55_nor_mutation -D %s", *path);
}

static void nor_trace_assert(const char *path, const char *const expected[],
                             size_t count)
{
    g_autofree char *text = NULL;
    g_auto(GStrv) lines = NULL;
    size_t seen = 0;

    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    lines = g_strsplit(text, "\n", -1);
    for (char **line = lines; *line; line++) {
        const char *record;

        if (!g_str_has_prefix(*line, "x55_nor_mutation ")) {
            continue;
        }
        g_assert_cmpuint(seen, <, count);
        record = strstr(*line, "chip=");
        g_assert_nonnull(record);
        g_assert_cmpstr(record, ==, expected[seen++]);
    }
    g_assert_cmpuint(seen, ==, count);
}

#endif
