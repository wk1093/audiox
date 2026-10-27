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

static int readSmallFile(const char *path, char *buf, size_t buf_sz, size_t *len_out) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }
    size_t len = fread(buf, 1, buf_sz, file);
    int failed = ferror(file);
    fclose(file);
    if (failed) {
        fprintf(stderr, "failed to read %s\n", path);
        return -1;
    }
    *len_out = len;
    return 0;
}

static int writeMetadataFile(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "failed to create %s: %s\n", path, strerror(errno));
        return -1;
    }
    int failed = writeAll(file, text, strlen(text));
    if (fclose(file) != 0) {
        failed = -1;
    }
    if (failed) {
        fprintf(stderr, "failed to write %s\n", path);
        unlink(path);
    }
    return failed;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --board <pi4|pi5> [--kernel-name <name>] [--version <v>] [--kernel-version <kv>]\n"
            "          [--cmdline <cmdline.txt>]\n"
            "          <output> <config.txt> <bootloader> <kernel> <program>\n"
            "  --kernel-name defaults to the file name of <kernel>\n",
            argv0);
}

} // namespace

int main(int argc, char **argv) {
    const char *board = NULL;
    const char *kernel_name = NULL;
    const char *version = NULL;
    const char *kernel_version = NULL;
    const char *cmdline_path = NULL;
    int argi = 1;
    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        const char *opt = argv[argi];
        if (argi + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        const char *value = argv[argi + 1];
        if (strcmp(opt, "--board") == 0) {
            board = value;
        } else if (strcmp(opt, "--kernel-name") == 0) {
            kernel_name = value;
        } else if (strcmp(opt, "--version") == 0) {
            version = value;
        } else if (strcmp(opt, "--kernel-version") == 0) {
            kernel_version = value;
        } else if (strcmp(opt, "--cmdline") == 0) {
            cmdline_path = value;
        } else {
            fprintf(stderr, "unknown option: %s\n", opt);
            usage(argv[0]);
            return 2;
        }
        argi += 2;
    }
    if (argc - argi != 5 || !board) {
        usage(argv[0]);
        return 2;
    }
    const char *output = argv[argi];
    const char *config_path = argv[argi + 1];
    const char *kernel_path = argv[argi + 3];
    if (!kernel_name) {
        const char *slash = strrchr(kernel_path, '/');
        kernel_name = slash ? slash + 1 : kernel_path;
    }

    char metadata[AUDIOX_UPDATE_METADATA_MAX];
    int meta_len = snprintf(metadata, sizeof(metadata), "board=%s\nkernel=%s\n", board, kernel_name);
    if (meta_len > 0 && version && version[0]) {
        meta_len += snprintf(metadata + meta_len, sizeof(metadata) - (size_t)meta_len, "version=%s\n", version);
    }
    if (meta_len > 0 && (size_t)meta_len < sizeof(metadata) && kernel_version && kernel_version[0]) {
        meta_len += snprintf(metadata + meta_len, sizeof(metadata) - (size_t)meta_len, "kernel_version=%s\n",
                             kernel_version);
    }
    if (meta_len <= 0 || (size_t)meta_len >= sizeof(metadata)) {
        fprintf(stderr, "metadata is too large\n");
        return 1;
    }
    AudioxUpdateMetadata parsed;
    const char *meta_error = audioxUpdateParseMetadata(metadata, (size_t)meta_len, &parsed);
    if (meta_error) {
        fprintf(stderr, "invalid metadata (%s):\n%s", meta_error, metadata);
        return 1;
    }

    // The archive's config.txt decides which kernel the firmware boots, so it must match.
    static char config_text[AUDIOX_UPDATE_CONFIG_MAX + 1];
    size_t config_len = 0;
    if (readSmallFile(config_path, config_text, sizeof(config_text), &config_len) < 0) {
        return 1;
    }
    char config_kernel[AUDIOX_UPDATE_VALUE_MAX];
    audioxConfigKernelName(config_text, config_len, config_kernel, sizeof(config_kernel));
    if (strcmp(config_kernel, parsed.kernel) != 0) {
        fprintf(stderr, "%s boots %s but the archive kernel is named %s\n", config_path, config_kernel, parsed.kernel);
        return 1;
    }

    if (cmdline_path) {
        char cmdline_text[AUDIOX_UPDATE_CMDLINE_MAX + 1];
        size_t cmdline_len = 0;
        if (readSmallFile(cmdline_path, cmdline_text, sizeof(cmdline_text), &cmdline_len) < 0) {
            return 1;
        }
        const char *cmdline_error = cmdline_len > AUDIOX_UPDATE_CMDLINE_MAX
                                        ? "cmdline is too long"
                                        : audioxValidateCmdline(cmdline_text, cmdline_len);
        if (cmdline_error) {
            fprintf(stderr, "invalid %s: %s\n", cmdline_path, cmdline_error);
            return 1;
        }
    }

    char meta_path[4096];
    int n = snprintf(meta_path, sizeof(meta_path), "%s.metadata", output);
    if (n <= 0 || (size_t)n >= sizeof(meta_path)) {
        fprintf(stderr, "output path is too long\n");
        return 1;
    }
    if (writeMetadataFile(meta_path, metadata) < 0) {
        return 1;
    }

    InputFile inputs[AUDIOX_UPDATE_ENTRY_COUNT + 1u] = {
        {AUDIOX_UPDATE_METADATA, meta_path, 0, 0},
        {AUDIOX_UPDATE_CONFIG, config_path, 0, 0},
        {AUDIOX_UPDATE_BOOTLOADER, argv[argi + 2], 0, 0},
        {AUDIOX_UPDATE_KERNEL, kernel_path, 0, 0},
        {AUDIOX_UPDATE_PROGRAM, argv[argi + 4], 0, 0},
        {AUDIOX_UPDATE_CMDLINE, cmdline_path, 0, 0},
    };
    const uint32_t input_count = cmdline_path ? AUDIOX_UPDATE_ENTRY_COUNT + 1u : AUDIOX_UPDATE_ENTRY_COUNT;
    for (uint32_t i = 0; i < input_count; ++i) {
        InputFile &input = inputs[i];
        if (scanFile(input) < 0) {
            unlink(meta_path);
            return 1;
        }
    }

    char temp_path[4096];
    n = snprintf(temp_path, sizeof(temp_path), "%s.tmp", output);
    if (n <= 0 || (size_t)n >= sizeof(temp_path)) {
        fprintf(stderr, "output path is too long\n");
        unlink(meta_path);
        return 1;
    }

    FILE *out = fopen(temp_path, "wb");
    if (!out) {
        fprintf(stderr, "failed to create %s: %s\n", temp_path, strerror(errno));
        unlink(meta_path);
        return 1;
    }

    uint8_t header[AUDIOX_UPDATE_HEADER_SIZE];
    memcpy(header, AUDIOX_UPDATE_MAGIC, AUDIOX_UPDATE_MAGIC_SIZE);
    audioxUpdatePutU32(header + 8, AUDIOX_UPDATE_VERSION);
    audioxUpdatePutU32(header + 12, input_count);
    int failed = writeAll(out, header, sizeof(header));
    for (uint32_t i = 0; i < input_count; ++i) {
        if (!failed && appendFile(out, inputs[i]) < 0) {
            failed = 1;
        }
    }
    unlink(meta_path);
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
    if (rename(temp_path, output) != 0) {
        fprintf(stderr, "failed to finalize %s: %s\n", output, strerror(errno));
        unlink(temp_path);
        return 1;
    }

    printf("Created update archive %s (board=%s, kernel=%s%s)\n", output, parsed.board, parsed.kernel,
           cmdline_path ? ", with cmdline.txt" : "");
    return 0;
}
