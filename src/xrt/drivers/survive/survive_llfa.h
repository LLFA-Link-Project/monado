// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "util/u_json.h"
#include <string.h>

// Only LLFA callers use this classification. Recognize the downloaded arpara Gaming Kit profile,
// or an ordinary Tundra. Other models require an explicit serial.
// This reads a host-side copy: never modifies device firmware or its calibration.
static inline bool
survive_llfa_tracker_config(const char *json, const char *selected_serial, char *out_serial, size_t out_size)
{
	cJSON *root = cJSON_Parse(json);
	cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model_number");
	if (!cJSON_IsString(model)) {
		model = cJSON_GetObjectItemCaseSensitive(root, "model_name");
	}
	cJSON *serial = cJSON_GetObjectItemCaseSensitive(root, "device_serial_number");
	cJSON *vendor = cJSON_GetObjectItemCaseSensitive(root, "device_vid");
	cJSON *product = cJSON_GetObjectItemCaseSensitive(root, "device_pid");
	cJSON *device_class = cJSON_GetObjectItemCaseSensitive(root, "device_class");
	cJSON *manufacturer = cJSON_GetObjectItemCaseSensitive(root, "manufacturer");
	bool arpara_module = cJSON_IsString(model) && strcmp(model->valuestring, "arpara") == 0 &&
	                     cJSON_IsString(manufacturer) && strcmp(manufacturer->valuestring, "arpara") == 0 &&
	                     cJSON_IsString(device_class) && strcmp(device_class->valuestring, "hmd") == 0 &&
	                     cJSON_IsNumber(vendor) && vendor->valuedouble == 0x28de && cJSON_IsNumber(product) &&
	                     product->valuedouble == 0x2300;
	bool selected =
	    cJSON_IsObject(root) && cJSON_IsString(serial) && serial->valuestring[0] != '\0' &&
	    (arpara_module || (cJSON_IsString(model) && strcmp(model->valuestring, "Tundra Tracker") == 0) ||
	     (selected_serial != NULL && selected_serial[0] != '\0' &&
	      strcmp(serial->valuestring, selected_serial) == 0));
	if (selected && strlen(serial->valuestring) < out_size) {
		memcpy(out_serial, serial->valuestring, strlen(serial->valuestring) + 1);
	} else {
		selected = false;
	}
	cJSON_Delete(root);
	return selected;
}
