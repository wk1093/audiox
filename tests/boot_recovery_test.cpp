#include "boot_recovery.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace {

int failures = 0;

void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void writeFile(const char *dir, const char *name, const char *text) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *fp = fopen(path, "w");
    if (fp) {
        fputs(text, fp);
        fclose(fp);
    }
}

bool fileEquals(const char *dir, const char *name, const char *expected) {
    char path[1100];
    char text[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return brReadSmallFile(path, text, sizeof(text)) >= 0 && strcmp(text, expected) == 0;
}

bool exists(const char *dir, const char *name) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return access(path, F_OK) == 0;
}

void installNewBootSet(const char *boot) {
    for (size_t i = 0; bootRecoveryFileName(i); ++i) {
        writeFile(boot, bootRecoveryFileName(i), "new");
    }
}

} // namespace

int main() {
    char base[] = "/tmp/audiox-boot-recovery-XXXXXX";
    if (!mkdtemp(base)) {
        perror("mkdtemp");
        return 1;
    }
    char root[512];
    char boot[512];
    char state[600];
    snprintf(root, sizeof(root), "%s/root", base);
    snprintf(boot, sizeof(boot), "%s/boot", base);
    snprintf(state, sizeof(state), "%s/system", root);
    mkdir(root, 0755);
    mkdir(boot, 0755);
    for (size_t i = 0; bootRecoveryFileName(i); ++i) {
        writeFile(boot, bootRecoveryFileName(i), "old");
    }
    writeFile(root, "stdout.log", "runtime crashed here\n");

    unsigned attempts = 0;
    check(bootRecoveryBeginBoot(root, "old", &attempts) == BOOT_TRIAL_NONE, "no trial before an update");

    // Update, then fail every trial boot until the bootloader reverts.
    check(bootRecoveryPrepareUpdate(root, boot, "1.0.0\n") == 0, "prepare update");
    check(bootRecoveryHasBackup(root), "backup created");
    installNewBootSet(boot);
    // A second request in the same update must keep the original backup.
    check(bootRecoveryPrepareUpdate(root, boot, "2.0.0\n") == 0, "prepare second request");
    check(fileEquals(state, BOOT_RECOVERY_BACKUP_NAME "/kernel8.img", "old"), "existing backup preserved");
    check(fileEquals(state, BOOT_RECOVERY_BACKUP_NAME "/" BOOT_RECOVERY_VERSION_NAME, "1.0.0\n"), "backup version preserved");

    for (unsigned expected = 1; expected <= BOOT_RECOVERY_MAX_ATTEMPTS; ++expected) {
        check(bootRecoveryBeginBoot(root, "2.0.0", &attempts) == BOOT_TRIAL_ATTEMPT, "trial attempt");
        check(attempts == expected, "attempt counter increments");
        brAppendText((std::string(state) + "/" BOOT_RECOVERY_EVENTS_NAME).c_str(), "[WATCHDOG] trial boot failed: test\n");
    }
    check(bootRecoveryBeginBoot(root, "2.0.0", &attempts) == BOOT_TRIAL_REVERT, "revert after max attempts");
    check(bootRecoveryRevert(root, boot, "2.0.0", attempts) == 0, "revert restores backup");
    for (size_t i = 0; bootRecoveryFileName(i); ++i) {
        check(fileEquals(boot, bootRecoveryFileName(i), "old"), "boot file restored");
    }
    check(!bootRecoveryTrialPending(root), "trial cleared after revert");
    check(!bootRecoveryHasBackup(root), "backup removed after revert");
    char report[8192];
    char reportPath[1100];
    snprintf(reportPath, sizeof(reportPath), "%s/%s", state, BOOT_RECOVERY_REPORT_NAME);
    check(brReadSmallFile(reportPath, report, sizeof(report)) > 0, "report written");
    check(strstr(report, "failed_version=2.0.0\n") != NULL, "report has failed version");
    check(strstr(report, "restored_version=1.0.0\n") != NULL, "report has restored version");
    check(strstr(report, "runtime crashed here") != NULL, "report has runtime log");
    check(strstr(report, "trial boot failed: test") != NULL, "report has trial events");
    check(bootRecoveryBeginBoot(root, "1.0.0", &attempts) == BOOT_TRIAL_NONE, "restored image boots normally");

    // A healthy update is confirmed and its safety copy dropped.
    check(bootRecoveryPrepareUpdate(root, boot, "1.0.0\n") == 0, "prepare healthy update");
    installNewBootSet(boot);
    check(bootRecoveryBeginBoot(root, "3.0.0", &attempts) == BOOT_TRIAL_ATTEMPT, "healthy trial attempt");
    bootRecoveryConfirm(root);
    check(!bootRecoveryTrialPending(root), "trial cleared after confirm");
    check(!bootRecoveryHasBackup(root), "backup removed after confirm");
    check(!exists(state, BOOT_RECOVERY_EVENTS_NAME), "events removed after confirm");
    check(bootRecoveryBeginBoot(root, "3.0.0", &attempts) == BOOT_TRIAL_NONE, "confirmed image boots normally");
    check(fileEquals(boot, "kernel8.img", "new"), "confirmed image kept");

    // A corrupt counter must not loop forever.
    check(bootRecoveryPrepareUpdate(root, boot, "3.0.0\n") == 0, "prepare corrupt test");
    writeFile(state, BOOT_RECOVERY_TRIAL_NAME, "garbage");
    check(bootRecoveryBeginBoot(root, "4.0.0", &attempts) == BOOT_TRIAL_REVERT, "corrupt counter reverts");

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    if (system(cmd) != 0) {
        fprintf(stderr, "warning: failed to remove %s\n", base);
    }

    if (failures != 0) {
        fprintf(stderr, "%d boot recovery check(s) failed\n", failures);
        return 1;
    }
    printf("boot recovery tests passed\n");
    return 0;
}
