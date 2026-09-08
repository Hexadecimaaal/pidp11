#include "gpio_fixture.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static struct pidp_fixture_trace *original_trace;
static const struct pidp_fixture_inputs *original_inputs;
static struct pidp_fixture_state original_state;
static unsigned int original_active_row;
static size_t original_sample_index[PIDP_GPIO_V2_SWITCH_ROWS];
static unsigned int original_nanosleeps;
static int *original_terminate;

static struct pidp_fixture_snapshot snapshot_for(
    const struct pidp_fixture_state *state)
{
  struct pidp_fixture_snapshot snapshot;

  snapshot.led_output = state->led_output;
  snapshot.led_high = state->led_high & state->led_output;
  snapshot.col_output = state->col_output;
  snapshot.col_high = state->col_high & state->col_output;
  snapshot.switch_output = state->switch_output;
  snapshot.switch_high = state->switch_high & state->switch_output;
  return snapshot;
}

void pidp_fixture_trace_reset(struct pidp_fixture_trace *trace)
{
  memset(trace, 0, sizeof(*trace));
}

void pidp_fixture_record_initial(struct pidp_fixture_trace *trace,
    const struct pidp_fixture_state *state)
{
  if (!trace->has_initial) {
    trace->initial_state = snapshot_for(state);
    trace->has_initial = 1;
  }
}

void pidp_fixture_record_final(struct pidp_fixture_trace *trace,
    const struct pidp_fixture_state *state)
{
  trace->final_state = snapshot_for(state);
  trace->has_final = 1;
}

void pidp_fixture_record_state(struct pidp_fixture_trace *trace,
    const struct pidp_fixture_state *state)
{
  struct pidp_fixture_snapshot *snapshot;

  if (trace->state_count >= PIDP_FIXTURE_MAX_STATES) {
    trace->overflow = 1;
    return;
  }
  snapshot = &trace->states[trace->state_count++];
  *snapshot = snapshot_for(state);
}

void pidp_fixture_record_delay(struct pidp_fixture_trace *trace,
    const struct pidp_fixture_state *state, uint64_t duration_ns)
{
  struct pidp_fixture_interval *interval;

  if (trace->interval_count >= PIDP_FIXTURE_MAX_INTERVALS) {
    trace->overflow = 1;
  } else {
    interval = &trace->intervals[trace->interval_count++];
    interval->duration_ns = duration_ns;
    interval->state = snapshot_for(state);
  }
  trace->now_ns += duration_ns;
}

void pidp_fixture_record_sample(struct pidp_fixture_trace *trace,
    unsigned int row, uint32_t physical_bits)
{
  struct pidp_fixture_sample *sample;

  if (trace->sample_count >= PIDP_FIXTURE_MAX_SAMPLES) {
    trace->overflow = 1;
    return;
  }
  sample = &trace->samples[trace->sample_count++];
  sample->at_ns = trace->now_ns;
  sample->row = row;
  sample->physical_bits = physical_bits & PIDP_GPIO_V2_LED_MASK;
}

int pidp_fixture_nanosleep(const struct timespec *requested,
    struct timespec *remaining);
int pidp_fixture_usleep(useconds_t microseconds);
int pidp_fixture_pthread_setschedparam(pthread_t thread, int policy,
    const struct sched_param *parameter);

#define gpiolib_init pidp_fixture_gpiolib_init
#define gpiolib_mmap pidp_fixture_gpiolib_mmap
#define gpio_set_fsel pidp_fixture_gpio_set_fsel
#define gpio_set_dir pidp_fixture_gpio_set_dir
#define gpio_set_drive pidp_fixture_gpio_set_drive
#define gpio_set_pull pidp_fixture_gpio_set_pull
#define gpio_get_level pidp_fixture_gpio_get_level
#define nanosleep pidp_fixture_nanosleep
#define usleep pidp_fixture_usleep
#define pthread_setschedparam pidp_fixture_pthread_setschedparam
/* gpio.c remains byte-exact from commit 3bc7ff560d5481593dfe96325cdf0d030363c7de. */
#include "gpio.c"
#undef gpiolib_init
#undef gpiolib_mmap
#undef gpio_set_fsel
#undef gpio_set_dir
#undef gpio_set_drive
#undef gpio_set_pull
#undef gpio_get_level
#undef nanosleep
#undef usleep
#undef pthread_setschedparam

volatile uint32_t gpiopattern_ledstatus_phases[2]
    [GPIOPATTERN_LED_BRIGHTNESS_PHASES][8];
int gpiopattern_ledstatus_phases_readidx;
int gpiopattern_ledstatus_phases_writeidx;
volatile uint32_t gpio_switchstatus[3];
int knobValue[2];

static int original_led(unsigned int gpio)
{
  static const unsigned int pins[PIDP_GPIO_V2_LED_ROWS] =
      {20u, 21u, 22u, 23u, 24u, 25u};
  unsigned int i;

  for (i = 0; i < PIDP_GPIO_V2_LED_ROWS; ++i) {
    if (pins[i] == gpio)
      return (int)i;
  }
  return -1;
}

static int original_switch(unsigned int gpio)
{
  static const unsigned int pins[PIDP_GPIO_V2_SWITCH_ROWS] =
      {16u, 17u, 18u};
  unsigned int i;

  for (i = 0; i < PIDP_GPIO_V2_SWITCH_ROWS; ++i) {
    if (pins[i] == gpio)
      return (int)i;
  }
  return -1;
}

static int original_column(unsigned int gpio)
{
  static const unsigned int pins[PIDP_GPIO_V2_COLS] =
      {26u, 27u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u};
  unsigned int i;

  for (i = 0; i < PIDP_GPIO_V2_COLS; ++i) {
    if (pins[i] == gpio)
      return (int)i;
  }
  return -1;
}

static void original_record_state(void)
{
  pidp_fixture_record_state(original_trace, &original_state);
}

int pidp_fixture_gpiolib_init(void)
{
  return 1;
}

int pidp_fixture_gpiolib_mmap(void)
{
  return 0;
}

int pidp_fixture_pthread_setschedparam(pthread_t thread, int policy,
    const struct sched_param *parameter)
{
  /* scheduling is outside virtual time; never change the host thread priority. */
  (void)thread;
  (void)policy;
  (void)parameter;
  return 0;
}

void pidp_fixture_gpio_set_fsel(unsigned int gpio, const GPIO_FSEL_T func)
{
  if (func == GPIO_FSEL_OUTPUT)
    pidp_fixture_gpio_set_dir(gpio, DIR_OUTPUT);
  else if (func == GPIO_FSEL_INPUT)
    pidp_fixture_gpio_set_dir(gpio, DIR_INPUT);
}

void pidp_fixture_gpio_set_dir(unsigned int gpio, GPIO_DIR_T dir)
{
  int index;

  index = original_led(gpio);
  if (index >= 0) {
    if (dir == DIR_OUTPUT)
      original_state.led_output |= (uint16_t)(UINT16_C(1) << index);
    else
      original_state.led_output &= (uint16_t)~(UINT16_C(1) << index);
    original_record_state();
    return;
  }

  index = original_column(gpio);
  if (index >= 0) {
    if (dir == DIR_OUTPUT)
      original_state.col_output |= (uint16_t)(UINT16_C(1) << index);
    else
      original_state.col_output &= (uint16_t)~(UINT16_C(1) << index);
    original_record_state();
    return;
  }

  index = original_switch(gpio);
  if (index >= 0) {
    if (dir == DIR_OUTPUT) {
      original_state.switch_output |= (uint8_t)(UINT8_C(1) << index);
      original_active_row = (unsigned int)index;
    } else {
      original_state.switch_output &= (uint8_t)~(UINT8_C(1) << index);
      if (original_active_row == (unsigned int)index) {
        if (original_sample_index[index]
            < GPIOPATTERN_LED_BRIGHTNESS_PHASES)
          ++original_sample_index[index];
        original_active_row = PIDP_GPIO_V2_SWITCH_ROWS;
      }
    }
    original_record_state();
  }
}

void pidp_fixture_gpio_set_drive(unsigned int gpio, GPIO_DRIVE_T drive)
{
  int index;
  uint16_t bit16;
  uint8_t bit8;

  index = original_led(gpio);
  if (index >= 0) {
    bit16 = (uint16_t)(UINT16_C(1) << index);
    if (drive == DRIVE_HIGH)
      original_state.led_high |= bit16;
    else
      original_state.led_high &= (uint16_t)~bit16;
    original_record_state();
    return;
  }

  index = original_column(gpio);
  if (index >= 0) {
    bit16 = (uint16_t)(UINT16_C(1) << index);
    if (drive == DRIVE_HIGH)
      original_state.col_high |= bit16;
    else
      original_state.col_high &= (uint16_t)~bit16;
    original_record_state();
    return;
  }

  index = original_switch(gpio);
  if (index >= 0) {
    bit8 = (uint8_t)(UINT8_C(1) << index);
    if (drive == DRIVE_HIGH)
      original_state.switch_high |= bit8;
    else
      original_state.switch_high &= (uint8_t)~bit8;
    original_record_state();
  }
}

void pidp_fixture_gpio_set_pull(unsigned int gpio, GPIO_PULL_T pull)
{
  if (gpio == 18u)
    pidp_fixture_record_initial(original_trace, &original_state);
  (void)pull;
}

int pidp_fixture_gpio_get_level(unsigned int gpio)
{
  int column;
  uint32_t value;
  unsigned int row;

  column = original_column(gpio);
  if (column < 0 || original_active_row >= PIDP_GPIO_V2_SWITCH_ROWS)
    return 1;
  row = original_active_row;
  value = original_inputs->values[row][original_sample_index[row]];
  if (column == (int)(PIDP_GPIO_V2_COLS - 1u))
    pidp_fixture_record_sample(original_trace, row, value);
  return (value & (UINT32_C(1) << column)) != 0 ? 1 : 0;
}

int pidp_fixture_nanosleep(const struct timespec *requested,
    struct timespec *remaining)
{
  uint64_t duration_ns;

  if (requested == NULL)
    return -1;
  (void)remaining;
  duration_ns = (uint64_t)requested->tv_sec * UINT64_C(1000000000)
      + (uint64_t)requested->tv_nsec;
  pidp_fixture_record_delay(original_trace, &original_state, duration_ns);
  ++original_nanosleeps;
  if (original_nanosleeps % (PIDP_GPIO_V2_LED_ROWS
      + PIDP_GPIO_V2_SWITCH_ROWS) == 0) {
    gpiopattern_ledstatus_phases_readidx =
        (original_nanosleeps / (PIDP_GPIO_V2_LED_ROWS
            + PIDP_GPIO_V2_SWITCH_ROWS)) & 1u;
  }
  if (original_nanosleeps == GPIOPATTERN_LED_BRIGHTNESS_PHASES
      * (PIDP_GPIO_V2_LED_ROWS + PIDP_GPIO_V2_SWITCH_ROWS))
    *original_terminate = 1;
  return 0;
}

int pidp_fixture_usleep(useconds_t microseconds)
{
  pidp_fixture_record_delay(original_trace, &original_state,
      (uint64_t)microseconds * UINT64_C(1000));
  return 0;
}

int pidp_fixture_run_original(struct pidp_fixture_trace *trace,
    const struct pidp_fixture_inputs *inputs,
    volatile uint32_t patterns[2][GPIOPATTERN_LED_BRIGHTNESS_PHASES][8],
    int read_index, int knobs[2], uint32_t switches[3])
{
  int terminate = 0;
  unsigned int buffer;
  unsigned int phase;
  unsigned int bank;
  void *result;

  if (trace == NULL || inputs == NULL || patterns == NULL || knobs == NULL
      || switches == NULL || read_index < 0 || read_index > 1) {
    errno = EINVAL;
    return -1;
  }
  pidp_fixture_trace_reset(trace);
  original_trace = trace;
  original_inputs = inputs;
  memset(&original_state, 0, sizeof(original_state));
  original_active_row = PIDP_GPIO_V2_SWITCH_ROWS;
  memset(original_sample_index, 0, sizeof(original_sample_index));
  original_nanosleeps = 0;
  original_terminate = &terminate;
  memset((void *)gpio_switchstatus, 0, sizeof(gpio_switchstatus));
  knobValue[0] = 0;
  knobValue[1] = 0;
  check_rotary_encoders(0xf00);
  knobValue[0] = knobs[0];
  knobValue[1] = knobs[1];
  for (buffer = 0; buffer < 2; ++buffer)
    for (phase = 0; phase < GPIOPATTERN_LED_BRIGHTNESS_PHASES; ++phase)
      for (bank = 0; bank < 8; ++bank)
        gpiopattern_ledstatus_phases[buffer][phase][bank]
            = patterns[buffer][phase][bank];
  gpiopattern_ledstatus_phases_readidx = read_index;
  gpiopattern_ledstatus_phases_writeidx = !read_index;

  result = blink(&terminate);
  if (result == (void *)-1) {
    errno = EIO;
    return -1;
  }
  pidp_fixture_record_final(trace, &original_state);
  switches[0] = gpio_switchstatus[0];
  switches[1] = gpio_switchstatus[1];
  switches[2] = gpio_switchstatus[2];
  knobs[0] = knobValue[0];
  knobs[1] = knobValue[1];
  return trace->overflow ? -1 : 0;
}
