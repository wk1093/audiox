#ifndef MIDI_INPUT_H
#define MIDI_INPUT_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct midi_ctx {
    int fd;
    char dev_path[256];
    int connected;
    uint8_t running_status;
    uint8_t data_buf[2];
    int data_have;
    int data_need;
    uint32_t event_seq;
    uint8_t last_status;
    uint8_t last_d0;
    uint8_t last_d1;
} midi_ctx_t;

typedef void (*midi_log_push_fn)(void *ctx, const char *line);

static inline void midi_log_emit(midi_log_push_fn log_push, void *log_ctx, const char *line) {
    if (log_push && line) {
        log_push(log_ctx, line);
    }
}

static inline void midi_ctx_init(midi_ctx_t *midi) {
    memset(midi, 0, sizeof(*midi));
    midi->fd = -1;
}

static inline int midi_consume_event(midi_ctx_t *midi,
                                     uint32_t *cursor,
                                     uint8_t *status,
                                     uint8_t *d0,
                                     uint8_t *d1) {
    if (!midi || !cursor || !status || !d0 || !d1) {
        return 0;
    }

    if (midi->event_seq == 0 || *cursor == midi->event_seq) {
        return 0;
    }

    *cursor = midi->event_seq;
    *status = midi->last_status;
    *d0 = midi->last_d0;
    *d1 = midi->last_d1;
    return 1;
}

static inline int midi_status_data_len(uint8_t status) {
    uint8_t kind = (uint8_t)(status & 0xF0);
    if (kind == 0xC0 || kind == 0xD0) {
        return 1;
    }
    return 2;
}

static inline void midi_format_message(uint8_t status, uint8_t d0, uint8_t d1, char *out, size_t out_len) {
    uint8_t kind = (uint8_t)(status & 0xF0);
    uint8_t ch = (uint8_t)((status & 0x0F) + 1);

    if (kind == 0x80) {
        snprintf(out, out_len, "CH%u NOTE OFF n=%u v=%u", ch, (unsigned)d0, (unsigned)d1);
    } else if (kind == 0x90) {
        if (d1 == 0) {
            snprintf(out, out_len, "CH%u NOTE OFF n=%u v=0", ch, (unsigned)d0);
        } else {
            snprintf(out, out_len, "CH%u NOTE ON  n=%u v=%u", ch, (unsigned)d0, (unsigned)d1);
        }
    } else if (kind == 0xA0) {
        snprintf(out, out_len, "CH%u AFTERTOUCH n=%u p=%u", ch, (unsigned)d0, (unsigned)d1);
    } else if (kind == 0xB0) {
        snprintf(out, out_len, "CH%u CC %u = %u", ch, (unsigned)d0, (unsigned)d1);
    } else if (kind == 0xC0) {
        snprintf(out, out_len, "CH%u PROGRAM %u", ch, (unsigned)d0);
    } else if (kind == 0xD0) {
        snprintf(out, out_len, "CH%u CH PRESSURE %u", ch, (unsigned)d0);
    } else if (kind == 0xE0) {
        int bend = (int)(((uint16_t)d1 << 7) | d0) - 8192;
        snprintf(out, out_len, "CH%u PITCH BEND %d", ch, bend);
    } else {
        snprintf(out, out_len, "MIDI 0x%02X %u %u", (unsigned)status, (unsigned)d0, (unsigned)d1);
    }
}

static inline int midi_parse_byte(midi_ctx_t *midi, uint8_t byte, midi_log_push_fn log_push, void *log_ctx) {
    if (byte >= 0xF8) {
        const char *name = NULL;
        if (byte == 0xF8) return 0;
        if (byte == 0xFA) name = "START";
        if (byte == 0xFB) name = "CONTINUE";
        if (byte == 0xFC) name = "STOP";
        if (name) {
            char line[96];
            snprintf(line, sizeof(line), "REALTIME %s", name);
            midi_log_emit(log_push, log_ctx, line);
            return 1;
        }
        return 0;
    }

    if (byte & 0x80) {
        if (byte >= 0xF0) {
            midi->running_status = 0;
            midi->data_have = 0;
            midi->data_need = 0;
            return 0;
        }

        midi->running_status = byte;
        midi->data_have = 0;
        midi->data_need = midi_status_data_len(byte);
        return 0;
    }

    if (midi->running_status == 0 || midi->data_need <= 0) {
        return 0;
    }

    if (midi->data_have < 2) {
        midi->data_buf[midi->data_have++] = byte;
    }

    if (midi->data_have >= midi->data_need) {
        char line[96];
        uint8_t d0 = midi->data_buf[0];
        uint8_t d1 = (midi->data_need > 1) ? midi->data_buf[1] : 0;
        midi->last_status = midi->running_status;
        midi->last_d0 = d0;
        midi->last_d1 = d1;
        ++midi->event_seq;
        midi_format_message(midi->running_status, d0, d1, line, sizeof(line));
        midi_log_emit(log_push, log_ctx, line);
        midi->data_have = 0;
        return 1;
    }

    return 0;
}

static inline int midi_try_open(midi_ctx_t *midi, midi_log_push_fn log_push, void *log_ctx) {
    DIR *dir = opendir("/dev/snd");
    if (!dir) {
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "midiC", 5) != 0) {
            continue;
        }

        char path[256];
        int n = snprintf(path, sizeof(path), "/dev/snd/%s", entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            continue;
        }

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }

        midi->fd = fd;
        midi->connected = 1;
        midi->running_status = 0;
        midi->data_have = 0;
        midi->data_need = 0;
        snprintf(midi->dev_path, sizeof(midi->dev_path), "%s", path);

        char line[320];
        snprintf(line, sizeof(line), "MIDI CONNECTED %s", path);
        midi_log_emit(log_push, log_ctx, line);

        closedir(dir);
        return 0;
    }

    closedir(dir);
    return -1;
}

static inline void midi_disconnect(midi_ctx_t *midi, midi_log_push_fn log_push, void *log_ctx) {
    if (midi->fd >= 0) {
        close(midi->fd);
    }
    midi->fd = -1;

    if (midi->connected) {
        midi_log_emit(log_push, log_ctx, "MIDI DISCONNECTED");
    }

    midi->connected = 0;
    midi->running_status = 0;
    midi->data_have = 0;
    midi->data_need = 0;
    midi->dev_path[0] = '\0';
}

static inline int midi_poll(midi_ctx_t *midi,
                            midi_log_push_fn log_push,
                            void *log_ctx,
                            int *midi_connected_changed) {
    int updated = 0;
    *midi_connected_changed = 0;

    if (midi->fd < 0) {
        if (midi_try_open(midi, log_push, log_ctx) == 0) {
            *midi_connected_changed = 1;
            return 1;
        }
        return 0;
    }

    uint8_t buf[128];
    while (1) {
        ssize_t n = read(midi->fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; ++i) {
                if (midi_parse_byte(midi, buf[i], log_push, log_ctx)) {
                    updated = 1;
                }
            }
            continue;
        }

        if (n == 0) {
            midi_disconnect(midi, log_push, log_ctx);
            *midi_connected_changed = 1;
            return 1;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        midi_disconnect(midi, log_push, log_ctx);
        *midi_connected_changed = 1;
        return 1;
    }

    return updated;
}

#endif
