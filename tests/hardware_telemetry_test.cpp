#include "hardware_telemetry.hpp"
#include "hardware_power_button.hpp"

#include <cassert>
#include <cstring>

int main() {
	using namespace audiox::system;

	ThrottleFlags undervoltage;
	ThrottleFlags throttling;
	ThrottleFlags softTemperatureLimit;
	parseFirmwareThrottleFlags(0, &undervoltage, &throttling, &softTemperatureLimit);
	assert(undervoltage.status == TelemetryStatus::Available);
	assert(undervoltage.current == 0 && undervoltage.sinceBoot == 0);
	assert(throttling.current == 0 && throttling.sinceBoot == 0);
	assert(softTemperatureLimit.current == 0 && softTemperatureLimit.sinceBoot == 0);

	parseFirmwareThrottleFlags((1U << 0) | (1U << 16) | (1U << 2) | (1U << 18) |
								   (1U << 3) | (1U << 19),
							   &undervoltage, &throttling, &softTemperatureLimit);
	assert(undervoltage.current == 1 && undervoltage.sinceBoot == 1);
	assert(throttling.current == 1 && throttling.sinceBoot == 1);
	assert(softTemperatureLimit.current == 1 && softTemperatureLimit.sinceBoot == 1);
	parseFirmwareThrottleFlags((1U << 2) | (1U << 19), &undervoltage,
							   &throttling, &softTemperatureLimit);
	assert(undervoltage.current == 0 && undervoltage.sinceBoot == 0);
	assert(throttling.current == 1 && throttling.sinceBoot == 0);
	assert(softTemperatureLimit.current == 0 && softTemperatureLimit.sinceBoot == 1);

	assert(!fanCoolingRequested(0, 0));
	assert(fanCoolingRequested(32, 0));
	assert(fanCoolingRequested(0, 1));
	assert(!fanStallWarning(0, false, 100, 20000));
	assert(!fanStallWarning(1200, true, 100, 20000));
	assert(!fanStallWarning(0, true, 100, 15099));
	assert(fanStallWarning(0, true, 100, 15100));
	assert(!fanStallWarning(0, true, 200, 100));

	const uint8_t deviceTreeCells[] = {
		0x00, 0x00, 0xc3, 0x50,
		0x00, 0x00, 0x00, 0x4b
	};
	int decoded[2]{};
	assert(decodeDeviceTreeCells(deviceTreeCells, sizeof(deviceTreeCells),
								 0, 150000, decoded, 2));
	assert(decoded[0] == 50000 && decoded[1] == 75);
	assert(!decodeDeviceTreeCells(deviceTreeCells, sizeof(deviceTreeCells) - 1,
								  0, 150000, decoded, 2));
	assert(classifyFanModuleStatus(true, true) == TelemetryStatus::Available);
	assert(classifyFanModuleStatus(true, false) == TelemetryStatus::Missing);
	assert(classifyFanModuleStatus(false, false) == TelemetryStatus::Unavailable);
	assert(classifyFanDeviceStatus(true, false) == TelemetryStatus::Missing);
	assert(classifyFanDeviceStatus(true, true) == TelemetryStatus::Available);

	const char config[] =
		"# AudioX Pi 5 fan curve: begin\n[all]\n"
		"dtparam=fan_temp0=45000\ndtparam=fan_temp0_hyst=5000\ndtparam=fan_temp0_speed=50\n"
		"dtparam=fan_temp1=55000\ndtparam=fan_temp1_hyst=5000\ndtparam=fan_temp1_speed=100\n"
		"dtparam=fan_temp2=65000\ndtparam=fan_temp2_hyst=5000\ndtparam=fan_temp2_speed=150\n"
		"dtparam=fan_temp3=75000\ndtparam=fan_temp3_hyst=5000\ndtparam=fan_temp3_speed=250\n"
		"# AudioX Pi 5 fan curve: end\n";
	FanCurve curve;
	assert(parseManagedFanCurve(config, strlen(config), &curve));
	assert(curve.status == TelemetryStatus::Available && curve.enabled);
	assert(curve.temperaturesC[0] == 45 && curve.temperaturesC[3] == 75);
	assert(curve.temperaturesMilliC[0] == 45000 && curve.temperaturesMilliC[3] == 75000);
	assert(curve.speedsPwm[0] == 50 && curve.speedsPwm[3] == 250);
	assert(curve.hysteresisC == 5);
	assert(curve.hysteresisMilliC == 5000);
	FanCurve active = curve;
	active.status = TelemetryStatus::Available;
	assert(fanCurvesMatchSavedActive(curve, active));
	++active.temperaturesMilliC[1];
	assert(!fanCurvesMatchSavedActive(curve, active));
	--active.temperaturesMilliC[1];

	const char invalidConfig[] =
		"# AudioX Pi 5 fan curve: begin\n[all]\n"
		"dtparam=fan_temp0=45000\n# AudioX Pi 5 fan curve: end\n";
	assert(!parseManagedFanCurve(invalidConfig, strlen(invalidConfig), &curve));
	assert(curve.status == TelemetryStatus::Unavailable);
	const char unmanagedConfig[] = "[all]\ndtparam=audio=on\n";
	assert(parseManagedFanCurve(unmanagedConfig, strlen(unmanagedConfig), &curve));
	assert(curve.status == TelemetryStatus::Missing);

	HardwareTelemetry unavailable;
	unavailable.undervoltage.status = TelemetryStatus::Unavailable;
	unavailable.fan.moduleStatus = TelemetryStatus::Available;
	unavailable.fan.deviceStatus = TelemetryStatus::Missing;
	unavailable.fan.driverStatus = TelemetryStatus::Missing;
	const std::string json = hardwareTelemetryJson(unavailable);
	assert(json.find("\"status\":\"unavailable\",\"current\":null,\"since_boot\":null") != std::string::npos);
	assert(json.find("\"driver_status\":\"missing\"") != std::string::npos);
	assert(json.find("\"module_status\":\"available\",\"device_status\":\"missing\"") != std::string::npos);
	assert(json.find("\"rpm\":null") != std::string::npos);
	assert(json.find("\"saved_matches_active\":null") != std::string::npos);
	assert(json.find("\"thermal_throttling\":{\"status\":\"unavailable\",\"current\":null,\"since_boot\":null}") != std::string::npos);

	HardwareTelemetry genericThrottle;
	parseFirmwareThrottleFlags(1U << 2, &genericThrottle.undervoltage,
							   &genericThrottle.throttling,
							   &genericThrottle.softTemperatureLimit);
	const std::string throttleJson = hardwareTelemetryJson(genericThrottle);
	assert(throttleJson.find("\"throttling\":{\"status\":\"available\",\"current\":true,\"since_boot\":false}") != std::string::npos);
	assert(throttleJson.find("\"soft_temperature_limit\":{\"status\":\"available\",\"current\":false,\"since_boot\":false}") != std::string::npos);
	assert(throttleJson.find("\"thermal_throttling\":{\"status\":\"available\",\"current\":true,\"since_boot\":false}") != std::string::npos);

	HardwareTelemetry matching;
	matching.fan.savedCurve = active;
	matching.fan.activeCurve = active;
	matching.fan.savedMatchesActive = 1;
	const std::string matchingJson = hardwareTelemetryJson(matching);
	assert(matchingJson.find("\"saved_matches_active\":true") != std::string::npos);
	assert(matchingJson.find("\"active_curve\":{\"status\":\"available\"") != std::string::npos);
	assert(matchingJson.find("\"temperatures_c\":[45.000,55.000,65.000,75.000]") != std::string::npos);

	input_event event{};
	event.type = EV_KEY;
	event.code = KEY_POWER;
	event.value = 1;
	assert(PowerButton::isPowerButtonPress(event));
	event.value = 0;
	assert(!PowerButton::isPowerButtonPress(event));
	event.value = 2;
	assert(!PowerButton::isPowerButtonPress(event));
	event.value = 1;
	event.code = KEY_SLEEP;
	assert(!PowerButton::isPowerButtonPress(event));
	PowerButton disabled(false);
	assert(!disabled.pollPressed());
}
