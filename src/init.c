#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "config.h"
#include "init_defs.h"
#include "init_helpers.h"
#include "shell.h"
#include "usb_gadget.h"

#ifndef DEBUG_SHELL
#define DEBUG_SHELL 0
#endif

static int load_modules_from_list(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("[INIT] [ERR] Could not open module load list %s\n", path);
        return -1;
    }

    char line[768];
    size_t loaded = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }

        if (*p == '\0' || *p == '\n' || *p == '#') {
            continue;
        }

        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
            p[len - 1] = '\0';
            --len;
        }

        if (len == 0) {
            continue;
        }

        printf("  -> Loading: %s\n", p);
        if (load_module(p) < 0) {
            fclose(fp);
            return -1;
        }
        ++loaded;
    }

    fclose(fp);

    if (loaded == 0) {
        printf("[INIT] [ERR] Module list %s was empty.\n", path);
        return -1;
    }

    printf("[INIT] Loaded %zu kernel modules from %s\n", loaded, path);
    return 0;
}

static void run_shell_if_debug_enabled(void) {
#if DEBUG_SHELL
    shell_run();
#else
    printf("[INIT] DEBUG_SHELL disabled; skipping temporary filesystem shell.\n");
#endif
}

static void debug_input_devices(void) {
    int fd = open("/proc/bus/input/devices", O_RDONLY);
    if (fd < 0) {
        printf("[INIT] Failed to open /proc/bus/input/devices\n");
        return;
    }
    char buf[4096];
    ssize_t bytes = read(fd, buf, sizeof(buf) - 1);
    if (bytes > 0) {
        buf[bytes] = '\0';
        printf("--- CURRENT INPUT DEVICES ---\n%s-----------------------------\n", buf);
    }
    close(fd);
}



int main(void) {
    umask(0);

    atexit(app_show_terminal_cursor);
    app_hide_terminal_cursor();

    /* Mount basic filesystems */
    if (mount_basic_filesystems() < 0) {
        goto emergency_halt;
    }

    printf("\n==================================================\n");
    printf("   CUSTOM AUDIO DESK APPLIANCE - INITIALIZER      \n");
    printf("==================================================\n");

    /* Optional debug shell before device initialization */
    run_shell_if_debug_enabled();

    /* Load kernel drivers */
    printf("[INIT] Injecting real-time audio gadget driver stack...\n");
    if (load_modules_from_list(MODULE_LOAD_LIST_FILE) < 0) {
        printf("[INIT] [CRIT] Driver framework injection stalled.\n");
        goto emergency_halt;
    }
    sleep(3); // let graphics initialize before we start writing to the framebuffer
    printf("[INIT] Gadget drivers mounted clean.\n");

    /* Setup USB audio device */
    printf("[INIT] Synthesizing UAC2 blueprint structures...\n");
    if (setup_usb_audio_gadget() < 0) {
        printf("[INIT] [CRIT] Structural assembly error tracked during ConfigFS generation.\n");
        goto emergency_halt;
    }

    printf("[INIT] Binding operational stack to DesignWare OTG Core controller...\n");
    if (write_sys_node(GADGET_UDC_NODE, GADGET_UDC_NAME) < 0) {
        goto emergency_halt;
    }

    sleep(1);

    printf("\n==================================================\n");
    printf("  SUCCESS: DEVICE OPERATIONAL ENGINES ARE LIVE     \n");
    printf("==================================================\n\n");

    debug_input_devices();

    /* Initialize application (UI, audio, touch, MIDI) */
    app_ctx_t app;
    if (app_init(&app) < 0) {
        printf("[INIT] [CRIT] Failed to initialize application.\n");
        goto emergency_halt;
    }

    /* Main application loop */
    app_run(&app);

    /* Cleanup (normally unreachable) */
    app_cleanup(&app);
    return 0;

emergency_halt:
    printf("[INIT] System tracking context faulted. Entering panic protection loop.\n");
    while (1) {
        sleep(60);
    }
    return 1;
}