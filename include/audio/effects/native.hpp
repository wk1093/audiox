#pragma once

#include <cstdint>

namespace audiox::effects {

constexpr unsigned kBuiltinDspCount = 11;
// Generated entries process the engine's fixed block size; parameters and
// caller-owned state remain live across code replacement.
using DspEntry = void (*)(const float *, float *, uint32_t, const float *, void *, const void *);
struct DspTable {
    DspEntry entries[kBuiltinDspCount]{};
};

// The audio thread selects a table whose lifetime is owned by NativeGraph.
void setCompiledDsp(const DspTable *);
void *compiledStateFor(const char *, uint8_t, uint8_t, bool create = true);

} // namespace audiox::effects
