#include "update_archive.hpp"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct InputFile {
    uint32_t type;
    const char *path;
    uint64_t size;
    uint32_t crc;
};

static int scanFile(InputFile &input) {
    FILE *file = fopen(input.path, "rb");
    if (!file) {
        fprintf(stderr, "failed to open %s: %s\n", input.path, strerror(errno));
        return -1;
    }

    unsigned char buf[65536];
    input.size = 0;
    input.crc = 0;
    size_t count = 0;
    while ((count = fread(buf, 1, sizeof(buf), file)) > 0) {
        input.size += count;
        input.crc = audioxUpdateCrc32(input.crc, buf, count);
    }
    if (ferror(file)) {
        fprintf(stderr, "failed to read %s: %s\n", input.path, strerror(errno));
        fclose(file);
        return -1;
    }
    fclose(file);

    if (input.size == 0) {
        fprintf(stderr, "refusing empty update entry: %s\n", input.path);
        return -1;
    }
    if (input.type == AUDIOX_UPDATE_CONFIG && input.size > AUDIOX_UPDATE_CONFIG_MAX) {
        fprintf(stderr, "boot config is too large: %s\n", input.path);
        return -1;
    }
    return 0;
}

static int writeAll(FILE *out, const void *data, size_t len) {
    return fwrite(data, 1, len, out) == len ? 0 : -1;
}

static int appendFile(FILE *out, const InputFile &input) {
    uint8_t header[AUDIOX_UPDATE_ENTRY_HEADER_SIZE];
    audioxUpdatePutU32(header, input.type);
    audioxUpdatePutU32(header + 4, input.crc);
    audioxUpdatePutU64(header + 8, input.size);
    if (writeAll(out, header, sizeof(header)) < 0) {
        return -1;
    }

    FILE *file = fopen(input.path, "rb");
    if (!file) {
        fprintf(stderr, "failed to reopen %s: %s\n", input.path, strerror(errno));
        return -1;
    }

    unsigned char buf[65536];
    uint64_t copied = 0;
    size_t count = 0;
    while ((count = fread(buf, 1, sizeof(buf), file)) > 0) {
        if (writeAll(out, buf, count) < 0) {
            fclose(file);
            return -1;
        }
        copied += count;
    }
    int failed = ferror(file);
    fclose(file);
    if (failed || copied != input.size) {
        fprintf(stderr, "input changed while archiving: %s\n", input.path);
        return -1;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s <output> <config.txt> <bootloader> <kernel> <program>\n", argv[0]);
        return 2;
    }

    InputFile inputs[AUDIOX_UPDATE_ENTRY_COUNT] = {
        {AUDIOX_UPDATE_CONFIG, argv[2], 0, 0},
        {AUDIOX_UPDATE_BOOTLOADER, argv[3], 0, 0},
        {AUDIOX_UPDATE_KERNEL, argv[4], 0, 0},
        {AUDIOX_UPDATE_PROGRAM, argv[5], 0, 0},
    };
    for (InputFile &input : inputs) {
        if (scanFile(input) < 0) {
            return 1;
        }
    }

    char temp_path[4096];
    int n = snprintf(temp_path, sizeof(temp_path), "%s.tmp", argv[1]);
    if (n <= 0 || (size_t)n >= sizeof(temp_path)) {
        fprintf(stderr, "output path is too long\n");
        return 1;
    }

    FILE *out = fopen(temp_path, "wb");
    if (!out) {
        fprintf(stderr, "failed to create %s: %s\n", temp_path, strerror(errno));
        return 1;
    }

    uint8_t header[AUDIOX_UPDATE_HEADER_SIZE];
    memcpy(header, AUDIOX_UPDATE_MAGIC, AUDIOX_UPDATE_MAGIC_SIZE);
    audioxUpdatePutU32(header + 8, AUDIOX_UPDATE_VERSION);
    audioxUpdatePutU32(header + 12, AUDIOX_UPDATE_ENTRY_COUNT);
    int failed = writeAll(out, header, sizeof(header));
    for (const InputFile &input : inputs) {
        if (!failed && appendFile(out, input) < 0) {
            failed = 1;
        }
    }
    if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
        failed = 1;
    }
    if (fclose(out) != 0) {
        failed = 1;
    }
    if (failed) {
        fprintf(stderr, "failed to write %s\n", temp_path);
        unlink(temp_path);
        return 1;
    }
    if (rename(temp_path, argv[1]) != 0) {
        fprintf(stderr, "failed to finalize %s: %s\n", argv[1], strerror(errno));
        unlink(temp_path);
        return 1;
    }

    printf("Created update archive %s\n", argv[1]);
    return 0;
}
