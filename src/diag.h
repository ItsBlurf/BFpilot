/*
 * BFpilot - runtime diagnostics and append logging.
 */

#pragma once

#include <stddef.h>

#define BFPILOT_DIAG_SKIPPED (-2147483000)

void bfpilot_diag_init(void);
void bfpilot_diag_install_signal_handlers(void);

void bfpilot_checkpoint(const char *checkpoint);
void bfpilot_log(const char *fmt, ...);

void bfpilot_diag_set_service_rcs(int netctl_rc, int user_service_rc);
void bfpilot_diag_set_notification_rc(int rc);
void bfpilot_diag_set_launcher_status(const char *status, int rc);
void bfpilot_diag_set_bind_rc(int rc);
void bfpilot_diag_set_listen_rc(int rc);
void bfpilot_diag_mark_first_http(const char *method, const char *path);
void bfpilot_diag_set_last_errno(int err);
void bfpilot_diag_set_log_udp_target(const char *ip, unsigned short port);

long bfpilot_diag_uptime(void);
int bfpilot_diag_last_errno(void);
int bfpilot_diag_netctl_rc(void);
int bfpilot_diag_user_service_rc(void);
int bfpilot_diag_notification_rc(void);
int bfpilot_diag_bind_rc(void);
int bfpilot_diag_listen_rc(void);
void bfpilot_diag_get_launcher_status(char *out, size_t out_size);
int bfpilot_diag_launcher_rc(void);
void bfpilot_diag_get_checkpoint(char *out, size_t out_size);

void bfpilot_diag_get_cwd(char *out, size_t out_size);
int bfpilot_diag_can_stat_root(void);
int bfpilot_diag_can_opendir_data(void);
int bfpilot_diag_can_opendir_data_homebrew(void);
int bfpilot_diag_can_opendir_usb0(void);
int bfpilot_diag_can_opendir_ext0(void);
int bfpilot_diag_can_opendir_ext0_homebrew(void);
int bfpilot_diag_can_write_data_bfpilot(void);
int bfpilot_diag_can_write_user_app(void);
