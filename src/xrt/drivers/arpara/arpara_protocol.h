// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Rust repr(C) ABI. All decoding and fixed outbound messages use the pinned
 * vrg9080-protocol-rs implementation. No outgoing arbitrary-command API. */
struct arpara_rs_decoded
{
	uint32_t kind;
	float values[9];
	uint16_t counter;
	uint8_t command;
	uint8_t status;
	uint8_t ack_requested;
	uint8_t ack[65];
};
bool
arpara_rs_decode(const uint8_t *input, size_t size, struct arpara_rs_decoded *out);
size_t
arpara_rs_startup(size_t index, uint8_t *out_report, uint32_t *out_timeout_ms);
uint64_t
arpara_rs_counter_delta_ns(uint16_t now, uint16_t prev);
#ifdef __cplusplus
}
#endif
