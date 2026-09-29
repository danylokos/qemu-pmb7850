/*
 * C166S TCG helper declarations.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

DEF_HELPER_1(c166_idle, void, env)
DEF_HELPER_1(c166_einit, void, env)
DEF_HELPER_1(c166_srst, void, env)
DEF_HELPER_2(c166_write_syscon, void, env, i32)
DEF_HELPER_3(c166_trace_window, void, env, i32, i32)
DEF_HELPER_5(c166_data_read, void, env, i32, i32, i32, i32)
DEF_HELPER_4(c166_probe, void, env, i32, i32, i32)
DEF_HELPER_FLAGS_2(c166_stack_address, TCG_CALL_NO_RWG_SE, i32, i32, i32)
DEF_HELPER_2(c166_trace_entry, void, env, i32)
DEF_HELPER_5(c166_trace_access, void, env, i32, i32, i32, i32)
DEF_HELPER_3(c166_trap_retire, i32, env, i32, i32)
DEF_HELPER_1(c166_trap_resume, void, env)
DEF_HELPER_1(c166_trap_return, void, env)
DEF_HELPER_2(c166_stack_arithmetic, void, env, i32)
