/*
 * BFpilot - file-manager API.
 */

#pragma once

#include <stddef.h>

#include "websrv.h"


int transfer_request(const http_request_t *req, const char *url);

int transfer_upload_request(const http_request_t *req, const char *initial_data,
                            size_t initial_size, size_t content_size);

int transfer_text_save_request(const http_request_t *req,
                               const char *initial_data,
                               size_t initial_size, size_t content_size);

int transfer_text_create_request(const http_request_t *req,
                                 const char *initial_data,
                                 size_t initial_size, size_t content_size);

int transfer_action_post_request(const http_request_t *req, const char *url,
                                 const char *initial_data,
                                 size_t initial_size, size_t content_size);

int transfer_archive_prepare_request(const http_request_t *req,
                                     const char *initial_data,
                                     size_t initial_size,
                                     size_t content_size);

int transfer_download_prepare_request(const http_request_t *req,
                                      const char *initial_data,
                                      size_t initial_size,
                                      size_t content_size);

int transfer_pkg_install_request(const http_request_t *req,
                                 const char *initial_data,
                                 size_t initial_size,
                                 size_t content_size);

int transfer_archive_busy(void);

/* True while a copy/move/delete/download/archive job must not be interrupted. */
int transfer_job_busy(void);
