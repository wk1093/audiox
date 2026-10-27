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
time_t g_readySince = 0;

time_t monotonicSeconds() {
    timespec now = {};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return now.tv_sec;
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
    const char *trialEnv = getenv(BOOT_RECOVERY_TRIAL_ENV);
    g_trialPendingAtStartup = trialEnv && strcmp(trialEnv, "1") == 0 && bootRecoveryTrialPending(ROOT_MOUNT_POINT);
    if (g_trialPendingAtStartup) {
        printf("[RECOVERY] trial boot of a new image; confirming after %d healthy seconds\n",
               BOOT_RECOVERY_CONFIRM_SECONDS);
    }
}

void pollBootRecovery() {
    if (!g_trialPendingAtStartup || g_trialResolved) {
        return;
    }
    // A newer update armed during this boot owns the trial state; leave it for the next boot.
    if (g_trialArmedThisBoot) {
        g_trialResolved = true;
        return;
    }
    const time_t now = monotonicSeconds();
    if (g_readySince == 0) {
        g_readySince = now;
        return;
    }
    if (now - g_readySince < BOOT_RECOVERY_CONFIRM_SECONDS) {
        return;
    }
    bootRecoveryConfirm(ROOT_MOUNT_POINT);
    g_trialResolved = true;
    printf("[RECOVERY] new image confirmed healthy; removed backup boot set\n");
}
