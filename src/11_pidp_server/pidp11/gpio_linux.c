#include "gpio_linux.h"

#include "gpio_scan.h"
#include "gpiopattern.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern int knobValue[2];

static struct pidp_gpio_v2 gpio_backend;
static struct pidp_gpio_rotary gpio_rotary = {{3, 3}};
static int gpio_backend_open;

static void report_gpio_error(const char *operation)
{
  int error = errno != 0 ? errno : EIO;

  fprintf(stderr, "pidp gpio-v2 %s failed: %s\n", operation,
      strerror(error));
  errno = error;
}

int pidp_gpio_linux_init(void)
{
  struct pidp_gpio_v2_mapping mapping;
  const char *chip_path;
  const char *offsets;

  if (gpio_backend_open)
    return 0;
  chip_path = getenv("PIDP_GPIO_CHIP");
  offsets = getenv("PIDP_GPIO_OFFSETS");
  if (chip_path == NULL || offsets == NULL) {
    errno = EINVAL;
    report_gpio_error("mapping is missing (set PIDP_GPIO_CHIP and "
        "PIDP_GPIO_OFFSETS)");
    return -1;
  }
  if (pidp_gpio_v2_parse_mapping(&mapping, chip_path, offsets) < 0) {
    report_gpio_error("mapping is malformed");
    return -1;
  }
  memset(&gpio_backend, 0, sizeof(gpio_backend));
  if (pidp_gpio_v2_open(&gpio_backend, &mapping, NULL, NULL) < 0) {
    report_gpio_error("open");
    return -1;
  }
  gpio_rotary.last_code[0] = 3;
  gpio_rotary.last_code[1] = 3;
  gpio_backend_open = 1;
  return 0;
}

int pidp_gpio_linux_demo_init(void)
{
  static const char expected_label[] = "13040000.pinctrl";
  struct pidp_gpio_v2_mapping mapping;
  struct gpiochip_info chip = {0};
  const char *path = getenv("PIDP_GPIO_CHIP");
  const char *offsets = getenv("PIDP_GPIO_OFFSETS");
  int fd;
  int error = 0;
  unsigned int i;

  if (path == NULL || offsets == NULL) {
    errno = EINVAL;
    report_gpio_error("demo mapping is missing");
    return -1;
  }
  if (pidp_gpio_v2_parse_mapping(&mapping, path, offsets) < 0) {
    report_gpio_error("demo mapping is malformed");
    return -1;
  }
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    report_gpio_error("demo chip preflight");
    return -1;
  }
  if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &chip) < 0) {
    error = errno;
    goto out;
  }
  if (chip.lines != 64
      || memcmp(chip.label, expected_label, sizeof(expected_label)) != 0) {
    error = ENODEV;
    goto out;
  }
  for (i = 0; i < PIDP_GPIO_V2_LINES; ++i) {
    struct gpio_v2_line_info line = {0};

    if (mapping.offsets[i] >= chip.lines) {
      error = EINVAL;
      goto out;
    }
    line.offset = mapping.offsets[i];
    if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &line) < 0) {
      error = errno;
      goto out;
    }
    if (line.flags & GPIO_V2_LINE_FLAG_USED) {
      fprintf(stderr, "pidp demo GPIO offset %u is already owned\n", line.offset);
      error = EBUSY;
      goto out;
    }
  }
out:
  if (close(fd) < 0 && error == 0)
    error = errno;
  if (error != 0) {
    errno = error;
    report_gpio_error("demo chip/ownership preflight");
    return -1;
  }
  return pidp_gpio_linux_init();
}

int pidp_gpio_linux_shutdown(void)
{
  int result;
  int error;

  if (!gpio_backend_open)
    return 0;
  result = pidp_gpio_v2_close(&gpio_backend);
  error = errno;
  gpio_backend_open = 0;
  if (result < 0) {
    errno = error != 0 ? error : EIO;
    report_gpio_error("close");
    return -1;
  }
  return 0;
}

int pidp_gpio_linux_demo_frame(
    const uint32_t rows[PIDP_GPIO_V2_LED_ROWS])
{
  if (!gpio_backend_open) {
    errno = EBADF;
    report_gpio_error("demo frame before initialization");
    return -1;
  }
  if (pidp_gpio_scan_single(&gpio_backend, rows, NULL, NULL) < 0) {
    gpio_backend_open = 0;
    if (errno != EINTR)
      report_gpio_error("demo scan");
    return -1;
  }
  return 0;
}

void *blink(void *argument)
{
  int *terminate = argument;

  if (terminate == NULL) {
    errno = EINVAL;
    report_gpio_error("scan argument");
    (void)pidp_gpio_linux_shutdown();
    return (void *)-1;
  }
  if (!gpio_backend_open) {
    errno = EBADF;
    report_gpio_error("scan before initialization");
    return (void *)-1;
  }

  while (*terminate == 0) {
    if (pidp_gpio_scan_cycle(&gpio_backend,
        gpiopattern_ledstatus_phases,
        &gpiopattern_ledstatus_phases_readidx, gpio_switchstatus,
        &gpio_rotary, knobValue, NULL, NULL) < 0) {
      report_gpio_error("scan");
      *terminate = 1;
      (void)pidp_gpio_linux_shutdown();
      exit(EXIT_FAILURE);
    }
  }

  if (pidp_gpio_linux_shutdown() < 0)
    return (void *)-1;
  return NULL;
}
