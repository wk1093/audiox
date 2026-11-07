#include "hardware_telemetry.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace audiox::system {
namespace {

constexpr const char *kCompatiblePath = "/proc/device-tree/compatible";
constexpr const char *kMailboxPath = "/dev/vcio";
constexpr const char *kHwmonPath = "/sys/class/hwmon";
constexpr const char *kCoolingPath = "/sys/class/thermal";
constexpr const char *kBootConfigPath = "/boot/config.txt";
constexpr const char *kDeviceTreePath = "/sys/firmware/devicetree/base";
constexpr uint32_t kFirmwareGetThrottled = 0x00030046;
constexpr uint32_t kFirmwareResponse = 0x80000000U;
constexpr uint64_t kFanStallDurationMs = 15000;
constexpr uint64_t kFanSampleGapMs = 5000;

struct BoardInfo {
	bool readable{false};
	bool raspberryPi{false};
	bool pi5{false};
};

struct FanStallTracker {
	bool coolingRequested{false};
	uint64_t requestedSinceMs{0};
	uint64_t lastSampleMs{0};

	int update(bool rpmAvailable, int rpm, bool requestAvailable,
			   bool request, uint64_t nowMs) {
		if (!rpmAvailable || !requestAvailable || !request || rpm != 0) {
			coolingRequested = false;
			requestedSinceMs = 0;
			lastSampleMs = 0;
			return requestAvailable && rpmAvailable ? 0 : -1;
		}
		if (coolingRequested &&
			(nowMs < lastSampleMs || nowMs - lastSampleMs > kFanSampleGapMs)) {
			coolingRequested = false;
		}
		if (!coolingRequested) {
			coolingRequested = true;
			requestedSinceMs = nowMs;
		}
		lastSampleMs = nowMs;
		return fanStallWarning(rpm, true, requestedSinceMs, nowMs) ? 1 : 0;
	}
};

uint64_t monotonicMs() {
	struct timespec ts{};
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

bool readSmallFile(const char *path, char *out, size_t outSize, size_t *length = nullptr) {
	if (!path || !out || outSize < 2) return false;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return false;
	size_t used = 0;
	while (used < outSize - 1) {
		const size_t remaining = outSize - 1 - used;
		ssize_t count = read(fd, out + used, remaining);
		if (count < 0 && errno == EINTR) continue;
		if (count < 0) {
			close(fd);
			return false;
		}
		if (count == 0) break;
		if ((size_t)count > remaining) {
			close(fd);
			return false;
		}
		used += (size_t)count;
	}
	char extra = 0;
	ssize_t extraCount = 0;
	if (used == outSize - 1) {
		do {
			extraCount = read(fd, &extra, 1);
		} while (extraCount < 0 && errno == EINTR);
	}
	close(fd);
	if (extraCount != 0 || used >= outSize) return false;
	out[used] = '\0';
	if (length) *length = used;
	return used > 0;
}

const char *findBytes(const char *text, size_t textLength,
					  const char *needle, size_t needleLength) {
	if (!text || !needle || needleLength == 0 || textLength < needleLength) return nullptr;
	for (size_t i = 0; i <= textLength - needleLength; ++i) {
		if (memcmp(text + i, needle, needleLength) == 0) return text + i;
	}
	return nullptr;
}

bool parseIntegerFile(const char *path, int minimum, int maximum, int *value) {
	char text[64];
	if (!value || !readSmallFile(path, text, sizeof(text))) return false;
	char *end = nullptr;
	errno = 0;
	long parsed = strtol(text, &end, 10);
	while (end && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) ++end;
	if (errno || !end || *end || parsed < minimum || parsed > maximum) return false;
	*value = (int)parsed;
	return true;
}

BoardInfo readBoardInfo() {
	BoardInfo board;
	char compatible[1024];
	size_t compatibleLength = 0;
	if (!readSmallFile(kCompatiblePath, compatible, sizeof(compatible), &compatibleLength)) return board;
	board.readable = true;
	for (size_t i = 0; i < compatibleLength; ++i) {
		if (compatible[i] == '\0') compatible[i] = '\n';
	}
	board.raspberryPi = strstr(compatible, "raspberrypi") != nullptr ||
		strstr(compatible, "brcm,bcm") != nullptr;
	board.pi5 = strstr(compatible, "brcm,bcm2712") != nullptr;
	return board;
}

bool readFirmwareThrottleFlags(uint32_t *flags) {
	if (!flags) return false;
	const int fd = open(kMailboxPath, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		static bool warned = false;
		if (!warned) printf("[SYSTEM] [WARN] open firmware mailbox: %s\n", strerror(errno));
		warned = true;
		return false;
	}
	alignas(16) uint32_t message[7] = {
		sizeof(message), 0, kFirmwareGetThrottled, sizeof(uint32_t), 0, 0, 0
	};
	const unsigned long command = _IOWR(100, 0, char *);
	const int result = ioctl(fd, command, message);
	const int error = errno;
	close(fd);
	if (result < 0 || message[1] != kFirmwareResponse ||
		message[2] != kFirmwareGetThrottled ||
		message[4] != (kFirmwareResponse | sizeof(uint32_t))) {
		static bool warned = false;
		if (!warned) printf("[SYSTEM] [WARN] firmware throttling query failed: %s\n",
						   result < 0 ? strerror(error) : "invalid mailbox response");
		warned = true;
		return false;
	}
	*flags = message[5];
	return true;
}

void readThrottleTelemetry(const BoardInfo &board, HardwareTelemetry *telemetry) {
	if (!telemetry) return;
	if (!board.readable) return;
	if (!board.raspberryPi) {
		telemetry->undervoltage.status = TelemetryStatus::Unsupported;
		telemetry->throttling.status = TelemetryStatus::Unsupported;
		telemetry->softTemperatureLimit.status = TelemetryStatus::Unsupported;
		return;
	}
	uint32_t flags = 0;
	if (!readFirmwareThrottleFlags(&flags)) return;
	parseFirmwareThrottleFlags(flags, &telemetry->undervoltage,
							   &telemetry->throttling,
							   &telemetry->softTemperatureLimit);
}

bool isFanHwmonName(const char *name) {
	return name && (strcmp(name, "pwmfan") == 0 || strcmp(name, "pwm-fan") == 0);
}

bool isFanCoolingType(const char *type) {
	return type && (strcmp(type, "pwm-fan") == 0 || strcmp(type, "pwmfan") == 0);
}

bool readFanHwmon(FanTelemetry *fan, bool *scanned) {
	if (!fan || !scanned) return false;
	DIR *directory = opendir(kHwmonPath);
	if (!directory) return false;
	*scanned = true;
	bool found = false;
	struct dirent *entry = nullptr;
	while ((entry = readdir(directory)) != nullptr) {
		if (strncmp(entry->d_name, "hwmon", 5) != 0) continue;
		char path[512];
		char name[64];
		snprintf(path, sizeof(path), "%s/%s/name", kHwmonPath, entry->d_name);
		if (!readSmallFile(path, name, sizeof(name))) continue;
		name[strcspn(name, "\r\n")] = '\0';
		if (!isFanHwmonName(name)) continue;
		found = true;
		int value = 0;
		snprintf(path, sizeof(path), "%s/%s/fan1_input", kHwmonPath, entry->d_name);
		if (parseIntegerFile(path, 0, 100000, &value)) {
			fan->rpmStatus = TelemetryStatus::Available;
			fan->rpm = value;
		} else {
			fan->rpmStatus = TelemetryStatus::Unsupported;
		}
		snprintf(path, sizeof(path), "%s/%s/pwm1", kHwmonPath, entry->d_name);
		if (parseIntegerFile(path, 0, 255, &value)) {
			fan->pwmStatus = TelemetryStatus::Available;
			fan->pwm = value;
		} else {
			fan->pwmStatus = TelemetryStatus::Unsupported;
		}
		break;
	}
	closedir(directory);
	if (found) fan->driverStatus = TelemetryStatus::Available;
	return found;
}

bool readFanCooling(FanTelemetry *fan, bool *scanned) {
	if (!fan || !scanned) return false;
	DIR *directory = opendir(kCoolingPath);
	if (!directory) return false;
	*scanned = true;
	bool found = false;
	struct dirent *entry = nullptr;
	while ((entry = readdir(directory)) != nullptr) {
		if (strncmp(entry->d_name, "cooling_device", 14) != 0) continue;
		char path[512];
		char type[64];
		snprintf(path, sizeof(path), "%s/%s/type", kCoolingPath, entry->d_name);
		if (!readSmallFile(path, type, sizeof(type))) continue;
		type[strcspn(type, "\r\n")] = '\0';
		if (!isFanCoolingType(type)) continue;
		found = true;
		int value = 0;
		snprintf(path, sizeof(path), "%s/%s/cur_state", kCoolingPath, entry->d_name);
		if (parseIntegerFile(path, 0, 65535, &value)) {
			fan->coolingState = value;
			fan->coolingStatus = TelemetryStatus::Available;
		}
		snprintf(path, sizeof(path), "%s/%s/max_state", kCoolingPath, entry->d_name);
		if (parseIntegerFile(path, 0, 65535, &value)) fan->coolingMaxState = value;
		break;
	}
	closedir(directory);
	if (found && fan->coolingStatus != TelemetryStatus::Available)
		fan->coolingStatus = TelemetryStatus::Unavailable;
	if (found) fan->driverStatus = TelemetryStatus::Available;
	return found;
}

void readSavedFanCurve(const BoardInfo &board, FanCurve *curve) {
	if (!curve) return;
	if (!board.readable) return;
	if (!board.pi5) {
		curve->status = TelemetryStatus::Unsupported;
		return;
	}
	char config[8192];
	size_t length = 0;
	if (!readSmallFile(kBootConfigPath, config, sizeof(config), &length)) return;
	if (!parseManagedFanCurve(config, length, curve))
		curve->status = strstr(config, "# AudioX Pi 5 fan curve: begin")
			? TelemetryStatus::Unavailable : TelemetryStatus::Missing;
}

bool readDeviceTreeCells(const char *path, int minimum, int maximum,
						 int *values, size_t valueCount) {
	if (!path || !values || valueCount == 0 || valueCount > 16) return false;
	unsigned char bytes[64];
	const size_t byteCount = valueCount * 4;
	const int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return false;
	size_t offset = 0;
	while (offset < byteCount) {
		const size_t remaining = byteCount - offset;
		if (offset >= sizeof(bytes) || remaining > sizeof(bytes) - offset) {
			close(fd);
			return false;
		}
		ssize_t count = read(fd, bytes + offset, remaining);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0 || (size_t)count > remaining) {
			close(fd);
			return false;
		}
		offset += (size_t)count;
	}
	unsigned char extra = 0;
	ssize_t extraCount = read(fd, &extra, 1);
	close(fd);
	if (extraCount != 0) return false;
	return decodeDeviceTreeCells(bytes, byteCount, minimum, maximum,
								 values, valueCount);
}

bool readDeviceTreeCell(const char *path, int minimum, int maximum, int *value) {
	return readDeviceTreeCells(path, minimum, maximum, value, 1);
}

bool readDeviceTreeText(const char *path, char *out, size_t outSize) {
	return readSmallFile(path, out, outSize);
}

void readActiveFanCurve(const BoardInfo &board, const FanTelemetry &fan,
						FanCurve *curve) {
	if (!curve) return;
	if (!board.readable) return;
	if (!board.pi5) {
		curve->status = TelemetryStatus::Unsupported;
		return;
	}
	if (fan.driverStatus != TelemetryStatus::Available) {
		curve->status = fan.driverStatus == TelemetryStatus::Missing
			? TelemetryStatus::Missing : TelemetryStatus::Unavailable;
		return;
	}
	char path[384];
	char status[32];
	snprintf(path, sizeof(path), "%s/cooling_fan/status", kDeviceTreePath);
	if (!readDeviceTreeText(path, status, sizeof(status))) return;
	status[strcspn(status, "\r\n")] = '\0';
	if (strcmp(status, "okay") != 0 && strcmp(status, "ok") != 0) {
		curve->status = TelemetryStatus::Missing;
		return;
	}
	int value = 0;
	snprintf(path, sizeof(path), "%s/cooling_fan/cooling-levels", kDeviceTreePath);
	int levels[5]{};
	if (!readDeviceTreeCells(path, 0, 255, levels, 5)) return;
	for (int i = 0; i < 4; ++i) {
		char tripName[32];
		snprintf(tripName, sizeof(tripName), "cpu-%s", i == 0 ? "tepid" :
				 i == 1 ? "warm" : i == 2 ? "hot" : "vhot");
		char temperaturePath[384];
		char hysteresisPath[384];
		snprintf(temperaturePath, sizeof(temperaturePath),
				 "%s/thermal-zones/cpu-thermal/trips/%s/temperature",
				 kDeviceTreePath, tripName);
		snprintf(hysteresisPath, sizeof(hysteresisPath),
				 "%s/thermal-zones/cpu-thermal/trips/%s/hysteresis",
				 kDeviceTreePath, tripName);
		if (!readDeviceTreeCell(temperaturePath, 0, 150000, &value)) return;
		curve->temperaturesMilliC[i] = value;
		curve->temperaturesC[i] = value / 1000;
		if (!readDeviceTreeCell(hysteresisPath, 0, 30000, &value)) return;
		if (i == 0) curve->hysteresisMilliC = value;
		else if (curve->hysteresisMilliC != value) return;
		curve->speedsPwm[i] = levels[i + 1];
	}
	curve->hysteresisC = curve->hysteresisMilliC / 1000;
	curve->enabled = true;
	curve->status = TelemetryStatus::Available;
}

int readFanTelemetry(const BoardInfo &board, FanTelemetry *fan) {
	if (!fan) return 0;
	if (!board.readable) return 0;
	if (!board.pi5) {
		fan->moduleStatus = TelemetryStatus::Unsupported;
		fan->deviceStatus = TelemetryStatus::Unsupported;
		fan->driverStatus = TelemetryStatus::Unsupported;
		fan->rpmStatus = TelemetryStatus::Unsupported;
		fan->coolingStatus = TelemetryStatus::Unsupported;
		fan->pwmStatus = TelemetryStatus::Unsupported;
		fan->stallStatus = TelemetryStatus::Unsupported;
		fan->savedCurve.status = TelemetryStatus::Unsupported;
		fan->activeCurve.status = TelemetryStatus::Unsupported;
		return 0;
	}
	struct stat moduleInfo{};
	if (stat("/sys/module/pwm_fan", &moduleInfo) == 0) {
		fan->moduleStatus = TelemetryStatus::Available;
	} else {
		fan->moduleStatus = classifyFanModuleStatus(errno == ENOENT, false);
	}
	bool hwmonScanned = false;
	bool coolingScanned = false;
	bool hwmonFound = readFanHwmon(fan, &hwmonScanned);
	bool coolingFound = readFanCooling(fan, &coolingScanned);
	const bool boundDeviceFound = hwmonFound || coolingFound;
	fan->deviceStatus = classifyFanDeviceStatus(hwmonScanned || coolingScanned,
												boundDeviceFound);
	fan->driverStatus = fan->deviceStatus;
	if (!hwmonFound) {
		fan->rpmStatus = fan->driverStatus == TelemetryStatus::Missing
			? TelemetryStatus::Missing : TelemetryStatus::Unavailable;
		fan->pwmStatus = fan->rpmStatus;
	}
	if (!coolingFound && fan->coolingStatus != TelemetryStatus::Available) {
		fan->coolingStatus = coolingScanned
			? TelemetryStatus::Missing : TelemetryStatus::Unavailable;
	}
	const bool requestAvailable = fan->pwmStatus == TelemetryStatus::Available ||
		fan->coolingStatus == TelemetryStatus::Available;
	const bool request = fanCoolingRequested(
		fan->pwmStatus == TelemetryStatus::Available ? fan->pwm : -1,
		fan->coolingStatus == TelemetryStatus::Available ? fan->coolingState : -1);
	fan->coolingRequested = requestAvailable ? request : -1;
	if (fan->rpmStatus == TelemetryStatus::Available && requestAvailable) {
		static FanStallTracker tracker;
		fan->stallStatus = TelemetryStatus::Available;
		fan->stallWarning = tracker.update(
			true, fan->rpm, true, request, monotonicMs());
	} else {
		fan->stallStatus = fan->rpmStatus == TelemetryStatus::Available
			? TelemetryStatus::Unavailable : fan->rpmStatus;
		fan->stallWarning = -1;
	}
	readSavedFanCurve(board, &fan->savedCurve);
	readActiveFanCurve(board, *fan, &fan->activeCurve);
	if (fan->savedCurve.status == TelemetryStatus::Available &&
		fan->activeCurve.status == TelemetryStatus::Available) {
		fan->savedMatchesActive = fanCurvesMatchSavedActive(fan->savedCurve, fan->activeCurve);
	}
	return 0;
}

const char *jsonBool(int value) {
	if (value < 0) return "null";
	return value ? "true" : "false";
}

std::string curveJson(const FanCurve &curve) {
	char out[256];
	if (curve.status != TelemetryStatus::Available) {
		snprintf(out, sizeof(out),
				 "{\"status\":\"%s\",\"enabled\":null,\"temperatures_c\":null,"
				 "\"speeds_pwm\":null,\"hysteresis_c\":null}",
				 telemetryStatusName(curve.status));
		return out;
	}
	snprintf(out, sizeof(out),
			 "{\"status\":\"available\",\"enabled\":%s,"
			 "\"temperatures_c\":[%.3f,%.3f,%.3f,%.3f],\"speeds_pwm\":[%d,%d,%d,%d],"
			 "\"hysteresis_c\":%.3f}",
			 curve.enabled ? "true" : "false",
			 curve.temperaturesMilliC[0] / 1000.0, curve.temperaturesMilliC[1] / 1000.0,
			 curve.temperaturesMilliC[2] / 1000.0, curve.temperaturesMilliC[3] / 1000.0,
			 curve.speedsPwm[0], curve.speedsPwm[1],
			 curve.speedsPwm[2], curve.speedsPwm[3],
			 curve.hysteresisMilliC / 1000.0);
	return out;
}

} // namespace

const char *telemetryStatusName(TelemetryStatus status) {
	switch (status) {
	case TelemetryStatus::Available: return "available";
	case TelemetryStatus::Unsupported: return "unsupported";
	case TelemetryStatus::Missing: return "missing";
	case TelemetryStatus::Unavailable: return "unavailable";
	}
	return "unavailable";
}

TelemetryStatus classifyFanModuleStatus(bool lookupSucceeded, bool loaded) {
	if (!lookupSucceeded) return TelemetryStatus::Unavailable;
	return loaded ? TelemetryStatus::Available : TelemetryStatus::Missing;
}

TelemetryStatus classifyFanDeviceStatus(bool sysfsScanned, bool boundDeviceFound) {
	if (boundDeviceFound) return TelemetryStatus::Available;
	return sysfsScanned ? TelemetryStatus::Missing : TelemetryStatus::Unavailable;
}

bool decodeDeviceTreeCells(const uint8_t *bytes, size_t byteCount, int minimum,
						   int maximum, int *values, size_t valueCount) {
	if (!bytes || !values || valueCount == 0 || valueCount > 16 ||
		byteCount != valueCount * 4 || minimum < 0 || maximum < minimum) return false;
	for (size_t i = 0; i < valueCount; ++i) {
		const uint32_t parsed = ((uint32_t)bytes[i * 4] << 24) |
			((uint32_t)bytes[i * 4 + 1] << 16) |
			((uint32_t)bytes[i * 4 + 2] << 8) | bytes[i * 4 + 3];
		if (parsed < (uint32_t)minimum || parsed > (uint32_t)maximum) return false;
		values[i] = (int)parsed;
	}
	return true;
}

bool fanCurvesMatchSavedActive(const FanCurve &saved, const FanCurve &active) {
	if (saved.status != TelemetryStatus::Available ||
		active.status != TelemetryStatus::Available ||
		saved.hysteresisMilliC != active.hysteresisMilliC) return false;
	for (int i = 0; i < 4; ++i) {
		if (saved.temperaturesMilliC[i] != active.temperaturesMilliC[i] ||
			saved.speedsPwm[i] != active.speedsPwm[i]) return false;
	}
	return true;
}

void parseFirmwareThrottleFlags(uint32_t flags, ThrottleFlags *undervoltage,
								ThrottleFlags *throttling,
								ThrottleFlags *softTemperatureLimit) {
	if (undervoltage) {
		undervoltage->status = TelemetryStatus::Available;
		undervoltage->current = (flags & (1U << 0)) != 0;
		undervoltage->sinceBoot = (flags & (1U << 16)) != 0;
	}
	if (throttling) {
		throttling->status = TelemetryStatus::Available;
		throttling->current = (flags & (1U << 2)) != 0;
		throttling->sinceBoot = (flags & (1U << 18)) != 0;
	}
	if (softTemperatureLimit) {
		softTemperatureLimit->status = TelemetryStatus::Available;
		softTemperatureLimit->current = (flags & (1U << 3)) != 0;
		softTemperatureLimit->sinceBoot = (flags & (1U << 19)) != 0;
	}
}

bool fanCoolingRequested(int pwm, int coolingState) {
	return pwm > 0 || coolingState > 0;
}

bool fanStallWarning(int rpm, bool coolingRequested, uint64_t requestedSinceMs,
					 uint64_t nowMs) {
	return coolingRequested && rpm == 0 && nowMs >= requestedSinceMs &&
		nowMs - requestedSinceMs >= kFanStallDurationMs;
}

bool parseManagedFanCurve(const char *config, size_t length, FanCurve *curve) {
	static const char begin[] = "# AudioX Pi 5 fan curve: begin";
	static const char end[] = "# AudioX Pi 5 fan curve: end";
	if (!config || !curve) return false;
	const char *block = findBytes(config, length, begin, sizeof(begin) - 1);
	if (!block) {
		curve->status = TelemetryStatus::Missing;
		return true;
	}
	const size_t afterBegin = (size_t)(block - config) + sizeof(begin) - 1;
	const char *blockEnd = findBytes(config + afterBegin, length - afterBegin,
									 end, sizeof(end) - 1);
	if (!blockEnd) {
		curve->status = TelemetryStatus::Unavailable;
		return false;
	}
	const size_t afterEnd = (size_t)(blockEnd - config) + sizeof(end) - 1;
	if (findBytes(config + afterEnd, length - afterEnd, begin, sizeof(begin) - 1)) {
		curve->status = TelemetryStatus::Unavailable;
		return false;
	}
	bool haveTemperature[4]{};
	bool haveSpeed[4]{};
	bool haveHysteresis[4]{};
	int hysteresis[4]{};
	const size_t blockLength = (size_t)(blockEnd - block);
	const char *line = (const char *)memchr(block, '\n', blockLength);
	if (!line) {
		curve->status = TelemetryStatus::Unavailable;
		return false;
	}
	++line;
	while (line < blockEnd) {
		const size_t remaining = (size_t)(blockEnd - line);
		const char *lineEnd = (const char *)memchr(line, '\n', remaining);
		if (!lineEnd) lineEnd = blockEnd;
		char entry[96];
		const size_t entryLength = (size_t)(lineEnd - line);
		if (entryLength >= sizeof(entry)) {
			curve->status = TelemetryStatus::Unavailable;
			return false;
		}
		memcpy(entry, line, entryLength);
		entry[entryLength] = '\0';
		if (strcmp(entry, "[all]") != 0) {
			int index = -1;
			long value = 0;
			int consumed = 0;
			if (sscanf(entry, "dtparam=fan_temp%d=%ld%n", &index, &value, &consumed) == 2 &&
				entry[consumed] == '\0' && index >= 0 && index < 4 &&
				value >= 35000 && value <= 80000) {
				curve->temperaturesC[index] = (int)(value / 1000);
				curve->temperaturesMilliC[index] = (int)value;
				haveTemperature[index] = true;
			} else if (sscanf(entry, "dtparam=fan_temp%d_hyst=%ld%n", &index, &value, &consumed) == 2 &&
					   entry[consumed] == '\0' && index >= 0 && index < 4 &&
					   value >= 1000 && value <= 15000) {
				hysteresis[index] = (int)(value / 1000);
				haveHysteresis[index] = true;
			} else if (sscanf(entry, "dtparam=fan_temp%d_speed=%ld%n", &index, &value, &consumed) == 2 &&
					   entry[consumed] == '\0' && index >= 0 && index < 4 &&
					   value >= 1 && value <= 255) {
				curve->speedsPwm[index] = (int)value;
				haveSpeed[index] = true;
			} else {
				curve->status = TelemetryStatus::Unavailable;
				return false;
			}
		}
		line = lineEnd + (lineEnd < blockEnd ? 1 : 0);
	}
	for (int i = 0; i < 4; ++i) {
		if (!haveTemperature[i] || !haveSpeed[i] || !haveHysteresis[i] ||
			(i > 0 && (curve->temperaturesC[i] <= curve->temperaturesC[i - 1] ||
					   curve->speedsPwm[i] <= curve->speedsPwm[i - 1] ||
					   curve->temperaturesC[i] - curve->temperaturesC[i - 1] <= hysteresis[0])) ||
			(i > 0 && hysteresis[i] != hysteresis[0])) {
			curve->status = TelemetryStatus::Unavailable;
			return false;
		}
	}
	curve->hysteresisC = hysteresis[0];
	curve->hysteresisMilliC = hysteresis[0] * 1000;
	curve->enabled = true;
	curve->status = TelemetryStatus::Available;
	return true;
}

std::string hardwareTelemetryJson(const HardwareTelemetry &telemetry) {
	const ThrottleFlags combinedThrottle = {
		telemetry.throttling.status == telemetry.softTemperatureLimit.status
			? telemetry.throttling.status : TelemetryStatus::Unavailable,
		telemetry.throttling.current < 0 || telemetry.softTemperatureLimit.current < 0
			? -1 : (telemetry.throttling.current || telemetry.softTemperatureLimit.current),
		telemetry.throttling.sinceBoot < 0 || telemetry.softTemperatureLimit.sinceBoot < 0
			? -1 : (telemetry.throttling.sinceBoot || telemetry.softTemperatureLimit.sinceBoot)
	};
	char flags[512];
	snprintf(flags, sizeof(flags),
			 "\"undervoltage\":{\"status\":\"%s\",\"current\":%s,\"since_boot\":%s},"
			 "\"throttling\":{\"status\":\"%s\",\"current\":%s,\"since_boot\":%s},"
			 "\"soft_temperature_limit\":{\"status\":\"%s\",\"current\":%s,\"since_boot\":%s},"
			 "\"thermal_throttling\":{\"status\":\"%s\",\"current\":%s,\"since_boot\":%s}",
			 telemetryStatusName(telemetry.undervoltage.status),
			 jsonBool(telemetry.undervoltage.current),
			 jsonBool(telemetry.undervoltage.sinceBoot),
			 telemetryStatusName(telemetry.throttling.status),
			 jsonBool(telemetry.throttling.current),
			 jsonBool(telemetry.throttling.sinceBoot),
			 telemetryStatusName(telemetry.softTemperatureLimit.status),
			 jsonBool(telemetry.softTemperatureLimit.current),
			 jsonBool(telemetry.softTemperatureLimit.sinceBoot),
			 telemetryStatusName(combinedThrottle.status),
			 jsonBool(combinedThrottle.current),
			 jsonBool(combinedThrottle.sinceBoot));
	char fanFields[2048];
	snprintf(fanFields, sizeof(fanFields),
			 "\"module_status\":\"%s\",\"device_status\":\"%s\",\"driver_status\":\"%s\","
			 "\"rpm_status\":\"%s\",\"rpm\":%s,"
			 "\"cooling_status\":\"%s\",\"cooling_state\":%s,\"cooling_max_state\":%s,"
			 "\"pwm_status\":\"%s\",\"pwm\":%s,\"cooling_requested\":%s,"
			 "\"stall_status\":\"%s\",\"stall_warning\":%s,"
			 "\"saved_curve\":%s,\"active_curve\":%s,"
			 "\"saved_matches_active\":%s",
			 telemetryStatusName(telemetry.fan.moduleStatus),
			 telemetryStatusName(telemetry.fan.deviceStatus),
			 telemetryStatusName(telemetry.fan.driverStatus),
			 telemetryStatusName(telemetry.fan.rpmStatus),
			 telemetry.fan.rpm < 0 ? "null" : std::to_string(telemetry.fan.rpm).c_str(),
			 telemetryStatusName(telemetry.fan.coolingStatus),
			 telemetry.fan.coolingState < 0 ? "null" : std::to_string(telemetry.fan.coolingState).c_str(),
			 telemetry.fan.coolingMaxState < 0 ? "null" : std::to_string(telemetry.fan.coolingMaxState).c_str(),
			 telemetryStatusName(telemetry.fan.pwmStatus),
			 telemetry.fan.pwm < 0 ? "null" : std::to_string(telemetry.fan.pwm).c_str(),
			 jsonBool(telemetry.fan.coolingRequested),
			 telemetryStatusName(telemetry.fan.stallStatus),
			 jsonBool(telemetry.fan.stallWarning),
			 curveJson(telemetry.fan.savedCurve).c_str(),
			 curveJson(telemetry.fan.activeCurve).c_str(),
			 jsonBool(telemetry.fan.savedMatchesActive));
	return std::string("{") + flags + ",\"fan\":{" + fanFields + "}}";
}

HardwareTelemetry readHardwareTelemetry() {
	HardwareTelemetry telemetry;
	const BoardInfo board = readBoardInfo();
	readThrottleTelemetry(board, &telemetry);
	readFanTelemetry(board, &telemetry.fan);
	return telemetry;
}

std::string hardwareTelemetryJson() {
	return hardwareTelemetryJson(readHardwareTelemetry());
}

} // namespace audiox::system
