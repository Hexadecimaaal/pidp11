#ifndef PIDP_GPIO_V2_H
#define PIDP_GPIO_V2_H

#include <stdint.h>

#define PIDP_GPIO_V2_LED_ROWS 6u
#define PIDP_GPIO_V2_COLS 12u
#define PIDP_GPIO_V2_SWITCH_ROWS 3u
#define PIDP_GPIO_V2_LINES 21u
#define PIDP_GPIO_V2_LED_MASK UINT32_C(0x0fff)

/* offsets are gpiochip line offsets, not header pins or global gpio numbers.
 * request order: led rows [0,6), columns [6,18), switch rows [18,21).
 * all offsets must be distinct and belong to this explicitly named chip.
 */
struct pidp_gpio_v2_mapping {
  const char *chip_path;
  uint32_t offsets[PIDP_GPIO_V2_LINES];
};

/* callbacks follow the corresponding posix syscall return/errno convention.
 * ioctl callbacks return zero on success. context must outlive the backend;
 * the operations themselves are copied. null ops selects all posix syscalls;
 * supplying a partial operations table is rejected before opening the chip.
 */
struct pidp_gpio_v2_ops {
  int (*open)(void *context, const char *path, int flags);
  int (*ioctl)(void *context, int fd, unsigned long request, void *argument);
  int (*close)(void *context, int fd);
};

/* initialize with {0}; do not copy, modify, or concurrently access a live
 * backend. these fields are private to the implementation. no allocation.
 */
struct pidp_gpio_v2 {
  struct pidp_gpio_v2_ops ops;
  void *context;
  int request_fd;
  unsigned int mode;
  uint32_t offsets[PIDP_GPIO_V2_LINES];
};

/* every operation returns zero on success, or -1 with errno on failure.
 * an error on a live backend best-effort blanks, restores idle, and closes
 * the request. this includes invalid arguments and an attempted second open.
 * reopen explicitly after an error. close on a closed backend is successful.
 * the first error is preserved; close is never retried on an ambiguous error.
 * failed ioctls/close cannot guarantee an electrical state or descriptor
 * release. there is no fallback if v2, pull-ups, or reconfiguration fail.
 */
int pidp_gpio_v2_open(struct pidp_gpio_v2 *backend,
    const struct pidp_gpio_v2_mapping *mapping,
    const struct pidp_gpio_v2_ops *ops, void *context);

/* led rows start output-low; columns and switch rows start input-pull-up.
 * display blanks led rows, releases a previously selected switch row before
 * driving columns, configures physical columns to the complement of led_bits,
 * then enables exactly one led row. bit k lights column k; upper bits fail.
 */
int pidp_gpio_v2_display(struct pidp_gpio_v2 *backend,
    unsigned int row, uint32_t led_bits);

/* selection blanks leds, keeps led rows output-low, and configures columns
 * input-pull-up together with one switch row output-low and the other switch
 * rows input-pull-up. transitions from one selected row release the old row
 * through idle before selecting another. read requires a successful
 * selection; bit k is the physical level on column k (not inverted),
 * matching the existing scanner. on failure the caller's output is
 * unchanged. selection remains until idle/display/close.
 */
int pidp_gpio_v2_select_switch(struct pidp_gpio_v2 *backend,
    unsigned int row);
int pidp_gpio_v2_read_switches(struct pidp_gpio_v2 *backend,
    uint32_t *physical_bits);

/* blank only lowers led rows, preserving columns and switch selection.
 * idle first blanks, then restores the initial directions and values.
 */
int pidp_gpio_v2_blank(struct pidp_gpio_v2 *backend);
int pidp_gpio_v2_idle(struct pidp_gpio_v2 *backend);
int pidp_gpio_v2_close(struct pidp_gpio_v2 *backend);

/* no sleeps or timing guarantees: the caller supplies led dwell, optional
 * blanking delay, and switch settling before read. grouped ioctls do not
 * guarantee simultaneous pin changes. board mapping, voltage compatibility,
 * pull-up support, electrical behavior, and achievable timing are unverified.
 */

#endif
