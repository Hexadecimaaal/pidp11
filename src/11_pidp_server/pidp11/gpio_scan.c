#include "gpio_scan.h"

#include <errno.h>
#include <time.h>

static int nanosleep_delay(void *context, long nanoseconds)
{
  struct timespec requested;

  (void)context;
  requested.tv_sec = nanoseconds / 1000000000L;
  requested.tv_nsec = nanoseconds % 1000000000L;
  while (nanosleep(&requested, &requested) < 0) {
    if (errno != EINTR)
      return -1;
  }
  return 0;
}

static int delay_for(void *context, pidp_gpio_delay delay, long nanoseconds)
{
  int result;
  int error;

  result = (delay != NULL ? delay : nanosleep_delay)(context, nanoseconds);
  if (result == 0)
    return 0;
  error = errno != 0 ? errno : EIO;
  errno = error;
  return -1;
}

static int scan_failure(struct pidp_gpio_v2 *backend, int error)
{
  int saved_error = error != 0 ? error : EIO;

  if (backend != NULL)
    (void)pidp_gpio_v2_close(backend);
  errno = saved_error;
  return -1;
}

static void update_rotary(struct pidp_gpio_rotary *rotary, int knobs[2],
    uint32_t *switchscan)
{
  int code[2];
  int i;

  code[0] = (int)((*switchscan & UINT32_C(0x300)) >> 8);
  code[1] = (int)((*switchscan & UINT32_C(0xc00)) >> 10);
  *switchscan &= UINT32_C(0xff);

  for (i = 0; i < 2; ++i) {
    if (code[i] == 1 && rotary->last_code[i] == 3)
      rotary->last_code[i] = code[i];
    else if (code[i] == 2 && rotary->last_code[i] == 3)
      rotary->last_code[i] = code[i];
  }

  for (i = 0; i < 2; ++i) {
    if (code[i] == 3 && rotary->last_code[i] == 1) {
      rotary->last_code[i] = code[i];
      *switchscan += UINT32_C(1) << ((i * 2) + 8);
      ++knobs[i];
    } else if (code[i] == 3 && rotary->last_code[i] == 2) {
      rotary->last_code[i] = code[i];
      *switchscan += UINT32_C(2) << ((i * 2) + 8);
      --knobs[i];
    }
  }

  knobs[0] &= 7;
  knobs[1] &= 3;
}

int pidp_gpio_scan_cycle(struct pidp_gpio_v2 *backend,
    volatile uint32_t patterns[2][GPIOPATTERN_LED_BRIGHTNESS_PHASES][8],
    const volatile int *read_index, volatile uint32_t switches[3],
    struct pidp_gpio_rotary *rotary, int knobs[2],
    pidp_gpio_delay delay, void *context)
{
  unsigned int phase;
  unsigned int row;
  int page;

  if (backend == NULL || patterns == NULL || read_index == NULL
      || switches == NULL || rotary == NULL || knobs == NULL)
    return scan_failure(backend, EINVAL);

  for (phase = 0; phase < GPIOPATTERN_LED_BRIGHTNESS_PHASES; ++phase) {
    const volatile uint32_t *phase_pattern;

    page = *read_index;
    if (page < 0 || page > 1)
      return scan_failure(backend, EINVAL);
    phase_pattern = patterns[page][phase];

    for (row = 0; row < PIDP_GPIO_V2_LED_ROWS; ++row) {
      uint32_t led_bits = phase_pattern[row] & PIDP_GPIO_V2_LED_MASK;

      if (pidp_gpio_v2_display(backend, row, led_bits) < 0)
        return scan_failure(backend, errno);
      if (delay_for(context, delay, 50000L) < 0)
        return scan_failure(backend, errno);
      if (pidp_gpio_v2_blank(backend) < 0)
        return scan_failure(backend, errno);
      if (delay_for(context, delay, 10000L) < 0)
        return scan_failure(backend, errno);
    }

    for (row = 0; row < PIDP_GPIO_V2_SWITCH_ROWS; ++row) {
      uint32_t physical_bits;
      uint32_t published_bits;

      if (pidp_gpio_v2_select_switch(backend, row) < 0)
        return scan_failure(backend, errno);
      if (delay_for(context, delay, 500L) < 0)
        return scan_failure(backend, errno);
      if (pidp_gpio_v2_read_switches(backend, &physical_bits) < 0)
        return scan_failure(backend, errno);

      published_bits = physical_bits & PIDP_GPIO_V2_LED_MASK;
      if (row == 2) {
        uint32_t rotary_bits = published_bits;
        update_rotary(rotary, knobs, &rotary_bits);
      }
      switches[row] = published_bits;
      if (pidp_gpio_v2_idle(backend) < 0)
        return scan_failure(backend, errno);
    }
  }

  return 0;
}

static int interruptible_delay(void *context, long nanoseconds)
{
  const struct timespec requested = {
    .tv_sec = nanoseconds / 1000000000L,
    .tv_nsec = nanoseconds % 1000000000L
  };

  (void)context;
  return nanosleep(&requested, NULL);
}

int pidp_gpio_scan_single(struct pidp_gpio_v2 *backend,
    const uint32_t rows[PIDP_GPIO_V2_LED_ROWS],
    pidp_gpio_delay delay, void *context)
{
  unsigned int row;
  unsigned int column;

  if (backend == NULL || rows == NULL)
    return scan_failure(backend, EINVAL);
  if (delay == NULL)
    delay = interruptible_delay;
  for (row = 0; row < PIDP_GPIO_V2_LED_ROWS; ++row) {
    if (rows[row] & ~PIDP_GPIO_V2_LED_MASK)
      return scan_failure(backend, EINVAL);
  }
  for (row = 0; row < PIDP_GPIO_V2_LED_ROWS; ++row) {
    for (column = 0; column < PIDP_GPIO_V2_COLS; ++column) {
      uint32_t bit = rows[row] & (UINT32_C(1) << column);
      int result;
      int error;

      if (pidp_gpio_v2_display(backend, row, bit) < 0)
        return scan_failure(backend, errno);
      result = delay_for(context, delay, 50000L);
      error = errno;
      /* blank before handling interruption, logging, or any further delay. */
      if (pidp_gpio_v2_blank(backend) < 0)
        return scan_failure(backend, errno);
      if (result < 0)
        return scan_failure(backend, error);
      if (delay_for(context, delay, 10000L) < 0)
        return scan_failure(backend, errno);
    }
  }
  if (pidp_gpio_v2_idle(backend) < 0)
    return scan_failure(backend, errno);
  return 0;
}

int pidp_gpio_scan_rows(struct pidp_gpio_v2 *backend,
    const uint32_t rows[PIDP_GPIO_V2_LED_ROWS],
    pidp_gpio_delay delay, void *context)
{
  unsigned int row;

  if (backend == NULL || rows == NULL)
    return scan_failure(backend, EINVAL);
  if (delay == NULL)
    delay = interruptible_delay;
  for (row = 0; row < PIDP_GPIO_V2_LED_ROWS; ++row) {
    if (rows[row] & ~PIDP_GPIO_V2_LED_MASK)
      return scan_failure(backend, EINVAL);
  }
  for (row = 0; row < PIDP_GPIO_V2_LED_ROWS; ++row) {
    int result;
    int error;

    if (pidp_gpio_v2_display(backend, row, rows[row]) < 0)
      return scan_failure(backend, errno);
    result = delay_for(context, delay, 50000L);
    error = errno;
    /* blank before handling interruption, logging, or any further delay. */
    if (pidp_gpio_v2_blank(backend) < 0)
      return scan_failure(backend, errno);
    if (result < 0)
      return scan_failure(backend, error);
    if (delay_for(context, delay, 10000L) < 0)
      return scan_failure(backend, errno);
  }
  if (pidp_gpio_v2_idle(backend) < 0)
    return scan_failure(backend, errno);
  return 0;
}

int pidp_gpio_scan_input_row(struct pidp_gpio_v2 *backend, unsigned int row,
    volatile uint32_t switches[PIDP_GPIO_V2_SWITCH_ROWS],
    struct pidp_gpio_rotary *rotary, int knobs[2],
    pidp_gpio_delay delay, void *context)
{
  uint32_t published_bits = 0;
  uint32_t column_mask;

  if (backend == NULL || switches == NULL || rotary == NULL || knobs == NULL
      || row >= PIDP_GPIO_V2_SWITCH_ROWS)
    return scan_failure(backend, EINVAL);
  if (delay == NULL)
    delay = interruptible_delay;
  for (column_mask = UINT32_C(0x03f); column_mask <= UINT32_C(0xfc0);
      column_mask <<= 6) {
    uint32_t physical_bits;

    if (pidp_gpio_v2_select_switch_columns(backend, row, column_mask) < 0)
      return scan_failure(backend, errno);
    if (delay_for(context, delay, 100000L) < 0)
      return scan_failure(backend, errno);
    if (pidp_gpio_v2_read_switches(backend, &physical_bits) < 0)
      return scan_failure(backend, errno);
    published_bits |= physical_bits & column_mask;
  }
  if (row == 2)
    update_rotary(rotary, knobs, &published_bits);
  switches[row] = published_bits;
  if (pidp_gpio_v2_idle(backend) < 0)
    return scan_failure(backend, errno);
  return 0;
}

int pidp_gpio_scan_inputs(struct pidp_gpio_v2 *backend,
    volatile uint32_t switches[PIDP_GPIO_V2_SWITCH_ROWS],
    struct pidp_gpio_rotary *rotary, int knobs[2],
    pidp_gpio_delay delay, void *context)
{
  unsigned int row;

  for (row = 0; row < PIDP_GPIO_V2_SWITCH_ROWS; ++row) {
    if (pidp_gpio_scan_input_row(backend, row, switches, rotary, knobs,
        delay, context) < 0)
      return -1;
  }
  return 0;
}
