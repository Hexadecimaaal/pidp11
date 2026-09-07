#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USE_NETWORK
#include "../src/02.3_simh/4.x+realcons/src/sim_ether.c"

/* use libc diagnostics, not the simulator's console wrapper. */
#undef fprintf

struct guarded_command {
  unsigned char before[16];
  char command[1024];
  unsigned char after[16];
};

static int failures;

static void expect(int condition, const char *description)
{
  if (!condition) {
    fprintf(stderr, "ether_bounds: %s\n", description);
    ++failures;
    }
}

static void guard_init(struct guarded_command *guard)
{
  memset(guard, 0xA5, sizeof(*guard));
  }

static int guard_intact(const struct guarded_command *guard)
{
  unsigned char expected[16];

  memset(expected, 0xA5, sizeof(expected));
  return (memcmp(guard->before, expected, sizeof(expected)) == 0)
      && (memcmp(guard->after, expected, sizeof(expected)) == 0);
  }

int main(void)
{
  const char *pattern = "grep MAC";
  struct guarded_command guard;
  char oversized[16896];
  char expected[1024];
  char quote_heavy[300];
  size_t required;

  memset(quote_heavy, '\'', sizeof(quote_heavy) - 1);
  quote_heavy[sizeof(quote_heavy) - 1] = '\0';

  guard_init(&guard);
  expect(eth_build_nic_hw_addr_command(guard.command, sizeof(guard.command),
                                       "eth0", pattern),
         "ordinary interface name is accepted");
  expect(strcmp(guard.command, "ifconfig 'eth0' | grep MAC  >NIC.hwaddr") == 0,
         "ordinary interface command is complete");
  expect(guard_intact(&guard), "ordinary command stays within its buffer");

  guard_init(&guard);
  expect(eth_build_nic_hw_addr_command(guard.command, sizeof(guard.command),
                                       "eth;touch /tmp/ether_bounds", pattern),
         "shell metacharacters are accepted as data");
  expect(strcmp(guard.command,
                "ifconfig 'eth;touch /tmp/ether_bounds' | grep MAC  >NIC.hwaddr") == 0,
         "shell metacharacters remain inside single quotes");
  expect(guard_intact(&guard), "metacharacter command stays within its buffer");

  guard_init(&guard);
  expect(eth_build_nic_hw_addr_command(guard.command, sizeof(guard.command),
                                       "eth'0", pattern),
         "apostrophe interface name is accepted");
  expect(strcmp(guard.command,
                "ifconfig 'eth'\\''0' | grep MAC  >NIC.hwaddr") == 0,
         "apostrophe interface name is shell-escaped");
  expect(guard_intact(&guard), "apostrophe command stays within its buffer");

  guard_init(&guard);
  expect(!eth_build_nic_hw_addr_command(guard.command, sizeof(guard.command),
                                        quote_heavy, pattern),
         "escaped apostrophes are included in the capacity check");
  expect(guard_intact(&guard), "escaped apostrophe rejection stays within its buffer");

  guard_init(&guard);
  expect(!eth_build_nic_hw_addr_command(guard.command, sizeof(guard.command),
                                        "", pattern),
         "empty interface name is rejected");
  expect(guard_intact(&guard), "empty interface rejection does not write");

  guard_init(&guard);
  expect(!eth_build_nic_hw_addr_command(guard.command, sizeof(guard.command),
                                        "-a", pattern),
         "option-like interface name is rejected");
  expect(guard_intact(&guard), "option-like interface rejection does not write");

  guard_init(&guard);
  expect(!eth_build_nic_hw_addr_command(guard.command, sizeof(guard.command),
                                        NULL, pattern),
         "null interface name is rejected");
  expect(guard_intact(&guard), "null interface rejection does not write");

  guard_init(&guard);
  expect(eth_build_nic_hw_addr_command(expected, sizeof(expected), "eth0", pattern),
         "capacity fixture command is built");
  required = strlen(expected) + 1;
  guard_init(&guard);
  expect(eth_build_nic_hw_addr_command(guard.command, required, "eth0", pattern),
         "exact command capacity is accepted");
  expect(strcmp(guard.command, expected) == 0,
         "exact-capacity command matches the complete command");
  expect(guard_intact(&guard), "exact-capacity command stays within its buffer");

  guard_init(&guard);
  expect(!eth_build_nic_hw_addr_command(guard.command, required - 1, "eth0", pattern),
         "one byte below command capacity is rejected");
  expect(guard_intact(&guard), "short command rejection does not overflow canaries");

  memset(oversized, 'x', sizeof(oversized) - 1);
  oversized[sizeof(oversized) - 1] = '\0';
  guard_init(&guard);
  expect(!eth_build_nic_hw_addr_command(guard.command, sizeof(guard.command),
                                        oversized, pattern),
         "oversized interface name is rejected without truncation");
  expect(guard_intact(&guard), "oversized interface rejection stays within its buffer");

  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
