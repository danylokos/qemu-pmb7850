/*
 * Siemens x55 handset machine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_C166_SIEMENS_X55_MACHINE_H
#define HW_C166_SIEMENS_X55_MACHINE_H

#include "hw/core/boards.h"
#include "hw/c166/siemens_x55/devices.h"

#define TYPE_SIEMENS_X55_MACHINE MACHINE_TYPE_NAME("siemens-x55")
OBJECT_DECLARE_TYPE(SiemensX55MachineState, SiemensX55MachineClass,
                    SIEMENS_X55_MACHINE)

struct SiemensX55MachineClass {
    MachineClass parent_class;
    const SiemensX55BoardDescriptor *board;
};

const SiemensX55BoardDescriptor *siemens_x55_board(MachineState *machine);

#endif
