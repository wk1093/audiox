#ifndef MIDI_INPUT_H
#define MIDI_INPUT_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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

static inline int midi_name_matches_node(const char *name) {
    return (name && strncmp(name, "midiC", 5) == 0) ? 1 : 0;
}

static inline int midi_card_index_from_name(const char *name) {
    if (!midi_name_matches_node(name)) {
        return -1;
    }

    const char *start = name + 5;
    const char *end = strchr(start, 'D');
    if (!end || end == start) {
        return -1;
    }

    int value = 0;
    for (const char *p = start; p < end; ++p) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        value = (value * 10) + (*p - '0');
    }
    return value;
}

static inline int midi_device_is_usb(const char *node_name) {
    if (!node_name || !node_name[0]) {
        return 0;
    }

    char sysfs_path[PATH_MAX];
    char resolved[PATH_MAX];
    int n = snprintf(sysfs_path, sizeof(sysfs_path), "/sys/class/sound/%s/device", node_name);
    if (n <= 0 || (size_t)n >= sizeof(sysfs_path)) {
        return 0;
    }

    if (!realpath(sysfs_path, resolved)) {
        return 0;
    }

    return strstr(resolved, "/usb") ? 1 : 0;
}

typedef struct midi_candidate {
    char path[256];
    int card_index;
    int is_usb;
} midi_candidate_t;

static inline int midi_candidate_cmp(const void *a, const void *b) {
    const midi_candidate_t *ma = (const midi_candidate_t *)a;
    const midi_candidate_t *mb = (const midi_candidate_t *)b;

    int a_rank = ma->is_usb ? 0 : 1;
    int b_rank = mb->is_usb ? 0 : 1;
    if (a_rank != b_rank) {
        return a_rank - b_rank;
    }
    if (ma->card_index != mb->card_index) {
        return ma->card_index - mb->card_index;
    }
    return strcmp(ma->path, mb->path);
}

static inline size_t midi_collect_candidates(midi_candidate_t *out, size_t cap) {
    DIR *dir = opendir("/dev/snd");
    if (!dir) {
        return 0;
    }

    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < cap) {
        if (!midi_name_matches_node(entry->d_name)) {
            continue;
        }

        int card_index = midi_card_index_from_name(entry->d_name);
        if (card_index < 0) {
            continue;
        }

        int n = snprintf(out[count].path, sizeof(out[count].path), "/dev/snd/%s", entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(out[count].path)) {
            continue;
        }

        out[count].card_index = card_index;
        out[count].is_usb = midi_device_is_usb(entry->d_name);
        ++count;
    }

    closedir(dir);

    if (count > 1) {
        qsort(out, count, sizeof(out[0]), midi_candidate_cmp);
    }
    return count;
}

static inline int midi_device_present(const midi_ctx_t *midi) {
    if (!midi || midi->dev_path[0] == '\0') {
        return 0;
    }
    return access(midi->dev_path, F_OK) == 0 ? 1 : 0;
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
    midi_candidate_t candidates[32];
    size_t count = midi_collect_candidates(candidates, 32);
    int saw_usb = 0;

    for (size_t i = 0; i < count; ++i) {
        if (candidates[i].is_usb) {
            saw_usb = 1;
            break;
        }
    }

    for (size_t i = 0; i < count; ++i) {
        if (saw_usb && !candidates[i].is_usb) {
            continue;
        }

        int fd = open(candidates[i].path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }

        midi->fd = fd;
        midi->connected = 1;
        midi->running_status = 0;
        midi->data_have = 0;
        midi->data_need = 0;
        snprintf(midi->dev_path, sizeof(midi->dev_path), "%s", candidates[i].path);

        char line[320];
        snprintf(line,
                 sizeof(line),
                 "MIDI CONNECTED %s%s",
                 candidates[i].path,
                 candidates[i].is_usb ? " (USB)" : "");
        midi_log_emit(log_push, log_ctx, line);
        return 0;
    }

    if (!saw_usb && count > 0) {
        midi_log_emit(log_push, log_ctx, "MIDI: no USB MIDI device found yet");
    }

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

    if (!midi_device_present(midi)) {
        midi_disconnect(midi, log_push, log_ctx);
        *midi_connected_changed = 1;
        return 1;
    }

    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = midi->fd;
    pfd.events = POLLIN | POLLERR | POLLHUP;
    int prc = poll(&pfd, 1, 0);
    if (prc > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        midi_disconnect(midi, log_push, log_ctx);
        *midi_connected_changed = 1;
        return 1;
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
