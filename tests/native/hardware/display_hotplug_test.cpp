#include "framebuffer/context.hpp"
#include "touch/context.hpp"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <new>
#include <poll.h>
#include <vector>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {
uint64_t nowMs = 0;
bool available = true;
bool connected = true;
bool hasDrm = true;
bool nestedDrm = false;
bool mapFailure = false;
bool touchGone = false;
bool queryFailure = false;
int panError = 0;
unsigned opens = 0, queries = 0, connectorScans = 0, maps = 0, unmaps = 0, closes = 0;
unsigned touchOpens = 0, touchCloses = 0;
unsigned heldStops = 0;
ino_t deviceInode = 1;
char statusPath[] = "/tmp/audiox-display-status-XXXXXX";
fb_var_screeninfo mode{};
fb_fix_screeninfo layout{};

// Fake DRM card0 (fd 500): one DSI connector (41) on CRTC 31, 80x48 preferred.
constexpr int kDrmFd = 500;
bool drmPresent = false;
bool drmConnected = true;
bool drmFlipEvent = false;
int drmFlipError = 0;
unsigned drmOpens = 0, drmCloses = 0, drmProbes = 0, drmModesets = 0, drmFlips = 0;
unsigned drmMaps = 0, drmUnmaps = 0, drmBuffers = 0, drmDestroyed = 0;
uint32_t drmScanout = 0, drmNextHandle = 0;
bool touchDirect = true;
bool touchNoProps = false;
std::vector<void *> drmMemory;

drm_mode_modeinfo drmMode() {
    drm_mode_modeinfo info{};
    info.hdisplay = 80;
    info.vdisplay = 48;
    info.vrefresh = 60;
    info.clock = 1000;
    info.type = DRM_MODE_TYPE_PREFERRED;
    snprintf(info.name, sizeof(info.name), "80x48");
    return info;
}

int fakeDrmIoctl(unsigned long request, void *data) {
    if (request == DRM_IOCTL_GET_CAP) {
        static_cast<drm_get_cap *>(data)->value = 1;
    } else if (request == DRM_IOCTL_MODE_GETRESOURCES) {
        auto *res = static_cast<drm_mode_card_res *>(data);
        if (res->count_crtcs >= 1 && res->crtc_id_ptr) *reinterpret_cast<uint32_t *>(res->crtc_id_ptr) = 31;
        if (res->count_connectors >= 1 && res->connector_id_ptr) {
            *reinterpret_cast<uint32_t *>(res->connector_id_ptr) = 41;
        }
        res->count_crtcs = res->count_connectors = res->count_encoders = 1;
    } else if (request == DRM_IOCTL_MODE_GETCONNECTOR) {
        auto *conn = static_cast<drm_mode_get_connector *>(data);
        assert(conn->connector_id == 41);
        if (conn->count_modes == 0) ++drmProbes;
        uint32_t modes = drmConnected ? 1 : 0;
        if (modes && conn->count_modes >= modes && conn->modes_ptr) {
            *reinterpret_cast<drm_mode_modeinfo *>(conn->modes_ptr) = drmMode();
        }
        if (conn->count_encoders >= 1 && conn->encoders_ptr) *reinterpret_cast<uint32_t *>(conn->encoders_ptr) = 51;
        conn->count_modes = modes;
        conn->count_encoders = 1;
        conn->count_props = 0;
        conn->encoder_id = 51;
        conn->connector_type = DRM_MODE_CONNECTOR_DSI;
        conn->connector_type_id = 1;
        conn->connection = drmConnected ? 1 : 2;
    } else if (request == DRM_IOCTL_MODE_GETENCODER) {
        auto *enc = static_cast<drm_mode_get_encoder *>(data);
        assert(enc->encoder_id == 51);
        enc->crtc_id = 0;
        enc->possible_crtcs = 1;
    } else if (request == DRM_IOCTL_MODE_CREATE_DUMB) {
        auto *create = static_cast<drm_mode_create_dumb *>(data);
        assert(create->bpp == 32);
        create->handle = ++drmNextHandle;
        create->pitch = create->width * 4;
        create->size = (uint64_t)create->pitch * create->height;
        ++drmBuffers;
    } else if (request == DRM_IOCTL_MODE_ADDFB2) {
        auto *fb = static_cast<drm_mode_fb_cmd2 *>(data);
        assert(fb->pixel_format == DRM_FORMAT_XRGB8888);
        fb->fb_id = 1000 + fb->handles[0];
    } else if (request == DRM_IOCTL_MODE_MAP_DUMB) {
        auto *map = static_cast<drm_mode_map_dumb *>(data);
        map->offset = (uint64_t)map->handle << 12;
    } else if (request == DRM_IOCTL_MODE_SETCRTC) {
        auto *crtc = static_cast<drm_mode_crtc *>(data);
        assert(crtc->crtc_id == 31);
        assert(crtc->fb_id && crtc->mode_valid && crtc->count_connectors == 1 &&
               *reinterpret_cast<uint32_t *>(crtc->set_connectors_ptr) == 41);
        ++drmModesets;
        drmScanout = crtc->fb_id;
    } else if (request == DRM_IOCTL_MODE_PAGE_FLIP) {
        auto *flip = static_cast<drm_mode_crtc_page_flip *>(data);
        if (drmFlipError) {
            errno = drmFlipError;
            return -1;
        }
        assert(flip->crtc_id == 31 && (flip->flags & DRM_MODE_PAGE_FLIP_EVENT));
        if (drmFlipEvent) {
            errno = EBUSY;
            return -1;
        }
        drmScanout = flip->fb_id;
        drmFlipEvent = true;
        ++drmFlips;
    } else if (request == DRM_IOCTL_MODE_RMFB) {
        if (*static_cast<uint32_t *>(data) == drmScanout) drmScanout = 0;
    } else if (request == DRM_IOCTL_MODE_DESTROY_DUMB) {
        ++drmDestroyed;
    } else {
        assert(false);
    }
    return 0;
}

int fakePoll(pollfd *fds, nfds_t count, int) {
    assert(count == 1 && fds[0].fd == kDrmFd);
    fds[0].revents = drmFlipEvent ? POLLIN : 0;
    return drmFlipEvent ? 1 : 0;
}

void setStatus(bool value) {
    connected = value;
    FILE *file = fopen(statusPath, "w");
    assert(file);
    assert(fputs(connected ? "connected\n" : "disconnected\n", file) >= 0);
    assert(fclose(file) == 0);
}

int fakeOpen(const char *path, int) {
    if (strncmp(path, "/dev/dri/", 9) == 0) {
        if (drmPresent && strcmp(path, "/dev/dri/card0") == 0) {
            ++drmOpens;
            return kDrmFd;
        }
    } else if (strncmp(path, "/dev/fb", 7) == 0) {
        ++opens;
        if (available && strcmp(path, "/dev/fb0") == 0) return 100;
    } else {
        ++touchOpens;
        if (strcmp(path, "/dev/input/event0") == 0 && !touchGone) return 200;
    }
    errno = ENOENT;
    return -1;
}

int fakeClose(int fd) {
    if (fd == 100) ++closes;
    else if (fd == kDrmFd) ++drmCloses;
    else {
        assert(fd == 200);
        ++touchCloses;
    }
    return 0;
}

int fakeClock(clockid_t, timespec *ts) {
    ts->tv_sec = nowMs / 1000;
    ts->tv_nsec = (nowMs % 1000) * 1000000;
    return 0;
}

int fakeGlob(const char *pattern, int, int (*)(const char *, int), glob_t *paths) {
    ++connectorScans;
    const char *expected = nestedDrm ? "/sys/class/graphics/fb0/device/drm/card*/card*-*/status" :
                                       "/sys/class/graphics/fb0/device/card*-*/status";
    if (!hasDrm || strcmp(pattern, expected) != 0) {
        return GLOB_NOMATCH;
    }
    paths->gl_pathc = 1;
    paths->gl_pathv = static_cast<char **>(calloc(2, sizeof(char *)));
    assert(paths->gl_pathv);
    paths->gl_pathv[0] = strdup(statusPath);
    assert(paths->gl_pathv[0]);
    return 0;
}

void *fakeMap(void *, size_t size, int, int, int fd, off_t) {
    if (fd == kDrmFd) {
        ++drmMaps;
        drmMemory.push_back(new uint8_t[size]);
        return drmMemory.back();
    }
    assert(fd == 100 && size == layout.smem_len);
    if (mapFailure) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    ++maps;
    return new uint8_t[size];
}

int fakeUnmap(void *memory, size_t) {
    bool drm = false;
    for (size_t i = 0; i < drmMemory.size(); ++i) {
        if (drmMemory[i] == memory) {
            drmMemory.erase(drmMemory.begin() + (long)i);
            drm = true;
            break;
        }
    }
    if (drm) ++drmUnmaps;
    else ++unmaps;
    delete[] static_cast<uint8_t *>(memory);
    return 0;
}

int fakeStat(const char *, struct stat *info) {
    *info = {};
    info->st_ino = deviceInode;
    return available ? 0 : -1;
}

int fakeFstat(int, struct stat *info) {
    *info = {};
    info->st_ino = 1;
    return 0;
}

int fakeIoctl(int fd, unsigned long request, void *data) {
    if (fd == kDrmFd) return fakeDrmIoctl(request, data);
    ++queries;
    if (fd == 100) {
        if (queryFailure) {
            errno = ENODEV;
            return -1;
        }
        if (request == FBIOGET_VSCREENINFO) *static_cast<fb_var_screeninfo *>(data) = mode;
        else if (request == FBIOGET_FSCREENINFO) *static_cast<fb_fix_screeninfo *>(data) = layout;
        else if (request == FBIOPUT_VSCREENINFO) {
            mode = *static_cast<fb_var_screeninfo *>(data);
            layout.smem_len = layout.line_length * mode.yres_virtual;
        } else if (request == FBIOPAN_DISPLAY) {
            if (panError) {
                errno = panError;
                return -1;
            }
            mode = *static_cast<fb_var_screeninfo *>(data);
        } else assert(false);
        return 0;
    }
    assert(fd == 200);
    if (request == EVIOCGABS(ABS_X) || request == EVIOCGABS(ABS_Y) ||
        request == EVIOCGABS(ABS_MT_POSITION_X) || request == EVIOCGABS(ABS_MT_POSITION_Y)) {
        auto *axis = static_cast<input_absinfo *>(data);
        *axis = {};
        axis->maximum = 4095;
    } else if (request == EVIOCGABS(ABS_MT_SLOT)) {
        auto *axis = static_cast<input_absinfo *>(data);
        *axis = {};
        axis->maximum = 4;
    } else if (request == EVIOCGBIT(EV_KEY, _IOC_SIZE(request))) {
        memset(data, 0, _IOC_SIZE(request));
        auto *bits = static_cast<unsigned long *>(data);
        bits[BTN_TOUCH / (8 * sizeof(unsigned long))] |= 1UL << (BTN_TOUCH % (8 * sizeof(unsigned long)));
        return (int)_IOC_SIZE(request); // evdev returns the copied length.
    } else if (_IOC_NR(request) >= _IOC_NR(EVIOCGBIT(0, 0)) &&
               _IOC_NR(request) <= _IOC_NR(EVIOCGBIT(EV_MAX, 0))) {
        memset(data, 0xff, _IOC_SIZE(request));
        return (int)_IOC_SIZE(request);
    } else if (request == EVIOCGPROP(_IOC_SIZE(request))) {
        memset(data, 0, _IOC_SIZE(request));
        if (touchNoProps) return (int)_IOC_SIZE(request);
        auto *bits = static_cast<unsigned long *>(data);
        bits[0] |= 1UL << (touchDirect ? INPUT_PROP_DIRECT : INPUT_PROP_POINTER);
    } else assert(false);
    return 0;
}

ssize_t fakeRead(int fd, void *data, size_t size) {
    if (fd == kDrmFd) {
        if (!drmFlipEvent) {
            errno = EAGAIN;
            return -1;
        }
        drm_event_vblank event{};
        event.base.type = DRM_EVENT_FLIP_COMPLETE;
        event.base.length = sizeof(event);
        assert(size >= sizeof(event));
        memcpy(data, &event, sizeof(event));
        drmFlipEvent = false;
        return sizeof(event);
    }
    assert(fd == 200);
    errno = touchGone ? ENODEV : EAGAIN;
    return -1;
}
}

#define open(...) fakeOpen(__VA_ARGS__)
#define close(...) fakeClose(__VA_ARGS__)
#define clock_gettime(...) fakeClock(__VA_ARGS__)
#define glob(...) fakeGlob(__VA_ARGS__)
#define mmap(...) fakeMap(__VA_ARGS__)
#define munmap(...) fakeUnmap(__VA_ARGS__)
#define stat(...) fakeStat(__VA_ARGS__)
#define fstat(...) fakeFstat(__VA_ARGS__)
#define ioctl(...) fakeIoctl(__VA_ARGS__)
#define read(...) fakeRead(__VA_ARGS__)
#include "../../../src/framebuffer/hardware.cpp"
#include "../../../src/framebuffer/drawing.cpp"
#include "../../../src/touch/touch.cpp"
#define poll(...) fakePoll(__VA_ARGS__)
#include "../../../src/framebuffer/kms.cpp"
#undef poll
#undef open
#undef close
#undef clock_gettime
#undef glob
#undef mmap
#undef munmap
#undef stat
#undef fstat
#undef ioctl
#undef read
#undef write

#include "../../../src/framebuffer/ui.cpp"

AudioContext::AudioContext(Audiox *context) : app(context) { app->audio = this; }
AudioContext::~AudioContext() { app->audio = nullptr; }
void AudioContext::stopHeldSfx() { ++heldStops; }
static char lastMeterId[BT_ROUTE_ID_MAX];
float AudioContext::getGraphThingChannelLevel(const char *id, int channel) const {
    snprintf(lastMeterId, sizeof(lastMeterId), "%s", id);
    return channel == 0 ? 0.25f : 0.5f;
}
float AudioContext::getChannelLevel(AudioHandle handle, int channel, bool capture) const {
    assert(handle == 42 && channel == 1 && capture);
    return 0.75f;
}

int main() {
    int statusFd = mkstemp(statusPath);
    assert(statusFd >= 0);
    close(statusFd);
    setStatus(true);
    mode.xres = 80;
    mode.yres = mode.yres_virtual = 48;
    mode.bits_per_pixel = 32;
    layout.line_length = 320;
    layout.smem_len = 320 * 48;
    {
        Audiox app;
        AudioContext audio(&app);
        FramebufferContext fb(&app);
        const uint64_t phone = kBluetoothMeterKey | 0xA4C6F0A57AA7ULL;
        const uint64_t headphones = kBluetoothMeterKey | 0xB8841102C99CULL;
        assert(getAudioChannelLevel(&fb, phone, 0, 1) == 0.25f);
        assert(strcmp(lastMeterId, "bt_A4C6F0A57AA7_in") == 0);
        assert(getAudioChannelLevel(&fb, headphones, 1, 0) == 0.5f);
        assert(strcmp(lastMeterId, "bt_B8841102C99C_out") == 0);
        assert(getAudioChannelLevel(&fb, headphones, 1, 1) == 0.5f);
        assert(strcmp(lastMeterId, "bt_B8841102C99C_in") == 0);
        assert(getAudioChannelLevel(&fb, 42, 1, 1) == 0.75f);
        assert(smoothMeterLevel(phone, 0, 1, 0.25f, 1) == 0.25f);
        assert(smoothMeterLevel(headphones, 0, 1, 0.75f, 1) == 0.75f);
        assert(smoothMeterLevel(headphones, 0, 0, 0.5f, 1) == 0.5f);
        assert(findMeterSmoothingState(phone, 0, 1, 1)->level == 0.25f);
        assert(audioMeterFraction(0.0f) == 0.0f);
        assert(audioMeterFraction(-1.0f) == 0.0f);
        assert(audioMeterFraction(0.0001f) == 0.0f);
        assert(audioMeterFraction(0.001f) == 0.0f);
        assert(fabsf(audioMeterFraction(0.01f) - 1.0f / 3.0f) < 0.0001f);
        assert(fabsf(audioMeterFraction(0.1f) - 2.0f / 3.0f) < 0.0001f);
        assert(fabsf(audioMeterFraction(0.5f) - 0.899657f) < 0.0001f);
        assert(audioMeterFraction(1.0f) == 1.0f);
        assert(audioMeterFraction(2.0f) == 1.0f);
        assert(audioMeterFraction(NAN) == 0.0f);
        assert(audioMeterFraction(INFINITY) == 0.0f);
        assert(std::isfinite(smoothMeterLevel(phone, 0, 1, NAN, 2)));
        TouchContext touch(&app);
        assert(app.fb == &fb && app.touch == &touch);
        assert(fb.isReady() && fb.pageflip_enabled && maps == 1);
        assert(fb.nextProbeMs == nowMs + 500);
        assert(touchOpens == 0);
        touch.checkForDevice();
        assert(touch.isReady() && touch.has_range && touch.max_slots == 5);
        assert(touch.nextProbeMs == nowMs + 500);
        unsigned initialQueries = queries, initialOpens = opens, initialScans = connectorScans;
        for (nowMs = 1; nowMs < 500; ++nowMs) fb.checkForFramebuffer();
        assert(queries == initialQueries && opens == initialOpens && connectorScans == initialScans);
        fb.checkForFramebuffer();
        assert(fb.isReady() && maps == 1 && queries == initialQueries + 2);

        soundboardUi.wasDown = true;
        soundboardUi.held = true;
        soundboardUi.pressedPad = 2;
        deviceListScrollState.tracking = true;
        buttonStates[0].tracking = true;
        uiSleepEnabled = uiSleepFrameDrawn = true;
        touch.state.is_pressed = true;
        setStatus(false);
        nowMs = 1000;
        fb.checkForFramebuffer();
        assert(!fb.isReady() && !fb.buffer && !fb.stagingBuffer && fb.bufferSize == 0);
        assert(unmaps == 1 && !touch.isReady() && !touch.state.is_pressed && touchCloses == 1);
        assert(!soundboardUi.wasDown && soundboardUi.pressedPad == -1);
        assert(!soundboardUi.held && heldStops == 1);
        assert(!deviceListScrollState.tracking && !buttonStates[0].tracking && !uiSleepEnabled);

        initialOpens = opens;
        initialScans = connectorScans;
        for (nowMs = 1001; nowMs < 1500; ++nowMs) fb.checkForFramebuffer();
        assert(opens == initialOpens && connectorScans == initialScans);
        setStatus(true);
        fb.checkForFramebuffer();
        assert(fb.isReady() && maps == 2 && !soundboardUi.wasDown);
        assert(heldStops == 1);
        touch.checkForDevice();
        assert(touch.isReady() && touch.max_slots == 5);
        touchGone = true;
        touch.state.is_pressed = true;
        touch.poll();
        assert(!touch.isReady() && !touch.state.is_pressed && touchCloses == 2);
        nowMs = 2000;
        touch.checkForDevice();
        unsigned initialTouchOpens = touchOpens;
        for (nowMs = 2001; nowMs < 2500; ++nowMs) touch.checkForDevice();
        assert(touchOpens == initialTouchOpens);
        touchGone = false;
        touch.checkForDevice();
        assert(touch.isReady() && touch.max_slots == 5 && touch.current_slot == 0);

        mode.xres = 100;
        layout.line_length = 400;
        layout.smem_len = 400 * mode.yres_virtual;
        fb.checkForFramebuffer();
        assert(fb.isReady() && fb.width == 100 && fb.line_length == 400 && maps == 3 && unmaps == 2);
        assert(!touch.isReady());
        fb.beginFrame();
        fb.drawPixel(99, 47, 255, 0, 0);
        fb.present();
        assert(fb.front_page == 1);
        panError = EINVAL;
        fb.beginFrame();
        fb.present();
        assert(fb.isReady() && !fb.pageflip_enabled);
        fb.beginFrame();
        assert(fb.draw_offset == fb.page_len);
        panError = 0;

        available = false;
        nowMs += 500;
        fb.checkForFramebuffer();
        assert(!fb.isReady() && maps == unmaps);
        available = true;
        hasDrm = false;
        mapFailure = true;
        nowMs += 500;
        fb.checkForFramebuffer();
        assert(!fb.isReady() && maps == unmaps);
        mapFailure = false;
        nowMs += 500;
        fb.checkForFramebuffer();
        assert(fb.isReady()); // Non-DRM panels still work.
        deviceInode = 2;
        unsigned priorUnmaps = unmaps;
        nowMs += 500;
        fb.checkForFramebuffer();
        assert(fb.isReady() && unmaps == priorUnmaps + 1);
        deviceInode = 1;
        queryFailure = true;
        fb.present();
        assert(!fb.isReady() && maps == unmaps);
        queryFailure = false;

        layout.smem_len = 1;
        nowMs += 500;
        fb.checkForFramebuffer();
        assert(!fb.isReady()); // Never map an invalid or undersized layout.
    }
    {
        Audiox app;
        available = false;
        hasDrm = true;
        nestedDrm = true;
        FramebufferContext fb(&app);
        TouchContext touch(&app);
        assert(!fb.isReady() && !touch.isReady());
        unsigned initialOpens = opens, initialTouchOpens = touchOpens;
        fb.checkForFramebuffer();
        assert(opens == initialOpens && touchOpens == initialTouchOpens);
        available = true;
        layout.smem_len = layout.line_length * mode.yres_virtual;
        setStatus(false);
        nowMs += 500;
        fb.checkForFramebuffer();
        assert(!fb.isReady()); // Nested DRM topology also detects unplugged outputs.
        setStatus(true);
        nowMs += 500;
        fb.checkForFramebuffer();
        assert(fb.isReady());
        touch.checkForDevice();
        assert(touch.isReady());
    }
    {
        // Full KMS: preferred over fbdev, page flips, cached polling, uevent re-probe.
        Audiox app;
        drmPresent = true;
        FramebufferContext fb(&app);
        assert(fb.usingKms() && fb.width == 80 && fb.height == 48 && fb.line_length == 320);
        assert(strcmp(fb.dev_path, "/dev/dri/card0") == 0 && strcmp(fb.kms.name, "DSI-1") == 0);
        assert(drmOpens == 1 && drmModesets == 1 && drmBuffers == 2 && drmMaps == 2 && drmScanout == 1001);
        fb.beginFrame();
        fb.drawPixel(79, 47, 255, 0, 0);
        fb.present();
        assert(drmFlips == 1 && drmScanout == 1002 && fb.front_page == 1);
        assert(fb.kms.maps[1][47 * 320 + 79 * 4 + 2] == 255);
        fb.beginFrame();
        fb.present(); // Waits for the previous flip's completion event first.
        assert(drmFlips == 2 && drmScanout == 1001 && fb.front_page == 0 && !fb.kms.flipTimeoutWarned);

        unsigned probes = drmProbes;
        nowMs += 500;
        fb.checkForFramebuffer();
        assert(fb.usingKms() && drmProbes == probes); // Periodic checks use cached state.
        fb.displayEvent();
        fb.checkForFramebuffer();
        assert(fb.usingKms() && drmProbes == probes + 1 && drmOpens == 1); // Uevents re-detect.

        TouchContext touch(&app);
        touch.checkForDevice();
        assert(touch.isReady() && touch.max_x == 4095);
        assert(touchScale(4095, 0, 4095, fb.width) == 79 && touchScale(0, 0, 4095, fb.width) == 0);
        assert(touchScale(2048, 0, 4095, fb.height) == 23 && touchScale(-5, 0, 4095, fb.height) == 0);

        drmConnected = false;
        available = false;
        fb.displayEvent();
        fb.checkForFramebuffer();
        assert(!fb.isReady() && !touch.isReady() && fb.kms.fd < 0 && fb.kms.idleCard);
        assert(drmCloses == 2 && drmMaps == drmUnmaps && drmDestroyed == drmBuffers);
        drmConnected = true;
        fb.displayEvent();
        fb.checkForFramebuffer();
        assert(fb.usingKms() && drmModesets == 2 && !fb.kms.idleCard);

        drmFlipError = ENODEV;
        fb.beginFrame();
        fb.present();
        assert(!fb.isReady() && drmCloses == 3);
        drmFlipError = 0;
        nowMs += 500;
        fb.checkForFramebuffer();
        assert(fb.usingKms());
    }
    {
        // A KMS driver binding later (vc4 replacing the firmware simplefb) takes over.
        Audiox app;
        drmPresent = false;
        available = true;
        setStatus(true);
        FramebufferContext fb(&app);
        assert(fb.isReady() && !fb.usingKms());
        drmPresent = true;
        nowMs += 500;
        fb.checkForFramebuffer();
        assert(!fb.usingKms()); // No DRM probing without a uevent.
        fb.displayEvent();
        fb.checkForFramebuffer();
        assert(fb.usingKms());
    }
    {
        // Touchpads and other indirect pointers are not touchscreens.
        Audiox app;
        TouchContext touch(&app);
        touchDirect = false;
        nowMs += 500;
        touch.checkForDevice();
        assert(!touch.isReady());
        touchDirect = true;
        touchNoProps = true; // Older touch drivers only report BTN_TOUCH.
        nowMs += 500;
        touch.checkForDevice();
        assert(touch.isReady());
        touchNoProps = false;
    }
    drmPresent = false;
    assert(drmMaps == drmUnmaps && drmOpens == drmCloses);
    assert(maps == unmaps && closes >= maps);
    assert(touchOpens > 0 && touchCloses == 7);
    assert(unlink(statusPath) == 0);
    puts("display hotplug, KMS, resource cleanup, touch recovery, and 500ms polling tests passed");
}
