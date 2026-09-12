#include "gpio_v2.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/gpio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if PIDP_GPIO_V2_LINES > GPIO_V2_LINES_MAX
#error "pidp gpio mapping exceeds gpio v2 request capacity"
#endif
#if 3 > GPIO_V2_LINE_NUM_ATTRS_MAX
#error "pidp gpio configuration exceeds gpio v2 attribute capacity"
#endif

#define PIDP_GPIO_V2_MODE_CLOSED 0u
#define PIDP_GPIO_V2_MODE_IDLE 1u
#define PIDP_GPIO_V2_MODE_DISPLAY 2u
#define PIDP_GPIO_V2_MODE_SWITCH 3u

#define PIDP_GPIO_V2_LED_REQUEST_MASK \
  ((UINT64_C(1) << PIDP_GPIO_V2_LED_ROWS) - UINT64_C(1))
#define PIDP_GPIO_V2_COL_REQUEST_MASK \
  (((UINT64_C(1) << PIDP_GPIO_V2_COLS) - UINT64_C(1)) \
    << PIDP_GPIO_V2_LED_ROWS)
#define PIDP_GPIO_V2_SWITCH_REQUEST_MASK \
  (((UINT64_C(1) << PIDP_GPIO_V2_SWITCH_ROWS) - UINT64_C(1)) \
    << (PIDP_GPIO_V2_LED_ROWS + PIDP_GPIO_V2_COLS))
#define PIDP_GPIO_V2_OUTPUT_REQUEST_MASK \
  (PIDP_GPIO_V2_LED_REQUEST_MASK | PIDP_GPIO_V2_COL_REQUEST_MASK)

_Static_assert(PIDP_GPIO_V2_LINES <= GPIO_V2_LINES_MAX,
    "gpio v2 line array is too small");
_Static_assert(PIDP_GPIO_V2_LED_ROWS + PIDP_GPIO_V2_COLS
    + PIDP_GPIO_V2_SWITCH_ROWS == PIDP_GPIO_V2_LINES,
    "gpio v2 request layout is inconsistent");
_Static_assert(PIDP_GPIO_V2_LED_MASK == UINT32_C(0x0fff),
    "gpio v2 led mask changed");

static int posix_open(void *context, const char *path, int flags)
{
  (void)context;
  return open(path, flags);
}

static int posix_ioctl(void *context, int fd, unsigned long request,
    void *argument)
{
  (void)context;
  return ioctl(fd, request, argument);
}

static int posix_close(void *context, int fd)
{
  (void)context;
  return close(fd);
}

static struct pidp_gpio_v2_ops default_ops(void)
{
  struct pidp_gpio_v2_ops ops = {
    .open = posix_open,
    .ioctl = posix_ioctl,
    .close = posix_close,
  };
  return ops;
}

static int select_ops(const struct pidp_gpio_v2_ops *ops,
    struct pidp_gpio_v2_ops *selected)
{
  if (selected == NULL)
    return EINVAL;
  if (ops == NULL) {
    *selected = default_ops();
    return 0;
  }
  if (ops->open == NULL || ops->ioctl == NULL || ops->close == NULL)
    return EINVAL;
  *selected = *ops;
  return 0;
}

static int failure_errno(void)
{
  return errno != 0 ? errno : EIO;
}

static int fail_return(int error)
{
  errno = error != 0 ? error : EIO;
  return -1;
}

static int call_open(const struct pidp_gpio_v2_ops *ops, void *context,
    const char *path, int flags)
{
  int result;

  errno = 0;
  result = ops->open(context, path, flags);
  if (result < 0)
    return fail_return(failure_errno());
  return result;
}

static int call_ioctl(const struct pidp_gpio_v2_ops *ops, void *context,
    int fd, unsigned long request, void *argument)
{
  int result;

  errno = 0;
  result = ops->ioctl(context, fd, request, argument);
  if (result < 0)
    return fail_return(failure_errno());
  return 0;
}

static int call_close(const struct pidp_gpio_v2_ops *ops, void *context,
    int fd)
{
  int result;

  errno = 0;
  result = ops->close(context, fd);
  if (result < 0)
    return fail_return(failure_errno());
  return 0;
}

static int valid_mapping(const struct pidp_gpio_v2_mapping *mapping)
{
  unsigned int i;
  unsigned int j;

  if (mapping == NULL || mapping->chip_path == NULL
      || mapping->chip_path[0] == '\0')
    return EINVAL;

  for (i = 0; i < PIDP_GPIO_V2_LINES; ++i) {
    for (j = 0; j < i; ++j) {
      if (mapping->offsets[i] == mapping->offsets[j])
        return EINVAL;
    }
  }
  return 0;
}

int pidp_gpio_v2_parse_mapping(struct pidp_gpio_v2_mapping *mapping,
    const char *chip_path, const char *offsets_text)
{
  struct pidp_gpio_v2_mapping parsed = {0};
  const char *cursor = offsets_text;
  unsigned int i;
  int error;

  if (mapping == NULL || chip_path == NULL || chip_path[0] == '\0'
      || offsets_text == NULL)
    return fail_return(EINVAL);

  parsed.chip_path = chip_path;
  for (i = 0; i < PIDP_GPIO_V2_LINES; ++i) {
    uint32_t value = 0;

    if (*cursor < '0' || *cursor > '9')
      return fail_return(EINVAL);
    do {
      uint32_t digit = (uint32_t)(*cursor - '0');

      if (value > (UINT32_MAX - digit) / UINT32_C(10))
        return fail_return(ERANGE);
      value = value * UINT32_C(10) + digit;
      ++cursor;
    } while (*cursor >= '0' && *cursor <= '9');
    parsed.offsets[i] = value;
    if (i + 1 == PIDP_GPIO_V2_LINES) {
      if (*cursor != '\0')
        return fail_return(EINVAL);
    } else {
      if (*cursor != ',')
        return fail_return(EINVAL);
      ++cursor;
    }
  }

  error = valid_mapping(&parsed);
  if (error != 0)
    return fail_return(error);
  *mapping = parsed;
  return 0;
}

static int add_attribute(struct gpio_v2_line_config *config, __u32 id,
    __u64 value, __u64 mask)
{
  struct gpio_v2_line_config_attribute *attribute;

  if (config->num_attrs >= GPIO_V2_LINE_NUM_ATTRS_MAX)
    return -EOVERFLOW;

  attribute = &config->attrs[config->num_attrs++];
  attribute->attr.id = id;
  if (id == GPIO_V2_LINE_ATTR_ID_FLAGS)
    attribute->attr.flags = value;
  else if (id == GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES)
    attribute->attr.values = value;
  else
    return -EINVAL;
  attribute->mask = mask;
  return 0;
}

static int initial_config(struct gpio_v2_line_config *config)
{
  int result;

  memset(config, 0, sizeof(*config));
  config->flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
  result = add_attribute(config, GPIO_V2_LINE_ATTR_ID_FLAGS,
      GPIO_V2_LINE_FLAG_OUTPUT, PIDP_GPIO_V2_LED_REQUEST_MASK);
  if (result != 0)
    return result;
  return add_attribute(config, GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES, 0,
      PIDP_GPIO_V2_LED_REQUEST_MASK);
}

static int display_config(struct gpio_v2_line_config *config,
    uint32_t led_bits)
{
  __u64 column_values;
  int result;

  memset(config, 0, sizeof(*config));
  config->flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
  result = add_attribute(config, GPIO_V2_LINE_ATTR_ID_FLAGS,
      GPIO_V2_LINE_FLAG_OUTPUT, PIDP_GPIO_V2_OUTPUT_REQUEST_MASK);
  if (result != 0)
    return result;

  column_values = ((~(__u64)led_bits) & UINT64_C(0x0fff))
      << PIDP_GPIO_V2_LED_ROWS;
  return add_attribute(config, GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES,
      column_values, PIDP_GPIO_V2_OUTPUT_REQUEST_MASK);
}

static int switch_config(struct gpio_v2_line_config *config,
    unsigned int row)
{
  __u64 row_mask = UINT64_C(1)
      << (PIDP_GPIO_V2_LED_ROWS + PIDP_GPIO_V2_COLS + row);
  __u64 output_mask = PIDP_GPIO_V2_LED_REQUEST_MASK | row_mask;
  int result;

  memset(config, 0, sizeof(*config));
  config->flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
  result = add_attribute(config, GPIO_V2_LINE_ATTR_ID_FLAGS,
      GPIO_V2_LINE_FLAG_OUTPUT, output_mask);
  if (result != 0)
    return result;
  return add_attribute(config, GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES, 0,
      output_mask);
}

static int switch_columns_config(struct gpio_v2_line_config *config,
    unsigned int row, uint32_t column_mask)
{
  __u64 row_mask = UINT64_C(1)
      << (PIDP_GPIO_V2_LED_ROWS + PIDP_GPIO_V2_COLS + row);
  __u64 output_mask = PIDP_GPIO_V2_LED_REQUEST_MASK | row_mask;
  __u64 pullup_mask = ((__u64)column_mask << PIDP_GPIO_V2_LED_ROWS)
      | (PIDP_GPIO_V2_SWITCH_REQUEST_MASK & ~row_mask);
  int result;

  memset(config, 0, sizeof(*config));
  config->flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_DISABLED;
  result = add_attribute(config, GPIO_V2_LINE_ATTR_ID_FLAGS,
      GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP, pullup_mask);
  if (result != 0)
    return result;
  result = add_attribute(config, GPIO_V2_LINE_ATTR_ID_FLAGS,
      GPIO_V2_LINE_FLAG_OUTPUT, output_mask);
  if (result != 0)
    return result;
  return add_attribute(config, GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES, 0,
      output_mask);
}

static int apply_config(struct pidp_gpio_v2 *backend,
    const struct gpio_v2_line_config *config)
{
  struct gpio_v2_line_config copy = *config;

  return call_ioctl(&backend->ops, backend->context, backend->request_fd,
      GPIO_V2_LINE_SET_CONFIG_IOCTL, &copy);
}

static int set_values(struct pidp_gpio_v2 *backend, __u64 bits, __u64 mask)
{
  struct gpio_v2_line_values values;

  memset(&values, 0, sizeof(values));
  values.bits = bits & mask;
  values.mask = mask;
  return call_ioctl(&backend->ops, backend->context, backend->request_fd,
      GPIO_V2_LINE_SET_VALUES_IOCTL, &values);
}

static int blank_for_transition(struct pidp_gpio_v2 *backend)
{
  if (backend->mode == PIDP_GPIO_V2_MODE_DISPLAY
      || backend->mode == PIDP_GPIO_V2_MODE_IDLE)
    return set_values(backend, 0, PIDP_GPIO_V2_LED_REQUEST_MASK);
  if (backend->mode == PIDP_GPIO_V2_MODE_SWITCH)
    return 0;
  return fail_return(EBADF);
}

static int idle_request(struct pidp_gpio_v2 *backend)
{
  struct gpio_v2_line_config config;
  int result;
  result = initial_config(&config);
  if (result < 0)
    return fail_return(-result);
  return apply_config(backend, &config);
}

static void clear_backend(struct pidp_gpio_v2 *backend)
{
  backend->request_fd = -1;
  backend->mode = PIDP_GPIO_V2_MODE_CLOSED;
  memset(backend->offsets, 0, sizeof(backend->offsets));
  memset(&backend->ops, 0, sizeof(backend->ops));
  backend->context = NULL;
}

static int fail_live(struct pidp_gpio_v2 *backend, int error)
{
  int ignored;
  int fd;

  if (backend->mode == PIDP_GPIO_V2_MODE_CLOSED)
    return fail_return(error);

  (void)blank_for_transition(backend);
  (void)idle_request(backend);
  fd = backend->request_fd;
  ignored = call_close(&backend->ops, backend->context, fd);
  (void)ignored;
  clear_backend(backend);
  return fail_return(error);
}

int pidp_gpio_v2_open(struct pidp_gpio_v2 *backend,
    const struct pidp_gpio_v2_mapping *mapping,
    const struct pidp_gpio_v2_ops *ops, void *context)
{
  struct gpiochip_info chip_info;
  struct gpio_v2_line_request request;
  struct pidp_gpio_v2_ops selected_ops;
  int chip_fd;
  int request_fd;
  int error;
  unsigned int i;

  if (backend == NULL)
    return fail_return(EINVAL);
  if (backend->mode != PIDP_GPIO_V2_MODE_CLOSED)
    return fail_return(EBUSY);
  error = valid_mapping(mapping);
  if (error != 0)
    return fail_return(error);


  error = select_ops(ops, &selected_ops);
  if (error != 0)
    return fail_return(error);

  chip_fd = call_open(&selected_ops, context, mapping->chip_path,
      O_RDWR | O_CLOEXEC);
  if (chip_fd < 0)
    return -1;

  memset(&chip_info, 0, sizeof(chip_info));
  if (call_ioctl(&selected_ops, context, chip_fd,
      GPIO_GET_CHIPINFO_IOCTL, &chip_info) < 0) {
    error = errno;
    (void)call_close(&selected_ops, context, chip_fd);
    return fail_return(error);
  }

  for (i = 0; i < PIDP_GPIO_V2_LINES; ++i) {
    if (mapping->offsets[i] >= chip_info.lines) {
      error = EINVAL;
      (void)call_close(&selected_ops, context, chip_fd);
      return fail_return(error);
    }
  }

  memset(&request, 0, sizeof(request));
  for (i = 0; i < PIDP_GPIO_V2_LINES; ++i)
    request.offsets[i] = mapping->offsets[i];
  request.num_lines = PIDP_GPIO_V2_LINES;
  error = initial_config(&request.config);
  if (error < 0) {
    (void)call_close(&selected_ops, context, chip_fd);
    return fail_return(-error);
  }

  if (call_ioctl(&selected_ops, context, chip_fd,
      GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
    error = errno;
    (void)call_close(&selected_ops, context, chip_fd);
    return fail_return(error);
  }
  request_fd = request.fd;
  if (request_fd < 0) {
    error = EIO;
    (void)call_close(&selected_ops, context, chip_fd);
    return fail_return(error);
  }

  if (call_close(&selected_ops, context, chip_fd) < 0) {
    error = errno;
    (void)call_close(&selected_ops, context, request_fd);
    return fail_return(error);
  }

  backend->ops = selected_ops;
  backend->context = context;
  backend->request_fd = request_fd;
  backend->mode = PIDP_GPIO_V2_MODE_IDLE;
  memcpy(backend->offsets, mapping->offsets, sizeof(backend->offsets));
  return 0;
}

int pidp_gpio_v2_display(struct pidp_gpio_v2 *backend,
    unsigned int row, uint32_t led_bits)
{
  struct gpio_v2_line_config config;
  __u64 row_mask;
  int error;

  if (backend == NULL)
    return fail_return(EINVAL);
  if (row >= PIDP_GPIO_V2_LED_ROWS)
    return fail_live(backend, EINVAL);
  if ((led_bits & ~PIDP_GPIO_V2_LED_MASK) != 0)
    return fail_live(backend, EINVAL);
  if (blank_for_transition(backend) < 0) {
    error = errno;
    return fail_live(backend, error);
  }
  if (backend->mode == PIDP_GPIO_V2_MODE_SWITCH
      && idle_request(backend) < 0) {
    error = errno;
    return fail_live(backend, error);
  }
  error = display_config(&config, led_bits);
  if (error < 0)
    return fail_live(backend, -error);
  if (apply_config(backend, &config) < 0) {
    error = errno;
    return fail_live(backend, error);
  }

  row_mask = UINT64_C(1) << row;
  if (set_values(backend, row_mask, PIDP_GPIO_V2_LED_REQUEST_MASK) < 0) {
    error = errno;
    return fail_live(backend, error);
  }
  backend->mode = PIDP_GPIO_V2_MODE_DISPLAY;
  return 0;
}

int pidp_gpio_v2_select_switch(struct pidp_gpio_v2 *backend,
    unsigned int row)
{
  struct gpio_v2_line_config config;
  int error;

  if (backend == NULL)
    return fail_return(EINVAL);
  if (row >= PIDP_GPIO_V2_SWITCH_ROWS)
    return fail_live(backend, EINVAL);
  if (backend->mode == PIDP_GPIO_V2_MODE_CLOSED)
    return fail_return(EBADF);
  if (blank_for_transition(backend) < 0) {
    error = errno;
    return fail_live(backend, error);
  }
  if (backend->mode == PIDP_GPIO_V2_MODE_SWITCH
      && idle_request(backend) < 0) {
    error = errno;
    return fail_live(backend, error);
  }
  error = switch_config(&config, row);
  if (error < 0)
    return fail_live(backend, -error);
  if (apply_config(backend, &config) < 0) {
    error = errno;
    return fail_live(backend, error);
  }
  backend->mode = PIDP_GPIO_V2_MODE_SWITCH;
  return 0;
}

int pidp_gpio_v2_select_switch_columns(struct pidp_gpio_v2 *backend,
    unsigned int row, uint32_t column_mask)
{
  struct gpio_v2_line_config config;
  int error;

  if (backend == NULL)
    return fail_return(EINVAL);
  if (row >= PIDP_GPIO_V2_SWITCH_ROWS || column_mask == 0
      || (column_mask & ~PIDP_GPIO_V2_LED_MASK) != 0)
    return fail_live(backend, EINVAL);
  if (column_mask == PIDP_GPIO_V2_LED_MASK)
    return pidp_gpio_v2_select_switch(backend, row);

  /* release driven columns and any selected row before changing subset bias. */
  if (pidp_gpio_v2_idle(backend) < 0)
    return -1;
  error = switch_columns_config(&config, row, column_mask);
  if (error < 0)
    return fail_live(backend, -error);
  if (apply_config(backend, &config) < 0) {
    error = errno;
    return fail_live(backend, error);
  }
  backend->mode = PIDP_GPIO_V2_MODE_SWITCH;
  return 0;
}

int pidp_gpio_v2_read_switches(struct pidp_gpio_v2 *backend,
    uint32_t *physical_bits)
{
  struct gpio_v2_line_values values;
  int error;

  if (backend == NULL || physical_bits == NULL)
    return backend != NULL ? fail_live(backend, EINVAL) : fail_return(EINVAL);
  if (backend->mode != PIDP_GPIO_V2_MODE_SWITCH)
    return fail_live(backend, EBADF);

  memset(&values, 0, sizeof(values));
  values.mask = PIDP_GPIO_V2_COL_REQUEST_MASK;
  if (call_ioctl(&backend->ops, backend->context, backend->request_fd,
      GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0) {
    error = errno;
    return fail_live(backend, error);
  }
  *physical_bits = (uint32_t)((values.bits & PIDP_GPIO_V2_COL_REQUEST_MASK)
      >> PIDP_GPIO_V2_LED_ROWS);
  return 0;
}

int pidp_gpio_v2_blank(struct pidp_gpio_v2 *backend)
{
  int error;

  if (backend == NULL)
    return fail_return(EINVAL);
  if (backend->mode == PIDP_GPIO_V2_MODE_CLOSED)
    return fail_return(EBADF);
  if (blank_for_transition(backend) < 0) {
    error = errno;
    return fail_live(backend, error);
  }
  return 0;
}

int pidp_gpio_v2_idle(struct pidp_gpio_v2 *backend)
{
  int error;

  if (backend == NULL)
    return fail_return(EINVAL);
  if (backend->mode == PIDP_GPIO_V2_MODE_CLOSED)
    return fail_return(EBADF);
  if (blank_for_transition(backend) < 0) {
    error = errno;
    return fail_live(backend, error);
  }
  if (idle_request(backend) < 0) {
    error = errno;
    return fail_live(backend, error);
  }
  backend->mode = PIDP_GPIO_V2_MODE_IDLE;
  return 0;
}

int pidp_gpio_v2_close(struct pidp_gpio_v2 *backend)
{
  int first_error = 0;
  int error;
  int fd;

  if (backend == NULL)
    return fail_return(EINVAL);
  if (backend->mode == PIDP_GPIO_V2_MODE_CLOSED)
    return 0;

  if (blank_for_transition(backend) < 0) {
    first_error = errno;
  }
  if (idle_request(backend) < 0 && first_error == 0) {
    first_error = errno;
  }

  fd = backend->request_fd;
  if (call_close(&backend->ops, backend->context, fd) < 0 && first_error == 0) {
    error = errno;
    first_error = error;
  }
  clear_backend(backend);
  if (first_error != 0)
    return fail_return(first_error);
  return 0;
}
