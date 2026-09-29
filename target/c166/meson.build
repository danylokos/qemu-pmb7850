# SPDX-License-Identifier: GPL-2.0-or-later

gen = [
  decodetree.process('insns.decode', extra_args: [ '--insnwidth', '16' ])
]

c166_ss = ss.source_set()
c166_ss.add(gen)
c166_ss.add(files(
  'cpu.c',
  'disas.c',
  'op_helper.c',
  'translate.c'))

c166_common_system_ss = ss.source_set()
c166_common_system_ss.add(files('gdbstub.c'))

target_arch += {'c166': c166_ss}
target_common_system_arch += {'c166': c166_common_system_ss}
