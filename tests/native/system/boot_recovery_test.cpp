#include "boot_recovery.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>

#include <string>
#include <vector>

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

void installBootSet(const char *boot, const char *text, const char *kernel) {
    for (size_t i = 0; bootRecoveryFileName(i); ++i) {
        if (strcmp(bootRecoveryFileName(i), "config.txt") != 0) {
            writeFile(boot, bootRecoveryFileName(i), text);
        }
    }
    writeFile(boot, kernel, text);
    std::string config = std::string("# ") + text + "\nkernel=" + kernel + "\narm_64bit=1\n";
    writeFile(boot, "config.txt", config.c_str());
}

void installNewBootSet(const char *boot) {
    installBootSet(boot, "new", "kernel8.img");
}

void testMetadata() {
    AudioxUpdateMetadata meta;
    const char good[] = "board=pi5\nkernel=kernel_2712.img\nversion=1.9.0-beta.4\nkernel_version=6.18.58-v8-16k+\nfuture=x\n";
    check(audioxUpdateParseMetadata(good, sizeof(good) - 1, &meta) == NULL, "metadata parses");
    check(strcmp(meta.board, "pi5") == 0 && strcmp(meta.kernel, "kernel_2712.img") == 0, "metadata board/kernel");
    check(strcmp(meta.kernel_version, "6.18.58-v8-16k+") == 0, "metadata kernel version");
    const char traversal[] = "board=pi4\nkernel=../kernel8.img\n";
    check(audioxUpdateParseMetadata(traversal, sizeof(traversal) - 1, &meta) != NULL, "metadata rejects paths");
    const char bad_board[] = "board=pi3\nkernel=kernel8.img\n";
    check(audioxUpdateParseMetadata(bad_board, sizeof(bad_board) - 1, &meta) != NULL, "metadata rejects unknown board");
    const char no_kernel[] = "board=pi4\n";
    check(audioxUpdateParseMetadata(no_kernel, sizeof(no_kernel) - 1, &meta) != NULL, "metadata requires kernel");
    check(!audioxUpdateValidKernelName("config.txt") && !audioxUpdateValidKernelName("kernel.img.bak"),
          "kernel name pattern");

    char name[AUDIOX_UPDATE_VALUE_MAX];
    const char config[] = "boot_delay=0\n  kernel=kernel_2712.img # comment\nkernel=kernel8.img\n";
    audioxConfigKernelName(config, sizeof(config) - 1, name, sizeof(name));
    check(strcmp(name, "kernel_2712.img") == 0, "config kernel line found");
    const char no_line[] = "boot_delay=0\n";
    audioxConfigKernelName(no_line, sizeof(no_line) - 1, name, sizeof(name));
    check(strcmp(name, "kernel8.img") == 0, "config kernel defaults to kernel8.img");

    const char cmdline[] = "console=tty1 quiet rdinit=/init panic=10\n";
    check(audioxValidateCmdline(cmdline, sizeof(cmdline) - 1) == NULL, "cmdline valid");
    const char cmdline_crlf[] = "rdinit=/init\r\n";
    check(audioxValidateCmdline(cmdline_crlf, sizeof(cmdline_crlf) - 1) == NULL, "cmdline crlf valid");
    const char no_rdinit[] = "console=tty1 quiet\n";
    check(audioxValidateCmdline(no_rdinit, sizeof(no_rdinit) - 1) != NULL, "cmdline requires rdinit");
    const char wrong_rdinit[] = "rdinit=/init2 quiet\n";
    check(audioxValidateCmdline(wrong_rdinit, sizeof(wrong_rdinit) - 1) != NULL, "cmdline rdinit must be exact");
    const char multi[] = "rdinit=/init\nquiet\n";
    check(audioxValidateCmdline(multi, sizeof(multi) - 1) != NULL, "cmdline rejects multiple lines");
    check(audioxValidateCmdline("\n", 1) != NULL, "cmdline rejects empty");
    const char ctrl[] = "rdinit=/init \x01";
    check(audioxValidateCmdline(ctrl, sizeof(ctrl) - 1) != NULL, "cmdline rejects control chars");
}

struct TestEntry {
    uint32_t type;
    std::string payload;
};

std::string buildArchive(uint32_t version, const std::vector<TestEntry> &items) {
    std::string out(AUDIOX_UPDATE_MAGIC, AUDIOX_UPDATE_MAGIC_SIZE);
    uint8_t buf[AUDIOX_UPDATE_ENTRY_HEADER_SIZE];
    audioxUpdatePutU32(buf, version);
    audioxUpdatePutU32(buf + 4, (uint32_t)items.size());
    out.append((const char *)buf, 8);
    for (const TestEntry &item : items) {
        audioxUpdatePutU32(buf, item.type);
        audioxUpdatePutU32(buf + 4, audioxUpdateCrc32(0, item.payload.data(), item.payload.size()));
        audioxUpdatePutU64(buf + 8, item.payload.size());
        out.append((const char *)buf, sizeof(buf));
        out += item.payload;
    }
    return out;
}

const char *parseArchive(const char *dir, const std::string &archive, AudioxUpdateEntry entries[AUDIOX_UPDATE_KNOWN_TYPES]) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/test.axu", dir);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return "open failed";
    }
    fwrite(archive.data(), 1, archive.size(), fp);
    fclose(fp);
    int fd = open(path, O_RDONLY);
    const char *error = audioxUpdateParseArchive(fd, archive.size(), entries);
    close(fd);
    unlink(path);
    return error;
}

std::vector<TestEntry> baseEntries() {
    return {
        {AUDIOX_UPDATE_METADATA, "board=pi4\nkernel=kernel8.img\n"},
        {AUDIOX_UPDATE_CONFIG, "kernel=kernel8.img\n"},
        {AUDIOX_UPDATE_BOOTLOADER, "loader"},
        {AUDIOX_UPDATE_KERNEL, "kernel"},
        {AUDIOX_UPDATE_PROGRAM, "program"},
    };
}

void testArchiveParse(const char *dir) {
    AudioxUpdateEntry entries[AUDIOX_UPDATE_KNOWN_TYPES];
    std::vector<TestEntry> items = baseEntries();
    check(parseArchive(dir, buildArchive(2, items), entries) == NULL, "v2 archive parses");
    check(entries[AUDIOX_UPDATE_CMDLINE - 1].size == 0, "cmdline absent");
    check(entries[AUDIOX_UPDATE_KERNEL - 1].size == 6, "kernel entry located");

    items.push_back({AUDIOX_UPDATE_CMDLINE, "rdinit=/init\n"});
    items.push_back({0x40, "future optional entry"});
    items.push_back({0x41, ""});
    check(parseArchive(dir, buildArchive(2, items), entries) == NULL, "unknown optional entries skipped");
    check(entries[AUDIOX_UPDATE_CMDLINE - 1].size == 13, "cmdline located after skipping");

    std::vector<TestEntry> required = baseEntries();
    required.push_back({AUDIOX_UPDATE_TYPE_REQUIRED | 0x40, "must understand"});
    check(parseArchive(dir, buildArchive(2, required), entries) != NULL, "unknown required entry rejected");

    std::vector<TestEntry> missing = baseEntries();
    missing.pop_back();
    check(parseArchive(dir, buildArchive(2, missing), entries) != NULL, "missing required entry rejected");

    std::vector<TestEntry> duplicate = baseEntries();
    duplicate.push_back({AUDIOX_UPDATE_KERNEL, "kernel2"});
    check(parseArchive(dir, buildArchive(2, duplicate), entries) != NULL, "duplicate entry rejected");

    check(parseArchive(dir, buildArchive(3, baseEntries()), entries) != NULL, "newer header version rejected");

    std::vector<TestEntry> v1 = baseEntries();
    v1.erase(v1.begin());
    check(parseArchive(dir, buildArchive(1, v1), entries) == NULL, "v1 archive parses");
    v1.push_back({0x40, "x"});
    check(parseArchive(dir, buildArchive(1, v1), entries) != NULL, "v1 rejects extra entries");

    std::string corrupt = buildArchive(2, baseEntries());
    corrupt[corrupt.size() - 1] ^= 1;
    check(parseArchive(dir, corrupt, entries) != NULL, "checksum mismatch rejected");
    check(parseArchive(dir, buildArchive(2, baseEntries()) + "x", entries) != NULL, "trailing data rejected");
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
    testMetadata();
    testArchiveParse(base);
    installBootSet(boot, "old", "kernel8.img");
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
        if (strcmp(bootRecoveryFileName(i), "config.txt") != 0) {
            check(fileEquals(boot, bootRecoveryFileName(i), "old"), "boot file restored");
        }
    }
    check(fileEquals(boot, "kernel8.img", "old"), "kernel restored");
    check(fileEquals(boot, "cmdline.txt", "old"), "cmdline restored");
    check(fileEquals(boot, "config.txt", "# old\nkernel=kernel8.img\narm_64bit=1\n"), "config restored");
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
    check(strstr(report, ",config.txt\n") != NULL && strstr(report, "kernel8.img") != NULL,
          "report lists restored files with config.txt last");
    check(bootRecoveryBeginBoot(root, "1.0.0", &attempts) == BOOT_TRIAL_NONE, "restored image boots normally");

    // Switching kernel file names (pi4 kernel8.img -> kernel_2712.img) must restore the old kernel and config.
    check(bootRecoveryPrepareUpdate(root, boot, "1.0.0\n") == 0, "prepare kernel rename update");
    check(exists(state, BOOT_RECOVERY_BACKUP_NAME "/kernel8.img"), "backup holds the kernel config.txt boots");
    installBootSet(boot, "renamed", "kernel_2712.img");
    check(bootRecoveryBeginBoot(root, "5.0.0", &attempts) == BOOT_TRIAL_ATTEMPT, "renamed trial attempt");
    writeFile(state, BOOT_RECOVERY_TRIAL_NAME, "attempts=99\n");
    check(bootRecoveryBeginBoot(root, "5.0.0", &attempts) == BOOT_TRIAL_REVERT, "renamed trial reverts");
    check(bootRecoveryRevert(root, boot, "5.0.0", attempts) == 0, "renamed revert");
    check(fileEquals(boot, "config.txt", "# old\nkernel=kernel8.img\narm_64bit=1\n"), "renamed config restored");
    check(fileEquals(boot, "kernel8.img", "old"), "original kernel still present after revert");

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
