/*
 * Siemens x55 GPIO bit-banged TWI register-file responder.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_C166_SIEMENS_TWI_H
#define HW_C166_SIEMENS_TWI_H

#include "hw/core/qdev.h"

#define TYPE_SIEMENS_TWI "siemens-x55-twi"
#define SIEMENS_TWI_MAX_REGISTERS 64

typedef struct SiemensTWIConfig {
    const char *model;
    uint8_t address;
    const uint8_t *registers;
    unsigned register_count;
} SiemensTWIConfig;

bool siemens_twi_configure(DeviceState *dev, const SiemensTWIConfig *config,
                           Error **errp);

#endif
