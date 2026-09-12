#ifndef PIDP_GPIO_LINUX_H
#define PIDP_GPIO_LINUX_H

#include <stdint.h>

#include "gpio_v2.h"

/* initialize the explicitly configured Linux gpio-v2 chip before creating
 * the mux thread. PIDP_GPIO_CHIP and PIDP_GPIO_OFFSETS are required.
 */
int pidp_gpio_linux_init(void);
int pidp_gpio_linux_shutdown(void);
/* the direct-header demo requires the verified 64-line JH7110 SYS controller. */
int pidp_gpio_linux_demo_init(void);
int pidp_gpio_linux_demo_frame(
    const uint32_t rows[PIDP_GPIO_V2_LED_ROWS]);
void *blink(void *argument);

#endif
