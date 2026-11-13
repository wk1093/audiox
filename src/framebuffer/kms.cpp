#include "framebuffer/context.hpp"

#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <new>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

// Software rendering cost grows with the pixel count; larger displays get the
// biggest mode up to this size (e.g. 1080p on a 4K HDMI monitor).
static constexpr uint64_t kMaxKmsPixels = 1920ULL * 1200ULL;
static constexpr int kMaxDrmCards = 8;

namespace {

struct KmsConnector {
    drm_mode_get_connector info{};
    std::vector<drm_mode_modeinfo> modes;
    std::vector<uint32_t> encoders;
};

struct KmsResources {
    std::vector<uint32_t> crtcs;
    std::vector<uint32_t> connectors;
};

struct KmsSelection {
    int fd{-1};
    int card{-1};
    int rank{-1};
    KmsConnector connector;
    drm_mode_modeinfo mode{};
    uint32_t crtcId{0};
};

int drmIoctl(int fd, unsigned long request, void *arg) {
    int rc;
    do {
        rc = ioctl(fd, request, arg);
    } while (rc < 0 && (errno == EINTR || errno == EAGAIN));
    return rc;
}

uint64_t ptrValue(const void *ptr) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
}

const char *connectorTypeName(uint32_t type) {
    switch (type) {
        case DRM_MODE_CONNECTOR_VGA: return "VGA";
        case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
        case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
        case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
        case DRM_MODE_CONNECTOR_Composite: return "Composite";
        case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
        case DRM_MODE_CONNECTOR_Component: return "Component";
        case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
        case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
        case DRM_MODE_CONNECTOR_TV: return "TV";
        case DRM_MODE_CONNECTOR_eDP: return "eDP";
        case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
        case DRM_MODE_CONNECTOR_DSI: return "DSI";
        case DRM_MODE_CONNECTOR_DPI: return "DPI";
        case DRM_MODE_CONNECTOR_SPI: return "SPI";
        case DRM_MODE_CONNECTOR_USB: return "USB";
        default: return "Unknown";
    }
}

// Built-in panels first (a DSI/DPI/SPI touchscreen is the UI's home), then
// external monitors, then analog TV outputs. Writeback is never a display.
int connectorRank(uint32_t type) {
    switch (type) {
        case DRM_MODE_CONNECTOR_DSI:
        case DRM_MODE_CONNECTOR_DPI:
        case DRM_MODE_CONNECTOR_LVDS:
        case DRM_MODE_CONNECTOR_eDP:
        case DRM_MODE_CONNECTOR_SPI:
            return 4;
        case DRM_MODE_CONNECTOR_HDMIA:
        case DRM_MODE_CONNECTOR_HDMIB:
        case DRM_MODE_CONNECTOR_DisplayPort:
        case DRM_MODE_CONNECTOR_DVII:
        case DRM_MODE_CONNECTOR_DVID:
        case DRM_MODE_CONNECTOR_DVIA:
        case DRM_MODE_CONNECTOR_VGA:
        case DRM_MODE_CONNECTOR_USB:
        case DRM_MODE_CONNECTOR_VIRTUAL:
            return 3;
        case DRM_MODE_CONNECTOR_WRITEBACK:
            return -1;
        default:
            return 1;
    }
}

bool getResources(int fd, KmsResources &out) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        drm_mode_card_res res{};
        if (drmIoctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) return false;
        if (res.count_crtcs == 0 || res.count_connectors == 0) return false;
        out.crtcs.assign(res.count_crtcs, 0);
        out.connectors.assign(res.count_connectors, 0);
        drm_mode_card_res fill{};
        fill.count_crtcs = res.count_crtcs;
        fill.crtc_id_ptr = ptrValue(out.crtcs.data());
        fill.count_connectors = res.count_connectors;
        fill.connector_id_ptr = ptrValue(out.connectors.data());
        if (drmIoctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &fill) < 0) return false;
        if (fill.count_crtcs == res.count_crtcs && fill.count_connectors == res.count_connectors) return true;
    }
    return false;
}

// probe=true asks the driver to re-detect the display and re-read its modes
// (EDID); otherwise the kernel's cached state is returned without touching hardware.
bool getConnector(int fd, uint32_t id, bool probe, KmsConnector &out) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        drm_mode_modeinfo scratch{};
        drm_mode_get_connector query{};
        query.connector_id = id;
        if (!probe || attempt > 0) {
            query.count_modes = 1;
            query.modes_ptr = ptrValue(&scratch);
        }
        if (drmIoctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &query) < 0) return false;
        out.modes.assign(query.count_modes, drm_mode_modeinfo{});
        out.encoders.assign(query.count_encoders, 0);
        drm_mode_get_connector fill{};
        fill.connector_id = id;
        fill.count_modes = query.count_modes;
        fill.modes_ptr = ptrValue(out.modes.empty() ? &scratch : out.modes.data());
        fill.count_encoders = query.count_encoders;
        fill.encoders_ptr = ptrValue(out.encoders.data());
        if (query.count_modes == 0) {
            // A zero mode count would trigger another probe; one mode slot avoids it.
            fill.count_modes = 1;
        }
        if (drmIoctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &fill) < 0) return false;
        if (fill.count_modes == query.count_modes && fill.count_encoders == query.count_encoders) {
            out.info = fill;
            return true;
        }
    }
    return false;
}

uint64_t modePixels(const drm_mode_modeinfo &mode) {
    return static_cast<uint64_t>(mode.hdisplay) * mode.vdisplay;
}

// The preferred mode when it is small enough; otherwise the largest mode within
// the limit, keeping the preferred aspect ratio and the highest refresh rate.
int chooseMode(const std::vector<drm_mode_modeinfo> &modes) {
    if (modes.empty()) return -1;
    int preferred = 0;
    for (size_t i = 0; i < modes.size(); ++i) {
        if (modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            preferred = static_cast<int>(i);
            break;
        }
    }
    const drm_mode_modeinfo &want = modes[preferred];
    if (modePixels(want) <= kMaxKmsPixels && want.hdisplay && want.vdisplay) return preferred;
    int best = -1;
    for (size_t i = 0; i < modes.size(); ++i) {
        const drm_mode_modeinfo &mode = modes[i];
        if (!mode.hdisplay || !mode.vdisplay || modePixels(mode) > kMaxKmsPixels) continue;
        if (mode.flags & DRM_MODE_FLAG_INTERLACE) continue;
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        const drm_mode_modeinfo &current = modes[best];
        bool aspect = (uint64_t)mode.hdisplay * want.vdisplay == (uint64_t)mode.vdisplay * want.hdisplay;
        bool currentAspect = (uint64_t)current.hdisplay * want.vdisplay == (uint64_t)current.vdisplay * want.hdisplay;
        if (aspect != currentAspect) {
            if (aspect) best = static_cast<int>(i);
            continue;
        }
        if (modePixels(mode) != modePixels(current)) {
            if (modePixels(mode) > modePixels(current)) best = static_cast<int>(i);
            continue;
        }
        if (mode.vrefresh > current.vrefresh) best = static_cast<int>(i);
    }
    return best >= 0 ? best : preferred;
}

uint32_t findCrtc(int fd, const KmsResources &res, const KmsConnector &connector) {
    std::vector<uint32_t> encoders;
    if (connector.info.encoder_id) encoders.push_back(connector.info.encoder_id);
    for (uint32_t id : connector.encoders) encoders.push_back(id);
    for (uint32_t id : encoders) {
        drm_mode_get_encoder encoder{};
        encoder.encoder_id = id;
        if (drmIoctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) < 0) continue;
        if (encoder.crtc_id) return encoder.crtc_id;
        for (size_t i = 0; i < res.crtcs.size() && i < 32; ++i) {
            if (encoder.possible_crtcs & (1U << i)) return res.crtcs[i];
        }
    }
    return 0;
}

bool sameMode(const drm_mode_modeinfo &a, const drm_mode_modeinfo &b) {
    return a.hdisplay == b.hdisplay && a.vdisplay == b.vdisplay && a.vrefresh == b.vrefresh &&
           a.clock == b.clock;
}

// Finds the best connected output over all DRM cards.
bool selectOutput(bool probe, KmsSelection &best, bool &idleCard) {
    idleCard = false;
    for (int card = 0; card < kMaxDrmCards; ++card) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", card);
        int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) continue;
        drm_get_cap cap{};
        cap.capability = DRM_CAP_DUMB_BUFFER;
        KmsResources res;
        bool usable = drmIoctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value && getResources(fd, res);
        bool chosen = false;
        if (usable) {
            bool connected = false;
            for (uint32_t id : res.connectors) {
                KmsConnector connector;
                if (!getConnector(fd, id, probe, connector)) continue;
                int rank = connectorRank(connector.info.connector_type);
                if (connector.info.connection != 1 || rank < 0 || connector.modes.empty()) continue;
                connected = true;
                if (rank <= best.rank) continue;
                int mode = chooseMode(connector.modes);
                uint32_t crtc = mode >= 0 ? findCrtc(fd, res, connector) : 0;
                if (!crtc) continue;
                if (best.fd >= 0 && best.fd != fd) close(best.fd);
                best.fd = fd;
                best.card = card;
                best.rank = rank;
                best.connector = connector;
                best.mode = connector.modes[mode];
                best.crtcId = crtc;
                chosen = true;
            }
            if (!connected) idleCard = true;
        }
        if (!chosen && fd != best.fd) close(fd);
    }
    return best.fd >= 0;
}

bool createBuffer(KmsDisplay &kms, int index, uint32_t width, uint32_t height, uint32_t &pitch, uint64_t &size) {
    drm_mode_create_dumb create{};
    create.width = width;
    create.height = height;
    create.bpp = 32;
    if (drmIoctl(kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) return false;
    kms.handles[index] = create.handle;
    pitch = create.pitch;
    size = create.size;

    drm_mode_fb_cmd2 fb2{};
    fb2.width = width;
    fb2.height = height;
    fb2.pixel_format = DRM_FORMAT_XRGB8888;
    fb2.handles[0] = create.handle;
    fb2.pitches[0] = create.pitch;
    if (drmIoctl(kms.fd, DRM_IOCTL_MODE_ADDFB2, &fb2) == 0) {
        kms.fbIds[index] = fb2.fb_id;
    } else {
        drm_mode_fb_cmd legacy{};
        legacy.width = width;
        legacy.height = height;
        legacy.pitch = create.pitch;
        legacy.bpp = 32;
        legacy.depth = 24;
        legacy.handle = create.handle;
        if (drmIoctl(kms.fd, DRM_IOCTL_MODE_ADDFB, &legacy) < 0) return false;
        kms.fbIds[index] = legacy.fb_id;
    }

    drm_mode_map_dumb map{};
    map.handle = create.handle;
    if (drmIoctl(kms.fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) return false;
    void *memory = mmap(nullptr, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, kms.fd, (off_t)map.offset);
    if (memory == MAP_FAILED) return false;
    kms.maps[index] = static_cast<uint8_t *>(memory);
    memset(memory, 0, create.size);
    return true;
}

bool setCrtc(KmsDisplay &kms, uint32_t fbId) {
    drm_mode_crtc crtc{};
    crtc.crtc_id = kms.crtcId;
    crtc.fb_id = fbId;
    crtc.set_connectors_ptr = ptrValue(&kms.connectorId);
    crtc.count_connectors = 1;
    crtc.mode = kms.mode;
    crtc.mode_valid = 1;
    return drmIoctl(kms.fd, DRM_IOCTL_MODE_SETCRTC, &crtc) == 0;
}

}  // namespace

void FramebufferContext::releaseKms() {
    for (int i = 0; i < 2; ++i) {
        if (kms.maps[i]) munmap(kms.maps[i], kms.mapSize);
        kms.maps[i] = nullptr;
        if (kms.fd >= 0 && kms.fbIds[i]) {
            uint32_t id = kms.fbIds[i];
            (void)drmIoctl(kms.fd, DRM_IOCTL_MODE_RMFB, &id);
        }
        kms.fbIds[i] = 0;
        if (kms.fd >= 0 && kms.handles[i]) {
            drm_mode_destroy_dumb destroy{};
            destroy.handle = kms.handles[i];
            (void)drmIoctl(kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        }
        kms.handles[i] = 0;
    }
    kms.mapSize = 0;
    kms.flipPending = false;
    if (kms.fd >= 0) close(kms.fd);
    kms.fd = -1;
    kms.card = -1;
    kms.crtcId = 0;
    kms.connectorId = 0;
}

bool FramebufferContext::initKms(bool probe) {
    KmsSelection selection;
    bool idleCard = false;
    if (!selectOutput(probe, selection, idleCard)) {
        kms.idleCard = idleCard;
        return false;
    }
    kms.idleCard = false;
    kms.fd = selection.fd;
    kms.card = selection.card;
    kms.connectorId = selection.connector.info.connector_id;
    kms.connectorType = selection.connector.info.connector_type;
    kms.crtcId = selection.crtcId;
    kms.mode = selection.mode;
    snprintf(kms.name, sizeof(kms.name), "%s-%u", connectorTypeName(kms.connectorType),
             selection.connector.info.connector_type_id);

    const uint32_t w = kms.mode.hdisplay, h = kms.mode.vdisplay;
    uint32_t pitch[2]{};
    uint64_t size[2]{};
    bool ok = createBuffer(kms, 0, w, h, pitch[0], size[0]) && createBuffer(kms, 1, w, h, pitch[1], size[1]) &&
              pitch[0] == pitch[1] && size[0] == size[1];
    kms.mapSize = (size_t)size[0];
    const char *step = "buffer allocation";
    if (ok) {
        step = "mode set";
        ok = setCrtc(kms, kms.fbIds[0]);
    }
    uint8_t *staging = nullptr;
    if (ok) {
        step = "staging allocation";
        staging = new (std::nothrow) uint8_t[(size_t)pitch[0] * h];
        ok = staging != nullptr;
    }
    if (!ok) {
        if (!kms.setupWarned) {
            printf("[FBUI] [WARN] KMS %s on card%d: %s failed: %s\n", kms.name, kms.card, step, strerror(errno));
            kms.setupWarned = true;
        }
        releaseKms();
        return false;
    }
    kms.setupWarned = false;
    kms.flipTimeoutWarned = false;

    backend = DisplayBackend::Kms;
    fd = kms.fd;
    buffer = kms.maps[0];
    bufferSize = kms.mapSize;
    stagingBuffer = staging;
    width = w;
    height = h;
    bpp = 32;
    line_length = pitch[0];
    page_len = (size_t)pitch[0] * h;
    stagingBufferSize = page_len;
    front_page = 0;
    draw_page = 1;
    draw_offset = 0;
    pageflip_enabled = 1;
    has_orig_vinfo = 0;
    snprintf(dev_path, sizeof(dev_path), "/dev/dri/card%d", kms.card);
    memset(staging, 0, page_len);
    printf("[FBUI] KMS display %s on card%d initialized: %ux%u@%uHz\n", kms.name, kms.card, w, h,
           kms.mode.vrefresh);
    return true;
}

bool FramebufferContext::kmsOutputValid(bool probe) {
    KmsConnector connector;
    if (!getConnector(kms.fd, kms.connectorId, probe, connector)) return false;
    if (connector.info.connection != 1 || connector.modes.empty()) return false;
    int mode = chooseMode(connector.modes);
    return mode >= 0 && sameMode(connector.modes[mode], kms.mode);
}

bool FramebufferContext::kmsDisplayAvailable() {
    KmsSelection selection;
    bool idleCard = false;
    bool found = selectOutput(true, selection, idleCard);
    if (selection.fd >= 0) close(selection.fd);
    kms.idleCard = !found && idleCard;
    return found;
}

void FramebufferContext::kmsWaitFlip(int timeoutMs) {
    while (kms.flipPending) {
        pollfd pfd{};
        pfd.fd = kms.fd;
        pfd.events = POLLIN;
        int ready = ::poll(&pfd, 1, timeoutMs);
        if (ready <= 0) {
            if (ready < 0 && errno == EINTR) continue;
            // A lost event must not stall the UI; the next flip resynchronises.
            if (!kms.flipTimeoutWarned) {
                printf("[FBUI] [WARN] Page flip on %s did not complete within %d ms\n", kms.name, timeoutMs);
                kms.flipTimeoutWarned = true;
            }
            kms.flipPending = false;
            return;
        }
        char events[1024];
        ssize_t count = read(kms.fd, events, sizeof(events));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            kms.flipPending = false;
            return;
        }
        for (ssize_t offset = 0; offset + (ssize_t)sizeof(drm_event) <= count;) {
            drm_event event{};
            memcpy(&event, events + offset, sizeof(event));
            if (event.length < sizeof(drm_event)) break;
            if (event.type == DRM_EVENT_FLIP_COMPLETE) kms.flipPending = false;
            offset += event.length;
        }
    }
}

bool FramebufferContext::kmsPresent() {
    if (kms.flipPending) kmsWaitFlip(100);
    const uint32_t next = (front_page ^ 1U) & 1U;
    memcpy(kms.maps[next], stagingBuffer, page_len);
    drm_mode_crtc_page_flip flip{};
    flip.crtc_id = kms.crtcId;
    flip.fb_id = kms.fbIds[next];
    flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
    if (drmIoctl(kms.fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0) {
        kms.flipPending = true;
        front_page = next;
        return true;
    }
    int error = errno;
    if (error == EBUSY) {
        kms.flipPending = true;
        kmsWaitFlip(100);
        if (drmIoctl(kms.fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0) {
            kms.flipPending = true;
            front_page = next;
            return true;
        }
        error = errno;
    }
    if (error == ENODEV || error == ENXIO || error == EIO || error == EACCES || error == EPERM) {
        printf("[FBUI] [WARN] Display %s lost (page flip: %s); releasing it.\n", kms.name, strerror(error));
        release();
        return false;
    }
    // Some drivers cannot flip (or the CRTC was turned off); a mode set always works.
    if (setCrtc(kms, kms.fbIds[next])) {
        front_page = next;
        return true;
    }
    printf("[FBUI] [WARN] Display %s update failed (%s); releasing it.\n", kms.name, strerror(errno));
    release();
    return false;
}
