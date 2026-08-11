/*
 * BFpilot - tiny HTTP server helpers.
 */

#pragma once

#include <stddef.h>


typedef struct http_request {
  int  fd;
  int  keep_alive; /* 1 = prefer Connection: keep-alive for this response */
  char method[8];
  char path[1024];
  char query[2048];
  char action[64];
} http_request_t;

typedef void (*websrv_ready_cb_t)(unsigned short port, void *arg);

typedef struct bfpilot_launcher_diag {
  int launcher_enabled;
  int launcher_attempted;
  int appinst_init_rc;
  int title_dir_resolved;
  int install_title_dir_resolved;
  int install_title_rc;
  int uninstall_resolved;
  int uninstall_rc;
  int install_all_resolved;
  int install_all_rc;
  int user_app_writable;
  int launcher_install_rc;
  char launcher_final_state[32];
} bfpilot_launcher_diag_t;


int websrv_write_all(int fd, const void *data, size_t size);

/* Thread-local: set before send_headers so responses can keep the TCP session. */
void websrv_conn_set_keep_alive(int on);
int  websrv_conn_get_keep_alive(void);

int websrv_send_headers(int fd, int status, const char *mime,
                        size_t size, const char *extra);

int websrv_send_chunked_headers(int fd, int status, const char *mime,
                                const char *extra);

int websrv_write_chunk(int fd, const void *data, size_t size);

int websrv_write_chunk_end(int fd);

int websrv_send(int fd, int status, const char *mime,
                const void *data, size_t size);

int websrv_send_text(int fd, int status, const char *mime, const char *body);

int websrv_send_error_json(int fd, int status, const char *message);

int websrv_get_query_arg(const http_request_t *req, const char *name,
                         char *out, size_t out_size);

void websrv_url_decode(char *out, size_t out_size, const char *in);

int websrv_listen(unsigned short port, websrv_ready_cb_t ready_cb,
                  void *ready_arg);

void websrv_set_launcher_diag(const bfpilot_launcher_diag_t *diag);

void websrv_set_runtime_port(unsigned short port);

void websrv_request_exit(void);

int websrv_exit_requested(void);
