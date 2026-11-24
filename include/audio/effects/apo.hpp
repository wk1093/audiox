#pragma once

#include "audio/effects/advanced_eq.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace audiox::effects {

struct ApoBand {
    uint8_t type = AEQ_PEAK;
    float hz = 1000, gainDb = 0, q = 0.707f;
};

struct ApoImport {
    float preampDb = 0;
    unsigned bandCount = 0;
    ApoBand bands[kAdvancedEqMaxBands]{};
    unsigned skipped = 0;
    std::vector<std::string> warnings;
};

// Parses Equalizer APO / AutoEQ "ParametricEQ.txt" text (Preamp and Filter lines).
// Returns false only when nothing usable was found; unsupported lines become warnings.
bool parseApo(const char *text, size_t length, ApoImport *out);
// Writes Advanced EQ parameter values (slot layout) into the Advanced EQ slot values.
void applyApo(const ApoImport &import, float *values);
std::string exportApo(const float *values);

} // namespace audiox::effects
