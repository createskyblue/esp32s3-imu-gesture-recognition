#pragma once

#include "esp_http_server.h"

/** Application-owned filesystem locations used by the HTTP file manager. */
typedef struct {
    const char *internal_mount_point;
    const char *internal_partition_label;
    const char *sd_mount_point;
} file_manager_storage_config_t;

/** Copy filesystem locations before registering HTTP handlers. */
esp_err_t file_manager_set_storage_config(
    const file_manager_storage_config_t *config);

/** Optional lease around each complete file-manager HTTP operation. */
typedef esp_err_t (*file_manager_access_begin_t)(void);
typedef void (*file_manager_access_end_t)(void);

void file_manager_set_access_callbacks(file_manager_access_begin_t begin,
                                       file_manager_access_end_t end);

/**
 * Optional application policy hook for listing and download operations.
 * Return NULL to allow access, otherwise return a short error message.
 */
typedef const char *(*file_manager_read_guard_t)(const char *fs_type,
                                                 const char *resolved_path);

void file_manager_set_read_guard(file_manager_read_guard_t guard);

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
