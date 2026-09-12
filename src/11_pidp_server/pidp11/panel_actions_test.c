#include "panel_actions.h"

#include <assert.h>
#include <stdint.h>

static uint32_t released(void)
{
  return UINT32_C(0xff);
}

static uint32_t with_pressed(uint8_t actions)
{
  return UINT32_C(0xff) & (uint32_t)~actions;
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

int main(void)
{
  test_press_release_and_bounce();
  test_independent_debounce_timers();
  test_startup_levels_and_momentary_suppression();
  test_idled_mode_cannot_publish_actions();
  return 0;
}
