#include "http/context.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HTTP_BODY_MAX 49152
#define HTTP_API_PREFIX "/api/"
#define HTTP_ROOTFS_PREFIX HTTP_API_PREFIX "rootfs/"
#define HTTP_SOUNDBOARD_TRIGGER_PREFIX HTTP_API_PREFIX "soundboard/trigger/"
#define HTTP_CONFIG_RELOAD_PATH HTTP_API_PREFIX "config/reload"
#define HTTP_ROUTING_RELOAD_PATH HTTP_API_PREFIX "routing/reload"
#define HTTP_ROUTING_THINGS_PATH HTTP_API_PREFIX "routing/things"
#define HTTP_SYSTEM_SYNC_PATH HTTP_API_PREFIX "system/sync"
#define HTTP_SYSTEM_RESTART_PATH HTTP_API_PREFIX "system/restart"
#define HTTP_SYSTEM_SHUTDOWN_PATH HTTP_API_PREFIX "system/shutdown"
#define HTTP_VERSION_PATH HTTP_API_PREFIX "version"
#define HTTP_FS_ROOT ROOT_MOUNT_POINT "/"

namespace {

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

static int handleSoundboardTrigger(HttpServer *server,
								   int cfd,
								   const char *method,
								   const char *path) {
	(void)server;
	(void)cfd;
	(void)method;
	(void)path;
	return RET_WARN;
}

static int handleConfigReload(HttpServer *server,
							  int cfd,
							  const char *method,
							  const char *path) {
	(void)server;
	(void)cfd;
	(void)method;
	(void)path;
	return RET_WARN;
}

static int handleRoutingReload(HttpServer *server,
							   int cfd,
							   const char *method,
							   const char *path) {
	(void)server;
	(void)cfd;
	(void)method;
	(void)path;
	return RET_WARN;
}

static int handleRoutingThings(HttpServer *server,
							   int cfd,
							   const char *method,
							   const char *path) {
	(void)server;
	(void)cfd;
	(void)method;
	(void)path;
	return RET_WARN;
}

static int handleSystemSync(HttpServer *server,
							int cfd,
							const char *method,
							const char *path) {
	(void)server;
	(void)cfd;
	(void)method;
	(void)path;
	return RET_WARN;
}

static int handleSystemRestart(HttpServer *server,
							   int cfd,
							   const char *method,
							   const char *path) {
	(void)server;
	(void)cfd;
	(void)method;
	(void)path;
	return RET_WARN;
}

static int handleSystemShutdown(HttpServer *server,
								int cfd,
								const char *method,
								const char *path) {
	(void)server;
	(void)cfd;
	(void)method;
	(void)path;
	return RET_WARN;
}

static int handleVersion(HttpServer *server,
						 int cfd,
						 const char *method,
						 const char *path) {
	(void)server;
	(void)cfd;
	(void)method;
	(void)path;
	return RET_WARN;
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
		int rc = handleSoundboardTrigger(server, cfd, method, path);
		if (rc == RET_WARN) {
			return sendNotImplemented(server, cfd, "soundboard trigger");
		}
		return rc;
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

	if (pathIsRootfs(path)) {
		if (strcmp(method, "GET") == 0) {
			return handleApiGetRootfs(server, cfd, path);
		}
		if (strcmp(method, "PUT") == 0) {
			return handleApiPutRootfs(server, cfd, path, body, body_len);
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