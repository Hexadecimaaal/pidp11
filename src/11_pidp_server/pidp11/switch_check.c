#define _POSIX_C_SOURCE 200809L
#include "gpio_v2.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/gpio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define SWITCH_MASK UINT32_C(0x0fff)
#define DEBOUNCE_NS INT64_C(20000000)
#define HEARTBEAT_NS INT64_C(5000000000)

static const char expected_chip_label[] = "13040000.pinctrl";

static const uint32_t switch_offsets[PIDP_GPIO_V2_LINES] = {
  61, 44, 47, 54, 51, 50, 60, 43, 55, 37, 39, 56,
  49, 53, 52, 48, 46, 59, 36, 42, 38
};
static volatile sig_atomic_t stop_signal;

struct switch_filter {
  uint32_t candidate[3];
  uint32_t stable[3];
  int64_t since[3][12];
  int initialized;
};

static void request_stop(int number)
{
  stop_signal = number;
}

static int install_signals(void)
{
  struct sigaction action = {0};
  action.sa_handler = request_stop;
  if (sigemptyset(&action.sa_mask) < 0
      || sigaction(SIGINT, &action, NULL) < 0
      || sigaction(SIGTERM, &action, NULL) < 0
      || sigaction(SIGHUP, &action, NULL) < 0)
    return -1;
  /* a closed log pipe must reach normal gpio cleanup, not terminate us. */
  action.sa_handler = SIG_IGN;
  return sigaction(SIGPIPE, &action, NULL);
}

static int monotonic_ns(int64_t *result)
{
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    return -1;
  *result = (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
  return 0;
}

/* returns one on a handled signal; never restart a shutdown-interrupted sleep. */
static int pause_ns(long nanoseconds)
{
  struct timespec delay = {0, nanoseconds};
  while (!stop_signal) {
    if (nanosleep(&delay, &delay) == 0)
      return stop_signal ? 1 : 0;
    if (errno != EINTR)
      return -1;
  }
  return 1;
}

static int inspect_chip(int fd, const char *path, int verbose)
{
  struct gpiochip_info chip = {0};
  unsigned int i;
  int busy = 0;

  if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &chip) < 0)
    return -1;
  if (verbose)
    printf("chip=%s name=%.*s label=%.*s lines=%u\n", path,
        (int)sizeof(chip.name), chip.name,
        (int)sizeof(chip.label), chip.label, chip.lines);
  if (memcmp(chip.label, expected_chip_label, sizeof(expected_chip_label)) != 0
      || chip.lines != 64) {
    fprintf(stderr, "refusing chip %s: expected 13040000.pinctrl with 64 lines\n",
        path);
    errno = ENODEV;
    return -1;
  }
  for (i = 0; i < PIDP_GPIO_V2_LINES; ++i) {
    struct gpio_v2_line_info line = {0};
    const char *role = i < 6 ? "led-low" : i < 18 ? "column" : "switch-row";
    unsigned int index = i < 6 ? i : i < 18 ? i - 6 : i - 18;
    line.offset = switch_offsets[i];
    if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &line) < 0)
      return -1;
    if (verbose)
      printf("%s[%u] offset=%u name=%.*s consumer=%.*s flags=0x%" PRIx64 "\n",
          role, index, line.offset, (int)sizeof(line.name), line.name,
          (int)sizeof(line.consumer), line.consumer, (uint64_t)line.flags);
    if ((line.flags & GPIO_V2_LINE_FLAG_USED) != 0 || line.consumer[0] != '\0') {
      fprintf(stderr, "refusing %s[%u] offset=%u: owned by %.*s\n",
          role, index, line.offset, (int)sizeof(line.consumer), line.consumer);
      busy = 1;
    }
  }
  if (busy) {
    errno = EBUSY;
    return -1;
  }
  return 0;
}

/* validate on the descriptor the backend will use, before any output request. */
static int checked_open(void *context, const char *path, int flags)
{
  int fd;
  int error;
  (void)context;
  if (stop_signal) {
    errno = EINTR;
    return -1;
  }
  fd = open(path, flags);
  if (fd < 0)
    return -1;
  if (inspect_chip(fd, path, 0) < 0 || stop_signal) {
    error = stop_signal ? EINTR : errno;
    (void)close(fd);
    errno = error;
    return -1;
  }
  return fd;
}

static int monitor_ioctl(void *context, int fd, unsigned long request, void *argument)
{
  (void)context;
  return ioctl(fd, request, argument);
}

static int monitor_close(void *context, int fd)
{
  (void)context;
  return close(fd);
}

/* a partial frame is never published; every selected row is released first. */
static int scan_switch_frame_full(struct pidp_gpio_v2 *backend,
    uint32_t rows[3], long settle_ns)
{
  uint32_t sampled[3];
  unsigned int row;
  int result;
  int error;

  for (row = 0; row < PIDP_GPIO_V2_SWITCH_ROWS; ++row) {
    if (stop_signal)
      return 1;
    if (pidp_gpio_v2_select_switch(backend, row) < 0)
      return -1;
    result = pause_ns(settle_ns);
    if (result != 0) {
      error = errno;
      if (pidp_gpio_v2_idle(backend) < 0)
        return -1;
      errno = error;
      return result;
    }
    if (pidp_gpio_v2_read_switches(backend, &sampled[row]) < 0
        || pidp_gpio_v2_idle(backend) < 0)
      return -1;
  }
  if (stop_signal)
    return 1;
  memcpy(rows, sampled, sizeof(sampled));
  return 0;
}

static int scan_switch_frame_grouped(struct pidp_gpio_v2 *backend,
    uint32_t rows[3], long settle_ns, unsigned int column_batch)
{
  uint32_t sampled[3] = {0};
  unsigned int row;
  unsigned int column;
  int selected = 0;

  for (row = 0; row < PIDP_GPIO_V2_SWITCH_ROWS; ++row) {
    for (column = 0; column < PIDP_GPIO_V2_COLS;) {
      unsigned int group_columns = PIDP_GPIO_V2_COLS - column;
      uint32_t column_mask;
      uint32_t physical_bits;
      int result;
      int error;

      if (group_columns > column_batch)
        group_columns = column_batch;
      if (stop_signal) {
        if (selected && pidp_gpio_v2_idle(backend) < 0)
          return -1;
        return 1;
      }
      column_mask = ((UINT32_C(1) << group_columns) - UINT32_C(1))
          << column;
      if (pidp_gpio_v2_select_switch_columns(backend, row, column_mask) < 0)
        return -1;
      selected = 1;
      result = pause_ns(settle_ns);
      if (result != 0) {
        error = errno;
        if (pidp_gpio_v2_idle(backend) < 0)
          return -1;
        selected = 0;
        errno = error;
        return result;
      }
      if (pidp_gpio_v2_read_switches(backend, &physical_bits) < 0)
        return -1;
      sampled[row] |= physical_bits & column_mask;
      column += group_columns;
      if (column >= PIDP_GPIO_V2_COLS) {
        if (pidp_gpio_v2_idle(backend) < 0)
          return -1;
        selected = 0;
      }
    }
  }
  if (stop_signal)
    return 1;
  memcpy(rows, sampled, sizeof(sampled));
  return 0;
}

static int scan_switch_frame(struct pidp_gpio_v2 *backend, uint32_t rows[3],
    long settle_ns, unsigned int column_batch)
{
  if (column_batch == PIDP_GPIO_V2_COLS)
    return scan_switch_frame_full(backend, rows, settle_ns);
  return scan_switch_frame_grouped(backend, rows, settle_ns, column_batch);
}

/* debounce each toggle independently; encoder contacts remain raw, not positions. */
static int filter_switches(struct switch_filter *filter, const uint32_t rows[3],
    int64_t now)
{
  unsigned int row;
  unsigned int bit;
  int changed = 0;

  if (!filter->initialized) {
    memcpy(filter->candidate, rows, sizeof(filter->candidate));
    memcpy(filter->stable, rows, sizeof(filter->stable));
    for (row = 0; row < 3; ++row)
      for (bit = 0; bit < 12; ++bit)
        filter->since[row][bit] = now;
    filter->initialized = 1;
    return 1;
  }
  for (row = 0; row < 3; ++row) {
    for (bit = 0; bit < 12; ++bit) {
      uint32_t mask = UINT32_C(1) << bit;
      if (((rows[row] ^ filter->candidate[row]) & mask) != 0) {
        filter->candidate[row] ^= mask;
        filter->since[row][bit] = now;
      }
      if (((filter->stable[row] ^ filter->candidate[row]) & mask) != 0
          && ((row == 2 && bit >= 8)
            || now - filter->since[row][bit] >= DEBOUNCE_NS)) {
        filter->stable[row] ^= mask;
        changed = 1;
      }
    }
  }
  return changed;
}

static int report_switches(const char *event, int64_t elapsed, const uint32_t rows[3])
{
  static const char *const controls[8] = {
    "LAMPTEST", "LOAD_ADRS", "EXAM", "DEPOSIT", "CONT", "HALT",
    "S_BUS_CYCLE", "START"
  };
  uint32_t sr = ((~rows[0]) & SWITCH_MASK) | (((~rows[1]) & 0x3ffu) << 12);
  unsigned int bit;

  printf("%s t=%.3f raw[0..2]=%03" PRIx32 ",%03" PRIx32 ",%03" PRIx32
      " SR=0x%06" PRIx32 "(0%08" PRIo32 ")", event,
      (double)elapsed / 1e9, rows[0], rows[1], rows[2], sr, sr);
  for (bit = 0; bit < 8; ++bit)
    printf(" %s=%u", controls[bit],
        (unsigned int)(((bit == 0 ? rows[2] : ~rows[2]) >> bit) & 1u));
  printf(" POWER_RAW=%u ROW1_BIT11_RAW=%u ADDR_AB_RAW=%u%u DATA_AB_RAW=%u%u\n",
      (unsigned int)((rows[1] >> 10) & 1u), (unsigned int)((rows[1] >> 11) & 1u),
      (unsigned int)((rows[2] >> 8) & 1u), (unsigned int)((rows[2] >> 9) & 1u),
      (unsigned int)((rows[2] >> 10) & 1u), (unsigned int)((rows[2] >> 11) & 1u));
  return fflush(stdout) == EOF || ferror(stdout) ? -1 : 0;
}

static void usage(FILE *stream)
{
  fprintf(stream, "usage: pidp-switch-monitor [--chip PATH] [--seconds 1..86400] [--settle-us 1..100000] [--column-batch 1..12] [--inspect]\n"
      "  default: /dev/gpiochip0, 300 seconds, 100us settling, 12-column batch; INT/TERM/HUP stop safely\n"
      "  --inspect: read-only chip identity and all 21 line owners; no requests\n"
      "  raw rows: 12-bit physical levels (1=high); toggles debounce for 20ms\n"
      "  SR/actions are active-low; LAMPTEST is active-high; rotary AB is unfiltered\n"
      "  row0 bits0..11=SR0..11; row1 bits0..9=SR12..21, bit10=power raw\n"
      "  row2 bits0..7=LAMPTEST,LOAD_ADRS,EXAM,DEPOSIT,CONT,HALT,S_BUS_CYCLE,START\n"
      "  row2 bits8,9=ADDR A,B; bits10,11=DATA A,B; no rotary position inference\n");
}

static int parse_bounded_uint(const char *text, unsigned int maximum,
    unsigned int *number)
{
  unsigned int value = 0;
  const unsigned char *cursor = (const unsigned char *)text;
  if (*cursor == '\0')
    return -1;
  for (; *cursor; ++cursor) {
    if (*cursor < '0' || *cursor > '9' || value > maximum / 10)
      return -1;
    value = value * 10 + (*cursor - '0');
  }
  if (value == 0 || value > maximum)
    return -1;
  *number = value;
  return 0;
}

int main(int argc, char **argv)
{
  struct pidp_gpio_v2 backend = {0};
  struct pidp_gpio_v2_mapping mapping = {.chip_path = "/dev/gpiochip0"};
  const struct pidp_gpio_v2_ops ops = {checked_open, monitor_ioctl, monitor_close};
  struct switch_filter filter = {0};
  uint32_t rows[3];
  uint32_t previous_raw[3];
  uint32_t raw_and[3] = {SWITCH_MASK, SWITCH_MASK, SWITCH_MASK};
  uint32_t raw_or[3] = {0};
  unsigned int seconds = 300;
  unsigned int settle_us = 100;
  unsigned int column_batch = PIDP_GPIO_V2_COLS;
  long settle_ns;
  int inspect = 0;
  int result = 0;
  int have_previous_raw = 0;
  int i;
  uint64_t completed_frames = 0;
  int64_t start;
  int64_t now;
  unsigned int row;
  int64_t heartbeat = 0;
  uint64_t raw_frame_changes = 0;
  uint64_t debounced_reports = 0;

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--help") == 0) {
      usage(stdout);
      return 0;
    } else if (strcmp(argv[i], "--inspect") == 0) {
      inspect = 1;
    } else if (strcmp(argv[i], "--chip") == 0 && i + 1 < argc) {
      mapping.chip_path = argv[++i];
      if (mapping.chip_path[0] == '\0') {
        usage(stderr);
        return 2;
      }
    } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      if (parse_bounded_uint(argv[++i], 86400, &seconds) < 0) {
        usage(stderr);
        return 2;
      }
    } else if (strcmp(argv[i], "--settle-us") == 0 && i + 1 < argc) {
      if (parse_bounded_uint(argv[++i], 100000, &settle_us) < 0) {
        usage(stderr);
        return 2;
      }
    } else if (strcmp(argv[i], "--column-batch") == 0 && i + 1 < argc) {
      if (parse_bounded_uint(argv[++i], PIDP_GPIO_V2_COLS, &column_batch) < 0) {
        usage(stderr);
        return 2;
      }
    } else {
      usage(stderr);
      return 2;
    }
  }
  if (install_signals() < 0) {
    perror("signal setup");
    return 1;
  }
  if (inspect) {
    int fd = open(mapping.chip_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      perror("inspect open");
      return 1;
    }
    if (inspect_chip(fd, mapping.chip_path, 1) < 0) {
      perror("inspect");
      result = 1;
    }
    if (close(fd) < 0) {
      perror("inspect close");
      result = 1;
    }
    if (fflush(stdout) == EOF || ferror(stdout))
      result = 1;
    return stop_signal ? 128 + stop_signal : result;
  }
  settle_ns = (long)settle_us * 1000L;
  memcpy(mapping.offsets, switch_offsets, sizeof(mapping.offsets));
  if (monotonic_ns(&start) < 0) {
    perror("monotonic clock");
    return 1;
  }
  if (pidp_gpio_v2_open(&backend, &mapping, &ops, NULL) < 0) {
    perror("gpio preflight/acquire");
    result = 1;
    goto cleanup;
  }
  printf("switch-only monitor: LEDs held LOW; column_batch=%u, %uus settling, 5ms idle, 20ms toggle debounce; heartbeat 5s\n",
      column_batch, settle_us);
  usage(stdout);
  if (fflush(stdout) == EOF || ferror(stdout)) {
    result = 1;
    goto cleanup;
  }
  while (!stop_signal) {
    if (monotonic_ns(&now) < 0) {
      perror("monotonic clock");
      result = 1;
      break;
    }
    if (now - start >= (int64_t)seconds * INT64_C(1000000000))
      break;
    i = scan_switch_frame(&backend, rows, settle_ns, column_batch);
    if (i != 0) {
      if (i < 0) {
        perror("switch scan");
        result = 1;
      }
      break;
    }
    for (row = 0; row < PIDP_GPIO_V2_SWITCH_ROWS; ++row) {
      raw_and[row] &= rows[row];
      raw_or[row] |= rows[row];
    }
    ++completed_frames;
    if (monotonic_ns(&now) < 0) {
      perror("monotonic clock");
      result = 1;
      break;
    }
    if (!have_previous_raw) {
      memcpy(previous_raw, rows, sizeof(previous_raw));
      have_previous_raw = 1;
    } else if (memcmp(previous_raw, rows, sizeof(previous_raw)) != 0) {
      memcpy(previous_raw, rows, sizeof(previous_raw));
      ++raw_frame_changes;
    }
    i = filter_switches(&filter, rows, now);
    if (i || now - heartbeat >= HEARTBEAT_NS) {
      if (report_switches(i ? "change" : "heartbeat", now - start,
            filter.stable) < 0) {
        perror("switch report");
        result = 1;
        break;
      }
      if (i)
        ++debounced_reports;
      if (now - heartbeat >= HEARTBEAT_NS || heartbeat == 0)
        heartbeat = now;
    }
    i = pause_ns(5000000L);
    if (i != 0) {
      if (i < 0) {
        perror("scan pause");
        result = 1;
      }
      break;
    }
  }

cleanup:
  /* close performs best-effort idle even on errors; no userspace guarantee after release. */
  if (pidp_gpio_v2_close(&backend) < 0) {
    perror("gpio idle/close");
    result = 1;
  }
  fprintf(stderr, "switch monitor statistics: completed_frames=%" PRIu64
      " raw_frame_changes=%" PRIu64 " debounced_reports=%" PRIu64
      " raw_and=%03" PRIx32 ",%03" PRIx32 ",%03" PRIx32
      " raw_or=%03" PRIx32 ",%03" PRIx32 ",%03" PRIx32 "\n",
      completed_frames, raw_frame_changes, debounced_reports,
      raw_and[0], raw_and[1], raw_and[2], raw_or[0], raw_or[1], raw_or[2]);
  fprintf(stderr, "switch monitor stopped: %s%s\n",
      stop_signal ? "signal" : result ? "error" : "timeout",
      result ? " (error; electrical state not guaranteed)" : " (request released)");
  return result ? result : stop_signal ? 128 + stop_signal : 0;
}
