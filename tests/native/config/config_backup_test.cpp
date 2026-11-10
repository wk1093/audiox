#include "config/backup.hpp"
#include "update_archive.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;
static bool failSync = false;

extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if (failSync) {
        failSync = false;
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

namespace {

const char *const names[] = {
    "config.txt", "config.staging.txt", "routing.txt", "midi_map.txt",
    "fx_params.txt", "volumes.txt", "bluetooth", "sfx", "scenes",
    "scripts.txt", "scripts",
};

fs::path live(const fs::path &root, const std::string &name) {
    return (name == "sfx" ? root : root / "config") / name;
}

struct Workspace {
    fs::path path;
    Workspace() {
        char pattern[] = "./audiox-backup-test-XXXXXX";
        assert(mkdtemp(pattern));
        path = pattern;
    }
    ~Workspace() { fs::remove_all(path); }
};

void put(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), (std::streamsize)text.size());
    out.close();
    assert(out);
}

std::string get(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    assert(in);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::map<std::string, std::string> snapshot(const fs::path &root) {
    std::map<std::string, std::string> result;
    for (const char *name : names) {
        fs::path path = live(root, name);
        if (!fs::exists(path)) continue;
        if (fs::is_regular_file(path)) result[name] = get(path);
        else {
            result[std::string(name) + "/"] = "";
            for (const auto &entry : fs::recursive_directory_iterator(path)) {
                std::string relative = std::string(name) + "/" + fs::relative(entry.path(), path).string();
                if (entry.is_directory()) result[relative + "/"] = "";
                else result[relative] = get(entry.path());
            }
        }
    }
    return result;
}

void fixture(const fs::path &root) {
    put(root / "config/.layout-version", "1\n");
    put(root / "config/config.txt", "usb_sample_rate=44100\nsoundboard_mode=hold\n");
    put(root / "config/config.staging.txt", "usb_sample_rate=48000\n");
    put(root / "config/routing.txt", "routing_helper=true\nusb_0:0>fx_7:0\nfx_7:0>alsa_0:0\n");
    put(root / "config/midi_map.txt", "note_map=36,kick.wav\nfx_state=fx_7,cut,1\n");
    put(root / "config/fx_params.txt", "fx_param=fx_7,channels,6\n");
    put(root / "config/volumes.txt", "usb_0=0.75\n");
    put(root / "config/bluetooth/paired.txt", "00:11:22:33:44:55 0123456789abcdef0123456789abcdef 4 Headphones\n");
    put(root / "config/scripts.txt", "# audiox scripts v1\nmacro=test\n");
    put(root / "config/scripts/nested/data.bin", "script data");
    fs::create_directories(root / "config/scripts/empty");
    put(root / "config/scenes/test.scene", "audiox-scenes 1\n");
    std::string sound(200000, '\0');
    for (size_t i = 0; i < sound.size(); ++i) sound[i] = (char)i;
    put(root / "sfx/kick.wav", sound);
    put(root / "sfx/nested/sound with spaces.wav", "more sound");
    fs::create_directories(root / "sfx/empty");
    put(root / "staging/program.cpio.gz", "not a setting");
    put(root / "system/boot-trial", "not a setting");
    put(root / "stdout.log", "not a setting");
}

int archive(const fs::path &root, const fs::path &path) {
    int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
    assert(fd >= 0);
    char error[256];
    int rc = audiox::backup::create(root.c_str(), fd, error, sizeof(error));
    if (rc < 0) fprintf(stderr, "%s\n", error);
    assert(rc == 0);
    return fd;
}

void stage(const fs::path &root, int fd) {
    char error[256];
    off_t size = lseek(fd, 0, SEEK_END);
    int rc = audiox::backup::stage(root.c_str(), fd, (uint64_t)size, error, sizeof(error));
    if (rc < 0) fprintf(stderr, "%s\n", error);
    assert(rc == 0);
}

void apply(const fs::path &root) {
    char error[256];
    int rc = audiox::backup::applyPending(root.c_str(), error, sizeof(error));
    if (rc < 0) fprintf(stderr, "%s\n", error);
    assert(rc == 0);
    assert(!fs::exists(root / ".config-restore"));
    assert(!fs::exists(root / ".config-restore.done"));
}

struct Entry {
    std::string path;
    uint32_t type;
    std::string data;
};

std::string encode(const std::vector<Entry> &entries) {
    std::string result = "AXBACKUP";
    unsigned char header[16] = {};
    audioxUpdatePutU32(header, 1);
    audioxUpdatePutU32(header + 4, (uint32_t)entries.size());
    result.append((const char *)header, 8);
    for (const Entry &entry : entries) {
        audioxUpdatePutU32(header, (uint32_t)entry.path.size());
        audioxUpdatePutU32(header + 4, entry.type);
        audioxUpdatePutU64(header + 8, entry.data.size());
        result.append((const char *)header, 16);
        result += entry.path;
        result += entry.data;
    }
    audioxUpdatePutU32(header, audioxUpdateCrc32(0, result.data(), result.size()));
    result.append((const char *)header, 4);
    return result;
}

void reject(const fs::path &root, const fs::path &path, const std::string &data) {
    put(path, data);
    int fd = open(path.c_str(), O_RDONLY);
    assert(fd >= 0);
    auto before = snapshot(root);
    char error[256] = {};
    assert(audiox::backup::stage(root.c_str(), fd, data.size(), error, sizeof(error)) < 0);
    assert(error[0]);
    close(fd);
    assert(snapshot(root) == before);
    assert(!fs::exists(root / ".config-restore"));
    for (const auto &entry : fs::directory_iterator(root)) {
        assert(entry.path().filename().string().find(".config-backup-") != 0);
    }
}

void testRoundTrip() {
    Workspace work;
    fs::path source = work.path / "source", target = work.path / "target";
    fixture(source);
    put(source / "config/usage-telemetry-device-local", "private source state");
    auto expected = snapshot(source);
    int fd = archive(source, work.path / "backup.axb");
    assert((uint64_t)lseek(fd, 0, SEEK_END) > 200000);
    assert(get(work.path / "backup.axb").find("not a setting") == std::string::npos);
    assert(get(work.path / "backup.axb").find("private source state") == std::string::npos);
    fixture(target);
    put(target / "config/config.txt", "new image defaults");
    put(target / "config.txt", "legacy must remain");
    put(target / "scripts/legacy", "legacy script data");
    put(target / "config/usage-telemetry-device-local", "private device state");
    put(target / "sfx/extra.wav", "remove me");
    put(target / "system/boot-trial", "leave recovery state alone");
    auto before = snapshot(target);
    stage(target, fd);
    assert(snapshot(target) == before);
    char error[256];
    assert(audiox::backup::stage(target.c_str(), fd, (uint64_t)lseek(fd, 0, SEEK_END), error, sizeof(error)) < 0);
    failSync = true;
    assert(audiox::backup::applyPending(target.c_str(), error, sizeof(error)) < 0);
    assert(fs::exists(target / ".config-restore"));
    apply(target);
    assert(snapshot(target) == expected);
    assert(get(target / "config.txt") == "legacy must remain");
    assert(get(target / "scripts/legacy") == "legacy script data");
    assert(get(target / "config/.layout-version") == "1\n");
    assert(get(target / "config/usage-telemetry-device-local") == "private device state");
    assert(get(target / "system/boot-trial") == "leave recovery state alone");
    assert(get(target / "staging/program.cpio.gz") == "not a setting");
    put(target / ".config-backup-ABC123/new/sfx/unfinished.wav", "interrupted staging");
    put(target / ".config-backup-user-notes/keep.txt", "not a staging directory");
    put(target / ".config-restore.done/old/sfx/old.wav", "interrupted cleanup");
    apply(target);
    assert(!fs::exists(target / ".config-backup-ABC123"));
    assert(get(target / ".config-backup-user-notes/keep.txt") == "not a staging directory");
    assert(snapshot(target) == expected);

    // Resume after any prefix of the root entries, and after saving an old entry
    // but before moving its replacement. No live subsystem has started yet.
    for (size_t stop = 0; stop <= std::size(names); ++stop) {
        for (const char *name : names) fs::remove_all(live(target, name));
        fixture(target);
        put(target / "config/volumes.txt", "new volumes");
        stage(target, fd);
        fs::path pending = target / ".config-restore";
        for (size_t i = 0; i <= stop && i < std::size(names); ++i) {
            fs::path dst = live(target, names[i]);
            fs::rename(dst, pending / (i == 7 ? "old" : "old-config") / names[i]);
            if (i < stop) fs::rename(pending / "new" / names[i], dst);
        }
        apply(target);
        assert(snapshot(target) == expected);
    }
    close(fd);
    puts("PASS: complete settings/audio round trip, staged isolation, and interrupted restore recovery");
}

void testAbsentAndSymlinks() {
    Workspace work;
    fs::path source = work.path / "source", target = work.path / "target";
    put(source / "config/config.txt", "minimal");
    int fd = archive(source, work.path / "minimal.axb");
    fixture(target);
    fs::path outside = work.path / "outside";
    put(outside / "must-stay.wav", "do not overwrite");
    fs::remove_all(target / "sfx");
    fs::create_directory_symlink(outside, target / "sfx");
    stage(target, fd);
    fs::rename(target / "config/scripts.txt", target / ".config-restore/old-config/scripts.txt");
    fs::rename(target / "config/scripts", target / ".config-restore/old-config/scripts");
    apply(target);
    assert(get(target / "config/config.txt") == "minimal");
    for (size_t i = 1; i < 6; ++i) assert(!fs::exists(live(target, names[i])));
    assert(!fs::exists(target / "config/scripts.txt"));
    assert(fs::is_empty(target / "config/scripts"));
    assert(fs::is_empty(target / "sfx") && fs::is_empty(target / "config/bluetooth"));
    assert(get(outside / "must-stay.wav") == "do not overwrite");
    close(fd);
    fs::create_directories(source / "sfx");
    fs::create_symlink(outside / "must-stay.wav", source / "sfx/link.wav");
    fd = open((work.path / "bad.axb").c_str(), O_CREAT | O_RDWR, 0600);
    assert(fd >= 0);
    char error[256];
    assert(audiox::backup::create(source.c_str(), fd, error, sizeof(error)) < 0);
    assert(strstr(error, "links or special files"));
    fs::remove(source / "sfx/link.wav");
    fs::remove(source / "sfx");
    fs::create_directory_symlink(outside, source / "sfx");
    assert(audiox::backup::create(source.c_str(), fd, error, sizeof(error)) < 0);
    fs::remove(source / "sfx");
    fs::create_directories(source / "config/bluetooth/paired.txt");
    assert(audiox::backup::create(source.c_str(), fd, error, sizeof(error)) < 0);
    assert(strstr(error, "Unexpected file type"));
    close(fd);
    puts("PASS: absent settings remove defaults and links cannot escape the backup or restore");
}

void testLegacyRestore() {
    Workspace work;
    fs::path target = work.path / "target", archivePath = work.path / "legacy.axb";
    const std::vector<Entry> entries = {
        {"config.txt", 1, "legacy backup config\n"},
        {"config.staging.txt", 1, "staging\n"},
        {"routing.txt", 1, "routing\n"},
        {"midi_map.txt", 1, "note_map=36,kick.wav\nkey_macro=legacy\n"},
        {"fx_params.txt", 1, "fx\n"}, {"volumes.txt", 1, "volumes\n"},
        {"bluetooth", 2, ""}, {"bluetooth/paired.txt", 1, "paired\n"},
        {"sfx", 2, ""}, {"sfx/legacy.wav", 1, "sound"},
        {"scenes", 2, ""}, {"scenes/test.scene", 1, "audiox-scenes 1\n"},
    };
    put(archivePath, encode(entries));
    int fd = open(archivePath.c_str(), O_RDONLY);
    assert(fd >= 0);
    fixture(target);
    put(target / "midi_map.txt", "legacy root unchanged");
    stage(target, fd);
    apply(target);
    assert(get(target / "config/midi_map.txt") == "note_map=36,kick.wav\nkey_macro=legacy\n");
    assert(!fs::exists(target / "config/scripts.txt"));
    assert(!fs::exists(target / "config/scripts"));
    assert(get(target / "midi_map.txt") == "legacy root unchanged");
    for (size_t stop = 0; stop <= 9; ++stop) {
        fs::remove_all(target);
        fixture(target);
        for (size_t i = 0; i < 9; ++i) {
            if (i != 7) fs::copy(live(target, names[i]), target / names[i], fs::copy_options::recursive);
        }
        stage(target, fd);
        fs::path pending = target / ".config-restore";
        std::string plan = get(pending / "plan");
        assert(plan.size() == 8);
        put(pending / "plan", plan.substr(0, 4));
        fs::remove_all(pending / "old-config");
        for (size_t i = 0; i <= stop && i < 9; ++i) {
            fs::rename(target / names[i], pending / "old" / names[i]);
            if (i < stop) fs::rename(pending / "new" / names[i], target / names[i]);
        }
        if (stop == 2) put(pending / "recovering/partial", "interrupted recovery copy");
        std::map<std::string, std::string> legacy;
        for (size_t i = 0; i < 6; ++i)
            if (fs::exists(target / names[i])) legacy[names[i]] = get(target / names[i]);
        apply(target);
        for (const auto &item : legacy) assert(get(target / item.first) == item.second);
        assert(get(target / "config/config.txt") == "legacy backup config\n");
        assert(get(target / "config/midi_map.txt") == "note_map=36,kick.wav\nkey_macro=legacy\n");
        assert(get(target / "config/bluetooth/paired.txt") == "paired\n");
        assert(get(target / "config/scenes/test.scene") == "audiox-scenes 1\n");
        assert(get(target / "sfx/legacy.wav") == "sound");
        assert(!fs::exists(target / "config/scripts.txt"));
        assert(!fs::exists(target / "config/scripts"));
        assert(get(target / "config/.layout-version") == "1\n");
    }
    close(fd);
    puts("PASS: old archives and interrupted old restore transactions migrate without touching legacy files");
}

void testMalformed() {
    Workspace work;
    fs::path target = work.path / "target", path = work.path / "invalid.axb";
    fixture(target);
    std::vector<Entry> entries = {{"config.txt", 1, "settings"}, {"bluetooth", 2, ""}, {"sfx", 2, ""}};
    std::string valid = encode(entries);
    for (size_t pos : {size_t(0), size_t(8), size_t(12), size_t(32), valid.size() - 5}) {
        std::string bad = valid;
        bad[pos] ^= 1;
        reject(target, path, bad);
    }
    reject(target, path, valid.substr(0, valid.size() - 1));
    reject(target, path, valid + "extra");
    reject(target, path, encode({{"sfx", 2, ""}}));
    std::vector<Entry> tooMany(audiox::backup::maxEntries + 1, {"sfx", 2, ""});
    reject(target, path, encode(tooMany));
    for (const std::string &name : {std::string("../config.txt"), std::string("/config.txt"),
            std::string("sfx/../config.txt"), std::string("sfx/./a"), std::string("sfx//a"),
            std::string("sfx/a/"), std::string("sfx\\a"), std::string("system/boot-trial"),
            std::string("bluetooth/other.txt"), std::string("sfx/a\0b", 7)}) {
        auto bad = entries;
        bad.push_back({name, 1, "evil"});
        reject(target, path, encode(bad));
    }
    for (const Entry &entry : std::vector<Entry>{
            {"config.txt", 1, "duplicate"}, {"sfx/link", 3, "symlink"},
            {"routing.txt", 2, ""}, {"scripts.txt", 2, ""}, {"scripts", 1, "bad"},
            {"scripts.txt", 1, "# audiox scripts v2\n"},
            {"sfx/directory", 2, "not empty"},
            {"sfx/missing/file.wav", 1, "no parent"}, {"fx_params.txt", 1, std::string(1024 * 1024 + 1, 'a')}}) {
        auto bad = entries;
        bad.push_back(entry);
        reject(target, path, encode(bad));
    }
    int fd = open(path.c_str(), O_RDONLY);
    assert(fd >= 0);
    char error[256];
    assert(audiox::backup::stage(target.c_str(), fd, audiox::backup::maxArchiveSize + 1, error, sizeof(error)) < 0);
    close(fd);
    // A valid CRC still cannot legitimize an overflowing payload length.
    std::string bad = valid.substr(0, valid.size() - 4);
    audioxUpdatePutU64((unsigned char *)&bad[24], UINT64_MAX);
    unsigned char trailer[4];
    audioxUpdatePutU32(trailer, audioxUpdateCrc32(0, bad.data(), bad.size()));
    bad.append((const char *)trailer, 4);
    reject(target, path, bad);
    puts("PASS: corrupt, unsafe, incomplete, duplicate, oversized and overflowing archives leave settings untouched");
}

void testUnsupportedSchema() {
    Workspace work;
    fs::path target = work.path / "target", archivePath = work.path / "future.axb";
    fixture(target);
    std::vector<Entry> futureConfig = {
        {"config.txt", 1, "config_version=2\nusb_sample_rate=96000\n"},
        {"bluetooth", 2, ""},
        {"sfx", 2, ""},
    };
    reject(target, archivePath, encode(futureConfig));
    std::vector<Entry> futureBluetooth = {
        {"config.txt", 1, "legacy settings"},
        {"bluetooth", 2, ""},
        {"bluetooth/paired.txt", 1, "# audiox bluetooth paired v2\n"},
        {"sfx", 2, ""},
    };
    reject(target, archivePath, encode(futureBluetooth));

    fs::path source = work.path / "source";
    fixture(source);
    put(source / "config/bluetooth/paired.txt", "# audiox bluetooth paired v2\n");
    int fd = open(archivePath.c_str(), O_CREAT | O_RDWR, 0600);
    assert(fd >= 0);
    char error[256] = {};
    assert(audiox::backup::create(source.c_str(), fd, error, sizeof(error)) < 0);
    assert(strstr(error, "unsupported schema version"));
    put(source / "config/bluetooth/paired.txt", "paired\n");
    put(source / "config/scripts.txt", "# audiox scripts v2\n");
    assert(audiox::backup::create(source.c_str(), fd, error, sizeof(error)) < 0);
    assert(strstr(error, "unsupported schema version"));
    close(fd);
    puts("PASS: unsupported configuration schemas cannot be exported or restored");
}

} // namespace

int main() {
    testRoundTrip();
    testAbsentAndSymlinks();
    testLegacyRestore();
    testMalformed();
    testUnsupportedSchema();
}
