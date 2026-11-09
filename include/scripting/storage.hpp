#pragma once

#include "scripting/script.hpp"
#include "defs.hpp"
#include <string>
#include <vector>

inline constexpr unsigned SCRIPT_STORAGE_FILES_MAX = 32;
inline constexpr size_t SCRIPT_STORAGE_FILE_MAX = 4096;
inline constexpr size_t SCRIPT_STORAGE_TOTAL_MAX = 65536;
inline constexpr size_t SCRIPT_STORAGE_NAME_MAX = 63;
inline constexpr const char *SCRIPT_STORAGE_SETTINGS_NAME = ".settings";

// All methods return RET_OK or RET_ERR with errno set. Missing reads return
// RET_ERR/ENOENT. Reads never create directories. The override is a scripts root.
class ScriptStorage {
public:
    explicit ScriptStorage(uint32_t id, const char *root = CONFIG_ROOT_DIR "/scripts");
    int readFile(const char *name, std::string &text) const;
    int listFiles(std::vector<std::string> &names) const;
    int writeFile(const char *name, const char *text, size_t length) const;
    int writeFile(const char *name, const std::string &text) const {
        return writeFile(name, text.data(), text.size());
    }
    int removeFile(const char *name) const;
    int readSettings(ScriptState &settings) const;
    int setSetting(const ScriptStateEntry &entry, bool remove = false) const;
    int nextId(uint32_t minimum, uint32_t &out) const;
    static bool validName(const char *name);
    static bool validSetting(const ScriptStateEntry &entry);
private:
    uint32_t id_;
    std::string root_;
    int openRoot(bool create) const;
    int openDirectory(bool create) const;
    int read(const char *name, std::string &text) const;
    int write(const char *name, const char *text, size_t length, bool settings) const;
    int writeLocked(int directory, const char *name, const char *text, size_t length, bool settings) const;
};
