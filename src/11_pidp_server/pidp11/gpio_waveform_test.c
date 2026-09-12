#include "gpio_fixture.h"

#include "gpio_scan.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <string.h>

#define FIXTURE_CHIP_FD 31
#define FIXTURE_REQUEST_FD 73
#define FIXTURE_CHIP_LINES 128u
#define FIXTURE_ALL_COLS UINT16_C(0x0fff)
#define FIXTURE_ALL_LEDS UINT16_C(0x003f)

struct port_fake {
  struct pidp_fixture_trace *trace;
  const struct pidp_fixture_inputs *inputs;
  volatile int *read_index;
  struct pidp_fixture_state state;
  size_t sample_index[PIDP_GPIO_V2_SWITCH_ROWS];
  unsigned int delay_count;
  int chip_open;
  int request_open;
};
static void port_record_state(struct port_fake *fake)
{
  pidp_fixture_record_state(fake->trace, &fake->state);
}

static void port_set_line_direction(struct pidp_fixture_state *state,
    unsigned int line, int output)
{
  if (line < PIDP_GPIO_V2_LED_ROWS) {
    if (output)
      state->led_output |= (uint16_t)(UINT16_C(1) << line);
    else
      state->led_output &= (uint16_t)~(UINT16_C(1) << line);
  } else if (line < PIDP_GPIO_V2_LED_ROWS + PIDP_GPIO_V2_COLS) {
    unsigned int column = line - PIDP_GPIO_V2_LED_ROWS;
    if (output)
      state->col_output |= (uint16_t)(UINT16_C(1) << column);
    else
      state->col_output &= (uint16_t)~(UINT16_C(1) << column);
  } else {
    unsigned int row = line - PIDP_GPIO_V2_LED_ROWS - PIDP_GPIO_V2_COLS;
    if (output)
      state->switch_output |= (uint8_t)(UINT8_C(1) << row);
    else
      state->switch_output &= (uint8_t)~(UINT8_C(1) << row);
  }
}

static void port_set_line_level(struct pidp_fixture_state *state,
    unsigned int line, int high)
{
  if (line < PIDP_GPIO_V2_LED_ROWS) {
    if (high)
      state->led_high |= (uint16_t)(UINT16_C(1) << line);
    else
      state->led_high &= (uint16_t)~(UINT16_C(1) << line);
  } else if (line < PIDP_GPIO_V2_LED_ROWS + PIDP_GPIO_V2_COLS) {
    unsigned int column = line - PIDP_GPIO_V2_LED_ROWS;
    if (high)
      state->col_high |= (uint16_t)(UINT16_C(1) << column);
    else
      state->col_high &= (uint16_t)~(UINT16_C(1) << column);
  } else {
    unsigned int row = line - PIDP_GPIO_V2_LED_ROWS - PIDP_GPIO_V2_COLS;
    if (high)
      state->switch_high |= (uint8_t)(UINT8_C(1) << row);
    else
      state->switch_high &= (uint8_t)~(UINT8_C(1) << row);
  }
}

static void port_apply_config(struct port_fake *fake,
    const struct gpio_v2_line_config *config)
{
  uint64_t output_mask = 0;
  uint64_t value_mask = 0;
  uint64_t value_bits = 0;
  unsigned int i;

  for (i = 0; i < config->num_attrs; ++i) {
    const struct gpio_v2_line_config_attribute *attribute
        = &config->attrs[i];
    if (attribute->attr.id == GPIO_V2_LINE_ATTR_ID_FLAGS) {
      if (attribute->attr.flags & GPIO_V2_LINE_FLAG_OUTPUT)
        output_mask |= attribute->mask;
    } else if (attribute->attr.id
        == GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES) {
      value_mask |= attribute->mask;
      value_bits |= attribute->attr.values;
    }
  }

  memset(&fake->state, 0, sizeof(fake->state));
  for (i = 0; i < PIDP_GPIO_V2_LINES; ++i) {
    int output = (output_mask & (UINT64_C(1) << i)) != 0;
    int high = output && (value_mask & (UINT64_C(1) << i)) != 0
        && (value_bits & (UINT64_C(1) << i)) != 0;
    port_set_line_direction(&fake->state, i, output);
    port_set_line_level(&fake->state, i, high);
  }
  port_record_state(fake);
}

static void port_set_values(struct port_fake *fake,
    const struct gpio_v2_line_values *values)
{
  unsigned int i;

  for (i = 0; i < PIDP_GPIO_V2_LINES; ++i) {
    if (values->mask & (UINT64_C(1) << i))
      port_set_line_level(&fake->state, i,
          (values->bits & (UINT64_C(1) << i)) != 0);
  }
  port_record_state(fake);
}

static int port_open(void *context, const char *path, int flags)
{
  struct port_fake *fake = context;

  if (strcmp(path, "/dev/gpiochip-fixture") != 0
      || flags != (O_RDWR | O_CLOEXEC) || fake->chip_open) {
    errno = EINVAL;
    return -1;
  }
  fake->chip_open = 1;
  return FIXTURE_CHIP_FD;
}

static int port_ioctl(void *context, int fd, unsigned long request,
    void *argument)
{
  struct port_fake *fake = context;

  if (request == GPIO_GET_CHIPINFO_IOCTL) {
    struct gpiochip_info *info = argument;
    if (fd != FIXTURE_CHIP_FD || !fake->chip_open) {
      errno = EBADF;
      return -1;
    }
    memset(info, 0, sizeof(*info));
    info->lines = FIXTURE_CHIP_LINES;
    return 0;
  }
  if (request == GPIO_V2_GET_LINE_IOCTL) {
    struct gpio_v2_line_request *line_request = argument;
    if (fd != FIXTURE_CHIP_FD || !fake->chip_open || fake->request_open
        || line_request->num_lines != PIDP_GPIO_V2_LINES) {
      errno = EINVAL;
      return -1;
    }
    line_request->fd = FIXTURE_REQUEST_FD;
    fake->request_open = 1;
    port_apply_config(fake, &line_request->config);
    return 0;
  }
  if (fd != FIXTURE_REQUEST_FD || !fake->request_open) {
    errno = EBADF;
    return -1;
  }
  if (request == GPIO_V2_LINE_SET_CONFIG_IOCTL) {
    port_apply_config(fake, argument);
    return 0;
  }
  if (request == GPIO_V2_LINE_SET_VALUES_IOCTL) {
    port_set_values(fake, argument);
    return 0;
  }
  if (request == GPIO_V2_LINE_GET_VALUES_IOCTL) {
    struct gpio_v2_line_values *values = argument;
    unsigned int row;
    uint32_t physical_bits;

    if (fake->state.switch_output == 0
        || (fake->state.switch_output
            & (uint8_t)(fake->state.switch_output - 1u)) != 0) {
      errno = EINVAL;
      return -1;
    }
    row = 0;
    while ((fake->state.switch_output & (UINT8_C(1) << row)) == 0)
      ++row;
    if (fake->sample_index[row] >= GPIOPATTERN_LED_BRIGHTNESS_PHASES) {
      errno = EOVERFLOW;
      return -1;
    }
    physical_bits = fake->inputs->values[row][fake->sample_index[row]++];
    pidp_fixture_record_sample(fake->trace, row, physical_bits);
    values->bits = ((uint64_t)physical_bits << PIDP_GPIO_V2_LED_ROWS);
    return 0;
  }

  errno = ENOTTY;
  return -1;
}

static int port_close(void *context, int fd)
{
  struct port_fake *fake = context;

  if (fd == FIXTURE_CHIP_FD && fake->chip_open) {
    fake->chip_open = 0;
    return 0;
  }
  if (fd == FIXTURE_REQUEST_FD && fake->request_open) {
    fake->request_open = 0;
    return 0;
  }
  errno = EBADF;
  return -1;
}

static const struct pidp_gpio_v2_ops port_ops = {
  .open = port_open,
  .ioctl = port_ioctl,
  .close = port_close,
};

static void port_mapping(struct pidp_gpio_v2_mapping *mapping)
{
  unsigned int i;

  mapping->chip_path = "/dev/gpiochip-fixture";
  for (i = 0; i < PIDP_GPIO_V2_LINES; ++i)
    mapping->offsets[i] = i;
}

static int port_delay(void *context, long nanoseconds)
{
  struct port_fake *fake = context;

  pidp_fixture_record_delay(fake->trace, &fake->state,
      (uint64_t)nanoseconds);
  ++fake->delay_count;
  if (fake->delay_count % (PIDP_GPIO_V2_LED_ROWS * 2u
      + PIDP_GPIO_V2_SWITCH_ROWS) == 0) {
    *fake->read_index = (fake->delay_count
        / (PIDP_GPIO_V2_LED_ROWS * 2u + PIDP_GPIO_V2_SWITCH_ROWS)) & 1u;
  }
  return 0;
}

static int open_port(struct port_fake *fake, struct pidp_gpio_v2 *backend,
    struct pidp_fixture_trace *trace,
    const struct pidp_fixture_inputs *inputs, volatile int *read_index)
{
  struct pidp_gpio_v2_mapping mapping;

  memset(fake, 0, sizeof(*fake));
  fake->trace = trace;
  fake->inputs = inputs;
  fake->read_index = read_index;
  memset(backend, 0, sizeof(*backend));
  port_mapping(&mapping);
  if (pidp_gpio_v2_open(backend, &mapping, &port_ops, fake) < 0)
    return -1;
  pidp_fixture_record_initial(trace, &fake->state);
  return 0;
}

int pidp_fixture_run_port(struct pidp_fixture_trace *trace,
    const struct pidp_fixture_inputs *inputs,
    volatile uint32_t patterns[2][GPIOPATTERN_LED_BRIGHTNESS_PHASES][8],
    int read_index, int knobs[2], uint32_t switches[3])
{
  struct pidp_gpio_v2 backend;
  struct pidp_fixture_state final_state;
  struct pidp_fixture_trace *saved_trace;
  struct port_fake fake;
  struct pidp_gpio_rotary rotary = {{3, 3}};
  int local_knobs[2];
  int local_index = read_index;
  int result;

  if (trace == NULL || inputs == NULL || patterns == NULL || knobs == NULL
      || switches == NULL || read_index < 0 || read_index > 1) {
    errno = EINVAL;
    return -1;
  }
  pidp_fixture_trace_reset(trace);
  saved_trace = trace;
  if (open_port(&fake, &backend, trace, inputs, &local_index) < 0)
    return -1;
  local_knobs[0] = knobs[0];
  local_knobs[1] = knobs[1];
  result = pidp_gpio_scan_cycle(&backend, patterns, &local_index, switches,
      &rotary, local_knobs, port_delay, &fake);
  if (result == 0) {
    if (pidp_gpio_v2_close(&backend) < 0)
      result = -1;
  }
  final_state = fake.state;
  pidp_fixture_record_final(saved_trace, &final_state);
  knobs[0] = local_knobs[0];
  knobs[1] = local_knobs[1];
  return result;
}

struct failing_delay {
  unsigned int calls;
  unsigned int fail_call;
  struct port_fake *fake;
};

static int fail_delay(void *context, long nanoseconds)
{
  struct failing_delay *failure = context;

  ++failure->calls;
  if (failure->calls == failure->fail_call) {
    errno = ETIMEDOUT;
    return -1;
  }
  return port_delay(failure->fake, nanoseconds);
}

static int snapshot_equal(const struct pidp_fixture_snapshot *left,
    const struct pidp_fixture_snapshot *right)
{
  return left->led_output == right->led_output
      && left->led_high == right->led_high
      && left->col_output == right->col_output
      && left->col_high == right->col_high
      && left->switch_output == right->switch_output
      && left->switch_high == right->switch_high;
}

static unsigned int bit_count(unsigned int value)
{
  unsigned int count = 0;

  while (value != 0) {
    value &= value - 1u;
    ++count;
  }
  return count;
}

static int safe_trace(const char *name, const struct pidp_fixture_trace *trace)
{
  size_t i;
  struct pidp_fixture_snapshot previous;

  if (trace->overflow || trace->state_count == 0) {
    fprintf(stderr, "%s trace overflow or no state\n", name);
    return 0;
  }
  previous = trace->states[0];
  for (i = 0; i < trace->state_count; ++i) {
    const struct pidp_fixture_snapshot *state = &trace->states[i];
    unsigned int enabled = state->led_output & state->led_high;
    unsigned int previous_enabled = previous.led_output & previous.led_high;

    if (bit_count(enabled) > 1u
        || bit_count(state->switch_output) > 1u
        || state->switch_high != 0
        || (enabled != 0 && (state->col_output != FIXTURE_ALL_COLS
            || state->switch_output != 0))
        || (state->switch_output != 0 && state->col_output != 0)) {
      fprintf(stderr, "%s has unsafe enabled-row state %zu\n", name, i);
      return 0;
    }
    if (i != 0 && (previous_enabled != 0 || enabled != 0)
        && (previous.col_output != state->col_output
            || previous.col_high != state->col_high
            || previous.switch_output != state->switch_output
            || (previous_enabled != 0 && enabled != 0
                && previous_enabled != enabled))) {
      fprintf(stderr, "%s changed columns or row selection while enabled\n", name);
      return 0;
    }
    previous = *state;
  }
  return 1;
}

static int compare_traces(const char *name,
    const struct pidp_fixture_trace *original,
    const struct pidp_fixture_trace *port,
    const uint32_t original_switches[3], const uint32_t port_switches[3],
    const int original_knobs[2], const int port_knobs[2],
    const int expected_knobs[2])
{
  size_t i;

  if (!safe_trace("original", original) || !safe_trace("port", port))
    return 0;
  if (original->interval_count != port->interval_count
      || original->interval_count
          != GPIOPATTERN_LED_BRIGHTNESS_PHASES
              * (PIDP_GPIO_V2_LED_ROWS * 2u + PIDP_GPIO_V2_SWITCH_ROWS)) {
    fprintf(stderr, "%s interval count differs\n", name);
    return 0;
  }
  if (original->now_ns != UINT64_C(11206500)
      || port->now_ns != original->now_ns) {
    fprintf(stderr, "%s virtual time differs\n", name);
    return 0;
  }
  for (i = 0; i < original->interval_count; ++i) {
    if (original->intervals[i].duration_ns
            != port->intervals[i].duration_ns
        || !snapshot_equal(&original->intervals[i].state,
            &port->intervals[i].state)) {
      fprintf(stderr, "%s interval %zu differs\n", name, i);
      return 0;
    }
  }
  if (original->sample_count != port->sample_count
      || original->sample_count
          != GPIOPATTERN_LED_BRIGHTNESS_PHASES * PIDP_GPIO_V2_SWITCH_ROWS) {
    fprintf(stderr, "%s sample count differs\n", name);
    return 0;
  }
  for (i = 0; i < original->sample_count; ++i) {
    if (original->samples[i].at_ns != port->samples[i].at_ns
        || original->samples[i].row != port->samples[i].row
        || original->samples[i].physical_bits
            != port->samples[i].physical_bits) {
      fprintf(stderr, "%s sample %zu differs\n", name, i);
      return 0;
    }
  }
  for (i = 0; i < 3; ++i) {
    if (original_switches[i] != port_switches[i]) {
      fprintf(stderr, "%s published switch row %zu differs\n", name, i);
      return 0;
    }
  }
  if (original_knobs[0] != port_knobs[0]
      || original_knobs[1] != port_knobs[1]
      || original_knobs[0] != expected_knobs[0]
      || original_knobs[1] != expected_knobs[1]) {
    fprintf(stderr, "%s rotary result differs or did not move\n", name);
    return 0;
  }
  /* legacy startup drives columns; v2 opens them as pull-up inputs. */
  if (!original->has_initial || !port->has_initial
      || original->initial_state.col_output != FIXTURE_ALL_COLS
      || port->initial_state.col_output != 0) {
    fprintf(stderr, "%s startup states differ as expected check failed\n", name);
    return 0;
  }
  if (!original->has_final || !port->has_final
      || !snapshot_equal(&original->final_state, &port->final_state)) {
    fprintf(stderr, "%s shutdown states differ\n", name);
    return 0;
  }
  return 1;
}

static void fill_inputs(struct pidp_fixture_inputs *inputs)
{
  static const unsigned int first_codes[5] = {3u, 1u, 0u, 2u, 3u};
  static const unsigned int second_codes[5] = {3u, 2u, 0u, 1u, 3u};
  unsigned int phase;

  memset(inputs, 0, sizeof(*inputs));
  for (phase = 0; phase < GPIOPATTERN_LED_BRIGHTNESS_PHASES; ++phase) {
    unsigned int first = phase < 5u ? first_codes[phase] : 3u;
    unsigned int second = phase < 5u ? second_codes[phase] : 3u;
    inputs->values[0][phase] = UINT32_C(0x055);
    inputs->values[1][phase] = UINT32_C(0xa5a);
    inputs->values[2][phase] = UINT32_C(0x05a)
        | (first << 8) | (second << 10);
  }
}

static void fill_patterns(volatile uint32_t patterns[2]
    [GPIOPATTERN_LED_BRIGHTNESS_PHASES][8], unsigned int mode)
{
  unsigned int phase;
  unsigned int row;

  memset((void *)patterns, 0, sizeof(uint32_t) * 2u
      * GPIOPATTERN_LED_BRIGHTNESS_PHASES * 8u);
  for (phase = 0; phase < GPIOPATTERN_LED_BRIGHTNESS_PHASES; ++phase) {
    for (row = 0; row < PIDP_GPIO_V2_LED_ROWS; ++row) {
      if (mode == 0) {
        patterns[0][phase][row] = 0;
        patterns[1][phase][row] = 0;
      } else if (mode == 1) {
        patterns[0][phase][row] = PIDP_GPIO_V2_LED_MASK;
        patterns[1][phase][row] = PIDP_GPIO_V2_LED_MASK;
      } else {
        patterns[0][phase][row] = UINT32_C(0xf000)
            | ((phase * 37u + row * 73u) & PIDP_GPIO_V2_LED_MASK);
        patterns[1][phase][row] = UINT32_C(0xf000)
            | ((phase * 53u + row * 19u + 0x155u)
                & PIDP_GPIO_V2_LED_MASK);
      }
    }
  }
}

static int run_pattern_case(unsigned int mode, int first_knob, int second_knob)
{
  struct pidp_fixture_inputs inputs;
  struct pidp_fixture_trace original_trace;
  struct pidp_fixture_trace port_trace;
  volatile uint32_t patterns[2][GPIOPATTERN_LED_BRIGHTNESS_PHASES][8];
  uint32_t original_switches[3] = {0};
  uint32_t port_switches[3] = {0};
  int original_knobs[2] = {first_knob, second_knob};
  int port_knobs[2] = {first_knob, second_knob};
  const int expected_knobs[2] = {(first_knob + 1) & 7, (second_knob + 3) & 3};
  char name[32];

  fill_inputs(&inputs);
  fill_patterns(patterns, mode);
  if (pidp_fixture_run_original(&original_trace, &inputs, patterns, 0,
      original_knobs, original_switches) < 0)
    return 0;
  if (pidp_fixture_run_port(&port_trace, &inputs, patterns, 0,
      port_knobs, port_switches) < 0)
    return 0;
  (void)snprintf(name, sizeof(name), "pattern-%u-knobs-%d-%d",
      mode, first_knob, second_knob);
  return compare_traces(name, &original_trace, &port_trace,
      original_switches, port_switches, original_knobs, port_knobs, expected_knobs);
}

static int single_pattern_case(const char *name, uint32_t value)
{
  struct pidp_fixture_inputs inputs;
  struct pidp_fixture_trace trace;
  struct pidp_gpio_v2 backend;
  struct port_fake fake;
  uint32_t rows[PIDP_GPIO_V2_LED_ROWS];
  int index = 0;
  unsigned int row;
  size_t i;

  memset(&inputs, 0, sizeof(inputs));
  memset(&trace, 0, sizeof(trace));
  memset(&backend, 0, sizeof(backend));
  for (row = 0; row < PIDP_GPIO_V2_LED_ROWS; ++row)
    rows[row] = value;
  if (open_port(&fake, &backend, &trace, &inputs, &index) < 0)
    return 0;
  if (pidp_gpio_scan_single(&backend, rows, port_delay, &fake) < 0
      || pidp_gpio_v2_close(&backend) < 0) {
    fprintf(stderr, "%s single-cell scan failed\n", name);
    (void)pidp_gpio_v2_close(&backend);
    return 0;
  }
  pidp_fixture_record_final(&trace, &fake.state);
  if (!safe_trace(name, &trace)
      || trace.sample_count != 0
      || trace.interval_count != PIDP_GPIO_V2_LED_ROWS * PIDP_GPIO_V2_COLS * 2u
      || fake.request_open || fake.chip_open) {
    fprintf(stderr, "%s selected or sampled a switch row\n", name);
    return 0;
  }
  for (i = 0; i < trace.interval_count; ++i) {
    const struct pidp_fixture_interval *interval = &trace.intervals[i];
    const struct pidp_fixture_snapshot *state = &interval->state;
    unsigned int cell = (unsigned int)(i / 2u);
    unsigned int expected_row = cell / PIDP_GPIO_V2_COLS;
    unsigned int expected_column = cell % PIDP_GPIO_V2_COLS;

    if (interval->duration_ns != (i % 2u == 0 ? 50000u : 10000u)
        || state->switch_output != 0
        || state->col_output != FIXTURE_ALL_COLS
        || (state->led_output & state->led_high)
            != (i % 2u == 0 ? 1u << expected_row : 0)
        || ((~state->col_high) & FIXTURE_ALL_COLS)
            != (rows[expected_row] & (1u << expected_column))) {
      fprintf(stderr, "%s displayed the wrong cell or interval\n", name);
      return 0;
    }
  }
  for (i = 0; i < trace.state_count; ++i) {
    const struct pidp_fixture_snapshot *state = &trace.states[i];
    unsigned int enabled = state->led_output & state->led_high;
    unsigned int active_columns =
        (~state->col_high) & FIXTURE_ALL_COLS;

    if (state->switch_output != 0) {
      fprintf(stderr, "%s drove a switch row\n", name);
      return 0;
    }

    if (enabled != 0 && bit_count(enabled) > 1u) {
      fprintf(stderr, "%s enabled more than one row\n", name);
      return 0;
    }
    if (enabled != 0 && bit_count(active_columns) > 1u) {
      fprintf(stderr, "%s enabled more than one LED\n", name);
      return 0;
    }
  }
  return 1;
}

static int row_pattern_case(const char *name,
    const uint32_t rows[PIDP_GPIO_V2_LED_ROWS])
{
  struct pidp_fixture_inputs inputs;
  struct pidp_fixture_trace trace;
  struct pidp_gpio_v2 backend;
  struct port_fake fake;
  unsigned int row;
  size_t i;
  int index = 0;

  memset(&inputs, 0, sizeof(inputs));
  memset(&trace, 0, sizeof(trace));
  memset(&backend, 0, sizeof(backend));
  if (open_port(&fake, &backend, &trace, &inputs, &index) < 0)
    return 0;
  if (pidp_gpio_scan_rows(&backend, rows, port_delay, &fake) < 0
      || pidp_gpio_v2_close(&backend) < 0) {
    fprintf(stderr, "%s row scan failed\n", name);
    (void)pidp_gpio_v2_close(&backend);
    return 0;
  }
  pidp_fixture_record_final(&trace, &fake.state);
  if (!safe_trace(name, &trace)
      || trace.sample_count != 0
      || trace.interval_count != PIDP_GPIO_V2_LED_ROWS * 2u
      || fake.request_open || fake.chip_open
      || trace.final_state.led_output != ((1u << PIDP_GPIO_V2_LED_ROWS) - 1u)
      || trace.final_state.led_high != 0
      || trace.final_state.col_output != 0
      || trace.final_state.switch_output != 0) {
    fprintf(stderr, "%s row scan did not safely idle and close\n", name);
    return 0;
  }
  for (i = 0; i < trace.interval_count; ++i) {
    const struct pidp_fixture_interval *interval = &trace.intervals[i];
    const struct pidp_fixture_snapshot *state = &interval->state;
    row = (unsigned int)(i / 2u);
    if (interval->duration_ns != (i % 2u == 0 ? 50000u : 10000u)
        || state->switch_output != 0
        || state->col_output != FIXTURE_ALL_COLS
        || (state->led_output & state->led_high)
            != (i % 2u == 0 ? 1u << row : 0)
        || ((~state->col_high) & FIXTURE_ALL_COLS) != rows[row]) {
      fprintf(stderr, "%s displayed the wrong row or interval\n", name);
      return 0;
    }
  }
  return 1;
}

static int input_row_case(void)
{
  struct pidp_fixture_inputs inputs;
  struct pidp_fixture_trace trace;
  struct pidp_gpio_v2 backend;
  struct port_fake fake;
  struct pidp_gpio_rotary rotary = {{1, 2}};
  uint32_t switches[PIDP_GPIO_V2_SWITCH_ROWS] =
      {UINT32_C(0x111), UINT32_C(0x222), UINT32_C(0x333)};
  int knobs[2] = {5, 2};
  int index = 0;
  size_t i;

  memset(&inputs, 0, sizeof(inputs));
  memset(&trace, 0, sizeof(trace));
  inputs.values[1][0] = UINT32_C(0xa5a);
  if (open_port(&fake, &backend, &trace, &inputs, &index) < 0)
    return 0;
  if (pidp_gpio_scan_input_row(&backend, 1, switches, &rotary, knobs,
      port_delay, &fake) < 0
      || pidp_gpio_v2_close(&backend) < 0) {
    fprintf(stderr, "input row scan failed\n");
    (void)pidp_gpio_v2_close(&backend);
    return 0;
  }
  pidp_fixture_record_final(&trace, &fake.state);
  if (!safe_trace("input row", &trace)
      || trace.sample_count != 1 || trace.interval_count != 1
      || trace.samples[0].row != 1
      || trace.samples[0].physical_bits != UINT32_C(0xa5a)
      || switches[0] != UINT32_C(0x111)
      || switches[1] != UINT32_C(0xa5a)
      || switches[2] != UINT32_C(0x333)
      || rotary.last_code[0] != 1 || rotary.last_code[1] != 2
      || knobs[0] != 5 || knobs[1] != 2
      || fake.request_open || fake.chip_open
      || trace.final_state.switch_output != 0
      || trace.final_state.led_high != 0
      || trace.final_state.col_output != 0) {
    fprintf(stderr, "input row changed an unselected row or rotary state\n");
    return 0;
  }
  for (i = 0; i < trace.interval_count; ++i) {
    const struct pidp_fixture_interval *interval = &trace.intervals[i];
    const struct pidp_fixture_snapshot *state = &interval->state;

    if (interval->duration_ns != 3000000u
        || state->led_output != ((1u << PIDP_GPIO_V2_LED_ROWS) - 1u)
        || state->led_high != 0 || state->col_output != 0
        || state->switch_output != (1u << 1)
        || (state->switch_high & state->switch_output) != 0) {
      fprintf(stderr, "input row selected an unsafe switch state\n");
      return 0;
    }
  }
  return 1;
}

static int input_row_encoder_case(void)
{
  struct pidp_fixture_inputs inputs;
  struct pidp_fixture_trace trace;
  struct pidp_gpio_v2 backend;
  struct port_fake fake;
  struct pidp_gpio_rotary rotary = {{3, 3}};
  uint32_t switches[PIDP_GPIO_V2_SWITCH_ROWS] =
      {UINT32_C(0x111), UINT32_C(0x222), UINT32_C(0x333)};
  int knobs[2] = {1, 1};
  int index = 0;

  memset(&inputs, 0, sizeof(inputs));
  memset(&trace, 0, sizeof(trace));
  inputs.values[2][0] = UINT32_C(0x102);
  inputs.values[2][1] = UINT32_C(0x303);
  if (open_port(&fake, &backend, &trace, &inputs, &index) < 0)
    return 0;
  if (pidp_gpio_scan_input_row(&backend, 2, switches, &rotary, knobs,
      port_delay, &fake) < 0
      || pidp_gpio_scan_input_row(&backend, 2, switches, &rotary, knobs,
          port_delay, &fake) < 0
      || pidp_gpio_v2_close(&backend) < 0) {
    fprintf(stderr, "input row encoder scan failed\n");
    (void)pidp_gpio_v2_close(&backend);
    return 0;
  }
  pidp_fixture_record_final(&trace, &fake.state);
  if (!safe_trace("input row encoder", &trace)
      || trace.sample_count != 2 || trace.interval_count != 2
      || trace.samples[0].row != 2 || trace.samples[1].row != 2
      || switches[0] != UINT32_C(0x111)
      || switches[1] != UINT32_C(0x222)
      || switches[2] != UINT32_C(0x103)
      || rotary.last_code[0] != 3 || rotary.last_code[1] != 3
      || knobs[0] != 2 || knobs[1] != 1
      || fake.request_open || fake.chip_open) {
    fprintf(stderr, "input row encoder state was not updated\n");
    return 0;
  }
  if (trace.intervals[0].duration_ns != 3000000u
      || trace.intervals[1].duration_ns != 3000000u
      || trace.intervals[0].state.switch_output != (1u << 2)
      || trace.intervals[1].state.switch_output != (1u << 2)) {
    fprintf(stderr, "input row encoder used the wrong waveform\n");
    return 0;
  }
  return 1;
}

static int input_row_invalid_case(void)
{
  struct pidp_fixture_inputs inputs;
  struct pidp_fixture_trace trace;
  struct pidp_gpio_v2 backend;
  struct port_fake fake;
  struct pidp_gpio_rotary rotary = {{3, 3}};
  uint32_t switches[PIDP_GPIO_V2_SWITCH_ROWS] =
      {UINT32_C(0x111), UINT32_C(0x222), UINT32_C(0x333)};
  int knobs[2] = {1, 1};
  int index = 0;

  memset(&inputs, 0, sizeof(inputs));
  memset(&trace, 0, sizeof(trace));
  if (open_port(&fake, &backend, &trace, &inputs, &index) < 0)
    return 0;
  if (pidp_gpio_scan_input_row(&backend, PIDP_GPIO_V2_SWITCH_ROWS,
      switches, &rotary, knobs, port_delay, &fake) >= 0
      || errno != EINVAL) {
    fprintf(stderr, "input row bound was not rejected\n");
    (void)pidp_gpio_v2_close(&backend);
    return 0;
  }
  pidp_fixture_record_final(&trace, &fake.state);
  if (!safe_trace("input row invalid", &trace)
      || trace.sample_count != 0 || trace.interval_count != 0
      || switches[0] != UINT32_C(0x111)
      || switches[1] != UINT32_C(0x222)
      || switches[2] != UINT32_C(0x333)
      || fake.request_open || fake.chip_open
      || fake.state.led_high != 0 || fake.state.col_output != 0
      || fake.state.switch_output != 0) {
    fprintf(stderr, "invalid input row did not safely close\n");
    return 0;
  }
  return 1;
}

static int input_scan_case(void)
{
  struct pidp_fixture_inputs inputs;
  struct pidp_fixture_trace trace;
  struct pidp_gpio_v2 backend;
  struct port_fake fake;
  struct pidp_gpio_rotary rotary = {{3, 3}};
  uint32_t switches[PIDP_GPIO_V2_SWITCH_ROWS] = {0};
  int knobs[2] = {1, 1};
  int index = 0;
  unsigned int phase;
  size_t i;

  memset(&inputs, 0, sizeof(inputs));
  memset(&trace, 0, sizeof(trace));
  for (phase = 0; phase < GPIOPATTERN_LED_BRIGHTNESS_PHASES; ++phase) {
    inputs.values[0][phase] = UINT32_C(0xa55);
    inputs.values[1][phase] = UINT32_C(0x155);
  }
  inputs.values[2][0] = UINT32_C(0x100);
  inputs.values[2][1] = UINT32_C(0x300);
  inputs.values[2][2] = UINT32_C(0x400);
  inputs.values[2][3] = UINT32_C(0xc00);
  if (open_port(&fake, &backend, &trace, &inputs, &index) < 0)
    return 0;
  for (phase = 0; phase < 4; ++phase) {
    if (pidp_gpio_scan_inputs(&backend, switches, &rotary, knobs,
        port_delay, &fake) < 0) {
      fprintf(stderr, "input scan failed\n");
      (void)pidp_gpio_v2_close(&backend);
      return 0;
    }
  }
  if (pidp_gpio_v2_close(&backend) < 0) {
    fprintf(stderr, "input scan close failed\n");
    return 0;
  }
  pidp_fixture_record_final(&trace, &fake.state);
  if (!safe_trace("input scan", &trace)
      || trace.sample_count != PIDP_GPIO_V2_SWITCH_ROWS * 4u
      || trace.interval_count != PIDP_GPIO_V2_SWITCH_ROWS * 4u
      || fake.request_open || fake.chip_open
      || knobs[0] != 2 || knobs[1] != 2
      || trace.final_state.led_output != ((1u << PIDP_GPIO_V2_LED_ROWS) - 1u)
      || trace.final_state.led_high != 0
      || trace.final_state.col_output != 0
      || trace.final_state.switch_output != 0) {
    fprintf(stderr, "input scan did not preserve rows, rotary, or cleanup\n");
    return 0;
  }
  if (switches[0] != UINT32_C(0xa55)
      || switches[1] != UINT32_C(0x155)
      || switches[2] != UINT32_C(0x400)
      || rotary.last_code[0] != 3 || rotary.last_code[1] != 3) {
    fprintf(stderr, "input scan returned wrong switch or rotary state\n");
    return 0;
  }
  for (i = 0; i < trace.interval_count; ++i) {
    const struct pidp_fixture_interval *interval = &trace.intervals[i];
    const struct pidp_fixture_snapshot *state = &interval->state;
    if (interval->duration_ns != 3000000u
        || state->led_output != ((1u << PIDP_GPIO_V2_LED_ROWS) - 1u)
        || state->led_high != 0 || state->col_output != 0
        || state->switch_output != (1u << (i % PIDP_GPIO_V2_SWITCH_ROWS))
        || (state->switch_high & state->switch_output) != 0) {
      fprintf(stderr, "input scan selected unsafe switch state\n");
      return 0;
    }
  }
  return 1;
}

static int interrupt_delay(void *context, long nanoseconds)
{
  int result = fail_delay(context, nanoseconds);

  if (result < 0)
    errno = EINTR;
  return result;
}

static int row_failure_case(unsigned int fail_call, int interrupted)
{
  struct pidp_fixture_inputs inputs;
  struct pidp_fixture_trace trace;
  struct pidp_gpio_v2 backend;
  struct port_fake fake;
  struct failing_delay failure = {0, fail_call, &fake};
  uint32_t rows[PIDP_GPIO_V2_LED_ROWS] = {1, 3, 0x155, 0x2aa, 0x555, 0xa55};
  int index = 0;
  int expected_error = fail_call == 0 ? EINVAL : interrupted ? EINTR : ETIMEDOUT;

  memset(&inputs, 0, sizeof(inputs));
  memset(&trace, 0, sizeof(trace));
  memset(&backend, 0, sizeof(backend));
  if (open_port(&fake, &backend, &trace, &inputs, &index) < 0)
    return 0;
  if (fail_call == 0)
    rows[PIDP_GPIO_V2_LED_ROWS - 1] = PIDP_GPIO_V2_LED_MASK + 1u;
  if (pidp_gpio_scan_rows(&backend, rows,
      interrupted ? interrupt_delay : fail_delay, &failure) >= 0
      || errno != expected_error) {
    fprintf(stderr, "row scan did not return its failure\n");
    (void)pidp_gpio_v2_close(&backend);
    return 0;
  }
  if (fake.request_open || fake.chip_open || trace.sample_count != 0
      || trace.interval_count != (fail_call == 0 ? 0 : fail_call - 1u)
      || fake.state.led_high != 0 || fake.state.col_output != 0
      || fake.state.switch_output != 0 || !safe_trace("row failure", &trace)) {
    fprintf(stderr, "row failure did not blank, idle and release\n");
    return 0;
  }
  return 1;
}

static int input_failure_case(unsigned int fail_call, int interrupted)
{
  struct pidp_fixture_inputs inputs;
  struct pidp_fixture_trace trace;
  struct pidp_gpio_v2 backend;
  struct port_fake fake;
  struct pidp_gpio_rotary rotary = {{3, 3}};
  struct failing_delay failure = {0, fail_call, &fake};
  uint32_t switches[PIDP_GPIO_V2_SWITCH_ROWS] =
      {UINT32_C(0xaaa), UINT32_C(0xbbb), UINT32_C(0xccc)};
  int knobs[2] = {1, 1};
  int index = 0;
  int expected_error = interrupted ? EINTR : ETIMEDOUT;

  memset(&inputs, 0, sizeof(inputs));
  memset(&trace, 0, sizeof(trace));
  inputs.values[0][0] = UINT32_C(0x155);
  inputs.values[1][0] = UINT32_C(0x2aa);
  memset(&backend, 0, sizeof(backend));
  if (open_port(&fake, &backend, &trace, &inputs, &index) < 0)
    return 0;
  if (pidp_gpio_scan_inputs(&backend, switches, &rotary, knobs,
      interrupted ? interrupt_delay : fail_delay, &failure) >= 0
      || errno != expected_error) {
    fprintf(stderr, "input scan did not return its failure\n");
    (void)pidp_gpio_v2_close(&backend);
    return 0;
  }
  if (fake.request_open || fake.chip_open
      || trace.sample_count != (fail_call == 1 ? 0 : 1)
      || trace.interval_count != fail_call - 1u
      || switches[0] != (fail_call == 1 ? UINT32_C(0xaaa)
          : UINT32_C(0x155))
      || switches[1] != UINT32_C(0xbbb)
      || switches[2] != UINT32_C(0xccc)
      || rotary.last_code[0] != 3 || rotary.last_code[1] != 3
      || knobs[0] != 1 || knobs[1] != 1
      || fake.state.led_high != 0 || fake.state.col_output != 0
      || fake.state.switch_output != 0 || !safe_trace("input failure", &trace)) {
    fprintf(stderr, "input failure did not idle and release\n");
    return 0;
  }
  return 1;
}

static int single_failure_case(unsigned int fail_call, int interrupted)
{
  struct pidp_fixture_inputs inputs;
  struct pidp_fixture_trace trace;
  struct pidp_gpio_v2 backend;
  struct port_fake fake;
  struct failing_delay failure = {0, fail_call, &fake};
  uint32_t rows[PIDP_GPIO_V2_LED_ROWS] = {1, 2, 4, 8, 16, 32};
  int index = 0;
  int expected_error = fail_call == 0 ? EINVAL : interrupted ? EINTR : ETIMEDOUT;

  memset(&inputs, 0, sizeof(inputs));
  memset(&trace, 0, sizeof(trace));
  memset(&backend, 0, sizeof(backend));
  if (open_port(&fake, &backend, &trace, &inputs, &index) < 0)
    return 0;
  if (fail_call == 0)
    rows[PIDP_GPIO_V2_LED_ROWS - 1] = PIDP_GPIO_V2_LED_MASK + 1u;
  if (pidp_gpio_scan_single(&backend, rows,
      interrupted ? interrupt_delay : fail_delay, &failure) >= 0
      || errno != expected_error) {
    fprintf(stderr, "single-cell scan did not return its failure\n");
    (void)pidp_gpio_v2_close(&backend);
    return 0;
  }
  if (fake.request_open || fake.chip_open || trace.sample_count != 0
      || trace.interval_count != (fail_call == 0 ? 0 : fail_call - 1u)
      || fake.state.led_high != 0 || fake.state.col_output != 0
      || fake.state.switch_output != 0 || !safe_trace("single failure", &trace)) {
    fprintf(stderr, "single-cell failure did not blank, idle and release\n");
    return 0;
  }
  return 1;
}

static int timing_failure_case(unsigned int fail_call)
{
  struct pidp_fixture_inputs inputs;
  struct pidp_fixture_trace trace;
  volatile uint32_t patterns[2][GPIOPATTERN_LED_BRIGHTNESS_PHASES][8];
  struct pidp_gpio_v2 backend;
  struct port_fake fake;
  struct pidp_gpio_v2_mapping mapping;
  struct failing_delay failure = {0, fail_call, &fake};
  struct pidp_gpio_rotary rotary = {{3, 3}};
  int index = 0;
  int knobs[2] = {1, 1};
  volatile uint32_t switches[3] = {0};

  memset(&inputs, 0, sizeof(inputs));
  memset((void *)patterns, 0, sizeof(patterns));
  pidp_fixture_trace_reset(&trace);
  memset(&fake, 0, sizeof(fake));
  fake.trace = &trace;
  fake.inputs = &inputs;
  fake.read_index = &index;
  memset(&backend, 0, sizeof(backend));
  backend.request_fd = -1;
  port_mapping(&mapping);
  if (pidp_gpio_v2_open(&backend, &mapping, &port_ops, &fake) < 0)
    return 0;
  if (pidp_gpio_scan_cycle(&backend, patterns, &index, switches, &rotary,
      knobs, fail_delay, &failure) >= 0 || errno != ETIMEDOUT) {
    fprintf(stderr, "timing failure was not returned\n");
    (void)pidp_gpio_v2_close(&backend);
    return 0;
  }
  if (fake.request_open || fake.chip_open
      || trace.interval_count != fail_call - 1u
      || fake.state.led_high != 0 || fake.state.col_output != 0
      || fake.state.switch_output != 0 || !safe_trace("timing failure", &trace)) {
    fprintf(stderr, "timing failure did not safely idle and close fake transport\n");
    return 0;
  }
  return 1;
}

int main(void)
{
  unsigned int mode;
  const uint32_t row_patterns[PIDP_GPIO_V2_LED_ROWS] =
      {0, FIXTURE_ALL_COLS, UINT32_C(0xa55), UINT32_C(0x155),
       UINT32_C(0x2aa), UINT32_C(0x555)};

  for (mode = 0; mode < 3; ++mode) {
    if (!run_pattern_case(mode, 1, 1))
      return 1;
  }
  if (!run_pattern_case(2, 7, 0))
    return 1;
  if (!single_pattern_case("single-dark", 0)
      || !single_pattern_case("single-dense", FIXTURE_ALL_COLS)
      || !single_pattern_case("single-mixed", UINT32_C(0xa55))
      || !row_pattern_case("rows-mixed", row_patterns)
      || !input_row_case() || !input_row_encoder_case()
      || !input_row_invalid_case() || !input_scan_case())
    return 1;
  if (!single_failure_case(0, 0) || !single_failure_case(1, 0)
      || !single_failure_case(2, 0) || !single_failure_case(144, 0)
      || !single_failure_case(1, 1) || !single_failure_case(2, 1)
      || !row_failure_case(0, 0) || !row_failure_case(1, 0)
      || !row_failure_case(2, 0) || !row_failure_case(12, 0)
      || !row_failure_case(1, 1) || !row_failure_case(2, 1)
      || !input_failure_case(1, 0) || !input_failure_case(2, 1))
    return 1;
  if (!timing_failure_case(1) || !timing_failure_case(2)
      || !timing_failure_case(13))
    return 1;
  puts("gpio waveform fixture passed");
  return 0;
}
