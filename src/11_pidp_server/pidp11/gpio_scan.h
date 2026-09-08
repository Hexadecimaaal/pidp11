#ifndef PIDP_GPIO_SCAN_H
#define PIDP_GPIO_SCAN_H

#include "gpio_v2.h"
#include "gpiopattern.h"

/* delay follows POSIX return/errno conventions; null selects nanosleep with
 * interrupted sleeps resumed. no transport or timing fallback is provided.
 */
typedef int (*pidp_gpio_delay)(void *context, long nanoseconds);

struct pidp_gpio_rotary {
  int last_code[2];
};

/* initialize rotary with {{3, 3}} and knobs as in main.c. one cycle consumes
 * all 31 phases, selecting the current read page at each phase like gpio.c.
 * raw switch bits, including encoder bits, are published without inversion.
 * on any failure the request is best-effort made idle and closed; errno is
 * the first failure. successful cycles leave the request idle, not closed.
 */
int pidp_gpio_scan_cycle(struct pidp_gpio_v2 *backend,
    volatile uint32_t patterns[2][GPIOPATTERN_LED_BRIGHTNESS_PHASES][8],
    const volatile int *read_index, volatile uint32_t switches[3],
    struct pidp_gpio_rotary *rotary, int knobs[2],
    pidp_gpio_delay delay, void *context);

#endif
