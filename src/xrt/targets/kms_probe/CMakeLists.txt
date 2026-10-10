# Copyright 2026, OpenAI.
# SPDX-License-Identifier: BSL-1.0
add_executable(monado-kms-probe main.c)
target_link_libraries(monado-kms-probe PRIVATE comp_drm_kms comp_drm_readback comp_drm_dma)
