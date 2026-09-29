/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_C166_PMB7850_CAPCOM_H
#define HW_C166_PMB7850_CAPCOM_H

/* Count matches after increments, including the value loaded on overflow. */
static inline uint64_t pmb_capcom_matches(uint16_t value, uint16_t reload,
                                         uint16_t compare, uint64_t steps)
{
    uint64_t first_wrap = 0x10000u - value;
    uint64_t period = 0x10000u - reload;
    uint64_t matches;

    if (steps < first_wrap) {
        return compare > value && compare <= value + steps;
    }
    matches = compare > value;
    steps -= first_wrap;
    if (compare >= reload) {
        matches += 1 + steps / period;
        if (compare > reload + steps % period) {
            matches--;
        }
    }
    return matches;
}

#endif
