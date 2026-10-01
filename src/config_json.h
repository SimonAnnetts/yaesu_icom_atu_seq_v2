#pragma once

#include <ArduinoJson.h>
#include <stddef.h>

#include "sequencer.h"

// Turns a parsed config document (see config/sequencer.json) into a
// SequencerConfig. Checks structure and types, then runs configValidate. On
// failure returns false with a short human-readable reason in err.
//
// Strict where it matters: schema_version must be 1, all four bands and all
// their fields must be present with the right types. Unknown keys are ignored,
// and cross_band_triggers may be omitted (no rules).
bool configFromJson(JsonVariantConst root, SequencerConfig &out, char *err, size_t errLen);
