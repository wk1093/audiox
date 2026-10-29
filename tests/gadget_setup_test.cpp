#include "../src/init/gadget.cpp"

#include <cassert>
#include <sys/stat.h>

static void createBinding(const char *binding) {
    int fd = open(GADGET_UDC_NODE, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, binding, strlen(binding)) == static_cast<ssize_t>(strlen(binding)));
    assert(close(fd) == 0);
}

static void createLinks() {
    assert(symlink("audio-function", GADGET_CONFIG_LINK) == 0);
    assert(symlink("network-function", GADGET_NETWORK_LINK) == 0);
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
    struct stat status = {};
    assert(lstat(GADGET_CONFIG_LINK, &status) == 0);
    assert(lstat(GADGET_NETWORK_LINK, &status) == 0);

    assert(unlink(GADGET_CONFIG_LINK) == 0);
    assert(unlink(GADGET_NETWORK_LINK) == 0);
    assert(rmdir(GADGET_UDC_NODE) == 0);
    assert(rmdir(GADGET_ROOT "/configs/c.1") == 0);
    assert(rmdir(GADGET_ROOT "/configs") == 0);
    assert(rmdir(GADGET_ROOT) == 0);
    assert(fchdir(originalDirectory) == 0);
    assert(close(originalDirectory) == 0);
    assert(rmdir(directory) == 0);
    puts("USB gadget restart preparation tests passed");
    return 0;
}
