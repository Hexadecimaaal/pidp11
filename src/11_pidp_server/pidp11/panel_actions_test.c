#include "panel_actions.h"

#define main pidp11_fixture_server_main
#include "main.c"
#undef main

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "historybuffer.h"

/* the fixture owns the registry normally supplied by the RPC server. */
blinkenlight_panel_list_t *blinkenlight_panel_list;

static uint32_t released(void)
{
  return UINT32_C(0xfe);
}

static uint32_t with_pressed(uint8_t actions)
{
  return (released() & (uint32_t)~actions)
      | (actions & PANEL_INPUT_LAMPTEST);
}

static void test_press_release_and_bounce(void)
{
  struct panel_actions state = {0};

  panel_actions_update(&state, released(), 0);
  panel_actions_update(&state, with_pressed(PANEL_ACTION_LOAD_ADRS), 1);
  panel_actions_update(&state, released(), UINT64_C(10000001));
  panel_actions_update(&state, with_pressed(PANEL_ACTION_LOAD_ADRS),
      UINT64_C(15000001));
  assert(panel_actions_value(&state, 1) == 0);
  panel_actions_update(&state, with_pressed(PANEL_ACTION_LOAD_ADRS),
      UINT64_C(35000001));
  assert(panel_actions_value(&state, 1) == PANEL_ACTION_LOAD_ADRS);

  panel_actions_update(&state, released(), UINT64_C(35000002));
  panel_actions_update(&state, released(), UINT64_C(55000002));
  assert(panel_actions_value(&state, 1) == 0);
}

static void test_independent_debounce_timers(void)
{
  struct panel_actions state = {0};

  panel_actions_update(&state, released(), 0);
  panel_actions_update(&state, with_pressed(PANEL_ACTION_LOAD_ADRS), 1);
  panel_actions_update(&state,
      with_pressed(PANEL_ACTION_LOAD_ADRS | PANEL_ACTION_EXAM),
      UINT64_C(10000001));
  panel_actions_update(&state,
      with_pressed(PANEL_ACTION_LOAD_ADRS | PANEL_ACTION_EXAM),
      UINT64_C(20000001));
  assert(panel_actions_value(&state, 1) == PANEL_ACTION_LOAD_ADRS);
  panel_actions_update(&state,
      with_pressed(PANEL_ACTION_LOAD_ADRS | PANEL_ACTION_EXAM),
      UINT64_C(30000001));
  assert(panel_actions_value(&state, 1)
      == (PANEL_ACTION_LOAD_ADRS | PANEL_ACTION_EXAM));
}

static void test_startup_levels_and_momentary_suppression(void)
{
  struct panel_actions state = {0};
  uint8_t levels = PANEL_ACTION_MOMENTARY_MASK | PANEL_ACTION_HALT
      | PANEL_ACTION_S_BUS_CYCLE;

  panel_actions_update(&state, with_pressed(levels), 0);
  assert(panel_actions_value(&state, 1)
      == (PANEL_ACTION_HALT | PANEL_ACTION_S_BUS_CYCLE));
  panel_actions_update(&state, with_pressed(levels),
      PANEL_ACTION_DEBOUNCE_NS);
  assert(panel_actions_value(&state, 1)
      == (PANEL_ACTION_HALT | PANEL_ACTION_S_BUS_CYCLE));

  panel_actions_update(&state, with_pressed(PANEL_ACTION_HALT
      | PANEL_ACTION_S_BUS_CYCLE), UINT64_C(20000001));
  panel_actions_update(&state, with_pressed(PANEL_ACTION_HALT
      | PANEL_ACTION_S_BUS_CYCLE), UINT64_C(40000001));
  assert(panel_actions_value(&state, 1)
      == (PANEL_ACTION_HALT | PANEL_ACTION_S_BUS_CYCLE));

  panel_actions_update(&state, released(), UINT64_C(40000002));
  panel_actions_update(&state, released(), UINT64_C(60000002));
  assert(panel_actions_value(&state, 1) == 0);

  panel_actions_update(&state, with_pressed(PANEL_ACTION_LOAD_ADRS),
      UINT64_C(60000003));
  panel_actions_update(&state, with_pressed(PANEL_ACTION_LOAD_ADRS),
      UINT64_C(80000003));
  assert(panel_actions_value(&state, 1) == PANEL_ACTION_LOAD_ADRS);
}

static void test_idled_mode_cannot_publish_actions(void)
{
  struct panel_actions state = {0};

  panel_actions_update(&state,
      with_pressed(PANEL_ACTION_CONT | PANEL_ACTION_HALT), 0);
  assert(panel_actions_value(&state, 0) == 0);
}

static void test_lamp_test_level(void)
{
  struct panel_actions state = {0};

  panel_actions_update(&state, released(), 0);
  panel_actions_update(&state, with_pressed(PANEL_INPUT_LAMPTEST), 1);
  assert(panel_actions_lamptest(&state, 1) == 0);
  panel_actions_update(&state, with_pressed(PANEL_INPUT_LAMPTEST),
      PANEL_ACTION_DEBOUNCE_NS + 1);
  assert(panel_actions_value(&state, 1) == 0);
  assert(panel_actions_lamptest(&state, 1) != 0);
  panel_actions_update(&state, with_pressed(PANEL_INPUT_LAMPTEST),
      UINT64_C(40000002));
  assert(panel_actions_lamptest(&state, 1) != 0);

  panel_actions_update(&state, released(), UINT64_C(40000003));
  assert(panel_actions_lamptest(&state, 1) != 0);
  panel_actions_update(&state, released(), UINT64_C(60000003));
  assert(panel_actions_lamptest(&state, 1) == 0);
  assert(panel_actions_value(&state, 1) == 0);

  memset(&state, 0, sizeof(state));
  panel_actions_update(&state, with_pressed(PANEL_INPUT_LAMPTEST), 0);
  assert(panel_actions_lamptest(&state, 1) != 0);
  assert(panel_actions_lamptest(&state, 0) == 0);
}

static void destroy_test_panel(void)
{
  unsigned int i;
  blinkenlight_panel_t *panel;

  if (blinkenlight_panel_list == NULL)
    return;
  panel = &blinkenlight_panel_list->panels[0];
  for (i = 0; i < panel->controls_count; ++i) {
    if (panel->controls[i].history != NULL) {
      historybuffer_destroy(panel->controls[i].history);
      panel->controls[i].history = NULL;
    }
  }
  free(blinkenlight_panel_list);
  blinkenlight_panel_list = NULL;
}

static void set_simulator_values(int changed)
{
  unsigned int i;
  blinkenlight_panel_t *panel = &blinkenlight_panel_list->panels[0];

  for (i = 0; i < panel->controls_count; ++i) {
    if (!panel->controls[i].is_input)
      panel->controls[i].value = 0;
  }
  if (!changed) {
    leds_ADDRESS->value = UINT64_C(1);
    leds_DATA->value = UINT64_C(1) << 12;
    led_PARITY_HIGH->value = 1;
    led_RUN->value = 1;
    knobValue[0] = 1;
    knobValue[1] = 1;
  } else {
    leds_ADDRESS->value = UINT64_C(1) << 21;
    leds_DATA->value = 1;
    led_PARITY_LOW->value = 1;
    led_PAUSE->value = 1;
    knobValue[0] = 7;
    knobValue[1] = 0;
  }
}

static void test_lamp_snapshot(void)
{
  static const uint32_t expected_lamp_rows[8] = {
    UINT32_C(0x0fff), UINT32_C(0x03ff), UINT32_C(0x0fff),
    UINT32_C(0x0fff), UINT32_C(0x0fff), UINT32_C(0x0fc0), 0, 0
  };
  uint32_t normal_before[8];
  uint32_t lamp_asserted[8];
  uint32_t lamp_held[8];
  uint32_t normal_after[8];
  uint32_t released[8];
  blinkenlight_panel_t *panel;
  unsigned int i;

  register_controls();
  panel = &blinkenlight_panel_list->panels[0];
  set_simulator_values(0);
  gpiopattern_demo_snapshot(panel, normal_before, 0);
  gpiopattern_demo_snapshot(panel, lamp_asserted, 1);
  for (i = 0; i < 8; ++i)
    assert(lamp_asserted[i] == expected_lamp_rows[i]);

  set_simulator_values(1);
  gpiopattern_demo_snapshot(panel, normal_after, 0);
  gpiopattern_demo_snapshot(panel, lamp_held, 1);
  for (i = 0; i < 8; ++i)
    assert(lamp_held[i] == expected_lamp_rows[i]);

  gpiopattern_demo_snapshot(panel, released, 0);
  assert(memcmp(normal_before, normal_after, sizeof(normal_before)) != 0);
  assert(memcmp(released, normal_after, sizeof(released)) == 0);
  destroy_test_panel();
}


int main(void)
{
  test_press_release_and_bounce();
  test_independent_debounce_timers();
  test_startup_levels_and_momentary_suppression();
  test_idled_mode_cannot_publish_actions();
  test_lamp_test_level();
  test_lamp_snapshot();
  return 0;
}
