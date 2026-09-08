#ifndef PIDP_GPIO_LINUX_H
#define PIDP_GPIO_LINUX_H

/* initialize the explicitly configured Linux gpio-v2 chip before creating
 * the mux thread. PIDP_GPIO_CHIP and PIDP_GPIO_OFFSETS are required.
 */
int pidp_gpio_linux_init(void);
int pidp_gpio_linux_shutdown(void);
void *blink(void *argument);

#endif
