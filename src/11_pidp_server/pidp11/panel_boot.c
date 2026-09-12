#include "panel_boot.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

static int elapsed(uint64_t now_ns, uint64_t since_ns)
{
  return now_ns >= since_ns
      && now_ns - since_ns >= PANEL_BOOT_DEBOUNCE_NS;
}

int panel_boot_update(struct panel_boot *state, uint32_t row1,
    uint32_t stable_sr, uint64_t now_ns, uint32_t *selection)
{
  int pressed;

  if (state == NULL || selection == NULL) {
    errno = EINVAL;
    return -1;
  }

  pressed = (row1 & PANEL_BOOT_SWITCH_BIT) == 0;
  if (!state->initialized) {
    state->candidate_pressed = pressed;
    state->stable_pressed = pressed;
    state->startup_blocked = pressed;
    state->candidate_since_ns = now_ns;
    state->initialized = 1;
    return 0;
  }

  if (state->candidate_pressed != pressed) {
    state->candidate_pressed = pressed;
    state->candidate_since_ns = now_ns;
  } else if (state->stable_pressed != state->candidate_pressed
      && elapsed(now_ns, state->candidate_since_ns)) {
    state->stable_pressed = state->candidate_pressed;
    if (!state->stable_pressed) {
      state->startup_blocked = 0;
    } else if (!state->startup_blocked) {
      *selection = stable_sr & PANEL_BOOT_SR_MASK;
      return 1;
    }
  }

  return 0;
}

int panel_boot_valid_selection(uint32_t selection)
{
  return selection == PANEL_BOOT_PANEL_SELECTION
      || selection == PANEL_BOOT_IDLED_SELECTION;
}

static int write_all(int fd, const char *text, size_t length)
{
  size_t written = 0;

  while (written < length) {
    ssize_t result = write(fd, text + written, length - written);

    if (result > 0) {
      written += (size_t)result;
      continue;
    }
    if (result < 0 && errno == EINTR)
      continue;
    if (result == 0)
      errno = EIO;
    return -1;
  }
  return 0;
}

int panel_boot_publish(const char *path, uint32_t selection)
{
  static const char panel_text[] = "0000\n";
  static const char idled_text[] = "1001\n";
  const char *text;
  size_t length;
  int fd;
  int error;

  if (path == NULL || path[0] == '\0' || !panel_boot_valid_selection(selection)) {
    errno = EINVAL;
    return -1;
  }
  if (selection == PANEL_BOOT_PANEL_SELECTION) {
    text = panel_text;
    length = sizeof(panel_text) - 1;
  } else {
    text = idled_text;
    length = sizeof(idled_text) - 1;
  }

  fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
      0600);
  if (fd < 0)
    return -1;
  if (write_all(fd, text, length) < 0) {
    error = errno;
    (void)close(fd);
    (void)unlink(path);
    errno = error;
    return -1;
  }
  if (close(fd) < 0) {
    error = errno;
    (void)unlink(path);
    errno = error;
    return -1;
  }
  return 0;
}
