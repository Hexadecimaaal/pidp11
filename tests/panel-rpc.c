#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <rpc/rpc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "rpc_blinkenlight_api.h"

#ifdef PIDP_PANEL_RPC_PRELOAD
/* bypass rpcbind without replacing the real RPC protocol or simulator. */

CLIENT *clnt_create(const char *hostname, const rpcprog_t program, const rpcvers_t version,
                    const char *protocol) {
  const char *port_text;
  char *end;
  unsigned long port;
  int socket_fd = RPC_ANYSOCK;
  struct sockaddr_in address;
  struct timeval timeout;

  (void)hostname;
  (void)protocol;
  port_text = getenv("PIDP_PANEL_RPC_PORT");
  if (port_text == NULL || *port_text == '\0')
    return NULL;
  errno = 0;
  port = strtoul(port_text, &end, 10);
  if (errno != 0 || *end != '\0' || port == 0 || port > 65535)
    return NULL;

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons((unsigned short)port);
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;
  return clntudp_create(&address, program, version, timeout, &socket_fd);
}

#else

#define INPUT_CONTROL_COUNT 12
#define OUTPUT_CONTROL_COUNT 14
#define CONTROL_COUNT (INPUT_CONTROL_COUNT + OUTPUT_CONTROL_COUNT)

static const char *control_names[CONTROL_COUNT] = {
    "POWER",         "PANEL_LOCK",    "SR",          "LOAD_ADRS",  "EXAM",        "DEPOSIT",
    "CONT",          "HALT",          "S_BUS_CYCLE", "START",      "DATA_SELECT", "ADDR_SELECT",
    "ADDRESS",       "DATA",          "PARITY_HIGH", "PARITY_LOW", "PAR_ERR",     "ADRS_ERR",
    "RUN",           "PAUSE",         "MASTER",      "MMR0_MODE",  "DATA_SPACE",  "ADDRESSING_16",
    "ADDRESSING_18", "ADDRESSING_22",
};

static u_char input_values[INPUT_CONTROL_COUNT] = {
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static rpc_blinkenlight_api_getinfo_res getinfo_result;
static rpc_blinkenlight_api_getpanelinfo_res getpanelinfo_result;
static rpc_blinkenlight_api_getcontrolinfo_res getcontrolinfo_result;
static rpc_blinkenlight_api_setpanel_controlvalues_res setvalues_result;
static rpc_blinkenlight_api_controlvalues_struct getvalues_result;
static rpc_param_result_struct param_result;
static rpc_test_cmdstatus_struct test_status_result;
static rpc_test_data_struct test_data_result;

rpc_blinkenlight_api_getinfo_res *rpc_blinkenlight_api_getinfo_1_svc(struct svc_req *request) {
  (void)request;
  getinfo_result.error_code = RPC_ERR_OK;
  getinfo_result.info = (char *)"panel shutdown fixture";
  return &getinfo_result;
}

rpc_blinkenlight_api_getpanelinfo_res *
rpc_blinkenlight_api_getpanelinfo_1_svc(u_int panel, struct svc_req *request) {
  (void)request;
  memset(&getpanelinfo_result, 0, sizeof(getpanelinfo_result));
  getpanelinfo_result.panel.name = (char *)"";
  if (panel != 0) {
    getpanelinfo_result.error_code = RPC_ERR_PARAM_ILL_OBJECT;
    return &getpanelinfo_result;
  }
  getpanelinfo_result.panel.name = (char *)"11/70";
  getpanelinfo_result.panel.controls_inputs_count = INPUT_CONTROL_COUNT;
  getpanelinfo_result.panel.controls_outputs_count = OUTPUT_CONTROL_COUNT;
  getpanelinfo_result.panel.controls_inputs_values_bytecount = INPUT_CONTROL_COUNT;
  getpanelinfo_result.panel.controls_outputs_values_bytecount = OUTPUT_CONTROL_COUNT;
  return &getpanelinfo_result;
}

rpc_blinkenlight_api_getcontrolinfo_res *
rpc_blinkenlight_api_getcontrolinfo_1_svc(u_int panel, u_int control, struct svc_req *request) {
  (void)request;
  memset(&getcontrolinfo_result, 0, sizeof(getcontrolinfo_result));
  getcontrolinfo_result.control.name = (char *)"";
  if (panel != 0 || control >= CONTROL_COUNT) {
    getcontrolinfo_result.error_code = RPC_ERR_PARAM_ILL_OBJECT;
    return &getcontrolinfo_result;
  }
  getcontrolinfo_result.control.name = (char *)control_names[control];
  getcontrolinfo_result.control.is_input = control < INPUT_CONTROL_COUNT;
  getcontrolinfo_result.control.type = getcontrolinfo_result.control.is_input
                                           ? rpc_blinkenlight_api_input_switch
                                           : rpc_blinkenlight_api_output_lamp;
  getcontrolinfo_result.control.radix = 16;
  getcontrolinfo_result.control.value_bitlen = 8;
  getcontrolinfo_result.control.value_bytelen = 1;
  return &getcontrolinfo_result;
}

rpc_blinkenlight_api_setpanel_controlvalues_res *rpc_blinkenlight_api_setpanel_controlvalues_1_svc(
    u_int panel, rpc_blinkenlight_api_controlvalues_struct values, struct svc_req *request) {
  static int running_reported;
  (void)panel;
  (void)request;
  if (!running_reported && values.value_bytes.value_bytes_len == OUTPUT_CONTROL_COUNT &&
      values.value_bytes.value_bytes_val[6]) {
    running_reported = 1;
    puts("PANEL_CPU_RUNNING");
    fflush(stdout);
  }
  setvalues_result.error_code = RPC_ERR_OK;
  return &setvalues_result;
}

rpc_blinkenlight_api_controlvalues_struct *
rpc_blinkenlight_api_getpanel_controlvalues_1_svc(u_int panel, struct svc_req *request) {
  (void)request;
  memset(&getvalues_result, 0, sizeof(getvalues_result));
  if (panel != 0) {
    getvalues_result.error_code = RPC_ERR_PARAM_ILL_OBJECT;
    return &getvalues_result;
  }
  getvalues_result.error_code = RPC_ERR_OK;
  getvalues_result.value_bytes.value_bytes_len = INPUT_CONTROL_COUNT;
  getvalues_result.value_bytes.value_bytes_val = input_values;
  return &getvalues_result;
}

rpc_param_result_struct *rpc_param_get_1_svc(rpc_param_cmd_get_struct command,
                                             struct svc_req *request) {
  (void)request;
  memset(&param_result, 0, sizeof(param_result));
  param_result.error_code = RPC_ERR_OK;
  param_result.object_class = command.object_class;
  param_result.object_handle = command.object_handle;
  param_result.param_handle = command.param_handle;
  param_result.param_value = RPC_PARAM_VALUE_PANEL_BLINKENBOARDS_STATE_ACTIVE;
  return &param_result;
}

rpc_param_result_struct *rpc_param_set_1_svc(rpc_param_cmd_set_struct command,
                                             struct svc_req *request) {
  (void)request;
  memset(&param_result, 0, sizeof(param_result));
  param_result.error_code = RPC_ERR_OK;
  param_result.object_class = command.object_class;
  param_result.object_handle = command.object_handle;
  param_result.param_handle = command.param_handle;
  param_result.param_value = command.param_value;
  return &param_result;
}

rpc_test_cmdstatus_struct *rpc_test_data_to_server_1_svc(rpc_test_data_struct data,
                                                         struct svc_req *request) {
  (void)data;
  (void)request;
  test_status_result.bytecount = 0;
  return &test_status_result;
}

rpc_test_data_struct *rpc_test_data_from_server_1_svc(rpc_test_cmdstatus_struct status,
                                                      struct svc_req *request) {
  (void)status;
  (void)request;
  memset(&test_data_result, 0, sizeof(test_data_result));
  return &test_data_result;
}

extern void blinkenlightd_1(struct svc_req *request, SVCXPRT *transport);

int main(int argc, char **argv) {
  SVCXPRT *transport;
  int socket_fd;
  int terminal_fd;
  struct sockaddr_in address;
  socklen_t address_length;
  FILE *port_file;

  if (argc != 3) {
    fprintf(stderr, "usage: %s port-file tty-file\n", argv[0]);
    return 2;
  }
  /* the service provides a pty; /dev/null would make a CPU halt exit on EOF. */
  terminal_fd = posix_openpt(O_RDWR | O_NOCTTY);
  if (terminal_fd < 0 || grantpt(terminal_fd) < 0 || unlockpt(terminal_fd) < 0) {
    perror("create panel test terminal");
    return 1;
  }
  port_file = fopen(argv[2], "w");
  if (port_file == NULL || ptsname(terminal_fd) == NULL) {
    perror("terminal-file");
    return 1;
  }
  fprintf(port_file, "%s\n", ptsname(terminal_fd));
  fclose(port_file);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd < 0 || bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("bind loopback RPC socket");
    return 1;
  }
  transport = svcudp_create(socket_fd);
  if (transport == NULL) {
    fprintf(stderr, "svcudp_create failed\n");
    return 1;
  }
  if (!svc_register(transport, BLINKENLIGHTD, BLINKENLIGHTD_VERS, blinkenlightd_1, 0)) {
    fprintf(stderr, "svc_register failed\n");
    return 1;
  }
  address_length = sizeof(address);
  if (getsockname(transport->xp_sock, (struct sockaddr *)&address, &address_length) < 0) {
    perror("getsockname");
    return 1;
  }
  port_file = fopen(argv[1], "w");
  if (port_file == NULL) {
    perror("port-file");
    return 1;
  }
  fprintf(port_file, "%u\n", (unsigned)ntohs(address.sin_port));
  fclose(port_file);
  svc_run();
  return 1;
}

#endif
