#include "config/storage.hpp"
#include "config/paths.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
static bool failSync = false;
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if (failSync) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
static void put(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << text;
    file.close();
    assert(file);
}
static std::string get(const fs::path &path) {
    std::ifstream file(path);
    assert(file);
    return {std::istreambuf_iterator<char>(file), {}};
}
int main() {
    using namespace audiox::config_storage;
    char directory[] = "/tmp/audiox-config-storage-XXXXXX";
    assert(mkdtemp(directory));
    const fs::path root(directory);
    char error[256]{};
    const std::string legacy = "# audiox midi map v1\nnote_map=36,kick.wav\n"
        "key_chord=0,60,ALT+TAB\nkey_macro=1,61,0,0,-,7461702822412229\n"
        "automation=42,1,0,62,1,2,54657374,-,7461702822422229\n"
        "automation_events=42,1\nautomation_config_write=42,1\nautomation_hooks=42,0\n";
    put(root / "config.txt", "usb_sample_rate=48000\n");
    put(root / "config.staging.txt", "usb_sample_rate=44100\n");
    put(root / "midi_map.txt", legacy);
    put(root / "scripts/42/options.txt", "option=1\n");
    put(root / "bluetooth/paired.txt", "pairing data");
    put(root / "scenes/test.scene", "audiox-scenes 1\n");
    put(root / "telemetry-consent-v3.txt", "0\n");
    put(root / "stdout.log", "log");
    put(root / "ffmpeg", "binary");
    put(root / "sfx/kick.wav", "sound");
    put(root / ".config-migration/partial", "interrupted migration");
    failSync = true;
    assert(migrate(directory, error, sizeof(error)) < 0);
    failSync = false;
    assert(!fs::exists(root / "config"));
    assert(get(root / "midi_map.txt") == legacy);
    assert(migrate(directory, error, sizeof(error)) == 0);
    assert(get(root / "config/.layout-version") == "1\n");
    assert(get(root / "midi_map.txt") == legacy);
    assert(get(root / "config/midi_map.txt") == "# audiox midi map v1\nnote_map=36,kick.wav\n");
    const auto scripts = get(root / "config/scripts.txt");
    assert(scripts.find("key_chord=") != std::string::npos);
    assert(scripts.find("key_macro=") != std::string::npos);
    assert(scripts.find("automation_config_write=42,1") != std::string::npos);
    assert(get(root / "config/scripts/42/options.txt") == "option=1\n");
    assert(get(root / "config/telemetry-consent-v3.txt") == "0\n");
    put(root / "config/config.txt", "active changes");
    assert(migrate(directory, error, sizeof(error)) == 0);
    assert(get(root / "config/config.txt") == "active changes");
    // Interrupted script splitting must not overwrite already-published scripts.
    put(root / "config/midi_map.txt", legacy);
    assert(splitScripts((root / "config").c_str(), error, sizeof(error)) == 0);
    assert(get(root / "config/scripts.txt") == scripts);
    assert(legacyCount(directory, error, sizeof(error)) > 0);
    assert(removeLegacy(directory, error, sizeof(error)) == 0);
    assert(legacyCount(directory, error, sizeof(error)) == 0);
    assert(get(root / "config/config.txt") == "active changes");
    assert(get(root / "stdout.log") == "log" && get(root / "sfx/kick.wav") == "sound");
    assert(get(root / "ffmpeg") == "binary");
    assert(removeLegacy(directory, error, sizeof(error)) == 0);
    put(root / "config.txt", "retain me");
    put(root / "config/.layout-version", "2\n");
    assert(migrate(directory, error, sizeof(error)) < 0);
    assert(removeLegacy(directory, error, sizeof(error)) < 0);
    assert(get(root / "config.txt") == "retain me");
    assert(rootfsPath("routing.txt") == "config/routing.txt");
    assert(rootfsPath("./config//routing.txt") == "config/routing.txt");
    assert(!rootfsWritable(directory, "config//./.layout-version"));
    assert(rootfsPath("scripts/42/a.txt") == "config/scripts/42/a.txt");
    assert(rootfsPath("sfx/kick.wav") == "sfx/kick.wav");
    fs::remove_all(root / "config");
    fs::create_directory_symlink(root / "sfx", root / "scripts");
    assert(migrate(directory, error, sizeof(error)) < 0);
    assert(!fs::exists(root / "config"));
    assert(get(root / "config.txt") == "retain me");
    fs::remove_all(root);
    // A clean installation gets an authoritative, idempotent empty layout.
    fs::create_directory(root);
    assert(migrate(directory, error, sizeof(error)) == 0);
    assert(get(root / "config/scripts.txt") == "# audiox scripts v1\n");
    assert(legacyCount(directory, error, sizeof(error)) == 0);
    put(root / "config/midi_map.txt", legacy);
    put(root / "config/scripts.txt", "# audiox scripts v2\nfuture script\n");
    assert(splitScripts((root / "config").c_str(), error, sizeof(error)) < 0);
    assert(get(root / "config/midi_map.txt") == legacy);
    fs::remove_all(root);
    puts("configuration migration, legacy retention, cleanup, and retry tests passed");
}
