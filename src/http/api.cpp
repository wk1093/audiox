#include "http/context.hpp"
#include "audio/context.hpp"
#include "config/context.hpp"
#include "init.hpp"
#include "midi/context.hpp"

#include <ctype.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#define HTTP_BODY_MAX 49152
#define HTTP_API_PREFIX "/api/"
#define HTTP_ROOTFS_PREFIX HTTP_API_PREFIX "rootfs/"
#define HTTP_SOUNDBOARD_TRIGGER_PREFIX HTTP_API_PREFIX "soundboard/trigger/"
#define HTTP_SOUNDBOARD_PRESS_PREFIX HTTP_API_PREFIX "soundboard/press/"
#define HTTP_SOUNDBOARD_RELEASE_PREFIX HTTP_API_PREFIX "soundboard/release/"
#define HTTP_SOUNDBOARD_MODE_PATH HTTP_API_PREFIX "soundboard/mode"
#define HTTP_SOUNDBOARD_MODES_PATH HTTP_API_PREFIX "soundboard/modes"
#define HTTP_SOUNDBOARD_MODE_SET_PATH HTTP_API_PREFIX "soundboard/mode/set"
#define HTTP_CONFIG_RELOAD_PATH HTTP_API_PREFIX "config/reload"
#define HTTP_ROUTING_RELOAD_PATH HTTP_API_PREFIX "routing/reload"
#define HTTP_ROUTING_THINGS_PATH HTTP_API_PREFIX "routing/things"
#define HTTP_SYSTEM_SYNC_PATH HTTP_API_PREFIX "system/sync"
#define HTTP_SYSTEM_RESTART_PATH HTTP_API_PREFIX "system/restart"
#define HTTP_SYSTEM_SHUTDOWN_PATH HTTP_API_PREFIX "system/shutdown"
#define HTTP_VERSION_PATH HTTP_API_PREFIX "version"
#define HTTP_SYSTEM_INFO_PATH HTTP_API_PREFIX "system/info"
#define HTTP_MIDI_LAST_NOTE_PATH HTTP_API_PREFIX "midi/last_note"
#define HTTP_MIDI_MAPPINGS_PATH HTTP_API_PREFIX "midi/mappings"
#define HTTP_MIDI_MAPPING_SET_PATH HTTP_API_PREFIX "midi/mapping/set"
#define HTTP_MIDI_MAPPING_DELETE_PATH HTTP_API_PREFIX "midi/mapping/delete"
#define HTTP_MIDI_LIGHT_CONFIG_PATH HTTP_API_PREFIX "midi/light_config"
#define HTTP_MIDI_LIGHT_SOUNDS_PATH HTTP_API_PREFIX "midi/light_sounds"
#define HTTP_MIDI_LIGHT_SOUND_SET_PATH HTTP_API_PREFIX "midi/light_sound/set"
#define HTTP_MIDI_LIGHT_SOUND_DELETE_PATH HTTP_API_PREFIX "midi/light_sound/delete"
#define HTTP_MIDI_LIGHT_REFRESH_PATH HTTP_API_PREFIX "midi/light_refresh"
#define HTTP_AUDIO_DEVICES_PATH HTTP_API_PREFIX "audio/devices"
#define HTTP_AUDIO_RESCAN_PATH HTTP_API_PREFIX "audio/rescan"
#define HTTP_FS_ROOT ROOT_MOUNT_POINT "/"

namespace {

static int mapping_path_safe(const char *s);
static int body_get_value(const char *body,
						  size_t body_len,
						  const char *key,
						  char *out,
						  size_t out_sz);
static void copy_bound(char *dst, size_t dst_sz, const char *src);
static int mapping_normalize_sfx(const char *raw, char *out, size_t out_sz);

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
		static const char none[] = "{\"connected\":false,\"device\":\"\",\"last_note\":-1,\"last_velocity\":0,\"last_seq\":0}\n";
		return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", none, sizeof(none) - 1);
	}

	char out[512];
	int n = snprintf(out,
					 sizeof(out),
					 "{\"connected\":%s,\"device\":\"%s\",\"last_note\":%d,\"last_velocity\":%u,\"last_seq\":%u}\n",
					 midi->connected ? "true" : "false",
					 midi->devPath,
					 midi->lastNoteSeq ? (int)midi->lastNote : -1,
					 (unsigned)midi->lastVelocity,
					 (unsigned)midi->lastNoteSeq);
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
			first ? "" : ",", ID, NAME, (unsigned)(INPUTS), (unsigned)(OUTPUTS)); \
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

static int handleSystemInfo(HttpServer *server,
							 int cfd,
							 const char *method,
							 const char *path) {
	(void)method;
	(void)path;
	if (!server) return -1;

	const char *version_str = "unknown";
#if defined(AUDIOX_VERSION_MAJOR) && defined(AUDIOX_VERSION_MINOR) && defined(AUDIOX_VERSION_PATCH)
	char version_buf[32];
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
	struct sysinfo si;
	if (sysinfo(&si) == 0) {
		uptime_secs = si.uptime;
		unsigned long unit = si.mem_unit;
		mem_total_mb = (long)((unsigned long long)si.totalram * unit / (1024UL * 1024UL));
		mem_avail_mb = (long)((unsigned long long)(si.freeram + si.bufferram) * unit / (1024UL * 1024UL));
		load1 = (float)si.loads[0] / 65536.0f;
	}

	char out[384];
	int n = snprintf(out, sizeof(out),
					 "{\"version\":\"%s\",\"kernel\":\"%s\","
					 "\"uptime_secs\":%ld,"
					 "\"mem_total_mb\":%ld,"
					 "\"mem_avail_mb\":%ld,"
					 "\"load1\":%.2f}\n",
					 version_str, kernel_buf,
					 uptime_secs, mem_total_mb, mem_avail_mb, load1);
	if (n < 0 || n >= (int)sizeof(out)) n = 0;
	return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", out, (size_t)n);
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

#if defined(AUDIOX_VERSION_MAJOR) && defined(AUDIOX_VERSION_MINOR) && defined(AUDIOX_VERSION_PATCH)
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
						 "{\"channel\":%u,\"mapped_vel\":%u,\"held_vel\":%u,\"playing_vel\":%u}\n",
						 (unsigned)data.globalLight.channel,
						 (unsigned)data.globalLight.mappedVel,
						 (unsigned)data.globalLight.heldVel,
						 (unsigned)data.globalLight.playingVel);
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

	char ch_buf[16], mapped_buf[16], held_buf[16], playing_buf[16];
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
	if (body_get_value(body, body_len, "held_vel", held_buf, sizeof(held_buf))) {
		char *ep = NULL;
		long v = strtol(held_buf, &ep, 10);
		if (ep && *ep == '\0' && v >= 0 && v <= 127) {
			data.globalLight.heldVel = (uint8_t)v;
		}
	}
	if (body_get_value(body, body_len, "playing_vel", playing_buf, sizeof(playing_buf))) {
		char *ep = NULL;
		long v = strtol(playing_buf, &ep, 10);
		if (ep && *ep == '\0' && v >= 0 && v <= 127) {
			data.globalLight.playingVel = (uint8_t)v;
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
					 "%s{\"sfx\":\"%s\",\"mapped_vel\":%u,\"held_vel\":%u,\"playing_vel\":%u}",
					 first ? "" : ",",
					 data.soundLights[i].sfxPath,
					 (unsigned)data.soundLights[i].mappedVel,
					 (unsigned)data.soundLights[i].heldVel,
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
	char mapped_buf[16], held_buf[16], playing_buf[16];
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
	if (body_get_value(body, body_len, "held_vel", held_buf, sizeof(held_buf))) {
		char *ep = NULL;
		long v = strtol(held_buf, &ep, 10);
		if (ep && *ep == '\0' && v >= 0 && v <= 127) {
			sl.heldVel = (uint8_t)v;
			sl.hasHeld = 1;
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
					 size_t body_len) {
	if (!server || cfd < 0 || !method || !path) {
		return -1;
	}

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

	if (strcmp(path, HTTP_SOUNDBOARD_MODES_PATH) == 0) {
		return handleSoundboardModes(server, cfd, method, path);
	}

	if (strcmp(path, HTTP_SOUNDBOARD_MODE_SET_PATH) == 0) {
		return handleSoundboardModeSet(server, cfd, method, path, body, body_len);
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

	if (strcmp(path, HTTP_AUDIO_DEVICES_PATH) == 0) {
		return handleAudioDevices(server, cfd, method, path);
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