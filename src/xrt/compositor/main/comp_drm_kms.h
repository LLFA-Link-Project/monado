// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct comp_drm_kms;
/* No display change in open, list, prepare, or test. Connector and mode must
 * be explicitly selected for anything beyond enumeration. */
struct comp_drm_kms *
comp_drm_kms_open(const char *device, FILE *log);
void
comp_drm_kms_list(struct comp_drm_kms *k);
bool
comp_drm_kms_prepare(struct comp_drm_kms *k, const char *connector, const char *mode, uint32_t plane);
bool
comp_drm_kms_test(struct comp_drm_kms *k);
bool
comp_drm_kms_present(struct comp_drm_kms *k, const uint8_t *bgra, uint32_t stride);
void
comp_drm_kms_close(struct comp_drm_kms *k);
uint32_t
comp_drm_kms_width(struct comp_drm_kms *k);
uint32_t
comp_drm_kms_height(struct comp_drm_kms *k);
uint64_t
comp_drm_kms_period_ns(struct comp_drm_kms *k);
