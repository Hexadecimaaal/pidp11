#include "gpio_v2.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_CHIP_FD 31
#define TEST_REQUEST_FD 73
#define TEST_CHIP_LINES 128u
#define TEST_MAX_IOCTLS 64u

#define TEST_LED_MASK \
  ((UINT64_C(1) << PIDP_GPIO_V2_LED_ROWS) - UINT64_C(1))
#define TEST_COL_MASK \
  (((UINT64_C(1) << PIDP_GPIO_V2_COLS) - UINT64_C(1)) \
    << PIDP_GPIO_V2_LED_ROWS)
#define TEST_SWITCH_MASK \
  (((UINT64_C(1) << PIDP_GPIO_V2_SWITCH_ROWS) - UINT64_C(1)) \
    << (PIDP_GPIO_V2_LED_ROWS + PIDP_GPIO_V2_COLS))
#define TEST_OUTPUT_MASK (TEST_LED_MASK | TEST_COL_MASK)

struct fake_transport {
  unsigned int open_calls;
  unsigned int ioctl_calls;
  unsigned int close_calls;
  unsigned int request_ioctl_calls;
  unsigned int config_count;
  unsigned int values_count;
  unsigned long ioctl_requests[TEST_MAX_IOCTLS];
  int fds[TEST_MAX_IOCTLS];
  struct gpio_v2_line_request line_requests[TEST_MAX_IOCTLS];
  struct gpio_v2_line_config configs[TEST_MAX_IOCTLS];
  struct gpio_v2_line_values values[TEST_MAX_IOCTLS];
  uint32_t chip_lines;
  uint64_t input_bits;
  unsigned int fail_open_call;
  int fail_open_errno;
  unsigned int fail_ioctl_call;
  int fail_ioctl_errno;
  unsigned int fail_close_call;
  int fail_close_errno;
  int request_open;
  int chip_open;
};

static void fake_init(struct fake_transport *fake)
{
  memset(fake, 0, sizeof(*fake));
  fake->chip_lines = TEST_CHIP_LINES;
}

static int fake_open(void *context, const char *path, int flags)
{
  struct fake_transport *fake = context;

  if (strcmp(path, "/dev/gpiochip-fake") != 0
      || flags != (O_RDWR | O_CLOEXEC) || fake->chip_open) {
    errno = EINVAL;
    return -1;
  }
  ++fake->open_calls;
  if (fake->fail_open_call != 0
      && fake->fail_open_call == fake->open_calls) {
    errno = fake->fail_open_errno;
    return -1;
  }
  fake->chip_open = 1;
  return TEST_CHIP_FD;
}

static int fake_ioctl(void *context, int fd, unsigned long request,
    void *argument)
{
  struct fake_transport *fake = context;
  unsigned int index = fake->ioctl_calls;

  if (index >= TEST_MAX_IOCTLS) {
    errno = EOVERFLOW;
    return -1;
  }
  fake->ioctl_requests[index] = request;
  fake->fds[index] = fd;
  ++fake->ioctl_calls;

  if (fake->fail_ioctl_call != 0 && fake->fail_ioctl_call == fake->ioctl_calls) {
    errno = fake->fail_ioctl_errno;
    return -1;
  }

  if (request == GPIO_GET_CHIPINFO_IOCTL) {
    struct gpiochip_info *info = argument;
    if (fd != TEST_CHIP_FD || index != 0) {
      errno = ENOTTY;
      return -1;
    }
    memset(info, 0, sizeof(*info));
    info->lines = fake->chip_lines;
    return 0;
  }
  if (request == GPIO_V2_GET_LINE_IOCTL) {
    struct gpio_v2_line_request *line_request = argument;
    unsigned int i;

    if (fd != TEST_CHIP_FD || fake->request_open
        || line_request->num_lines != PIDP_GPIO_V2_LINES) {
      errno = ENOTTY;
      return -1;
    }
    for (i = 0; i < PIDP_GPIO_V2_LINES; ++i) {
      if (line_request->offsets[i] >= fake->chip_lines) {
        errno = EINVAL;
        return -1;
      }
    }
    fake->line_requests[index] = *line_request;
    assert(fake->config_count < TEST_MAX_IOCTLS);
    fake->configs[fake->config_count++] = line_request->config;
    line_request->fd = TEST_REQUEST_FD;
    fake->request_open = 1;
    ++fake->request_ioctl_calls;
    return 0;
  }
  if (fd != TEST_REQUEST_FD || !fake->request_open) {
    errno = ENOTTY;
    return -1;
  }
  if (request == GPIO_V2_LINE_SET_CONFIG_IOCTL) {
    assert(fake->config_count < TEST_MAX_IOCTLS);
    fake->configs[fake->config_count++] =
        *(const struct gpio_v2_line_config *)argument;
    ++fake->request_ioctl_calls;
    return 0;
  }
  if (request == GPIO_V2_LINE_SET_VALUES_IOCTL) {
    assert(fake->values_count < TEST_MAX_IOCTLS);
    fake->values[fake->values_count++] =
        *(const struct gpio_v2_line_values *)argument;
    ++fake->request_ioctl_calls;
    return 0;
  }
  if (request == GPIO_V2_LINE_GET_VALUES_IOCTL) {
    struct gpio_v2_line_values *values = argument;
    values->bits = fake->input_bits;
    ++fake->request_ioctl_calls;
    return 0;
  }

  errno = ENOTTY;
  return -1;
}

static int fake_close(void *context, int fd)
{
  struct fake_transport *fake = context;

  ++fake->close_calls;
  if (fake->fail_close_call != 0
      && fake->fail_close_call == fake->close_calls) {
    errno = fake->fail_close_errno;
    return -1;
  }
  if (fd == TEST_CHIP_FD && fake->chip_open) {
    fake->chip_open = 0;
    return 0;
  }
  if (fd == TEST_REQUEST_FD && fake->request_open) {
    fake->request_open = 0;
    return 0;
  }
  errno = EBADF;
  return -1;
}

static const struct pidp_gpio_v2_ops fake_ops = {
  .open = fake_open,
  .ioctl = fake_ioctl,
  .close = fake_close,
};

static struct pidp_gpio_v2_mapping test_mapping(void)
{
  struct pidp_gpio_v2_mapping mapping = {
    .chip_path = "/dev/gpiochip-fake",
    .offsets = {
      90, 4, 72, 5, 111, 6,
      50, 12, 88, 13, 89, 14, 91, 15, 92, 16, 93, 17,
      20, 21, 22,
    },
  };
  return mapping;
}

static const char valid_offsets_text[] =
  "90,4,72,5,111,6,50,12,88,13,89,14,91,15,92,16,93,17,20,21,22";
static const char max_offsets_text[] =
  "4294967295,4,72,5,111,6,50,12,88,13,89,14,91,15,92,16,93,17,20,21,22";
static const char overflow_offsets_text[] =
  "90,4,72,5,111,6,50,12,88,13,89,14,91,15,92,16,93,17,20,21,4294967296";
static const char duplicate_offsets_text[] =
  "90,4,72,5,111,6,50,12,88,13,89,14,91,15,92,16,93,17,20,21,90";
static const char sign_offsets_text[] =
  "+90,4,72,5,111,6,50,12,88,13,89,14,91,15,92,16,93,17,20,21,22";
static const char negative_offsets_text[] =
  "-90,4,72,5,111,6,50,12,88,13,89,14,91,15,92,16,93,17,20,21,22";
static const char whitespace_offsets_text[] =
  "90, 4,72,5,111,6,50,12,88,13,89,14,91,15,92,16,93,17,20,21,22";
static const char trailing_junk_offsets_text[] =
  "90,4,72,5,111,6,50,12,88,13,89,14,91,15,92,16,93,17,20,21,22x";
static const char trailing_comma_offsets_text[] =
  "90,4,72,5,111,6,50,12,88,13,89,14,91,15,92,16,93,17,20,21,22,";
static const char short_offsets_text[] =
  "90,4,72,5,111,6,50,12,88,13,89,14,91,15,92,16,93,17,20,21";
static const char long_offsets_text[] =
  "90,4,72,5,111,6,50,12,88,13,89,14,91,15,92,16,93,17,20,21,22,23";

static void assert_mapping_unchanged(
    const struct pidp_gpio_v2_mapping *mapping,
    const struct pidp_gpio_v2_mapping *expected)
{
  unsigned int i;

  assert(mapping->chip_path == expected->chip_path);
  for (i = 0; i < PIDP_GPIO_V2_LINES; ++i)
    assert(mapping->offsets[i] == expected->offsets[i]);
}

static void assert_parse_failure(struct pidp_gpio_v2_mapping *mapping,
    const struct pidp_gpio_v2_mapping *expected, const char *chip_path,
    const char *offsets_text, int expected_errno)
{
  errno = 0;
  assert(pidp_gpio_v2_parse_mapping(mapping, chip_path, offsets_text) == -1);
  assert(errno == expected_errno);
  assert_mapping_unchanged(mapping, expected);
}

static void assert_initial_config(const struct gpio_v2_line_config *config)
{
  assert(config->flags == (GPIO_V2_LINE_FLAG_INPUT
      | GPIO_V2_LINE_FLAG_BIAS_PULL_UP));
  assert(config->num_attrs == 2);
  assert(config->attrs[0].attr.id == GPIO_V2_LINE_ATTR_ID_FLAGS);
  assert(config->attrs[0].attr.flags == GPIO_V2_LINE_FLAG_OUTPUT);
  assert(config->attrs[0].mask == TEST_LED_MASK);
  assert(config->attrs[1].attr.id == GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES);
  assert(config->attrs[1].attr.values == 0);
  assert(config->attrs[1].mask == TEST_LED_MASK);
}

static void assert_display_config(const struct gpio_v2_line_config *config,
    uint32_t led_bits)
{
  uint64_t expected_values = ((~(uint64_t)led_bits) & UINT64_C(0x0fff)) << 6;

  assert(config->flags == (GPIO_V2_LINE_FLAG_INPUT
      | GPIO_V2_LINE_FLAG_BIAS_PULL_UP));
  assert(config->num_attrs == 2);
  assert(config->attrs[0].attr.id == GPIO_V2_LINE_ATTR_ID_FLAGS);
  assert(config->attrs[0].attr.flags == GPIO_V2_LINE_FLAG_OUTPUT);
  assert(config->attrs[0].mask == TEST_OUTPUT_MASK);
  assert(config->attrs[1].attr.id == GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES);
  assert(config->attrs[1].attr.values == expected_values);
  assert(config->attrs[1].mask == TEST_OUTPUT_MASK);
}

static void assert_switch_config(const struct gpio_v2_line_config *config,
    unsigned int row)
{
  uint64_t row_mask = UINT64_C(1) << (18 + row);
  uint64_t output_mask = TEST_LED_MASK | row_mask;

  assert(config->flags == (GPIO_V2_LINE_FLAG_INPUT
      | GPIO_V2_LINE_FLAG_BIAS_PULL_UP));
  assert(config->num_attrs == 2);
  assert(config->attrs[0].attr.id == GPIO_V2_LINE_ATTR_ID_FLAGS);
  assert(config->attrs[0].attr.flags == GPIO_V2_LINE_FLAG_OUTPUT);
  assert(config->attrs[0].mask == output_mask);
  assert(config->attrs[1].attr.id == GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES);
  assert(config->attrs[1].attr.values == 0);
  assert(config->attrs[1].mask == output_mask);
}

static void open_backend(struct pidp_gpio_v2 *backend,
    struct fake_transport *fake)
{
  struct pidp_gpio_v2_mapping mapping = test_mapping();
  unsigned int i;

  memset(backend, 0, sizeof(*backend));
  assert(pidp_gpio_v2_open(backend, &mapping, &fake_ops, fake) == 0);
  assert(fake->open_calls == 1);
  assert(fake->close_calls == 1);
  assert(fake->request_ioctl_calls == 1);
  assert_initial_config(&fake->configs[0]);
  for (i = 0; i < PIDP_GPIO_V2_LINES; ++i)
    assert(fake->line_requests[1].offsets[i] == mapping.offsets[i]);
  assert(fake->ioctl_requests[0] == GPIO_GET_CHIPINFO_IOCTL);
  assert(fake->ioctl_requests[1] == GPIO_V2_GET_LINE_IOCTL);
  assert(fake->fds[0] == TEST_CHIP_FD);
  assert(fake->fds[1] == TEST_CHIP_FD);
}
static void test_mapping_validation(void)
{
  struct fake_transport fake;
  struct pidp_gpio_v2 backend = {0};
  struct pidp_gpio_v2_mapping mapping = test_mapping();
  const struct pidp_gpio_v2_ops partial_ops = {
    .open = fake_open,
    .ioctl = NULL,
    .close = fake_close,
  };

  fake_init(&fake);
  mapping.offsets[20] = mapping.offsets[0];
  errno = 0;
  assert(pidp_gpio_v2_open(&backend, &mapping, &fake_ops, &fake) == -1);
  assert(errno == EINVAL);
  assert(fake.open_calls == 0);
  assert(fake.ioctl_calls == 0);

  fake_init(&fake);
  mapping = test_mapping();
  mapping.offsets[4] = TEST_CHIP_LINES;
  assert(pidp_gpio_v2_open(&backend, &mapping, &fake_ops, &fake) == -1);
  assert(errno == EINVAL);
  assert(fake.open_calls == 1);
  assert(fake.request_ioctl_calls == 0);
  assert(fake.close_calls == 1);

  fake_init(&fake);
  mapping = test_mapping();
  assert(pidp_gpio_v2_open(&backend, &mapping, &partial_ops, &fake) == -1);
  assert(errno == EINVAL);
  assert(fake.open_calls == 0);
  assert(fake.ioctl_calls == 0);
}

static void test_mapping_parser(void)
{
  struct pidp_gpio_v2_mapping mapping;
  struct pidp_gpio_v2_mapping expected;
  const char *chip_path;

  expected = test_mapping();
  chip_path = expected.chip_path;
  mapping = test_mapping();
  assert(pidp_gpio_v2_parse_mapping(&mapping, chip_path,
      valid_offsets_text) == 0);
  assert_mapping_unchanged(&mapping, &expected);

  mapping = test_mapping();
  assert(pidp_gpio_v2_parse_mapping(&mapping, chip_path,
      max_offsets_text) == 0);
  assert(mapping.chip_path == chip_path);
  assert(mapping.offsets[0] == UINT32_MAX);

  mapping = test_mapping();
  expected = mapping;
  assert_parse_failure(&mapping, &expected, NULL, valid_offsets_text, EINVAL);

  mapping = test_mapping();
  expected = mapping;
  assert_parse_failure(&mapping, &expected, "", valid_offsets_text, EINVAL);

  mapping = test_mapping();
  expected = mapping;
  assert_parse_failure(&mapping, &expected, chip_path, NULL, EINVAL);

  mapping = test_mapping();
  expected = mapping;
  assert_parse_failure(&mapping, &expected, chip_path, "", EINVAL);

  mapping = test_mapping();
  expected = mapping;
  assert_parse_failure(&mapping, &expected, chip_path, sign_offsets_text,
      EINVAL);

  mapping = test_mapping();
  expected = mapping;
  assert_parse_failure(&mapping, &expected, chip_path, negative_offsets_text,
      EINVAL);

  mapping = test_mapping();
  expected = mapping;
  assert_parse_failure(&mapping, &expected, chip_path, whitespace_offsets_text,
      EINVAL);

  mapping = test_mapping();
  expected = mapping;
  assert_parse_failure(&mapping, &expected, chip_path,
      trailing_junk_offsets_text, EINVAL);

  mapping = test_mapping();
  expected = mapping;
  assert_parse_failure(&mapping, &expected, chip_path,
      trailing_comma_offsets_text, EINVAL);

  mapping = test_mapping();
  expected = mapping;
  assert_parse_failure(&mapping, &expected, chip_path, short_offsets_text,
      EINVAL);

  mapping = test_mapping();
  expected = mapping;
  assert_parse_failure(&mapping, &expected, chip_path, long_offsets_text,
      EINVAL);

  mapping = test_mapping();
  expected = mapping;
  assert_parse_failure(&mapping, &expected, chip_path, overflow_offsets_text,
      ERANGE);

  mapping = test_mapping();
  expected = mapping;
  assert_parse_failure(&mapping, &expected, chip_path,
      duplicate_offsets_text, EINVAL);
}

static void test_open_transport_failures(void)
{
  struct fake_transport fake;
  struct pidp_gpio_v2 backend = {0};
  struct pidp_gpio_v2_mapping mapping = test_mapping();

  fake_init(&fake);
  fake.fail_open_call = 1;
  fake.fail_open_errno = EACCES;
  assert(pidp_gpio_v2_open(&backend, &mapping, &fake_ops, &fake) == -1);
  assert(errno == EACCES);
  assert(fake.open_calls == 1);
  assert(fake.ioctl_calls == 0);
  assert(fake.close_calls == 0);
  assert(fake.chip_open == 0);
  assert(pidp_gpio_v2_close(&backend) == 0);

  fake_init(&fake);
  fake.fail_ioctl_call = 1;
  fake.fail_ioctl_errno = EIO;
  assert(pidp_gpio_v2_open(&backend, &mapping, &fake_ops, &fake) == -1);
  assert(errno == EIO);
  assert(fake.open_calls == 1);
  assert(fake.ioctl_calls == 1);
  assert(fake.close_calls == 1);
  assert(fake.chip_open == 0);
  assert(pidp_gpio_v2_close(&backend) == 0);

  fake_init(&fake);
  fake.fail_ioctl_call = 2;
  fake.fail_ioctl_errno = EBUSY;
  assert(pidp_gpio_v2_open(&backend, &mapping, &fake_ops, &fake) == -1);
  assert(errno == EBUSY);
  assert(fake.open_calls == 1);
  assert(fake.ioctl_calls == 2);
  assert(fake.close_calls == 1);
  assert(fake.chip_open == 0);
  assert(pidp_gpio_v2_close(&backend) == 0);

  fake_init(&fake);
  fake.fail_close_call = 1;
  fake.fail_close_errno = EIO;
  assert(pidp_gpio_v2_open(&backend, &mapping, &fake_ops, &fake) == -1);
  assert(errno == EIO);
  assert(fake.open_calls == 1);
  assert(fake.ioctl_calls == 2);
  assert(fake.close_calls == 2);
  assert(fake.request_open == 0);
  assert(pidp_gpio_v2_close(&backend) == 0);
  assert(fake.close_calls == 2);
}

static void test_display_switch_and_idle(void)
{
  struct fake_transport fake;
  struct pidp_gpio_v2 backend;
  uint32_t physical_bits = 0;
  uint32_t led_bits = UINT32_C(0x005);
  unsigned int values_before;
  unsigned int configs_before;

  fake_init(&fake);
  open_backend(&backend, &fake);
  assert(pidp_gpio_v2_display(&backend, 2, led_bits) == 0);
  assert(fake.values_count == 2);
  assert(fake.values[0].bits == 0);
  assert(fake.values[0].mask == TEST_LED_MASK);
  assert(fake.config_count == 2);
  assert_display_config(&fake.configs[1], led_bits);
  assert(fake.values[1].bits == (UINT64_C(1) << 2));
  assert(fake.values[1].mask == TEST_LED_MASK);

  values_before = fake.values_count;
  configs_before = fake.config_count;
  assert(pidp_gpio_v2_select_switch(&backend, 1) == 0);
  assert(fake.values_count == values_before + 1);
  assert(fake.values[fake.values_count - 1].bits == 0);
  assert(fake.values[fake.values_count - 1].mask == TEST_LED_MASK);
  assert(fake.config_count == configs_before + 1);
  assert_switch_config(&fake.configs[fake.config_count - 1], 1);

  fake.input_bits = (UINT64_C(0x5a3) << 6) | (UINT64_C(1) << 1);
  assert(pidp_gpio_v2_read_switches(&backend, &physical_bits) == 0);
  assert(physical_bits == UINT32_C(0x5a3));

  values_before = fake.values_count;
  configs_before = fake.config_count;
  assert(pidp_gpio_v2_select_switch(&backend, 2) == 0);
  assert(fake.values_count == values_before);
  assert(fake.config_count == configs_before + 2);
  assert_initial_config(&fake.configs[configs_before]);
  assert_switch_config(&fake.configs[fake.config_count - 1], 2);

  values_before = fake.values_count;
  configs_before = fake.config_count;
  assert(pidp_gpio_v2_display(&backend, 3, led_bits) == 0);
  assert(fake.values_count == values_before + 1);
  assert(fake.config_count == configs_before + 2);
  assert_initial_config(&fake.configs[configs_before]);
  assert_display_config(&fake.configs[fake.config_count - 1], led_bits);
  assert(fake.values[fake.values_count - 1].bits == (UINT64_C(1) << 3));
  assert(fake.values[fake.values_count - 1].mask == TEST_LED_MASK);

  values_before = fake.values_count;
  assert(pidp_gpio_v2_blank(&backend) == 0);
  assert(fake.values_count == values_before + 1);
  assert(fake.values[fake.values_count - 1].bits == 0);
  assert(fake.values[fake.values_count - 1].mask == TEST_LED_MASK);
  values_before = fake.values_count;
  assert(pidp_gpio_v2_idle(&backend) == 0);
  assert(fake.values_count == values_before + 1);
  assert_initial_config(&fake.configs[fake.config_count - 1]);
  assert(pidp_gpio_v2_close(&backend) == 0);
  assert(fake.request_open == 0);
  assert(fake.close_calls == 2);
  assert(pidp_gpio_v2_close(&backend) == 0);
}

static void test_invalid_runtime_arguments(void)
{
  struct fake_transport fake;
  struct pidp_gpio_v2 backend;
  unsigned int close_calls;

  fake_init(&fake);
  open_backend(&backend, &fake);
  assert(pidp_gpio_v2_display(&backend, PIDP_GPIO_V2_LED_ROWS, 0) == -1);
  assert(errno == EINVAL);
  close_calls = fake.close_calls;
  assert(close_calls == 2);
  assert(pidp_gpio_v2_close(&backend) == 0);

  fake_init(&fake);
  open_backend(&backend, &fake);
  assert(pidp_gpio_v2_display(&backend, 0,
      PIDP_GPIO_V2_LED_MASK + UINT32_C(1)) == -1);
  assert(errno == EINVAL);
  assert(fake.close_calls == 2);
  assert(pidp_gpio_v2_close(&backend) == 0);

  fake_init(&fake);
  open_backend(&backend, &fake);
  assert(pidp_gpio_v2_read_switches(&backend, NULL) == -1);
  assert(errno == EINVAL);
  assert(fake.close_calls == 2);
  assert(pidp_gpio_v2_close(&backend) == 0);
}

static void test_ioctl_failure_and_cleanup(void)
{
  struct fake_transport fake;
  struct pidp_gpio_v2 backend;
  unsigned int close_calls;

  fake_init(&fake);
  open_backend(&backend, &fake);
  fake.fail_ioctl_call = fake.ioctl_calls + 2;
  fake.fail_ioctl_errno = EIO;
  assert(pidp_gpio_v2_display(&backend, 0, 0) == -1);
  assert(errno == EIO);
  assert(fake.request_open == 0);
  close_calls = fake.close_calls;
  assert(close_calls == 2);
  assert(pidp_gpio_v2_close(&backend) == 0);

  fake_init(&fake);
  open_backend(&backend, &fake);
  fake.fail_close_call = fake.close_calls + 1;
  fake.fail_close_errno = EIO;
  assert(pidp_gpio_v2_close(&backend) == -1);
  assert(errno == EIO);
  assert(fake.close_calls == 2);
  assert(pidp_gpio_v2_close(&backend) == 0);
  assert(fake.close_calls == 2);
}

int main(void)
{
  test_mapping_validation();
  test_mapping_parser();
  test_open_transport_failures();
  test_display_switch_and_idle();
  test_invalid_runtime_arguments();
  test_ioctl_failure_and_cleanup();
  puts("gpio_v2 fake transport tests passed");
  return 0;
}
