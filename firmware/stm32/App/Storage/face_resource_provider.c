#include "face_resource_provider.h"

#include <stddef.h>
#include <string.h>

#define FACE_RESOURCE_DEFAULT_RANDOM_SEED 0xA17E5EEDu

static uint32_t next_random(face_resource_provider_t *provider) {
    uint32_t value = provider->random_state;

    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    provider->random_state = value;
    return value;
}

static bool time_reached(uint32_t now_ms, uint32_t target_ms) {
    return (int32_t)(now_ms - target_ms) >= 0;
}

static bool package_shape_is_valid(const resource_package_t *package) {
    return package != NULL && package->read != NULL &&
           package->header.version == RESOURCE_FORMAT_VERSION &&
           package->header.width == RESOURCE_FRAME_WIDTH &&
           package->header.height == RESOURCE_FRAME_HEIGHT &&
           package->header.clip_count > 0u &&
           package->header.clip_count <= RESOURCE_MAX_CLIPS &&
           package->header.frame_count > 0u &&
           package->header.frame_count <= RESOURCE_MAX_FRAMES &&
           package->header.total_length <= package->source_length;
}

static face_resource_provider_status_t mark_format_failure(
    face_resource_provider_t *provider, resource_format_status_t format_status,
    face_resource_provider_status_t provider_status) {
    provider->last_format_status = format_status;
    provider->healthy = false;
    provider->has_selection = false;
    if (format_status == RESOURCE_FORMAT_READ_FAILED) {
        return FACE_RESOURCE_PROVIDER_READ_ERROR;
    }
    return provider_status;
}

static face_resource_provider_status_t select_cached_clip(
    face_resource_provider_t *provider, uint8_t expression, uint32_t now_ms) {
    uint32_t total_weight = 0u;
    uint32_t selection;
    uint16_t clip_index;

    provider->has_selection = false;
    provider->expression = expression;
    if (!provider->directory_ready) {
        return FACE_RESOURCE_PROVIDER_FALLBACK;
    }
    for (clip_index = 0u; clip_index < provider->loaded_clip_count;
         ++clip_index) {
        const resource_clip_t *clip = &provider->clip_cache[clip_index];

        if (clip->expression_id == expression) {
            total_weight += clip->weight;
        }
    }
    if (total_weight == 0u) {
        provider->last_format_status = RESOURCE_FORMAT_OK;
        return FACE_RESOURCE_PROVIDER_FALLBACK;
    }

    selection = next_random(provider) % total_weight;
    for (clip_index = 0u; clip_index < provider->loaded_clip_count;
         ++clip_index) {
        const resource_clip_t *clip = &provider->clip_cache[clip_index];

        if (clip->expression_id != expression) {
            continue;
        }
        if (selection < clip->weight) {
            provider->current_clip = *clip;
            provider->current_clip_index = clip_index;
            provider->frame_in_clip = 0u;
            provider->next_frame_ms = now_ms + clip->frame_interval_ms;
            provider->has_selection = true;
            provider->last_format_status = RESOURCE_FORMAT_OK;
            return FACE_RESOURCE_PROVIDER_OK;
        }
        selection -= clip->weight;
    }

    return mark_format_failure(provider, RESOURCE_FORMAT_BAD_CLIP,
                               FACE_RESOURCE_PROVIDER_DIRECTORY_ERROR);
}

face_resource_provider_status_t face_resource_provider_init(
    face_resource_provider_t *provider,
    const resource_package_t *verified_package, uint32_t random_seed) {
    if (provider == NULL) {
        return FACE_RESOURCE_PROVIDER_INVALID_ARGUMENT;
    }
    memset(provider, 0, sizeof(*provider));
    if (!package_shape_is_valid(verified_package)) {
        return FACE_RESOURCE_PROVIDER_INVALID_ARGUMENT;
    }

    provider->package = *verified_package;
    provider->random_state = random_seed != 0u
                                 ? random_seed
                                 : FACE_RESOURCE_DEFAULT_RANDOM_SEED;
    provider->initialized = true;
    provider->healthy = true;
    provider->last_format_status = RESOURCE_FORMAT_OK;
    return FACE_RESOURCE_PROVIDER_OK;
}

face_resource_provider_status_t face_resource_provider_set_expression(
    face_resource_provider_t *provider, uint8_t expression, uint32_t now_ms) {
    if (provider == NULL || !provider->initialized ||
        expression > RESOURCE_MAX_EXPRESSION_ID) {
        return FACE_RESOURCE_PROVIDER_INVALID_ARGUMENT;
    }
    if (!provider->healthy) {
        return FACE_RESOURCE_PROVIDER_UNHEALTHY;
    }
    provider->expression = expression;
    provider->has_selection = false;
    if (!provider->directory_ready) {
        provider->last_format_status = RESOURCE_FORMAT_OK;
        return FACE_RESOURCE_PROVIDER_FALLBACK;
    }
    return select_cached_clip(provider, expression, now_ms);
}

face_resource_provider_status_t face_resource_provider_tick(
    face_resource_provider_t *provider, uint32_t now_ms, bool *frame_changed) {
    face_resource_provider_status_t status;

    if (provider == NULL || frame_changed == NULL || !provider->initialized) {
        return FACE_RESOURCE_PROVIDER_INVALID_ARGUMENT;
    }
    *frame_changed = false;
    if (!provider->healthy) {
        return FACE_RESOURCE_PROVIDER_UNHEALTHY;
    }
    if (!provider->directory_ready) {
        resource_clip_t *clip;
        resource_format_status_t format_status;

        clip = &provider->clip_cache[provider->loaded_clip_count];
        format_status = resource_package_read_clip(
            &provider->package, provider->loaded_clip_count, clip);
        if (format_status != RESOURCE_FORMAT_OK) {
            return mark_format_failure(
                provider, format_status,
                FACE_RESOURCE_PROVIDER_DIRECTORY_ERROR);
        }
        ++provider->loaded_clip_count;
        if (provider->loaded_clip_count <
            provider->package.header.clip_count) {
            return FACE_RESOURCE_PROVIDER_FALLBACK;
        }
        provider->directory_ready = true;
        status = select_cached_clip(
            provider, provider->expression, now_ms);
        if (status == FACE_RESOURCE_PROVIDER_OK) {
            *frame_changed = true;
        }
        return status;
    }
    if (!provider->has_selection) {
        return FACE_RESOURCE_PROVIDER_FALLBACK;
    }
    if (!time_reached(now_ms, provider->next_frame_ms)) {
        return FACE_RESOURCE_PROVIDER_OK;
    }

    if ((uint32_t)provider->frame_in_clip + 1u <
        provider->current_clip.frame_count) {
        ++provider->frame_in_clip;
        provider->next_frame_ms =
            now_ms + provider->current_clip.frame_interval_ms;
        *frame_changed = true;
        return FACE_RESOURCE_PROVIDER_OK;
    }

    status = select_cached_clip(provider, provider->expression, now_ms);
    if (status == FACE_RESOURCE_PROVIDER_OK) {
        *frame_changed = true;
    }
    return status;
}

face_resource_provider_status_t face_resource_provider_decode_current(
    face_resource_provider_t *provider, uint8_t *output,
    uint32_t output_capacity) {
    resource_frame_t frame;
    resource_format_status_t format_status;
    uint32_t frame_index;

    if (provider == NULL || output == NULL || !provider->initialized ||
        output_capacity < RESOURCE_DECODED_FRAME_SIZE) {
        return FACE_RESOURCE_PROVIDER_INVALID_ARGUMENT;
    }
    if (!provider->healthy) {
        return FACE_RESOURCE_PROVIDER_UNHEALTHY;
    }
    if (!provider->has_selection) {
        return FACE_RESOURCE_PROVIDER_FALLBACK;
    }

    frame_index = provider->current_clip.first_frame_index +
                  provider->frame_in_clip;
    if (frame_index >= provider->package.header.frame_count) {
        return mark_format_failure(provider, RESOURCE_FORMAT_BAD_CLIP,
                                   FACE_RESOURCE_PROVIDER_DIRECTORY_ERROR);
    }
    format_status = resource_package_read_frame(
        &provider->package, (uint16_t)frame_index, &frame);
    if (format_status != RESOURCE_FORMAT_OK) {
        return mark_format_failure(provider, format_status,
                                   FACE_RESOURCE_PROVIDER_DIRECTORY_ERROR);
    }
    format_status = resource_package_decode_frame(
        &provider->package, &frame, output, output_capacity);
    if (format_status != RESOURCE_FORMAT_OK) {
        return mark_format_failure(provider, format_status,
                                   FACE_RESOURCE_PROVIDER_DECODE_ERROR);
    }
    provider->last_format_status = RESOURCE_FORMAT_OK;
    return FACE_RESOURCE_PROVIDER_OK;
}

bool face_resource_provider_is_healthy(
    const face_resource_provider_t *provider) {
    return provider != NULL && provider->initialized && provider->healthy;
}

bool face_resource_provider_has_frame(
    const face_resource_provider_t *provider) {
    return face_resource_provider_is_healthy(provider) &&
           provider->has_selection;
}
