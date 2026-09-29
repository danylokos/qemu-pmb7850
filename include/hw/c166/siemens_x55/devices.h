/*
 * Shared Siemens x55 handset board description.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_C166_SIEMENS_X55_DEVICES_H
#define HW_C166_SIEMENS_X55_DEVICES_H

#include "hw/c166/siemens_x55/battery.h"
#include "hw/c166/siemens_x55/keypad.h"

#define SIEMENS_X55_MAX_FLASH_CHIPS 2
#define SIEMENS_X55_MAX_FLASH_TYPES 3
#define SIEMENS_X55_MAX_RESET_SEEDS 4
#define SIEMENS_X55_MAX_PORT_INPUTS 4

typedef struct SiemensX55FlashChipDescriptor {
    const char *name;
    const char *default_type;
    const char *permitted_types[SIEMENS_X55_MAX_FLASH_TYPES];
    uint32_t size;
    unsigned boot_region;
    unsigned cs_region;
    unsigned cs;
    uint32_t protected_start;
    uint32_t protected_size;
    bool secsi_factory_locked;
} SiemensX55FlashChipDescriptor;

enum {
    SIEMENS_X55_IRQ55_CC23 = 1u << 0,
    SIEMENS_X55_IRQ55_KEYPAD_ACTIVITY = 1u << 1,
};

#define SIEMENS_X55_IRQ55_DEFAULT \
    (SIEMENS_X55_IRQ55_CC23 | SIEMENS_X55_IRQ55_KEYPAD_ACTIVITY)

typedef struct SiemensX55RAMDescriptor {
    const char *name;
    const char *type;
    uint32_t size;
    unsigned cs;
} SiemensX55RAMDescriptor;

typedef struct SiemensX55ResetSeedDescriptor {
    uint16_t address;
    uint16_t value;
} SiemensX55ResetSeedDescriptor;

typedef struct SiemensX55SerialLinkDescriptor {
    bool available;
    unsigned port;
    unsigned bit;
    bool idle_high;
} SiemensX55SerialLinkDescriptor;

typedef struct SiemensX55PortInputDescriptor {
    unsigned port;
    uint16_t mask;
    uint16_t value;
} SiemensX55PortInputDescriptor;

typedef struct SiemensX55DisplayDescriptor {
    const char *type;
    unsigned dc_port;
    unsigned dc_bit;
    int reset_capcom_channel;
    bool reset_idle_high;
    uint16_t panel_width;
    uint16_t panel_height;
    uint16_t panel_origin_x;
    bool panel_mirror_x;
    bool panel_mirror_y;
} SiemensX55DisplayDescriptor;

typedef struct SiemensX55TWIDescriptor {
    bool available;
    unsigned register_count;
} SiemensX55TWIDescriptor;

typedef struct SiemensX55BoardDescriptor {
    const char *name;
    const char *description;
    uint32_t sysclk_hz;
    SiemensX55FlashChipDescriptor flash[SIEMENS_X55_MAX_FLASH_CHIPS];
    unsigned flash_count;
    SiemensX55RAMDescriptor ram;
    SiemensX55ResetSeedDescriptor reset_seeds[SIEMENS_X55_MAX_RESET_SEEDS];
    unsigned reset_seed_count;
    uint16_t xbus_device_id;
    uint8_t xbus_irq;
    uint8_t irq55_sources;
    SiemensX55SerialLinkDescriptor serial;
    bool ssc0_unselected_rx_high;
    SiemensX55PortInputDescriptor port_inputs[SIEMENS_X55_MAX_PORT_INPUTS];
    unsigned port_input_count;
    SiemensX55DisplayDescriptor display;
    const SiemensKeypadConfig *keypad;
    const SiemensBatteryConfig *battery;
    SiemensX55TWIDescriptor twi;
    bool audio_ringer_present;
    bool audio_xbus_v1;
    unsigned audio_ringer_channel;
} SiemensX55BoardDescriptor;

extern const SiemensX55BoardDescriptor siemens_x55_device_a52;
extern const SiemensX55BoardDescriptor siemens_x55_device_a55;
extern const SiemensX55BoardDescriptor siemens_x55_device_a60;
extern const SiemensX55BoardDescriptor siemens_x55_device_a62;
extern const SiemensX55BoardDescriptor siemens_x55_device_a65;
extern const SiemensX55BoardDescriptor siemens_x55_device_c55;
extern const SiemensX55BoardDescriptor siemens_x55_device_c60;
extern const SiemensX55BoardDescriptor siemens_x55_device_cf62;
extern const SiemensX55BoardDescriptor siemens_x55_device_m55;
extern const SiemensX55BoardDescriptor siemens_x55_device_mc60;
extern const SiemensX55BoardDescriptor siemens_x55_device_s55;
extern const SiemensX55BoardDescriptor siemens_x55_device_sl55;

extern const SiemensX55BoardDescriptor *const siemens_x55_devices[];
extern const size_t siemens_x55_device_count;

#endif
