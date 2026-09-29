/*
 * Infineon PMB7850 / E-GOLD+ V3 SoC container.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_C166_PMB7850_H
#define HW_C166_PMB7850_H

#include "hw/core/clock.h"
#include "hw/core/sysbus.h"
#include "hw/char/serial.h"
#include "chardev/char-fe.h"
#include "hw/ssi/ssi.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "target/c166/cpu.h"

#define TYPE_PMB7850_SOC "pmb7850-soc"
OBJECT_DECLARE_SIMPLE_TYPE(PMB7850State, PMB7850_SOC)

#define PMB7850_ADDR_SPACE_SIZE 0x01000000u
#define PMB7850_ADC_INPUTS 16
#define PMB7850_EXT_IRQS 128
#define PMB7850_MAX_RESET_SEEDS 16
#define PMB7850_FAST_CS_MIRRORS 16
#define PMB7850_SIM_QUEUE_SIZE 512
#define PMB7850_SIM_APDU_SIZE 260
#define PMB7850_SIM_FILE_COUNT 19

typedef struct PMB7850SIMFile {
    uint16_t parent;
    uint16_t id;
    int structure;
    uint8_t *data;
    uint16_t size;
    uint8_t record_size;
} PMB7850SIMFile;

typedef struct PMB7850SIMState {
    char *profile;
    uint8_t phase;
    uint64_t due_tick;
    uint64_t byte_sequence;
    uint64_t apdu_count;
    uint64_t event_count;
    uint8_t output[PMB7850_SIM_QUEUE_SIZE];
    uint8_t output_requires_rx[PMB7850_SIM_QUEUE_SIZE];
    uint8_t output_internal[PMB7850_SIM_QUEUE_SIZE];
    uint16_t output_head;
    uint16_t output_len;
    uint8_t input[PMB7850_SIM_APDU_SIZE];
    uint16_t input_len;
    uint16_t input_need;
    uint8_t input_kind;
    uint16_t selected_df;
    uint16_t selected_ef;
    uint8_t response[32];
    uint8_t response_len;
    uint16_t completion_sw;
    uint16_t last_control;
    bool completion_pending;
    bool rx_loaded;
    PMB7850SIMFile files[PMB7850_SIM_FILE_COUNT];
    uint8_t file_count;
    uint8_t iccid[10];
    uint8_t imsi[9];
    uint8_t lp[1];
    uint8_t phase_ef[1];
    uint8_t sst[2];
    uint8_t ad[4];
    uint8_t kc[9];
    uint8_t hpplmn[1];
    uint8_t plmn[24];
    uint8_t loci[11];
    uint8_t bcch[16];
    uint8_t acc[2];
    uint8_t fplmn[12];
    uint8_t cphs_info[3];
    uint8_t operator_name[16];
} PMB7850SIMState;

typedef struct PMB7850InterruptSource {
    uint16_t addr;
    uint8_t vector;
} PMB7850InterruptSource;

typedef struct PMB7850ResetSeed {
    uint16_t addr;
    uint16_t value;
} PMB7850ResetSeed;

struct PMB7850State {
    SysBusDevice parent_obj;

    C166CPU cpu;
    MemoryRegion *memory;
    MemoryRegion *boot_memory;
    MemoryRegion *cs_memory[5];
    MemoryRegion *xbus_memory[6];
    AddressSpace boot_as;
    AddressSpace cs_as[5];
    AddressSpace xbus_as[6];
    bool boot_as_valid;
    bool cs_as_valid[5];
    bool xbus_as_valid[6];
    MemoryRegion cs0_fast_alias[PMB7850_FAST_CS_MIRRORS];
    bool cs0_fast_alias_valid;
    bool cs0_fast_alias_enabled[PMB7850_FAST_CS_MIRRORS];
    unsigned cs0_fast_alias_count;
    MemoryRegion cs_fast_alias[4][PMB7850_FAST_CS_MIRRORS];
    bool cs_fast_alias_valid[4];
    unsigned cs_fast_alias_count[4];
    hwaddr cs_fast_alias_start[4];
    uint64_t cs_fast_alias_size[4];

    MemoryRegion router;
    MemoryRegion local_memory;
    MemoryRegion local_memory_low;
    MemoryRegion local_memory_high;
    MemoryRegion local_memory_full;
    bool local_memory_low_enabled;
    bool local_memory_full_enabled;
    MemoryRegion internal_io;
    MemoryRegion dpram;
    MemoryRegion dpram_low;
    MemoryRegion dpram_ram;
    MemoryRegion dpram_high;
    MemoryRegion dpram_stack_trace;
    MemoryRegion esfr;
    MemoryRegion sfr;
    uint8_t internal_io_data[0x1000];
    uint8_t dpram_data[0x0c00];
    uint8_t local_memory_data[0x40000];
    uint16_t regs[0x800];
    PMB7850ResetSeed reset_seeds[PMB7850_MAX_RESET_SEEDS];
    unsigned num_reset_seeds;

    Clock *sysclk;
    QEMUTimer peripheral_timer;
    uint64_t peripheral_deadline_sources;
    int64_t last_sync_ns;
    uint64_t last_guest_icount;
    uint64_t timing_ticks;
    uint64_t timer_phase[9];
    uint64_t tdma_phase;
    bool instruction_timing;
    bool observe_flash_reads;
    bool sim_attached;

    PMB7850InterruptSource irq_sources[96];
    unsigned num_irq_sources;
    int selected_source;
    uint16_t selected_payload;

    uint16_t port_input[4];
    uint16_t port_valid[4];
    qemu_irq port_out[4][16];
    qemu_irq port_dir[4][16];
    qemu_irq port_state[4];
    qemu_irq capcom_out[32];
    qemu_irq capcom_driven[32];
    uint32_t capcom_observe_mask;
    uint64_t capcom_edge_tick;
    bool irq55_cc23_connected;
    qemu_irq keypad_matrix_select;
    uint8_t capcom_input[32];
    uint8_t capcom_output_level[32];
    uint8_t capcom_timer_input[4];
    qemu_irq rstout;

    uint16_t adc_raw[PMB7850_ADC_INPUTS];
    /* Board-selected firmware ABI calibration, not a silicon identification. */
    uint8_t adc_result_word;

    CharFrontend serial0;
    QEMUTimer asc_timer;
    uint8_t asc_tx[2];
    unsigned asc_tx_count;
    uint8_t asc_rx;
    bool asc_rx_full;

    SSIBus *ssc0;
    bool ssc0_unselected_rx_high;
    QEMUTimer ssc_timer;
    qemu_irq ssc0_frame_start;
    uint16_t ssc_config;
    uint16_t ssc_status;
    uint16_t ssc_shift_tx;
    uint16_t ssc_tb;
    uint8_t ssc_shift_bits;
    bool ssc_shift_msb_first;
    bool ssc_shift_active;
    bool ssc_tb_full;
    bool ssc_rb_full;

    PMB7850SIMState sim;
};

bool pmb7850_add_reset_seed(PMB7850State *s, hwaddr addr, uint16_t value,
                            Error **errp);
bool pmb7850_set_instruction_timing(PMB7850State *s, bool enabled,
                                    Error **errp);
uint64_t pmb7850_timing_ticks(PMB7850State *s);
/* Diagnostic clock sample: does not dispatch timers or mutate SoC state. */
uint64_t pmb7850_peek_timing_ticks(PMB7850State *s);

#endif
