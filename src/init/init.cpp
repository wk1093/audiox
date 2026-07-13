#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>

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

#define HANDLE_ERROR(func) \
    do { \
        int ret = (func); \
        if (ret == RET_ERR) { \
            printf("[INIT] [CRIT] %s failed with error code %d\n", #func, ret); \
            flushLogs(); \
            freopen("/dev/tty1", "w", stdout); \
            freopen("/dev/tty1", "w", stderr); \
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


int main() {
    umask(0);

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
    
    fb.bootStatus("Loading configuration...");
    ConfigStore config(&mainContext);
    printf("[INIT] config: sampleRate=%u, playbackChannels=%u, captureChannels=%u, sampleSize=%u\n",
           config.readConfigFile().sampleRate,
           config.readConfigFile().playbackChannels,
           config.readConfigFile().captureChannels,
           config.readConfigFile().sampleSize);

    fb.bootStatus("Setting up Audio gadget...");
    HANDLE_ERROR(setupAudioGadget(&mainContext));

    fb.bootStatus("Setting up Network gadget...");
    HANDLE_ERROR(setupNetworkGadget(&mainContext));

    fb.bootStatus("Binding USB gadget...");
    HANDLE_ERROR(bindUsbGadget(&mainContext));

    sleep(1); // give it a moment to setup the interface

    fb.bootStatus("Setting up network interface...");
    HANDLE_ERROR(setupNetworkInterface(&mainContext));

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

    // starts the initial worker threads
    // these might get removed or new threads added based on config, and auto device discovery
    fb.bootStatus("Starting audio manager thread...");
    HANDLE_ERROR(audio.setupThreads());

    HANDLE_ERROR(http.startSocket());

    fb.bootStatus("Starting main event loop...");
    sleep(1);
    mainContext.setReady();
    flushLogs();
    int logFlushCounter = 0;
    while (1) {
        // nothing in this loop is super high-importance, so I am going to add a small delay to give the kernel 
        // more of an opportunity to dedicate resources to audio processing.
        usleep(10000); // 10ms
        if (++logFlushCounter >= 100) {
            flushLogs();
            logFlushCounter = 0;
        }
        mainContext.eventLoop();
        touch.poll();
        fb.drawMain(&touch.state);
        http.poll();
        audio.poll(); // no audio processing, only hotplug/talking to config for device/routing settings.
        midi.poll();
        if (!fb.isReady()) {
            fb.checkForFramebuffer();
        }
        if (!touch.isReady()) {
            touch.checkForDevice();
        }
    }

}
