#include "system.hpp"
#include <cassert>
#include <sys/stat.h>

static int testEnsureDir(const char *path, mode_t mode) {
    int result = ensureDir(path, mode);
    if (result != RET_OK) return result;
    const char *midiAttrs[] = {"id", "in_ports", "out_ports"};
    const char *hidAttrs[] = {"protocol", "subclass", "report_length", "report_desc"};
    const char *const *attrs = nullptr;
    unsigned count = 0;
    if (strcmp(path, GADGET_MIDI_FUNC) == 0) { attrs = midiAttrs; count = 3; }
    if (strcmp(path, GADGET_KEYBOARD_FUNC) == 0) { attrs = hidAttrs; count = 4; }
    for (unsigned i = 0; i < count; ++i) {
        char attr[256];
        snprintf(attr, sizeof(attr), "%s/%s", path, attrs[i]);
        int fd = open(attr, O_CREAT | O_WRONLY, 0600);
        assert(fd >= 0);
        close(fd);
    }
    return RET_OK;
}

static int testRemoveFunction(const char *path) {
    const char *attrs[] = {"id", "in_ports", "out_ports", "protocol", "subclass", "report_length", "report_desc"};
    if (strcmp(path, GADGET_MIDI_FUNC) == 0 || strcmp(path, GADGET_KEYBOARD_FUNC) == 0) {
        for (const char *name : attrs) {
            char attr[256];
            snprintf(attr, sizeof(attr), "%s/%s", path, name);
            assert(unlink(attr) == 0 || errno == ENOENT);
        }
    }
    return rmdir(path);
}

#define ensureDir testEnsureDir
#define rmdir testRemoveFunction
#include "../src/init/gadget.cpp"
#undef ensureDir
#undef rmdir

static void createBinding(const char *binding) {
    int fd = open(GADGET_UDC_NODE, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, binding, strlen(binding)) == static_cast<ssize_t>(strlen(binding)));
    assert(close(fd) == 0);
}

static void createLinks() {
    assert(symlink("audio-function", GADGET_CONFIG_LINK) == 0);
    assert(symlink("network-function", GADGET_NETWORK_LINK) == 0);
    assert(symlink("midi-function", GADGET_MIDI_LINK) == 0);
    assert(symlink("keyboard-function", GADGET_KEYBOARD_LINK) == 0);
}

static void assertPrepared() {
    char binding[128] = {};
    int fd = open(GADGET_UDC_NODE, O_RDONLY);
    assert(fd >= 0);
    assert(read(fd, binding, sizeof(binding) - 1) == 1);
    assert(close(fd) == 0);
    assert(strcmp(binding, "\n") == 0);
    struct stat status = {};
    assert(lstat(GADGET_CONFIG_LINK, &status) < 0 && errno == ENOENT);
    assert(lstat(GADGET_NETWORK_LINK, &status) < 0 && errno == ENOENT);
    assert(lstat(GADGET_MIDI_LINK, &status) < 0 && errno == ENOENT);
    assert(lstat(GADGET_KEYBOARD_LINK, &status) < 0 && errno == ENOENT);
}

int main() {
    char directory[] = "/tmp/audiox-gadget-setup-XXXXXX";
    assert(mkdtemp(directory));
    int originalDirectory = open(".", O_RDONLY | O_DIRECTORY);
    assert(originalDirectory >= 0);
    assert(chdir(directory) == 0);

    assert(prepareUsbGadget() == RET_OK);
    assert(mkdir(GADGET_ROOT, 0700) == 0);
    assert(mkdir(GADGET_ROOT "/configs", 0700) == 0);
    assert(mkdir(GADGET_ROOT "/configs/c.1", 0700) == 0);
    assert(mkdir(GADGET_ROOT "/functions", 0700) == 0);

    ConfigData cfg{};
    assert(configureMidiFunctions(cfg) == RET_OK);
    struct stat status{};
    assert(lstat(GADGET_MIDI_FUNC, &status) < 0 && errno == ENOENT);
    assert(lstat(GADGET_KEYBOARD_FUNC, &status) < 0 && errno == ENOENT);
    cfg.midiPassthrough = 1;
    cfg.keyboardMacros = 1;
    assert(configureMidiFunctions(cfg) == RET_OK);
    assert(lstat(GADGET_MIDI_LINK, &status) == 0 && S_ISLNK(status.st_mode));
    assert(lstat(GADGET_KEYBOARD_LINK, &status) == 0 && S_ISLNK(status.st_mode));
    FILE *id = fopen(GADGET_MIDI_FUNC "/id", "r");
    assert(id);
    char name[64]{};
    assert(fgets(name, sizeof(name), id));
    fclose(id);
    assert(strcmp(name, AUDIOX_MIDI_CARD_ID) == 0);
    uint32_t length;
    assert(readSysNodeU32(GADGET_KEYBOARD_FUNC "/report_length", &length) == RET_OK && length == 8);
    cfg.midiPassthrough = 0;
    assert(configureMidiFunctions(cfg) == RET_OK);
    assert(lstat(GADGET_MIDI_FUNC, &status) < 0 && errno == ENOENT);
    assert(lstat(GADGET_MIDI_LINK, &status) < 0 && errno == ENOENT);
    assert(lstat(GADGET_KEYBOARD_LINK, &status) == 0);
    cfg.keyboardMacros = 0;
    assert(configureMidiFunctions(cfg) == RET_OK);
    assert(lstat(GADGET_KEYBOARD_FUNC, &status) < 0 && errno == ENOENT);

    createBinding("usbip-vudc.0\n");
    createLinks();
    assert(prepareUsbGadget() == RET_OK);
    assertPrepared();
    assert(prepareUsbGadget() == RET_OK);
    assertPrepared();

    createBinding("fe980000.usb\n");
    createLinks();
    assert(prepareUsbGadget() == RET_OK);
    assertPrepared();

    createLinks();
    assert(prepareUsbGadget() == RET_OK);
    assertPrepared();

    assert(unlink(GADGET_UDC_NODE) == 0);
    assert(mkdir(GADGET_UDC_NODE, 0700) == 0);
    createLinks();
    assert(prepareUsbGadget() == RET_ERR);
    assert(lstat(GADGET_CONFIG_LINK, &status) == 0);
    assert(lstat(GADGET_NETWORK_LINK, &status) == 0);

    assert(unlink(GADGET_CONFIG_LINK) == 0);
    assert(unlink(GADGET_NETWORK_LINK) == 0);
    assert(unlink(GADGET_MIDI_LINK) == 0);
    assert(unlink(GADGET_KEYBOARD_LINK) == 0);
    assert(rmdir(GADGET_UDC_NODE) == 0);
    assert(rmdir(GADGET_ROOT "/configs/c.1") == 0);
    assert(rmdir(GADGET_ROOT "/configs") == 0);
    assert(rmdir(GADGET_ROOT "/functions") == 0);
    assert(rmdir(GADGET_ROOT) == 0);
    assert(fchdir(originalDirectory) == 0);
    assert(close(originalDirectory) == 0);
    assert(rmdir(directory) == 0);
    puts("USB gadget restart preparation tests passed");
    return 0;
}
