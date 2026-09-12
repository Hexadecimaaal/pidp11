#ifndef PIDP_PANEL_ACTIONS_H
#define PIDP_PANEL_ACTIONS_H

#include <stdint.h>

#define PANEL_ACTION_LOAD_ADRS UINT8_C(0x02)
#define PANEL_ACTION_EXAM UINT8_C(0x04)
#define PANEL_ACTION_DEPOSIT UINT8_C(0x08)
#define PANEL_ACTION_CONT UINT8_C(0x10)
#define PANEL_ACTION_HALT UINT8_C(0x20)
#define PANEL_ACTION_S_BUS_CYCLE UINT8_C(0x40)
#define PANEL_ACTION_START UINT8_C(0x80)
#define PANEL_ACTION_MOMENTARY_MASK \
  (PANEL_ACTION_LOAD_ADRS | PANEL_ACTION_EXAM | PANEL_ACTION_DEPOSIT \
    | PANEL_ACTION_CONT | PANEL_ACTION_START)
#define PANEL_ACTION_MASK UINT8_C(0xfe)
#define PANEL_ACTION_DEBOUNCE_NS UINT64_C(20000000)

struct panel_actions {
  uint64_t candidate_since[8];
  uint8_t candidate;
  uint8_t stable;
  uint8_t startup_blocked;
  int initialized;
};

/* row2 lower eight bits are physical active-low inputs, not rotary positions. */
static inline void panel_actions_update(struct panel_actions *state,
    uint32_t row2, uint64_t now_ns)
{
  uint8_t pressed = (uint8_t)~row2 & PANEL_ACTION_MASK;
  unsigned int i;

  if (!state->initialized) {
    state->candidate = pressed;
    state->stable = pressed;
    /* momentary controls held at startup must first be released. */
    state->startup_blocked = pressed & PANEL_ACTION_MOMENTARY_MASK;
    for (i = 0; i < 8; ++i)
      state->candidate_since[i] = now_ns;
    state->initialized = 1;
    return;
  }

  for (i = 0; i < 8; ++i) {
    uint8_t mask = (uint8_t)(1u << i);

    if ((pressed ^ state->candidate) & mask) {
      state->candidate ^= mask;
      state->candidate_since[i] = now_ns;
    } else if (((state->stable ^ state->candidate) & mask)
        && now_ns - state->candidate_since[i] >= PANEL_ACTION_DEBOUNCE_NS) {
      state->stable ^= mask;
      if (!(state->stable & mask))
        state->startup_blocked &= (uint8_t)~mask;
    }
  }
}

/* demo mode without -F never publishes actions, even with physical inputs. */
static inline uint8_t panel_actions_value(const struct panel_actions *state,
    int front_panel)
{
  return front_panel
      ? state->stable & (uint8_t)~state->startup_blocked & PANEL_ACTION_MASK
      : 0;
}

#endif
