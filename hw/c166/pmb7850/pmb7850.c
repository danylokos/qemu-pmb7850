/*
 * Infineon PMB7850 / E-GOLD+ V3 reusable SoC model.
 *
 * This is intentionally a silicon container, not a handset board.  External
 * memories and product devices are supplied through explicit links and buses.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "hw/c166/pmb7850/capcom.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "exec/icount.h"
#include "exec/cpu-interrupt.h"
#include "hw/c166/pmb7850/pmb7850.h"
#include "hw/block/x55_nor_trace.h"
#include "hw/block/x55_nor_view.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/irq.h"
#include "system/memory.h"
#include "system/qtest.h"
#include "trace/trace-hw_c166.h"

#define PMB_IO_BASE       0x00e000u
#define PMB_ESFR_BASE     0x00f000u
#define PMB_DPRAM_BASE    0x00f200u
#define PMB_DPRAM_SIZE    0x000c00u
#define PMB_DPRAM_EDGE_SIZE 0x000200u
#define PMB_DPRAM_RAM_OFFSET 0x000200u
#define PMB_DPRAM_RAM_SIZE 0x000800u
#define PMB_DPRAM_HIGH_OFFSET 0x000a00u
#define PMB_STACK_TRACE_BASE 0x00fbf8u
#define PMB_STACK_TRACE_SIZE 8u
#define PMB_SFR_BASE      0x00fe00u
#define PMB_LM_SIZE       (256 * KiB)

#define SYSCON            0xff12u
#define SYSCON_XPEN       (1u << 2)
#define SYSCON_ROMEN      (1u << 10)
#define SYSCON_ROMS1      (1u << 12)
#define XPERCON           0xf024u
#define XADRS1            0xf014u
#define XBCON1            0xf114u
#define ADDRSEL1          0xfe18u
#define BUSCON0           0xff0cu
#define BUSCON1           0xff14u
#define BUSACT            (1u << 10)

#define XIC_IR            (1u << 7)
#define XIC_IE            (1u << 6)
#define EOPIC             0xf180u
#define PECISNC           0xffa8u
#define PECXISNC          0xffbau

#define S0TBUF            0xfeb0u
#define S0RBUF            0xfeb2u
#define S0BG              0xfeb4u
#define S0FDV             0xfeb6u
#define S0TIC             0xff6cu
#define S0RIC             0xff6eu
#define S0EIC             0xff70u
#define S0CON             0xffb0u

#define SSC0TB            0xf0b0u
#define SSC0RB            0xf0b2u
#define SSC0BR            0xf0b4u
#define SSC0PISEL         0xf0b6u
#define SSC0TIC           0xff72u
#define SSC0RIC           0xff74u
#define SSC0EIC           0xff76u
#define SSC0CON           0xffb2u

#define SSC_CON_BM_MASK   0x000fu
#define SSC_CON_HB        (1u << 4)
#define SSC_CON_LB        (1u << 7)
#define SSC_CON_TE        (1u << 8)
#define SSC_CON_RE        (1u << 9)
#define SSC_CON_ERR_MASK  (0x0fu << 8)
#define SSC_CON_BSY       (1u << 12)
#define SSC_CON_MS        (1u << 14)
#define SSC_CON_EN        (1u << 15)

#define ADC_COMMAND_OFFSET 0x062u
#define ADC_RESULT0_OFFSET 0xf2cu
#define ADC_RESULT1_OFFSET 0xf2eu

#define P3                0xffc4u
#define DP3               0xffc6u
#define P6                0xffccu
#define DP6               0xffceu
#define P7                0xffd0u
#define DP7               0xffd2u
#define P8                0xffd4u
#define DP8               0xffd6u

#define TDMA_E020_OFFSET          0x020u
#define TDMA_FRAME_TOP_OFFSET     0x022u
#define TDMA_IRQ36_COMPARE_OFFSET 0x024u
#define TDMA_IRQ37_COMPARE_OFFSET 0x026u
#define TDMA_RESET_OFFSET         0x030u
#define TDMA_COUNTER_OFFSET       0x034u
#define TDMA_CONTROL_OFFSET       0x060u
#define TDMA_ENABLE               (1u << 1)
#define TDMA_IRQ36IC              0xf186u
#define TDMA_IRQ37IC              0xf18eu
#define TDMA_DIVISOR              12u

#define SIM_CTRL_OFFSET           0xf50u
#define SIM_STATUS_OFFSET         0xf54u
#define SIM_TX_OFFSET             0xf58u
#define SIM_RX_OFFSET             0xf5au
#define SIM_RESULT_HI_OFFSET      0xf60u
#define SIM_RESULT_LO_OFFSET      0xf62u
#define SIM_CTRL_ACTIVATE         (1u << 13)
#define SIM_BYTE_IC               0xf184u
#define SIM_STATUS_IC             0xf18cu
#define SIM_EVENT_IC              0xf194u
#define SIM_BYTE_TICKS            256u

#define PECC_PT           (1u << 15)
#define PECC_EOPINT       (1u << 14)
#define PECC_CL           (1u << 11)
#define PECC_INC_MASK     0x0600u
#define PECC_BWT          (1u << 8)

static void pmb_sync_peripherals(PMB7850State *s);
static void pmb_arm_peripheral_timer(PMB7850State *s);
static void pmb_update_fast_cs0(PMB7850State *s);
static void pmb_update_fast_cs(PMB7850State *s);
static void pmb_update_fast_lm(PMB7850State *s);
static void pmb_sim_tick(PMB7850State *s);
static uint64_t pmb_sim_next(PMB7850State *s);

static const uint16_t pecc_addr[16] = {
    0xfec0, 0xfec2, 0xfec4, 0xfec6, 0xfec8, 0xfeca, 0xfecc, 0xfece,
    0xfee8, 0xfeea, 0xfeec, 0xfeee, 0xfef8, 0xfefa, 0xfefc, 0xfefe,
};
static const uint16_t pecsn_addr[16] = {
    0xfed0, 0xfed2, 0xfed4, 0xfed6, 0xfed8, 0xfeda, 0xfedc, 0xfede,
    0xfee0, 0xfee2, 0xfee4, 0xfee6, 0xfeb8, 0xfeba, 0xfebc, 0xfebe,
};
static const uint16_t srcp_addr[16] = {
    0xfce0, 0xfce4, 0xfce8, 0xfcec, 0xfcf0, 0xfcf4, 0xfcf8, 0xfcfc,
    0xfcd0, 0xfcd4, 0xfcd8, 0xfcdc, 0xfcc0, 0xfcc4, 0xfcc8, 0xfccc,
};
static const uint16_t dstp_addr[16] = {
    0xfce2, 0xfce6, 0xfcea, 0xfcee, 0xfcf2, 0xfcf6, 0xfcfa, 0xfcfe,
    0xfcd2, 0xfcd6, 0xfcda, 0xfcde, 0xfcc2, 0xfcc6, 0xfcca, 0xfcce,
};

static uint16_t pmb_reg_raw(PMB7850State *s, hwaddr addr)
{
    uint16_t value;

    if (c166_cpu_csfr_read(CPU(&s->cpu), addr, &value)) {
        return value;
    }
    return s->regs[(addr - PMB_ESFR_BASE) >> 1];
}

static void pmb_update_irq(PMB7850State *s);

static void pmb_reg_store(PMB7850State *s, hwaddr addr, uint16_t value)
{
    if (c166_cpu_csfr_write(CPU(&s->cpu), addr, value)) {
        return;
    }
    s->regs[(addr - PMB_ESFR_BASE) >> 1] = value;
}

static int pmb_port_index(hwaddr addr)
{
    switch (addr) {
    case P3: return 0;
    case P6: return 1;
    case P7: return 2;
    case P8: return 3;
    default: return -1;
    }
}

static unsigned pmb_port_number(unsigned index)
{
    static const unsigned numbers[] = { 3, 6, 7, 8 };

    return numbers[index];
}

static uint16_t pmb_port_data_addr(unsigned index)
{
    static const uint16_t addresses[] = { P3, P6, P7, P8 };

    return addresses[index];
}

static uint16_t pmb_port_dir_addr(unsigned index)
{
    static const uint16_t dirs[] = { DP3, DP6, DP7, DP8 };
    return dirs[index];
}

static uint16_t pmb_reg_read_word(PMB7850State *s, hwaddr addr)
{
    uint16_t value = pmb_reg_raw(s, addr);
    int port = pmb_port_index(addr);

    if (port >= 0) {
        uint16_t dir = pmb_reg_raw(s, pmb_port_dir_addr(port));
        uint16_t mask = s->port_valid[port] & ~dir;

        value = (value & ~mask) | (s->port_input[port] & mask);
    }
    if (addr == S0RBUF) {
        s->asc_rx_full = false;
    } else if (addr == SSC0RB) {
        s->ssc_rb_full = false;
    }
    return value;
}

static int pmb_source_by_addr(PMB7850State *s, hwaddr addr)
{
    for (unsigned i = 0; i < s->num_irq_sources; i++) {
        if (s->irq_sources[i].addr == addr) {
            return i;
        }
    }
    return -1;
}

static int pmb_pec_channel(PMB7850State *s, uint16_t ic, unsigned ilvl)
{
    unsigned xglvl = (((ic >> 8) & 1) << 2) | (ic & 3);

    for (unsigned ch = 0; ch < 16; ch++) {
        uint16_t pecc = pmb_reg_raw(s, pecc_addr[ch]);
        unsigned plev = (pecc >> 12) & 3;
        unsigned channel_ilvl = 8 | ((((~plev) >> 1) & 1) << 2) |
                                (((~plev) & 1) << 1) | ((ch >> 2) & 1);
        unsigned channel_xglvl = (((ch >> 3) & 1) << 2) |
                                  (((ch >> 1) & 1) << 1) | (ch & 1);
        unsigned count = pecc & 0xff;

        if (channel_ilvl == ilvl && channel_xglvl == xglvl &&
            !(pecc & PECC_CL) && count != 0) {
            return ch;
        }
        if ((ch == 0 || ch == 2) && channel_ilvl == ilvl &&
            channel_xglvl == xglvl && !(pecc & PECC_CL) &&
            (pecc & PECC_PT) && count == 0 &&
            pmb_reg_raw(s, ch == 0 ? 0xfef0 : 0xfef2)) {
            return ch;
        }
    }
    return -1;
}

static void pmb_update_irq(PMB7850State *s)
{
    int winner = -1;
    int best_ilvl = -1;
    int best_xglvl = -1;
    bool pec = false;

    for (unsigned i = 0; i < s->num_irq_sources; i++) {
        uint16_t value = pmb_reg_raw(s, s->irq_sources[i].addr);
        int ilvl;
        int xglvl;

        if ((value & (XIC_IR | XIC_IE)) != (XIC_IR | XIC_IE)) {
            continue;
        }
        ilvl = (value >> 2) & 0xf;
        xglvl = (((value >> 8) & 1) << 2) | (value & 3);
        if (ilvl > best_ilvl || (ilvl == best_ilvl && xglvl > best_xglvl)) {
            winner = i;
            best_ilvl = ilvl;
            best_xglvl = xglvl;
        }
    }

    if (winner >= 0) {
        uint16_t ic = pmb_reg_raw(s, s->irq_sources[winner].addr);

        pec = pmb_pec_channel(s, ic, best_ilvl) >= 0;
        s->selected_source = winner;
        s->selected_payload = C166_IRQ_ASSERT |
            (pec ? C166_IRQ_PEC : 0) | (best_ilvl << 8) |
            s->irq_sources[winner].vector;
        trace_pmb7850_irq(s->cpu.env.guest_icount,
                          c166_env_get_pc(&s->cpu.env), "select", winner,
                          s->irq_sources[winner].vector, best_ilvl, pec);
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->cpu), 0),
                     s->selected_payload);
        if (s->instruction_timing && s->cpu.env.idle) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            int64_t requested = now + icount_to_ns(1);

            s->peripheral_deadline_sources = 1ull << 42;
            if (trace_event_get_state_backends(TRACE_PMB7850_DEADLINE)) {
                trace_pmb7850_deadline(s->cpu.env.guest_icount,
                    c166_env_get_pc(&s->cpu.env), s->timing_ticks, now,
                    s->peripheral_deadline_sources,
                    timer_expire_time_ns(&s->peripheral_timer), requested,
                    "idle-wake");
            }
            timer_mod_ns(&s->peripheral_timer, requested);
        }
    } else {
        s->selected_source = -1;
        s->selected_payload = 0;
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->cpu), 0), 0);
    }
}

static void pmb_set_ir(PMB7850State *s, hwaddr addr)
{
    pmb_reg_store(s, addr, pmb_reg_raw(s, addr) | XIC_IR);
    pmb_update_irq(s);
}

static void pmb_refresh_eopic(PMB7850State *s)
{
    uint16_t internal = pmb_reg_raw(s, PECISNC);
    uint16_t external = pmb_reg_raw(s, PECXISNC);
    uint16_t value = pmb_reg_raw(s, EOPIC) & ~XIC_IR;

    if (((internal & 0x5555) & ((internal >> 1) & 0x5555)) ||
        ((external & 0x5555) & ((external >> 1) & 0x5555))) {
        value |= XIC_IR;
    }
    pmb_reg_store(s, EOPIC, value);
}

static uint32_t pmb_bus_read(PMB7850State *s, uint32_t addr, unsigned size);
static void pmb_bus_write(PMB7850State *s, uint32_t addr, uint32_t value,
                          unsigned size);

static void pmb_pec_transfer(PMB7850State *s, int ch, int source)
{
    uint16_t pecc = pmb_reg_raw(s, pecc_addr[ch]);
    uint16_t seg = pmb_reg_raw(s, pecsn_addr[ch]);
    uint16_t src_off = lduw_le_p(s->dpram_data +
                                 srcp_addr[ch] - PMB_DPRAM_BASE);
    uint16_t dst_off = lduw_le_p(s->dpram_data +
                                 dstp_addr[ch] - PMB_DPRAM_BASE);
    uint32_t src = ((seg & 0xff) << 16) | src_off;
    uint32_t dst = ((seg >> 8) << 16) | dst_off;
    unsigned size = (pecc & PECC_BWT) ? 1 : 2;
    bool is_long = pecc & PECC_PT;
    uint16_t counter_addr = ch == 0 ? 0xfef0 : ch == 2 ? 0xfef2 : 0;
    unsigned before = is_long ? pmb_reg_raw(s, counter_addr) : pecc & 0xff;
    unsigned after = before == 0xff && !is_long ? 0xff : before - 1;
    uint32_t value;
    unsigned inc = (pecc & PECC_INC_MASK) >> 9;

    /*
     * A PEC destination may itself assert the serviced source.  Consume the
     * old request before performing the transfer so such a new edge remains
     * pending (notably SSC0TB raising SSC0TIC for the next queued byte).
     */
    if (before > 1 || (before == 1 && (pecc & PECC_EOPINT))) {
        pmb_reg_store(s, s->irq_sources[source].addr,
                      pmb_reg_raw(s, s->irq_sources[source].addr) & ~XIC_IR);
    }
    value = pmb_bus_read(s, src, size);
    pmb_bus_write(s, dst, value, size);
    if (inc == 1) {
        stw_le_p(s->dpram_data + dstp_addr[ch] - PMB_DPRAM_BASE,
                 dst_off + size);
    } else if (inc == 2 || inc == 3) {
        stw_le_p(s->dpram_data + srcp_addr[ch] - PMB_DPRAM_BASE,
                 src_off + size);
    }
    if (is_long) {
        pmb_reg_store(s, counter_addr, after);
    } else {
        pmb_reg_store(s, pecc_addr[ch], (pecc & 0xff00) | after);
    }
    if (before == 1 && (pecc & PECC_EOPINT)) {
        hwaddr sub = ch < 8 ? PECISNC : PECXISNC;
        unsigned local = ch & 7;
        uint16_t old = pmb_reg_raw(s, sub);

        pmb_reg_store(s, sub, old | (1u << (2 * local + 1)));
        if (old & (1u << (2 * local))) {
            pmb_set_ir(s, EOPIC);
        }
    }
    trace_pmb7850_pec(s->cpu.env.guest_icount,
                      c166_env_get_pc(&s->cpu.env), ch, src, dst, size,
                      value, before, after);
}

static void pmb_irq_ack(void *opaque, int n, int level)
{
    PMB7850State *s = opaque;
    int source = s->selected_source;

    if (!(level & C166_IRQ_ASSERT) || source < 0) {
        return;
    }
    trace_pmb7850_irq(s->cpu.env.guest_icount,
                      c166_env_get_pc(&s->cpu.env), "ack", source,
                      s->irq_sources[source].vector, (level >> 8) & 0xf,
                      !!(level & C166_IRQ_PEC));
    if (level & C166_IRQ_PEC) {
        int ch = pmb_pec_channel(s,
                                 pmb_reg_raw(s, s->irq_sources[source].addr),
                                 (level >> 8) & 0xf);
        if (ch >= 0) {
            pmb_pec_transfer(s, ch, source);
        }
    } else {
        pmb_reg_store(s, s->irq_sources[source].addr,
                      pmb_reg_raw(s, s->irq_sources[source].addr) & ~XIC_IR);
        if (s->irq_sources[source].addr == EOPIC) {
            pmb_refresh_eopic(s);
        }
    }
    pmb_update_irq(s);
    pmb_arm_peripheral_timer(s);
}

static void pmb_lifecycle(void *opaque, int n, int level)
{
    PMB7850State *s = opaque;

    if (!level) {
        return;
    }
    if (n == 0) {
        qemu_set_irq(s->rstout, 1);
        trace_pmb7850_lifecycle(s->cpu.env.guest_icount,
                                c166_env_get_pc(&s->cpu.env), "einit");
    } else {
        device_cold_reset(DEVICE(s));
        trace_pmb7850_lifecycle(s->cpu.env.guest_icount,
                                c166_env_get_pc(&s->cpu.env), "srst");
    }
}

static uint16_t pmb_capcom_cc_addr(unsigned channel)
{
    return channel < 16 ? 0xfe80 + 2 * channel : 0xfe60 + 2 * (channel - 16);
}

static uint16_t pmb_capcom_ic_addr(unsigned channel)
{
    static const uint16_t capcom2_ic[] = {
        0xf160, 0xf162, 0xf164, 0xf166, 0xf168, 0xf16a, 0xf16c, 0xf16e,
        0xf170, 0xf172, 0xf174, 0xf176, 0xf178, 0xf184, 0xf18c, 0xf194,
    };

    return channel < 16 ? 0xff78 + 2 * channel : capcom2_ic[channel - 16];
}

static uint16_t pmb_capcom_ccm_addr(unsigned channel)
{
    return channel < 16 ? 0xff52 + 2 * (channel / 4) :
                          0xff22 + 2 * ((channel - 16) / 4);
}

static unsigned pmb_capcom_mode(PMB7850State *s, unsigned channel,
                                unsigned *timer)
{
    unsigned local = channel & 15;
    uint16_t ccm = pmb_reg_raw(s, pmb_capcom_ccm_addr(channel));
    unsigned field = (ccm >> (4 * (local & 3))) & 0xf;

    *timer = (channel < 16 ? 0 : 2) + !!(field & 8);
    return field & 7;
}

static void pmb_update_capcom_driven(PMB7850State *s, hwaddr addr)
{
    for (unsigned channel = 0; channel < 32; channel++) {
        unsigned timer;
        unsigned mode;

        if (pmb_capcom_ccm_addr(channel) != addr) {
            continue;
        }
        mode = pmb_capcom_mode(s, channel, &timer);
        qemu_set_irq(s->capcom_driven[channel], mode == 5 || mode == 7);
    }
}

static uint16_t pmb_capcom_timer_addr(unsigned timer)
{
    static const uint16_t addresses[] = { 0xfe50, 0xfe52, 0xf050, 0xf052 };

    return addresses[timer];
}

static void pmb_capcom_capture(PMB7850State *s, unsigned channel, bool rising)
{
    unsigned timer;
    unsigned mode = pmb_capcom_mode(s, channel, &timer);

    if (mode == 3 || (mode == 1 && rising) || (mode == 2 && !rising)) {
        uint16_t value;

        /* Capture observes the live counter even when no deadline is armed. */
        pmb_sync_peripherals(s);
        value = pmb_reg_raw(s, pmb_capcom_timer_addr(timer));
        pmb_reg_store(s, pmb_capcom_cc_addr(channel), value);
        trace_pmb7850_capcom_capture(s->cpu.env.guest_icount,
                                    c166_env_get_pc(&s->cpu.env),
                                    s->timing_ticks, channel, value);
        pmb_set_ir(s, pmb_capcom_ic_addr(channel));
    }
}

static void pmb_capcom_input(void *opaque, int n, int level)
{
    PMB7850State *s = opaque;
    bool old = s->capcom_input[n];

    s->capcom_input[n] = !!level;
    if (old != !!level) {
        pmb_capcom_capture(s, n, !!level);
    }
}

static void pmb_capcom_timer_input(void *opaque, int n, int level)
{
    PMB7850State *s = opaque;
    bool old = s->capcom_timer_input[n];
    uint16_t con_addr = n < 2 ? 0xff50 : 0xff20;
    unsigned shift = (n & 1) * 8;
    uint16_t con = pmb_reg_raw(s, con_addr);
    unsigned selection = (con >> shift) & 7;

    s->capcom_timer_input[n] = !!level;
    if (old == !!level || !(con & (1 << (shift + 6))) ||
        !(con & (1 << (shift + 3)))) {
        return;
    }
    if (selection == 3 || (selection == 1 && level) ||
        (selection == 2 && !level)) {
        uint16_t timer_addr = pmb_capcom_timer_addr(n);
        pmb_reg_store(s, timer_addr, pmb_reg_raw(s, timer_addr) + 1);
    }
}

static void pmb_port_input(void *opaque, int n, int level)
{
    PMB7850State *s = opaque;
    unsigned port = n >> 4;
    unsigned bit = n & 15;
    uint16_t mask = 1u << bit;

    s->port_valid[port] |= mask;
    bool old = s->port_input[port] & mask;

    if (level) {
        s->port_input[port] |= mask;
    } else {
        s->port_input[port] &= ~mask;
    }
    trace_pmb7850_port(pmb_port_number(port), bit, !!level, false);
    if (port == 1 && bit < 8 && old != !!level) {
        pmb_capcom_capture(s, bit, !!level);
    }
}

static void pmb_p3_input(void *opaque, int n, int level)
{
    pmb_port_input(opaque, n, level);
}

static void pmb_p6_input(void *opaque, int n, int level)
{
    pmb_port_input(opaque, 16 + n, level);
}

static void pmb_p7_input(void *opaque, int n, int level)
{
    pmb_port_input(opaque, 32 + n, level);
}

static void pmb_p8_input(void *opaque, int n, int level)
{
    pmb_port_input(opaque, 48 + n, level);
}

static void pmb_adc_input(void *opaque, int n, int level)
{
    PMB7850State *s = opaque;

    s->adc_raw[n] = level;
    trace_pmb7850_adc(n, (int16_t)level);
}

static void pmb_external_irq(void *opaque, int n, int level)
{
    PMB7850State *s = opaque;

    if (!level) {
        return;
    }
    for (unsigned i = 0; i < s->num_irq_sources; i++) {
        if (s->irq_sources[i].vector == n) {
            pmb_set_ir(s, s->irq_sources[i].addr);
            return;
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "pmb7850: external IRQ vector 0x%02x is not registered\n",
                  n);
}

static uint64_t pmb_serial_frame_ticks(PMB7850State *s)
{
    uint16_t con = pmb_reg_raw(s, S0CON);
    uint64_t reload = (pmb_reg_raw(s, S0BG) & 0x1fff) + 1;
    unsigned mode = con & 7;
    unsigned bits = mode == 0 ? 8 :
                    (mode == 1 || mode == 3) ? 10 + !!(con & 8) :
                    (mode == 4 || mode == 5 || mode == 7) ?
                    11 + !!(con & 8) : 0;

    if (!bits) {
        return 0;
    }
    if (mode == 0) {
        return (con & (1 << 13) ? 12 : 8) * reload * bits;
    }
    if (!(con & (1 << 11))) {
        return (con & (1 << 13) ? 48 : 32) * reload * bits;
    }
    if (!(pmb_reg_raw(s, S0FDV) & 0x1ff)) {
        return 16 * reload * bits;
    }
    return DIV_ROUND_UP(8192 * reload * bits,
                        pmb_reg_raw(s, S0FDV) & 0x1ff);
}

static uint64_t pmb_ticks_to_ns(PMB7850State *s, uint64_t ticks)
{
    return s->instruction_timing ? icount_to_ns(ticks) :
           clock_ticks_to_ns(s->sysclk, ticks);
}

static void pmb_asc_schedule(PMB7850State *s)
{
    uint64_t ticks;

    if (!s->asc_tx_count || !(pmb_reg_raw(s, S0CON) & (1 << 15))) {
        return;
    }
    ticks = pmb_serial_frame_ticks(s);
    if (ticks) {
        timer_mod_ns(&s->asc_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     pmb_ticks_to_ns(s, ticks));
    }
}

static void pmb_asc_complete(void *opaque)
{
    PMB7850State *s = opaque;
    uint8_t byte = s->asc_tx[0];

    qemu_chr_fe_write_all(&s->serial0, &byte, 1);
    trace_pmb7850_serial(s->cpu.env.guest_icount,
                         c166_env_get_pc(&s->cpu.env), "ASC0", "tx", byte);
    memmove(&s->asc_tx[0], &s->asc_tx[1], --s->asc_tx_count);
    pmb_set_ir(s, S0TIC);
    pmb_asc_schedule(s);
}

static int pmb_asc_can_receive(void *opaque)
{
    PMB7850State *s = opaque;

    return !s->asc_rx_full &&
        (pmb_reg_raw(s, S0CON) & ((1 << 15) | (1 << 4))) ==
        ((1 << 15) | (1 << 4));
}

static void pmb_asc_receive(void *opaque, const uint8_t *buf, int size)
{
    PMB7850State *s = opaque;

    for (int i = 0; i < size; i++) {
        if (s->asc_rx_full) {
            pmb_reg_store(s, S0CON, pmb_reg_raw(s, S0CON) | (1 << 10));
            pmb_set_ir(s, S0EIC);
            break;
        }
        s->asc_rx = buf[i];
        s->asc_rx_full = true;
        pmb_reg_store(s, S0RBUF, buf[i]);
        pmb_set_ir(s, S0RIC);
        trace_pmb7850_serial(s->cpu.env.guest_icount,
                             c166_env_get_pc(&s->cpu.env),
                             "ASC0", "rx", buf[i]);
    }
}

static uint16_t pmb_ssc_mask(unsigned bits)
{
    return bits == 16 ? 0xffff : (1u << bits) - 1;
}

static void pmb_ssc_start(PMB7850State *s, uint16_t tx, bool queued)
{
    uint16_t con = s->ssc_config;
    unsigned bits = (con & SSC_CON_BM_MASK) + 1;
    uint64_t ticks = 2ull * (pmb_reg_raw(s, SSC0BR) + 1) * bits;

    s->ssc_shift_active = true;
    s->ssc_shift_tx = tx & pmb_ssc_mask(bits);
    s->ssc_shift_bits = bits;
    s->ssc_shift_msb_first = !!(con & SSC_CON_HB);
    s->ssc_status |= SSC_CON_BSY;
    pmb_reg_store(s, SSC0CON, s->ssc_config | s->ssc_status);
    qemu_set_irq(s->ssc0_frame_start,
                 bits | (s->ssc_shift_msb_first ? 0x100 : 0));
    pmb_set_ir(s, SSC0TIC);
    trace_pmb7850_ssc(s->cpu.env.guest_icount,
                      c166_env_get_pc(&s->cpu.env), "start",
                      s->ssc_shift_tx, bits, s->ssc_shift_msb_first,
                      pmb_reg_raw(s, SSC0BR) + 1, queued);
    timer_mod_ns(&s->ssc_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 pmb_ticks_to_ns(s, ticks));
}

static void pmb_ssc_abort(PMB7850State *s)
{
    timer_del(&s->ssc_timer);
    s->ssc_shift_active = false;
    s->ssc_tb_full = false;
    s->ssc_status &= ~SSC_CON_BSY;
    pmb_reg_store(s, SSC0CON, s->ssc_config | s->ssc_status);
}

static uint16_t pmb_ssc_receive(PMB7850State *s, uint16_t mask)
{
    uint16_t rx = ssi_transfer(s->ssc0, s->ssc_shift_tx) & mask;
    BusChild *kid;

    if (!s->ssc0_unselected_rx_high) {
        return rx;
    }
    /*
     * SSI's zero for an unselected bus is not an electrical input level.
     * Keep board calibration separate, and never replace a selected reply
     * (including a legitimate zero). All x55 slaves use standard SSI CS.
     */
    QTAILQ_FOREACH(kid, &BUS(s->ssc0)->children, sibling) {
        SSIPeripheral *dev = SSI_PERIPHERAL(kid->child);
        SSICSMode mode = dev->spc->cs_polarity;

        if (mode == SSI_CS_NONE ||
            (mode == SSI_CS_HIGH && dev->cs) ||
            (mode == SSI_CS_LOW && !dev->cs)) {
            return rx;
        }
    }
    return mask;
}

static void pmb_ssc_complete(void *opaque)
{
    PMB7850State *s = opaque;
    uint16_t mask = pmb_ssc_mask(s->ssc_shift_bits);
    uint16_t slave_rx = pmb_ssc_receive(s, mask);
    uint16_t rx = s->ssc_config & SSC_CON_LB ?
                  s->ssc_shift_tx : slave_rx;
    bool queued = s->ssc_tb_full;

    if (s->ssc_rb_full && (s->ssc_config & SSC_CON_RE)) {
        s->ssc_status |= SSC_CON_RE;
        pmb_set_ir(s, SSC0EIC);
    }
    pmb_reg_store(s, SSC0RB, rx);
    s->ssc_rb_full = true;
    s->ssc_shift_active = false;
    if (!queued) {
        s->ssc_status &= ~SSC_CON_BSY;
    }
    pmb_reg_store(s, SSC0CON, s->ssc_config | s->ssc_status);
    pmb_set_ir(s, SSC0RIC);
    trace_pmb7850_ssc(s->cpu.env.guest_icount,
                      c166_env_get_pc(&s->cpu.env), "complete", rx,
                      s->ssc_shift_bits, s->ssc_shift_msb_first,
                      pmb_reg_raw(s, SSC0BR) + 1, queued);
    if (queued) {
        uint16_t next = s->ssc_tb;

        s->ssc_tb_full = false;
        pmb_ssc_start(s, next, true);
    }
}

static void pmb_ssc_write_con(PMB7850State *s, uint16_t value)
{
    uint16_t old = s->ssc_config;

    if (old & SSC_CON_EN) {
        s->ssc_status &= value & SSC_CON_ERR_MASK;
        s->ssc_config = (old & ~(SSC_CON_EN | SSC_CON_MS)) |
                        (value & (SSC_CON_EN | SSC_CON_MS));

        if (s->ssc_shift_active) {
            s->ssc_status |= SSC_CON_BSY;
        }
        if (!(value & SSC_CON_EN)) {
            pmb_ssc_abort(s);
        }
    } else {
        s->ssc_config = value & ~SSC_CON_BSY;
        s->ssc_status = 0;
    }
    pmb_reg_store(s, SSC0CON, s->ssc_config | s->ssc_status);
}

static void pmb_reg_write_word(PMB7850State *s, hwaddr addr, uint16_t value)
{
    uint16_t old = pmb_reg_raw(s, addr);
    int port = pmb_port_index(addr);

    if (addr == SSC0CON) {
        pmb_ssc_write_con(s, value);
    } else {
        pmb_reg_store(s, addr, value);
    }
    if (port >= 0 || addr == DP3 || addr == DP6 ||
        addr == DP7 || addr == DP8) {
        unsigned p = port >= 0 ? port :
                     addr == DP3 ? 0 : addr == DP6 ? 1 : addr == DP7 ? 2 : 3;
        uint16_t data = pmb_reg_raw(s, pmb_port_data_addr(p));
        uint16_t dir = pmb_reg_raw(s, pmb_port_dir_addr(p));

        for (unsigned bit = 0; bit < 16; bit++) {
            qemu_set_irq(s->port_out[p][bit], !!(data & (1 << bit)));
            qemu_set_irq(s->port_dir[p][bit], !!(dir & (1 << bit)));
        }
        qemu_set_irq(s->port_state[p], data | ((uint32_t)dir << 16));
    }
    if (addr == DP3) {
        qemu_set_irq(s->keypad_matrix_select, pmb_reg_raw(s, DP3));
    }
    if (addr == 0xff52 || addr == 0xff54 || addr == 0xff56 ||
        addr == 0xff58 || addr == 0xff22 || addr == 0xff24 ||
        addr == 0xff26 || addr == 0xff28) {
        pmb_update_capcom_driven(s, addr);
    }
    if (addr == S0TBUF) {
        if (s->asc_tx_count < ARRAY_SIZE(s->asc_tx)) {
            s->asc_tx[s->asc_tx_count++] = value;
            if (s->asc_tx_count == 1) {
                pmb_asc_schedule(s);
            }
        } else {
            pmb_reg_store(s, S0CON, pmb_reg_raw(s, S0CON) | (1 << 10));
            pmb_set_ir(s, S0EIC);
        }
    } else if (addr == S0CON && !(value & (1 << 15))) {
        timer_del(&s->asc_timer);
    } else if (addr == SSC0TB) {
        uint16_t con = pmb_reg_raw(s, SSC0CON);

        if ((con & (SSC_CON_EN | SSC_CON_MS)) !=
            (SSC_CON_EN | SSC_CON_MS)) {
            /* Slave mode is clocked externally and never starts itself. */
        } else if (!s->ssc_shift_active) {
            pmb_ssc_start(s, value, false);
        } else if (!s->ssc_tb_full) {
            s->ssc_tb = value;
            s->ssc_tb_full = true;
        } else {
            s->ssc_status |= SSC_CON_TE;
            pmb_reg_store(s, SSC0CON, s->ssc_config | s->ssc_status);
            pmb_set_ir(s, SSC0EIC);
        }
    }
    if (addr == PECISNC || addr == PECXISNC) {
        pmb_refresh_eopic(s);
        pmb_update_irq(s);
    }
    if (old != pmb_reg_raw(s, addr) && pmb_source_by_addr(s, addr) >= 0) {
        pmb_update_irq(s);
    }
    if (old != pmb_reg_raw(s, addr) &&
        ((((addr >= ADDRSEL1 && addr <= ADDRSEL1 + 6) ||
           (addr >= BUSCON1 && addr <= BUSCON1 + 6) ||
           (addr >= XADRS1 && addr <= XADRS1 + 10) ||
           (addr >= XBCON1 && addr <= XBCON1 + 10)) && !(addr & 1)) ||
         addr == XPERCON || addr == SYSCON)) {
        pmb_update_fast_cs0(s);
        pmb_update_fast_cs(s);
        if (addr == SYSCON) {
            pmb_update_fast_lm(s);
        }
    }
}

static uint64_t pmb_reg_access_read(PMB7850State *s, hwaddr addr,
                                    unsigned size)
{
    hwaddr word_addr = addr & ~1;
    uint16_t value;

    pmb_sync_peripherals(s);
    value = pmb_reg_read_word(s, word_addr);
    trace_pmb7850_reg(false, addr, size, value);
    return size == 1 ? (value >> ((addr & 1) * 8)) & 0xff : value;
}

static void pmb_reg_access_write(PMB7850State *s, hwaddr addr,
                                 uint64_t value, unsigned size)
{
    hwaddr word_addr = addr & ~1;
    uint16_t merged = value;

    pmb_sync_peripherals(s);
    if (size == 1) {
        unsigned shift = (addr & 1) * 8;
        merged = (pmb_reg_raw(s, word_addr) & ~(0xff << shift)) |
                 ((value & 0xff) << shift);
    }
    trace_pmb7850_reg(true, addr, size, value);
    pmb_reg_write_word(s, word_addr, merged);
    pmb_arm_peripheral_timer(s);
}

static uint64_t pmb_esfr_read(void *opaque, hwaddr offset, unsigned size)
{
    return pmb_reg_access_read(opaque, PMB_ESFR_BASE + offset, size);
}

static void pmb_esfr_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    pmb_reg_access_write(opaque, PMB_ESFR_BASE + offset, value, size);
}

static uint64_t pmb_sfr_read(void *opaque, hwaddr offset, unsigned size)
{
    return pmb_reg_access_read(opaque, PMB_SFR_BASE + offset, size);
}

static void pmb_sfr_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    pmb_reg_access_write(opaque, PMB_SFR_BASE + offset, value, size);
}

static const MemoryRegionOps pmb_esfr_ops = {
    .read = pmb_esfr_read,
    .write = pmb_esfr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
};

static const MemoryRegionOps pmb_sfr_ops = {
    .read = pmb_sfr_read,
    .write = pmb_sfr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
};

enum {
    SIM_PHASE_OFF,
    SIM_PHASE_INITIAL,
    SIM_PHASE_ATR,
    SIM_PHASE_READY,
    SIM_INPUT_IDLE,
    SIM_INPUT_PPS,
    SIM_INPUT_HEADER,
    SIM_INPUT_DATA,
};

static bool pmb_sim_enabled(PMB7850State *s)
{
    return s->sim_attached;
}

static void pmb_sim_raise(PMB7850State *s, hwaddr addr, const char *reason)
{
    bool already_pending = pmb_reg_raw(s, addr) & XIC_IR;

    trace_pmb7850_sim_irq(s->cpu.env.guest_icount,
                          c166_env_get_pc(&s->cpu.env), addr,
                          already_pending, reason);
    pmb_set_ir(s, addr);
}

static void pmb_sim_trace_byte(PMB7850State *s, const char *direction,
                               uint8_t value, const char *path)
{
    trace_pmb7850_sim_byte(s->cpu.env.guest_icount,
                           c166_env_get_pc(&s->cpu.env), direction, value,
                           s->sim.byte_sequence, path);
}

static uint16_t pmb_sim_io_word(PMB7850State *s, hwaddr offset)
{
    return lduw_le_p(s->internal_io_data + offset);
}

static void pmb_sim_put_word(PMB7850State *s, hwaddr offset, uint16_t value)
{
    stw_le_p(s->internal_io_data + offset, value);
}

static unsigned pmb_sim_pec_count(PMB7850State *s)
{
    return pmb_reg_raw(s, pecc_addr[3]) & 0xff;
}

static uint32_t pmb_sim_pec_addr(PMB7850State *s, bool source)
{
    uint16_t segment = pmb_reg_raw(s, pecsn_addr[3]);
    uint16_t offset = lduw_le_p(s->dpram_data +
        (source ? srcp_addr[3] : dstp_addr[3]) - PMB_DPRAM_BASE);

    return ((source ? segment & 0xff : segment >> 8) << 16) | offset;
}

static bool pmb_sim_pec_receives(PMB7850State *s)
{
    return pmb_sim_pec_count(s) &&
           (pmb_sim_pec_addr(s, true) & 0xffff) ==
           PMB_IO_BASE + SIM_RX_OFFSET;
}

static void pmb_sim_queue(PMB7850State *s, uint8_t value, bool requires_rx,
                          bool internal)
{
    PMB7850SIMState *sim = &s->sim;
    unsigned at;

    if (sim->output_len == PMB7850_SIM_QUEUE_SIZE) {
        return;
    }
    at = (sim->output_head + sim->output_len) % PMB7850_SIM_QUEUE_SIZE;
    sim->output[at] = value;
    sim->output_requires_rx[at] = requires_rx;
    sim->output_internal[at] = internal;
    sim->output_len++;
}

static void pmb_sim_queue_bytes(PMB7850State *s, const uint8_t *bytes,
                                size_t count, bool requires_rx)
{
    for (size_t i = 0; i < count; i++) {
        pmb_sim_queue(s, bytes[i], requires_rx, false);
    }
}

static void pmb_sim_pop(PMB7850State *s)
{
    if (s->sim.output_len) {
        s->sim.output_head =
            (s->sim.output_head + 1) % PMB7850_SIM_QUEUE_SIZE;
        s->sim.output_len--;
    }
}

static void pmb_sim_add_file(PMB7850SIMState *sim, uint16_t parent,
                             uint16_t id, int structure, uint8_t *data,
                             uint16_t size, uint8_t record_size)
{
    g_assert(sim->file_count < PMB7850_SIM_FILE_COUNT);
    sim->files[sim->file_count++] = (PMB7850SIMFile) {
        parent, id, structure, data, size, record_size,
    };
}

static void pmb_sim_init_profile(PMB7850SIMState *sim)
{
    static const uint8_t iccid[] = {
        0x98, 0x94, 0x10, 0, 0, 0, 0, 0, 0, 0xf0,
    };
    static const uint8_t imsi[] = {
        0x08, 0x10, 0x10, 0x10, 0x32, 0x54, 0x76, 0x98, 0xf0,
    };

    memcpy(sim->iccid, iccid, sizeof(iccid));
    memcpy(sim->imsi, imsi, sizeof(imsi));
    sim->lp[0] = 0xff;
    sim->phase_ef[0] = 2;
    sim->sst[0] = 3;
    sim->ad[3] = 2;
    memset(sim->kc, 0xff, 8);
    sim->kc[8] = 7;
    memset(sim->plmn, 0xff, sizeof(sim->plmn));
    memset(sim->loci, 0xff, 10);
    sim->loci[10] = 1;
    memset(sim->bcch, 0xff, sizeof(sim->bcch));
    sim->acc[1] = 1;
    memset(sim->fplmn, 0xff, sizeof(sim->fplmn));
    sim->cphs_info[0] = 1;
    sim->cphs_info[1] = 2;
    memset(sim->operator_name, 0xff, sizeof(sim->operator_name));
    memcpy(sim->operator_name, "UA-KYIVSTAR", 11);

    pmb_sim_add_file(sim, 0, 0x3f00, -1, NULL, 0, 0);
    pmb_sim_add_file(sim, 0x3f00, 0x7f10, -1, NULL, 0, 0);
    pmb_sim_add_file(sim, 0x3f00, 0x7f20, -1, NULL, 0, 0);
    pmb_sim_add_file(sim, 0x3f00, 0x7f21, -1, NULL, 0, 0);
    pmb_sim_add_file(sim, 0x3f00, 0x2fe2, 0, sim->iccid,
                     sizeof(sim->iccid), 0);
    pmb_sim_add_file(sim, 0x7f20, 0x6f07, 0, sim->imsi,
                     sizeof(sim->imsi), 0);
    pmb_sim_add_file(sim, 0x7f20, 0x6f05, 0, sim->lp, sizeof(sim->lp), 0);
    pmb_sim_add_file(sim, 0x7f20, 0x6fae, 0, sim->phase_ef,
                     sizeof(sim->phase_ef), 0);
    pmb_sim_add_file(sim, 0x7f20, 0x6f38, 0, sim->sst, sizeof(sim->sst), 0);
    pmb_sim_add_file(sim, 0x7f20, 0x6fad, 0, sim->ad, sizeof(sim->ad), 0);
    pmb_sim_add_file(sim, 0x7f20, 0x6f20, 0, sim->kc, sizeof(sim->kc), 0);
    pmb_sim_add_file(sim, 0x7f20, 0x6f31, 0, sim->hpplmn,
                     sizeof(sim->hpplmn), 0);
    pmb_sim_add_file(sim, 0x7f20, 0x6f30, 0, sim->plmn,
                     sizeof(sim->plmn), 0);
    pmb_sim_add_file(sim, 0x7f20, 0x6f7e, 0, sim->loci,
                     sizeof(sim->loci), 0);
    pmb_sim_add_file(sim, 0x7f20, 0x6f74, 0, sim->bcch,
                     sizeof(sim->bcch), 0);
    pmb_sim_add_file(sim, 0x7f20, 0x6f78, 0, sim->acc, sizeof(sim->acc), 0);
    pmb_sim_add_file(sim, 0x7f20, 0x6f7b, 0, sim->fplmn,
                     sizeof(sim->fplmn), 0);
    pmb_sim_add_file(sim, 0x7f20, 0x6f16, 0, sim->cphs_info,
                     sizeof(sim->cphs_info), 0);
    pmb_sim_add_file(sim, 0x7f20, 0x6f14, 0, sim->operator_name,
                     sizeof(sim->operator_name), 0);
    sim->selected_df = 0x3f00;
}

static PMB7850SIMFile *pmb_sim_find(PMB7850State *s, uint16_t id)
{
    for (unsigned i = 0; i < s->sim.file_count; i++) {
        if (s->sim.files[i].id == id &&
            (s->sim.files[i].structure < 0 ||
             s->sim.files[i].parent == s->sim.selected_df)) {
            return &s->sim.files[i];
        }
    }
    return NULL;
}

static PMB7850SIMFile *pmb_sim_selected(PMB7850State *s)
{
    return pmb_sim_find(s, s->sim.selected_ef ?:
                        s->sim.selected_df);
}

static unsigned pmb_sim_response(PMB7850State *s, uint8_t *out,
                                 unsigned capacity)
{
    PMB7850SIMFile *file = pmb_sim_selected(s);
    unsigned count = MIN(capacity, 15);

    memset(out, 0, count);
    if (!file || count < 15) {
        return count;
    }
    out[2] = file->size >> 8;
    out[3] = file->size;
    out[4] = file->id >> 8;
    out[5] = file->id;
    if (file->structure < 0) {
        out[6] = 2;
        out[12] = 9;
        out[13] = 0x93;
        if (capacity >= 20) {
            out[16] = 2;
            out[18] = 0x83;
            out[19] = 0x8a;
        }
    } else {
        out[6] = 4;
        out[12] = 2;
        out[13] = file->structure;
        out[14] = file->record_size;
    }
    return count;
}

static void pmb_sim_execute(PMB7850State *s)
{
    PMB7850SIMState *sim = &s->sim;
    uint8_t *a = sim->input;
    uint8_t temporary[256];
    uint16_t status = 0x9000;
    unsigned output = 0;
    unsigned input_len = sim->input_len;
    PMB7850SIMFile *file;
    uint16_t df_before = sim->selected_df;
    uint16_t ef_before = sim->selected_ef;

    switch (a[1]) {
    case 0xa4: {
        uint16_t id = (a[5] << 8) | a[6];
        file = pmb_sim_find(s, id);
        sim->response_len = 0;
        if (!file) {
            status = 0x9404;
        } else {
            if (file->structure < 0) {
                sim->selected_df = id;
                sim->selected_ef = 0;
            } else {
                sim->selected_ef = id;
            }
            sim->response_len = pmb_sim_response(
                s, sim->response, sizeof(sim->response));
            status = 0x9f00 | sim->response_len;
        }
        break;
    }
    case 0xc0:
        output = MIN(a[4] ?: 256, sim->response_len);
        memcpy(temporary, sim->response, output);
        break;
    case 0xf2:
        output = a[4] ?: 256;
        memset(temporary, 0, output);
        pmb_sim_response(s, temporary, output);
        break;
    case 0xb0: {
        unsigned offset = ((a[2] & 0x7f) << 8) | a[3];
        unsigned count = a[4] ?: 256;
        file = pmb_sim_selected(s);
        if (!file || !file->data || offset + count > file->size) {
            status = 0x9402;
        } else {
            output = count;
            memcpy(temporary, file->data + offset, output);
        }
        break;
    }
    case 0xd6: {
        unsigned offset = ((a[2] & 0x7f) << 8) | a[3];

        file = pmb_sim_selected(s);
        if (!file || !file->data || offset + a[4] > file->size) {
            status = 0x9402;
        } else {
            memcpy(file->data + offset, a + 5, a[4]);
        }
        break;
    }
    case 0xb2: {
        unsigned count = a[4] ?: 256;

        file = pmb_sim_selected(s);
        if (!file || !file->data || !file->record_size || !a[2] ||
            count > file->record_size || a[2] * file->record_size >
            file->size) {
            status = 0x9402;
        } else {
            output = count;
            memcpy(temporary,
                   file->data + (a[2] - 1) * file->record_size, output);
        }
        break;
    }
    case 0xdc:
        file = pmb_sim_selected(s);
        if (!file || !file->data || !file->record_size || !a[2] ||
            a[4] > file->record_size || a[2] * file->record_size >
            file->size) {
            status = 0x9402;
        } else {
            memcpy(file->data + (a[2] - 1) * file->record_size,
                   a + 5, a[4]);
        }
        break;
    case 0x20:
        break;
    default:
        status = 0x6d00;
        break;
    }
    pmb_sim_queue_bytes(s, temporary, output, true);
    sim->completion_sw = status;
    sim->completion_pending = true;
    sim->input_len = 0;
    sim->input_need = 5;
    sim->input_kind = SIM_INPUT_HEADER;
    sim->apdu_count++;
    uint64_t command = ((uint64_t)a[0] << 32) | ((uint64_t)a[1] << 24) |
                       ((uint64_t)a[2] << 16) | ((uint64_t)a[3] << 8) | a[4];
    uint64_t selection = ((uint64_t)df_before << 48) |
                         ((uint64_t)ef_before << 32) |
                         ((uint64_t)sim->selected_df << 16) |
                         sim->selected_ef;

    if (a[1] == 0xa4 && input_len >= 7) {
        command |= (UINT64_C(1) << 56) |
                   ((uint64_t)((a[5] << 8) | a[6]) << 40);
    }
    trace_pmb7850_sim_apdu(s->cpu.env.guest_icount,
                           c166_env_get_pc(&s->cpu.env), command, selection,
                           status, output);
}

static bool pmb_sim_has_input(uint8_t instruction)
{
    return instruction == 0xa4 || instruction == 0xd6 ||
           instruction == 0xdc || instruction == 0x20;
}

static void pmb_sim_input(PMB7850State *s, uint8_t value)
{
    PMB7850SIMState *sim = &s->sim;

    sim->byte_sequence++;
    pmb_sim_trace_byte(s, "phone_to_card", value, "SIM_TX");
    if (sim->input_kind == SIM_INPUT_IDLE) {
        sim->input_len = 0;
        sim->input_kind = value == 0xff ? SIM_INPUT_PPS : SIM_INPUT_HEADER;
        sim->input_need = sim->input_kind == SIM_INPUT_PPS ? 3 : 5;
    }
    if (sim->input_len == PMB7850_SIM_APDU_SIZE) {
        sim->input_kind = SIM_INPUT_IDLE;
        sim->input_len = 0;
        return;
    }
    sim->input[sim->input_len++] = value;
    if (sim->input_kind == SIM_INPUT_PPS && sim->input_len == 2) {
        uint8_t pps0 = sim->input[1];
        sim->input_need = 3 + !!(pps0 & 0x10) + !!(pps0 & 0x20) +
                          !!(pps0 & 0x40);
    }
    if (sim->input_len < sim->input_need) {
        return;
    }
    if (sim->input_kind == SIM_INPUT_PPS) {
        uint8_t check = 0;
        for (unsigned i = 0; i < sim->input_len; i++) {
            check ^= sim->input[i];
        }
        if (!check) {
            pmb_sim_queue_bytes(s, sim->input, sim->input_len, true);
        }
        sim->input_kind = SIM_INPUT_IDLE;
        sim->input_len = 0;
    } else {
        if (sim->input_kind == SIM_INPUT_HEADER) {
            if (pmb_sim_has_input(sim->input[1]) && sim->input[4]) {
                pmb_sim_queue(s, sim->input[1], true, true);
                sim->input_kind = SIM_INPUT_DATA;
                sim->input_need = 5 + sim->input[4];
                return;
            }
            pmb_sim_queue(s, sim->input[1], true, true);
        }
        pmb_sim_execute(s);
    }
    sim->due_tick = s->timing_ticks + SIM_BYTE_TICKS;
}

static void pmb_sim_activate(PMB7850State *s, uint16_t before)
{
    PMB7850SIMState *sim = &s->sim;

    sim->phase = (pmb_sim_io_word(s, SIM_CTRL_OFFSET) & 1) ?
                 SIM_PHASE_READY : SIM_PHASE_INITIAL;
    sim->due_tick = s->timing_ticks + SIM_BYTE_TICKS;
    sim->output_head = 0;
    sim->output_len = 0;
    sim->input_len = 0;
    sim->input_need = 0;
    sim->input_kind = SIM_INPUT_IDLE;
    sim->completion_pending = false;
    sim->rx_loaded = false;
    trace_pmb7850_sim_control(s->cpu.env.guest_icount,
                              c166_env_get_pc(&s->cpu.env), before,
                              pmb_sim_io_word(s, SIM_CTRL_OFFSET),
                              sim->phase == SIM_PHASE_READY ?
                              "ready" : "activate");
}

static void pmb_sim_io_write(PMB7850State *s, hwaddr offset)
{
    PMB7850SIMState *sim = &s->sim;

    if (!pmb_sim_enabled(s)) {
        return;
    }
    if (offset == SIM_CTRL_OFFSET) {
        uint16_t current = pmb_sim_io_word(s, SIM_CTRL_OFFSET);
        if (!(sim->last_control & SIM_CTRL_ACTIVATE) &&
            (current & SIM_CTRL_ACTIVATE)) {
            pmb_sim_activate(s, sim->last_control);
        } else if ((sim->last_control & SIM_CTRL_ACTIVATE) &&
                   !(current & SIM_CTRL_ACTIVATE)) {
            sim->phase = SIM_PHASE_OFF;
            sim->output_len = 0;
            sim->completion_pending = false;
            sim->rx_loaded = false;
            trace_pmb7850_sim_control(s->cpu.env.guest_icount,
                                      c166_env_get_pc(&s->cpu.env),
                                      sim->last_control, current,
                                      "deactivate");
        }
        sim->last_control = current;
    } else if (offset == SIM_TX_OFFSET && sim->phase == SIM_PHASE_READY) {
        pmb_sim_input(s, s->internal_io_data[SIM_TX_OFFSET]);
    }
}

static void pmb_sim_deliver(PMB7850State *s)
{
    PMB7850SIMState *sim = &s->sim;
    bool requires_rx;

    if (!sim->output_len || sim->rx_loaded) {
        return;
    }
    requires_rx = sim->output_requires_rx[sim->output_head];
    if (requires_rx && !pmb_sim_pec_receives(s)) {
        return;
    }
    s->internal_io_data[SIM_RX_OFFSET] = sim->output[sim->output_head];
    sim->byte_sequence++;
    pmb_sim_trace_byte(s, "card_to_phone",
                       s->internal_io_data[SIM_RX_OFFSET],
                       pmb_sim_pec_receives(s) ? "PEC3_SIM_RX" : "SIM_RX");
    if (pmb_sim_pec_receives(s)) {
        sim->rx_loaded = true;
        pmb_sim_raise(s, SIM_BYTE_IC, "rx_byte");
    } else {
        pmb_sim_pop(s);
        pmb_sim_raise(s, SIM_BYTE_IC, "atr_byte");
        sim->due_tick = s->timing_ticks + SIM_BYTE_TICKS;
    }
}

static bool pmb_sim_can_advance(PMB7850State *s)
{
    PMB7850SIMState *sim = &s->sim;
    bool byte_pending = pmb_reg_raw(s, SIM_BYTE_IC) & XIC_IR;
    bool status_pending = pmb_reg_raw(s, SIM_STATUS_IC) & XIC_IR;

    if (!s->sim_attached || sim->phase == SIM_PHASE_OFF) {
        return false;
    }
    if (sim->phase == SIM_PHASE_INITIAL) {
        return !status_pending;
    }
    if (sim->phase == SIM_PHASE_ATR) {
        return !status_pending && !byte_pending;
    }
    if (sim->output_len && sim->output_internal[sim->output_head] &&
        pmb_sim_pec_count(s) &&
        (((pmb_sim_pec_addr(s, false) & 0xffff) ==
          PMB_IO_BASE + SIM_TX_OFFSET) || pmb_sim_pec_receives(s))) {
        return true;
    }
    if (sim->completion_pending && !sim->output_len && !sim->rx_loaded &&
        (!pmb_sim_pec_count(s) ||
         (sim->completion_sw != 0x9000 &&
          (sim->completion_sw & 0xff00) != 0x9f00)) &&
        !byte_pending && !status_pending) {
        return true;
    }
    if (sim->output_len && !byte_pending && !sim->rx_loaded &&
        (!sim->output_requires_rx[sim->output_head] ||
         pmb_sim_pec_receives(s))) {
        return true;
    }
    return !sim->output_len && pmb_sim_pec_count(s) &&
           (pmb_sim_pec_addr(s, false) & 0xffff) ==
           PMB_IO_BASE + SIM_TX_OFFSET && !byte_pending;
}

static void pmb_sim_tick(PMB7850State *s)
{
    PMB7850SIMState *sim = &s->sim;

    if (s->timing_ticks < sim->due_tick || !pmb_sim_can_advance(s)) {
        return;
    }
    sim->event_count++;
    if (sim->phase == SIM_PHASE_INITIAL) {
        static const uint8_t atr[] = { 0x90, 0x11, 0 };
        if (pmb_reg_raw(s, SIM_STATUS_IC) & XIC_IR) {
            return;
        }
        pmb_sim_put_word(s, SIM_STATUS_OFFSET, 2);
        pmb_sim_put_word(s, SIM_RX_OFFSET, 3);
        pmb_sim_raise(s, SIM_STATUS_IC, "initial_convention");
        pmb_sim_queue_bytes(s, atr, sizeof(atr), false);
        sim->phase = SIM_PHASE_ATR;
        sim->due_tick = s->timing_ticks + SIM_BYTE_TICKS;
        return;
    }
    if (sim->phase == SIM_PHASE_ATR) {
        if (!(pmb_reg_raw(s, SIM_STATUS_IC) & XIC_IR) &&
            !(pmb_reg_raw(s, SIM_BYTE_IC) & XIC_IR)) {
            pmb_sim_deliver(s);
            if (!sim->output_len) {
                sim->phase = SIM_PHASE_READY;
                sim->input_kind = SIM_INPUT_IDLE;
            }
        }
        return;
    }
    if (sim->output_len && sim->output_internal[sim->output_head] &&
        pmb_sim_pec_count(s) &&
        (((pmb_sim_pec_addr(s, false) & 0xffff) ==
          PMB_IO_BASE + SIM_TX_OFFSET) || pmb_sim_pec_receives(s))) {
        sim->byte_sequence++;
        pmb_sim_trace_byte(s, "card_to_phone",
                           sim->output[sim->output_head],
                           "controller_T0_procedure");
        pmb_sim_pop(s);
        sim->due_tick = s->timing_ticks + SIM_BYTE_TICKS;
        return;
    }
    if (sim->completion_pending && !sim->output_len && !sim->rx_loaded &&
        (!pmb_sim_pec_count(s) ||
         (sim->completion_sw != 0x9000 &&
          (sim->completion_sw & 0xff00) != 0x9f00)) &&
        !(pmb_reg_raw(s, SIM_BYTE_IC) & XIC_IR) &&
        !(pmb_reg_raw(s, SIM_STATUS_IC) & XIC_IR)) {
        sim->completion_pending = false;
        pmb_sim_put_word(s, SIM_RESULT_HI_OFFSET, sim->completion_sw >> 8);
        pmb_sim_put_word(s, SIM_RESULT_LO_OFFSET,
                         (uint8_t)sim->completion_sw);
        pmb_sim_put_word(s, SIM_STATUS_OFFSET, 8);
        pmb_sim_raise(s, SIM_STATUS_IC, "t0_complete");
        return;
    }
    if (sim->output_len && !(pmb_reg_raw(s, SIM_BYTE_IC) & XIC_IR)) {
        pmb_sim_deliver(s);
    } else if (!sim->output_len && pmb_sim_pec_count(s) &&
               (pmb_sim_pec_addr(s, false) & 0xffff) ==
               PMB_IO_BASE + SIM_TX_OFFSET &&
               !(pmb_reg_raw(s, SIM_BYTE_IC) & XIC_IR)) {
        pmb_sim_raise(s, SIM_BYTE_IC, "tx_ready");
    }
}

static uint64_t pmb_sim_next(PMB7850State *s)
{
    if (!pmb_sim_enabled(s) || s->sim.phase == SIM_PHASE_OFF) {
        return UINT64_MAX;
    }
    if (s->sim.due_tick > s->timing_ticks) {
        return s->sim.due_tick - s->timing_ticks;
    }
    return pmb_sim_can_advance(s) ? 1 : UINT64_MAX;
}

static uint64_t pmb_internal_io_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    PMB7850State *s = opaque;

    pmb_sync_peripherals(s);
    uint64_t value = ldn_le_p(s->internal_io_data + offset, size);

    trace_pmb7850_internal_io(s->timing_ticks, s->cpu.env.guest_icount,
                             c166_env_get_pc(&s->cpu.env), false,
                             PMB_IO_BASE + offset, size, value);
    if (pmb_sim_enabled(s) && offset == SIM_RX_OFFSET && s->sim.rx_loaded) {
        s->sim.rx_loaded = false;
        pmb_sim_pop(s);
        s->sim.due_tick = s->timing_ticks + SIM_BYTE_TICKS;
        pmb_arm_peripheral_timer(s);
    }
    return value;
}

static void pmb_internal_io_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size)
{
    PMB7850State *s = opaque;

    pmb_sync_peripherals(s);
    stn_le_p(s->internal_io_data + offset, size, value);
    trace_pmb7850_internal_io(s->timing_ticks, s->cpu.env.guest_icount,
                             c166_env_get_pc(&s->cpu.env), true,
                             PMB_IO_BASE + offset, size, value);
    if (offset == SIM_CTRL_OFFSET || offset == SIM_TX_OFFSET) {
        pmb_sim_io_write(s, offset);
    }
    if (offset == ADC_COMMAND_OFFSET) {
        uint16_t command = lduw_le_p(s->internal_io_data + ADC_COMMAND_OFFSET);

        if (command & 0x0800) {
            unsigned channel = command & 0xff;
            uint16_t raw = channel < PMB7850_ADC_INPUTS ?
                           s->adc_raw[channel] : 0;
            uint16_t result0 = s->adc_result_word == 0 ? raw : 0;
            uint16_t result1 = s->adc_result_word == 1 ? raw : 0;

            stw_le_p(s->internal_io_data + ADC_RESULT0_OFFSET, result0);
            stw_le_p(s->internal_io_data + ADC_RESULT1_OFFSET, result1);
            trace_pmb7850_adc_conversion(s->cpu.env.guest_icount,
                                         c166_env_get_pc(&s->cpu.env),
                                         command, channel, (int16_t)raw,
                                         s->adc_result_word, result0, result1);
        }
    }
    if (offset == TDMA_RESET_OFFSET &&
        lduw_le_p(s->internal_io_data + TDMA_RESET_OFFSET) == 0) {
        stw_le_p(s->internal_io_data + TDMA_COUNTER_OFFSET, 0);
    }
    pmb_arm_peripheral_timer(s);
}

static uint64_t pmb_stack_trace_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    PMB7850State *s = opaque;
    hwaddr dpram_offset = PMB_STACK_TRACE_BASE - PMB_DPRAM_BASE + offset;

    return ldn_le_p(s->dpram_data + dpram_offset, size);
}

static uint64_t pmb_dpram_low_read(void *opaque, hwaddr offset,
                                   unsigned size)
{
    PMB7850State *s = opaque;

    return ldn_le_p(s->dpram_data + offset, size);
}

static void pmb_dpram_low_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    PMB7850State *s = opaque;

    stn_le_p(s->dpram_data + offset, size, value);
}

static uint64_t pmb_dpram_high_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    PMB7850State *s = opaque;

    return ldn_le_p(s->dpram_data + PMB_DPRAM_HIGH_OFFSET + offset, size);
}

static void pmb_dpram_high_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    PMB7850State *s = opaque;
    hwaddr addr = PMB_DPRAM_BASE + PMB_DPRAM_HIGH_OFFSET + offset;

    stn_le_p(s->dpram_data + PMB_DPRAM_HIGH_OFFSET + offset, size, value);
    if (s->sim_attached && addr <= dstp_addr[3] + 1 &&
        addr + size > srcp_addr[3]) {
        pmb_arm_peripheral_timer(s);
    }
}

static void pmb_stack_trace_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size)
{
    PMB7850State *s = opaque;
    hwaddr addr = PMB_STACK_TRACE_BASE + offset;
    hwaddr dpram_offset = addr - PMB_DPRAM_BASE;

    stn_le_p(s->dpram_data + dpram_offset, size, value);
    trace_pmb7850_stack_write(s->cpu.env.guest_icount,
                              c166_env_get_pc(&s->cpu.env), addr, size,
                              value);
}

static const MemoryRegionOps pmb_internal_io_ops = {
    .read = pmb_internal_io_read,
    .write = pmb_internal_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .impl.unaligned = true,
};

static const MemoryRegionOps pmb_stack_trace_ops = {
    .read = pmb_stack_trace_read,
    .write = pmb_stack_trace_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .impl.unaligned = true,
};

static const MemoryRegionOps pmb_dpram_low_ops = {
    .read = pmb_dpram_low_read,
    .write = pmb_dpram_low_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .impl.unaligned = true,
};

static const MemoryRegionOps pmb_dpram_high_ops = {
    .read = pmb_dpram_high_read,
    .write = pmb_dpram_high_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .impl.unaligned = true,
};

static bool pmb_lm_offset(PMB7850State *s, uint32_t addr, uint32_t *offset)
{
    uint16_t syscon = s->cpu.env.syscon;

    if (!(syscon & SYSCON_ROMEN)) {
        return false;
    }
    if (!(syscon & SYSCON_ROMS1) && addr < 0x8000) {
        *offset = addr;
        return true;
    }
    if (addr >= 0x18000 && addr < 0x50000 && !(syscon & SYSCON_ROMS1)) {
        *offset = addr - 0x10000;
        return true;
    }
    if (addr >= 0x10000 && addr < 0x50000 && (syscon & SYSCON_ROMS1)) {
        *offset = addr - 0x10000;
        return true;
    }
    return false;
}

static bool pmb_window_geometry(uint16_t selector, bool small,
                                uint32_t *start, uint32_t *size)
{
    unsigned rgsz = selector & 0xf;

    if (rgsz >= 12) {
        return false;
    }
    *size = (small ? 0x100 : 0x1000) << rgsz;
    *start = ((selector >> 4) << (small ? 8 : 12)) & ~(*size - 1);
    return true;
}

static bool pmb_decode_window(uint16_t selector, bool small, uint32_t addr,
                              uint32_t *offset)
{
    uint32_t size;
    uint32_t start;

    if (!pmb_window_geometry(selector, small, &start, &size) ||
        addr < start || addr >= start + size) {
        return false;
    }
    *offset = addr - start;
    return true;
}

static bool pmb_window_overlaps(uint16_t selector, bool small,
                                uint32_t first, uint32_t last)
{
    uint32_t size;
    uint32_t start;

    if (!pmb_window_geometry(selector, small, &start, &size)) {
        return false;
    }
    return start < last && start + size > first;
}

static void pmb_update_fast_lm(PMB7850State *s)
{
    uint16_t syscon = s->cpu.env.syscon;
    bool low = (syscon & (SYSCON_ROMEN | SYSCON_ROMS1)) == SYSCON_ROMEN;
    bool full = (syscon & (SYSCON_ROMEN | SYSCON_ROMS1)) ==
                (SYSCON_ROMEN | SYSCON_ROMS1);

    if (low != s->local_memory_low_enabled ||
        full != s->local_memory_full_enabled) {
        BQL_LOCK_GUARD();

        trace_pmb7850_fast_lm(s->cpu.env.guest_icount,
                              c166_env_get_pc(&s->cpu.env), low, full,
                              syscon);
        memory_region_transaction_begin();
        memory_region_set_enabled(&s->local_memory_low, low);
        memory_region_set_enabled(&s->local_memory_high, low);
        memory_region_set_enabled(&s->local_memory_full, full);
        memory_region_transaction_commit();
        s->local_memory_low_enabled = low;
        s->local_memory_full_enabled = full;
    }
}

static void pmb_syscon_changed(void *opaque)
{
    PMB7850State *s = opaque;

    pmb_update_fast_cs0(s);
    pmb_update_fast_cs(s);
    pmb_update_fast_lm(s);
}

static bool pmb_fast_cs_blocked(PMB7850State *s, unsigned cs,
                                uint32_t first, uint32_t last)
{
    static const unsigned priority[] = { 2, 4, 1, 3 };
    uint16_t xpercon = pmb_reg_raw(s, XPERCON);
    unsigned position = ARRAY_SIZE(priority);

    if (trace_event_get_state_backends(TRACE_PMB7850_BUS)) {
        return true;
    }
    for (unsigned i = 0; i < 6; i++) {
        uint16_t sel = pmb_reg_raw(s, XADRS1 + 2 * i);
        uint16_t con = pmb_reg_raw(s, XBCON1 + 2 * i);

        if ((s->cpu.env.syscon & SYSCON_XPEN) && (xpercon & (1 << i)) &&
            (con & BUSACT) &&
            pmb_window_overlaps(sel, i < 4, first, last)) {
            return true;
        }
    }
    for (unsigned p = 0; p < ARRAY_SIZE(priority); p++) {
        if (priority[p] == cs) {
            position = p;
            break;
        }
    }
    for (unsigned p = 0; p < position; p++) {
        unsigned i = priority[p] - 1;
        uint16_t sel = pmb_reg_raw(s, ADDRSEL1 + 2 * i);
        uint16_t con = pmb_reg_raw(s, BUSCON1 + 2 * i);

        if ((con & BUSACT) &&
            pmb_window_overlaps(sel, false, first, last)) {
            return true;
        }
    }
    return false;
}

/*
 * RAM and complete NOR programmable chip-select views can be exposed to TCG.
 * Small physical devices repeat across a larger EBC aperture, so use a
 * bounded set of aliases to retain the router's modulo folding.  Any XBUS or
 * higher-priority ADDRSEL overlap disables the complete lower-priority view.
 */
static void pmb_update_fast_cs(PMB7850State *s)
{
    static const unsigned priority[] = { 2, 4, 1, 3 };

    for (unsigned p = 0; p < ARRAY_SIZE(priority); p++) {
        unsigned cs = priority[p];
        unsigned i = cs - 1;
        MemoryRegion *source = s->cs_memory[cs];
        uint16_t sel = pmb_reg_raw(s, ADDRSEL1 + 2 * i);
        uint16_t con = pmb_reg_raw(s, BUSCON1 + 2 * i);
        uint32_t start = 0;
        uint32_t aperture = 0;
        uint64_t source_size = source ? memory_region_size(source) : 0;
        unsigned count = 0;

        if (s->cs_fast_alias_valid[i] && (con & BUSACT) &&
            pmb_window_geometry(sel, false, &start, &aperture) &&
            source_size &&
            DIV_ROUND_UP(aperture, source_size) <=
            PMB7850_FAST_CS_MIRRORS &&
            !pmb_fast_cs_blocked(s, cs, start, start + aperture)) {
            count = DIV_ROUND_UP(aperture, source_size);
        }
        if (count == s->cs_fast_alias_count[i] &&
            (!count || (start == s->cs_fast_alias_start[i] &&
                        aperture == s->cs_fast_alias_size[i]))) {
            continue;
        }

        BQL_LOCK_GUARD();
        trace_pmb7850_fast_cs(s->cpu.env.guest_icount,
                              c166_env_get_pc(&s->cpu.env), cs, !!count,
                              start, aperture, count, sel, con);
        memory_region_transaction_begin();
        for (unsigned mirror = 0; mirror < PMB7850_FAST_CS_MIRRORS;
             mirror++) {
            MemoryRegion *alias = &s->cs_fast_alias[i][mirror];
            bool enabled = mirror < count;

            memory_region_set_enabled(alias, false);
            if (enabled) {
                uint64_t offset = mirror * source_size;
                uint64_t size = MIN(source_size, aperture - offset);

                memory_region_set_address(alias, start + offset);
                memory_region_set_size(alias, size);
                memory_region_set_enabled(alias, true);
            }
        }
        memory_region_transaction_commit();
        s->cs_fast_alias_count[i] = count;
        s->cs_fast_alias_start[i] = start;
        s->cs_fast_alias_size[i] = aperture;
    }
}

/*
 * The boot view followed by CS0 fallback covers the complete external
 * aperture.  Bounded aliases retain descriptor-provided folding and
 * dual-flash composition.  Any programmable CS/XBUS overlap disables the
 * direct view so the router preserves exact EBC priority and bus tracing.
 */
static void pmb_update_fast_cs0(PMB7850State *s)
{
    uint16_t xpercon;
    bool trace_bus;
    bool changed = false;

    if (!s->cs0_fast_alias_valid) {
        return;
    }
    trace_bus = trace_event_get_state_backends(TRACE_PMB7850_BUS);
    xpercon = pmb_reg_raw(s, XPERCON);
    BQL_LOCK_GUARD();
    memory_region_transaction_begin();
    for (unsigned alias = 0; alias < s->cs0_fast_alias_count; alias++) {
        uint32_t first = alias * (PMB7850_ADDR_SPACE_SIZE /
                                  PMB7850_FAST_CS_MIRRORS);
        uint32_t last = first + (PMB7850_ADDR_SPACE_SIZE /
                                 PMB7850_FAST_CS_MIRRORS);
        bool enabled = !trace_bus;

        for (unsigned i = 0; enabled && i < 4; i++) {
            uint16_t sel = pmb_reg_raw(s, ADDRSEL1 + 2 * i);
            uint16_t con = pmb_reg_raw(s, BUSCON1 + 2 * i);

            if ((con & BUSACT) &&
                pmb_window_overlaps(sel, false, first, last)) {
                enabled = false;
            }
        }
        for (unsigned i = 0; enabled && i < 6; i++) {
            uint16_t sel = pmb_reg_raw(s, XADRS1 + 2 * i);
            uint16_t con = pmb_reg_raw(s, XBCON1 + 2 * i);

            if ((s->cpu.env.syscon & SYSCON_XPEN) &&
                (xpercon & (1 << i)) && (con & BUSACT) &&
                pmb_window_overlaps(sel, i < 4, first, last)) {
                enabled = false;
            }
        }
        if (enabled != s->cs0_fast_alias_enabled[alias]) {
            memory_region_set_enabled(&s->cs0_fast_alias[alias], enabled);
            s->cs0_fast_alias_enabled[alias] = enabled;
            changed = true;
        }
    }
    memory_region_transaction_commit();
    if (changed) {
        trace_pmb7850_fast_cs0(s->cpu.env.guest_icount,
                               c166_env_get_pc(&s->cpu.env),
                               !trace_bus,
                               pmb_reg_raw(s, ADDRSEL1 + 2),
                               pmb_reg_raw(s, BUSCON1 + 2));
    }
}

static bool pmb_address_space_rw(AddressSpace *as, bool valid, bool write,
                                 uint32_t offset, uint8_t *buf, unsigned size)
{
    if (!valid) {
        return false;
    }
    if (write) {
        address_space_write(as, offset, MEMTXATTRS_UNSPECIFIED, buf, size);
    } else {
        address_space_read(as, offset, MEMTXATTRS_UNSPECIFIED, buf, size);
    }
    return true;
}

static uint32_t pmb_fold_offset(MemoryRegion *region, uint32_t offset)
{
    uint64_t size = region ? memory_region_size(region) : 0;

    return size ? offset % size : offset;
}

static AddressSpace *pmb_external_target(PMB7850State *s, uint32_t addr,
                                         uint32_t *offset, const char **kind)
{
    uint32_t off;
    uint16_t xpercon = pmb_reg_raw(s, XPERCON);
    static const unsigned priority[] = { 2, 4, 1, 3 };

    for (unsigned i = 0; i < 6; i++) {
        uint16_t sel = pmb_reg_raw(s, XADRS1 + 2 * i);
        uint16_t con = pmb_reg_raw(s, XBCON1 + 2 * i);

        if ((s->cpu.env.syscon & SYSCON_XPEN) && (xpercon & (1 << i)) &&
            (con & BUSACT) && pmb_decode_window(sel, i < 4, addr, &off)) {
            *kind = "xbus";
            *offset = off;
            return s->xbus_as_valid[i] ? &s->xbus_as[i] : NULL;
        }
    }
    for (unsigned p = 0; p < ARRAY_SIZE(priority); p++) {
        unsigned i = priority[p] - 1;
        uint16_t sel = pmb_reg_raw(s, ADDRSEL1 + 2 * i);
        uint16_t con = pmb_reg_raw(s, BUSCON1 + 2 * i);

        if ((con & BUSACT) && pmb_decode_window(sel, false, addr, &off)) {
            off = pmb_fold_offset(s->cs_memory[i + 1], off);
            *kind = "chip-select";
            *offset = off;
            return s->cs_as_valid[i + 1] ? &s->cs_as[i + 1] : NULL;
        }
    }
    if (s->boot_as_valid && addr < memory_region_size(s->boot_memory)) {
        *kind = "boot";
        *offset = addr;
        return &s->boot_as;
    }
    if ((pmb_reg_raw(s, BUSCON0) & BUSACT) || s->cs_as_valid[0]) {
        off = pmb_fold_offset(s->cs_memory[0], addr);
        *kind = "cs0";
        *offset = off;
        return s->cs_as_valid[0] ? &s->cs_as[0] : NULL;
    }
    *kind = NULL;
    return NULL;
}

static bool pmb_external_rw(PMB7850State *s, bool write, uint32_t addr,
                            uint8_t *buf, unsigned size)
{
    uint32_t offset = 0;
    const char *kind;
    AddressSpace *as = pmb_external_target(s, addr, &offset, &kind);

    if (kind) {
        trace_pmb7850_bus(write, addr, size,
                          write ? ldn_le_p(buf, size) : 0, kind, offset);
    }
    return pmb_address_space_rw(as, as != NULL, write, offset, buf, size);
}

static void pmb_observe_data_read(PMB7850State *s, uint32_t addr,
                                  uint32_t value, unsigned size, uint32_t pc,
                                  const char *source)
{
    X55NORTraceContext context = {
        .tick = pmb7850_peek_timing_ticks(s),
        .icount = s->cpu.env.guest_icount,
        .pc = pc,
    };
    unsigned consumed = 0;

    /* icount runs virtual timers at CPU-loop boundaries, not inside a load. */
    BQL_LOCK_GUARD();
    while (consumed < size) {
        uint32_t guest_addr = addr + consumed;
        MemoryRegionSection section = memory_region_find(s->memory,
                                                         guest_addr,
                                                         size - consumed);
        unsigned count = size - consumed;

        if (section.mr == &s->router) {
            uint32_t offset = 0, lm;
            const char *kind;
            AddressSpace *as = pmb_lm_offset(s, guest_addr, &lm) ? NULL :
                pmb_external_target(s, guest_addr, &offset, &kind);

            memory_region_unref(section.mr);
            section = as ? memory_region_find(as->root, offset, count) :
                           (MemoryRegionSection) { 0 };
        }
        if (section.mr) {
            count = MIN(count, int128_get64(section.size));
            x55_nor_trace_read(section.mr, section.offset_within_region,
                                guest_addr, value >> (8 * consumed), count,
                                context, source);
            memory_region_unref(section.mr);
        }
        consumed += count;
    }
}

static void pmb_cpu_data_read(void *opaque, uint32_t addr, uint32_t value,
                              unsigned size, uint32_t pc)
{
    pmb_observe_data_read(opaque, addr, value, size, pc, "cpu");
}

static uint64_t pmb_router_read(void *opaque, hwaddr offset, unsigned size)
{
    PMB7850State *s = opaque;
    uint8_t buf[8];
    uint32_t lm;

    memset(buf, 0xff, sizeof(buf));
    if (pmb_lm_offset(s, offset, &lm)) {
        memcpy(buf, s->local_memory_data + lm, size);
    } else {
        pmb_external_rw(s, false, offset, buf, size);
    }
    return ldn_le_p(buf, size);
}

static void pmb_router_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    PMB7850State *s = opaque;
    uint8_t buf[8];
    uint32_t lm;

    stn_le_p(buf, size, value);
    if (pmb_lm_offset(s, offset, &lm)) {
        memcpy(s->local_memory_data + lm, buf, size);
    } else {
        pmb_external_rw(s, true, offset, buf, size);
    }
}

static uint32_t pmb_bus_read(PMB7850State *s, uint32_t addr, unsigned size)
{
    uint8_t buf[2];

    address_space_read(CPU(&s->cpu)->as, addr, MEMTXATTRS_UNSPECIFIED,
                       buf, size);
    if (s->observe_flash_reads) {
        pmb_observe_data_read(s, addr, ldn_le_p(buf, size), size,
                              c166_env_get_pc(&s->cpu.env), "pec");
    }
    return ldn_le_p(buf, size);
}

static void pmb_bus_write(PMB7850State *s, uint32_t addr, uint32_t value,
                          unsigned size)
{
    uint8_t buf[2];

    stn_le_p(buf, size, value);
    address_space_write(CPU(&s->cpu)->as, addr, MEMTXATTRS_UNSPECIFIED,
                        buf, size);
}

static const MemoryRegionOps pmb_router_ops = {
    .read = pmb_router_read,
    .write = pmb_router_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .impl.unaligned = true,
};

static void pmb_timer_advance(PMB7850State *s, hwaddr counter,
                              hwaddr con_addr, hwaddr ic_addr,
                              uint64_t cycles, uint64_t *phase,
                              unsigned base, const char *name)
{
    uint16_t con = pmb_reg_raw(s, con_addr);
    uint64_t divisor;
    uint64_t steps;
    uint32_t value;
    bool wrapped;

    if (!(con & (1 << 6)) || ((con >> 3) & 7) != 0) {
        return;
    }
    divisor = (uint64_t)base << (con & 7);
    *phase += cycles;
    steps = *phase / divisor;
    *phase %= divisor;
    if (!steps) {
        return;
    }
    value = pmb_reg_raw(s, counter);
    if (con & (1 << 7)) {
        wrapped = steps > value;
        value -= steps;
    } else {
        wrapped = value + steps > 0xffff;
        value += steps;
    }
    pmb_reg_store(s, counter, value);
    if (wrapped) {
        pmb_set_ir(s, ic_addr);
    }
    trace_pmb7850_timer(s->cpu.env.guest_icount,
                        c166_env_get_pc(&s->cpu.env), name, value, wrapped);
}

static bool pmb_capcom_observed(PMB7850State *s, unsigned channel)
{
    return (s->capcom_observe_mask & (1u << channel)) ||
           trace_event_get_state_backends(TRACE_PMB7850_CAPCOM_OUTPUT) ||
           qemu_irq_is_connected(s->capcom_out[channel]);
}

static bool pmb_request_repeats(PMB7850State *s, uint16_t addr)
{
    return (pmb_reg_raw(s, addr) & (XIC_IR | XIC_IE)) == XIC_IR;
}

static void pmb_capcom_advance(PMB7850State *s, unsigned timer,
                               uint64_t cycles, uint64_t *phase)
{
    static const uint16_t con_addr[] = { 0xff50, 0xff50, 0xff20, 0xff20 };
    static const uint16_t counter_addr[] = { 0xfe50, 0xfe52, 0xf050, 0xf052 };
    static const uint16_t reload_addr[] = { 0xfe54, 0xfe56, 0xf054, 0xf056 };
    static const uint16_t ic_addr[] = { 0xff9c, 0xff9e, 0xf17a, 0xf17c };
    static const char *const names[] = { "T0", "T1", "T7", "T8" };
    unsigned shift = (timer & 1) * 8;
    uint16_t con = pmb_reg_raw(s, con_addr[timer]);
    uint64_t divisor;
    uint64_t steps;
    uint16_t value;
    uint16_t reload;
    bool exact_trace;
    uint64_t edge_tick;

    if (!(con & (1 << (shift + 6))) || (con & (1 << (shift + 3)))) {
        return;
    }
    divisor = 1ull << (((con >> shift) & 7) + 3);
    edge_tick = s->timing_ticks - cycles + divisor - *phase;
    *phase += cycles;
    steps = *phase / divisor;
    *phase %= divisor;
    value = pmb_reg_raw(s, counter_addr[timer]);
    reload = pmb_reg_raw(s, reload_addr[timer]);
    exact_trace = trace_event_get_state_backends(TRACE_PMB7850_TIMER) ||
                  trace_event_get_state_backends(TRACE_PMB7850_TDMA) ||
                  trace_event_get_state_backends(TRACE_PMB7850_SCHEDULE);
    /* A wired waveform consumer needs every transition, even without trace. */
    for (unsigned channel = 0; channel < 29; channel++) {
        unsigned selected_timer;
        unsigned mode;

        if (!pmb_capcom_observed(s, channel)) {
            continue;
        }
        mode = pmb_capcom_mode(s, channel, &selected_timer);
        exact_trace |= selected_timer == timer && (mode == 5 || mode == 7);
    }
    if (!exact_trace && steps) {
        uint64_t first_wrap = 0x10000u - value;
        uint64_t remaining = steps >= first_wrap ? steps - first_wrap : 0;
        uint64_t period = 0x10000u - reload;
        uint64_t wraps = steps >= first_wrap ? 1 + remaining / period : 0;
        uint64_t tail = steps >= first_wrap ? remaining % period : steps;
        uint16_t final = steps >= first_wrap ? reload + tail : value + steps;

        pmb_reg_store(s, counter_addr[timer], final);
        if (wraps) {
            pmb_set_ir(s, ic_addr[timer]);
        }
        for (unsigned channel = timer < 2 ? 0 : 16;
             channel < (timer < 2 ? 16 : 29); channel++) {
            unsigned selected_timer;
            unsigned mode = pmb_capcom_mode(s, channel, &selected_timer);
            uint16_t compare = pmb_reg_raw(s,
                                           pmb_capcom_cc_addr(channel));
            uint64_t matches = 0;

            if (selected_timer != timer || mode < 4) {
                continue;
            }
            matches = pmb_capcom_matches(value, reload, compare, steps);
            if (!matches) {
                continue;
            }
            if (channel != 23 || s->irq55_cc23_connected) {
                pmb_set_ir(s, pmb_capcom_ic_addr(channel));
            }
            if ((mode == 5 || mode == 7) &&
                !(channel >= 24 && channel <= 27) && (matches & 1)) {
                s->capcom_output_level[channel] ^= 1;
                qemu_set_irq(s->capcom_out[channel],
                             s->capcom_output_level[channel]);
            }
        }
        return;
    }
    while (steps--) {
        bool wrapped = value == 0xffff;

        value = wrapped ? reload : value + 1;
        pmb_reg_store(s, counter_addr[timer], value);
        if (wrapped) {
            pmb_set_ir(s, ic_addr[timer]);
        }
        for (unsigned channel = timer < 2 ? 0 : 16;
             channel < (timer < 2 ? 16 : 29); channel++) {
            unsigned selected_timer;
            unsigned mode = pmb_capcom_mode(s, channel, &selected_timer);

            if (selected_timer != timer || mode < 4 ||
                pmb_reg_raw(s, pmb_capcom_cc_addr(channel)) != value) {
                continue;
            }
            if (channel != 23 || s->irq55_cc23_connected) {
                pmb_set_ir(s, pmb_capcom_ic_addr(channel));
            }
            if ((mode == 5 || mode == 7) &&
                !(channel >= 24 && channel <= 27)) {
                s->capcom_output_level[channel] ^= 1;
                s->capcom_edge_tick = edge_tick;
                trace_pmb7850_capcom_output(edge_tick, s->cpu.env.guest_icount,
                                           c166_env_get_pc(&s->cpu.env),
                                           channel,
                                           s->capcom_output_level[channel]);
                qemu_set_irq(s->capcom_out[channel],
                             s->capcom_output_level[channel]);
            }
        }
        trace_pmb7850_timer(s->cpu.env.guest_icount,
                            c166_env_get_pc(&s->cpu.env), names[timer],
                            value, wrapped);
        edge_tick += divisor;
    }
}

static void pmb_tdma_raise_irq(PMB7850State *s, uint16_t ic_addr,
                               uint16_t compare, uint16_t counter,
                               const char *name)
{
    bool pending = pmb_reg_raw(s, ic_addr) & XIC_IR;

    pmb_set_ir(s, ic_addr);
    trace_pmb7850_tdma_irq(s->cpu.env.guest_icount,
                           c166_env_get_pc(&s->cpu.env), name, compare,
                           counter, pending);
}

static void pmb_tdma_advance(PMB7850State *s, uint64_t cycles)
{
    uint16_t control = lduw_le_p(s->internal_io_data +
                                 TDMA_CONTROL_OFFSET);
    uint64_t steps;

    /*
     * CEMU's measured contract clocks TDMA from the free-running SoC tick:
     * the tick is advanced before each instruction and the enable bit only
     * gates counter updates.  Preserve that phase while disabled as well.
     */
    s->tdma_phase += cycles;
    steps = s->tdma_phase / TDMA_DIVISOR;
    s->tdma_phase %= TDMA_DIVISOR;
    if (!(control & TDMA_ENABLE)) {
        return;
    }
    if (steps &&
        !trace_event_get_state_backends(TRACE_PMB7850_TIMER) &&
        !trace_event_get_state_backends(TRACE_PMB7850_TDMA) &&
        !trace_event_get_state_backends(TRACE_PMB7850_SCHEDULE)) {
        uint16_t top = lduw_le_p(s->internal_io_data +
                                 TDMA_FRAME_TOP_OFFSET);
        uint16_t before = lduw_le_p(s->internal_io_data +
                                    TDMA_COUNTER_OFFSET);
        uint16_t irq36 = lduw_le_p(s->internal_io_data +
                                   TDMA_IRQ36_COMPARE_OFFSET);
        uint16_t irq37 = lduw_le_p(s->internal_io_data +
                                   TDMA_IRQ37_COMPARE_OFFSET);
        uint64_t period = (uint64_t)top + 1;
        uint64_t first36 = (irq36 + period - before) % period;
        uint64_t first37 = (irq37 + period - before) % period;
        uint64_t matches36;
        uint64_t matches37;
        uint16_t after;

        first36 = first36 ? first36 : period;
        first37 = first37 ? first37 : period;
        matches36 = irq36 <= top && first36 <= steps ?
                    1 + (steps - first36) / period : 0;
        matches37 = irq37 <= top && first37 <= steps ?
                    1 + (steps - first37) / period : 0;
        after = (before + steps) % period;
        stw_le_p(s->internal_io_data + TDMA_COUNTER_OFFSET, after);
        if (matches37) {
            pmb_tdma_raise_irq(s, TDMA_IRQ37IC, irq37, after, "XP1");
        }
        if (matches36) {
            pmb_tdma_raise_irq(s, TDMA_IRQ36IC, irq36, after, "XP0");
        }
        return;
    }
    while (steps--) {
        uint16_t top = lduw_le_p(s->internal_io_data +
                                 TDMA_FRAME_TOP_OFFSET);
        uint16_t before = lduw_le_p(s->internal_io_data +
                                    TDMA_COUNTER_OFFSET);
        uint16_t after = before >= top ? 0 : before + 1;
        uint16_t irq36 = lduw_le_p(s->internal_io_data +
                                   TDMA_IRQ36_COMPARE_OFFSET);
        uint16_t irq37 = lduw_le_p(s->internal_io_data +
                                   TDMA_IRQ37_COMPARE_OFFSET);

        stw_le_p(s->internal_io_data + TDMA_COUNTER_OFFSET, after);
        trace_pmb7850_tdma(s->cpu.env.guest_icount,
                           c166_env_get_pc(&s->cpu.env), before, after, top);
        if (after == irq37) {
            pmb_tdma_raise_irq(s, TDMA_IRQ37IC, irq37, after, "XP1");
        }
        if (after == irq36) {
            pmb_tdma_raise_irq(s, TDMA_IRQ36IC, irq36, after, "XP0");
        }
    }
}

static bool pmb_raw_step_tracing(void)
{
    return trace_event_get_state_backends(TRACE_PMB7850_TIMER) ||
           trace_event_get_state_backends(TRACE_PMB7850_TDMA) ||
           trace_event_get_state_backends(TRACE_PMB7850_SCHEDULE);
}

static uint64_t pmb_steps_delay(uint64_t divisor, uint64_t phase,
                                uint64_t steps)
{
    g_assert(steps > 0);
    return divisor - phase + (steps - 1) * divisor;
}

/*
 * Source bits: GPT 0..4, CAPCOM wraps 5..8, CC 9..37,
 * TDMA 38..40, SIM 41, idle wake 42, periodic clock 43.
 */
static void pmb_deadline_candidate(uint64_t *sources, uint64_t *next,
                                   uint64_t delay, unsigned source)
{
    if (delay == UINT64_MAX) {
        return;
    }
    if (delay < *next) {
        *next = delay;
        *sources = 0;
    }
    if (delay == *next) {
        *sources |= 1ull << source;
    }
}

static uint64_t pmb_next_event_delay(PMB7850State *s, uint64_t *sources)
{
    static const struct {
        uint16_t con;
        unsigned base;
    } timers[] = {
        { 0xff40, 8 }, { 0xff42, 8 }, { 0xff44, 8 },
        { 0xff46, 4 }, { 0xff48, 4 },
    };
    static const uint16_t capcom_con[] = {
        0xff50, 0xff50, 0xff20, 0xff20,
    };
    static const uint16_t capcom_ic[] = {
        0xff9c, 0xff9e, 0xf17a, 0xf17c,
    };
    uint64_t next = UINT64_MAX;

    *sources = 0;

    for (unsigned i = 0; i < ARRAY_SIZE(timers); i++) {
        uint16_t con = pmb_reg_raw(s, timers[i].con);
        uint64_t divisor;

        if (!(con & (1 << 6)) || ((con >> 3) & 7) != 0) {
            continue;
        }
        divisor = (uint64_t)timers[i].base << (con & 7);
        if (pmb_raw_step_tracing()) {
            pmb_deadline_candidate(sources, &next,
                                   divisor - s->timer_phase[i], i);
        } else {
            uint16_t value = pmb_reg_raw(s, 0xfe40 + 2 * i);
            uint64_t steps = con & (1 << 7) ?
                             (uint64_t)value + 1 : 0x10000u - value;

            pmb_deadline_candidate(sources, &next, pmb_steps_delay(divisor,
                                             s->timer_phase[i], steps), i);
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(capcom_con); i++) {
        unsigned shift = (i & 1) * 8;
        uint16_t con = pmb_reg_raw(s, capcom_con[i]);
        uint64_t divisor;

        if (!(con & (1 << (shift + 6))) ||
            (con & (1 << (shift + 3)))) {
            continue;
        }
        divisor = 1ull << (((con >> shift) & 7) + 3);
        if (pmb_raw_step_tracing()) {
            pmb_deadline_candidate(sources, &next,
                                  divisor - s->timer_phase[5 + i], 5 + i);
        } else {
            uint16_t value = pmb_reg_raw(s, 0xfe50 + 2 * (i & 1));
            uint16_t reload = pmb_reg_raw(s, 0xfe54 + 2 * (i & 1));
            uint64_t wrap_steps = 0x10000u - value;
            unsigned first = i < 2 ? 0 : 16;
            unsigned limit = i < 2 ? 16 : 29;

            if (i >= 2) {
                value = pmb_reg_raw(s, 0xf050 + 2 * (i & 1));
                reload = pmb_reg_raw(s, 0xf054 + 2 * (i & 1));
                wrap_steps = 0x10000u - value;
            }
            trace_pmb7850_deadline_capcom(i, value, reload,
                pmb_reg_raw(s, capcom_ic[i]));
            /* A latched disabled request has no further observable effect. */
            if (!pmb_request_repeats(s, capcom_ic[i])) {
                pmb_deadline_candidate(sources, &next,
                    pmb_steps_delay(divisor, s->timer_phase[5 + i], wrap_steps),
                    5 + i);
            }
            for (unsigned channel = first; channel < limit; channel++) {
                unsigned selected_timer;
                unsigned mode = pmb_capcom_mode(s, channel,
                                                &selected_timer);
                uint16_t compare;
                uint64_t steps;

                if (selected_timer != i || mode < 4) {
                    continue;
                }
                trace_pmb7850_deadline_compare(channel,
                    pmb_reg_raw(s, pmb_capcom_ic_addr(channel)),
                    pmb_capcom_observed(s, channel));
                if (pmb_request_repeats(s, pmb_capcom_ic_addr(channel)) &&
                    !pmb_capcom_observed(s, channel)) {
                    continue;
                }
                compare = pmb_reg_raw(s, pmb_capcom_cc_addr(channel));
                if (compare > value) {
                    steps = compare - value;
                } else if (compare >= reload) {
                    steps = wrap_steps + compare - reload;
                } else {
                    continue;
                }
                pmb_deadline_candidate(sources, &next, pmb_steps_delay(
                    divisor, s->timer_phase[5 + i], steps), 9 + channel);
            }
        }
    }
    if (lduw_le_p(s->internal_io_data + TDMA_CONTROL_OFFSET) & TDMA_ENABLE) {
        if (pmb_raw_step_tracing()) {
            pmb_deadline_candidate(sources, &next,
                                  TDMA_DIVISOR - s->tdma_phase, 38);
        } else {
            uint16_t top = lduw_le_p(s->internal_io_data +
                                     TDMA_FRAME_TOP_OFFSET);
            uint16_t value = lduw_le_p(s->internal_io_data +
                                       TDMA_COUNTER_OFFSET);
            uint16_t compare[] = {
                0,
                lduw_le_p(s->internal_io_data +
                          TDMA_IRQ36_COMPARE_OFFSET),
                lduw_le_p(s->internal_io_data +
                          TDMA_IRQ37_COMPARE_OFFSET),
            };
            uint64_t period = (uint64_t)top + 1;

            for (unsigned i = 0; i < ARRAY_SIZE(compare); i++) {
                uint64_t steps;

                if (compare[i] > top) {
                    continue;
                }
                steps = (compare[i] + period - value) % period;
                steps = steps ? steps : period;
                pmb_deadline_candidate(sources, &next,
                    pmb_steps_delay(TDMA_DIVISOR, s->tdma_phase, steps),
                    38 + i);
            }
        }
    }
    pmb_deadline_candidate(sources, &next, pmb_sim_next(s), 41);
    return next;
}

static void pmb_advance_peripherals(PMB7850State *s, uint64_t cycles)
{
    static const struct {
        uint16_t counter, con, ic;
        unsigned base;
        const char *name;
    } timers[] = {
        {0xfe40, 0xff40, 0xff60, 8, "T2"},
        {0xfe42, 0xff42, 0xff62, 8, "T3"},
        {0xfe44, 0xff44, 0xff64, 8, "T4"},
        {0xfe46, 0xff46, 0xff66, 4, "T5"},
        {0xfe48, 0xff48, 0xff68, 4, "T6"},
    };

    for (unsigned i = 0; i < ARRAY_SIZE(timers); i++) {
        pmb_timer_advance(s, timers[i].counter, timers[i].con, timers[i].ic,
                          cycles, &s->timer_phase[i], timers[i].base,
                          timers[i].name);
    }
    for (unsigned i = 0; i < 4; i++) {
        pmb_capcom_advance(s, i, cycles, &s->timer_phase[5 + i]);
    }
    pmb_tdma_advance(s, cycles);
    pmb_sim_tick(s);
}

static uint64_t pmb_pending_cycles(PMB7850State *s, int64_t now)
{
    uint64_t retired = s->cpu.env.guest_icount - s->last_guest_icount;
    uint64_t cycles;

    if (s->instruction_timing) {
        cycles = retired;
        if (!cycles && s->cpu.env.idle) {
            uint64_t sources;
            uint64_t next;

            /* Do not consume another deadline before the vCPU can wake. */
            if (CPU(&s->cpu)->interrupt_request & CPU_INTERRUPT_HARD) {
                return 0;
            }
            next = pmb_next_event_delay(s, &sources);

            /*
             * The instruction clock models event ordering, not host latency.
             * Fixed-shift icount sleep may warp past the programmed wakeup.
             * Stop idle progress at the first peripheral event so the CPU
             * can arbitrate before a later compare/wrap changes its inputs.
             * Rebase at now in pmb_sync_peripherals: host oversleep is not
             * deferred guest work to replay after interrupt acceptance.
             */
            cycles = MIN((uint64_t)icount_round(now - s->last_sync_ns), next);
        }
    } else {
        cycles = clock_ns_to_ticks(s->sysclk, now - s->last_sync_ns);
    }
    return cycles;
}

uint64_t pmb7850_peek_timing_ticks(PMB7850State *s)
{
    if (s->instruction_timing && !s->cpu.env.idle) {
        /*
         * Ordinary translated loads cannot read QEMU's virtual clock: their
         * TB may not be at an I/O boundary. The calibrated handset clock is
         * exactly the retired-instruction delta, with no timer dispatch.
         */
        return s->timing_ticks +
               (s->cpu.env.guest_icount - s->last_guest_icount);
    }
    return s->timing_ticks +
           pmb_pending_cycles(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void pmb_sync_peripherals(PMB7850State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t cycles = pmb_pending_cycles(s, now);

    if (s->instruction_timing && s->cpu.env.idle &&
        s->cpu.env.guest_icount == s->last_guest_icount) {
        trace_pmb7850_idle_clock(s->cpu.env.guest_icount,
            c166_env_get_pc(&s->cpu.env), s->timing_ticks, now,
            icount_round(now - s->last_sync_ns), cycles);
    }
    s->last_sync_ns = now;
    s->last_guest_icount = s->cpu.env.guest_icount;
    s->timing_ticks += cycles;
    pmb_advance_peripherals(s, cycles);
}

uint64_t pmb7850_timing_ticks(PMB7850State *s)
{
    pmb_sync_peripherals(s);
    return s->timing_ticks;
}

static void pmb_arm_peripheral_timer(PMB7850State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t previous = timer_expire_time_ns(&s->peripheral_timer);
    int64_t requested;
    bool unchanged;

    if (s->instruction_timing) {
        uint64_t delay = pmb_next_event_delay(s,
                                            &s->peripheral_deadline_sources);

        requested = delay == UINT64_MAX ? -1 : now + icount_to_ns(delay);
        if (delay != UINT64_MAX) {
            trace_pmb7850_schedule(s->cpu.env.guest_icount,
                                  c166_env_get_pc(&s->cpu.env), delay,
                                  s->cpu.env.idle);
        }
    } else {
        requested = now + 1000;
        s->peripheral_deadline_sources = 1ull << 43;
    }
    unchanged = timer_pending(&s->peripheral_timer) && previous == requested;
    trace_pmb7850_deadline(s->cpu.env.guest_icount,
                          c166_env_get_pc(&s->cpu.env), s->timing_ticks,
                          now, s->peripheral_deadline_sources, previous,
                          requested, requested < 0 ? "cancel" :
                          unchanged ? "unchanged" : "arm");
    if (requested < 0) {
        timer_del(&s->peripheral_timer);
    } else if (!unchanged) {
        timer_mod_ns(&s->peripheral_timer, requested);
    }
}

static void pmb_peripheral_tick(void *opaque)
{
    PMB7850State *s = opaque;

    trace_pmb7850_deadline(s->cpu.env.guest_icount,
                          c166_env_get_pc(&s->cpu.env),
                          pmb7850_peek_timing_ticks(s),
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                          s->peripheral_deadline_sources, -1, -1, "fire");
    pmb_sync_peripherals(s);
    pmb_arm_peripheral_timer(s);
}

static void pmb_register_source(PMB7850State *s, uint16_t addr, uint8_t vector)
{
    g_assert(s->num_irq_sources < ARRAY_SIZE(s->irq_sources));
    g_assert(pmb_source_by_addr(s, addr) < 0);
    s->irq_sources[s->num_irq_sources++] =
        (PMB7850InterruptSource) { addr, vector };
}

static void pmb_register_interrupts(PMB7850State *s)
{
    for (unsigned i = 0; i < 16; i++) {
        pmb_register_source(s, 0xff78 + 2 * i, 0x10 + i);
    }
    pmb_register_source(s, 0xff9c, 0x20);
    pmb_register_source(s, 0xff9e, 0x21);
    for (unsigned i = 0; i < 7; i++) {
        pmb_register_source(s, 0xf160 + 2 * i, 0x30 + i);
    }
    pmb_register_source(s, 0xf16e, 0x37);
    for (unsigned i = 0; i < 5; i++) {
        pmb_register_source(s, 0xf170 + 2 * i, 0x38 + i);
    }
    pmb_register_source(s, 0xf17a, 0x3d);
    pmb_register_source(s, 0xf17c, 0x3e);
    pmb_register_source(s, TDMA_IRQ36IC, 0x40);
    pmb_register_source(s, TDMA_IRQ37IC, 0x41);
    pmb_register_source(s, 0xff60, 0x22);
    pmb_register_source(s, 0xff62, 0x23);
    pmb_register_source(s, 0xff64, 0x24);
    pmb_register_source(s, 0xff66, 0x25);
    pmb_register_source(s, 0xff68, 0x26);
    pmb_register_source(s, 0xff9a, 0x29); /* ADEIC */
    pmb_register_source(s, S0TIC, 0x2a);
    pmb_register_source(s, S0RIC, 0x2b);
    pmb_register_source(s, S0EIC, 0x2c);
    pmb_register_source(s, SSC0TIC, 0x2d);
    pmb_register_source(s, SSC0RIC, 0x2e);
    pmb_register_source(s, SSC0EIC, 0x2f);
    pmb_register_source(s, EOPIC, 0x4c);
    pmb_register_source(s, SIM_BYTE_IC, 0x44);
    pmb_register_source(s, SIM_STATUS_IC, 0x45);
    pmb_register_source(s, SIM_EVENT_IC, 0x46);
    pmb_register_source(s, 0xf140, 0x50); /* external XBUS endpoint */
}

static void pmb7850_reset_hold(Object *obj, ResetType type)
{
    PMB7850State *s = PMB7850_SOC(obj);
    char *sim_profile = s->sim.profile;

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->timer_phase, 0, sizeof(s->timer_phase));
    s->timing_ticks = 0;
    s->capcom_edge_tick = 0;
    /* CEMU advances the SoC tick before instruction execution. */
    s->tdma_phase = 1;
    memset(s->capcom_output_level, 0, sizeof(s->capcom_output_level));
    memset(s->internal_io_data, 0, sizeof(s->internal_io_data));
    memset(s->dpram_data, 0, sizeof(s->dpram_data));
    memset(s->local_memory_data, 0, sizeof(s->local_memory_data));
    memset(&s->sim, 0, sizeof(s->sim));
    s->sim.profile = sim_profile;
    pmb_sim_init_profile(&s->sim);
    s->selected_source = -1;
    s->selected_payload = 0;
    s->asc_tx_count = 0;
    s->asc_rx_full = false;
    s->ssc_shift_active = false;
    s->ssc_tb_full = false;
    s->ssc_rb_full = false;
    s->ssc_config = 0;
    s->ssc_status = 0;
    timer_del(&s->asc_timer);
    timer_del(&s->ssc_timer);
    s->last_sync_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->last_guest_icount = s->cpu.env.guest_icount;
    pmb_arm_peripheral_timer(s);
    for (unsigned i = 0; i < s->num_reset_seeds; i++) {
        pmb_reg_store(s, s->reset_seeds[i].addr,
                      s->reset_seeds[i].value);
    }
    pmb_update_fast_cs0(s);
    pmb_update_fast_cs(s);
    pmb_update_fast_lm(s);
    for (unsigned p = 0; p < 4; p++) {
        for (unsigned bit = 0; bit < 16; bit++) {
            qemu_set_irq(s->port_out[p][bit], 0);
            qemu_set_irq(s->port_dir[p][bit], 0);
        }
        qemu_set_irq(s->port_state[p], 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->capcom_out); i++) {
        qemu_set_irq(s->capcom_out[i], 0);
        qemu_set_irq(s->capcom_driven[i], 0);
    }
    qemu_set_irq(s->rstout, 0);
    pmb_update_irq(s);
    trace_pmb7850_lifecycle(s->cpu.env.guest_icount,
                            c166_env_get_pc(&s->cpu.env), "reset");
}

bool pmb7850_add_reset_seed(PMB7850State *s, hwaddr addr, uint16_t value,
                            Error **errp)
{
    if (DEVICE(s)->realized) {
        error_setg(errp, "reset seeds must be added before realization");
        return false;
    }
    if ((addr & 1) || addr < PMB_ESFR_BASE || addr > 0xffff) {
        error_setg(errp, "invalid reset-seed register 0x%06" HWADDR_PRIx,
                   addr);
        return false;
    }
    if (s->num_reset_seeds == ARRAY_SIZE(s->reset_seeds)) {
        error_setg(errp, "too many PMB7850 reset seeds");
        return false;
    }
    s->reset_seeds[s->num_reset_seeds++] = (PMB7850ResetSeed) {
        .addr = addr,
        .value = value,
    };
    return true;
}

bool pmb7850_set_instruction_timing(PMB7850State *s, bool enabled,
                                    Error **errp)
{
    if (DEVICE(s)->realized) {
        error_setg(errp, "instruction timing must be set before realization");
        return false;
    }
    s->instruction_timing = enabled;
    return true;
}

static void pmb7850_realize(DeviceState *dev, Error **errp)
{
    PMB7850State *s = PMB7850_SOC(dev);

    if (s->observe_flash_reads &&
        (!s->instruction_timing || (!icount_enabled() && !qtest_enabled()))) {
        error_setg(errp, "flash data-read observation requires instruction "
                   "timing and -icount");
        return;
    }
    if (s->adc_result_word > 1) {
        error_setg(errp, "ADC result word must be 0 or 1");
        return;
    }
    if (s->sim.profile && strcmp(s->sim.profile, "deterministic")) {
        error_setg(errp, "unsupported PMB7850 SIM profile '%s'",
                   s->sim.profile);
        return;
    }
    s->sim_attached = s->sim.profile != NULL;
    if (!s->memory) {
        error_setg(errp, "memory link must be set");
        return;
    }
    if (!clock_has_source(s->sysclk)) {
        error_setg(errp, "sysclk must be connected");
        return;
    }
    object_property_set_link(OBJECT(&s->cpu), "memory", OBJECT(s->memory),
                             &error_abort);
    c166_cpu_set_dpram(CPU(&s->cpu), s->dpram_data);
    c166_cpu_set_syscon_write_hook(CPU(&s->cpu), pmb_syscon_changed, s);
    if (s->observe_flash_reads) {
        c166_cpu_set_data_read_hook(CPU(&s->cpu), pmb_cpu_data_read, s);
    }
    if (!qdev_realize(DEVICE(&s->cpu), NULL, errp)) {
        return;
    }
    qdev_connect_gpio_out_named(DEVICE(&s->cpu), "irq-ack", 0,
                                qdev_get_gpio_in_named(dev, "cpu-ack", 0));
    qdev_connect_gpio_out_named(DEVICE(&s->cpu), "einit", 0,
                                qdev_get_gpio_in_named(dev, "cpu-lifecycle",
                                                       0));
    qdev_connect_gpio_out_named(DEVICE(&s->cpu), "srst", 0,
                                qdev_get_gpio_in_named(dev, "cpu-lifecycle",
                                                       1));

    if (s->boot_memory) {
        address_space_init(&s->boot_as, s->boot_memory, "pmb7850-boot");
        s->boot_as_valid = true;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->cs_memory); i++) {
        if (s->cs_memory[i]) {
            g_autofree char *name = g_strdup_printf("pmb7850-cs%u", i);
            address_space_init(&s->cs_as[i], s->cs_memory[i], name);
            s->cs_as_valid[i] = true;
        }
    }
    if (s->boot_memory && s->cs_memory[0]) {
        uint64_t boot_size = MIN(memory_region_size(s->boot_memory),
                                 PMB7850_ADDR_SPACE_SIZE);
        uint64_t source_size = memory_region_size(s->cs_memory[0]);
        uint64_t chunk = PMB7850_ADDR_SPACE_SIZE /
                         PMB7850_FAST_CS_MIRRORS;

        for (uint64_t address = 0; source_size &&
             address < PMB7850_ADDR_SPACE_SIZE; address += chunk) {
            unsigned i = s->cs0_fast_alias_count++;
            MemoryRegion *source = address < boot_size ?
                                   s->boot_memory : s->cs_memory[0];
            uint64_t offset = address < boot_size ? address :
                              address % source_size;
            g_autofree char *name = g_strdup_printf(
                "pmb7850.cs0-fast-%u", i);

            memory_region_init_alias(&s->cs0_fast_alias[i], OBJECT(s), name,
                                     source, offset, chunk);
            memory_region_set_enabled(&s->cs0_fast_alias[i], false);
            memory_region_add_subregion_overlap(s->memory, address,
                                                &s->cs0_fast_alias[i], -5);
        }
        s->cs0_fast_alias_valid = s->cs0_fast_alias_count ==
                                  PMB7850_FAST_CS_MIRRORS;
    }
    for (unsigned i = 0; i < 4; i++) {
        MemoryRegion *source = s->cs_memory[i + 1];

        if (!source || (!memory_region_is_ram(source) &&
                        !x55_nor_is_native_view(source))) {
            continue;
        }
        for (unsigned mirror = 0; mirror < PMB7850_FAST_CS_MIRRORS;
             mirror++) {
            g_autofree char *name = g_strdup_printf(
                "pmb7850.cs%u-fast-%u", i + 1, mirror);
            MemoryRegion *alias = &s->cs_fast_alias[i][mirror];

            memory_region_init_alias(alias, OBJECT(s), name, source, 0, 1);
            memory_region_set_enabled(alias, false);
            memory_region_add_subregion_overlap(s->memory, 0, alias, -1);
        }
        s->cs_fast_alias_valid[i] = true;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->xbus_memory); i++) {
        if (s->xbus_memory[i]) {
            g_autofree char *name = g_strdup_printf("pmb7850-xbus%u", i + 1);
            address_space_init(&s->xbus_as[i], s->xbus_memory[i], name);
            s->xbus_as_valid[i] = true;
        }
    }

    memory_region_add_subregion_overlap(s->memory, 0, &s->router, -10);
    memory_region_add_subregion_overlap(s->memory, 0,
                                        &s->local_memory_low, 5);
    memory_region_add_subregion_overlap(s->memory, 0x18000,
                                        &s->local_memory_high, 5);
    memory_region_add_subregion_overlap(s->memory, 0x10000,
                                        &s->local_memory_full, 5);
    memory_region_add_subregion_overlap(s->memory, PMB_IO_BASE,
                                        &s->internal_io, 10);
    memory_region_add_subregion_overlap(s->memory, PMB_ESFR_BASE,
                                        &s->esfr, 20);
    memory_region_add_subregion_overlap(s->memory, PMB_DPRAM_BASE,
                                        &s->dpram, 30);
    memory_region_add_subregion_overlap(s->memory, PMB_SFR_BASE,
                                        &s->sfr, 20);
    pmb_update_fast_cs0(s);
    pmb_update_fast_cs(s);
    pmb_update_fast_lm(s);
    qemu_chr_fe_set_handlers(&s->serial0, pmb_asc_can_receive,
                             pmb_asc_receive, NULL, NULL, s, NULL, true);
    device_cold_reset(dev);
}

static void pmb7850_unrealize(DeviceState *dev)
{
    PMB7850State *s = PMB7850_SOC(dev);

    if (s->boot_as_valid) {
        address_space_destroy(&s->boot_as);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->cs_as); i++) {
        if (s->cs_as_valid[i]) {
            address_space_destroy(&s->cs_as[i]);
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->xbus_as); i++) {
        if (s->xbus_as_valid[i]) {
            address_space_destroy(&s->xbus_as[i]);
        }
    }
}

static bool pmb_get_sim_attached(Object *obj, Error **errp)
{
    return PMB7850_SOC(obj)->sim_attached;
}

static void pmb_get_sim_byte_count(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    uint64_t value = PMB7850_SOC(obj)->sim.byte_sequence;

    visit_type_uint64(v, name, &value, errp);
}

static void pmb_get_sim_apdu_count(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    uint64_t value = PMB7850_SOC(obj)->sim.apdu_count;

    visit_type_uint64(v, name, &value, errp);
}

static void pmb_get_sim_event_count(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    uint64_t value = PMB7850_SOC(obj)->sim.event_count;

    visit_type_uint64(v, name, &value, errp);
}

static void pmb7850_init(Object *obj)
{
    PMB7850State *s = PMB7850_SOC(obj);
    DeviceState *dev = DEVICE(obj);
    static const char * const port_out_names[] = {
        "p3-out", "p6-out", "p7-out", "p8-out",
    };
    static const char * const port_dir_names[] = {
        "p3-dir", "p6-dir", "p7-dir", "p8-dir",
    };
    static const char * const port_state_names[] = {
        "p3-state", "p6-state", "p7-state", "p8-state",
    };

    object_initialize_child(obj, "cpu", &s->cpu, TYPE_C166S_V1_CPU);
    s->sysclk = qdev_init_clock_in(dev, "sysclk", NULL, NULL, 0);
    s->ssc0 = ssi_create_bus(dev, "ssc0");
    object_property_add_bool(obj, "sim-attached", pmb_get_sim_attached,
                             NULL);
    object_property_add(obj, "sim-byte-count", "uint64",
                        pmb_get_sim_byte_count, NULL, NULL, NULL);
    object_property_add(obj, "sim-apdu-count", "uint64",
                        pmb_get_sim_apdu_count, NULL, NULL, NULL);
    object_property_add(obj, "sim-event-count", "uint64",
                        pmb_get_sim_event_count, NULL, NULL, NULL);

    memory_region_init_io(&s->router, obj, &pmb_router_ops, s,
                          "pmb7850-router", PMB7850_ADDR_SPACE_SIZE);
    memory_region_init_ram_ptr(&s->local_memory, obj,
                               "pmb7850.local-memory", PMB_LM_SIZE,
                               s->local_memory_data);
    vmstate_register_ram(&s->local_memory, dev);
    memory_region_init_alias(&s->local_memory_low, obj,
                             "pmb7850.local-memory-low",
                             &s->local_memory, 0, 0x8000);
    memory_region_init_alias(&s->local_memory_high, obj,
                             "pmb7850.local-memory-high",
                             &s->local_memory, 0x8000, 0x38000);
    memory_region_init_alias(&s->local_memory_full, obj,
                             "pmb7850.local-memory-full",
                             &s->local_memory, 0, PMB_LM_SIZE);
    memory_region_set_enabled(&s->local_memory_low, false);
    memory_region_set_enabled(&s->local_memory_high, false);
    memory_region_set_enabled(&s->local_memory_full, false);
    memory_region_init_io(&s->internal_io, obj, &pmb_internal_io_ops, s,
                          "pmb7850.internal-io", 4 * KiB);
    memory_region_init(&s->dpram, obj, "pmb7850.dpram", PMB_DPRAM_SIZE);
    memory_region_init_io(&s->dpram_low, obj, &pmb_dpram_low_ops, s,
                          "pmb7850.dpram-low", PMB_DPRAM_EDGE_SIZE);
    memory_region_init_ram_ptr(&s->dpram_ram, obj, "pmb7850.dpram-ram",
                               PMB_DPRAM_RAM_SIZE,
                               s->dpram_data + PMB_DPRAM_RAM_OFFSET);
    vmstate_register_ram(&s->dpram_ram, dev);
    memory_region_init_io(&s->dpram_high, obj, &pmb_dpram_high_ops, s,
                          "pmb7850.dpram-high", PMB_DPRAM_EDGE_SIZE);
    memory_region_add_subregion(&s->dpram, 0, &s->dpram_low);
    memory_region_add_subregion(&s->dpram, PMB_DPRAM_RAM_OFFSET,
                                &s->dpram_ram);
    memory_region_add_subregion(&s->dpram, PMB_DPRAM_HIGH_OFFSET,
                                &s->dpram_high);
    if (trace_event_get_state_backends(TRACE_PMB7850_STACK_WRITE)) {
        memory_region_init_io(&s->dpram_stack_trace, obj,
                              &pmb_stack_trace_ops, s,
                              "pmb7850.dpram-stack-trace",
                              PMB_STACK_TRACE_SIZE);
        memory_region_add_subregion_overlap(
            &s->dpram_ram,
            PMB_STACK_TRACE_BASE - PMB_DPRAM_BASE - PMB_DPRAM_RAM_OFFSET,
            &s->dpram_stack_trace, 1);
    }
    memory_region_init_io(&s->esfr, obj, &pmb_esfr_ops, s,
                          "pmb7850.esfr", 0x200);
    memory_region_init_io(&s->sfr, obj, &pmb_sfr_ops, s,
                          "pmb7850.sfr", 0x200);

    timer_init_ns(&s->peripheral_timer, QEMU_CLOCK_VIRTUAL,
                  pmb_peripheral_tick, s);
    timer_init_ns(&s->asc_timer, QEMU_CLOCK_VIRTUAL, pmb_asc_complete, s);
    timer_init_ns(&s->ssc_timer, QEMU_CLOCK_VIRTUAL, pmb_ssc_complete, s);

    qdev_init_gpio_in_named(dev, pmb_irq_ack, "cpu-ack", 1);
    qdev_init_gpio_in_named(dev, pmb_lifecycle, "cpu-lifecycle", 2);
    qdev_init_gpio_in_named(dev, pmb_p3_input, "p3-in", 16);
    qdev_init_gpio_in_named(dev, pmb_p6_input, "p6-in", 16);
    qdev_init_gpio_in_named(dev, pmb_p7_input, "p7-in", 16);
    qdev_init_gpio_in_named(dev, pmb_p8_input, "p8-in", 16);
    qdev_init_gpio_in_named(dev, pmb_capcom_input, "capcom-in", 32);
    qdev_init_gpio_in_named(dev, pmb_capcom_timer_input,
                            "capcom-timer-in", 4);
    qdev_init_gpio_in_named(dev, pmb_adc_input, "adc-raw", PMB7850_ADC_INPUTS);
    qdev_init_gpio_in_named(dev, pmb_external_irq, "ext-irq",
                            PMB7850_EXT_IRQS);
    for (unsigned p = 0; p < 4; p++) {
        qdev_init_gpio_out_named(dev, s->port_out[p],
                                 port_out_names[p], 16);
        qdev_init_gpio_out_named(dev, s->port_dir[p],
                                 port_dir_names[p], 16);
        qdev_init_gpio_out_named(dev, &s->port_state[p],
                                 port_state_names[p], 1);
    }
    qdev_init_gpio_out_named(dev, s->capcom_out, "capcom-out", 32);
    qdev_init_gpio_out_named(dev, s->capcom_driven, "capcom-driven", 32);
    qdev_init_gpio_out_named(dev, &s->keypad_matrix_select,
                             "keypad-matrix-select", 1);
    qdev_init_gpio_out_named(dev, &s->ssc0_frame_start,
                             "ssc0-frame-start", 1);
    qdev_init_gpio_out_named(dev, &s->rstout, "rstout", 1);
    pmb_register_interrupts(s);
}

static const Property pmb7850_properties[] = {
    DEFINE_PROP_BOOL("ssc0-unselected-rx-high", PMB7850State,
                     ssc0_unselected_rx_high, false),
    DEFINE_PROP_UINT32("capcom-observe-mask", PMB7850State,
                       capcom_observe_mask, 0),
    DEFINE_PROP_BOOL("observe-flash-reads", PMB7850State,
                     observe_flash_reads, false),
    DEFINE_PROP_BOOL("instruction-timing", PMB7850State,
                     instruction_timing, false),
    DEFINE_PROP_LINK("memory", PMB7850State, memory, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("boot-memory", PMB7850State, boot_memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("cs0-memory", PMB7850State, cs_memory[0],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("cs1-memory", PMB7850State, cs_memory[1],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("cs2-memory", PMB7850State, cs_memory[2],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("cs3-memory", PMB7850State, cs_memory[3],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("cs4-memory", PMB7850State, cs_memory[4],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("xbus1-memory", PMB7850State, xbus_memory[0],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("xbus2-memory", PMB7850State, xbus_memory[1],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("xbus3-memory", PMB7850State, xbus_memory[2],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("xbus4-memory", PMB7850State, xbus_memory[3],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("xbus5-memory", PMB7850State, xbus_memory[4],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("xbus6-memory", PMB7850State, xbus_memory[5],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_CHR("serial0", PMB7850State, serial0),
    DEFINE_PROP_UINT8("adc-result-word", PMB7850State, adc_result_word, 1),
    DEFINE_PROP_BOOL("irq55-cc23-connected", PMB7850State,
                     irq55_cc23_connected, true),
    DEFINE_PROP_STRING("sim-profile", PMB7850State, sim.profile),
};

static const VMStateDescription vmstate_pmb_sim = {
    .name = "pmb7850/sim",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(phase, PMB7850SIMState),
        VMSTATE_UINT8(input_kind, PMB7850SIMState),
        VMSTATE_UINT8(response_len, PMB7850SIMState),
        VMSTATE_UINT64(due_tick, PMB7850SIMState),
        VMSTATE_UINT64(byte_sequence, PMB7850SIMState),
        VMSTATE_UINT64(apdu_count, PMB7850SIMState),
        VMSTATE_UINT64(event_count, PMB7850SIMState),
        VMSTATE_UINT16(output_head, PMB7850SIMState),
        VMSTATE_UINT16(output_len, PMB7850SIMState),
        VMSTATE_UINT16(input_len, PMB7850SIMState),
        VMSTATE_UINT16(input_need, PMB7850SIMState),
        VMSTATE_UINT16(selected_df, PMB7850SIMState),
        VMSTATE_UINT16(selected_ef, PMB7850SIMState),
        VMSTATE_UINT16(completion_sw, PMB7850SIMState),
        VMSTATE_UINT16(last_control, PMB7850SIMState),
        VMSTATE_BOOL(completion_pending, PMB7850SIMState),
        VMSTATE_BOOL(rx_loaded, PMB7850SIMState),
        VMSTATE_BUFFER(output, PMB7850SIMState),
        VMSTATE_BUFFER(output_requires_rx, PMB7850SIMState),
        VMSTATE_BUFFER(output_internal, PMB7850SIMState),
        VMSTATE_BUFFER(input, PMB7850SIMState),
        VMSTATE_BUFFER(response, PMB7850SIMState),
        VMSTATE_BUFFER(iccid, PMB7850SIMState),
        VMSTATE_BUFFER(imsi, PMB7850SIMState),
        VMSTATE_BUFFER(lp, PMB7850SIMState),
        VMSTATE_BUFFER(phase_ef, PMB7850SIMState),
        VMSTATE_BUFFER(sst, PMB7850SIMState),
        VMSTATE_BUFFER(ad, PMB7850SIMState),
        VMSTATE_BUFFER(kc, PMB7850SIMState),
        VMSTATE_BUFFER(hpplmn, PMB7850SIMState),
        VMSTATE_BUFFER(plmn, PMB7850SIMState),
        VMSTATE_BUFFER(loci, PMB7850SIMState),
        VMSTATE_BUFFER(bcch, PMB7850SIMState),
        VMSTATE_BUFFER(acc, PMB7850SIMState),
        VMSTATE_BUFFER(fplmn, PMB7850SIMState),
        VMSTATE_BUFFER(cphs_info, PMB7850SIMState),
        VMSTATE_BUFFER(operator_name, PMB7850SIMState),
        VMSTATE_END_OF_LIST()
    },
};

static int pmb7850_post_load(void *opaque, int version_id)
{
    PMB7850State *s = opaque;

    if (s->asc_tx_count > ARRAY_SIZE(s->asc_tx) ||
        s->ssc_shift_bits > 16 ||
        s->sim.output_head >= PMB7850_SIM_QUEUE_SIZE ||
        s->sim.output_len > PMB7850_SIM_QUEUE_SIZE ||
        s->sim.input_len > PMB7850_SIM_APDU_SIZE ||
        s->sim.input_need > PMB7850_SIM_APDU_SIZE ||
        s->sim.response_len > sizeof(s->sim.response) ||
        s->selected_source < -1 ||
        s->selected_source >= (int)s->num_irq_sources) {
        return -EINVAL;
    }
    /*
     * SIM file pointers still refer to the destination's fixed backing arrays.
     * CPU interrupt inputs and device pin receivers are saved independently:
     * replaying GPIO here would generate false edges and reset transactions.
     */
    pmb_syscon_changed(s);
    return 0;
}

static const VMStateDescription vmstate_pmb7850 = {
    .name = "pmb7850/soc",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pmb7850_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_SUB_ARRAY(dpram_data, PMB7850State, 0,
                                PMB_DPRAM_EDGE_SIZE),
        VMSTATE_UINT8_SUB_ARRAY(dpram_data, PMB7850State,
                                PMB_DPRAM_HIGH_OFFSET, PMB_DPRAM_EDGE_SIZE),
        VMSTATE_STRUCT(sim, PMB7850State, 0, vmstate_pmb_sim, PMB7850SIMState),
        VMSTATE_BUFFER(internal_io_data, PMB7850State),
        VMSTATE_UINT16_ARRAY(regs, PMB7850State, 0x800),
        VMSTATE_UINT16_ARRAY(port_input, PMB7850State, 4),
        VMSTATE_UINT16_ARRAY(port_valid, PMB7850State, 4),
        VMSTATE_UINT16_ARRAY(adc_raw, PMB7850State, PMB7850_ADC_INPUTS),
        VMSTATE_TIMER(peripheral_timer, PMB7850State),
        VMSTATE_TIMER(asc_timer, PMB7850State),
        VMSTATE_TIMER(ssc_timer, PMB7850State),
        VMSTATE_UINT64(peripheral_deadline_sources, PMB7850State),
        VMSTATE_UINT64(last_guest_icount, PMB7850State),
        VMSTATE_UINT64(timing_ticks, PMB7850State),
        VMSTATE_UINT64(tdma_phase, PMB7850State),
        VMSTATE_UINT64(capcom_edge_tick, PMB7850State),
        VMSTATE_INT64(last_sync_ns, PMB7850State),
        VMSTATE_UINT64_ARRAY(timer_phase, PMB7850State, 9),
        VMSTATE_INT32(selected_source, PMB7850State),
        VMSTATE_UINT16(selected_payload, PMB7850State),
        VMSTATE_UINT16(ssc_config, PMB7850State),
        VMSTATE_UINT16(ssc_status, PMB7850State),
        VMSTATE_UINT16(ssc_shift_tx, PMB7850State),
        VMSTATE_UINT16(ssc_tb, PMB7850State),
        VMSTATE_BUFFER(capcom_input, PMB7850State),
        VMSTATE_BUFFER(capcom_output_level, PMB7850State),
        VMSTATE_BUFFER(capcom_timer_input, PMB7850State),
        VMSTATE_BUFFER(asc_tx, PMB7850State),
        VMSTATE_UINT32(asc_tx_count, PMB7850State),
        VMSTATE_UINT8(asc_rx, PMB7850State),
        VMSTATE_UINT8(ssc_shift_bits, PMB7850State),
        VMSTATE_BOOL(asc_rx_full, PMB7850State),
        VMSTATE_BOOL(ssc_shift_msb_first, PMB7850State),
        VMSTATE_BOOL(ssc_shift_active, PMB7850State),
        VMSTATE_BOOL(ssc_tb_full, PMB7850State),
        VMSTATE_BOOL(ssc_rb_full, PMB7850State),
        VMSTATE_END_OF_LIST()
    },
};

static void pmb7850_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Infineon PMB7850 / E-GOLD+ V3 SoC";
    dc->vmsd = &vmstate_pmb7850;
    dc->realize = pmb7850_realize;
    dc->unrealize = pmb7850_unrealize;
    device_class_set_props(dc, pmb7850_properties);
    rc->phases.hold = pmb7850_reset_hold;
}

static const TypeInfo pmb7850_type_info = {
    .name = TYPE_PMB7850_SOC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PMB7850State),
    .instance_align = __alignof__(PMB7850State),
    .instance_init = pmb7850_init,
    .class_init = pmb7850_class_init,
};

static void pmb7850_register_types(void)
{
    type_register_static(&pmb7850_type_info);
}

type_init(pmb7850_register_types)
