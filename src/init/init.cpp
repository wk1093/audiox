#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/types.h>

#include "defs.hpp"
#include "context.hpp"
#include "init.hpp"
#include "framebuffer/context.hpp"
#include "config/context.hpp"
#include "touch/context.hpp"
#include "audio/context.hpp"
#include "midi/context.hpp"
#include "http/context.hpp"
#include "http/network.hpp"
#include "http/mdns.hpp"
#include "bluetooth/context.hpp"
#include "watchdog_heartbeat.hpp"
#include "diagnostics.hpp"
#include "hardware_power_button.hpp"
#include "system_history.hpp"
#include "usage_telemetry.hpp"

#define HANDLE_ERROR(func) \
    do { \
        int ret = (func); \
        if (ret == RET_ERR) { \
            printf("[INIT] [CRIT] %s failed with error code %d\n", #func, ret); \
            flushLogs(); \
            if (!freopen(AUDIOX_FATAL_CONSOLE, "w", stdout)) { \
                printf("[INIT] [WARN] failed to redirect stdout to %s: %s\n", AUDIOX_FATAL_CONSOLE, strerror(errno)); \
            } \
            if (!freopen(AUDIOX_FATAL_CONSOLE, "w", stderr)) { \
                printf("[INIT] [WARN] failed to redirect stderr to %s: %s\n", AUDIOX_FATAL_CONSOLE, strerror(errno)); \
            } \
            FILE *old = fopen("/audiox/stdout.log", "r"); \
            if (old) { \
                char buf[256]; \
                while (fgets(buf, sizeof(buf), old)) { \
                    fputs(buf, stdout); \
                } \
                fclose(old); \
            } \
            printf("[INIT] [CRIT] halting system due to fatal error.\n"); \
            while (1) { \
                sleep(60); \
            } \
        } else if (ret == RET_WARN) { \
            printf("[INIT] [WARN] %s returned warning code %d\n", #func, ret); \
        } else if (ret != RET_OK) { \
            printf("[INIT] [WARN] %s returned unexpected code %d\n", #func, ret); \
        } \
    } while (0)

static uint64_t monotonicMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void drawPowerButtonStatus(FramebufferContext *fb, const char *title,
                                  const char *detail, unsigned char r,
                                  unsigned char g, unsigned char b) {
    if (!fb || !fb->isReady()) return;
    fb->beginFrame();
    fb->drawRect(0, 0, (int)fb->width, (int)fb->height, 5, 9, 16);
    const int titleWidth = (int)strlen(title) * 16;
    int titleX = ((int)fb->width - titleWidth) / 2;
    if (titleX < 0) titleX = 0;
    const int titleY = ((int)fb->height - 32) / 2;
    fb->drawText(titleX, titleY,
                 title, r, g, b, 2);
    if (detail && detail[0]) {
        const int detailWidth = (int)strlen(detail) * 8;
        int detailX = ((int)fb->width - detailWidth) / 2;
        if (detailX < 0) detailX = 0;
        fb->drawText(detailX, titleY + 40,
                     detail, 220, 225, 235, 1);
    }
    fb->present();
}

static pid_t startPowerButtonShutdown(int *resultFd) {
    if (!resultFd) {
        errno = EINVAL;
        return -1;
    }
    int resultPipe[2];
    if (pipe2(resultPipe, O_CLOEXEC | O_NONBLOCK) < 0) return -1;
    const pid_t child = fork();
    if (child < 0) {
        const int savedErrno = errno;
        close(resultPipe[0]);
        close(resultPipe[1]);
        errno = savedErrno;
        return -1;
    }
    if (child == 0) {
        close(resultPipe[0]);
        sync();
        sleep(1);
        if (reboot(RB_POWER_OFF) < 0) {
            const int error = errno;
            ssize_t count;
            do {
                count = write(resultPipe[1], &error, sizeof(error));
            } while (count < 0 && errno == EINTR);
            if (count != (ssize_t)sizeof(error)) {
                fprintf(stderr, "[INIT] [ERR] reporting power-off failure (%s) failed: %s\n",
                        strerror(error), count < 0 ? strerror(errno) : "short pipe write");
            }
            _exit(1);
        }
        _exit(0);
    }
    close(resultPipe[1]);
    *resultFd = resultPipe[0];
    return child;
}

int main() {
    umask(0);
    printf("[INIT] audiox main process pid=%d ppid=%d\n", getpid(), getppid());

    // global appstate context
    Audiox mainContext;

    HANDLE_ERROR(mountFilesystems());
    HANDLE_ERROR(loadBaseModules()); // mostly just framebuffer and touchscreen drivers.

    // framebuffer should start asap so it can show boot logo and status
    FramebufferContext fb(&mainContext);

    fb.waitForFramebuffer(1); // wait max of 1 second for framebuffer to be ready.

    // disable cursor
    fprintf(stdout, "\e[?25l");

    fb.drawBootLogo();

    fb.bootStatus("Loading kernel modules...");
    HANDLE_ERROR(loadAllModules()); // all the audio and usb drivers.

    fb.bootStatus("Setting up filesystem...");
    HANDLE_ERROR(mountRootfs(&mainContext));

    fb.bootStatus("Setting up logging...");
    // this makes it write to audiox.log in the rootfs.
    HANDLE_ERROR(setupLogging(&mainContext)); // want this as soon as possible so errors are logged to rootfs
    initBootRecovery();
    
    fb.bootStatus("Loading configuration...");
    ConfigStore config(&mainContext);
    const ConfigData bootConfig = config.readConfigFile();
    printf("[INIT] config: sampleRate=%u, playbackChannels=%u, captureChannels=%u, sampleSize=%u\n",
           bootConfig.sampleRate,
           bootConfig.playbackChannels,
           bootConfig.captureChannels,
           bootConfig.sampleSize);

    fb.bootStatus("Setting up Audio gadget...");
    HANDLE_ERROR(setupAudioGadget(&mainContext));

    fb.bootStatus("Setting up Network gadget...");
    HANDLE_ERROR(setupNetworkGadget(&mainContext));

    fb.bootStatus("Binding USB gadget...");
    HANDLE_ERROR(bindUsbGadget(&mainContext));

    fb.bootStatus("Setting up network interface...");
    HANDLE_ERROR(setupNetworkInterface(&mainContext));

#if AUDIOX_QEMU
    fb.bootStatus("Setting up QEMU debug network and USB/IP...");
    HANDLE_ERROR(setupQemuDebug(&mainContext));
#endif

    // these constructors mostly just set up the context, and don't start anything
    // some will open the device files and do basic hardware initialization though.
    fb.bootStatus("Setting up touchscreen...");
    TouchContext touch(&mainContext);
    fb.bootStatus("Setting up audio subsystem...");
    AudioContext audio(&mainContext);
    fb.bootStatus("Setting up MIDI subsystem...");
    MidiContext midi(&mainContext);
    fb.bootStatus("Starting HTTP server...");
    HttpServer http(&mainContext);
    BluetoothContext bluetooth(&mainContext);
    PowerButton powerButton(strcmp(audioxDetectBoard(), "pi5") == 0);

    // starts the initial worker threads
    // these might get removed or new threads added based on config, and auto device discovery
    fb.bootStatus("Starting audio manager thread...");
    HANDLE_ERROR(audio.setupThreads());

    fb.bootStatus("Starting bluetooth thread...");
    HANDLE_ERROR(bluetooth.start());

    HANDLE_ERROR(http.startSocket());
    audiox::mdns::Responder mdns(USB_NET_IFACE, AUDIOX_MDNS_HOSTNAME);

    fb.bootStatus("Starting main event loop...");
    mainContext.setReady();
    flushLogs();
    // Heartbeat on elapsed time, not iteration count: slow iterations (e.g. software
    // framebuffer drawing in QEMU) would otherwise stretch the interval past the watchdog timeout.
    uint64_t lastHeartbeatMs = monotonicMs();
    pid_t poweroffPid = -1;
    int poweroffResultFd = -1;
    bool poweroffPending = false;
    bool poweroffFailed = false;
    while (1) {
        // nothing in this loop is super high-importance, so I am going to add a small delay to give the kernel 
        // more of an opportunity to dedicate resources to audio processing.
        usleep(10000); // 10ms
        const uint64_t nowMs = monotonicMs();
        if (nowMs - lastHeartbeatMs >= 1000) {
            flushLogsPeriodic(nowMs);
            lastHeartbeatMs = nowMs;
            audioxWatchdogHeartbeat();
        }
        mainContext.eventLoop();
        audiox::diagnostics::pollKernelLog();
        pollBootRecovery();
        fb.checkForFramebuffer();
        if (fb.isReady()) {
            touch.checkForDevice();
            touch.poll();
            if (!poweroffPending && !poweroffFailed) fb.drawMain(&touch.state);
        } else if (touch.isReady()) {
            touch.suspend();
        }
        audiox::system::pollSystemHistory(nowMs);
        audiox::telemetry::poll(mainContext, nowMs);
        http.poll();
        mdns.poll();
        audio.poll(); // no audio processing, only hotplug/talking to config for device/routing settings.
        midi.poll();
        mainContext.scripts->pollScriptLifecycle(nowMs, bootStartupStable());
        if (poweroffPending) {
            int rebootError = 0;
            const ssize_t count = read(poweroffResultFd, &rebootError, sizeof(rebootError));
            int childStatus = 0;
            pid_t exited = waitpid(poweroffPid, &childStatus,
                                    count == (ssize_t)sizeof(rebootError) ? 0 : WNOHANG);
            while (exited < 0 && errno == EINTR) {
                exited = waitpid(poweroffPid, &childStatus,
                                 count == (ssize_t)sizeof(rebootError) ? 0 : WNOHANG);
            }
            if (count == (ssize_t)sizeof(rebootError) || exited == poweroffPid) {
                if (poweroffResultFd >= 0) close(poweroffResultFd);
                poweroffResultFd = -1;
                poweroffPid = -1;
                poweroffPending = false;
                poweroffFailed = true;
                if (count != (ssize_t)sizeof(rebootError)) {
                    rebootError = EIO;
                }
                printf("[INIT] [ERR] power-button shutdown failed: reboot(RB_POWER_OFF): %s (errno %d)\n",
                       strerror(rebootError), rebootError);
                drawPowerButtonStatus(&fb, "Shutdown failed",
                                      strerror(rebootError), 255, 105, 105);
                flushLogs();
            }
        } else if (powerButton.pollPressed()) {
            poweroffFailed = false;
            drawPowerButtonStatus(&fb, "Shutting down...", nullptr, 240, 245, 255);
            flushLogs();
            poweroffPid = startPowerButtonShutdown(&poweroffResultFd);
            if (poweroffPid < 0) {
                const int error = errno;
                poweroffFailed = true;
                printf("[INIT] [ERR] power-button shutdown could not start: %s (errno %d)\n",
                       strerror(error), error);
                drawPowerButtonStatus(&fb, "Shutdown failed",
                                      strerror(error), 255, 105, 105);
                flushLogs();
            } else {
                poweroffPending = true;
            }
        }
    }

}
