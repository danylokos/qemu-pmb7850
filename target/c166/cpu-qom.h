/*
 * Infineon C166S QOM declarations.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_C166_CPU_QOM_H
#define QEMU_C166_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_C166_CPU "c166-cpu"
#define C166_CPU_TYPE_SUFFIX "-" TYPE_C166_CPU
#define C166_CPU_TYPE_NAME(model) model C166_CPU_TYPE_SUFFIX
#define TYPE_C166S_V1_CPU C166_CPU_TYPE_NAME("c166s-v1")

OBJECT_DECLARE_CPU_TYPE(C166CPU, C166CPUClass, C166_CPU)

#endif
