/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "eye.h"

int t736_lp4_eye_select(const uint32_t *errors, size_t count,
                        uint32_t required_lanes, uint16_t guard,
                        uint16_t min_width, int require_closed,
                        struct t736_lp4_eye *eye)
{
    size_t cursor = 0;
    size_t best_first = 0, best_width = 0;
    if (!errors || !eye || count == 0u || count > T736_LP4_MAX_TAPS ||
        required_lanes == 0u || min_width == 0u)
        return -1;
    while (cursor < count) {
        size_t first, width;
        if ((errors[cursor] & required_lanes) != 0u) {
            ++cursor;
            continue;
        }
        first = cursor;
        do {
            ++cursor;
        } while (cursor < count && (errors[cursor] & required_lanes) == 0u);
        width = cursor - first;
        if (require_closed && (first == 0u || cursor == count))
            continue;
        if (width <= 2u * (size_t)guard)
            continue;
        width -= 2u * (size_t)guard;
        if (width < min_width || width <= best_width)
            continue;
        best_first = first + guard;
        best_width = width;
    }
    if (best_width == 0u)
        return -2;
    eye->first = (uint16_t)best_first;
    eye->last = (uint16_t)(best_first + best_width - 1u);
    eye->center = (uint16_t)(best_first + (best_width - 1u) / 2u);
    eye->width = (uint16_t)best_width;
    return 0;
}
