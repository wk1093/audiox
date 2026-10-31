#include "framebuffer/context.hpp"
#include "touch/context.hpp"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <new>
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

void setStatus(bool value) {
    connected = value;
    FILE *file = fopen(statusPath, "w");
    assert(file);
    assert(fputs(connected ? "connected\n" : "disconnected\n", file) >= 0);
    assert(fclose(file) == 0);
}

int fakeOpen(const char *path, int) {
    if (strncmp(path, "/dev/fb", 7) == 0) {
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
    assert(fd == 100 && size == layout.smem_len);
    if (mapFailure) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    ++maps;
    return new uint8_t[size];
}

int fakeUnmap(void *memory, size_t) {
    ++unmaps;
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
    if (request == EVIOCGABS(ABS_X) || request == EVIOCGABS(ABS_Y)) {
        auto *axis = static_cast<input_absinfo *>(data);
        *axis = {};
        axis->maximum = 4095;
    } else if (request == EVIOCGABS(ABS_MT_SLOT)) {
        auto *axis = static_cast<input_absinfo *>(data);
        *axis = {};
        axis->maximum = 4;
    } else if (_IOC_NR(request) >= _IOC_NR(EVIOCGBIT(0, 0)) &&
               _IOC_NR(request) <= _IOC_NR(EVIOCGBIT(EV_MAX, 0))) {
        memset(data, 0xff, _IOC_SIZE(request));
    } else assert(false);
    return 0;
}

ssize_t fakeRead(int fd, void *, size_t) {
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
#include "../src/framebuffer/hardware.cpp"
#include "../src/framebuffer/drawing.cpp"
#include "../src/touch/touch.cpp"
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

#include "../src/framebuffer/ui.cpp"

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
    assert(maps == unmaps && closes >= maps);
    assert(touchOpens > 0 && touchCloses == 4);
    assert(unlink(statusPath) == 0);
    puts("display hotplug, resource cleanup, touch recovery, and 500ms polling tests passed");
}
