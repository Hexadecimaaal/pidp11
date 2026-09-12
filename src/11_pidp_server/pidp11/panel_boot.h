#ifndef PIDP_PANEL_BOOT_H
#define PIDP_PANEL_BOOT_H

#include <stdint.h>

#define PANEL_BOOT_SWITCH_BIT (UINT32_C(1) << 10)
#define PANEL_BOOT_SR_MASK UINT32_C(0777777)
#define PANEL_BOOT_DEBOUNCE_NS UINT64_C(20000000)
#define PANEL_BOOT_PANEL_SELECTION UINT32_C(000000)
#define PANEL_BOOT_IDLED_SELECTION UINT32_C(01001)

struct panel_boot {
  uint64_t candidate_since_ns;
  int candidate_pressed;
  int stable_pressed;
  int startup_blocked;
  int initialized;
};

/* update the active-low ADDRESS push switch using a monotonic timestamp.
 * returns 1 once for each debounced press, 0 otherwise, or -1 for invalid
 * arguments. a button held during initialization is suppressed until release.
 * stable_sr is copied into *selection only when a press is reported; only
 * its lower 18 bits are used.
 */
int panel_boot_update(struct panel_boot *state, uint32_t row1,
    uint32_t stable_sr, uint64_t now_ns, uint32_t *selection);

int panel_boot_valid_selection(uint32_t selection);

/* publish exactly the selected text into path without following or replacing
 * an existing file. returns zero on success, or -1 with errno on failure.
 */
int panel_boot_publish(const char *path, uint32_t selection);

#endif
