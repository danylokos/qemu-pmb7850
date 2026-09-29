# SPDX-License-Identifier: GPL-2.0-or-later

c166_ss = ss.source_set()
c166_ss.add(when: 'CONFIG_C166_TEST', if_true: files('c166_test.c'))
hw_arch += {'c166': c166_ss}
