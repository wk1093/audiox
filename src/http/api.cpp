#include "http/context.hpp"
#include "http/ffmpeg.hpp"
#include "http/logs.hpp"
#include "audio/context.hpp"
#include "audio/health.hpp"
#include "audio/stress_test.hpp"
#include "audio/effects/slot.hpp"
#include "bluetooth/context.hpp"
#include "config/context.hpp"
#include "init.hpp"
#include "boot_recovery.hpp"
#include "midi/context.hpp"
#include "scripting/storage.hpp"
#include "json.hpp"
#include "audio/plugin_registry.hpp"
#include <algorithm>

#include <ctype.h>
#include <cmath>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <sys/mount.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#define HTTP_BODY_MAX 49152
#define HTTP_API_PREFIX "/api/"
#define HTTP_ROOTFS_PREFIX HTTP_API_PREFIX "rootfs/"
#define HTTP_SOUNDBOARD_TRIGGER_PREFIX HTTP_API_PREFIX "soundboard/trigger/"
#define HTTP_SOUNDBOARD_PRESS_PREFIX HTTP_API_PREFIX "soundboard/press/"
#define HTTP_SOUNDBOARD_RELEASE_PREFIX HTTP_API_PREFIX "soundboard/release/"
#define HTTP_SOUNDBOARD_STOP_ALL_PATH HTTP_API_PREFIX "soundboard/stop_all"
#define HTTP_SOUNDBOARD_MODE_PATH HTTP_API_PREFIX "soundboard/mode"
#define HTTP_SOUNDBOARD_MODES_PATH HTTP_API_PREFIX "soundboard/modes"
#define HTTP_SOUNDBOARD_ORDER_PATH HTTP_API_PREFIX "soundboard/order"
#define HTTP_SOUNDBOARD_LAYOUT_PATH HTTP_API_PREFIX "soundboard/layout"
#define HTTP_SOUNDBOARD_MODE_SET_PATH HTTP_API_PREFIX "soundboard/mode/set"
#define HTTP_SOUNDBOARD_RELOAD_PATH HTTP_API_PREFIX "soundboard/reload"
#define HTTP_FRAMEBUFFER_VIEW_PATH HTTP_API_PREFIX "framebuffer/view"
#define HTTP_UPDATE_NOTIFICATIONS_PATH HTTP_API_PREFIX "updates/notifications"
#define HTTP_CONFIG_RELOAD_PATH HTTP_API_PREFIX "config/reload"
#define HTTP_ROUTING_RELOAD_PATH HTTP_API_PREFIX "routing/reload"
#define HTTP_ROUTING_THINGS_PATH HTTP_API_PREFIX "routing/things"
#define HTTP_BOOT_CONFIG_PATH HTTP_API_PREFIX "boot/config.txt"
#define HTTP_BOOT_CMDLINE_PATH HTTP_API_PREFIX "boot/cmdline.txt"
#define HTTP_ROUTING_HELPER_CREATE_PATH HTTP_API_PREFIX "routing/helper/create"
#define HTTP_ROUTING_HELPER_DELETE_PATH HTTP_API_PREFIX "routing/helper/delete"
#define HTTP_BLUETOOTH_DEVICES_PATH HTTP_API_PREFIX "bluetooth/devices"
#define HTTP_BLUETOOTH_SCAN_PATH HTTP_API_PREFIX "bluetooth/scan"
#define HTTP_BLUETOOTH_PAIR_PATH HTTP_API_PREFIX "bluetooth/pair"
#define HTTP_BLUETOOTH_CONNECT_PATH HTTP_API_PREFIX "bluetooth/connect"
#define HTTP_BLUETOOTH_DISCONNECT_PATH HTTP_API_PREFIX "bluetooth/disconnect"
#define HTTP_BLUETOOTH_UNPAIR_PATH HTTP_API_PREFIX "bluetooth/unpair"
#define HTTP_BLUETOOTH_AUTO_CONNECT_PATH HTTP_API_PREFIX "bluetooth/auto-connect"
#define HTTP_SYSTEM_SYNC_PATH HTTP_API_PREFIX "system/sync"
#define HTTP_SYSTEM_RESTART_PATH HTTP_API_PREFIX "system/restart"
#define HTTP_SYSTEM_SHUTDOWN_PATH HTTP_API_PREFIX "system/shutdown"
#define HTTP_SYSTEM_BOOT_FAILURE_PATH HTTP_API_PREFIX "system/boot-failure"
#define HTTP_SYSTEM_AUDIO_STRESS_PATH HTTP_API_PREFIX "system/audio-stress"
#define HTTP_VERSION_PATH HTTP_API_PREFIX "version"
#define HTTP_SYSTEM_INFO_PATH HTTP_API_PREFIX "system/info"
#define HTTP_SYSTEM_FAN_PATH HTTP_API_PREFIX "system/fan"
#define HTTP_MIDI_LAST_NOTE_PATH HTTP_API_PREFIX "midi/last_note"
#define HTTP_MIDI_MAPPINGS_PATH HTTP_API_PREFIX "midi/mappings"
#define HTTP_MIDI_MAPPING_SET_PATH HTTP_API_PREFIX "midi/mapping/set"
#define HTTP_MIDI_MAPPING_DELETE_PATH HTTP_API_PREFIX "midi/mapping/delete"
#define HTTP_MIDI_ACTIONS_PATH HTTP_API_PREFIX "midi/actions"
#define HTTP_MIDI_ACTION_SET_PATH HTTP_API_PREFIX "midi/action/set"
#define HTTP_MIDI_ACTION_DELETE_PATH HTTP_API_PREFIX "midi/action/delete"
#define HTTP_MIDI_LIGHT_CONFIG_PATH HTTP_API_PREFIX "midi/light_config"
#define HTTP_MIDI_LIGHT_SOUNDS_PATH HTTP_API_PREFIX "midi/light_sounds"
#define HTTP_MIDI_LIGHT_SOUND_SET_PATH HTTP_API_PREFIX "midi/light_sound/set"
#define HTTP_MIDI_LIGHT_SOUND_DELETE_PATH HTTP_API_PREFIX "midi/light_sound/delete"
#define HTTP_MIDI_LIGHT_REFRESH_PATH HTTP_API_PREFIX "midi/light_refresh"
#define HTTP_MIDI_LIGHT_DIM_PATH HTTP_API_PREFIX "midi/light_dim"
#define HTTP_MIDI_SAMPLER_CONFIG_PATH HTTP_API_PREFIX "midi/sampler_config"
#define HTTP_MIDI_SAMPLER_MODE_PATH HTTP_API_PREFIX "midi/sampler_mode"
#define HTTP_MIDI_LAST_CC_PATH HTTP_API_PREFIX "midi/last_cc"
#define HTTP_MIDI_CC_VOLUMES_PATH HTTP_API_PREFIX "midi/cc_volumes"
#define HTTP_MIDI_CC_VOLUME_SET_PATH HTTP_API_PREFIX "midi/cc_volume/set"
#define HTTP_MIDI_CC_VOLUME_DELETE_PATH HTTP_API_PREFIX "midi/cc_volume/delete"
#define HTTP_EFFECTS_PATH HTTP_API_PREFIX "effects"
#define HTTP_EFFECT_SET_PATH HTTP_API_PREFIX "effect/set"
#define HTTP_EFFECT_CREATE_PATH HTTP_API_PREFIX "effect/create"
#define HTTP_EFFECT_DELETE_PATH HTTP_API_PREFIX "effect/delete"
#define HTTP_EFFECT_MIDI_CC_SET_PATH HTTP_API_PREFIX "effect/midi_cc/set"
#define HTTP_EFFECT_MIDI_CC_DELETE_PATH HTTP_API_PREFIX "effect/midi_cc/delete"
#define HTTP_EFFECT_MIDI_TOGGLE_SET_PATH HTTP_API_PREFIX "effect/midi_toggle/set"
#define HTTP_EFFECT_MIDI_TOGGLE_DELETE_PATH HTTP_API_PREFIX "effect/midi_toggle/delete"
#define HTTP_EFFECT_MIDI_LIGHT_SET_PATH HTTP_API_PREFIX "effect/midi_light/set"
#define HTTP_EFFECT_MIDI_LIGHT_DELETE_PATH HTTP_API_PREFIX "effect/midi_light/delete"
#define HTTP_AUDIO_VOLUMES_PATH HTTP_API_PREFIX "audio/volumes"
#define HTTP_AUDIO_VOLUME_SET_PATH HTTP_API_PREFIX "audio/volume/set"
#define HTTP_AUDIO_DEVICES_PATH HTTP_API_PREFIX "audio/devices"
#define HTTP_AUDIO_METERS_PATH HTTP_API_PREFIX "audio/meters"
#define HTTP_AUDIO_RESCAN_PATH HTTP_API_PREFIX "audio/rescan"
#define HTTP_FS_ROOT ROOT_MOUNT_POINT "/"
#define HTTP_BOOT_CONFIG_FILE "/boot/config.txt"
#define HTTP_BOOT_CMDLINE_FILE "/boot/cmdline.txt"

namespace {

static int mapping_path_safe(const char *s);
static int body_get_value(const char *body,
						  size_t body_len,
						  const char *key,
						  char *out,
						  size_t out_sz);
static void copy_bound(char *dst, size_t dst_sz, const char *src);
static int mapping_normalize_sfx(const char *raw, char *out, size_t out_sz);

static int purgeMissingSoundMappings(HttpServer *server) {
	if (!server->app || !server->app->config) {
		return RET_ERR;
	}
	if (server->app->midi) {
		return server->app->midi->reloadSoundboardMappings();
	}
	MidiMapData data = server->app->config->readMidiMapFile();
	return server->app->config->purgeMissingSoundMappings(&data);
}

static int sendMethodNotAllowed(HttpServer *server, int cfd) {
	static const char mna[] = "method not allowed\n";
	return server->sendResponse(cfd, "405 Method Not Allowed", "text/plain; charset=utf-8", mna, sizeof(mna) - 1);
}

static int sendNotImplemented(HttpServer *server, int cfd, const char *name) {
	char out[128];
	int n = snprintf(out, sizeof(out), "%s not implemented\n", name ? name : "endpoint");
	if (n < 0) {
		n = 0;
	}
	return server->sendResponse(cfd, "501 Not Implemented", "text/plain; charset=utf-8", out, (size_t)n);
}

static int mountBootConfig() {
	if (mkdir("/boot", 0755) < 0 && errno != EEXIST) {
		return RET_ERR;
	}
	if (mount(BOOT_DEVICE_PATH, "/boot", "vfat", MS_NOATIME, nullptr) < 0 && errno != EBUSY) {
		return RET_ERR;
	}
	return RET_OK;
}

static int handleBootConfig(HttpServer *server,
							int cfd,
							const char *method,
							const char *path,
							const char *body,
							size_t body_len) {
	(void)path;
	if (!server || !method) {
		return -1;
	}
	if (mountBootConfig() != RET_OK) {
		static const char err[] = "boot partition unavailable\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	if (strcmp(method, "GET") == 0) {
		char config[HTTP_BODY_MAX];
		int fd = open(HTTP_BOOT_CONFIG_FILE, O_RDONLY);
		if (fd < 0) {
			static const char err[] = "boot config unavailable\n";
			return server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8", err, sizeof(err) - 1);
		}
		ssize_t length = read(fd, config, sizeof(config));
		close(fd);
		if (length < 0) {
			static const char err[] = "failed to read boot config\n";
			return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
		}
		return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", config, (size_t)length);
	}

	if (strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}
	if (!body || body_len == 0 || body_len >= HTTP_BODY_MAX) {
		static const char err[] = "invalid boot config body\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (prepareBootUpdateRecovery() != RET_OK) {
		static const char err[] = "failed to back up boot files\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	int fd = open(HTTP_BOOT_CONFIG_FILE, O_WRONLY | O_TRUNC | O_CREAT, 0644);
	if (fd < 0) {
		static const char err[] = "failed to open boot config\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	size_t written = 0;
	while (written < body_len) {
		ssize_t count = write(fd, body + written, body_len - written);
		if (count <= 0) {
			close(fd);
			static const char err[] = "failed to write boot config\n";
			return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
		}
		written += (size_t)count;
	}
	int syncRc = fsync(fd);
	close(fd);
	if (syncRc < 0) {
		static const char err[] = "failed to sync boot config\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	static const char ok[] = "{\"ok\":true}\n";
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
}

static int sendPi5FanSettings(HttpServer *server, int cfd, bool enabled,
							  const int temperatures[4], const int speeds[4], int hysteresis) {
	char out[256];
	int n = snprintf(out, sizeof(out),
					 "{\"enabled\":%s,\"temperatures\":[%d,%d,%d,%d],"
					 "\"speeds\":[%d,%d,%d,%d],\"hysteresis\":%d}\n",
					 enabled ? "true" : "false",
					 temperatures[0], temperatures[1], temperatures[2], temperatures[3],
					 speeds[0], speeds[1], speeds[2], speeds[3], hysteresis);
	if (n < 0 || n >= (int)sizeof(out)) return -1;
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static bool readPi5FanSettings(const char *config, size_t length, int temperatures[4],
							   int speeds[4], int *hysteresis, bool *enabled) {
	static const char begin[] = "# AudioX Pi 5 fan curve: begin";
	static const char end[] = "# AudioX Pi 5 fan curve: end";
	const char *block = strstr(config, begin);
	if (!block) {
		static const int defaultTemperatures[4] = {50, 60, 68, 75};
		static const int defaultSpeeds[4] = {75, 125, 175, 250};
		memcpy(temperatures, defaultTemperatures, sizeof(defaultTemperatures));
		memcpy(speeds, defaultSpeeds, sizeof(defaultSpeeds));
		*hysteresis = 5;
		*enabled = false;
		return true;
	}
	const char *blockEnd = strstr(block, end);
	if (!blockEnd || (size_t)(blockEnd - config) >= length || strstr(blockEnd + sizeof(end) - 1, begin))
		return false;
	*enabled = true;
	bool haveTemperature[4] = {};
	bool haveSpeed[4] = {};
	bool haveHysteresis[4] = {};
	int hysteresisByStep[4] = {};
	const char *line = strchr(block, '\n');
	if (!line || line >= blockEnd) return false;
	++line;
	while (line < blockEnd) {
		const char *lineEnd = strchr(line, '\n');
		if (!lineEnd || lineEnd > blockEnd) lineEnd = blockEnd;
		char entry[96];
		size_t entryLength = (size_t)(lineEnd - line);
		if (entryLength >= sizeof(entry)) return false;
		memcpy(entry, line, entryLength);
		entry[entryLength] = '\0';
		if (strcmp(entry, "[all]") == 0) {
			line = lineEnd + (lineEnd < blockEnd ? 1 : 0);
			continue;
		}
		int index = -1;
		long value = 0;
		int consumed = 0;
		if (sscanf(entry, "dtparam=fan_temp%d=%ld%n", &index, &value, &consumed) == 2 &&
			entry[consumed] == '\0' && index >= 0 && index < 4 && value >= 35000 && value <= 80000) {
			temperatures[index] = (int)(value / 1000);
			haveTemperature[index] = true;
		} else if (sscanf(entry, "dtparam=fan_temp%d_hyst=%ld%n", &index, &value, &consumed) == 2 &&
				   entry[consumed] == '\0' && index >= 0 && index < 4 && value >= 1000 && value <= 15000) {
			hysteresisByStep[index] = (int)(value / 1000);
			haveHysteresis[index] = true;
		} else if (sscanf(entry, "dtparam=fan_temp%d_speed=%ld%n", &index, &value, &consumed) == 2 &&
				   entry[consumed] == '\0' && index >= 0 && index < 4 && value >= 1 && value <= 255) {
			speeds[index] = (int)value;
			haveSpeed[index] = true;
		} else {
			return false;
		}
		line = lineEnd + (lineEnd < blockEnd ? 1 : 0);
	}
	for (int i = 0; i < 4; ++i) {
		if (!haveTemperature[i] || !haveSpeed[i] || !haveHysteresis[i] ||
			(i > 0 && (temperatures[i] <= temperatures[i - 1] || speeds[i] <= speeds[i - 1])) ||
			(i > 0 && temperatures[i] - temperatures[i - 1] <= hysteresisByStep[0]) ||
			(i > 0 && hysteresisByStep[i] != hysteresisByStep[0]))
			return false;
	}
	*hysteresis = hysteresisByStep[0];
	return true;
}

static bool fanInteger(const char *body, size_t bodyLength, const char *key, long minimum,
					   long maximum, long *value) {
	char text[16];
	if (!body_get_value(body, bodyLength, key, text, sizeof(text)) || !text[0]) return false;
	char *end = nullptr;
	errno = 0;
	long parsed = strtol(text, &end, 10);
	if (errno || !end || *end || parsed < minimum || parsed > maximum) return false;
	*value = parsed;
	return true;
}

static int handlePi5FanConfig(HttpServer *server, int cfd, const char *method,
							  const char *body, size_t bodyLength) {
	if (!server || !method) return -1;
	if (strcmp(audioxDetectBoard(), "pi5") != 0) {
		static const char unsupported[] = "Pi 5 fan controls are only available on Raspberry Pi 5.\n";
		return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8",
									unsupported, sizeof(unsupported) - 1);
	}
	if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0)
		return sendMethodNotAllowed(server, cfd);
	if (mountBootConfig() != RET_OK) {
		static const char unavailable[] = "boot partition unavailable\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "text/plain; charset=utf-8",
									unavailable, sizeof(unavailable) - 1);
	}
	char config[HTTP_BODY_MAX];
	int fd = open(HTTP_BOOT_CONFIG_FILE, O_RDONLY);
	if (fd < 0) {
		static const char unavailable[] = "boot config unavailable\n";
		return server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8",
									unavailable, sizeof(unavailable) - 1);
	}
	size_t configLength = 0;
	while (configLength < sizeof(config)) {
		ssize_t count = read(fd, config + configLength, sizeof(config) - configLength);
		if (count < 0 && errno == EINTR) continue;
		if (count < 0) {
			close(fd);
			static const char error[] = "failed to read boot config\n";
			return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8",
										error, sizeof(error) - 1);
		}
		if (count == 0) break;
		configLength += (size_t)count;
	}
	close(fd);
	if (configLength == sizeof(config)) {
		static const char tooLarge[] = "boot config is too large to update safely\n";
		return server->sendResponse(cfd, "413 Payload Too Large", "text/plain; charset=utf-8",
									tooLarge, sizeof(tooLarge) - 1);
	}
	config[configLength] = '\0';

	int temperatures[4] = {}, speeds[4] = {}, hysteresis = 0;
	bool enabled = false;
	if (!readPi5FanSettings(config, configLength, temperatures, speeds, &hysteresis, &enabled)) {
		static const char invalid[] = "managed Pi 5 fan settings are incomplete or invalid\n";
		return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8",
									invalid, sizeof(invalid) - 1);
	}
	if (strcmp(method, "GET") == 0)
		return sendPi5FanSettings(server, cfd, enabled, temperatures, speeds, hysteresis);

	long enabledValue = 0;
	if (!fanInteger(body, bodyLength, "enabled", 0, 1, &enabledValue)) {
		static const char invalid[] = "expected enabled=0|1 and a valid four-step fan curve\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8",
									invalid, sizeof(invalid) - 1);
	}
	enabled = enabledValue != 0;
	long hysteresisValue = 0;
	if (!fanInteger(body, bodyLength, "hysteresis", 1, 15, &hysteresisValue)) {
		static const char invalid[] = "fan hysteresis must be between 1 and 15 degrees C\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8",
									invalid, sizeof(invalid) - 1);
	}
	hysteresis = (int)hysteresisValue;
	for (int i = 0; i < 4; ++i) {
		char key[24];
		long value = 0;
		snprintf(key, sizeof(key), "temperature%d", i);
		if (!fanInteger(body, bodyLength, key, 40, 80, &value)) {
			static const char invalid[] = "fan temperatures must be between 40 and 80 degrees C\n";
			return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8",
										invalid, sizeof(invalid) - 1);
		}
		temperatures[i] = (int)value;
		snprintf(key, sizeof(key), "speed%d", i);
		if (!fanInteger(body, bodyLength, key, 1, 255, &value)) {
			static const char invalid[] = "fan speeds must be between 1 and 255\n";
			return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8",
										invalid, sizeof(invalid) - 1);
		}
		speeds[i] = (int)value;
		if (i > 0 && (temperatures[i] <= temperatures[i - 1] || speeds[i] <= speeds[i - 1] ||
					  temperatures[i] - temperatures[i - 1] <= hysteresis)) {
			static const char invalid[] = "fan temperatures and speeds must increase at every step; temperature gaps must exceed hysteresis\n";
			return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8",
										invalid, sizeof(invalid) - 1);
		}
	}
	static const char begin[] = "# AudioX Pi 5 fan curve: begin";
	static const char end[] = "# AudioX Pi 5 fan curve: end";
	char updated[HTTP_BODY_MAX];
	size_t configWritten = 0;
	int syncResult = 0;
	size_t prefixLength = configLength;
	size_t suffixOffset = configLength;
	const char *oldBegin = strstr(config, begin);
	if (oldBegin) {
		const char *oldEnd = strstr(oldBegin, end);
		if (!oldEnd) {
			static const char invalid[] = "managed Pi 5 fan settings are incomplete\n";
			return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8",
										invalid, sizeof(invalid) - 1);
		}
		const char *after = strchr(oldEnd, '\n');
		if (after) ++after;
		else after = config + configLength;
		prefixLength = (size_t)(oldBegin - config);
		suffixOffset = (size_t)(after - config);
	}
	size_t written = prefixLength;
	memcpy(updated, config, prefixLength);
	if (suffixOffset < configLength) {
		size_t suffixLength = configLength - suffixOffset;
		if (suffixLength >= sizeof(updated) - written) goto fan_config_too_large;
		memcpy(updated + written, config + suffixOffset, suffixLength);
		written += suffixLength;
	}
	if (enabled) {
		if (written && updated[written - 1] != '\n') updated[written++] = '\n';
		int count = snprintf(updated + written, sizeof(updated) - written,
							 "%s\n[all]\n", begin);
		if (count < 0 || (size_t)count >= sizeof(updated) - written) goto fan_config_too_large;
		written += (size_t)count;
		for (int i = 0; i < 4; ++i) {
			count = snprintf(updated + written, sizeof(updated) - written,
							 "dtparam=fan_temp%d=%d000\n"
							 "dtparam=fan_temp%d_hyst=%d000\n"
							 "dtparam=fan_temp%d_speed=%d\n",
							 i, temperatures[i], i, hysteresis, i, speeds[i]);
			if (count < 0 || (size_t)count >= sizeof(updated) - written) goto fan_config_too_large;
			written += (size_t)count;
		}
		count = snprintf(updated + written, sizeof(updated) - written, "%s\n", end);
		if (count < 0 || (size_t)count >= sizeof(updated) - written) goto fan_config_too_large;
		written += (size_t)count;
	}
	if (written == 0 || written >= sizeof(updated)) goto fan_config_too_large;
	updated[written] = '\0';
	if (prepareBootUpdateRecovery() != RET_OK) {
		static const char error[] = "failed to back up boot files\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8",
									error, sizeof(error) - 1);
	}
	fd = open(HTTP_BOOT_CONFIG_FILE, O_WRONLY | O_TRUNC | O_CREAT, 0644);
	if (fd < 0) {
		static const char error[] = "failed to open boot config\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8",
									error, sizeof(error) - 1);
	}
	while (configWritten < written) {
		ssize_t count = write(fd, updated + configWritten, written - configWritten);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) {
			close(fd);
			static const char error[] = "failed to write boot config\n";
			return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8",
										error, sizeof(error) - 1);
		}
		configWritten += (size_t)count;
	}
	syncResult = fsync(fd);
	close(fd);
	if (syncResult < 0) {
		static const char error[] = "failed to sync boot config\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8",
									error, sizeof(error) - 1);
	}
	return sendPi5FanSettings(server, cfd, enabled, temperatures, speeds, hysteresis);

fan_config_too_large:
	{
		static const char tooLarge[] = "updated boot config is too large\n";
		return server->sendResponse(cfd, "413 Payload Too Large", "text/plain; charset=utf-8",
									tooLarge, sizeof(tooLarge) - 1);
	}
}

static int handleBootCmdline(HttpServer *server, int cfd, const char *method, const char *body, size_t body_len) {
	if (!server || !method) {
		return -1;
	}
	if (strcmp(method, "GET") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}
	if (mountBootConfig() != RET_OK) {
		static const char err[] = "boot partition unavailable\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	if (strcmp(method, "GET") == 0) {
		char cmdline[AUDIOX_UPDATE_CMDLINE_MAX];
		ssize_t length = brReadSmallFile(HTTP_BOOT_CMDLINE_FILE, cmdline, sizeof(cmdline));
		if (length < 0) {
			static const char err[] = "cmdline unavailable\n";
			return server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8", err, sizeof(err) - 1);
		}
		return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", cmdline, (size_t)length);
	}

	const char *invalid = body ? audioxValidateCmdline(body, body_len) : "missing body";
	if (invalid) {
		char err[160];
		int n = snprintf(err, sizeof(err), "invalid cmdline: %s\n", invalid);
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, n > 0 ? (size_t)n : 0);
	}
	if (prepareBootUpdateRecovery() != RET_OK) {
		static const char err[] = "failed to back up boot files\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	// The firmware reads cmdline.txt as a single line; always terminate it with one newline.
	char text[AUDIOX_UPDATE_CMDLINE_MAX + 2];
	size_t len = body_len;
	while (len > 0 && (body[len - 1] == '\n' || body[len - 1] == '\r')) {
		--len;
	}
	memcpy(text, body, len);
	text[len] = '\n';
	text[len + 1] = '\0';
	if (brWriteFileAtomic("/boot", "cmdline.txt", text) < 0) {
		static const char err[] = "failed to write cmdline\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	static const char ok[] = "{\"ok\":true}\n";
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
}

static int safeApiSuffix(const char *suffix) {
	if (!suffix) {
		return 0;
	}
	if (!suffix[0]) {
		return 1;
	}
	if (strstr(suffix, "..") != NULL) {
		return 0;
	}
	if (strchr(suffix, '\\') != NULL) {
		return 0;
	}
	return 1;
}

static int pathIsRootfs(const char *path) {
	if (!path) {
		return 0;
	}
	if (strcmp(path, "/api/rootfs") == 0) {
		return 1;
	}
	return strncmp(path, HTTP_ROOTFS_PREFIX, strlen(HTTP_ROOTFS_PREFIX)) == 0;
}

static const char *rootfsSuffix(const char *path) {
	if (!path) {
		return NULL;
	}
	if (strcmp(path, "/api/rootfs") == 0) {
		return "";
	}
	if (strncmp(path, HTTP_ROOTFS_PREFIX, strlen(HTTP_ROOTFS_PREFIX)) == 0) {
		return path + strlen(HTTP_ROOTFS_PREFIX);
	}
	return NULL;
}

static int ensureParentDirs(const char *path) {
	if (!path || !path[0]) {
		return -1;
	}

	char tmp[512];
	size_t n = strnlen(path, sizeof(tmp) - 1);
	if (n == 0 || n >= sizeof(tmp) - 1) {
		return -1;
	}
	memcpy(tmp, path, n);
	tmp[n] = '\0';

	for (char *p = tmp + 1; *p; ++p) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
			return -1;
		}
		*p = '/';
	}

	return 0;
}

static int sendRootfsDirListing(HttpServer *server,
								int cfd,
								const char *api_path,
								const char *fs_path) {
	DIR *dir = opendir(fs_path);
	if (!dir) {
		static const char err[] = "opendir failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	char body[HTTP_BODY_MAX];
	size_t off = 0;
	int n = snprintf(body,
					 sizeof(body),
					 "rootfs listing: %s\n",
					 api_path ? api_path : "/api/rootfs");
	if (n > 0) {
		off = (size_t)n;
	}

	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
			continue;
		}

		char full[512];
		int fn = snprintf(full, sizeof(full), "%s/%s", fs_path, ent->d_name);
		if (fn <= 0 || (size_t)fn >= sizeof(full)) {
			continue;
		}

		struct stat st;
		int is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
		n = snprintf(body + off,
					 sizeof(body) - off,
					 "%s%s\n",
					 ent->d_name,
					 is_dir ? "/" : "");
		if (n <= 0 || (size_t)n >= sizeof(body) - off) {
			break;
		}
		off += (size_t)n;
	}

	closedir(dir);
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", body, off);
}

static int handleApiGetRootfs(HttpServer *server, int cfd, const char *path) {
	const char *suffix = rootfsSuffix(path);
	if (!suffix || !safeApiSuffix(suffix)) {
		static const char bad[] = "bad path\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char fs_path[512];
	int pn = snprintf(fs_path, sizeof(fs_path), "%s%s", HTTP_FS_ROOT, suffix);
	if (pn <= 0 || (size_t)pn >= sizeof(fs_path)) {
		static const char bad[] = "path too long\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	struct stat st;
	if (stat(fs_path, &st) < 0) {
		static const char nf[] = "not found\n";
		return server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8", nf, sizeof(nf) - 1);
	}

	if (S_ISDIR(st.st_mode)) {
		return sendRootfsDirListing(server, cfd, path, fs_path);
	}

	if (server->sendFilePath(cfd, fs_path, NULL) < 0) {
		static const char err[] = "read failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	return 0;
}

static int handleApiPutRootfs(HttpServer *server,
							  int cfd,
							  const char *path,
							  const char *body,
							  size_t body_len) {
	const char *suffix = rootfsSuffix(path);
	if (!suffix || !safeApiSuffix(suffix)) {
		static const char bad[] = "bad path\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}
	if (!suffix[0] || suffix[strlen(suffix) - 1] == '/') {
		static const char bad[] = "bad file path\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char fs_path[512];
	int pn = snprintf(fs_path, sizeof(fs_path), "%s%s", HTTP_FS_ROOT, suffix);
	if (pn <= 0 || (size_t)pn >= sizeof(fs_path)) {
		static const char bad[] = "path too long\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	if (ensureParentDirs(fs_path) < 0) {
		static const char err[] = "mkdir failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	int fd = open(fs_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		static const char err[] = "open failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	size_t written = 0;
	while (written < body_len) {
		ssize_t nw = write(fd, body + written, body_len - written);
		if (nw < 0) {
			if (errno == EINTR) {
				continue;
			}
			close(fd);
			static const char err[] = "write failed\n";
			return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
		}
		if (nw == 0) {
			break;
		}
		written += (size_t)nw;
	}
	close(fd);

	if (written != body_len) {
		static const char err[] = "write failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	static const char ok[] = "ok\n";
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleApiDeleteRootfs(HttpServer *server, int cfd, const char *path) {
	const char *suffix = rootfsSuffix(path);
	if (!suffix || !safeApiSuffix(suffix)) {
		static const char bad[] = "bad path\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}
	if (!suffix[0] || suffix[strlen(suffix) - 1] == '/') {
		static const char bad[] = "cannot delete a directory\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char fs_path[512];
	int pn = snprintf(fs_path, sizeof(fs_path), "%s%s", HTTP_FS_ROOT, suffix);
	if (pn <= 0 || (size_t)pn >= sizeof(fs_path)) {
		static const char bad[] = "path too long\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	if (unlink(fs_path) < 0) {
		if (errno == ENOENT) {
			static const char nf[] = "not found\n";
			return server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8", nf, sizeof(nf) - 1);
		}
		static const char err[] = "delete failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	if (strncmp(suffix, "sfx/", 4) == 0 && purgeMissingSoundMappings(server) != RET_OK) {
		static const char err[] = "audio deleted, but MIDI cleanup failed; retry soundboard reload\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	static const char ok[] = "ok\n";
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
}

static int decodeSoundboardTarget(const char *token, char *triggerPath, size_t triggerPathSz) {
    if (!token || !triggerPath || triggerPathSz == 0 || !token[0]) {
        return RET_ERR;
    }

    char decoded[MIDI_SFX_PATH_MAX];
    size_t di = 0;
    for (size_t i = 0; token[i] && di + 1 < sizeof(decoded); ++i) {
        unsigned char c = (unsigned char)token[i];
        if (c == '%') {
            if (!isxdigit((unsigned char)token[i + 1]) || !isxdigit((unsigned char)token[i + 2])) {
                return RET_ERR;
            }
            char hex[3] = {token[i + 1], token[i + 2], '\0'};
            decoded[di++] = (char)strtol(hex, NULL, 16);
            i += 2;
            continue;
        }
        if (c == '+') {
            decoded[di++] = ' ';
            continue;
        }
        decoded[di++] = (char)c;
    }
    decoded[di] = '\0';

    char *endp = NULL;
    long slot = strtol(decoded, &endp, 10);
    int isNumericSlot = (endp && *endp == '\0' && slot >= 0 && slot <= 255) ? 1 : 0;
    if (!isNumericSlot && !mapping_path_safe(decoded)) {
        return RET_ERR;
    }

    int pn = 0;
    if (isNumericSlot) {
        pn = snprintf(triggerPath, triggerPathSz, "%s/%ld.wav", SFX_ROOT_DIR, slot);
    } else if (strncmp(decoded, SFX_ROOT_DIR "/", strlen(SFX_ROOT_DIR) + 1) == 0) {
        pn = snprintf(triggerPath, triggerPathSz, "%s", decoded);
    } else {
        pn = snprintf(triggerPath, triggerPathSz, "%s/%s", SFX_ROOT_DIR, decoded);
    }

    if (pn <= 0 || (size_t)pn >= triggerPathSz) {
        return RET_ERR;
    }

    return RET_OK;
}

static int handleSoundboardTrigger(HttpServer *server,
								   int cfd,
								   const char *method,
								   const char *path,
								   const char *prefix,
								   int action) {
	if (!server || !server->app || !method || !path) {
		return -1;
	}

	const char *token = path + strlen(prefix);
	if (!token[0]) {
		static const char bad[] = "missing trigger target\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	if (!server->app || !server->app->audio) {
		static const char err[] = "audio subsystem unavailable\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	char triggerPath[256];
	if (decodeSoundboardTarget(token, triggerPath, sizeof(triggerPath)) != RET_OK) {
		static const char bad[] = "invalid trigger target\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	int rc = RET_ERR;
	if (action == 1) {
		rc = server->app->audio->startHeldSfx(triggerPath);
	} else if (action == 2) {
		server->app->audio->stopHeldSfx();
		rc = RET_OK;
	} else {
		rc = server->app->audio->triggerSfx(triggerPath);
	}

	if (rc == RET_ERR) {
		static const char err[] = "trigger failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (rc == RET_WARN) {
		static const char missing[] = "sfx file not found\n";
		return server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8", missing, sizeof(missing) - 1);
	}

	static const char ok[] = "ok\n";
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleSoundboardStopAll(HttpServer *server,
								 int cfd,
								 const char *method,
								 const char *path) {
	(void)path;
	if (!server || !server->app || !method) {
		return -1;
	}

	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	if (!server->app->audio) {
		static const char err[] = "audio subsystem unavailable\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	server->app->audio->stopAllSfx();
	static const char ok[] = "ok\n";
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleSoundboardReload(HttpServer *server,
								 int cfd,
								 const char *method,
								 const char *path) {
	(void)path;
	if (!server || !server->app || !method) {
		return -1;
	}

	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	if (!server->app->audio) {
		static const char err[] = "audio subsystem unavailable\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	int rc = server->app->audio->reloadSfxBank();
	if (purgeMissingSoundMappings(server) != RET_OK) {
		static const char err[] = "failed to clean up missing sound MIDI mappings\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	uint32_t loaded = server->app->audio->loadedSfxCount();
	char out[128];
	int n = snprintf(out,
					 sizeof(out),
					 "{\"ok\":%s,\"loaded\":%u,\"max\":%u}\n",
					 (rc == RET_ERR) ? "false" : "true",
					 (unsigned)loaded,
					 (unsigned)AUDIO_SFX_SLOT_COUNT);
	if (n < 0) {
		n = 0;
	}
	if ((size_t)n >= sizeof(out)) {
		n = (int)(sizeof(out) - 1);
	}

	if (rc == RET_ERR) {
		return server->sendResponse(cfd, "500 Internal Server Error", "application/json; charset=utf-8", out, (size_t)n);
	}
	if (rc == RET_WARN) {
		return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleSoundboardMode(HttpServer *server,
								int cfd,
								const char *method,
								const char *path,
								const char *body,
								size_t body_len) {
	(void)path;
	if (!server || !server->app || !method || !server->app->config) {
		return -1;
	}

	if (strcmp(method, "GET") == 0) {
		ConfigData cfg = server->app->config->readConfigFile();
		char out[96];
		int n = snprintf(out, sizeof(out), "{\"mode\":\"%s\"}\n", soundboardModeToString(cfg.soundboardMode));
		if (n < 0 || (size_t)n >= sizeof(out)) {
			n = 0;
		}
		return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
	}

	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char modeBuf[24];
	if (!body || !body_get_value(body, body_len, "mode", modeBuf, sizeof(modeBuf))) {
		static const char bad[] = "expected mode=play|hold\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char modeLower[24];
	size_t ml = strnlen(modeBuf, sizeof(modeLower) - 1);
	for (size_t i = 0; i < ml; ++i) {
		modeLower[i] = (char)tolower((unsigned char)modeBuf[i]);
	}
	modeLower[ml] = '\0';
	if (strcmp(modeLower, "play") != 0 && strcmp(modeLower, "hold") != 0) {
		static const char bad[] = "invalid mode\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	uint8_t mode = soundboardModeFromString(modeLower);

	ConfigData cfg = server->app->config->readConfigFile();
	cfg.soundboardMode = mode;
	if (server->app->config->writeConfigFile(&cfg) != RET_OK) {
		static const char err[] = "failed to write config\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	if (server->app->audio) {
		server->app->audio->setSoundboardMode(mode);
		if (mode == SOUNDBOARD_MODE_PLAY) {
			server->app->audio->stopHeldSfx();
		}
	}

	char out[96];
	int n = snprintf(out, sizeof(out), "{\"ok\":true,\"mode\":\"%s\"}\n", soundboardModeToString(mode));
	if (n < 0 || (size_t)n >= sizeof(out)) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleFramebufferView(HttpServer *server, int cfd, const char *method,
                                 const char *body, size_t body_len) {
	if (!server || !server->app || !server->app->config) return -1;
	if (strcmp(method, "GET") == 0) {
		ConfigData cfg = server->app->config->readConfigFile();
		const char *out = cfg.framebufferSoundboard ? "{\"view\":\"soundboard\"}\n" : "{\"view\":\"meters\"}\n";
		return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, strlen(out));
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) return sendMethodNotAllowed(server, cfd);
	char view[24];
	if (!body || !body_get_value(body, body_len, "view", view, sizeof(view)) ||
	    (strcmp(view, "meters") != 0 && strcmp(view, "soundboard") != 0)) {
		static const char bad[] = "expected view=meters|soundboard\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}
	ConfigData cfg = server->app->config->readConfigFile();
	cfg.framebufferSoundboard = strcmp(view, "soundboard") == 0 ? 1 : 0;
	if (server->app->config->writeConfigFile(&cfg) != RET_OK) {
		static const char err[] = "failed to write config\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	const char *out = cfg.framebufferSoundboard ? "{\"view\":\"soundboard\"}\n" : "{\"view\":\"meters\"}\n";
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, strlen(out));
}

static int handleUpdateNotifications(HttpServer *server, int cfd, const char *method,
                                     const char *body, size_t body_len) {
	if (!server || !server->app || !server->app->config) return -1;
	static const char enabledJson[] = "{\"enabled\":true}\n";
	static const char disabledJson[] = "{\"enabled\":false}\n";
	auto replyState = [&](const ConfigData &cfg) {
		const char *out = cfg.updateNotificationsOff ? disabledJson : enabledJson;
		return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, strlen(out));
	};
	if (strcmp(method, "GET") == 0) return replyState(server->app->config->readConfigFile());
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) return sendMethodNotAllowed(server, cfd);
	char enabled[8];
	if (!body || !body_get_value(body, body_len, "enabled", enabled, sizeof(enabled)) ||
	    (strcmp(enabled, "0") != 0 && strcmp(enabled, "1") != 0)) {
		static const char bad[] = "expected enabled=0|1\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}
	ConfigData cfg = server->app->config->readConfigFile();
	cfg.updateNotificationsOff = strcmp(enabled, "0") == 0 ? 1 : 0;
	if (server->app->config->writeConfigFile(&cfg) != RET_OK) {
		static const char err[] = "failed to write config\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	return replyState(cfg);
}

static int handleSoundboardOrder(HttpServer *server, int cfd, const char *method,
								 const char *body, size_t body_len) {
	if (!server || !server->app || !server->app->config || !method) return -1;
	if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) return sendMethodNotAllowed(server, cfd);
	auto reply = [&](const char *status, const char *message) {
		return server->sendResponse(cfd, status, "text/plain; charset=utf-8", message, strlen(message));
	};
	static_assert(SOUNDBOARD_ORDER_MAX == AUDIO_SFX_SLOT_COUNT, "Web and framebuffer soundboard capacities must match");
	char files[SOUNDBOARD_ORDER_MAX][MIDI_SFX_PATH_MAX] = {};
	uint32_t count = 0;
	if (server->app->config->listSoundboardFiles(files, SOUNDBOARD_ORDER_MAX, &count) != RET_OK) {
		return reply("500 Internal Server Error", "Could not read the soundboard files.\n");
	}
	if (strcmp(method, "POST") == 0) {
		MidiMapData data = server->app->config->readMidiMapFile();
		data.soundOrderCount = 0;
		size_t offset = 0;
		while (offset < body_len) {
			const char *start = body + offset;
			const char *newline = (const char *)memchr(start, '\n', body_len - offset);
			const size_t len = newline ? (size_t)(newline - start) : body_len - offset;
			if (!len || len >= MIDI_SFX_PATH_MAX || memchr(start, '\0', len) ||
				data.soundOrderCount >= SOUNDBOARD_ORDER_MAX) {
				return reply("400 Bad Request", "Invalid soundboard order.\n");
			}
			char name[MIDI_SFX_PATH_MAX] = {};
			memcpy(name, start, len);
			bool exists = false;
			for (uint32_t i = 0; i < count; ++i) {
				if (strcmp(files[i], name) == 0) exists = true;
			}
			if (!exists) return reply("409 Conflict", "Sound files changed. Refresh the soundboard before reordering.\n");
			for (uint32_t i = 0; i < data.soundOrderCount; ++i) {
				if (strcmp(data.soundOrder[i].sfxPath, name) == 0) {
					return reply("400 Bad Request", "Soundboard order contains a duplicate sound.\n");
				}
			}
			copy_bound(data.soundOrder[data.soundOrderCount++].sfxPath, MIDI_SFX_PATH_MAX, name);
			offset += len + (newline ? 1 : 0);
		}
		if (data.soundOrderCount != count) {
			return reply("409 Conflict", "Sound files changed. Refresh the soundboard before reordering.\n");
		}
		if (server->app->config->writeMidiMapFile(&data) != RET_OK) {
			return reply("500 Internal Server Error", "Could not save the soundboard order.\n");
		}
		if (server->app->midi) server->app->midi->cachedMidiMap = data;
		for (uint32_t i = 0; i < count; ++i) {
			copy_bound(files[i], MIDI_SFX_PATH_MAX, data.soundOrder[i].sfxPath);
		}
	}
	char out[SOUNDBOARD_ORDER_MAX * MIDI_SFX_PATH_MAX];
	size_t length = 0;
	for (uint32_t i = 0; i < count; ++i) {
		const size_t len = strlen(files[i]);
		memcpy(out + length, files[i], len);
		length += len;
		out[length++] = '\n';
	}
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", out, length);
}

static int parseSoundboardGridNumber(const char *text, unsigned min, unsigned max, unsigned *out) {
	if (!text || !text[0] || !out) return RET_ERR;
	char *end = nullptr;
	unsigned long value = strtoul(text, &end, 10);
	if (!end || *end != '\0' || value < min || value > max) return RET_ERR;
	*out = (unsigned)value;
	return RET_OK;
}

static bool soundboardGridCellAt(const SoundboardGridCell *cells, uint32_t count,
								 uint8_t row, uint8_t column) {
	for (uint32_t i = 0; i < count; ++i) {
		if (cells[i].row == row && cells[i].column == column) return true;
	}
	return false;
}

static bool soundboardGridHasFile(const SoundboardGridCell *cells, uint32_t count, const char *sfx) {
	for (uint32_t i = 0; i < count; ++i) {
		if (strcmp(cells[i].sfxPath, sfx) == 0) return true;
	}
	return false;
}

static bool soundboardGridMappingMatches(const char *mappingPath, const char *sfx) {
	char normalized[MIDI_SFX_PATH_MAX]{};
	if (mapping_normalize_sfx(mappingPath, normalized, sizeof(normalized)) == RET_OK)
		return strcmp(normalized, sfx) == 0;
	if (strcmp(mappingPath, sfx) == 0) return true;
	if (mappingPath[0] != '/') return false;
	char path[sizeof(SFX_ROOT_DIR) + MIDI_SFX_PATH_MAX + 1]{};
	snprintf(path, sizeof(path), "%s/%s", SFX_ROOT_DIR, sfx);
	struct stat mapped{}, sound{};
	return stat(mappingPath, &mapped) == 0 && stat(path, &sound) == 0 &&
		mapped.st_dev == sound.st_dev && mapped.st_ino == sound.st_ino;
}

static int buildSoundboardGridCells(const MidiMapData &data,
									const char (*files)[MIDI_SFX_PATH_MAX],
									uint32_t fileCount,
									SoundboardGridCell *out,
									uint32_t *outCount,
									bool preferMidiNotes, bool fillUnassigned = false) {
	if (!files || !out || !outCount || fileCount > SOUNDBOARD_GRID_ELEMENTS_MAX ||
		data.gridRows == 0 || data.gridRows > SOUNDBOARD_GRID_ROWS_MAX ||
		data.gridColumns == 0 || data.gridColumns > SOUNDBOARD_GRID_COLUMNS_MAX) return RET_ERR;

	*outCount = 0;
	if (preferMidiNotes && (data.gridCornerMask & 0x0f) == 0x0f) {
		const uint32_t soundMappings = std::min<uint32_t>(data.mappingCount, MIDI_MAPPINGS_MAX);
		const uint32_t actionMappings = std::min<uint32_t>(data.actionMappingCount, MIDI_ACTION_MAPPINGS_MAX);
		for (uint32_t i = 0; i < soundMappings + actionMappings; ++i) {
			const char *mappingPath = nullptr;
			uint8_t mappedNote = 0;
			if (i < soundMappings) {
				mappingPath = data.mappings[i].sfxPath;
				mappedNote = data.mappings[i].note;
			} else {
				const MidiActionMapping &mapping = data.actionMappings[i - soundMappings];
				mappedNote = mapping.note;
				if (mapping.action == MIDI_ACTION_STOP_ALL) mappingPath = SOUNDBOARD_GRID_STOP_ALL_ID;
				if (mapping.action == MIDI_ACTION_SAMPLER_TOGGLE) mappingPath = SOUNDBOARD_GRID_SAMPLER_TOGGLE_ID;
			}
			if (!mappingPath) continue;
			if (!fillUnassigned) {
				bool assigned = false;
				for (uint32_t j = 0; j < data.gridCellCount && j < SOUNDBOARD_GRID_CELLS_MAX; ++j)
					if (soundboardGridMappingMatches(mappingPath, data.gridCells[j].sfxPath))
						assigned = true;
				if (!assigned) continue;
			}
			uint8_t row = 0;
			uint8_t column = 0;
			bool foundCell = false;
			for (uint8_t r = 0; r < data.gridRows && !foundCell; ++r) {
				for (uint8_t c = 0; c < data.gridColumns; ++c) {
					uint8_t note = 0;
					if (soundboardGridNote(&data, r, c, &note) && note == mappedNote) {
						row = r;
						column = c;
						foundCell = true;
						break;
					}
				}
			}
			char normalized[MIDI_SFX_PATH_MAX]{};
			for (uint32_t f = 0; f < fileCount; ++f)
				if (soundboardGridMappingMatches(mappingPath, files[f])) {
					copy_bound(normalized, sizeof(normalized), files[f]);
					break;
				}
			if (foundCell && normalized[0] && !soundboardGridHasFile(out, *outCount, normalized) &&
				!soundboardGridCellAt(out, *outCount, row, column)) {
				SoundboardGridCell &cell = out[(*outCount)++];
				copy_bound(cell.sfxPath, sizeof(cell.sfxPath), normalized);
				cell.row = row;
				cell.column = column;
			}
		}
	}
	for (uint32_t i = 0; i < data.gridCellCount && i < SOUNDBOARD_GRID_CELLS_MAX; ++i) {
		SoundboardGridCell saved = data.gridCells[i];
		if (saved.row >= data.gridRows || saved.column >= data.gridColumns ||
			soundboardGridHasFile(out, *outCount, saved.sfxPath)) continue;
		bool fileExists = false;
		for (uint32_t f = 0; f < fileCount; ++f)
			if (strcmp(files[f], saved.sfxPath) == 0) fileExists = true;
		if (!fileExists) continue;
		if (soundboardGridCellAt(out, *outCount, saved.row, saved.column)) {
			if (!preferMidiNotes) continue;
			bool placed = false;
			for (uint8_t row = 0; row < data.gridRows && !placed; ++row) {
				for (uint8_t column = 0; column < data.gridColumns; ++column) {
					if (soundboardGridCellAt(out, *outCount, row, column)) continue;
					saved.row = row;
					saved.column = column;
					placed = true;
					break;
				}
			}
			if (!placed) continue;
		}
		out[(*outCount)++] = saved;
	}

	for (uint32_t f = 0; f < fileCount; ++f) {
		if (!fillUnassigned) break;
		if (soundboardGridAction(files[f]) != MIDI_ACTION_NONE) continue;
		if (soundboardGridHasFile(out, *outCount, files[f])) continue;
		bool placed = false;
		for (uint8_t row = 0; row < data.gridRows && !placed; ++row) {
			for (uint8_t column = 0; column < data.gridColumns; ++column) {
				if (soundboardGridCellAt(out, *outCount, row, column)) continue;
				SoundboardGridCell &cell = out[(*outCount)++];
				copy_bound(cell.sfxPath, sizeof(cell.sfxPath), files[f]);
				cell.row = row;
				cell.column = column;
				placed = true;
				break;
			}
		}
		if (!placed) break;
	}
	return RET_OK;
}

static bool validSoundboardGridCalibration(const MidiMapData &data) {
	if ((data.gridCornerMask & 0x0f) != 0x0f || data.gridRows < 2 || data.gridColumns < 2) return false;
	uint8_t notes[SOUNDBOARD_GRID_CELLS_MAX]{};
	uint32_t count = 0;
	for (uint8_t row = 0; row < data.gridRows; ++row) {
		for (uint8_t column = 0; column < data.gridColumns; ++column) {
			uint8_t note = 0;
			if (!soundboardGridNote(&data, row, column, &note)) return false;
			for (uint32_t i = 0; i < count; ++i) if (notes[i] == note) return false;
			notes[count++] = note;
		}
	}
	return true;
}

static bool replaceSoundboardGridMidiMapping(MidiMapData *data, const char *sfx, uint8_t note) {
	if (!data || !sfx) return false;
	const uint8_t action = soundboardGridAction(sfx);
	uint32_t actionsKept = 0;
	for (uint32_t i = 0; i < data->actionMappingCount && i < MIDI_ACTION_MAPPINGS_MAX; ++i) {
		if (data->actionMappings[i].note == note ||
			(action != MIDI_ACTION_NONE && data->actionMappings[i].action == action)) continue;
		data->actionMappings[actionsKept++] = data->actionMappings[i];
	}
	data->actionMappingCount = actionsKept;
	uint32_t kept = 0;
	for (uint32_t i = 0; i < data->mappingCount && i < MIDI_MAPPINGS_MAX; ++i) {
		if (soundboardGridMappingMatches(data->mappings[i].sfxPath, sfx) ||
			data->mappings[i].note == note) continue;
		data->mappings[kept++] = data->mappings[i];
	}
	data->mappingCount = kept;
	if (action != MIDI_ACTION_NONE) {
		if (data->actionMappingCount >= MIDI_ACTION_MAPPINGS_MAX) return false;
		data->actionMappings[data->actionMappingCount++] = {note, action};
		return true;
	}
	if (data->mappingCount >= MIDI_MAPPINGS_MAX) return false;
	data->mappings[data->mappingCount].note = note;
	copy_bound(data->mappings[data->mappingCount].sfxPath,
			   sizeof(data->mappings[data->mappingCount].sfxPath), sfx);
	++data->mappingCount;
	return true;
}

static void removeSoundboardGridMidiMappings(MidiMapData &data, const char *sfx) {
	const uint8_t action = soundboardGridAction(sfx);
	if (action != MIDI_ACTION_NONE) {
		uint32_t kept = 0;
		for (uint32_t i = 0; i < data.actionMappingCount && i < MIDI_ACTION_MAPPINGS_MAX; ++i)
			if (data.actionMappings[i].action != action) data.actionMappings[kept++] = data.actionMappings[i];
		data.actionMappingCount = kept;
		return;
	}
	uint32_t kept = 0;
	for (uint32_t i = 0; i < data.mappingCount && i < MIDI_MAPPINGS_MAX; ++i)
		if (!soundboardGridMappingMatches(data.mappings[i].sfxPath, sfx))
			data.mappings[kept++] = data.mappings[i];
	data.mappingCount = kept;
}

static int handleSoundboardLayout(HttpServer *server, int cfd, const char *method,
								  const char *body, size_t body_len) {
	if (!server || !server->app || !server->app->config || !method) return -1;
	if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0)
		return sendMethodNotAllowed(server, cfd);

	MidiMapData data = server->app->config->readMidiMapFile();
	const MidiMapData original = data;
	char files[SOUNDBOARD_GRID_ELEMENTS_MAX][MIDI_SFX_PATH_MAX]{};
	uint32_t fileCount = 0;
	if (server->app->config->listSoundboardFiles(files, SOUNDBOARD_GRID_CELLS_MAX, &fileCount) != RET_OK) {
		static const char err[] = "Could not read the soundboard files.\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	copy_bound(files[fileCount++], MIDI_SFX_PATH_MAX, SOUNDBOARD_GRID_STOP_ALL_ID);
	copy_bound(files[fileCount++], MIDI_SFX_PATH_MAX, SOUNDBOARD_GRID_SAMPLER_TOGGLE_ID);
	if (strcmp(method, "GET") != 0) {
		if (!body) {
			static const char err[] = "Expected soundboard layout fields.\n";
			return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
		}
		char mode[16]{}, rowsText[8]{}, columnsText[8]{}, corner[24]{}, noteText[8]{};
		char sfxValue[MIDI_SFX_PATH_MAX]{}, rowText[8]{}, columnText[8]{};
		char unassignText[8]{};
		char actionText[24]{};
		bool hasMode = body_get_value(body, body_len, "mode", mode, sizeof(mode));
		bool hasRows = body_get_value(body, body_len, "rows", rowsText, sizeof(rowsText));
		bool hasColumns = body_get_value(body, body_len, "columns", columnsText, sizeof(columnsText));
		bool hasCorner = body_get_value(body, body_len, "corner", corner, sizeof(corner));
		bool hasNote = body_get_value(body, body_len, "note", noteText, sizeof(noteText));
		bool hasSfx = body_get_value(body, body_len, "sfx", sfxValue, sizeof(sfxValue));
		bool hasRow = body_get_value(body, body_len, "row", rowText, sizeof(rowText));
		bool hasColumn = body_get_value(body, body_len, "column", columnText, sizeof(columnText));
		bool hasUnassign = body_get_value(body, body_len, "unassign", unassignText, sizeof(unassignText));
		bool hasAction = body_get_value(body, body_len, "action", actionText, sizeof(actionText));
		if (hasAction) {
			if (hasSfx || (strcmp(actionText, "stop_all") != 0 && strcmp(actionText, "sampler_toggle") != 0)) {
				static const char err[] = "Invalid grid action.\n";
				return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
			}
			copy_bound(sfxValue, sizeof(sfxValue), strcmp(actionText, "stop_all") == 0
				? SOUNDBOARD_GRID_STOP_ALL_ID : SOUNDBOARD_GRID_SAMPLER_TOGGLE_ID);
			hasSfx = true;
		}
		if (!hasMode && !hasRows && !hasCorner && !hasSfx) {
			static const char err[] = "No soundboard layout fields were provided.\n";
			return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
		}

		if (hasMode) {
			if (strcmp(mode, "normal") != 0 && strcmp(mode, "grid") != 0) {
				static const char err[] = "mode must be normal or grid\n";
				return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
			}
			data.soundboardLayout = strcmp(mode, "grid") == 0
				? SOUNDBOARD_LAYOUT_GRID : SOUNDBOARD_LAYOUT_NORMAL;
		}
		if (hasRows != hasColumns) {
			static const char err[] = "rows and columns must be set together\n";
			return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
		}
		if (hasRows) {
			unsigned rows = 0, columns = 0;
			if (parseSoundboardGridNumber(rowsText, 1, SOUNDBOARD_GRID_ROWS_MAX, &rows) != RET_OK ||
				parseSoundboardGridNumber(columnsText, 1, SOUNDBOARD_GRID_COLUMNS_MAX, &columns) != RET_OK) {
				static const char err[] = "Invalid grid size.\n";
				return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
			}
			if (data.gridRows != rows || data.gridColumns != columns) data.gridCornerMask = 0;
			data.gridRows = (uint8_t)rows;
			data.gridColumns = (uint8_t)columns;
		}
		if (hasCorner != hasNote) {
			static const char err[] = "corner and note must be set together\n";
			return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
		}
		if (hasCorner) {
			static const char *const names[SOUNDBOARD_GRID_CORNER_COUNT] = {
				"top_left", "top_right", "bottom_left", "bottom_right",
			};
			unsigned note = 0;
			if (parseSoundboardGridNumber(noteText, 0, 127, &note) != RET_OK) {
				static const char err[] = "Invalid MIDI note.\n";
				return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
			}
			int cornerIndex = -1;
			for (int i = 0; i < SOUNDBOARD_GRID_CORNER_COUNT; ++i)
				if (strcmp(corner, names[i]) == 0) cornerIndex = i;
			if (cornerIndex < 0) {
				static const char err[] = "Invalid grid corner.\n";
				return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
			}
			data.gridCornerNotes[cornerIndex] = (uint8_t)note;
			data.gridCornerMask |= (uint8_t)(1U << cornerIndex);
			if ((data.gridCornerMask & 0x0f) == 0x0f && !validSoundboardGridCalibration(data)) {
				static const char err[] = "Corner notes do not describe a unique, evenly numbered MIDI grid.\n";
				return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
			}
			if (validSoundboardGridCalibration(data)) {
				SoundboardGridCell converted[SOUNDBOARD_GRID_CELLS_MAX]{};
				uint32_t convertedCount = 0;
				if (buildSoundboardGridCells(data, files, fileCount, converted, &convertedCount, true) != RET_OK) {
					static const char err[] = "Could not convert the current soundboard MIDI mappings to grid positions.\n";
					return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8", err, sizeof(err) - 1);
				}
				data.gridCellCount = convertedCount;
				memcpy(data.gridCells, converted, sizeof(converted));
			}
		}
		if (hasSfx || hasRow || hasColumn || hasUnassign) {
			char normalized[MIDI_SFX_PATH_MAX]{};
			unsigned row = 0, column = 0;
			const bool unassign = hasUnassign && strcmp(unassignText, "1") == 0;
			if (!hasSfx || (hasUnassign && (!unassign || hasRow || hasColumn)) ||
				mapping_normalize_sfx(sfxValue, normalized, sizeof(normalized)) != RET_OK ||
				(!unassign && (!hasRow || !hasColumn ||
				parseSoundboardGridNumber(rowText, 0, SOUNDBOARD_GRID_ROWS_MAX - 1, &row) != RET_OK ||
				parseSoundboardGridNumber(columnText, 0, SOUNDBOARD_GRID_COLUMNS_MAX - 1, &column) != RET_OK ||
				row >= data.gridRows || column >= data.gridColumns))) {
				static const char err[] = "Invalid soundboard cell assignment.\n";
				return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
			}
			bool fileExists = false;
			for (uint32_t i = 0; i < fileCount; ++i)
				if (strcmp(files[i], normalized) == 0) fileExists = true;
			if (!fileExists) {
				static const char err[] = "Sound file not found. Refresh the soundboard and try again.\n";
				return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8", err, sizeof(err) - 1);
			}
			SoundboardGridCell cells[SOUNDBOARD_GRID_CELLS_MAX]{};
			uint32_t count = 0;
			if (buildSoundboardGridCells(data, files, fileCount, cells, &count, false) != RET_OK) {
				static const char err[] = "Invalid grid.\n";
				return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8", err, sizeof(err) - 1);
			}
			data.gridCellCount = count;
			memcpy(data.gridCells, cells, sizeof(cells));
			uint32_t index = 0;
			while (index < data.gridCellCount &&
				   strcmp(data.gridCells[index].sfxPath, normalized) != 0) ++index;
			const bool wasAssigned = index < data.gridCellCount;
			SoundboardGridCell source{};
			if (wasAssigned) source = data.gridCells[index];
			char displaced[MIDI_SFX_PATH_MAX]{};
			for (uint32_t i = 0; !unassign && i < data.gridCellCount; ++i) {
				if (i == index || data.gridCells[i].row != row || data.gridCells[i].column != column) continue;
				copy_bound(displaced, sizeof(displaced), data.gridCells[i].sfxPath);
				if (wasAssigned) {
					data.gridCells[i].row = source.row;
					data.gridCells[i].column = source.column;
				} else {
					data.gridCells[i] = data.gridCells[--data.gridCellCount];
					index = data.gridCellCount;
				}
				break;
			}
			if (unassign) {
				if (wasAssigned) data.gridCells[index] = data.gridCells[--data.gridCellCount];
				removeSoundboardGridMidiMappings(data, normalized);
			} else {
				if (index == data.gridCellCount) {
					if (data.gridCellCount >= SOUNDBOARD_GRID_CELLS_MAX) {
						static const char err[] = "Soundboard grid assignment table is full.\n";
						return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8", err, sizeof(err) - 1);
					}
					++data.gridCellCount;
				}
				copy_bound(data.gridCells[index].sfxPath, sizeof(data.gridCells[index].sfxPath), normalized);
				data.gridCells[index].row = (uint8_t)row;
				data.gridCells[index].column = (uint8_t)column;
				if (displaced[0] && !wasAssigned) removeSoundboardGridMidiMappings(data, displaced);
				uint8_t mappedNote = 0;
				if (data.soundboardLayout == SOUNDBOARD_LAYOUT_GRID &&
					validSoundboardGridCalibration(data)) {
					removeSoundboardGridMidiMappings(data, normalized);
					if (displaced[0]) removeSoundboardGridMidiMappings(data, displaced);
					soundboardGridNote(&data, (uint8_t)row, (uint8_t)column, &mappedNote);
					bool mapped = replaceSoundboardGridMidiMapping(&data, normalized, mappedNote);
					if (displaced[0] && wasAssigned) {
						soundboardGridNote(&data, source.row, source.column, &mappedNote);
						mapped = mapped && replaceSoundboardGridMidiMapping(&data, displaced, mappedNote);
					}
					if (!mapped) {
						static const char err[] = "MIDI mapping table is full; the grid cell was not saved.\n";
						return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8", err, sizeof(err) - 1);
					}
				}
			}
		}

		if (data.soundboardLayout == SOUNDBOARD_LAYOUT_GRID || hasRows) {
			SoundboardGridCell converted[SOUNDBOARD_GRID_CELLS_MAX]{};
			uint32_t convertedCount = 0;
			const bool converting = data.soundboardLayout == SOUNDBOARD_LAYOUT_GRID &&
				!data.gridInitialized && data.gridCellCount == 0;
			if (buildSoundboardGridCells(data, files, fileCount, converted, &convertedCount, false, converting) != RET_OK) {
				static const char err[] = "Could not place all sounds in the selected grid.\n";
				return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8", err, sizeof(err) - 1);
			}
			data.gridCellCount = convertedCount;
			memcpy(data.gridCells, converted, sizeof(converted));
			if (data.soundboardLayout == SOUNDBOARD_LAYOUT_GRID) data.gridInitialized = 1;
		}
		for (uint32_t i = 0; i < original.gridCellCount && i < SOUNDBOARD_GRID_CELLS_MAX; ++i)
			if (!soundboardGridHasFile(data.gridCells, data.gridCellCount, original.gridCells[i].sfxPath))
				removeSoundboardGridMidiMappings(data, original.gridCells[i].sfxPath);
		if (server->app->config->writeMidiMapFile(&data) != RET_OK) {
			static const char err[] = "Could not save the soundboard layout.\n";
			return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
		}
		if (server->app->midi) {
			server->app->midi->cachedMidiMap = data;
			server->app->midi->refreshLightingFromState();
		}
	}

	SoundboardGridCell cells[SOUNDBOARD_GRID_CELLS_MAX]{};
	uint32_t cellCount = 0;
	if (data.soundboardLayout == SOUNDBOARD_LAYOUT_GRID &&
		buildSoundboardGridCells(data, files, fileCount, cells, &cellCount, false) != RET_OK) {
		static const char err[] = "Could not build the soundboard grid.\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	char out[HTTP_BODY_MAX];
	size_t off = 0;
	int n = snprintf(out, sizeof(out),
		"{\"mode\":\"%s\",\"rows\":%u,\"columns\":%u,\"corners\":[",
		data.soundboardLayout == SOUNDBOARD_LAYOUT_GRID ? "grid" : "normal",
		(unsigned)data.gridRows, (unsigned)data.gridColumns);
	if (n < 0 || (size_t)n >= sizeof(out)) {
		static const char err[] = "Could not encode the soundboard layout.\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	off = (size_t)n;
	for (uint8_t i = 0; i < SOUNDBOARD_GRID_CORNER_COUNT; ++i) {
		const bool present = (data.gridCornerMask & (uint8_t)(1U << i)) != 0;
		n = snprintf(out + off, sizeof(out) - off, "%s%s",
			i ? "," : "", present ? "" : "null");
		if (present) n = snprintf(out + off, sizeof(out) - off, "%s%u",
			i ? "," : "", (unsigned)data.gridCornerNotes[i]);
		if (n < 0 || (size_t)n >= sizeof(out) - off) {
			static const char err[] = "Could not encode the soundboard layout.\n";
			return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
		}
		off += (size_t)n;
	}
	n = snprintf(out + off, sizeof(out) - off, "],\"cells\":[");
	if (n < 0 || (size_t)n >= sizeof(out) - off) {
		static const char err[] = "Could not encode the soundboard layout.\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	off += (size_t)n;
	for (uint32_t i = 0; i < cellCount; ++i) {
		const std::string escapedSfx = audiox::json::escape(cells[i].sfxPath);
		n = snprintf(out + off, sizeof(out) - off, "%s{\"sfx\":\"%s\",\"row\":%u,\"column\":%u}",
			i ? "," : "", escapedSfx.c_str(), (unsigned)cells[i].row, (unsigned)cells[i].column);
		if (n < 0 || (size_t)n >= sizeof(out) - off) {
			static const char err[] = "Could not encode the soundboard layout.\n";
			return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
		}
		off += (size_t)n;
	}
	n = snprintf(out + off, sizeof(out) - off, "]}\n");
	if (n < 0 || (size_t)n >= sizeof(out) - off) {
		static const char err[] = "Could not encode the soundboard layout.\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	off += (size_t)n;
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, off);
}

static int handleSoundboardModes(HttpServer *server,
								 int cfd,
								 const char *method,
								 const char *path) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method) {
		return -1;
	}

	if (strcmp(method, "GET") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	MidiMapData data = server->app->config->readMidiMapFile();
	char out[HTTP_BODY_MAX];
	size_t off = 0;

	int n = snprintf(out + off, sizeof(out) - off, "{\"count\":%u,\"modes\":[", (unsigned)data.soundModeCount);
	if (n <= 0 || (size_t)n >= sizeof(out) - off) {
		static const char err[] = "encode failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	off += (size_t)n;

	uint32_t count = data.soundModeCount;
	if (count > MIDI_SOUND_MODES_MAX) {
		count = MIDI_SOUND_MODES_MAX;
	}
	int first = 1;
	for (uint32_t i = 0; i < count; ++i) {
		if (!data.soundModes[i].sfxPath[0]) {
			continue;
		}
		n = snprintf(out + off,
					 sizeof(out) - off,
					 "%s{\"sfx\":\"%s\",\"mode\":\"%s\"}",
					 first ? "" : ",",
					 data.soundModes[i].sfxPath,
					 soundboardModeToString(data.soundModes[i].mode));
		if (n <= 0 || (size_t)n >= sizeof(out) - off) {
			break;
		}
		off += (size_t)n;
		first = 0;
	}

	n = snprintf(out + off, sizeof(out) - off, "]}\n");
	if (n > 0 && (size_t)n < sizeof(out) - off) {
		off += (size_t)n;
	}

	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, off);
}

static int handleSoundboardModeSet(HttpServer *server,
								   int cfd,
								   const char *method,
								   const char *path,
								   const char *body,
								   size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method || !body) {
		return -1;
	}

	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char sfx_buf[MIDI_SFX_PATH_MAX];
	char mode_buf[24];
	if (!body_get_value(body, body_len, "sfx", sfx_buf, sizeof(sfx_buf)) ||
		!body_get_value(body, body_len, "mode", mode_buf, sizeof(mode_buf))) {
		static const char bad[] = "expected sfx and mode\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char normalized[MIDI_SFX_PATH_MAX];
	if (mapping_normalize_sfx(sfx_buf, normalized, sizeof(normalized)) != RET_OK) {
		static const char bad[] = "invalid sfx path\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char modeLower[24];
	size_t ml = strnlen(mode_buf, sizeof(modeLower) - 1);
	for (size_t i = 0; i < ml; ++i) {
		modeLower[i] = (char)tolower((unsigned char)mode_buf[i]);
	}
	modeLower[ml] = '\0';
	if (strcmp(modeLower, "play") != 0 && strcmp(modeLower, "hold") != 0) {
		static const char bad[] = "invalid mode\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}
	uint8_t mode = soundboardModeFromString(modeLower);

	MidiMapData data = server->app->config->readMidiMapFile();
	uint32_t count = data.soundModeCount;
	if (count > MIDI_SOUND_MODES_MAX) {
		count = MIDI_SOUND_MODES_MAX;
	}

	int idx = -1;
	for (uint32_t i = 0; i < count; ++i) {
		if (strcmp(data.soundModes[i].sfxPath, normalized) == 0) {
			idx = (int)i;
			break;
		}
	}

	if (mode == SOUNDBOARD_MODE_PLAY) {
		if (idx >= 0) {
			for (uint32_t i = (uint32_t)idx; i + 1 < count; ++i) {
				data.soundModes[i] = data.soundModes[i + 1];
			}
			data.soundModeCount = (count > 0) ? (count - 1U) : 0U;
		}
	} else {
		if (idx >= 0) {
			data.soundModes[idx].mode = mode;
			copy_bound(data.soundModes[idx].sfxPath, sizeof(data.soundModes[idx].sfxPath), normalized);
		} else {
			if (count >= MIDI_SOUND_MODES_MAX) {
				static const char full[] = "sound mode table full\n";
				return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8", full, sizeof(full) - 1);
			}
			copy_bound(data.soundModes[count].sfxPath, sizeof(data.soundModes[count].sfxPath), normalized);
			data.soundModes[count].mode = mode;
			data.soundModeCount = count + 1U;
		}
	}

	if (server->app->config->writeMidiMapFile(&data) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	if (server->app->midi) {
		server->app->midi->cachedMidiMap = data;
	}

	char out[128];
	int n = snprintf(out, sizeof(out), "{\"ok\":true,\"sfx\":\"%s\",\"mode\":\"%s\"}\n", normalized, soundboardModeToString(mode));
	if (n < 0) {
		n = 0;
	}
	if ((size_t)n >= sizeof(out)) {
		n = (int)(sizeof(out) - 1);
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleAudioDevices(HttpServer *server,
						 int cfd,
						 const char *method,
						 const char *path) {
	(void)path;
	if (!server || !server->app || !method) {
		return -1;
	}

	if (strcmp(method, "GET") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	if (!server->app->audio) {
		static const char err[] = "{\"ok\":false,\"error\":\"audio unavailable\"}\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}

	char out[HTTP_BODY_MAX];
	if (server->app->audio->buildDevicesJson(out, sizeof(out)) != RET_OK) {
		static const char err[] = "{\"ok\":false,\"error\":\"encode failed\"}\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}

	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, strlen(out));
}

static int handleAudioMeters(HttpServer *server, int cfd, const char *method) {
	if (!server || !server->app || !method) {
		return -1;
	}
	if (strcmp(method, "GET") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}
	if (!server->app->audio) {
		static const char err[] = "audio unavailable\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	char out[HTTP_BODY_MAX];
	if (server->app->audio->buildMetersJson(out, sizeof(out)) != RET_OK) {
		static const char err[] = "encode failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, strlen(out));
}

static int handleAudioRescan(HttpServer *server,
						int cfd,
						const char *method,
						const char *path) {
	(void)path;
	if (!server || !server->app || !method) {
		return -1;
	}

	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	if (!server->app->audio) {
		static const char err[] = "{\"ok\":false,\"error\":\"audio unavailable\"}\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}

	int rc = server->app->audio->forceRescan();
	if (rc == RET_ERR) {
		static const char err[] = "{\"ok\":false,\"error\":\"rescan failed\"}\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}

	char out[HTTP_BODY_MAX];
	if (server->app->audio->buildDevicesJson(out, sizeof(out)) != RET_OK) {
		static const char err[] = "{\"ok\":false,\"error\":\"encode failed\"}\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}

	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, strlen(out));
}

static void copy_bound(char *dst, size_t dst_sz, const char *src) {
	if (!dst || dst_sz == 0) {
		return;
	}
	if (!src) {
		dst[0] = '\0';
		return;
	}
	size_t n = strnlen(src, dst_sz - 1);
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static void trim_space(char *s) {
	if (!s) {
		return;
	}

	while (*s && isspace((unsigned char)*s)) {
		memmove(s, s + 1, strlen(s));
	}

	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1])) {
		s[n - 1] = '\0';
		--n;
	}
}

static int body_get_value(const char *body,
						  size_t body_len,
						  const char *key,
						  char *out,
						  size_t out_sz) {
	if (!body || !key || !out || out_sz == 0) {
		return 0;
	}

	char buf[512];
	if (body_len >= sizeof(buf)) {
		return 0;
	}
	memcpy(buf, body, body_len);
	buf[body_len] = '\0';

	for (char *p = buf; *p; ++p) {
		if (*p == '&') {
			*p = '\n';
		}
	}

	char *savep = NULL;
	for (char *line = strtok_r(buf, "\n", &savep); line; line = strtok_r(NULL, "\n", &savep)) {
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		char *k = line;
		char *v = eq + 1;
		trim_space(k);
		trim_space(v);
		if (strcmp(k, key) != 0) {
			continue;
		}
		copy_bound(out, out_sz, v);
		return 1;
	}

	return 0;
}

static int mapping_path_safe(const char *s) {
	if (!s || !s[0]) {
		return 0;
	}
	if (strstr(s, "..") != NULL) {
		return 0;
	}
	if (strchr(s, '\\') != NULL) {
		return 0;
	}
	for (const char *p = s; *p; ++p) {
		unsigned char c = (unsigned char)*p;
		if ((c >= 'a' && c <= 'z') ||
			(c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') ||
			c == '.' || c == '_' || c == '-' || c == '/') {
			continue;
		}
		return 0;
	}
	return 1;
}

static int mapping_normalize_sfx(const char *raw, char *out, size_t out_sz) {
	if (!raw || !out || out_sz == 0 || !mapping_path_safe(raw)) {
		return RET_ERR;
	}

	if (strncmp(raw, SFX_ROOT_DIR "/", strlen(SFX_ROOT_DIR) + 1) == 0) {
		copy_bound(out, out_sz, raw + strlen(SFX_ROOT_DIR) + 1);
		return out[0] ? RET_OK : RET_ERR;
	}

	if (raw[0] == '/') {
		return RET_ERR;
	}

	copy_bound(out, out_sz, raw);
	return out[0] ? RET_OK : RET_ERR;
}

static uint8_t midi_action_from_string(const char *value) {
	if (!value) {
		return MIDI_ACTION_NONE;
	}
	if (strcmp(value, "stop_all") == 0) {
		return MIDI_ACTION_STOP_ALL;
	}
	if (strcmp(value, "sampler_toggle") == 0) {
		return MIDI_ACTION_SAMPLER_TOGGLE;
	}
	return MIDI_ACTION_NONE;
}

static const char *midi_action_to_string(uint8_t action) {
	switch (action) {
		case MIDI_ACTION_STOP_ALL:
			return "stop_all";
		case MIDI_ACTION_SAMPLER_TOGGLE:
			return "sampler_toggle";
		default:
			return NULL;
	}
}

static int effect_id_valid(const char *effectId) {
	if (!effectId || !effectId[0]) {
		return 0;
	}
	if (strncmp(effectId, "fx_", 3) != 0) {
		return 0;
	}
	for (const char *p = effectId + 3; *p; ++p) {
		if (((*p < '0' || *p > '9') &&
			 (*p < 'a' || *p > 'z') &&
			 (*p < 'A' || *p > 'Z')) &&
			*p != '_') {
			return 0;
		}
	}
	return 1;
}

static int effect_param_valid(const char *param) {
	if (!param || !param[0]) {
		return 0;
	}
	for (const char *p = param; *p; ++p) {
		if (((*p >= 'a' && *p <= 'z') ||
			 (*p >= 'A' && *p <= 'Z') ||
			 (*p >= '0' && *p <= '9') ||
			 *p == '_') == 0) {
			return 0;
		}
	}
	return 1;
}

static void upsertEffectStateFromRuntime(MidiMapData *map,
										 const char *effectId,
										 const audiox::effects::SlotParams &params) {
	if (!map || !effectId || !effectId[0]) {
		return;
	}

	uint32_t count = map->effectStateCount;
	if (count > MIDI_EFFECT_STATES_MAX) {
		count = MIDI_EFFECT_STATES_MAX;
	}

	int idx = -1;
	for (uint32_t i = 0; i < count; ++i) {
		if (strcmp(map->effectStates[i].effectId, effectId) == 0) {
			idx = (int)i;
			break;
		}
	}
	if (idx < 0) {
		if (count >= MIDI_EFFECT_STATES_MAX) {
			return;
		}
		idx = (int)count;
		memset(&map->effectStates[idx], 0, sizeof(map->effectStates[idx]));
		copy_bound(map->effectStates[idx].effectId, sizeof(map->effectStates[idx].effectId), effectId);
		map->effectStateCount = count + 1U;
	}

	MidiEffectState &state = map->effectStates[(uint32_t)idx];
	copy_bound(state.type, sizeof(state.type), audiox::effects::effectTypeToString(params.type));
	state.enabled = params.enabled ? 1U : 0U;
	if (audiox::plugin::isPluginType(params.type) && !audiox::plugin::metadataFor(params.type)) return;

	float value = 0.0f;
	state.gain = (audiox::effects::getSlotParamValue(params, "gain", &value) == RET_OK) ? value : 1.0f;
	state.drive = (audiox::effects::getSlotParamValue(params, "drive", &value) == RET_OK) ? value : 1.0f;
	state.clip = (audiox::effects::getSlotParamValue(params, "clip", &value) == RET_OK) ? value : 1.0f;
	state.output = (audiox::effects::getSlotParamValue(params, "output", &value) == RET_OK) ? value : 1.0f;

	uint32_t writeCount = 0;
	uint32_t oldCount = map->effectParamCount;
	if (oldCount > MIDI_EFFECT_PARAMS_MAX) {
		oldCount = MIDI_EFFECT_PARAMS_MAX;
	}
	for (uint32_t i = 0; i < oldCount; ++i) {
		const MidiEffectParam &p = map->effectParams[i];
		if (strcmp(p.effectId, effectId) == 0) {
			continue;
		}
		if (writeCount >= MIDI_EFFECT_PARAMS_MAX) {
			break;
		}
		map->effectParams[writeCount++] = p;
	}

	const audiox::effects::EffectTypeSpec *spec = audiox::effects::effectTypeSpecFor(params.type);
	if (spec) {
		uint8_t paramCount = spec->paramCount;
		if (paramCount > audiox::effects::EFFECT_PARAM_MAX) {
			paramCount = audiox::effects::EFFECT_PARAM_MAX;
		}
		for (uint8_t i = 0; i < paramCount; ++i) {
			if (writeCount >= MIDI_EFFECT_PARAMS_MAX) {
				break;
			}
			MidiEffectParam &dst = map->effectParams[writeCount++];
			memset(&dst, 0, sizeof(dst));
			copy_bound(dst.effectId, sizeof(dst.effectId), effectId);
			copy_bound(dst.param, sizeof(dst.param), spec->params[i].name);
			dst.value = params.values[i];
		}
	}
	map->effectParamCount = writeCount;
}

static int handleMidiSamplerMode(HttpServer *server, int cfd, const char *method) {
	if (!server || !server->app || !method) return -1;
	if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0)
		return sendMethodNotAllowed(server, cfd);
	if (!server->app->midi) {
		static const char err[] = "MIDI sampler is unavailable.\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (strcmp(method, "POST") == 0) server->app->midi->toggleSamplerMode();
	const char *out = server->app->midi->samplerModeActive ? "{\"active\":true}\n" : "{\"active\":false}\n";
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, strlen(out));
}

static int handleMidiSamplerConfig(HttpServer *server,
								 int cfd,
								 const char *method,
								 const char *path,
								 const char *body,
								 size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method) {
		return -1;
	}

	if (strcmp(method, "GET") == 0) {
		MidiMapData data = server->app->config->readMidiMapFile();
		char out[256];
		int n = snprintf(out,
						 sizeof(out),
						 "{\"keyboard_enabled\":%s,\"keyboard_channel\":%u,\"root_note\":%u}\n",
						 data.samplerKeyboardEnabled ? "true" : "false",
						 (unsigned)(data.samplerKeyboardChannel & 0x0Fu),
						 (unsigned)data.samplerRootNote);
		if (n < 0 || (size_t)n >= sizeof(out)) {
			n = 0;
		}
		return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
	}

	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}
	if (!body) {
		static const char bad[] = "missing body\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	MidiMapData data = server->app->config->readMidiMapFile();
	char enabled_buf[16];
	char channel_buf[16];
	char root_buf[16];

	if (body_get_value(body, body_len, "keyboard_enabled", enabled_buf, sizeof(enabled_buf))) {
		char *ep = NULL;
		long v = strtol(enabled_buf, &ep, 10);
		if (ep && *ep == '\0') {
			data.samplerKeyboardEnabled = (v != 0) ? 1U : 0U;
		}
	}
	if (body_get_value(body, body_len, "keyboard_channel", channel_buf, sizeof(channel_buf))) {
		char *ep = NULL;
		long v = strtol(channel_buf, &ep, 10);
		if (ep && *ep == '\0' && v >= 0 && v <= 15) {
			data.samplerKeyboardChannel = (uint8_t)v;
		}
	}
	if (body_get_value(body, body_len, "root_note", root_buf, sizeof(root_buf))) {
		char *ep = NULL;
		long v = strtol(root_buf, &ep, 10);
		if (ep && *ep == '\0' && v >= 0 && v <= 127) {
			data.samplerRootNote = (uint8_t)v;
		}
	}

	if (server->app->config->writeMidiMapFile(&data) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	if (server->app->midi) {
		server->app->midi->cachedMidiMap = data;
	}

	char out[256];
	int n = snprintf(out,
					 sizeof(out),
					 "{\"ok\":true,\"keyboard_enabled\":%s,\"keyboard_channel\":%u,\"root_note\":%u}\n",
					 data.samplerKeyboardEnabled ? "true" : "false",
					 (unsigned)(data.samplerKeyboardChannel & 0x0Fu),
					 (unsigned)data.samplerRootNote);
	if (n < 0 || (size_t)n >= sizeof(out)) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleMidiLastCc(HttpServer *server,
						   int cfd,
						   const char *method,
						   const char *path) {
	(void)path;
	if (!server || !server->app || !method) {
		return -1;
	}
	if (strcmp(method, "GET") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	MidiContext *midi = server->app->midi;
	if (!midi) {
		static const char none[] = "{\"connected\":false,\"last_cc\":-1,\"last_cc_value\":0,\"last_seq\":0}\n";
		return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", none, sizeof(none) - 1);
	}

	char out[256];
	int n = snprintf(out,
					 sizeof(out),
					 "{\"connected\":%s,\"last_cc\":%d,\"last_cc_value\":%u,\"last_seq\":%u}\n",
					 midi->connected ? "true" : "false",
					 midi->lastCcSeq ? (int)midi->lastCc : -1,
					 (unsigned)midi->lastCcValue,
					 (unsigned)midi->lastCcSeq);
	if (n < 0 || (size_t)n >= sizeof(out)) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleAudioVolumes(HttpServer *server,
							  int cfd,
							  const char *method,
							  const char *path) {
	(void)path;
	if (!server || !server->app || !method) {
		return -1;
	}
	if (strcmp(method, "GET") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	AudioContext *audio = server->app->audio;
	if (!audio) {
		static const char none[] = "{\"volumes\":[]}\n";
		return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", none, sizeof(none) - 1);
	}

	AudioGraphThingInfo things[AUDIO_GRAPH_MAX_THINGS];
	size_t thingCount = audio->copyRoutingThings(things, AUDIO_GRAPH_MAX_THINGS);

	char out[HTTP_BODY_MAX];
	size_t off = 0;
	int n = snprintf(out, sizeof(out), "{\"volumes\":[");
	if (n <= 0 || (size_t)n >= sizeof(out)) {
		static const char err[] = "encode failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	off += (size_t)n;

	int first = 1;
	for (size_t i = 0; i < thingCount; ++i) {
		if (!things[i].id[0]) {
			continue;
		}
		float gain = audio->getThingGain(things[i].id);
		n = snprintf(out + off,
					 sizeof(out) - off,
					 "%s{\"id\":\"%s\",\"name\":\"%s\",\"gain\":%.4f}",
					 first ? "" : ",",
					 audiox::json::escape(things[i].id).c_str(),
					 audiox::json::escape(things[i].name).c_str(),
					 (double)gain);
		if (n <= 0 || (size_t)n >= sizeof(out) - off) {
			static const char err[] = "encode failed\n";
			return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
		}
		off += (size_t)n;
		first = 0;
	}

	n = snprintf(out + off, sizeof(out) - off, "]}\n");
	if (n <= 0 || (size_t)n >= sizeof(out) - off) {
		static const char err[] = "encode failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	off += (size_t)n;

	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, off);
}

static int handleAudioVolumeSet(HttpServer *server,
								int cfd,
								const char *method,
								const char *path,
								const char *body,
								size_t body_len) {
	(void)path;
	if (!server || !server->app || !method) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}
	if (!server->app->audio || !server->app->config) {
		static const char err[] = "audio subsystem unavailable\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	char idBuf[64];
	char gainBuf[32];
	if (!body ||
	    !body_get_value(body, body_len, "id", idBuf, sizeof(idBuf)) ||
	    !body_get_value(body, body_len, "gain", gainBuf, sizeof(gainBuf))) {
		static const char bad[] = "expected id=...&gain=0.0-1.0\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	if (!idBuf[0] || !mapping_path_safe(idBuf)) {
		static const char bad[] = "invalid id\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char *endp = NULL;
	double gainVal = strtod(gainBuf, &endp);
	if (!endp || endp == gainBuf || *endp != '\0' || !std::isfinite(gainVal) || gainVal < 0.0 || gainVal > 2.0) {
		static const char bad[] = "gain must be 0.0-2.0\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}
	float gain = (float)gainVal;

	// Persist to volumes.txt.
	VolumeEntry entries[VOLUME_ENTRIES_MAX];
	uint32_t count = 0;
	if (server->app->config->readVolumesFile(entries, &count, VOLUME_ENTRIES_MAX) == RET_ERR) {
		static const char err[] = "failed to read volumes\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	int found = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (strcmp(entries[i].thingId, idBuf) == 0) {
			entries[i].gain = gain;
			found = 1;
			break;
		}
	}
	if (!found && count >= VOLUME_ENTRIES_MAX) {
		static const char err[] = "volume table full\n";
		return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (!found) {
		copy_bound(entries[count].thingId, sizeof(entries[count].thingId), idBuf);
		entries[count].gain = gain;
		++count;
	}
	if (server->app->config->writeVolumesFile(entries, count) != RET_OK) {
		static const char err[] = "failed to save volumes\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	server->app->audio->setThingGain(idBuf, gain);

	char out[128];
	int n = snprintf(out, sizeof(out), "{\"ok\":true,\"id\":\"%s\",\"gain\":%.4f}\n", idBuf, (double)gain);
	if (n < 0 || (size_t)n >= sizeof(out)) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleMidiCcVolumes(HttpServer *server,
							   int cfd,
							   const char *method,
							   const char *path) {
	(void)path;
	if (!server || !server->app || !method || !server->app->config) {
		return -1;
	}
	if (strcmp(method, "GET") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	MidiMapData data = server->app->config->readMidiMapFile();
	char out[HTTP_BODY_MAX];
	size_t off = 0;
	int n = snprintf(out, sizeof(out), "{\"mappings\":[");
	if (n <= 0 || (size_t)n >= sizeof(out)) {
		static const char err[] = "encode failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	off += (size_t)n;

	uint32_t count = data.ccVolumeMappingCount;
	if (count > MIDI_CC_VOLUME_MAPPINGS_MAX) {
		count = MIDI_CC_VOLUME_MAPPINGS_MAX;
	}
	int first = 1;
	for (uint32_t i = 0; i < count; ++i) {
		if (!data.ccVolumeMappings[i].thingId[0]) {
			continue;
		}
		n = snprintf(out + off,
					 sizeof(out) - off,
					 "%s{\"cc\":%u,\"id\":\"%s\"}",
					 first ? "" : ",",
					 (unsigned)data.ccVolumeMappings[i].cc,
					 data.ccVolumeMappings[i].thingId);
		if (n <= 0 || (size_t)n >= sizeof(out) - off) {
			break;
		}
		off += (size_t)n;
		first = 0;
	}

	n = snprintf(out + off, sizeof(out) - off, "]}\n");
	if (n > 0 && (size_t)n < sizeof(out) - off) {
		off += (size_t)n;
	}

	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, off);
}

static int handleMidiCcVolumeSet(HttpServer *server,
								 int cfd,
								 const char *method,
								 const char *path,
								 const char *body,
								 size_t body_len) {
	(void)path;
	if (!server || !server->app || !method || !server->app->config) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char ccBuf[16];
	char idBuf[64];
	if (!body ||
	    !body_get_value(body, body_len, "cc", ccBuf, sizeof(ccBuf)) ||
	    !body_get_value(body, body_len, "id", idBuf, sizeof(idBuf))) {
		static const char bad[] = "expected cc=0-127&id=...\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char *endp = NULL;
	long cc = strtol(ccBuf, &endp, 10);
	if (!endp || *endp != '\0' || cc < 0 || cc > 127) {
		static const char bad[] = "cc must be 0-127\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}
	if (!idBuf[0] || !mapping_path_safe(idBuf)) {
		static const char bad[] = "invalid id\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	MidiMapData data = server->app->config->readMidiMapFile();

	// Remove any existing mapping for this cc or this id.
	uint32_t out_count = 0;
	for (uint32_t i = 0; i < data.ccVolumeMappingCount && i < MIDI_CC_VOLUME_MAPPINGS_MAX; ++i) {
		if (data.ccVolumeMappings[i].cc == (uint8_t)cc ||
		    strcmp(data.ccVolumeMappings[i].thingId, idBuf) == 0) {
			continue;
		}
		data.ccVolumeMappings[out_count++] = data.ccVolumeMappings[i];
	}
	if (out_count < MIDI_CC_VOLUME_MAPPINGS_MAX) {
		data.ccVolumeMappings[out_count].cc = (uint8_t)cc;
		copy_bound(data.ccVolumeMappings[out_count].thingId,
				   sizeof(data.ccVolumeMappings[out_count].thingId),
				   idBuf);
		++out_count;
	}
	data.ccVolumeMappingCount = out_count;

	if (server->app->config->writeMidiMapFile(&data) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (server->app->midi) {
		server->app->midi->cachedMidiMap = data;
	}

	char outBuf[128];
	int n = snprintf(outBuf, sizeof(outBuf), "{\"ok\":true,\"cc\":%u,\"id\":\"%s\"}\n", (unsigned)cc, idBuf);
	if (n < 0 || (size_t)n >= sizeof(outBuf)) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", outBuf, (size_t)n);
}

static int handleMidiCcVolumeDelete(HttpServer *server,
									int cfd,
									const char *method,
									const char *path,
									const char *body,
									size_t body_len) {
	(void)path;
	if (!server || !server->app || !method || !server->app->config) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char ccBuf[16];
	if (!body || !body_get_value(body, body_len, "cc", ccBuf, sizeof(ccBuf))) {
		static const char bad[] = "expected cc=0-127\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char *endp = NULL;
	long cc = strtol(ccBuf, &endp, 10);
	if (!endp || *endp != '\0' || cc < 0 || cc > 127) {
		static const char bad[] = "cc must be 0-127\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	MidiMapData data = server->app->config->readMidiMapFile();

	uint32_t out_count = 0;
	for (uint32_t i = 0; i < data.ccVolumeMappingCount && i < MIDI_CC_VOLUME_MAPPINGS_MAX; ++i) {
		if (data.ccVolumeMappings[i].cc == (uint8_t)cc) {
			continue;
		}
		data.ccVolumeMappings[out_count++] = data.ccVolumeMappings[i];
	}
	data.ccVolumeMappingCount = out_count;

	if (server->app->config->writeMidiMapFile(&data) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (server->app->midi) {
		server->app->midi->cachedMidiMap = data;
	}

	char outBuf[64];
	int n = snprintf(outBuf, sizeof(outBuf), "{\"ok\":true,\"cc\":%u}\n", (unsigned)cc);
	if (n < 0 || (size_t)n >= sizeof(outBuf)) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", outBuf, (size_t)n);
}

static int handleEffects(HttpServer *server,
						 int cfd,
						 const char *method,
						 const char *path) {
	(void)path;
	if (!server || !server->app || !method) {
		return -1;
	}
	if (strcmp(method, "GET") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}
	if (!server->app->audio || !server->app->config) {
		static const char none[] = "{\"effects\":[]}\n";
		return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", none, sizeof(none) - 1);
	}

	AudioEffectSlotState slots[AUDIO_GRAPH_MAX_THINGS] = {};
	int slotCount = server->app->audio->listEffects(slots, AUDIO_GRAPH_MAX_THINGS);
	if (slotCount < 0) {
		slotCount = 0;
	}

	MidiMapData map = server->app->config->readMidiMapFile();

	char out[AUDIO_GRAPH_MAX_THINGS * 8192U];
	size_t off = 0;
	int n = snprintf(out + off, sizeof(out) - off, "{\"effects\":[");
	if (n <= 0 || (size_t)n >= sizeof(out) - off) {
		static const char err[] = "encode failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	off += (size_t)n;

	int first = 1;
    int encodedEffects = 0;
	for (int i = 0; i < slotCount; ++i) {
		audiox::effects::SlotParams params = {};
		if (server->app->audio->getEffectParams(slots[i].thingId, &params) != RET_OK) {
			continue;
		}

		int toggleNote = -1;
		uint32_t toggleCount = map.effectToggleMappingCount;
		if (toggleCount > MIDI_EFFECT_TOGGLE_MAPPINGS_MAX) {
			toggleCount = MIDI_EFFECT_TOGGLE_MAPPINGS_MAX;
		}
		for (uint32_t t = 0; t < toggleCount; ++t) {
			if (strcmp(map.effectToggleMappings[t].effectId, slots[i].thingId) == 0) {
				toggleNote = (int)map.effectToggleMappings[t].note;
				break;
			}
		}

		uint8_t lightEnabledVel = map.globalLight.playingVel;
		uint8_t lightBypassedVel = map.globalLight.mappedVel;
		uint32_t lightCount = map.effectLightMappingCount;
		if (lightCount > MIDI_EFFECT_LIGHT_MAPPINGS_MAX) {
			lightCount = MIDI_EFFECT_LIGHT_MAPPINGS_MAX;
		}
		for (uint32_t l = 0; l < lightCount; ++l) {
			if (strcmp(map.effectLightMappings[l].effectId, slots[i].thingId) == 0) {
				lightEnabledVel = map.effectLightMappings[l].enabledVel;
				lightBypassedVel = map.effectLightMappings[l].bypassedVel;
				break;
			}
		}

		int ccByParam[audiox::effects::EFFECT_PARAM_MAX];
		for (uint8_t p = 0; p < audiox::effects::EFFECT_PARAM_MAX; ++p) {
			ccByParam[p] = -1;
		}
		uint32_t ccCount = map.effectCcMappingCount;
		if (ccCount > MIDI_EFFECT_CC_MAPPINGS_MAX) {
			ccCount = MIDI_EFFECT_CC_MAPPINGS_MAX;
		}
		const audiox::effects::EffectTypeSpec *typeSpec = audiox::effects::effectTypeSpecFor(params.type);
		uint8_t paramCount = typeSpec ? typeSpec->paramCount : 0;
		if (paramCount > audiox::effects::EFFECT_PARAM_MAX) {
			paramCount = audiox::effects::EFFECT_PARAM_MAX;
		}
		for (uint32_t c = 0; c < ccCount; ++c) {
			const MidiEffectCcMapping &m = map.effectCcMappings[c];
			if (strcmp(m.effectId, slots[i].thingId) != 0) {
				continue;
			}
			uint8_t resolvedIdx = 0;
			if (audiox::effects::effectParamSpecFor(params.type, m.param, &resolvedIdx) != nullptr &&
				resolvedIdx < paramCount) {
				ccByParam[resolvedIdx] = (int)m.cc;
			}
		}

		char displayName[64]{};
		midiEffectDisplayName(map, slots[i].thingId, audiox::effects::effectTypeToString(params.type),
			displayName, sizeof(displayName));
		const char *customName = "";
		for (uint32_t s = 0; s < map.effectStateCount; ++s)
			if (!strcmp(map.effectStates[s].effectId, slots[i].thingId)) customName = map.effectStates[s].name;
		n = snprintf(out + off,
					 sizeof(out) - off,
					 "%s{\"id\":\"%s\",\"name\":\"%s\",\"custom_name\":\"%s\",\"type\":\"%s\",\"enabled\":%s,\"params\":{",
					 first ? "" : ",",
					 slots[i].thingId,
					 audiox::json::escape(displayName).c_str(),
					 audiox::json::escape(customName).c_str(),
					 audiox::effects::effectTypeToString(params.type),
					 params.enabled ? "true" : "false");
		if (n <= 0 || (size_t)n >= sizeof(out) - off) {
			break;
		}
		off += (size_t)n;

		for (uint8_t p = 0; p < paramCount; ++p) {
			float value = 0.0f;
			if (audiox::effects::getSlotParamValue(params, typeSpec->params[p].name, &value) != RET_OK) {
				value = typeSpec->params[p].defaultValue;
			}
			n = snprintf(out + off,
						 sizeof(out) - off,
						 "%s\"%s\":%.9g",
						 (p == 0U) ? "" : ",",
						 typeSpec->params[p].name,
						 (double)value);
			if (n <= 0 || (size_t)n >= sizeof(out) - off) {
				off = sizeof(out);
				break;
			}
			off += (size_t)n;
		}
		if (off >= sizeof(out)) {
			break;
		}

		n = snprintf(out + off, sizeof(out) - off, "},\"param_meta\":[");
		if (n <= 0 || (size_t)n >= sizeof(out) - off) {
			break;
		}
		off += (size_t)n;

		for (uint8_t p = 0; p < paramCount; ++p) {
			n = snprintf(out + off,
						 sizeof(out) - off,
						 "%s{\"name\":\"%s\",\"label\":\"%s\",\"min\":%.9g,\"max\":%.9g,\"default\":%.9g}",
						 (p == 0U) ? "" : ",",
						 typeSpec->params[p].name,
						 audiox::json::escape(typeSpec->params[p].label).c_str(),
						 (double)typeSpec->params[p].minValue,
						 (double)typeSpec->params[p].maxValue,
						 (double)typeSpec->params[p].defaultValue);
			if (n <= 0 || (size_t)n >= sizeof(out) - off) {
				off = sizeof(out);
				break;
			}
			off += (size_t)n;
		}
		if (off >= sizeof(out)) {
			break;
		}

		n = snprintf(out + off, sizeof(out) - off, "],\"midi\":{\"toggle_note\":%d,\"cc\":{", toggleNote);
		if (n <= 0 || (size_t)n >= sizeof(out) - off) {
			break;
		}
		off += (size_t)n;

		for (uint8_t p = 0; p < paramCount; ++p) {
			n = snprintf(out + off,
						 sizeof(out) - off,
						 "%s\"%s\":%d",
						 (p == 0U) ? "" : ",",
						 typeSpec->params[p].name,
						 ccByParam[p]);
			if (n <= 0 || (size_t)n >= sizeof(out) - off) {
				off = sizeof(out);
				break;
			}
			off += (size_t)n;
		}
		if (off >= sizeof(out)) {
			break;
		}

		std::string pluginFields;
		if (audiox::plugin::isPluginType(params.type)) {
		    const bool ui = audiox::plugin::hasUi(params.type);
		    const auto status = params.pluginHost ? params.pluginHost->status() :
		        audiox::plugin::PluginHost::Status::Stopped;
		    const char *state = status == audiox::plugin::PluginHost::Status::Ready ? "ready" :
		        status == audiox::plugin::PluginHost::Status::Fault ? "fault" : "unavailable";
		    pluginFields = ",\"plugin_ui\":" + std::string(ui ? "true" : "false") +
		        ",\"plugin_status\":\"" + state + "\",\"plugin_error\":\"" +
		        audiox::json::escape(params.pluginHost ? params.pluginHost->error() : "Plugin worker unavailable") + "\"";
		}
		n = snprintf(out + off,
					 sizeof(out) - off,
					 "},\"light\":{\"enabled_vel\":%u,\"bypassed_vel\":%u}}%s}",
					 (unsigned)lightEnabledVel,
					 (unsigned)lightBypassedVel, pluginFields.c_str());
		if (n <= 0 || (size_t)n >= sizeof(out) - off) {
			break;
		}
		off += (size_t)n;
		first = 0;
        ++encodedEffects;
	}

    if (encodedEffects != slotCount) {
        static const char error[] = "effect response could not be encoded completely\n";
        return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", error, sizeof(error) - 1);
    }
	n = snprintf(out + off, sizeof(out) - off, "]}\n");
	if (n > 0 && (size_t)n < sizeof(out) - off) {
		off += (size_t)n;
	}

	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, off);
}

static int handleEffectName(HttpServer *server, int cfd, const char *method,
                            const char *body, size_t bodyLen) {
	if (!server || !server->app || !server->app->audio || !server->app->config) return -1;
	if (strcmp(method, "POST")) return sendMethodNotAllowed(server, cfd);
	auto reply = [&](const char *status, const char *text) {
		return server->sendResponse(cfd, status, "text/plain; charset=utf-8", text, strlen(text));
	};
	char id[64]{}, hex[512]{}, name[64]{};
	if (!body || bodyLen > 512 || memchr(body, '\0', bodyLen) ||
		!body_get_value(body, bodyLen, "id", id, sizeof(id)) || !effect_id_valid(id) ||
		!body_get_value(body, bodyLen, "name_hex", hex, sizeof(hex)) ||
		!scriptHexDecode(hex, name, sizeof(name)) || !midiEffectNameValid(name))
		return reply("400 Bad Request", "Name must be valid UTF-8, at most 63 bytes, without control characters.\n");
	audiox::effects::SlotParams params{};
	if (server->app->audio->getEffectParams(id, &params) != RET_OK)
		return reply("404 Not Found", "Effect not found.\n");
	auto map = server->app->config->readMidiMapFile();
	bool persisted = false;
	for (uint32_t i = 0; i < map.effectStateCount; ++i)
		if (!strcmp(map.effectStates[i].effectId, id)) persisted = true;
	if (!persisted) upsertEffectStateFromRuntime(&map, id, params);
	for (uint32_t i = 0; i < map.effectStateCount; ++i) {
		if (strcmp(map.effectStates[i].effectId, id)) continue;
		copy_bound(map.effectStates[i].name, sizeof(map.effectStates[i].name), name);
		if (server->app->config->writeMidiMapFile(&map) != RET_OK)
			return reply("500 Internal Server Error", "Could not save effect name.\n");
		if (server->app->midi) server->app->midi->cachedMidiMap = map;
		return reply("200 OK", "Effect name saved.\n");
	}
	return reply("409 Conflict", "Effect settings capacity reached.\n");
}

static int handleEffectSet(HttpServer *server,
						   int cfd,
						   const char *method,
						   const char *path,
						   const char *body,
						   size_t body_len) {
	(void)path;
	if (!server || !server->app || !method || !server->app->audio || !server->app->config) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}
	if (!body) {
		static const char bad[] = "missing body\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char idBuf[64] = {};
	if (!body_get_value(body, body_len, "id", idBuf, sizeof(idBuf)) || !effect_id_valid(idBuf)) {
		static const char bad[] = "invalid effect id\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}
	char paramBuf[24] = {};
	char valueBuf[32] = {};
	const bool hasParam = body_get_value(body, body_len, "param", paramBuf, sizeof(paramBuf));
	const bool hasValue = body_get_value(body, body_len, "value", valueBuf, sizeof(valueBuf));
	float value = 0.0f;
	if (hasParam != hasValue || (hasParam && !effect_param_valid(paramBuf))) {
		static const char bad[] = "invalid effect param\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}
	if (hasValue) {
		char *ep = nullptr;
		value = strtof(valueBuf, &ep);
		if (ep == valueBuf || *ep != '\0' || !std::isfinite(value)) {
			static const char bad[] = "invalid effect value\n";
			return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
		}
	}
	audiox::effects::SlotParams previousParams = {};
	const bool hadPreviousParams = server->app->audio->getEffectParams(idBuf, &previousParams) == RET_OK;
	int routingShapeChanged = 0;

	int changed = 0;
	char typeBuf[24] = {};
	if (body_get_value(body, body_len, "type", typeBuf, sizeof(typeBuf))) {
		uint8_t type = audiox::effects::effectTypeFromString(typeBuf);
		if (strcmp(typeBuf, audiox::effects::effectTypeToString(type)) ||
		    (audiox::plugin::isPluginType(type) && !audiox::plugin::available(type)))
		    return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8",
		        "unknown or unavailable effect type\n", 35);
		if (server->app->audio->setEffectType(idBuf, type) == RET_OK) {
			changed = 1;
			if (hadPreviousParams &&
				(previousParams.type == audiox::effects::EFFECT_CUT || type == audiox::effects::EFFECT_CUT)) {
				routingShapeChanged = 1;
			}
		} else return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8",
			"effect preparation failed; check device logs\n", 45);
	}

	char enabledBuf[16] = {};
	if (body_get_value(body, body_len, "enabled", enabledBuf, sizeof(enabledBuf))) {
		char *ep = NULL;
		long enabled = strtol(enabledBuf, &ep, 10);
		if (ep && *ep == '\0' && (enabled == 0 || enabled == 1)) {
			if (server->app->audio->setEffectEnabled(idBuf, (uint8_t)enabled) == RET_OK) {
				changed = 1;
			}
		}
	}

	if (hasParam) {
		if (server->app->audio->setEffectParam(idBuf, paramBuf, value) == RET_OK) {
			changed = 1;
			if (strcmp(paramBuf, "channels") == 0) {
				audiox::effects::SlotParams updatedParams = {};
				if (server->app->audio->getEffectParams(idBuf, &updatedParams) == RET_OK &&
					updatedParams.type == audiox::effects::EFFECT_CUT) {
					routingShapeChanged = 1;
				}
			}
		}
	}

	if (!changed) {
		static const char bad[] = "no effect fields updated\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	audiox::effects::SlotParams params = {};
	if (server->app->audio->getEffectParams(idBuf, &params) != RET_OK) {
		static const char err[] = "effect update failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	MidiMapData map = server->app->config->readMidiMapFile();
	upsertEffectStateFromRuntime(&map, idBuf, params);
	if (server->app->config->writeMidiMapFile(&map) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (server->app->midi) {
		server->app->midi->cachedMidiMap = map;
	}
	if (routingShapeChanged) {
		const int reloadRc = server->app->audio->reloadRoutingGraph();
		if (reloadRc == RET_ERR) {
			printf("[HTTP] [WARN] reloadRoutingGraph after Cut layout change failed\n");
		}
	}

	char out[1024];
	size_t off = 0;
	int n = snprintf(out,
					 sizeof(out),
					 "{\"ok\":true,\"id\":\"%s\",\"type\":\"%s\",\"enabled\":%s,\"params\":{",
					 idBuf,
					 audiox::effects::effectTypeToString(params.type),
					 params.enabled ? "true" : "false");
	if (n <= 0 || (size_t)n >= sizeof(out)) {
		n = 0;
		return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
	}
	off += (size_t)n;

	const audiox::effects::EffectTypeSpec *spec = audiox::effects::effectTypeSpecFor(params.type);
	uint8_t paramCount = spec ? spec->paramCount : 0;
	if (paramCount > audiox::effects::EFFECT_PARAM_MAX) {
		paramCount = audiox::effects::EFFECT_PARAM_MAX;
	}
	for (uint8_t p = 0; p < paramCount; ++p) {
		float v = 0.0f;
		if (audiox::effects::getSlotParamValue(params, spec->params[p].name, &v) != RET_OK) {
			v = spec->params[p].defaultValue;
		}
		n = snprintf(out + off,
					 sizeof(out) - off,
					 "%s\"%s\":%.9g",
					 (p == 0U) ? "" : ",",
					 spec->params[p].name,
					 (double)v);
		if (n <= 0 || (size_t)n >= sizeof(out) - off) {
			break;
		}
		off += (size_t)n;
	}

	n = snprintf(out + off, sizeof(out) - off, "}}\n");
	if (n > 0 && (size_t)n < sizeof(out) - off) {
		off += (size_t)n;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, off);
}

static int handleEffectCreate(HttpServer *server,
							 int cfd,
							 const char *method,
							 const char *path,
							 const char *body,
							 size_t body_len) {
	(void)path;
	if (!server || !server->app || !method || !server->app->audio || !server->app->config) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char typeBuf[24] = "gain";
	if (body) {
		(void)body_get_value(body, body_len, "type", typeBuf, sizeof(typeBuf));
	}

	char idBuf[64] = {};
	const uint8_t type = audiox::effects::effectTypeFromString(typeBuf);
	if (strcmp(typeBuf, audiox::effects::effectTypeToString(type)) ||
	    (audiox::plugin::isPluginType(type) && !audiox::plugin::available(type)))
	    return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8",
	        "unknown or unavailable effect type\n", 35);
	if (server->app->audio->createEffect(typeBuf, idBuf, sizeof(idBuf)) != RET_OK) {
		static const char err[] = "failed to create effect\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	audiox::effects::SlotParams params = {};
	if (server->app->audio->getEffectParams(idBuf, &params) != RET_OK) {
		static const char err[] = "failed to read created effect\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	MidiMapData map = server->app->config->readMidiMapFile();
	upsertEffectStateFromRuntime(&map, idBuf, params);
	if (server->app->config->writeMidiMapFile(&map) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (server->app->midi) {
		server->app->midi->cachedMidiMap = map;
	}

	if (server->app->audio) {
		int reloadRc = server->app->audio->reloadRoutingGraph();
		if (reloadRc != RET_OK) {
			printf("[HTTP] [WARN] reloadRoutingGraph after effect create failed (%d)\n", reloadRc);
		}
	}

	char out[192];
	int n = snprintf(out,
					 sizeof(out),
					 "{\"ok\":true,\"id\":\"%s\",\"type\":\"%s\"}\n",
					 idBuf,
					 audiox::effects::effectTypeToString(params.type));
	if (n < 0 || (size_t)n >= sizeof(out)) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleEffectDelete(HttpServer *server,
							 int cfd,
							 const char *method,
							 const char *path,
							 const char *body,
							 size_t body_len) {
	(void)path;
	if (!server || !server->app || !method || !server->app->audio || !server->app->config) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char idBuf[64] = {};
	if (!body || !body_get_value(body, body_len, "id", idBuf, sizeof(idBuf)) || !effect_id_valid(idBuf)) {
		static const char bad[] = "invalid effect id\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	if (server->app->audio->deleteEffect(idBuf) != RET_OK) {
		static const char nf[] = "effect not found\n";
		return server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8", nf, sizeof(nf) - 1);
	}

	MidiMapData map = server->app->config->readMidiMapFile();

	uint32_t outStateCount = 0;
	for (uint32_t i = 0; i < map.effectStateCount && i < MIDI_EFFECT_STATES_MAX; ++i) {
		if (strcmp(map.effectStates[i].effectId, idBuf) == 0) {
			continue;
		}
		map.effectStates[outStateCount++] = map.effectStates[i];
	}
	map.effectStateCount = outStateCount;

	uint32_t outCcCount = 0;
	for (uint32_t i = 0; i < map.effectCcMappingCount && i < MIDI_EFFECT_CC_MAPPINGS_MAX; ++i) {
		if (strcmp(map.effectCcMappings[i].effectId, idBuf) == 0) {
			continue;
		}
		map.effectCcMappings[outCcCount++] = map.effectCcMappings[i];
	}
	map.effectCcMappingCount = outCcCount;

	uint32_t outParamCount = 0;
	for (uint32_t i = 0; i < map.effectParamCount && i < MIDI_EFFECT_PARAMS_MAX; ++i) {
		if (strcmp(map.effectParams[i].effectId, idBuf) == 0) {
			continue;
		}
		map.effectParams[outParamCount++] = map.effectParams[i];
	}
	map.effectParamCount = outParamCount;

	uint32_t outToggleCount = 0;
	for (uint32_t i = 0; i < map.effectToggleMappingCount && i < MIDI_EFFECT_TOGGLE_MAPPINGS_MAX; ++i) {
		if (strcmp(map.effectToggleMappings[i].effectId, idBuf) == 0) {
			continue;
		}
		map.effectToggleMappings[outToggleCount++] = map.effectToggleMappings[i];
	}
	map.effectToggleMappingCount = outToggleCount;

	uint32_t outLightCount = 0;
	for (uint32_t i = 0; i < map.effectLightMappingCount && i < MIDI_EFFECT_LIGHT_MAPPINGS_MAX; ++i) {
		if (strcmp(map.effectLightMappings[i].effectId, idBuf) == 0) {
			continue;
		}
		map.effectLightMappings[outLightCount++] = map.effectLightMappings[i];
	}
	map.effectLightMappingCount = outLightCount;

	if (server->app->config->writeMidiMapFile(&map) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (server->app->midi) {
		server->app->midi->cachedMidiMap = map;
	}

	if (server->app->audio) {
		int reloadRc = server->app->audio->reloadRoutingGraph();
		if (reloadRc != RET_OK) {
			printf("[HTTP] [WARN] reloadRoutingGraph after effect delete failed (%d)\n", reloadRc);
		}
	}

	char out[96];
	int n = snprintf(out, sizeof(out), "{\"ok\":true,\"id\":\"%s\"}\n", idBuf);
	if (n < 0 || (size_t)n >= sizeof(out)) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleEffectMidiCcSet(HttpServer *server,
								 int cfd,
								 const char *method,
								 const char *path,
								 const char *body,
								 size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char ccBuf[16] = {};
	char idBuf[64] = {};
	char paramBuf[24] = {};
	if (!body ||
		!body_get_value(body, body_len, "cc", ccBuf, sizeof(ccBuf)) ||
		!body_get_value(body, body_len, "id", idBuf, sizeof(idBuf)) ||
		!body_get_value(body, body_len, "param", paramBuf, sizeof(paramBuf))) {
		static const char bad[] = "expected cc,id,param\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char *ep = NULL;
	long cc = strtol(ccBuf, &ep, 10);
	if (!ep || *ep != '\0' || cc < 0 || cc > 127 || !effect_id_valid(idBuf) || !effect_param_valid(paramBuf)) {
		static const char bad[] = "invalid cc/id/param\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	audiox::effects::SlotParams effectParams = {};
	if (!server->app->audio || server->app->audio->getEffectParams(idBuf, &effectParams) != RET_OK ||
		audiox::effects::effectParamSpecFor(effectParams.type, paramBuf, nullptr) == nullptr) {
		static const char bad[] = "unknown effect param for this effect type\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}
	if (effectParams.type == audiox::effects::EFFECT_CUT && strcmp(paramBuf, "channels") == 0) {
		static const char bad[] = "Cut channel count changes routing and cannot be MIDI-mapped\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	MidiMapData map = server->app->config->readMidiMapFile();
	uint32_t outCount = 0;
	for (uint32_t i = 0; i < map.effectCcMappingCount && i < MIDI_EFFECT_CC_MAPPINGS_MAX; ++i) {
		const MidiEffectCcMapping &m = map.effectCcMappings[i];
		if (m.cc == (uint8_t)cc ||
			(strcmp(m.effectId, idBuf) == 0 && strcmp(m.param, paramBuf) == 0)) {
			continue;
		}
		map.effectCcMappings[outCount++] = m;
	}
	if (outCount < MIDI_EFFECT_CC_MAPPINGS_MAX) {
		map.effectCcMappings[outCount].cc = (uint8_t)cc;
		copy_bound(map.effectCcMappings[outCount].effectId,
				   sizeof(map.effectCcMappings[outCount].effectId),
				   idBuf);
		copy_bound(map.effectCcMappings[outCount].param,
				   sizeof(map.effectCcMappings[outCount].param),
				   paramBuf);
		++outCount;
	}
	map.effectCcMappingCount = outCount;

	if (server->app->config->writeMidiMapFile(&map) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (server->app->midi) {
		server->app->midi->cachedMidiMap = map;
	}

	char out[128];
	int n = snprintf(out,
					 sizeof(out),
					 "{\"ok\":true,\"cc\":%u,\"id\":\"%s\",\"param\":\"%s\"}\n",
					 (unsigned)cc,
					 idBuf,
					 paramBuf);
	if (n < 0 || (size_t)n >= sizeof(out)) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleEffectMidiCcDelete(HttpServer *server,
									int cfd,
									const char *method,
									const char *path,
									const char *body,
									size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char ccBuf[16] = {};
	if (!body || !body_get_value(body, body_len, "cc", ccBuf, sizeof(ccBuf))) {
		static const char bad[] = "expected cc\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}
	char *ep = NULL;
	long cc = strtol(ccBuf, &ep, 10);
	if (!ep || *ep != '\0' || cc < 0 || cc > 127) {
		static const char bad[] = "invalid cc\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	MidiMapData map = server->app->config->readMidiMapFile();
	uint32_t outCount = 0;
	for (uint32_t i = 0; i < map.effectCcMappingCount && i < MIDI_EFFECT_CC_MAPPINGS_MAX; ++i) {
		if (map.effectCcMappings[i].cc == (uint8_t)cc) {
			continue;
		}
		map.effectCcMappings[outCount++] = map.effectCcMappings[i];
	}
	map.effectCcMappingCount = outCount;

	if (server->app->config->writeMidiMapFile(&map) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (server->app->midi) {
		server->app->midi->cachedMidiMap = map;
	}

	char out[64];
	int n = snprintf(out, sizeof(out), "{\"ok\":true,\"cc\":%u}\n", (unsigned)cc);
	if (n < 0 || (size_t)n >= sizeof(out)) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleEffectMidiToggleSet(HttpServer *server,
									 int cfd,
									 const char *method,
									 const char *path,
									 const char *body,
									 size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char noteBuf[16] = {};
	char idBuf[64] = {};
	if (!body ||
		!body_get_value(body, body_len, "note", noteBuf, sizeof(noteBuf)) ||
		!body_get_value(body, body_len, "id", idBuf, sizeof(idBuf))) {
		static const char bad[] = "expected note,id\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char *ep = NULL;
	long note = strtol(noteBuf, &ep, 10);
	if (!ep || *ep != '\0' || note < 0 || note > 127 || !effect_id_valid(idBuf)) {
		static const char bad[] = "invalid note/id\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	MidiMapData map = server->app->config->readMidiMapFile();
	uint32_t outCount = 0;
	for (uint32_t i = 0; i < map.effectToggleMappingCount && i < MIDI_EFFECT_TOGGLE_MAPPINGS_MAX; ++i) {
		const MidiEffectToggleMapping &m = map.effectToggleMappings[i];
		if (m.note == (uint8_t)note || strcmp(m.effectId, idBuf) == 0) {
			continue;
		}
		map.effectToggleMappings[outCount++] = m;
	}
	if (outCount < MIDI_EFFECT_TOGGLE_MAPPINGS_MAX) {
		map.effectToggleMappings[outCount].note = (uint8_t)note;
		copy_bound(map.effectToggleMappings[outCount].effectId,
				   sizeof(map.effectToggleMappings[outCount].effectId),
				   idBuf);
		++outCount;
	}
	map.effectToggleMappingCount = outCount;

	if (server->app->config->writeMidiMapFile(&map) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (server->app->midi) {
		server->app->midi->cachedMidiMap = map;
	}

	char out[96];
	int n = snprintf(out,
					 sizeof(out),
					 "{\"ok\":true,\"note\":%u,\"id\":\"%s\"}\n",
					 (unsigned)note,
					 idBuf);
	if (n < 0 || (size_t)n >= sizeof(out)) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleEffectMidiToggleDelete(HttpServer *server,
										int cfd,
										const char *method,
										const char *path,
										const char *body,
										size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char noteBuf[16] = {};
	if (!body || !body_get_value(body, body_len, "note", noteBuf, sizeof(noteBuf))) {
		static const char bad[] = "expected note\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char *ep = NULL;
	long note = strtol(noteBuf, &ep, 10);
	if (!ep || *ep != '\0' || note < 0 || note > 127) {
		static const char bad[] = "invalid note\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	MidiMapData map = server->app->config->readMidiMapFile();
	uint32_t outCount = 0;
	for (uint32_t i = 0; i < map.effectToggleMappingCount && i < MIDI_EFFECT_TOGGLE_MAPPINGS_MAX; ++i) {
		if (map.effectToggleMappings[i].note == (uint8_t)note) {
			continue;
		}
		map.effectToggleMappings[outCount++] = map.effectToggleMappings[i];
	}
	map.effectToggleMappingCount = outCount;

	if (server->app->config->writeMidiMapFile(&map) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (server->app->midi) {
		server->app->midi->cachedMidiMap = map;
	}

	char out[80];
	int n = snprintf(out, sizeof(out), "{\"ok\":true,\"note\":%u}\n", (unsigned)note);
	if (n < 0 || (size_t)n >= sizeof(out)) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleEffectMidiLightSet(HttpServer *server,
									int cfd,
									const char *method,
									const char *path,
									const char *body,
									size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char idBuf[64] = {};
	char enabledBuf[16] = {};
	char bypassedBuf[16] = {};
	if (!body ||
		!body_get_value(body, body_len, "id", idBuf, sizeof(idBuf)) ||
		!body_get_value(body, body_len, "enabled_vel", enabledBuf, sizeof(enabledBuf)) ||
		!body_get_value(body, body_len, "bypassed_vel", bypassedBuf, sizeof(bypassedBuf))) {
		static const char bad[] = "expected id,enabled_vel,bypassed_vel\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char *ep = NULL;
	long enabledVel = strtol(enabledBuf, &ep, 10);
	if (!ep || *ep != '\0' || enabledVel < 0 || enabledVel > 127 || !effect_id_valid(idBuf)) {
		static const char bad[] = "invalid id/enabled_vel\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}
	ep = NULL;
	long bypassedVel = strtol(bypassedBuf, &ep, 10);
	if (!ep || *ep != '\0' || bypassedVel < 0 || bypassedVel > 127) {
		static const char bad[] = "invalid bypassed_vel\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	MidiMapData map = server->app->config->readMidiMapFile();
	uint32_t outCount = 0;
	for (uint32_t i = 0; i < map.effectLightMappingCount && i < MIDI_EFFECT_LIGHT_MAPPINGS_MAX; ++i) {
		if (strcmp(map.effectLightMappings[i].effectId, idBuf) == 0) {
			continue;
		}
		map.effectLightMappings[outCount++] = map.effectLightMappings[i];
	}
	if (outCount < MIDI_EFFECT_LIGHT_MAPPINGS_MAX) {
		copy_bound(map.effectLightMappings[outCount].effectId,
				   sizeof(map.effectLightMappings[outCount].effectId),
				   idBuf);
		map.effectLightMappings[outCount].enabledVel = (uint8_t)enabledVel;
		map.effectLightMappings[outCount].bypassedVel = (uint8_t)bypassedVel;
		++outCount;
	}
	map.effectLightMappingCount = outCount;

	if (server->app->config->writeMidiMapFile(&map) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (server->app->midi) {
		server->app->midi->cachedMidiMap = map;
		server->app->midi->refreshLightingFromState();
	}

	char out[128];
	int n = snprintf(out,
				 sizeof(out),
				 "{\"ok\":true,\"id\":\"%s\",\"enabled_vel\":%u,\"bypassed_vel\":%u}\n",
				 idBuf,
				 (unsigned)enabledVel,
				 (unsigned)bypassedVel);
	if (n < 0 || (size_t)n >= sizeof(out)) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleEffectMidiLightDelete(HttpServer *server,
								   int cfd,
								   const char *method,
								   const char *path,
								   const char *body,
								   size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char idBuf[64] = {};
	if (!body || !body_get_value(body, body_len, "id", idBuf, sizeof(idBuf)) || !effect_id_valid(idBuf)) {
		static const char bad[] = "expected valid id\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	MidiMapData map = server->app->config->readMidiMapFile();
	uint32_t outCount = 0;
	for (uint32_t i = 0; i < map.effectLightMappingCount && i < MIDI_EFFECT_LIGHT_MAPPINGS_MAX; ++i) {
		if (strcmp(map.effectLightMappings[i].effectId, idBuf) == 0) {
			continue;
		}
		map.effectLightMappings[outCount++] = map.effectLightMappings[i];
	}
	map.effectLightMappingCount = outCount;

	if (server->app->config->writeMidiMapFile(&map) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (server->app->midi) {
		server->app->midi->cachedMidiMap = map;
		server->app->midi->refreshLightingFromState();
	}

	char out[96];
	int n = snprintf(out, sizeof(out), "{\"ok\":true,\"id\":\"%s\"}\n", idBuf);
	if (n < 0 || (size_t)n >= sizeof(out)) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleMidiLastNote(HttpServer *server,
						   int cfd,
						   const char *method,
						   const char *path) {
	(void)path;
	if (!server || !server->app || !method) {
		return -1;
	}

	if (strcmp(method, "GET") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	MidiContext *midi = server->app->midi;
	if (!midi) {
		static const char none[] = "{\"connected\":false,\"device\":\"\",\"last_note\":-1,\"last_channel\":0,\"last_velocity\":0,\"last_seq\":0}\n";
		return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", none, sizeof(none) - 1);
	}

	char out[512];
	int n = snprintf(out,
					 sizeof(out),
					 "{\"connected\":%s,\"device\":\"%s\",\"last_note\":%d,\"last_velocity\":%u,\"last_seq\":%u,\"last_channel\":%u}\n",
					 midi->connected ? "true" : "false",
					 midi->devPath,
					 midi->lastNoteSeq ? (int)midi->lastNote : -1,
					 (unsigned)midi->lastVelocity,
					 (unsigned)midi->lastNoteSeq,
					 (unsigned)midi->lastChannel);
	if (n < 0) {
		n = 0;
	}
	if ((size_t)n >= sizeof(out)) {
		n = (int)(sizeof(out) - 1);
	}

	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleMidiMappings(HttpServer *server,
					   int cfd,
					   const char *method,
					   const char *path) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method) {
		return -1;
	}

	if (strcmp(method, "GET") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	// Note mappings live in midi_map.txt (separated from audio config).
	MidiMapData data = server->app->config->readMidiMapFile();
	char out[HTTP_BODY_MAX];
	size_t off = 0;

	int n = snprintf(out + off, sizeof(out) - off, "{\"mapping_count\":%u,\"mappings\":[", (unsigned)data.mappingCount);
	if (n <= 0 || (size_t)n >= sizeof(out) - off) {
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", "encode failed\n", 14);
	}
	off += (size_t)n;

	uint32_t count = data.mappingCount;
	if (count > MIDI_MAPPINGS_MAX) {
		count = MIDI_MAPPINGS_MAX;
	}
	int first = 1;
	for (uint32_t i = 0; i < count; ++i) {
		if (!data.mappings[i].sfxPath[0]) {
			continue;
		}
		n = snprintf(out + off,
					 sizeof(out) - off,
					 "%s{\"note\":%u,\"sfx\":\"%s\"}",
					 first ? "" : ",",
					 (unsigned)data.mappings[i].note,
					 data.mappings[i].sfxPath);
		if (n <= 0 || (size_t)n >= sizeof(out) - off) {
			break;
		}
		off += (size_t)n;
		first = 0;
	}

	n = snprintf(out + off, sizeof(out) - off, "]}\n");
	if (n > 0 && (size_t)n < sizeof(out) - off) {
		off += (size_t)n;
	}

	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, off);
}

static int handleMidiMappingSet(HttpServer *server,
						 int cfd,
						 const char *method,
						 const char *path,
						 const char *body,
						 size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method || !body) {
		return -1;
	}

	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char note_buf[32];
	char sfx_buf[MIDI_SFX_PATH_MAX];
	if (!body_get_value(body, body_len, "note", note_buf, sizeof(note_buf)) ||
		!body_get_value(body, body_len, "sfx", sfx_buf, sizeof(sfx_buf))) {
		static const char bad[] = "expected note and sfx\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char *endp = NULL;
	long note = strtol(note_buf, &endp, 10);
	if (!endp || *endp != '\0' || note < 0 || note > 127) {
		static const char bad[] = "invalid note\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char normalized[MIDI_SFX_PATH_MAX];
	if (mapping_normalize_sfx(sfx_buf, normalized, sizeof(normalized)) != RET_OK) {
		static const char bad[] = "invalid sfx path\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	// Note mappings are stored in midi_map.txt.
	MidiMapData data = server->app->config->readMidiMapFile();
	uint32_t count = data.mappingCount;
	if (count > MIDI_MAPPINGS_MAX) {
		count = MIDI_MAPPINGS_MAX;
	}

	int found = -1;
	for (uint32_t i = 0; i < count; ++i) {
		if (data.mappings[i].note == (uint8_t)note) {
			found = (int)i;
			break;
		}
	}

	if (found >= 0) {
		data.mappings[found].note = (uint8_t)note;
		copy_bound(data.mappings[found].sfxPath, sizeof(data.mappings[found].sfxPath), normalized);
	} else {
		if (count >= MIDI_MAPPINGS_MAX) {
			static const char full[] = "mapping table full\n";
			return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8", full, sizeof(full) - 1);
		}
		data.mappings[count].note = (uint8_t)note;
		copy_bound(data.mappings[count].sfxPath, sizeof(data.mappings[count].sfxPath), normalized);
		data.mappingCount = count + 1;
	}

	uint32_t actionCount = data.actionMappingCount;
	if (actionCount > MIDI_ACTION_MAPPINGS_MAX) {
		actionCount = MIDI_ACTION_MAPPINGS_MAX;
	}
	for (uint32_t i = 0; i < actionCount; ++i) {
		if (data.actionMappings[i].note != (uint8_t)note) {
			continue;
		}
		for (uint32_t j = i; j + 1U < actionCount; ++j) {
			data.actionMappings[j] = data.actionMappings[j + 1U];
		}
		data.actionMappingCount = (actionCount > 0U) ? (actionCount - 1U) : 0U;
		break;
	}

	if (server->app->config->writeMidiMapFile(&data) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	// Push update to MIDI context immediately so lighting reflects new mapping.
	if (server->app->midi) {
		server->app->midi->cachedMidiMap = data;
		server->app->midi->refreshAllLighting();
	}

	static const char ok[] = "ok\n";
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleMidiMappingDelete(HttpServer *server,
							int cfd,
							const char *method,
							const char *path,
							const char *body,
							size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method || !body) {
		return -1;
	}

	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char note_buf[32];
	if (!body_get_value(body, body_len, "note", note_buf, sizeof(note_buf))) {
		static const char bad[] = "expected note\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char *endp = NULL;
	long note = strtol(note_buf, &endp, 10);
	if (!endp || *endp != '\0' || note < 0 || note > 127) {
		static const char bad[] = "invalid note\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	MidiMapData data = server->app->config->readMidiMapFile();
	uint32_t count = data.mappingCount;
	if (count > MIDI_MAPPINGS_MAX) {
		count = MIDI_MAPPINGS_MAX;
	}

	int idx = -1;
	for (uint32_t i = 0; i < count; ++i) {
		if (data.mappings[i].note == (uint8_t)note) {
			idx = (int)i;
			break;
		}
	}

	if (idx < 0) {
		static const char missing[] = "mapping not found\n";
		return server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8", missing, sizeof(missing) - 1);
	}

	for (uint32_t i = (uint32_t)idx; i + 1 < count; ++i) {
		data.mappings[i] = data.mappings[i + 1];
	}
	if (count > 0) {
		data.mappingCount = count - 1;
	}

	if (server->app->config->writeMidiMapFile(&data) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	// Turn off the removed note's LED and refresh the rest.
	if (server->app->midi) {
		server->app->midi->cachedMidiMap = data;
		server->app->midi->sendLightNote((uint8_t)note, data.globalLight.channel, 0);
		server->app->midi->refreshAllLighting();
	}

	static const char ok[] = "ok\n";
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleMidiActions(HttpServer *server,
						 int cfd,
						 const char *method,
						 const char *path) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method) {
		return -1;
	}

	if (strcmp(method, "GET") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	MidiMapData data = server->app->config->readMidiMapFile();
	char out[HTTP_BODY_MAX];
	size_t off = 0;

	int n = snprintf(out + off, sizeof(out) - off, "{\"count\":%u,\"actions\":[", (unsigned)data.actionMappingCount);
	if (n <= 0 || (size_t)n >= sizeof(out) - off) {
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", "encode failed\n", 14);
	}
	off += (size_t)n;

	uint32_t count = data.actionMappingCount;
	if (count > MIDI_ACTION_MAPPINGS_MAX) {
		count = MIDI_ACTION_MAPPINGS_MAX;
	}
	int first = 1;
	for (uint32_t i = 0; i < count; ++i) {
		const char *action = midi_action_to_string(data.actionMappings[i].action);
		if (!action) {
			continue;
		}
		n = snprintf(out + off,
					 sizeof(out) - off,
					 "%s{\"note\":%u,\"action\":\"%s\"}",
					 first ? "" : ",",
					 (unsigned)data.actionMappings[i].note,
					 action);
		if (n <= 0 || (size_t)n >= sizeof(out) - off) {
			break;
		}
		off += (size_t)n;
		first = 0;
	}

	n = snprintf(out + off, sizeof(out) - off, "]}\n");
	if (n > 0 && (size_t)n < sizeof(out) - off) {
		off += (size_t)n;
	}

	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, off);
}

static int handleMidiActionSet(HttpServer *server,
						   int cfd,
						   const char *method,
						   const char *path,
						   const char *body,
						   size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method || !body) {
		return -1;
	}

	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char note_buf[32];
	char action_buf[32];
	if (!body_get_value(body, body_len, "note", note_buf, sizeof(note_buf)) ||
		!body_get_value(body, body_len, "action", action_buf, sizeof(action_buf))) {
		static const char bad[] = "expected note and action\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char *endp = NULL;
	long note = strtol(note_buf, &endp, 10);
	if (!endp || *endp != '\0' || note < 0 || note > 127) {
		static const char bad[] = "invalid note\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	uint8_t action = midi_action_from_string(action_buf);
	if (action == MIDI_ACTION_NONE) {
		static const char bad[] = "invalid action\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	MidiMapData data = server->app->config->readMidiMapFile();

	uint32_t actionCount = data.actionMappingCount;
	if (actionCount > MIDI_ACTION_MAPPINGS_MAX) {
		actionCount = MIDI_ACTION_MAPPINGS_MAX;
	}

	int found = -1;
	for (uint32_t i = 0; i < actionCount; ++i) {
		if (data.actionMappings[i].note == (uint8_t)note) {
			found = (int)i;
			break;
		}
	}

	if (found >= 0) {
		data.actionMappings[found].action = action;
	} else {
		if (actionCount >= MIDI_ACTION_MAPPINGS_MAX) {
			static const char full[] = "action mapping table full\n";
			return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8", full, sizeof(full) - 1);
		}
		data.actionMappings[actionCount].note = (uint8_t)note;
		data.actionMappings[actionCount].action = action;
		data.actionMappingCount = actionCount + 1U;
	}

	// Keep note ownership unambiguous: a note can map to one clip OR one action.
	uint32_t mapCount = data.mappingCount;
	if (mapCount > MIDI_MAPPINGS_MAX) {
		mapCount = MIDI_MAPPINGS_MAX;
	}
	for (uint32_t i = 0; i < mapCount; ++i) {
		if (data.mappings[i].note != (uint8_t)note) {
			continue;
		}
		for (uint32_t j = i; j + 1U < mapCount; ++j) {
			data.mappings[j] = data.mappings[j + 1U];
		}
		data.mappingCount = (mapCount > 0U) ? (mapCount - 1U) : 0U;
		break;
	}

	if (server->app->config->writeMidiMapFile(&data) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	if (server->app->midi) {
		server->app->midi->cachedMidiMap = data;
		server->app->midi->refreshAllLighting();
	}

	char out[128];
	const char *actionName = midi_action_to_string(action);
	int n = snprintf(out,
					 sizeof(out),
					 "{\"ok\":true,\"note\":%u,\"action\":\"%s\"}\n",
					 (unsigned)note,
					 actionName ? actionName : "unknown");
	if (n < 0) {
		n = 0;
	}
	if ((size_t)n >= sizeof(out)) {
		n = (int)(sizeof(out) - 1);
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleMidiActionDelete(HttpServer *server,
							 int cfd,
							 const char *method,
							 const char *path,
							 const char *body,
							 size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method || !body) {
		return -1;
	}

	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char note_buf[32];
	if (!body_get_value(body, body_len, "note", note_buf, sizeof(note_buf))) {
		static const char bad[] = "expected note\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char *endp = NULL;
	long note = strtol(note_buf, &endp, 10);
	if (!endp || *endp != '\0' || note < 0 || note > 127) {
		static const char bad[] = "invalid note\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	MidiMapData data = server->app->config->readMidiMapFile();
	uint32_t count = data.actionMappingCount;
	if (count > MIDI_ACTION_MAPPINGS_MAX) {
		count = MIDI_ACTION_MAPPINGS_MAX;
	}

	int idx = -1;
	for (uint32_t i = 0; i < count; ++i) {
		if (data.actionMappings[i].note == (uint8_t)note) {
			idx = (int)i;
			break;
		}
	}

	if (idx < 0) {
		static const char missing[] = "action mapping not found\n";
		return server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8", missing, sizeof(missing) - 1);
	}

	for (uint32_t i = (uint32_t)idx; i + 1U < count; ++i) {
		data.actionMappings[i] = data.actionMappings[i + 1U];
	}
	if (count > 0U) {
		data.actionMappingCount = count - 1U;
	}

	if (server->app->config->writeMidiMapFile(&data) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	if (server->app->midi) {
		server->app->midi->cachedMidiMap = data;
	}

	static const char ok[] = "ok\n";
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleConfigReload(HttpServer *server,
							  int cfd,
							  const char *method,
							  const char *path) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method) {
		return -1;
	}

	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	int reloadRc = reloadAudioGadget(server->app);
	if (reloadRc == RET_ERR) {
		static const char err[] = "usb audio gadget reload failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	if (reloadRc == RET_WARN) {
		static const char warn[] = "usb audio gadget reload rejected by config\n";
		return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8", warn, sizeof(warn) - 1);
	}

	if (server->app->audio) {
		usleep(50000);
		int rescanRc = server->app->audio->forceRescan();
		if (rescanRc == RET_ERR) {
			printf("[HTTP] [WARN] audio rescan failed after USB gadget reload\n");
		}
	}

	ConfigData cfg = server->app->config->readConfigFile();
	if (server->app->audio) {
		server->app->audio->setSoundboardMode(cfg.soundboardMode);
		if (cfg.soundboardMode != SOUNDBOARD_MODE_HOLD) {
			server->app->audio->stopHeldSfx();
		}
	}
	char out[256];
	int n = snprintf(out,
				 sizeof(out),
				 "{\"ok\":true,\"sampleRate\":%u,\"playbackChannels\":%u,\"captureChannels\":%u,\"sampleSize\":%u,\"soundboardMode\":\"%s\",\"source\":\"real\"}\n",
				 (unsigned)cfg.sampleRate,
				 (unsigned)cfg.playbackChannels,
				 (unsigned)cfg.captureChannels,
				 (unsigned)cfg.sampleSize,
				 soundboardModeToString(cfg.soundboardMode));
	if (n < 0) {
		n = 0;
	}
	if ((size_t)n >= sizeof(out)) {
		n = (int)(sizeof(out) - 1);
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleRoutingReload(HttpServer *server,
							   int cfd,
							   const char *method,
							   const char *path) {
	(void)path;
	if (!server || !server->app || !server->app->audio || !method) {
		return -1;
	}

	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	int rc = server->app->audio->reloadRoutingGraph();
	if (rc == RET_ERR) {
		static const char err[] = "{\"ok\":false,\"error\":\"routing graph reload failed\"}\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}

	char out[512];
	if (server->app->audio->buildRoutingGraphJson(out, sizeof(out)) != RET_OK) {
		static const char err[] = "{\"ok\":false,\"error\":\"routing graph encode failed\"}\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}

	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, strlen(out));
}

static int handleRoutingThings(HttpServer *server,
							   int cfd,
							   const char *method,
							   const char *path) {
	(void)path;
	if (!server || !server->app || !method) {
		return -1;
	}

	if (strcmp(method, "GET") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	ConfigData cfg = {};
	if (server->app->config) {
		cfg = server->app->config->readConfigFile();
	}

	AudioGraphThingInfo things[AUDIO_GRAPH_MAX_THINGS];
	size_t thingCount = 0;
	if (server->app->audio) {
		thingCount = server->app->audio->copyRoutingThings(things, AUDIO_GRAPH_MAX_THINGS);
	}

	char out[HTTP_BODY_MAX];
	size_t used = 0;
	int n = snprintf(out + used, sizeof(out) - used, "{\"ok\":true,\"things\":[");
	if (n < 0 || (size_t)n >= sizeof(out) - used) {
		static const char err[] = "encode failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	used += (size_t)n;

	int first = 1;
#define APPEND_THING(ID, NAME, INPUTS, OUTPUTS) \
	do { \
		n = snprintf(out + used, sizeof(out) - used, "%s{\"id\":\"%s\",\"name\":\"%s\",\"inputs\":%u,\"outputs\":%u}", \
			first ? "" : ",", audiox::json::escape(ID).c_str(), audiox::json::escape(NAME).c_str(), (unsigned)(INPUTS), (unsigned)(OUTPUTS)); \
		if (n < 0 || (size_t)n >= sizeof(out) - used) { \
			static const char err[] = "encode failed\n"; \
			return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1); \
		} \
		used += (size_t)n; \
		first = 0; \
	} while (0)

	for (size_t i = 0; i < thingCount; ++i) {
		APPEND_THING(things[i].id,
					 things[i].name,
					 things[i].inputs,
					 things[i].outputs);
	}

#undef APPEND_THING

	n = snprintf(out + used, sizeof(out) - used, "]}\n");
	if (n < 0 || (size_t)n >= sizeof(out) - used) {
		static const char err[] = "encode failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	used += (size_t)n;

	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, used);
}

static int handleSystemSync(HttpServer *server,
							int cfd,
							const char *method,
							const char *path) {
	(void)method;
	(void)path;
	sync();
	static const char ok[] = "{\"status\":\"synced\"}\n";
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleSystemRestart(HttpServer *server,
							   int cfd,
							   const char *method,
							   const char *path) {
	(void)method;
	(void)path;
	static const char ok[] = "{\"status\":\"restarting\"}\n";
	int ret = server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
	sync();
	sleep(1);
	reboot(RB_AUTOBOOT);
	return ret;
}

static int handleSystemBootFailure(HttpServer *server, int cfd, const char *method) {
	if (strcmp(method, "GET") == 0) {
		if (access(BOOT_RECOVERY_REPORT_PATH, R_OK) < 0) {
			return server->sendResponse(cfd, "204 No Content", "text/plain; charset=utf-8", "", 0);
		}
		return server->sendFilePath(cfd, BOOT_RECOVERY_REPORT_PATH, "text/plain; charset=utf-8");
	}
	if (strcmp(method, "DELETE") == 0) {
		if (unlink(BOOT_RECOVERY_REPORT_PATH) < 0 && errno != ENOENT) {
			static const char err[] = "failed to dismiss boot failure report\n";
			return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
		}
		sync();
		static const char ok[] = "{\"ok\":true}\n";
		return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
	}
	return sendMethodNotAllowed(server, cfd);
}

static int handleSystemShutdown(HttpServer *server,
								int cfd,
								const char *method,
								const char *path) {
	(void)method;
	(void)path;
	static const char ok[] = "{\"status\":\"shutting down\"}\n";
	int ret = server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
	sync();
	sleep(1);
	reboot(RB_POWER_OFF);
	return ret;
}

static void getFfmpegVersion(char *out, size_t out_sz) {
	if (!out || out_sz < 2) {
		printf("[API] getFfmpegVersion: invalid output buffer\n");
		return;
	}

	const char *ffmpeg_path = getFfmpegPath();
	if (!ffmpeg_path) {
		printf("[API] getFfmpegVersion: ffmpeg path not found\n");
		snprintf(out, out_sz, "Unavailable");
		return;
	}

	int pipefd[2];
	if (pipe(pipefd) < 0) {
		printf("[API] getFfmpegVersion: pipe() failed\n");
		snprintf(out, out_sz, "Unavailable");
		return;
	}

	pid_t pid = fork();
	if (pid < 0) {
		printf("[API] getFfmpegVersion: fork() failed\n");
		close(pipefd[0]);
		close(pipefd[1]);
		snprintf(out, out_sz, "Unavailable");
		return;
	}

	if (pid == 0) {
		// Child process
		close(pipefd[0]); // close read end
		dup2(pipefd[1], STDOUT_FILENO); // redirect stdout to pipe
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		close(pipefd[1]); // close original pipe write end after dup2
		execl(ffmpeg_path, "ffmpeg", "-version", (char *)NULL);
		_exit(127);
	}

	// Parent process
	close(pipefd[1]); // close write end
	
	FILE *fp = fdopen(pipefd[0], "r");
	if (!fp) {
		printf("[API] getFfmpegVersion: fdopen() failed\n");
		close(pipefd[0]);
		snprintf(out, out_sz, "Unavailable");
		return;
	}

	char line[256] = {0};
	bool have_line = fgets(line, sizeof(line), fp) != NULL;
	if (have_line) {
		line[strcspn(line, "\r\n")] = '\0';
	}
	fclose(fp);
	int status = 0;
	waitpid(pid, &status, 0);

	static bool failure_logged = false;
	if (have_line && line[0]) {
		snprintf(out, out_sz, "%s", line);
		failure_logged = false;
		return;
	}

	if (!failure_logged) {
		if (WIFEXITED(status)) {
			printf("[API] getFfmpegVersion: %s produced no version (exit %d%s)\n",
				   ffmpeg_path,
				   WEXITSTATUS(status),
				   WEXITSTATUS(status) == 127 ? ", exec failed; check /lib/ld-linux-aarch64.so.1" : "");
		} else if (WIFSIGNALED(status)) {
			printf("[API] getFfmpegVersion: %s killed by signal %d\n", ffmpeg_path, WTERMSIG(status));
		} else {
			printf("[API] getFfmpegVersion: %s produced no version\n", ffmpeg_path);
		}
		failure_logged = true;
	}
	snprintf(out, out_sz, "Unavailable");
}

static int handleBluetoothDevices(HttpServer *server,
								  int cfd,
								  const char *method,
								  const char *path) {
	(void)path;
	if (!server || !method) {
		return -1;
	}
	if (strcmp(method, "GET") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}
	BluetoothContext *bluetooth = server->app ? server->app->bluetooth : nullptr;
	static char out[HTTP_BODY_MAX];
	if (!bluetooth || bluetooth->buildStatusJson(out, sizeof(out)) != RET_OK) {
		static const char err[] = "{\"ok\":false,\"error\":\"bluetooth unavailable\"}\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, strlen(out));
}

static int handleBluetoothScan(HttpServer *server,
							   int cfd,
							   const char *method,
							   const char *path) {
	(void)path;
	if (!server || !method) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}
	BluetoothContext *bluetooth = server->app ? server->app->bluetooth : nullptr;
	if (!bluetooth) {
		static const char err[] = "{\"ok\":false,\"error\":\"bluetooth unavailable\"}\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}
	if (bluetooth->requestScan() != RET_OK) {
		static const char busy[] = "{\"ok\":false,\"error\":\"bluetooth is busy\"}\n";
		return server->sendResponse(cfd, "409 Conflict", "application/json; charset=utf-8", busy, sizeof(busy) - 1);
	}
	static const char ok[] = "{\"ok\":true}\n";
	return server->sendResponse(cfd, "202 Accepted", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleRoutingHelper(HttpServer *server,
							   int cfd,
							   const char *method,
							   const char *path,
							   bool enabled) {
	(void)path;
	if (!server || !server->app || !server->app->config || !server->app->audio || !method) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	RouterConfig router = server->app->config->router();
	router.setRoutingHelper(enabled);
	int rc = server->app->audio->reloadRoutingGraph();
	if (rc == RET_ERR) {
		static const char err[] = "{\"ok\":false,\"error\":\"routing helper update failed\"}\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}
	static const char ok[] = "{\"ok\":true}\n";
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleBluetoothPair(HttpServer *server,
							   int cfd,
							   const char *method,
							   const char *path,
							   const char *body,
							   size_t body_len) {
	(void)path;
	if (!server || !method || !body) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}
	BluetoothContext *bluetooth = server->app ? server->app->bluetooth : nullptr;
	if (!bluetooth) {
		static const char err[] = "{\"ok\":false,\"error\":\"bluetooth unavailable\"}\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}

	char address[32] = {};
	if (!body_get_value(body, body_len, "address", address, sizeof(address))) {
		static const char bad[] = "{\"ok\":false,\"error\":\"missing bluetooth address\"}\n";
		return server->sendResponse(cfd, "400 Bad Request", "application/json; charset=utf-8", bad, sizeof(bad) - 1);
	}

	int rc = bluetooth->requestPair(address);
	if (rc == RET_ERR) {
		static const char bad[] = "{\"ok\":false,\"error\":\"invalid bluetooth address\"}\n";
		return server->sendResponse(cfd, "400 Bad Request", "application/json; charset=utf-8", bad, sizeof(bad) - 1);
	}
	if (rc != RET_OK) {
		static const char busy[] = "{\"ok\":false,\"error\":\"bluetooth is busy\"}\n";
		return server->sendResponse(cfd, "409 Conflict", "application/json; charset=utf-8", busy, sizeof(busy) - 1);
	}

	static const char ok[] = "{\"ok\":true}\n";
	return server->sendResponse(cfd, "202 Accepted", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleBluetoothConnect(HttpServer *server,
							      int cfd,
							      const char *method,
							      const char *body,
							      size_t body_len) {
	if (!server || !method || !body) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}
	BluetoothContext *bluetooth = server->app ? server->app->bluetooth : nullptr;
	if (!bluetooth) {
		static const char err[] = "{\"ok\":false,\"error\":\"bluetooth unavailable\"}\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}
	char address[32] = {};
	if (!body_get_value(body, body_len, "address", address, sizeof(address))) {
		static const char bad[] = "{\"ok\":false,\"error\":\"missing bluetooth address\"}\n";
		return server->sendResponse(cfd, "400 Bad Request", "application/json; charset=utf-8", bad, sizeof(bad) - 1);
	}
	int rc = bluetooth->requestConnect(address);
	if (rc == RET_ERR) {
		static const char bad[] = "{\"ok\":false,\"error\":\"device is not paired or is not a classic device\"}\n";
		return server->sendResponse(cfd, "400 Bad Request", "application/json; charset=utf-8", bad, sizeof(bad) - 1);
	}
	if (rc != RET_OK) {
		static const char busy[] = "{\"ok\":false,\"error\":\"bluetooth is busy or already connected\"}\n";
		return server->sendResponse(cfd, "409 Conflict", "application/json; charset=utf-8", busy, sizeof(busy) - 1);
	}
	static const char ok[] = "{\"ok\":true}\n";
	return server->sendResponse(cfd, "202 Accepted", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleBluetoothDisconnect(HttpServer *server,
							     int cfd,
							     const char *method,
							     const char *body,
							     size_t body_len) {
	if (!server || !method) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}
	BluetoothContext *bluetooth = server->app ? server->app->bluetooth : nullptr;
	if (!bluetooth) {
		static const char err[] = "{\"ok\":false,\"error\":\"bluetooth unavailable\"}\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}
	// Optional: without an address every Bluetooth audio connection is closed.
	char address[32] = {};
	if (body && body_len > 0) {
		(void)body_get_value(body, body_len, "address", address, sizeof(address));
	}
	int rc = bluetooth->requestDisconnect(address);
	if (rc == RET_ERR) {
		static const char bad[] = "{\"ok\":false,\"error\":\"invalid bluetooth address\"}\n";
		return server->sendResponse(cfd, "400 Bad Request", "application/json; charset=utf-8", bad, sizeof(bad) - 1);
	}
	if (rc != RET_OK) {
		static const char busy[] = "{\"ok\":false,\"error\":\"device is not connected or bluetooth is busy\"}\n";
		return server->sendResponse(cfd, "409 Conflict", "application/json; charset=utf-8", busy, sizeof(busy) - 1);
	}
	static const char ok[] = "{\"ok\":true}\n";
	return server->sendResponse(cfd, "202 Accepted", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleBluetoothUnpair(HttpServer *server,
							     int cfd,
							     const char *method,
							     const char *body,
							     size_t body_len) {
	if (!server || !method || !body) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}
	BluetoothContext *bluetooth = server->app ? server->app->bluetooth : nullptr;
	if (!bluetooth) {
		static const char err[] = "{\"ok\":false,\"error\":\"bluetooth unavailable\"}\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}
	char address[32] = {};
	if (!body_get_value(body, body_len, "address", address, sizeof(address))) {
		static const char bad[] = "{\"ok\":false,\"error\":\"missing bluetooth address\"}\n";
		return server->sendResponse(cfd, "400 Bad Request", "application/json; charset=utf-8", bad, sizeof(bad) - 1);
	}
	int rc = bluetooth->requestUnpair(address);
	if (rc == RET_ERR) {
		static const char bad[] = "{\"ok\":false,\"error\":\"device is not paired or address is invalid\"}\n";
		return server->sendResponse(cfd, "400 Bad Request", "application/json; charset=utf-8", bad, sizeof(bad) - 1);
	}
	if (rc != RET_OK) {
		static const char busy[] = "{\"ok\":false,\"error\":\"bluetooth is busy\"}\n";
		return server->sendResponse(cfd, "409 Conflict", "application/json; charset=utf-8", busy, sizeof(busy) - 1);
	}
	static const char ok[] = "{\"ok\":true}\n";
	return server->sendResponse(cfd, "202 Accepted", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleBluetoothAutoConnect(HttpServer *server,
                                      int cfd,
                                      const char *method,
                                      const char *body,
                                      size_t body_len) {
    if (!server || !method || !body) {
        return -1;
    }
    if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
        return sendMethodNotAllowed(server, cfd);
    }
    BluetoothContext *bluetooth = server->app ? server->app->bluetooth : nullptr;
    if (!bluetooth) {
        static const char err[] = "{\"ok\":false,\"error\":\"bluetooth unavailable\"}\n";
        return server->sendResponse(cfd, "503 Service Unavailable", "application/json; charset=utf-8", err, sizeof(err) - 1);
    }
    char address[32] = {};
    char enabledText[8] = {};
    if (!body_get_value(body, body_len, "address", address, sizeof(address)) ||
        !body_get_value(body, body_len, "enabled", enabledText, sizeof(enabledText))) {
        static const char bad[] = "{\"ok\":false,\"error\":\"address and enabled are required\"}\n";
        return server->sendResponse(cfd, "400 Bad Request", "application/json; charset=utf-8", bad, sizeof(bad) - 1);
    }
    bool enabled = false;
    if (strcmp(enabledText, "true") == 0 || strcmp(enabledText, "1") == 0) {
        enabled = true;
    } else if (strcmp(enabledText, "false") != 0 && strcmp(enabledText, "0") != 0) {
        static const char bad[] = "{\"ok\":false,\"error\":\"enabled must be true or false\"}\n";
        return server->sendResponse(cfd, "400 Bad Request", "application/json; charset=utf-8", bad, sizeof(bad) - 1);
    }
    int rc = bluetooth->requestAutoConnect(address, enabled);
    if (rc == RET_ERR) {
        static const char bad[] = "{\"ok\":false,\"error\":\"device is not a saved Classic Bluetooth device\"}\n";
        return server->sendResponse(cfd, "400 Bad Request", "application/json; charset=utf-8", bad, sizeof(bad) - 1);
    }
    if (rc != RET_OK) {
        static const char err[] = "{\"ok\":false,\"error\":\"could not save Auto-connect setting\"}\n";
        return server->sendResponse(cfd, "500 Internal Server Error", "application/json; charset=utf-8", err, sizeof(err) - 1);
    }
    static const char ok[] = "{\"ok\":true}\n";
    return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleSystemInfo(HttpServer *server,
							 int cfd,
							 const char *method,
							 const char *path) {
	(void)method;
	(void)path;
	if (!server) return -1;

	const char *version_str = "unknown";
#if defined(AUDIOX_VERSION_STRING)
	version_str = AUDIOX_VERSION_STRING;
#elif defined(AUDIOX_VERSION_MAJOR) && defined(AUDIOX_VERSION_MINOR) && defined(AUDIOX_VERSION_PATCH)
	char version_buf[48];
	snprintf(version_buf, sizeof(version_buf), "%d.%d.%d",
			 AUDIOX_VERSION_MAJOR, AUDIOX_VERSION_MINOR, AUDIOX_VERSION_PATCH);
	version_str = version_buf;
#endif

	char kernel_buf[192] = "unknown";
	struct utsname u;
	if (uname(&u) == 0) {
		snprintf(kernel_buf, sizeof(kernel_buf), "Linux %s", u.release);
	}

	long uptime_secs = 0;
	long mem_total_mb = 0;
	long mem_avail_mb = 0;
	float load1 = 0.0f;
	char model_buf[128] = "unknown";
	FILE *model_file = fopen("/proc/device-tree/model", "r");
	if (model_file) {
		size_t model_len = fread(model_buf, 1, sizeof(model_buf) - 1, model_file);
		fclose(model_file);
		while (model_len > 0 && model_buf[model_len - 1] == '\0') {
			--model_len;
		}
		model_buf[model_len] = '\0';
		if (model_len == 0) {
			snprintf(model_buf, sizeof(model_buf), "unknown");
		}
		for (size_t i = 0; model_buf[i]; ++i) {
			if (model_buf[i] == '"' || model_buf[i] == '\\' ||
				(unsigned char)model_buf[i] < 0x20) {
				model_buf[i] = ' ';
			}
		}
	}
	long cpu_freq_mhz = 0;
	FILE *cpu_freq_file = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
	if (cpu_freq_file) {
		long cpu_freq_khz = 0;
		if (fscanf(cpu_freq_file, "%ld", &cpu_freq_khz) == 1 && cpu_freq_khz > 0) {
			cpu_freq_mhz = cpu_freq_khz / 1000;
		}
		fclose(cpu_freq_file);
	}
	long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
	if (cpu_count < 1) cpu_count = 1;
	char cpu_times_json[128] = "null";
	FILE *cpu_stat_file = fopen("/proc/stat", "r");
	if (cpu_stat_file) {
		char cpu_label[8] = {};
		unsigned long long user = 0, nice = 0, system = 0, idle = 0;
		unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
		if (fscanf(cpu_stat_file, "%7s %llu %llu %llu %llu %llu %llu %llu %llu",
				   cpu_label, &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) == 9 &&
			strcmp(cpu_label, "cpu") == 0) {
			const unsigned long long totalTicks = user + nice + system + idle + iowait +
				irq + softirq + steal;
			const unsigned long long idleTicks = idle + iowait;
			snprintf(cpu_times_json, sizeof(cpu_times_json),
					 "{\"total_ticks\":%llu,\"idle_ticks\":%llu}", totalTicks, idleTicks);
		}
		fclose(cpu_stat_file);
	}
	char temperature_json[24] = "null";
	FILE *temperature_file = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
	if (temperature_file) {
		long temperature_millidegrees = -1;
		if (fscanf(temperature_file, "%ld", &temperature_millidegrees) == 1 &&
			temperature_millidegrees >= 0 && temperature_millidegrees <= 150000) {
			snprintf(temperature_json, sizeof(temperature_json), "%.1f",
					 temperature_millidegrees / 1000.0);
		}
		fclose(temperature_file);
	}
	struct sysinfo si;
	if (sysinfo(&si) == 0) {
		uptime_secs = si.uptime;
		unsigned long unit = si.mem_unit;
		mem_total_mb = (long)((unsigned long long)si.totalram * unit / (1024UL * 1024UL));
		mem_avail_mb = (long)((unsigned long long)(si.freeram + si.bufferram) * unit / (1024UL * 1024UL));
		load1 = (float)si.loads[0] / 65536.0f;
	}

	char ffmpeg_version[256];
	getFfmpegVersion(ffmpeg_version, sizeof(ffmpeg_version));

	char storage_json[256] = "null";
	struct statvfs storage{};
	if (statvfs(ROOT_MOUNT_POINT, &storage) == 0) {
		const uint64_t unit = storage.f_frsize;
		snprintf(storage_json, sizeof(storage_json),
				 "{\"total_bytes\":%llu,\"used_bytes\":%llu,\"free_bytes\":%llu,\"reserved_bytes\":%llu}",
				 (unsigned long long)(storage.f_blocks * unit),
				 (unsigned long long)((storage.f_blocks - storage.f_bfree) * unit),
				 (unsigned long long)(storage.f_bavail * unit),
				 (unsigned long long)((storage.f_bfree - storage.f_bavail) * unit));
	} else {
		printf("[HTTP] [WARN] read storage usage (%s): %s\n", ROOT_MOUNT_POINT, strerror(errno));
	}

	const uint64_t warningAt = audiox::audioHealth.warningAt();
	const uint64_t now = audiox::healthMonotonicMs();
	const uint32_t targetPeriodFrames =
		audiox::adaptiveAudioBuffer.targetPeriodFrames(BUFFER_FRAMES);
	const uint32_t largestPeriodFrames = audiox::adaptiveAudioBuffer.largestPeriodFrames();
	const uint32_t largestBufferFrames = audiox::adaptiveAudioBuffer.largestBufferFrames();
	const double estimatedBufferMs = SAMPLE_RATE
		? 1000.0 * largestBufferFrames / SAMPLE_RATE : 0.0;
	const uint32_t stressMode = audiox::audioStressTest.mode();
#if AUDIOX_ENABLE_STRESS_TEST
	const char *stressAvailable = "true";
#else
	const char *stressAvailable = "false";
#endif
	char out[2048];
	int n = snprintf(out, sizeof(out),
					 "{\"version\":\"%s\",\"kernel\":\"%s\","
					 "\"board\":\"%s\","
					 "\"model\":\"%s\","
					 "\"cpu_freq_mhz\":%ld,"
					 "\"cpu_count\":%ld,"
					 "\"cpu_times\":%s,"
					 "\"temperature_c\":%s,"
					 "\"uptime_secs\":%ld,"
					 "\"mem_total_mb\":%ld,"
					 "\"mem_avail_mb\":%ld,"
					 "\"load1\":%.2f,"
					 "\"ffmpeg\":\"%s\","
					 "\"storage\":%s,"
					 "\"audio_health\":{\"xruns\":%llu,\"warning_id\":\"%llu\","
					 "\"warning_active\":%s},"
					 "\"audio_buffer\":{\"base_block_frames\":%u,\"target_period_frames\":%u,"
					 "\"largest_period_frames\":%u,\"largest_buffer_frames\":%u,"
					 "\"active_streams\":%u,"
					 "\"estimated_largest_buffer_ms\":%.2f,\"level\":%u,"
					 "\"downshift_delay_ms\":%u,\"downshift_failures\":%u,"
					 "\"runtime_id\":\"%llu\",\"change_id\":\"%llu\","
					 "\"increase_id\":\"%llu\"},"
					 "\"audio_stress_test\":{\"available\":%s,\"mode\":\"%s\","
					 "\"remaining_ms\":%u,\"workers\":%u}}\n",
					 version_str, kernel_buf, audioxDetectBoard(), model_buf, cpu_freq_mhz,
					 cpu_count, cpu_times_json, temperature_json,
					 uptime_secs, mem_total_mb, mem_avail_mb, load1,
					 ffmpeg_version, storage_json,
					 (unsigned long long)audiox::audioHealth.total(),
					 (unsigned long long)warningAt,
					 audiox::audioHealth.warningActive(now) ? "true" : "false",
					 (unsigned)BUFFER_FRAMES, (unsigned)targetPeriodFrames,
					 (unsigned)largestPeriodFrames,
					 (unsigned)largestBufferFrames,
					 (unsigned)audiox::adaptiveAudioBuffer.activeStreams(),
					 estimatedBufferMs, (unsigned)audiox::adaptiveAudioBuffer.level(),
					 (unsigned)audiox::adaptiveAudioBuffer.downshiftDelayMs(),
					 (unsigned)audiox::adaptiveAudioBuffer.downshiftFailures(),
					 (unsigned long long)audiox::adaptiveAudioBuffer.instanceId(),
					 (unsigned long long)audiox::adaptiveAudioBuffer.changeId(),
					 (unsigned long long)audiox::adaptiveAudioBuffer.increaseId(),
					 stressAvailable, audiox::AudioStressTest::modeName(stressMode),
					 (unsigned)audiox::audioStressTest.remainingMs(),
					 (unsigned)audiox::audioStressTest.workerCount());
	if (n < 0 || n >= (int)sizeof(out)) n = 0;
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
}

static int handleSystemAudioStress(HttpServer *server, int cfd, const char *method,
								  const char *body, size_t body_len) {
	if (!server) return -1;
#if !AUDIOX_ENABLE_STRESS_TEST
	(void)method;
	(void)body;
	(void)body_len;
	static const char unavailable[] = "audio stress tests are not enabled in this build\n";
	return server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8",
								unavailable, sizeof(unavailable) - 1);
#else
	if (!method) return -1;
	if (strcmp(method, "POST") != 0) return sendMethodNotAllowed(server, cfd);

	char mode[16];
	if (!body || !body_get_value(body, body_len, "mode", mode, sizeof(mode))) {
		static const char bad[] = "expected mode=audio|cpu|stop\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8",
									bad, sizeof(bad) - 1);
	}

	if (strcmp(mode, "stop") == 0) {
		audiox::audioStressTest.stop();
		static const char stopped[] = "{\"ok\":true,\"mode\":\"idle\"}\n";
		return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8",
									stopped, sizeof(stopped) - 1);
	}

	char durationText[16];
	if (!body_get_value(body, body_len, "duration_secs", durationText, sizeof(durationText))) {
		static const char bad[] = "duration_secs is required\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8",
									bad, sizeof(bad) - 1);
	}
	char *end = nullptr;
	const unsigned long duration = strtoul(durationText, &end, 10);
	if (!end || *end != '\0' || duration == 0 ||
		duration > audiox::AudioStressTest::kMaxDurationSeconds) {
		static const char bad[] = "duration_secs must be between 1 and 30\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8",
									bad, sizeof(bad) - 1);
	}

	int rc = 0;
	if (strcmp(mode, "cpu") == 0) {
		rc = audiox::audioStressTest.startCpuContention(static_cast<uint32_t>(duration));
	} else if (strcmp(mode, "audio") == 0) {
		char workText[16];
		if (!body_get_value(body, body_len, "work_ms", workText, sizeof(workText))) {
			static const char bad[] = "work_ms is required for audio mode\n";
			return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8",
										bad, sizeof(bad) - 1);
		}
		end = nullptr;
		const unsigned long workMs = strtoul(workText, &end, 10);
		if (!end || *end != '\0' || workMs == 0 ||
			workMs > audiox::AudioStressTest::kMaxAudioWorkMsPerBlock) {
			static const char bad[] = "work_ms must be between 1 and 20\n";
			return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8",
										bad, sizeof(bad) - 1);
		}
		rc = audiox::audioStressTest.startAudioThread(static_cast<uint32_t>(duration),
													  static_cast<uint32_t>(workMs));
	} else {
		static const char bad[] = "mode must be audio, cpu, or stop\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8",
									bad, sizeof(bad) - 1);
	}

	if (rc == -EBUSY) {
		static const char busy[] = "another stress test is already running\n";
		return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8",
									busy, sizeof(busy) - 1);
	}
	if (rc < 0) {
		printf("[HTTP] [ERR] start audio stress test: %s\n", strerror(-rc));
		static const char failed[] = "failed to start stress test\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8",
									failed, sizeof(failed) - 1);
	}

	static const char started[] = "{\"ok\":true}\n";
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8",
								started, sizeof(started) - 1);
#endif
}

static int handleVersion(HttpServer *server,
						 int cfd,
						 const char *method,
						 const char *path) {
	(void)method;
	(void)path;
	if (!server) {
		return -1;
	}

#if defined(AUDIOX_VERSION_STRING)
	char out[96];
	int n = snprintf(out, sizeof(out), "%s\n", AUDIOX_VERSION_STRING);
	if (n < 0) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", out, (size_t)n);
#elif defined(AUDIOX_VERSION_MAJOR) && defined(AUDIOX_VERSION_MINOR) && defined(AUDIOX_VERSION_PATCH)
	char out[64];
	int n = snprintf(out,
					 sizeof(out),
					 "%d.%d.%d\n",
					 AUDIOX_VERSION_MAJOR,
					 AUDIOX_VERSION_MINOR,
					 AUDIOX_VERSION_PATCH);
	if (n < 0) {
		n = 0;
	}
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", out, (size_t)n);
#else
	static const char unknown[] = "unknown\n";
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", unknown, sizeof(unknown) - 1);
#endif
}

static int handleMidiLightConfig(HttpServer *server,
								 int cfd,
								 const char *method,
								 const char *path,
								 const char *body,
								 size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method) {
		return -1;
	}

	if (strcmp(method, "GET") == 0) {
		MidiMapData data = server->app->config->readMidiMapFile();
		char out[256];
		int n = snprintf(out, sizeof(out),
						 "{\"channel\":%u,\"mapped_vel\":%u,\"playing_vel\":%u,\"stop_all_vel\":%u}\n",
						 (unsigned)data.globalLight.channel,
						 (unsigned)data.globalLight.mappedVel,
						 (unsigned)data.globalLight.playingVel,
						 (unsigned)data.globalLight.stopAllVel);
		if (n < 0 || (size_t)n >= sizeof(out)) {
			n = 0;
		}
		return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
	}

	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	if (!body) {
		static const char bad[] = "missing body\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char ch_buf[16], mapped_buf[16], stop_all_buf[16], playing_buf[16];
	MidiMapData data = server->app->config->readMidiMapFile();

	if (body_get_value(body, body_len, "channel", ch_buf, sizeof(ch_buf))) {
		char *ep = NULL;
		long v = strtol(ch_buf, &ep, 10);
		if (ep && *ep == '\0' && v >= 0 && v <= 15) {
			data.globalLight.channel = (uint8_t)v;
		}
	}
	if (body_get_value(body, body_len, "mapped_vel", mapped_buf, sizeof(mapped_buf))) {
		char *ep = NULL;
		long v = strtol(mapped_buf, &ep, 10);
		if (ep && *ep == '\0' && v >= 0 && v <= 127) {
			data.globalLight.mappedVel = (uint8_t)v;
		}
	}
	if (body_get_value(body, body_len, "playing_vel", playing_buf, sizeof(playing_buf))) {
		char *ep = NULL;
		long v = strtol(playing_buf, &ep, 10);
		if (ep && *ep == '\0' && v >= 0 && v <= 127) {
			data.globalLight.playingVel = (uint8_t)v;
		}
	}
	if (body_get_value(body, body_len, "stop_all_vel", stop_all_buf, sizeof(stop_all_buf))) {
		char *ep = NULL;
		long v = strtol(stop_all_buf, &ep, 10);
		if (ep && *ep == '\0' && v >= 0 && v <= 127) {
			data.globalLight.stopAllVel = (uint8_t)v;
		}
	}

	if (server->app->config->writeMidiMapFile(&data) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	// Immediately push updated lighting to the controller if connected.
	if (server->app->midi) {
		server->app->midi->cachedMidiMap = data;
		server->app->midi->refreshAllLighting();
	}

	static const char ok[] = "ok\n";
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleMidiLightSounds(HttpServer *server, int cfd, const char *method, const char *path) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method) {
		return -1;
	}
	if (strcmp(method, "GET") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	MidiMapData data = server->app->config->readMidiMapFile();
	char out[HTTP_BODY_MAX];
	size_t off = 0;

	int n = snprintf(out + off, sizeof(out) - off, "{\"count\":%u,\"sounds\":[",
					 (unsigned)data.soundLightCount);
	if (n <= 0 || (size_t)n >= sizeof(out) - off) {
		static const char err[] = "encode failed\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}
	off += (size_t)n;

	uint32_t count = data.soundLightCount;
	if (count > MIDI_SOUND_LIGHTS_MAX) {
		count = MIDI_SOUND_LIGHTS_MAX;
	}
	int first = 1;
	for (uint32_t i = 0; i < count; ++i) {
		if (!data.soundLights[i].sfxPath[0]) {
			continue;
		}
		n = snprintf(out + off, sizeof(out) - off,
					 "%s{\"sfx\":\"%s\",\"mapped_vel\":%u,\"playing_vel\":%u}",
					 first ? "" : ",",
					 data.soundLights[i].sfxPath,
					 (unsigned)data.soundLights[i].mappedVel,
					 (unsigned)data.soundLights[i].playingVel);
		if (n <= 0 || (size_t)n >= sizeof(out) - off) {
			break;
		}
		off += (size_t)n;
		first = 0;
	}

	n = snprintf(out + off, sizeof(out) - off, "]}\n");
	if (n > 0 && (size_t)n < sizeof(out) - off) {
		off += (size_t)n;
	}

	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, off);
}

static int handleMidiLightSoundSet(HttpServer *server,
									int cfd,
									const char *method,
									const char *path,
									const char *body,
									size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method || !body) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char sfx_buf[MIDI_SFX_PATH_MAX];
	char mapped_buf[16], playing_buf[16];
	if (!body_get_value(body, body_len, "sfx", sfx_buf, sizeof(sfx_buf))) {
		static const char bad[] = "expected sfx\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char normalized[MIDI_SFX_PATH_MAX];
	if (mapping_normalize_sfx(sfx_buf, normalized, sizeof(normalized)) != RET_OK) {
		static const char bad[] = "invalid sfx path\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	MidiMapData data = server->app->config->readMidiMapFile();
	uint32_t count = data.soundLightCount;
	if (count > MIDI_SOUND_LIGHTS_MAX) {
		count = MIDI_SOUND_LIGHTS_MAX;
	}

	// Find existing entry or create new.
	int idx = -1;
	for (uint32_t i = 0; i < count; ++i) {
		if (strcmp(data.soundLights[i].sfxPath, normalized) == 0) {
			idx = (int)i;
			break;
		}
	}
	if (idx < 0) {
		if (count >= MIDI_SOUND_LIGHTS_MAX) {
			static const char full[] = "sound light table full\n";
			return server->sendResponse(cfd, "409 Conflict", "text/plain; charset=utf-8", full, sizeof(full) - 1);
		}
		idx = (int)count;
		memset(&data.soundLights[idx], 0, sizeof(data.soundLights[idx]));
		copy_bound(data.soundLights[idx].sfxPath,
				   sizeof(data.soundLights[idx].sfxPath),
				   normalized);
		data.soundLightCount = count + 1;
	}

	MidiSoundLight &sl = data.soundLights[idx];
	if (body_get_value(body, body_len, "mapped_vel", mapped_buf, sizeof(mapped_buf))) {
		char *ep = NULL;
		long v = strtol(mapped_buf, &ep, 10);
		if (ep && *ep == '\0' && v >= 0 && v <= 127) {
			sl.mappedVel = (uint8_t)v;
			sl.hasMapped = 1;
		}
	}
	if (body_get_value(body, body_len, "playing_vel", playing_buf, sizeof(playing_buf))) {
		char *ep = NULL;
		long v = strtol(playing_buf, &ep, 10);
		if (ep && *ep == '\0' && v >= 0 && v <= 127) {
			sl.playingVel = (uint8_t)v;
			sl.hasPlaying = 1;
		}
	}

	if (server->app->config->writeMidiMapFile(&data) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	if (server->app->midi) {
		server->app->midi->cachedMidiMap = data;
		server->app->midi->refreshAllLighting();
	}

	static const char ok[] = "ok\n";
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleMidiLightSoundDelete(HttpServer *server,
									   int cfd,
									   const char *method,
									   const char *path,
									   const char *body,
									   size_t body_len) {
	(void)path;
	if (!server || !server->app || !server->app->config || !method || !body) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	char sfx_buf[MIDI_SFX_PATH_MAX];
	if (!body_get_value(body, body_len, "sfx", sfx_buf, sizeof(sfx_buf))) {
		static const char bad[] = "expected sfx\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	char normalized[MIDI_SFX_PATH_MAX];
	if (mapping_normalize_sfx(sfx_buf, normalized, sizeof(normalized)) != RET_OK) {
		static const char bad[] = "invalid sfx path\n";
		return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
	}

	MidiMapData data = server->app->config->readMidiMapFile();
	uint32_t count = data.soundLightCount;
	if (count > MIDI_SOUND_LIGHTS_MAX) {
		count = MIDI_SOUND_LIGHTS_MAX;
	}

	int idx = -1;
	for (uint32_t i = 0; i < count; ++i) {
		if (strcmp(data.soundLights[i].sfxPath, normalized) == 0) {
			idx = (int)i;
			break;
		}
	}

	if (idx < 0) {
		static const char nf[] = "sound light not found\n";
		return server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8", nf, sizeof(nf) - 1);
	}

	for (uint32_t i = (uint32_t)idx; i + 1 < count; ++i) {
		data.soundLights[i] = data.soundLights[i + 1];
	}
	if (count > 0) {
		data.soundLightCount = count - 1;
	}

	if (server->app->config->writeMidiMapFile(&data) != RET_OK) {
		static const char err[] = "failed to write midi_map\n";
		return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
	}

	if (server->app->midi) {
		server->app->midi->cachedMidiMap = data;
		server->app->midi->refreshAllLighting();
	}

	static const char ok[] = "ok\n";
	return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
}

static std::string scriptScalarJson(const ScriptStateEntry &value) {
    if (value.kind == ScriptValueKind::Boolean) return value.booleanValue ? "true" : "false";
    if (value.kind == ScriptValueKind::String)
        return "\"" + audiox::json::escape(value.stringValue) + "\"";
    char number[64];
    if (value.numberIsInteger)
        snprintf(number, sizeof(number), "%lld", static_cast<long long>(value.integerValue));
    else snprintf(number, sizeof(number), "%.17g", value.numberValue);
    return number;
}

static std::string scriptUiJson(const ScriptDefinition &script) {
    ScriptUi ui{};
    ScriptState settings{};
    char error[192]{};
    if (!scriptInspectUi(script.script, ui, error, sizeof(error)) ||
        ScriptStorage(script.id).readSettings(settings) != RET_OK) {
        if (!error[0]) snprintf(error, sizeof(error), "Cannot read script options: %s", strerror(errno));
        printf("[HTTP] [WARN] Script %u UI: %s\n", script.id, error);
        return "\"ui\":[],\"ui_error_hex\":\"" + scriptHexEncode(error) + "\"";
    }
    std::string result = "\"ui\":[";
    for (unsigned i = 0; i < ui.count; ++i) {
        const auto &control = ui.controls[i];
        const char *type = control.kind == ScriptUiKind::Boolean ? "boolean" :
            control.kind == ScriptUiKind::Number ? "number" :
            control.kind == ScriptUiKind::String ? "string" : "button";
        auto value = control.defaultValue;
        for (unsigned j = 0; j < settings.count; ++j)
            if (!strcmp(settings.entries[j].key, control.key)) value = settings.entries[j];
        if (control.kind != ScriptUiKind::Button && !scriptUiValueValid(control, value)) {
            snprintf(error, sizeof(error), "Saved option %s no longer matches its declaration", control.key);
            printf("[HTTP] [WARN] Script %u UI: %s\n", script.id, error);
            return "\"ui\":[],\"ui_error_hex\":\"" + scriptHexEncode(error) + "\"";
        }
        if (i) result += ',';
        result += "{\"key\":\"" + audiox::json::escape(control.key) +
            "\",\"label\":\"" + audiox::json::escape(control.label) + "\",\"type\":\"" + type + "\"";
        if (control.kind != ScriptUiKind::Button) result += ",\"value\":" + scriptScalarJson(value);
        char bound[64];
        if (control.hasMin) {
            snprintf(bound, sizeof(bound), "%.17g", control.min);
            result += ",\"min\":" + std::string(bound);
        }
        if (control.hasMax) {
            snprintf(bound, sizeof(bound), "%.17g", control.max);
            result += ",\"max\":" + std::string(bound);
        }
        result += '}';
    }
    return result + ']';
}

static int handleScriptControl(HttpServer *server, int cfd, const char *method,
                               const char *body, size_t length) {
    auto reply = [&](const char *status, const char *message) {
        return server->sendResponse(cfd, status, "text/plain; charset=utf-8", message, strlen(message));
    };
    if (strcmp(method, "POST")) return sendMethodNotAllowed(server, cfd);
    if (!server->app || !server->app->config || !server->app->scripts)
        return reply("503 Service Unavailable", "script runtime unavailable\n");
    if (!body || !length || length > 512 || memchr(body, '\0', length))
        return reply("400 Bad Request", "invalid control request\n");
    char idText[32]{}, keyHex[64]{}, valueHex[256]{};
    bool hasId = false, hasKey = false, hasValue = false;
    for (size_t start = 0; start < length;) {
        size_t end = start;
        while (end < length && body[end] != '&' && body[end] != '\n') ++end;
        const char *eq = static_cast<const char *>(memchr(body + start, '=', end - start));
        if (!eq) return reply("400 Bad Request", "invalid control field\n");
        const std::string key(body + start, eq);
        char *target = nullptr;
        size_t capacity = 0;
        bool *present = nullptr;
        if (key == "id") { target = idText; capacity = sizeof(idText); present = &hasId; }
        else if (key == "key_hex") { target = keyHex; capacity = sizeof(keyHex); present = &hasKey; }
        else if (key == "value_hex") { target = valueHex; capacity = sizeof(valueHex); present = &hasValue; }
        else return reply("400 Bad Request", "unknown control field\n");
        const size_t size = static_cast<size_t>(body + end - eq - 1);
        if (*present || size >= capacity) return reply("400 Bad Request", "duplicate or oversized control field\n");
        *present = true;
        memcpy(target, eq + 1, size);
        target[size] = '\0';
        start = end + 1;
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = strtoul(idText, &end, 10);
    char key[32]{};
    if (!hasId || !idText[0] || idText[0] == '-' || *end || errno || !parsed ||
        parsed > UINT32_MAX || !hasKey || !scriptHexDecode(keyHex, key, sizeof(key)) || !key[0])
        return reply("400 Bad Request", "invalid script ID or control key\n");
    auto data = server->app->config->readMidiMapFile();
    unsigned index = 0;
    while (index < data.chordCount && data.chords[index].id != parsed) ++index;
    if (index == data.chordCount) return reply("404 Not Found", "script not found\n");
    ScriptUi ui{};
    char error[192]{};
    if (!scriptInspectUi(data.chords[index].script, ui, error, sizeof(error)))
        return reply("409 Conflict", error);
    const ScriptUiControl *control = nullptr;
    for (unsigned i = 0; i < ui.count; ++i)
        if (!strcmp(ui.controls[i].key, key)) control = &ui.controls[i];
    if (!control) return reply("404 Not Found", "undeclared script control\n");
    const bool button = control->kind == ScriptUiKind::Button;
    if (button == hasValue) return reply("400 Bad Request", "buttons omit values; options require values\n");
    ScriptEvent event{};
    event.kind = button ? ScriptEventKind::Ui : ScriptEventKind::ConfigChange;
    event.fromWeb = true;
    event.hasValue = !button;
    snprintf(event.key, sizeof(event.key), "%s", key);
    snprintf(event.value.key, sizeof(event.value.key), "%s", key);
    if (!button) {
        char value[128]{};
        if (!scriptHexDecode(valueHex, value, sizeof(value)))
            return reply("400 Bad Request", "invalid control value encoding\n");
        if (control->kind == ScriptUiKind::Boolean) {
            if (strcmp(value, "true") && strcmp(value, "false"))
                return reply("400 Bad Request", "expected boolean control value\n");
            event.value.kind = ScriptValueKind::Boolean;
            event.value.booleanValue = !strcmp(value, "true");
        } else if (control->kind == ScriptUiKind::String) {
            event.value.kind = ScriptValueKind::String;
            snprintf(event.value.stringValue, sizeof(event.value.stringValue), "%s", value);
        } else {
            errno = 0;
            char *last = nullptr;
            event.value.kind = ScriptValueKind::Number;
            event.value.numberValue = strtod(value, &last);
            if (!value[0] || *last || errno || !std::isfinite(event.value.numberValue))
                return reply("400 Bad Request", "expected finite numeric control value\n");
        }
        if (!scriptUiValueValid(*control, event.value))
            return reply("400 Bad Request", "control value violates declared type or range\n");
    }
    auto *runtime = server->app->scripts;
    runtime->midi.cachedMidiMap = data;
    runtime->syncMacros();
    if (index >= runtime->compiledMacroCount || !runtime->macroValid[index] ||
        (button && !(runtime->macroEventMasks[index] & (1U << 11))))
        return reply("409 Conflict", "script unavailable or button has no ui hook\n");
    const bool notify = runtime->macroEventMasks[index] & (1U << static_cast<unsigned>(event.kind));
    if (notify && runtime->macroQueueCount == SCRIPTS_MAX)
        return reply("409 Conflict", "script trigger queue full\n");
    if (notify && !runtime->queueMacroEvent(index, event))
        return reply("409 Conflict", "cannot capture or queue script event\n");
    if (!button && ScriptStorage(data.chords[index].id).setSetting(event.value) != RET_OK) {
        if (notify) --runtime->macroQueueCount;
        snprintf(error, sizeof(error), "Cannot persist script option: %s\n", strerror(errno));
        printf("[HTTP] [WARN] %s", error);
        return reply("500 Internal Server Error", error);
    }
    return reply("200 OK", button ? "button queued\n" : notify ?
                 "setting saved and config_change queued\n" : "setting saved\n");
}

static int handlePlugins(HttpServer *server, int cfd, const char *method, const char *path,
                         const char *body, size_t length) {
    auto reply = [&](const char *status, const std::string &message, const char *mime = "text/plain; charset=utf-8") {
        return server->sendResponse(cfd, status, mime, message.data(), message.size());
    };
    std::string error;
    if (!strcmp(path, "/api/plugins")) {
        if (strcmp(method, "GET")) return sendMethodNotAllowed(server, cfd);
        std::vector<audiox::plugin::PackageMetadata> catalog;
        if (!audiox::plugin::listPackages(audiox::plugin::ROOT, catalog, error))
            return reply("500 Internal Server Error", error);
        std::string out = "{\"plugins\":[";
        for (size_t i = 0; i < catalog.size(); ++i) {
            const auto &plugin = catalog[i];
            const bool hasUi = audiox::plugin::hasUi(audiox::plugin::findType(plugin.id));
            if (i) out += ',';
            out += "{\"id\":\"" + audiox::json::escape(plugin.id) + "\",\"name\":\"" +
                audiox::json::escape(plugin.name) + "\",\"version\":\"" + audiox::json::escape(plugin.version) +
                "\",\"ui\":" + (hasUi ? "true" : "false") + ",\"params\":[";
            for (unsigned p = 0; p < plugin.paramCount; ++p) {
                const auto &param = plugin.params[p];
                char numbers[128];
                snprintf(numbers, sizeof(numbers), "\",\"min\":%.9g,\"max\":%.9g,\"default\":%.9g}",
                         param.min, param.max, param.defaultValue);
                if (p) out += ',';
                out += "{\"name\":\"" + audiox::json::escape(param.name) + "\",\"label\":\"" +
                    audiox::json::escape(param.label) + numbers;
            }
            out += "]}";
        }
        return reply("200 OK", out + "]}\n", "application/json; charset=utf-8");
    }
    if (!strcmp(path, "/api/plugins/install")) {
        if (strcmp(method, "PUT")) return sendMethodNotAllowed(server, cfd);
        audiox::plugin::Package package;
        if (!body || !audiox::plugin::parsePackage(reinterpret_cast<const uint8_t *>(body), length, package, error))
            return reply("400 Bad Request", error.empty() ? "invalid AXM package\n" : error);
        const bool previouslyAvailable = audiox::plugin::available(audiox::plugin::findType(package.metadata.id));
        if (previouslyAvailable) return reply("409 Conflict", "plugin already installed; remove it explicitly first\n");
        const bool ui = std::any_of(package.entries.begin(), package.entries.end(),
            [](const auto &entry) { return entry.kind == audiox::plugin::Asset && entry.name == "index.html"; });
        if (!audiox::plugin::registerPackage(package.metadata, error, ui))
            return reply("409 Conflict", error);
        if (!audiox::plugin::installPackage(reinterpret_cast<const uint8_t *>(body), length, audiox::plugin::ROOT, error)) {
            if (!previouslyAvailable) audiox::plugin::markUnavailable(audiox::plugin::findType(package.metadata.id));
            return reply("409 Conflict", error);
        }
        return reply("200 OK", "plugin installed\n");
    }
    if (!strcmp(path, "/api/plugins/remove") || !strcmp(path, "/api/plugins/reload")) {
        if (strcmp(method, "POST")) return sendMethodNotAllowed(server, cfd);
        char id[24]{};
        if (!body || length > 64 || memchr(body, '\0', length) ||
            !body_get_value(body, length, "id", id, sizeof(id)) || !audiox::plugin::validTypeName(id))
            return reply("400 Bad Request", "invalid plugin ID\n");
        auto *audio = server->app ? server->app->audio : nullptr;
        if (!audio) return reply("503 Service Unavailable", "audio runtime unavailable\n");
        if (!strcmp(path, "/api/plugins/reload")) {
            if (audio->reloadPlugins(id) != RET_OK)
                return reply("409 Conflict", "plugin reload failed; check device logs\n");
            return reply("200 OK", "plugin workers reloaded\n");
        }
        if (audio->pluginInUse(id)) return reply("409 Conflict", "remove all effects using this plugin first\n");
        if (server->app->config) {
            const auto map = server->app->config->readMidiMapFile();
            for (uint32_t i = 0; i < map.effectStateCount && i < MIDI_EFFECT_STATES_MAX; ++i)
                if (!strcmp(map.effectStates[i].type, id))
                    return reply("409 Conflict", "saved effects still reference this plugin\n");
        }
        if (!audiox::plugin::removePackage(id, audiox::plugin::ROOT, error))
            return reply("409 Conflict", error);
        audiox::plugin::markUnavailable(audiox::plugin::findType(id));
        return reply("200 OK", "plugin removed\n");
    }
    const std::string prefix = "/api/plugins/";
    std::string assetPath = path;
    const size_t separator = assetPath.find("/assets/", prefix.size());
    if (separator != std::string::npos && assetPath.compare(0, prefix.size(), prefix) == 0) {
        if (strcmp(method, "GET")) return sendMethodNotAllowed(server, cfd);
        const std::string id = assetPath.substr(prefix.size(), separator - prefix.size());
        const std::string name = assetPath.substr(separator + 8);
        std::string asset;
        if (!audiox::plugin::readPluginAsset(id.c_str(), name.c_str(), audiox::plugin::ROOT, asset, error))
            return reply("404 Not Found", error);
        const char *mime = name.size() > 3 && name.compare(name.size() - 3, 3, ".js") == 0 ?
            "application/javascript; charset=utf-8" : name.size() > 4 &&
            name.compare(name.size() - 4, 4, ".css") == 0 ? "text/css; charset=utf-8" : "text/html; charset=utf-8";
        const std::string source = "http://" + std::string(server->requestHost[0] ?
            server->requestHost : AUDIOX_USB_NET_SERVER_IP) + prefix + id + "/assets/";
        const std::string policy = "X-Content-Type-Options: nosniff\r\nCache-Control: no-store\r\n"
            "Content-Security-Policy: sandbox allow-scripts; default-src 'none'; script-src 'unsafe-inline' " +
            source + "; style-src 'unsafe-inline' " + source +
            "; connect-src 'none'; img-src data:; form-action 'none'; "
            "base-uri 'none'; frame-src 'none'\r\n";
        return server->sendResponseWithHeaders(cfd, "200 OK", mime, asset.data(), asset.size(), nullptr, policy.c_str());
    }
    return reply("404 Not Found", "unknown plugin endpoint\n");
}

static int handleMidiChords(HttpServer *server, int cfd, const char *method,
                           const char *body, size_t body_len, bool named = false, bool run = false,
                           bool scripts = false) {
	if (!server || !server->app || !server->app->config) return -1;
	auto reply = [&](const char *status, const char *text) {
		return server->sendResponse(cfd, status, "text/plain; charset=utf-8", text, strlen(text));
	};
	if (strcmp(method, "GET") && strcmp(method, "POST") && strcmp(method, "DELETE")) {
		return sendMethodNotAllowed(server, cfd);
	}
	if (run && strcmp(method, "POST")) return sendMethodNotAllowed(server, cfd);
	MidiMapData data = server->app->config->readMidiMapFile();
	if (strcmp(method, "GET") != 0) {
		if (!body || body_len > SCRIPT_STORAGE_MAX * 2 + 512 || memchr(body, '\0', body_len)) {
			return reply("400 Bad Request", "invalid script request size or encoding\n");
		}
		auto field = [&](const char *key, char *out, size_t capacity) {
			int found = 0;
			for (size_t start = 0; start < body_len;) {
				size_t end = start;
				while (end < body_len && body[end] != '&' && body[end] != '\n') ++end;
				size_t keySize = strlen(key);
				if (end - start > keySize && !memcmp(body + start, key, keySize) && body[start + keySize] == '=') {
					size_t size = end - start - keySize - 1;
					if (found || size >= capacity) return -1;
					memcpy(out, body + start + keySize + 1, size);
					out[size] = '\0';
					found = 1;
				}
				start = end + 1;
			}
			return found;
		};
		char channelText[32]{}, noteText[32]{};
		uint32_t id = 0;
		char idText[32]{};
		int idPresent = field("id", idText, sizeof(idText));
		if (named && idPresent != 0) {
			char *end = nullptr;
			errno = 0;
			unsigned long parsed = strtoul(idText, &end, 10);
			if (idPresent != 1 || !idText[0] || idText[0] == '-' || *end || errno ||
				!parsed || parsed > UINT32_MAX)
				return reply("400 Bad Request", "invalid script id\n");
			id = static_cast<uint32_t>(parsed);
		}
		if (named && (run || !strcmp(method, "DELETE")) && !id)
			return reply("400 Bad Request", "expected script id\n");
		bool needsBinding = !named || (!run && strcmp(method, "DELETE"));
		if (needsBinding && (field("channel", channelText, sizeof(channelText)) != 1 ||
			field("note", noteText, sizeof(noteText)) != 1)) {
			return reply("400 Bad Request", "expected channel and note\n");
		}
		char *endChannel = nullptr, *endNote = nullptr;
		long channel = strtol(channelText, &endChannel, 10), note = strtol(noteText, &endNote, 10);
		if (needsBinding && (!channelText[0] || !noteText[0] || *endChannel || *endNote ||
			channel < 0 || channel > 15 || note < 0 || note > 127)) {
			return reply("400 Bad Request", "channel must be 0-15; note must be 0-127\n");
		}
		uint32_t index = 0;
		while (index < data.chordCount && (named ? data.chords[index].id != id :
			(!data.chords[index].midiEnabled || data.chords[index].channel != channel ||
			 data.chords[index].note != note || !(data.chords[index].eventMask & 1)))) ++index;
		if (named && id && index == data.chordCount)
			return reply("404 Not Found", "script not found\n");
		if (run) {
			auto *runtime = server->app->scripts;
			if (!runtime) return reply("503 Service Unavailable", "automation runtime unavailable\n");
			runtime->midi.cachedMidiMap = data;
			runtime->syncMacros();
			if (index < runtime->compiledMacroCount && runtime->compiledMacros[index].hookMode &&
				runtime->macroValid[index] && !(runtime->macroEventMasks[index] & 128))
				return reply("409 Conflict", "callback script has no run hook\n");
			if (!runtime->triggerMacro(index, 127, true))
				return reply("409 Conflict", "script is invalid or its trigger queue is full\n");
			return reply("200 OK", "queued\n");
		}
		if (strcmp(method, "DELETE") == 0) {
			if (index < data.chordCount) {
				for (uint32_t j = index + 1; j < data.chordCount; ++j) data.chords[j - 1] = data.chords[j];
				--data.chordCount;
			}
		} else {
			ScriptDefinition chord{};
			if (index < data.chordCount) chord = data.chords[index];
			if (named) {
				char nameHex[128]{}, enabled[4]{};
				if (field("name_hex", nameHex, sizeof(nameHex)) != 1 ||
					!scriptHexDecode(nameHex, chord.name, sizeof(chord.name)) || !chord.name[0] ||
					field("midi_enabled", enabled, sizeof(enabled)) != 1 ||
					(strcmp(enabled, "0") && strcmp(enabled, "1")))
					return reply("400 Bad Request", "expected name (1-63 bytes) and midi_enabled=0/1\n");
				for (const unsigned char *p = reinterpret_cast<const unsigned char *>(chord.name); *p; ++p)
					if (*p < 32 || *p == 127) return reply("400 Bad Request", "name cannot contain control characters\n");
				chord.midiEnabled = !strcmp(enabled, "1");
			}
			char hex[SCRIPT_STORAGE_MAX * 2]{}, velocity[32]{};
			chord.keys[0] = chord.script[0] = '\0';
			int keysField = field("keys", chord.keys, sizeof(chord.keys));
			int scriptField = field("script_hex", hex, sizeof(hex));
			if (keysField < 0 || scriptField < 0 ||
				!scriptHexDecode(hex, chord.script, sizeof(chord.script))) {
				return reply("400 Bad Request", "invalid legacy shortcut or script encoding\n");
			}
			uint8_t report[8];
			if ((!chord.script[0] && !scriptChordReport(chord.keys, report)) ||
				(chord.keys[0] && !scriptChordReport(chord.keys, report))) {
				return reply("400 Bad Request", "invalid chord; use e.g. CTRL+SHIFT+F13 (up to six keys)\n");
			}
			char error[192]{};
			char hooks[4]{};
			int hooksField = field("hook_mode", hooks, sizeof(hooks));
			if (hooksField < 0 || (hooksField && strcmp(hooks, "0") && strcmp(hooks, "1")))
				return reply("400 Bad Request", "hook_mode must be 0 or 1\n");
			bool hookMode = hooksField == 1 && !strcmp(hooks, "1");
			if (scripts) {
				if ((hooksField && !hookMode) || chord.keys[0] || !chord.script[0])
					return reply("400 Bad Request", "Scripts requires Lua hook callbacks, not sequences or keyboard chords\n");
				hookMode = true;
			}
			if (chord.hookMode && !hookMode) chord.eventMask = 1;
			chord.hookMode = hookMode;
			if (chord.hookMode && (!named || !chord.script[0]))
				return reply("400 Bad Request", "callbacks require a named Lua script\n");
			if (chord.script[0] && !scriptValidate(chord.script, error, sizeof(error))) {
				return reply("400 Bad Request", error);
			}
			char eventMask[8]{}, acknowledge[4]{};
			int maskField = field("event_mask", eventMask, sizeof(eventMask));
			if (maskField < 0) return reply("400 Bad Request", "invalid event mask\n");
			if (maskField) {
				char *end = nullptr;
				long mask = strtol(eventMask, &end, 10);
				if (!eventMask[0] || *end || mask < 0 ||
					mask > (chord.hookMode ? SCRIPT_EVENTS_MASK : SCRIPT_LEGACY_EVENTS_MASK))
					return reply("400 Bad Request", "invalid event_mask for this script mode\n");
				chord.eventMask = static_cast<uint16_t>(mask);
			}
			if (!named && chord.eventMask != 1)
				return reply("400 Bad Request", "event subscriptions require the named scripts API\n");
			if (chord.hookMode && !scriptInspectHooks(chord.script, chord.eventMask, error, sizeof(error)))
				return reply("400 Bad Request", error);
			int ackField = field("config_write_ack", acknowledge, sizeof(acknowledge));
			if (ackField < 0 || (ackField && strcmp(acknowledge, "0") && strcmp(acknowledge, "1")))
				return reply("400 Bad Request", "config_write_ack must be 0 or 1\n");
			chord.configWriteAllowed = ackField == 1 && !strcmp(acknowledge, "1");
			if (scriptNeedsDevicePermission(chord.script) && !chord.configWriteAllowed)
				return reply("409 Conflict", "This script accesses device configuration or management. Confirm the device-management/configuration-write warning before saving.\n");
			if (!scriptMigrate(chord, error, sizeof(error)))
				return reply("400 Bad Request", error);
			auto readVelocity = [&](const char *name, uint8_t &target) {
				int present = field(name, velocity, sizeof(velocity));
				if (present < 0) return false;
				if (present) {
					char *end = nullptr;
					long value = strtol(velocity, &end, 10);
					if (!velocity[0] || *end || value < 0 || value > 127) return false;
					target = value;
				}
				return true;
			};
			if (!readVelocity("idle_velocity", chord.idleVelocity) ||
				!readVelocity("pressed_velocity", chord.pressedVelocity)) {
				return reply("400 Bad Request", "lighting velocities must be 0-127\n");
			}
			if (index == SCRIPTS_MAX) return reply("409 Conflict", "maximum 16 chord mappings\n");
			if (index == data.chordCount) ++data.chordCount;
			chord.channel = channel;
			chord.note = note;
			for (uint32_t j = 0; j < data.chordCount; ++j) {
				if (j != index && scriptMacroTriggersOverlap(chord, data.chords[j]))
					return reply("409 Conflict", "MIDI trigger already assigned to another script\n");
			}
			if (!chord.id) {
			    uint32_t minimum = 1;
			    for (uint32_t j = 0; j < data.chordCount; ++j) {
			        if (data.chords[j].id == UINT32_MAX)
			            return reply("409 Conflict", "script ID space exhausted\n");
			        if (data.chords[j].id >= minimum) minimum = data.chords[j].id + 1;
			    }
			    if (ScriptStorage(0).nextId(minimum, chord.id) != RET_OK)
			        return reply("500 Internal Server Error", "cannot allocate isolated script storage identity\n");
			}
			data.chords[index] = chord;
		}
		if (server->app->config->writeMidiMapFile(&data) != RET_OK) {
			return reply("500 Internal Server Error", "failed to save chord mappings\n");
		}
		if (server->app->midi) {
			server->app->midi->cachedMidiMap = data;
			server->app->midi->scripts.syncMacros();
			server->app->midi->refreshAllLighting();
		}
	}
	std::string out = scripts ? "{\"scripts\":[" : named ? "{\"macros\":[" : "{\"chords\":[";
	for (uint32_t i = 0; i < data.chordCount; ++i) {
		const auto &chord = data.chords[i];
		char row[160];
		snprintf(row, sizeof(row), "%s{\"channel\":%u,\"note\":%u,\"keys\":\"%s\",",
			i ? "," : "", chord.channel, chord.note, chord.keys);
		out += row;
		if (named) {
			snprintf(row, sizeof(row), "\"id\":%u,\"midi_enabled\":%s,\"name\":\"",
				chord.id, chord.midiEnabled ? "true" : "false");
			out += row;
			for (const unsigned char *p = reinterpret_cast<const unsigned char *>(chord.name); *p; ++p) {
				if (*p == '"' || *p == '\\') out += '\\';
				if (*p < 32) {
					char escaped[7];
					snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
					out += escaped;
				} else out += static_cast<char>(*p);
			}
			out += "\",";
		}
		snprintf(row, sizeof(row), "\"event_mask\":%u,\"hook_mode\":%s,\"config_write_allowed\":%s,\"idle_velocity\":%u,\"pressed_velocity\":%u,\"script_hex\":\"",
			chord.eventMask, chord.hookMode ? "true" : "false", chord.configWriteAllowed ? "true" : "false",
			chord.idleVelocity, chord.pressedVelocity);
		out += row;
		out += scriptHexEncode(chord.script);
		out += "\",\"runtime_error_hex\":\"";
		if (server->app->scripts && i < server->app->scripts->compiledMacroCount &&
			scriptMacroEqual(chord, server->app->scripts->compiledMacros[i])) {
			out += scriptHexEncode(server->app->scripts->macroErrors[i]);
		}
		out += "\"";
		if (scripts) out += "," + scriptUiJson(chord);
		out += "}";
	}
	out += "]}\n";
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out.data(), out.size());
}

static int handleMidiLightDim(HttpServer *server, int cfd, const char *method,
							  const char *body, size_t body_len) {
	if (!server || !server->app || !method) {
		return -1;
	}
	if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}
	if (!server->app->midi) {
		static const char err[] = "{\"ok\":false,\"error\":\"midi unavailable\"}\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}
	MidiContext *midi = server->app->midi;
	if (strcmp(method, "POST") == 0) {
		char dimmed[16];
		if (!body || !body_get_value(body, body_len, "dimmed", dimmed, sizeof(dimmed)) ||
			(strcmp(dimmed, "0") != 0 && strcmp(dimmed, "1") != 0)) {
			static const char err[] = "{\"ok\":false,\"error\":\"dimmed must be 0 or 1\"}\n";
			return server->sendResponse(cfd, "400 Bad Request", "application/json; charset=utf-8", err, sizeof(err) - 1);
		}
		midi->setLightsDimmed(strcmp(dimmed, "1") == 0);
	}
	const char *out = midi->lightsDimmed ? "{\"ok\":true,\"dimmed\":true}\n" : "{\"ok\":true,\"dimmed\":false}\n";
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, strlen(out));
}

static int handleMidiLightRefresh(HttpServer *server, int cfd, const char *method, const char *path) {
	(void)path;
	if (!server || !server->app || !method) {
		return -1;
	}
	if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
		return sendMethodNotAllowed(server, cfd);
	}

	if (!server->app->midi) {
		static const char err[] = "{\"ok\":false,\"error\":\"midi unavailable\"}\n";
		return server->sendResponse(cfd, "503 Service Unavailable", "application/json; charset=utf-8", err, sizeof(err) - 1);
	}

	if (server->app->config) {
		server->app->midi->cachedMidiMap = server->app->config->readMidiMapFile();
	}
	server->app->midi->refreshAllLighting();

	static const char ok[] = "{\"ok\":true}\n";
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
}

} // namespace

int handleApiRequest(HttpServer *server,
					 int cfd,
					 const char *method,
					 const char *path,
					 const char *body,
					 size_t body_len,
					 const char *query) {
	if (!server || cfd < 0 || !method || !path) {
		return -1;
	}
	if (!strcmp(path, "/api/plugins") || !strncmp(path, "/api/plugins/", 13))
	    return handlePlugins(server, cfd, method, path, body, body_len);

	if (strcmp(path, "/api/system/logs") == 0) return handleSystemLogs(server, cfd, method, query);

	if (strncmp(path, HTTP_SOUNDBOARD_TRIGGER_PREFIX, strlen(HTTP_SOUNDBOARD_TRIGGER_PREFIX)) == 0) {
		if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
			return sendMethodNotAllowed(server, cfd);
		}
		int rc = handleSoundboardTrigger(server, cfd, method, path, HTTP_SOUNDBOARD_TRIGGER_PREFIX, 0);
		if (rc == RET_WARN) {
			return sendNotImplemented(server, cfd, "soundboard trigger");
		}
		return rc;
	}

	if (strncmp(path, HTTP_SOUNDBOARD_PRESS_PREFIX, strlen(HTTP_SOUNDBOARD_PRESS_PREFIX)) == 0) {
		if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
			return sendMethodNotAllowed(server, cfd);
		}
		int rc = handleSoundboardTrigger(server, cfd, method, path, HTTP_SOUNDBOARD_PRESS_PREFIX, 1);
		if (rc == RET_WARN) {
			return sendNotImplemented(server, cfd, "soundboard press");
		}
		return rc;
	}

	if (strncmp(path, HTTP_SOUNDBOARD_RELEASE_PREFIX, strlen(HTTP_SOUNDBOARD_RELEASE_PREFIX)) == 0) {
		if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
			return sendMethodNotAllowed(server, cfd);
		}
		int rc = handleSoundboardTrigger(server, cfd, method, path, HTTP_SOUNDBOARD_RELEASE_PREFIX, 2);
		if (rc == RET_WARN) {
			return sendNotImplemented(server, cfd, "soundboard release");
		}
		return rc;
	}

	if (strcmp(path, HTTP_SOUNDBOARD_MODE_PATH) == 0) {
		return handleSoundboardMode(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_SOUNDBOARD_STOP_ALL_PATH) == 0) {
		return handleSoundboardStopAll(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_SOUNDBOARD_MODES_PATH) == 0) {
		return handleSoundboardModes(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_SOUNDBOARD_ORDER_PATH) == 0) {
		return handleSoundboardOrder(server, cfd, method, body, body_len);
	}

	if (strcmp(path, HTTP_SOUNDBOARD_LAYOUT_PATH) == 0) {
		return handleSoundboardLayout(server, cfd, method, body, body_len);
	}

	if (strcmp(path, HTTP_SOUNDBOARD_MODE_SET_PATH) == 0) {
		return handleSoundboardModeSet(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_SOUNDBOARD_RELOAD_PATH) == 0) {
		return handleSoundboardReload(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_FRAMEBUFFER_VIEW_PATH) == 0) {
		return handleFramebufferView(server, cfd, method, body, body_len);
	}
	if (strcmp(path, HTTP_UPDATE_NOTIFICATIONS_PATH) == 0) {
		return handleUpdateNotifications(server, cfd, method, body, body_len);
	}

	if (strcmp(path, HTTP_CONFIG_RELOAD_PATH) == 0) {
		if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
			return sendMethodNotAllowed(server, cfd);
		}
		int rc = handleConfigReload(server, cfd, method, path);
		if (rc == RET_WARN) {
			return sendNotImplemented(server, cfd, "config reload");
		}
		return rc;
	}

	if (strcmp(path, HTTP_ROUTING_RELOAD_PATH) == 0) {
		if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
			return sendMethodNotAllowed(server, cfd);
		}
		int rc = handleRoutingReload(server, cfd, method, path);
		if (rc == RET_WARN) {
			return sendNotImplemented(server, cfd, "routing reload");
		}
		return rc;
	}

	if (strcmp(path, HTTP_BOOT_CONFIG_PATH) == 0) {
		return handleBootConfig(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_BOOT_CMDLINE_PATH) == 0) {
		return handleBootCmdline(server, cfd, method, body, body_len);
	}

	if (strcmp(path, HTTP_ROUTING_THINGS_PATH) == 0) {
		if (strcmp(method, "GET") != 0) {
			return sendMethodNotAllowed(server, cfd);
		}
		int rc = handleRoutingThings(server, cfd, method, path);
		if (rc == RET_WARN) {
			return sendNotImplemented(server, cfd, "routing things");
		}
		return rc;
	}

	if (strcmp(path, HTTP_ROUTING_HELPER_CREATE_PATH) == 0) {
		return handleRoutingHelper(server, cfd, method, path, true);
	}

	if (strcmp(path, HTTP_ROUTING_HELPER_DELETE_PATH) == 0) {
		return handleRoutingHelper(server, cfd, method, path, false);
	}

	if (strcmp(path, HTTP_BLUETOOTH_DEVICES_PATH) == 0) {
		return handleBluetoothDevices(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_BLUETOOTH_SCAN_PATH) == 0) {
		return handleBluetoothScan(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_BLUETOOTH_PAIR_PATH) == 0) {
		return handleBluetoothPair(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_BLUETOOTH_CONNECT_PATH) == 0) {
		return handleBluetoothConnect(server, cfd, method, body, body_len);
	}

	if (strcmp(path, HTTP_BLUETOOTH_DISCONNECT_PATH) == 0) {
		return handleBluetoothDisconnect(server, cfd, method, body, body_len);
	}

	if (strcmp(path, HTTP_BLUETOOTH_UNPAIR_PATH) == 0) {
		return handleBluetoothUnpair(server, cfd, method, body, body_len);
	}

	if (strcmp(path, HTTP_BLUETOOTH_AUTO_CONNECT_PATH) == 0) {
		return handleBluetoothAutoConnect(server, cfd, method, body, body_len);
	}

	if (strcmp(path, HTTP_SYSTEM_SYNC_PATH) == 0) {
		if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
			return sendMethodNotAllowed(server, cfd);
		}
		int rc = handleSystemSync(server, cfd, method, path);
		if (rc == RET_WARN) {
			return sendNotImplemented(server, cfd, "system sync");
		}
		return rc;
	}

	if (strcmp(path, HTTP_SYSTEM_RESTART_PATH) == 0) {
		if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
			return sendMethodNotAllowed(server, cfd);
		}
		int rc = handleSystemRestart(server, cfd, method, path);
		if (rc == RET_WARN) {
			return sendNotImplemented(server, cfd, "system restart");
		}
		return rc;
	}

	if (strcmp(path, HTTP_SYSTEM_SHUTDOWN_PATH) == 0) {
		if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0) {
			return sendMethodNotAllowed(server, cfd);
		}
		int rc = handleSystemShutdown(server, cfd, method, path);
		if (rc == RET_WARN) {
			return sendNotImplemented(server, cfd, "system shutdown");
		}
		return rc;
	}

	if (strcmp(path, HTTP_SYSTEM_BOOT_FAILURE_PATH) == 0) {
		return handleSystemBootFailure(server, cfd, method);
	}

	if (strcmp(path, HTTP_SYSTEM_AUDIO_STRESS_PATH) == 0) {
		return handleSystemAudioStress(server, cfd, method, body, body_len);
	}

	if (strcmp(path, HTTP_SYSTEM_FAN_PATH) == 0) {
		return handlePi5FanConfig(server, cfd, method, body, body_len);
	}

	if (strcmp(path, HTTP_SYSTEM_INFO_PATH) == 0) {
		if (strcmp(method, "GET") != 0) {
			return sendMethodNotAllowed(server, cfd);
		}
		return handleSystemInfo(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_VERSION_PATH) == 0) {
		if (strcmp(method, "GET") != 0) {
			return sendMethodNotAllowed(server, cfd);
		}
		int rc = handleVersion(server, cfd, method, path);
		if (rc == RET_WARN) {
			return sendNotImplemented(server, cfd, "version");
		}
		return rc;
	}

	if (strcmp(path, HTTP_MIDI_LAST_NOTE_PATH) == 0) {
		return handleMidiLastNote(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_MIDI_MAPPINGS_PATH) == 0) {
		return handleMidiMappings(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_MIDI_MAPPING_SET_PATH) == 0) {
		return handleMidiMappingSet(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_MIDI_MAPPING_DELETE_PATH) == 0) {
		return handleMidiMappingDelete(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_MIDI_ACTIONS_PATH) == 0) {
		return handleMidiActions(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_MIDI_ACTION_SET_PATH) == 0) {
		return handleMidiActionSet(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_MIDI_ACTION_DELETE_PATH) == 0) {
		return handleMidiActionDelete(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_AUDIO_DEVICES_PATH) == 0) {
		return handleAudioDevices(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_AUDIO_METERS_PATH) == 0) {
		return handleAudioMeters(server, cfd, method);
	}

	if (strcmp(path, HTTP_AUDIO_RESCAN_PATH) == 0) {
		return handleAudioRescan(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_MIDI_LIGHT_CONFIG_PATH) == 0) {
		return handleMidiLightConfig(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_MIDI_LIGHT_SOUNDS_PATH) == 0) {
		return handleMidiLightSounds(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_MIDI_LIGHT_SOUND_SET_PATH) == 0) {
		return handleMidiLightSoundSet(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_MIDI_LIGHT_SOUND_DELETE_PATH) == 0) {
		return handleMidiLightSoundDelete(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_MIDI_LIGHT_REFRESH_PATH) == 0) {
		return handleMidiLightRefresh(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_MIDI_LIGHT_DIM_PATH) == 0) {
		return handleMidiLightDim(server, cfd, method, body, body_len);
	}
	if (strcmp(path, HTTP_API_PREFIX "midi/chords") == 0) {
		return handleMidiChords(server, cfd, method, body, body_len);
	}
	if (strcmp(path, HTTP_API_PREFIX "macros") == 0 ||
		strcmp(path, HTTP_API_PREFIX "macros/run") == 0) {
		return handleMidiChords(server, cfd, method, body, body_len, true,
			strcmp(path, HTTP_API_PREFIX "macros/run") == 0);
	}
	if (strcmp(path, HTTP_API_PREFIX "scripts/control") == 0)
	    return handleScriptControl(server, cfd, method, body, body_len);
	if (strcmp(path, HTTP_API_PREFIX "scripts") == 0 ||
		strcmp(path, HTTP_API_PREFIX "scripts/run") == 0) {
		return handleMidiChords(server, cfd, method, body, body_len, true,
			strcmp(path, HTTP_API_PREFIX "scripts/run") == 0, true);
	}

	if (strcmp(path, HTTP_MIDI_SAMPLER_CONFIG_PATH) == 0) {
		return handleMidiSamplerConfig(server, cfd, method, path, body, body_len);
	}
	if (strcmp(path, HTTP_MIDI_SAMPLER_MODE_PATH) == 0) {
		return handleMidiSamplerMode(server, cfd, method);
	}

	if (strcmp(path, HTTP_MIDI_LAST_CC_PATH) == 0) {
		return handleMidiLastCc(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_MIDI_CC_VOLUMES_PATH) == 0) {
		return handleMidiCcVolumes(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_MIDI_CC_VOLUME_SET_PATH) == 0) {
		return handleMidiCcVolumeSet(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_MIDI_CC_VOLUME_DELETE_PATH) == 0) {
		return handleMidiCcVolumeDelete(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_EFFECTS_PATH) == 0) {
		return handleEffects(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_EFFECT_SET_PATH) == 0) {
		return handleEffectSet(server, cfd, method, path, body, body_len);
	}
	if (strcmp(path, HTTP_API_PREFIX "effect/name") == 0) {
		return handleEffectName(server, cfd, method, body, body_len);
	}

	if (strcmp(path, HTTP_EFFECT_CREATE_PATH) == 0) {
		return handleEffectCreate(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_EFFECT_DELETE_PATH) == 0) {
		return handleEffectDelete(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_EFFECT_MIDI_CC_SET_PATH) == 0) {
		return handleEffectMidiCcSet(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_EFFECT_MIDI_CC_DELETE_PATH) == 0) {
		return handleEffectMidiCcDelete(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_EFFECT_MIDI_TOGGLE_SET_PATH) == 0) {
		return handleEffectMidiToggleSet(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_EFFECT_MIDI_TOGGLE_DELETE_PATH) == 0) {
		return handleEffectMidiToggleDelete(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_EFFECT_MIDI_LIGHT_SET_PATH) == 0) {
		return handleEffectMidiLightSet(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_EFFECT_MIDI_LIGHT_DELETE_PATH) == 0) {
		return handleEffectMidiLightDelete(server, cfd, method, path, body, body_len);
	}

	if (strcmp(path, HTTP_AUDIO_VOLUMES_PATH) == 0) {
		return handleAudioVolumes(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_AUDIO_VOLUME_SET_PATH) == 0) {
		return handleAudioVolumeSet(server, cfd, method, path, body, body_len);
	}

	if (pathIsRootfs(path)) {
		if (strcmp(method, "GET") == 0) {
			return handleApiGetRootfs(server, cfd, path);
		}
		if (strcmp(method, "PUT") == 0) {
			return handleApiPutRootfs(server, cfd, path, body, body_len);
		}
		if (strcmp(method, "DELETE") == 0) {
			return handleApiDeleteRootfs(server, cfd, path);
		}
		return sendMethodNotAllowed(server, cfd);
	}

	static const char not_found[] = "404 Not Found\n";
	return server->sendResponse(cfd,
								"404 Not Found",
								"text/plain; charset=utf-8",
								not_found,
								sizeof(not_found) - 1);
}
