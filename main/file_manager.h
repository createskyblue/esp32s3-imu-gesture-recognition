#pragma once

#include "esp_http_server.h"

/**
 * Optional application policy hook for delete and upload operations.
 * Return NULL to allow the mutation, otherwise return a short error message.
 */
typedef const char *(*file_manager_mutation_guard_t)(const char *fs_type,
                                                      const char *resolved_path);

void file_manager_set_mutation_guard(file_manager_mutation_guard_t guard);

/**
 * Register file-manager HTTP handlers.
 * Registers the /files page and one POST /api/fs action endpoint.
 */
esp_err_t file_manager_register(httpd_handle_t server);
