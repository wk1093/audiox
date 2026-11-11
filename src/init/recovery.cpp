#include "init.hpp"
#include "boot_recovery.hpp"
#include "system.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <time.h>

namespace {

bool g_trialPendingAtStartup = false;
bool g_trialArmedThisBoot = false;
bool g_trialResolved = false;
int64_t g_readySince = 0;
bool g_startupStable = false;
bool g_readyTimerStarted = false;
BootRecoveryHealthGate g_trialHealthGate;

int64_t monotonicMilliseconds() {
    timespec now = {};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        printf("[RECOVERY] [WARN] Cannot capture startup clock: %s\n", strerror(errno));
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

} // namespace

int prepareBootUpdateRecovery() {
    if (mkdir("/boot", 0755) < 0 && errno != EEXIST) {
        return RET_ERR;
    }
    if (mount(BOOT_DEVICE_PATH, "/boot", "vfat", MS_NOATIME, NULL) < 0 && errno != EBUSY) {
        return RET_ERR;
    }

    char version[128];
    if (brReadSmallFile(AUDIOX_VERSION_FILE, version, sizeof(version)) <= 0) {
#if defined(AUDIOX_VERSION_STRING)
        snprintf(version, sizeof(version), "%s\n", AUDIOX_VERSION_STRING);
#else
        snprintf(version, sizeof(version), "unknown\n");
#endif
    }

    const bool keptBackup = bootRecoveryHasBackup(ROOT_MOUNT_POINT);
    if (bootRecoveryPrepareUpdate(ROOT_MOUNT_POINT, "/boot", version) < 0) {
        printf("[RECOVERY] [ERR] failed to back up boot files before update: %s\n", strerror(errno));
        return RET_ERR;
    }
    g_trialArmedThisBoot = true;
    printf("[RECOVERY] boot update armed (%s backup)\n", keptBackup ? "kept existing" : "created new");
    return RET_OK;
}

void initBootRecovery() {
    g_readySince = 0;
    g_readyTimerStarted = false;
    g_startupStable = false;
    g_trialHealthGate.reset();
    const char *trialEnv = getenv(BOOT_RECOVERY_TRIAL_ENV);
    g_trialPendingAtStartup = trialEnv && strcmp(trialEnv, "1") == 0 && bootRecoveryTrialPending(ROOT_MOUNT_POINT);
    if (g_trialPendingAtStartup) {
        printf("[RECOVERY] trial boot of a new image; confirming after %d healthy seconds\n",
               BOOT_RECOVERY_CONFIRM_SECONDS);
    }
}

void pollBootRecovery(uint64_t audioProgressAtMs) {
    const int64_t now = monotonicMilliseconds();
    if (now < 0) return;
    if (!g_readyTimerStarted) {
        g_readySince = now;
        g_readyTimerStarted = true;
    }
    g_startupStable = now - g_readySince >= (int64_t)BOOT_RECOVERY_CONFIRM_SECONDS * 1000;
    if (!g_trialPendingAtStartup || g_trialResolved) {
        return;
    }
    // A newer update armed during this boot owns the trial state; leave it for the next boot.
    if (g_trialArmedThisBoot) {
        g_trialResolved = true;
        return;
    }
    const uint64_t nowMs = (uint64_t)now;
    if (!g_trialHealthGate.update(nowMs, audioProgressAtMs)) {
        return;
    }

    if (bootRecoveryConfirm(ROOT_MOUNT_POINT) < 0) {
        printf("[RECOVERY] [WARN] trial passed health checks, but confirmation was not durable: %s\n",
               strerror(errno));
        g_trialResolved = true;
        return;
    }
    g_trialResolved = true;
    printf("[RECOVERY] new image confirmed healthy; removed backup boot set\n");
}

bool bootStartupStable() {
    return g_startupStable;
}
