#pragma once

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

// AudioX update archive (.axu). All integers are little-endian.
// Header: magic[8] = "AXUPDATE", u32 version, u32 entry_count.
// Each entry: u32 type, u32 crc32, u64 size, followed by size payload bytes. Entries may appear
// in any order and the archive ends exactly after the last entry.
//
// Version 1 (legacy): exactly one config, bootloader, kernel, and program entry; the kernel is
// installed as kernel8.img.
//
// Version 2 is the current format and is designed to stay readable by every v2 device:
// - Required entries: config, bootloader, kernel, program, metadata (one of each).
// - Optional known entries (one at most): cmdline. Absent optional entries keep the device's file.
// - entry_count may be anything from 1 to AUDIOX_UPDATE_ENTRY_LIMIT.
// - Readers skip entries whose type they do not know (after checking their CRC), unless the
//   type has AUDIOX_UPDATE_TYPE_REQUIRED set, in which case the archive is rejected because
//   installing it without understanding that entry would be wrong.
// - Metadata is "key=value" lines (blank lines and '#' comments allowed). board= (pi4, pi5) and
//   kernel= (boot partition file name of the kernel entry) are required; version= and
//   kernel_version= are optional; readers ignore unknown keys.
// Rules for future writers: add new data as new entry types or metadata keys, never change the
// meaning of an existing type or key, set AUDIOX_UPDATE_TYPE_REQUIRED only when older readers
// must refuse the update, and bump the header version only for changes that cannot be expressed
// this way (older readers then reject the archive instead of installing it wrongly).
#define AUDIOX_UPDATE_MAGIC "AXUPDATE"
#define AUDIOX_UPDATE_MAGIC_SIZE 8u
#define AUDIOX_UPDATE_VERSION_V1 1u
#define AUDIOX_UPDATE_VERSION 2u
#define AUDIOX_UPDATE_HEADER_SIZE 16u
#define AUDIOX_UPDATE_ENTRY_HEADER_SIZE 16u
#define AUDIOX_UPDATE_ENTRY_COUNT_V1 4u
// Number of required v2 entries (types 1..5).
#define AUDIOX_UPDATE_ENTRY_COUNT 5u
// Highest entry type this reader understands; entries[] arrays are indexed by type - 1.
#define AUDIOX_UPDATE_KNOWN_TYPES 6u
#define AUDIOX_UPDATE_ENTRY_LIMIT 64u
#define AUDIOX_UPDATE_TYPE_REQUIRED 0x80000000u
#define AUDIOX_UPDATE_CONFIG_MAX (1024u * 1024u)
#define AUDIOX_UPDATE_METADATA_MAX 4096u
#define AUDIOX_UPDATE_CMDLINE_MAX 4096u
#define AUDIOX_UPDATE_VALUE_MAX 64u
#define AUDIOX_UPDATE_DEFAULT_KERNEL "kernel8.img"

enum AudioxUpdateEntryType : uint32_t {
    AUDIOX_UPDATE_CONFIG = 1,
    AUDIOX_UPDATE_BOOTLOADER = 2,
    AUDIOX_UPDATE_KERNEL = 3,
    AUDIOX_UPDATE_PROGRAM = 4,
    AUDIOX_UPDATE_METADATA = 5,
    AUDIOX_UPDATE_CMDLINE = 6,
};

// cmdline.txt must be one non-empty line of printable ASCII (an optional trailing newline is
// allowed) and must start the bootloader with rdinit=/init, or the device cannot boot.
// Returns NULL when valid or a short error description.
static inline const char *audioxValidateCmdline(const char *text, size_t len) {
    if (len > 0 && text[len - 1] == '\n') {
        --len;
        if (len > 0 && text[len - 1] == '\r') {
            --len;
        }
    }
    if (len == 0) {
        return "cmdline is empty";
    }
    if (len >= AUDIOX_UPDATE_CMDLINE_MAX) {
        return "cmdline is too long";
    }
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '\n' || text[i] == '\r') {
            return "cmdline must be a single line";
        }
        if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e) {
            return "cmdline contains non-printable characters";
        }
    }
    static const char required[] = "rdinit=/init";
    const size_t req_len = sizeof(required) - 1;
    for (size_t i = 0; i + req_len <= len; ++i) {
        bool start = i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t';
        bool end = i + req_len == len || text[i + req_len] == ' ' || text[i + req_len] == '\t';
        if (start && end && memcmp(text + i, required, req_len) == 0) {
            return NULL;
        }
    }
    return "cmdline must contain rdinit=/init";
}

struct AudioxUpdateMetadata {
    char board[AUDIOX_UPDATE_VALUE_MAX];
    char kernel[AUDIOX_UPDATE_VALUE_MAX];
    char version[AUDIOX_UPDATE_VALUE_MAX];
    char kernel_version[AUDIOX_UPDATE_VALUE_MAX];
};

// Metadata values are restricted to [A-Za-z0-9._+-] so they are safe as file names and in logs.
static inline bool audioxUpdateValidValue(const char *value, size_t len) {
    if (len == 0 || len >= AUDIOX_UPDATE_VALUE_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = value[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '+' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// Kernel names must look like kernel*.img and stay inside the boot partition root.
static inline bool audioxUpdateValidKernelName(const char *name) {
    size_t len = strlen(name);
    return audioxUpdateValidValue(name, len) && len >= 10 && memcmp(name, "kernel", 6) == 0 &&
           memcmp(name + len - 4, ".img", 4) == 0;
}

static inline bool audioxUpdateValidBoard(const char *board) {
    return strcmp(board, "pi4") == 0 || strcmp(board, "pi5") == 0;
}

static inline bool audioxUpdateKeyIs(const char *key, size_t key_len, const char *name) {
    return strlen(name) == key_len && memcmp(key, name, key_len) == 0;
}

// Finds the first "kernel=" line in config.txt text (conditional sections are not evaluated).
// Writes the default kernel name when no valid line is present.
static inline void audioxConfigKernelName(const char *text, size_t len, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s", AUDIOX_UPDATE_DEFAULT_KERNEL);
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n') {
            ++end;
        }
        size_t p = pos;
        while (p < end && (text[p] == ' ' || text[p] == '\t')) {
            ++p;
        }
        if (end - p > 7 && memcmp(text + p, "kernel=", 7) == 0) {
            size_t v = p + 7;
            size_t v_end = v;
            while (v_end < end && text[v_end] != ' ' && text[v_end] != '\t' && text[v_end] != '\r' &&
                   text[v_end] != '#') {
                ++v_end;
            }
            char name[AUDIOX_UPDATE_VALUE_MAX];
            size_t name_len = v_end - v;
            if (name_len < sizeof(name)) {
                memcpy(name, text + v, name_len);
                name[name_len] = '\0';
                if (audioxUpdateValidKernelName(name)) {
                    snprintf(out, out_sz, "%s", name);
                }
            }
            return;
        }
        pos = end + 1;
    }
}

// Parses a metadata payload. Returns NULL on success or a short error description.
static inline const char *audioxUpdateParseMetadata(const char *text, size_t len, AudioxUpdateMetadata *out) {
    out->board[0] = '\0';
    out->kernel[0] = '\0';
    out->version[0] = '\0';
    out->kernel_version[0] = '\0';
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n') {
            ++end;
        }
        size_t line_end = end;
        if (line_end > pos && text[line_end - 1] == '\r') {
            --line_end;
        }
        size_t eq = pos;
        while (eq < line_end && text[eq] != '=') {
            ++eq;
        }
        if (line_end > pos && text[pos] != '#') {
            if (eq == line_end) {
                return "malformed metadata line";
            }
            const char *key = text + pos;
            size_t key_len = eq - pos;
            const char *value = text + eq + 1;
            size_t value_len = line_end - eq - 1;
            char *dest = NULL;
            if (audioxUpdateKeyIs(key, key_len, "board")) {
                dest = out->board;
            } else if (audioxUpdateKeyIs(key, key_len, "kernel")) {
                dest = out->kernel;
            } else if (audioxUpdateKeyIs(key, key_len, "version")) {
                dest = out->version;
            } else if (audioxUpdateKeyIs(key, key_len, "kernel_version")) {
                dest = out->kernel_version;
            }
            if (dest) {
                if (!audioxUpdateValidValue(value, value_len)) {
                    return "invalid metadata value";
                }
                for (size_t i = 0; i < value_len; ++i) {
                    dest[i] = value[i];
                }
                dest[value_len] = '\0';
            }
        }
        pos = end + 1;
    }
    if (!out->board[0] || !audioxUpdateValidBoard(out->board)) {
        return "metadata board is missing or unsupported";
    }
    if (!out->kernel[0] || !audioxUpdateValidKernelName(out->kernel)) {
        return "metadata kernel name is missing or invalid";
    }
    return NULL;
}

static inline uint32_t audioxUpdateCrc32(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

static inline void audioxUpdatePutU32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static inline void audioxUpdatePutU64(uint8_t *out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = (uint8_t)(value >> (i * 8));
    }
}

static inline uint32_t audioxUpdateGetU32(const uint8_t *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static inline uint64_t audioxUpdateGetU64(const uint8_t *in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= (uint64_t)in[i] << (i * 8);
    }
    return value;
}

// Byte range of one known entry inside an archive file; size 0 means the entry is absent.
struct AudioxUpdateEntry {
    uint32_t crc;
    uint64_t size;
    off_t offset;
};

static inline int audioxUpdateReadAt(int fd, void *buf, size_t len, off_t offset) {
    unsigned char *p = (unsigned char *)buf;
    size_t done = 0;
    while (done < len) {
        ssize_t nr = pread(fd, p + done, len - done, offset + (off_t)done);
        if (nr < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (nr == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)nr;
    }
    return 0;
}

// Returns 0 when the payload matches its CRC, 1 on mismatch, -1 on read failure.
static inline int audioxUpdateCheckEntryCrc(int fd, const AudioxUpdateEntry &entry) {
    unsigned char buf[16384];
    uint64_t done = 0;
    uint32_t crc = 0;
    while (done < entry.size) {
        size_t want = sizeof(buf);
        if (entry.size - done < want) {
            want = (size_t)(entry.size - done);
        }
        if (audioxUpdateReadAt(fd, buf, want, entry.offset + (off_t)done) < 0) {
            return -1;
        }
        crc = audioxUpdateCrc32(crc, buf, want);
        done += want;
    }
    return crc == entry.crc ? 0 : 1;
}

// Validates the archive structure and every entry checksum, filling entries[type - 1] for the
// known types. Unknown entries are checked and skipped unless they carry
// AUDIOX_UPDATE_TYPE_REQUIRED. Returns NULL on success or an error message ending in '\n'.
static inline const char *audioxUpdateParseArchive(int fd, uint64_t archive_size,
                                                   AudioxUpdateEntry entries[AUDIOX_UPDATE_KNOWN_TYPES]) {
    memset(entries, 0, sizeof(AudioxUpdateEntry) * AUDIOX_UPDATE_KNOWN_TYPES);
    uint8_t header[AUDIOX_UPDATE_HEADER_SIZE];
    if (archive_size < sizeof(header) || audioxUpdateReadAt(fd, header, sizeof(header), 0) < 0) {
        return "update archive header is incomplete\n";
    }
    if (memcmp(header, AUDIOX_UPDATE_MAGIC, AUDIOX_UPDATE_MAGIC_SIZE) != 0) {
        return "invalid update archive magic\n";
    }
    const uint32_t version = audioxUpdateGetU32(header + 8);
    const uint32_t entry_count = audioxUpdateGetU32(header + 12);
    uint32_t required_mask = 0;
    uint32_t max_type = AUDIOX_UPDATE_PROGRAM;
    if (version == AUDIOX_UPDATE_VERSION_V1) {
        if (entry_count != AUDIOX_UPDATE_ENTRY_COUNT_V1) {
            return "update archive has the wrong number of entries\n";
        }
        required_mask = (1u << AUDIOX_UPDATE_ENTRY_COUNT_V1) - 1u;
    } else if (version == AUDIOX_UPDATE_VERSION) {
        if (entry_count == 0 || entry_count > AUDIOX_UPDATE_ENTRY_LIMIT) {
            return "update archive has the wrong number of entries\n";
        }
        required_mask = (1u << AUDIOX_UPDATE_ENTRY_COUNT) - 1u;
        max_type = AUDIOX_UPDATE_KNOWN_TYPES;
    } else {
        return "unsupported update archive version (newer firmware required)\n";
    }

    uint32_t seen = 0;
    uint64_t offset = sizeof(header);
    for (uint32_t i = 0; i < entry_count; ++i) {
        uint8_t entry_header[AUDIOX_UPDATE_ENTRY_HEADER_SIZE];
        if (archive_size - offset < sizeof(entry_header) ||
            audioxUpdateReadAt(fd, entry_header, sizeof(entry_header), (off_t)offset) < 0) {
            return "update archive entry header is incomplete\n";
        }
        offset += sizeof(entry_header);

        const uint32_t type = audioxUpdateGetU32(entry_header);
        AudioxUpdateEntry entry;
        entry.crc = audioxUpdateGetU32(entry_header + 4);
        entry.size = audioxUpdateGetU64(entry_header + 8);
        entry.offset = (off_t)offset;
        if (entry.size > archive_size - offset) {
            return "invalid update archive entry size\n";
        }
        const bool known = type >= AUDIOX_UPDATE_CONFIG && type <= max_type;
        if (!known) {
            if (version == AUDIOX_UPDATE_VERSION_V1) {
                return "unknown update archive entry type\n";
            }
            if ((type & AUDIOX_UPDATE_TYPE_REQUIRED) != 0) {
                return "update archive needs newer firmware (unknown required entry)\n";
            }
        } else {
            const uint32_t bit = 1u << (type - 1u);
            if ((seen & bit) != 0) {
                return "duplicate update archive entry\n";
            }
            if (entry.size == 0) {
                return "invalid update archive entry size\n";
            }
            if ((type == AUDIOX_UPDATE_CONFIG && entry.size > AUDIOX_UPDATE_CONFIG_MAX) ||
                (type == AUDIOX_UPDATE_METADATA && entry.size > AUDIOX_UPDATE_METADATA_MAX) ||
                (type == AUDIOX_UPDATE_CMDLINE && entry.size > AUDIOX_UPDATE_CMDLINE_MAX)) {
                return "update archive entry is too large\n";
            }
            seen |= bit;
        }

        const int crc_rc = audioxUpdateCheckEntryCrc(fd, entry);
        if (crc_rc < 0) {
            return "failed to read update archive entry\n";
        }
        if (crc_rc > 0) {
            return "update archive entry checksum mismatch\n";
        }
        if (known) {
            entries[type - 1u] = entry;
        }
        offset += entry.size;
    }

    if (offset != archive_size) {
        return "update archive has unexpected trailing data\n";
    }
    if ((seen & required_mask) != required_mask) {
        return "update archive is missing required entries\n";
    }
    return NULL;
}
