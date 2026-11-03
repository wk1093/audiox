#include "midi/script_storage.hpp"

#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int main() {
    const std::string base = "tests/.script-storage-" + std::to_string(getpid());
    const std::string root = base + "/scripts";
    assert(!std::filesystem::exists(base));
    ScriptStorage store(7, root.c_str()), other(8, root.c_str()), invalid(0, root.c_str());
    std::string text;
    std::vector<std::string> names;
    MidiScriptState settings{};
    assert(invalid.writeFile("x", "x", 1) == RET_ERR && errno == EINVAL);
    assert(invalid.listFiles(names) == RET_ERR);
    assert(store.readFile("absent", text) == RET_ERR && errno == ENOENT);
    assert(store.listFiles(names) == RET_OK && names.empty());
    assert(store.readSettings(settings) == RET_OK && settings.count == 0);
    assert(!std::filesystem::exists(base));
    uint32_t id = 0;
    assert(store.nextId(10, id) == RET_OK && id == 10);
    for (const char *bad : {"", ".", "..", "../config.txt", "/config.txt", "x/y", "x\\y",
                           ".settings", ".hidden", "a..b", "a b", "ä", "a\n"}) {
        assert(!ScriptStorage::validName(bad));
        assert(store.writeFile(bad, "x", 1) == RET_ERR && errno == EINVAL);
        assert(store.readFile(bad, text) == RET_ERR && errno == EINVAL);
        assert(store.removeFile(bad) == RET_ERR && errno == EINVAL);
    }
    assert(!ScriptStorage::validName(std::string(64, 'x').c_str()));
    assert(store.writeFile("a-_.txt", "first", 5) == RET_OK);
    assert(store.writeFile("a-_.txt", "second", 6) == RET_OK);
    assert(store.readFile("a-_.txt", text) == RET_OK && text == "second");
    assert(other.readFile("a-_.txt", text) == RET_ERR && errno == ENOENT);
    assert(store.writeFile("nul", "a\0b\1", 4) == RET_OK);
    assert(store.readFile("nul", text) == RET_OK && text == std::string("a\0b\1", 4));
    assert(store.removeFile("nul") == RET_OK);
    assert(store.writeFile("large", std::string(SCRIPT_STORAGE_FILE_MAX + 1, 'x')) == RET_ERR);
    assert(store.listFiles(names) == RET_OK && names.size() == 1 && names[0] == "a-_.txt");
    assert(store.removeFile("a-_.txt") == RET_OK);
    assert(store.removeFile("a-_.txt") == RET_OK);
    assert(store.removeFile("missing") == RET_OK);
    {
        const std::string stale = root + "/7/.write-123-0";
        int pending = open(stale.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(pending >= 0 && write(pending, "partial", 7) == 7 && close(pending) == 0);
        assert(store.listFiles(names) == RET_OK && names.empty());
        assert(store.writeFile("recovered", "ok", 2) == RET_OK);
        assert(!std::filesystem::exists(stale));
        assert(store.removeFile("recovered") == RET_OK);
    }

    MidiScriptStateEntry entry{};
    strcpy(entry.key, "enabled"); entry.kind = MidiScriptValueKind::Boolean; entry.booleanValue = true;
    assert(store.setSetting(entry) == RET_OK);
    strcpy(entry.key, "number"); entry.kind = MidiScriptValueKind::Number; entry.numberValue = 0.125;
    assert(store.setSetting(entry) == RET_OK);
    strcpy(entry.key, "integer"); entry.numberIsInteger = true; entry.integerValue = INT64_MAX;
    entry.numberValue = static_cast<double>(INT64_MAX);
    assert(store.setSetting(entry) == RET_OK);
    strcpy(entry.key, "text\t\n"); entry.kind = MidiScriptValueKind::String;
    strcpy(entry.stringValue, "quote \" slash \\ newline\n");
    assert(store.setSetting(entry) == RET_OK);
    assert(store.readSettings(settings) == RET_OK && settings.count == 4);
    assert(settings.entries[0].booleanValue && settings.entries[1].numberValue == 0.125);
    assert(settings.entries[2].integerValue == INT64_MAX && settings.entries[2].numberIsInteger);
    assert(!strcmp(settings.entries[3].stringValue, entry.stringValue));
    assert(other.readSettings(settings) == RET_OK && settings.count == 0);
    assert(store.listFiles(names) == RET_OK && names.empty());
    assert(store.setSetting(entry, true) == RET_OK);
    assert(store.readSettings(settings) == RET_OK && settings.count == 3);
    entry = {}; strcpy(entry.key, "bad"); entry.kind = MidiScriptValueKind::Number; entry.numberValue = INFINITY;
    assert(store.setSetting(entry) == RET_ERR);
    entry.numberValue = 1; entry.numberIsInteger = true; entry.integerValue = 2;
    assert(store.setSetting(entry) == RET_ERR);
    entry = {}; entry.kind = MidiScriptValueKind::Boolean;
    assert(store.setSetting(entry) == RET_ERR);

    ScriptStorage countStore(20, root.c_str());
    for (unsigned i = 0; i < SCRIPT_STORAGE_FILES_MAX; ++i)
        assert(countStore.writeFile(("f" + std::to_string(i)).c_str(), "x", 1) == RET_OK);
    assert(countStore.writeFile("overflow", "x", 1) == RET_ERR && errno == EDQUOT);
    assert(countStore.writeFile("f0", "", 0) == RET_OK);
    assert(countStore.removeFile("f1") == RET_OK);
    assert(countStore.writeFile("overflow", "x", 1) == RET_OK);
    ScriptStorage quotaStore(21, root.c_str());
    for (unsigned i = 0; i < SCRIPT_STORAGE_TOTAL_MAX / SCRIPT_STORAGE_FILE_MAX; ++i)
        assert(quotaStore.writeFile(("f" + std::to_string(i)).c_str(), std::string(SCRIPT_STORAGE_FILE_MAX, 'x')) == RET_OK);
    assert(quotaStore.writeFile("overflow", "x", 1) == RET_ERR && errno == EDQUOT);
    assert(quotaStore.writeFile("f0", "short", 5) == RET_OK);
    assert(quotaStore.writeFile("overflow", "x", 1) == RET_OK);
    ScriptStorage settingsStore(22, root.c_str());
    for (unsigned i = 0; i < MIDI_SCRIPT_STATE_ENTRIES_MAX; ++i) {
        entry = {}; snprintf(entry.key, sizeof(entry.key), "k%u", i);
        entry.kind = MidiScriptValueKind::Boolean;
        assert(settingsStore.setSetting(entry) == RET_OK);
    }
    strcpy(entry.key, "overflow");
    assert(settingsStore.setSetting(entry) == RET_ERR && errno == EDQUOT);
    strcpy(entry.key, "k0"); assert(settingsStore.setSetting(entry, true) == RET_OK);
    strcpy(entry.key, "overflow"); assert(settingsStore.setSetting(entry) == RET_OK);

    const std::string directory = root + "/7";
    const std::string outside = base + "/outside";
    int fd = open(outside.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, "safe", 4) == 4 && close(fd) == 0);
    const std::string dangerous = directory + "/danger";
    assert(symlink(outside.c_str(), dangerous.c_str()) == 0);
    assert(store.readFile("danger", text) == RET_ERR);
    assert(store.writeFile("danger", "replace", 7) == RET_ERR);
    assert(store.removeFile("danger") == RET_ERR);
    assert(store.listFiles(names) == RET_ERR);
    assert(unlink(dangerous.c_str()) == 0);
    assert(link(outside.c_str(), dangerous.c_str()) == 0);
    assert(store.readFile("danger", text) == RET_ERR);
    assert(store.writeFile("danger", "replace", 7) == RET_ERR);
    assert(store.removeFile("danger") == RET_ERR);
    assert(store.listFiles(names) == RET_ERR);
    assert(unlink(dangerous.c_str()) == 0);
    assert(mkfifo(dangerous.c_str(), 0600) == 0);
    assert(store.readFile("danger", text) == RET_ERR);
    assert(store.writeFile("danger", "replace", 7) == RET_ERR);
    assert(unlink(dangerous.c_str()) == 0);
    assert(std::filesystem::create_directory(dangerous));
    assert(store.readFile("danger", text) == RET_ERR);
    assert(store.removeFile("danger") == RET_ERR);
    assert(rmdir(dangerous.c_str()) == 0);
    const std::string reserved = directory + "/.settings";
    assert(unlink(reserved.c_str()) == 0 && link(outside.c_str(), reserved.c_str()) == 0);
    assert(store.readSettings(settings) == RET_ERR);
    assert(store.setSetting(entry) == RET_ERR);
    assert(unlink(reserved.c_str()) == 0 && symlink(outside.c_str(), reserved.c_str()) == 0);
    assert(store.readSettings(settings) == RET_ERR);
    assert(store.setSetting(entry) == RET_ERR);
    assert(unlink(reserved.c_str()) == 0);
    fd = open(reserved.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, "garbage", 7) == 7 && close(fd) == 0);
    assert(store.readSettings(settings) == RET_ERR && errno == EINVAL);
    assert(store.setSetting(entry) == RET_ERR);
    assert(unlink(reserved.c_str()) == 0);

    assert(symlink("7", (root + "/30").c_str()) == 0);
    ScriptStorage symlinkId(30, root.c_str());
    assert(symlinkId.readFile("x", text) == RET_ERR);
    assert(symlinkId.writeFile("x", "x", 1) == RET_ERR);
    assert(unlink((root + "/30").c_str()) == 0);
    assert(symlink("scripts", (base + "/root-link").c_str()) == 0);
    ScriptStorage symlinkRoot(7, (base + "/root-link").c_str());
    assert(symlinkRoot.readFile("x", text) == RET_ERR);
    assert(symlinkRoot.writeFile("x", "x", 1) == RET_ERR);
    assert(symlink("scripts", (base + "/ancestor-link").c_str()) == 0);
    ScriptStorage symlinkAncestor(7, (base + "/ancestor-link/nested").c_str());
    assert(symlinkAncestor.writeFile("x", "x", 1) == RET_ERR);
    assert(!std::filesystem::exists(root + "/nested"));
    assert(store.nextId(1, id) == RET_OK && id == 23);
    assert(store.nextId(100, id) == RET_OK && id == 100);
    assert(std::filesystem::create_directory(root + "/4294967295"));
    assert(store.nextId(1, id) == RET_ERR && errno == EOVERFLOW && id == 0);
    fd = open(outside.c_str(), O_RDONLY);
    char safe[5]{};
    assert(fd >= 0 && read(fd, safe, 4) == 4 && close(fd) == 0 && !strcmp(safe, "safe"));
    std::filesystem::remove_all(base);
    puts("Script storage tests passed");
}
