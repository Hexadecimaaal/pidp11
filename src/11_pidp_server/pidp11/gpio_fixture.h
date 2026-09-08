#ifndef PIDP_GPIO_FIXTURE_H
#define PIDP_GPIO_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

#include "gpio_v2.h"
#include "gpiopattern.h"

/* compare requested dwell intervals and stable inputs, not real-time latency,
 * electrical behavior or simultaneous edges within a GPIO-v2 ioctl.
 * zero-duration state transitions are audited separately for unsafe overlap.
 */
#define PIDP_FIXTURE_MAX_INTERVALS 512u
#define PIDP_FIXTURE_MAX_STATES 8192u
#define PIDP_FIXTURE_MAX_SAMPLES 128u

struct pidp_fixture_state {
  uint16_t led_output;
  uint16_t led_high;
  uint16_t col_output;
  uint16_t col_high;
  uint8_t switch_output;
  uint8_t switch_high;
};

struct pidp_fixture_snapshot {
  uint16_t led_output;
  uint16_t led_high;
  uint16_t col_output;
  uint16_t col_high;
  uint8_t switch_output;
  uint8_t switch_high;
};

struct pidp_fixture_interval {
  uint64_t duration_ns;
  struct pidp_fixture_snapshot state;
};

struct pidp_fixture_sample {
  uint64_t at_ns;
  unsigned int row;
  uint32_t physical_bits;
};

struct pidp_fixture_trace {
  uint64_t now_ns;
  size_t interval_count;
  size_t state_count;
  size_t sample_count;
  int overflow;
  int has_initial;
  int has_final;
  struct pidp_fixture_snapshot initial_state;
  struct pidp_fixture_snapshot final_state;
  struct pidp_fixture_interval intervals[PIDP_FIXTURE_MAX_INTERVALS];
  struct pidp_fixture_snapshot states[PIDP_FIXTURE_MAX_STATES];
  struct pidp_fixture_sample samples[PIDP_FIXTURE_MAX_SAMPLES];
};

struct pidp_fixture_inputs {
  uint32_t values[PIDP_GPIO_V2_SWITCH_ROWS]
      [GPIOPATTERN_LED_BRIGHTNESS_PHASES];
};

void pidp_fixture_record_initial(struct pidp_fixture_trace *trace,
    const struct pidp_fixture_state *state);
void pidp_fixture_record_final(struct pidp_fixture_trace *trace,
    const struct pidp_fixture_state *state);

void pidp_fixture_trace_reset(struct pidp_fixture_trace *trace);
void pidp_fixture_record_state(struct pidp_fixture_trace *trace,
    const struct pidp_fixture_state *state);
void pidp_fixture_record_delay(struct pidp_fixture_trace *trace,
    const struct pidp_fixture_state *state, uint64_t duration_ns);
void pidp_fixture_record_sample(struct pidp_fixture_trace *trace,
    unsigned int row, uint32_t physical_bits);

int pidp_fixture_run_original(struct pidp_fixture_trace *trace,
    const struct pidp_fixture_inputs *inputs,
    volatile uint32_t patterns[2][GPIOPATTERN_LED_BRIGHTNESS_PHASES][8],
    int read_index, int knobs[2], uint32_t switches[3]);

int pidp_fixture_run_port(struct pidp_fixture_trace *trace,
    const struct pidp_fixture_inputs *inputs,
    volatile uint32_t patterns[2][GPIOPATTERN_LED_BRIGHTNESS_PHASES][8],
    int read_index, int knobs[2], uint32_t switches[3]);

#endif
