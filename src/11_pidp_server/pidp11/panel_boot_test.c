#include "panel_boot.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static uint32_t released_row(void)
{
  return PANEL_BOOT_SWITCH_BIT;
}

static uint32_t pressed_row(void)
{
  return 0;
}

static void test_startup_hold_is_suppressed(void)
{
  struct panel_boot state = {0};
  uint32_t selection = UINT32_C(0xffffffff);

  assert(panel_boot_update(&state, pressed_row(), 01001, 0, &selection) == 0);
  assert(panel_boot_update(&state, pressed_row(), 01001,
      PANEL_BOOT_DEBOUNCE_NS, &selection) == 0);
  assert(selection == UINT32_C(0xffffffff));

  assert(panel_boot_update(&state, released_row(), 01001,
      PANEL_BOOT_DEBOUNCE_NS + 1, &selection) == 0);
  assert(panel_boot_update(&state, released_row(), 01001,
      PANEL_BOOT_DEBOUNCE_NS * 2 + 1, &selection) == 0);

  assert(panel_boot_update(&state, pressed_row(), 0,
      PANEL_BOOT_DEBOUNCE_NS * 2 + 2, &selection) == 0);
  assert(panel_boot_update(&state, pressed_row(), 0,
      PANEL_BOOT_DEBOUNCE_NS * 3 + 3, &selection) == 1);
  assert(selection == 0);
  assert(panel_boot_update(&state, pressed_row(), 01001,
      PANEL_BOOT_DEBOUNCE_NS * 4 + 3, &selection) == 0);
}

static void test_bounce_and_one_event_per_press(void)
{
  struct panel_boot state = {0};
  uint32_t selection = UINT32_C(0xffffffff);

  assert(panel_boot_update(&state, released_row(), 0, 0, &selection) == 0);
  assert(panel_boot_update(&state, pressed_row(), 100, 1, &selection) == 0);
  assert(panel_boot_update(&state, released_row(), 100,
      UINT64_C(10000001), &selection) == 0);
  assert(panel_boot_update(&state, pressed_row(), 01001,
      UINT64_C(15000001), &selection) == 0);
  assert(panel_boot_update(&state, pressed_row(), 01001,
      UINT64_C(35000000), &selection) == 0);
  assert(panel_boot_update(&state, pressed_row(), 01001,
      UINT64_C(35000002), &selection) == 1);
  assert(selection == 01001);
  assert(panel_boot_update(&state, pressed_row(), 0,
      UINT64_C(55000002), &selection) == 0);

  assert(panel_boot_update(&state, released_row(), 0,
      UINT64_C(55000003), &selection) == 0);
  assert(panel_boot_update(&state, released_row(), 0,
      UINT64_C(75000003), &selection) == 0);
  assert(panel_boot_update(&state, pressed_row(), 0,
      UINT64_C(75000004), &selection) == 0);
  assert(panel_boot_update(&state, pressed_row(), 0,
      UINT64_C(95000005), &selection) == 1);
  assert(selection == 0);
}

static void test_selection_validation_and_snapshot(void)
{
  struct panel_boot state = {0};
  uint32_t selection = UINT32_C(0xffffffff);
  uint32_t full_sr = UINT32_C(0123456) | UINT32_C(07000000);

  assert(panel_boot_valid_selection(0));
  assert(panel_boot_valid_selection(01001));
  assert(!panel_boot_valid_selection(1));
  assert(!panel_boot_valid_selection(0777777));

  assert(panel_boot_update(&state, released_row(), 0, 0, &selection) == 0);
  assert(panel_boot_update(&state, pressed_row(), full_sr, 1,
      &selection) == 0);
  assert(panel_boot_update(&state, pressed_row(), full_sr,
      PANEL_BOOT_DEBOUNCE_NS + 1, &selection) == 1);
  assert(selection == UINT32_C(0123456));
}

static void read_exact_file(const char *path, const char *expected)
{
  char actual[sizeof("1001\n")];
  int fd;
  struct stat status;
  ssize_t count;

  assert(stat(path, &status) == 0);
  assert((status.st_mode & 0777) == 0600);
  fd = open(path, O_RDONLY | O_CLOEXEC);
  assert(fd >= 0);
  count = read(fd, actual, sizeof(actual));
  assert(count == 5);
  assert(memcmp(actual, expected, 5) == 0);
  assert(read(fd, actual, sizeof(actual)) == 0);
  assert(close(fd) == 0);
}

static void test_safe_publication(void)
{
  char directory_template[] = "/tmp/pidp-panel-boot.XXXXXX";
  char request_path[256];
  char invalid_path[256];
  char target_path[256];
  char symlink_path[256];
  char command_like_path[256];
  char marker_path[256];
  char *directory;
  int fd;
  char target_contents[8] = {0};

  directory = mkdtemp(directory_template);
  assert(directory != NULL);
  assert(snprintf(request_path, sizeof(request_path), "%s/request", directory)
      > 0);
  assert(snprintf(invalid_path, sizeof(invalid_path), "%s/invalid", directory)
      > 0);
  assert(snprintf(target_path, sizeof(target_path), "%s/target", directory)
      > 0);
  assert(snprintf(symlink_path, sizeof(symlink_path), "%s/link", directory)
      > 0);
  assert(snprintf(command_like_path, sizeof(command_like_path),
      "%s/request;touch-marker", directory) > 0);
  assert(snprintf(marker_path, sizeof(marker_path), "%s/touch-marker", directory)
      > 0);

  assert(panel_boot_publish(request_path, 0) == 0);
  read_exact_file(request_path, "0000\n");
  errno = 0;
  assert(panel_boot_publish(request_path, 01001) < 0);
  assert(errno == EEXIST);

  errno = 0;
  assert(panel_boot_publish(invalid_path, 1) < 0);
  assert(errno == EINVAL);
  assert(access(invalid_path, F_OK) < 0 && errno == ENOENT);

  fd = open(target_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  assert(fd >= 0);
  assert(write(fd, "keep", 4) == 4);
  assert(close(fd) == 0);
  assert(symlink(target_path, symlink_path) == 0);
  errno = 0;
  assert(panel_boot_publish(symlink_path, 0) < 0);
  assert(errno == EEXIST || errno == ELOOP);
  fd = open(target_path, O_RDONLY | O_CLOEXEC);
  assert(fd >= 0);
  assert(read(fd, target_contents, 4) == 4);
  assert(memcmp(target_contents, "keep", 4) == 0);
  assert(close(fd) == 0);

  assert(panel_boot_publish(command_like_path, 01001) == 0);
  read_exact_file(command_like_path, "1001\n");
  assert(access(marker_path, F_OK) < 0 && errno == ENOENT);

  assert(unlink(request_path) == 0);
  assert(unlink(command_like_path) == 0);
  assert(unlink(symlink_path) == 0);
  assert(unlink(target_path) == 0);
  assert(rmdir(directory) == 0);
}

int main(void)
{
  test_startup_hold_is_suppressed();
  test_bounce_and_one_event_per_press();
  test_selection_validation_and_snapshot();
  test_safe_publication();
  return 0;
}
