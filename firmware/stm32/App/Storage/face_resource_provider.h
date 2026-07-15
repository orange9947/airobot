#ifndef AIROBOT_FACE_RESOURCE_PROVIDER_H
#define AIROBOT_FACE_RESOURCE_PROVIDER_H

#include <stdbool.h>
#include <stdint.h>

#include "resource_format.h"

typedef enum {
    FACE_RESOURCE_PROVIDER_OK = 0,
    FACE_RESOURCE_PROVIDER_FALLBACK,
    FACE_RESOURCE_PROVIDER_INVALID_ARGUMENT,
    FACE_RESOURCE_PROVIDER_UNHEALTHY,
    FACE_RESOURCE_PROVIDER_READ_ERROR,
    FACE_RESOURCE_PROVIDER_DIRECTORY_ERROR,
    FACE_RESOURCE_PROVIDER_DECODE_ERROR,
} face_resource_provider_status_t;

typedef struct {
    resource_package_t package;
    resource_clip_t clip_cache[RESOURCE_MAX_CLIPS];
    resource_clip_t current_clip;
    uint32_t random_state;
    uint32_t next_frame_ms;
    uint16_t current_clip_index;
    uint16_t frame_in_clip;
    uint16_t loaded_clip_count;
    uint8_t expression;
    bool initialized;
    bool healthy;
    bool directory_ready;
    bool has_selection;
    resource_format_status_t last_format_status;
} face_resource_provider_t;

/* verified_package must have passed resource_package_verify(). */
face_resource_provider_status_t face_resource_provider_init(
    face_resource_provider_t *provider,
    const resource_package_t *verified_package, uint32_t random_seed);
face_resource_provider_status_t face_resource_provider_set_expression(
    face_resource_provider_t *provider, uint8_t expression, uint32_t now_ms);
/* Loads at most one clip record or advances at most one frame per call. */
face_resource_provider_status_t face_resource_provider_tick(
    face_resource_provider_t *provider, uint32_t now_ms, bool *frame_changed);
/* The caller must use output only when this function returns OK. */
face_resource_provider_status_t face_resource_provider_decode_current(
    face_resource_provider_t *provider, uint8_t *output,
    uint32_t output_capacity);
bool face_resource_provider_is_healthy(
    const face_resource_provider_t *provider);
bool face_resource_provider_has_frame(
    const face_resource_provider_t *provider);

#endif
