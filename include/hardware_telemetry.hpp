#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace audiox::system {

enum class TelemetryStatus {
	Available,
	Unsupported,
	Unavailable,
	Missing
};

struct ThrottleFlags {
	TelemetryStatus status{TelemetryStatus::Unavailable};
	int current{-1};
	int sinceBoot{-1};
};

struct FanCurve {
	TelemetryStatus status{TelemetryStatus::Unavailable};
	bool enabled{false};
	int temperaturesC[4]{};
	int temperaturesMilliC[4]{};
	int speedsPwm[4]{};
	int hysteresisC{0};
	int hysteresisMilliC{0};
};

struct FanTelemetry {
	TelemetryStatus moduleStatus{TelemetryStatus::Unavailable};
	TelemetryStatus deviceStatus{TelemetryStatus::Unavailable};
	TelemetryStatus driverStatus{TelemetryStatus::Unavailable};
	TelemetryStatus rpmStatus{TelemetryStatus::Unavailable};
	int rpm{-1};
	TelemetryStatus coolingStatus{TelemetryStatus::Unavailable};
	int coolingState{-1};
	int coolingMaxState{-1};
	TelemetryStatus pwmStatus{TelemetryStatus::Unavailable};
	int pwm{-1};
	int coolingRequested{-1};
	TelemetryStatus stallStatus{TelemetryStatus::Unavailable};
	int stallWarning{-1};
	FanCurve savedCurve{};
	FanCurve activeCurve{};
	int savedMatchesActive{-1};
};

struct HardwareTelemetry {
	ThrottleFlags undervoltage{};
	ThrottleFlags throttling{};
	ThrottleFlags softTemperatureLimit{};
	FanTelemetry fan{};
};

const char *telemetryStatusName(TelemetryStatus status);
TelemetryStatus classifyFanModuleStatus(bool lookupSucceeded, bool loaded);
TelemetryStatus classifyFanDeviceStatus(bool sysfsScanned, bool boundDeviceFound);
bool decodeDeviceTreeCells(const uint8_t *bytes, size_t byteCount, int minimum,
						   int maximum, int *values, size_t valueCount);
bool fanCurvesMatchSavedActive(const FanCurve &saved, const FanCurve &active);
void parseFirmwareThrottleFlags(uint32_t flags, ThrottleFlags *undervoltage,
								ThrottleFlags *throttling,
								ThrottleFlags *softTemperatureLimit);
bool fanCoolingRequested(int pwm, int coolingState);
bool fanStallWarning(int rpm, bool coolingRequested, uint64_t requestedSinceMs,
					 uint64_t nowMs);
bool parseManagedFanCurve(const char *config, size_t length, FanCurve *curve);
std::string hardwareTelemetryJson(const HardwareTelemetry &telemetry);
// Returns a complete JSON object with explicit statuses and null unavailable values.
std::string hardwareTelemetryJson();
HardwareTelemetry readHardwareTelemetry();

} // namespace audiox::system
