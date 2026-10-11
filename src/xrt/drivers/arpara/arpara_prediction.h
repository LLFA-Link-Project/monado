// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "math/m_api.h"
#include "math/m_relation_history.h"
#include <stdint.h>

/* Receipt time is a host-clock proxy, not synchronized device capture time.
 * Never extrapolate stale samples, past requests or more than the configured
 * limit. The gyro is bias-corrected body-space rad/s, as used by 3DoF fusion. */
static inline int64_t
arpara_prediction_horizon(int64_t receipt, int64_t query, int64_t requested, int64_t limit)
{
	if (limit <= 0 || receipt <= 0 || query < receipt || query - receipt > 20000000 || requested <= query)
		return 0;
	int64_t horizon = requested - receipt;
	return horizon < limit ? horizon : limit;
}

static inline struct xrt_space_relation
arpara_history_sample(const struct xrt_quat *orientation, const struct xrt_vec3 *gyro_body, bool tracked)
{
	struct xrt_space_relation rel = {};
	rel.pose.orientation = *orientation;
	rel.relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                   XRT_SPACE_RELATION_POSITION_VALID_BIT);
	if (tracked) {
		rel.relation_flags = (enum xrt_space_relation_flags)(rel.relation_flags |
		    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
		/* m_predict_relation expects angular velocity in world/tracking space
		 * and transforms it back into body space before quaternion integration. */
		math_quat_rotate_derivative(orientation, gyro_body, &rel.angular_velocity);
	}
	return rel;
}
