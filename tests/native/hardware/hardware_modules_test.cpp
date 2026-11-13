#include "hardware_modules.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

std::string root;
std::vector<std::string> loads;
int failWith = 0;
std::string failModule;

void writeFile(const std::string &path, const std::string &text) {
    std::string dir = path.substr(0, path.rfind('/'));
    std::string command = "mkdir -p '" + dir + "'";
    assert(system(command.c_str()) == 0);
    FILE *file = fopen(path.c_str(), "w");
    assert(file);
    assert(fputs(text.c_str(), file) >= 0);
    assert(fclose(file) == 0);
}

int fakeLoader(const char *path, void *) {
    std::string name = hardwareModuleName(path);
    loads.push_back(name);
    if (name == failModule) return -failWith;
    // Bus drivers create child devices once loaded; coldplug must find them.
    if (name == "i2c_brcmstb") writeFile(root + "/sys/devices/platform/i2c/i2c-10/10-0038/modalias", "i2c:edt-ft5406\n");
    if (name == "already_there") return -EEXIST;
    return 0;
}

size_t indexOf(const std::string &name) {
    auto it = std::find(loads.begin(), loads.end(), name);
    assert(it != loads.end());
    return (size_t)(it - loads.begin());
}

bool loaded(const std::string &name) {
    return std::find(loads.begin(), loads.end(), name) != loads.end();
}

void sendEvent(HardwareModules &hardware, const std::vector<std::string> &fields, HardwareModules::PollResult &result) {
    std::string message;
    for (const std::string &field : fields) {
        message += field;
        message.push_back('\0');
    }
    hardware.handleUevent(message.data(), message.size(), result);
}

}  // namespace

int main() {
    char tmp[] = "/tmp/audiox-hardware-modules-XXXXXX";
    assert(mkdtemp(tmp));
    root = tmp;
    const std::string modules = root + "/lib/modules/test";
    writeFile(modules + "/modules.dep",
              "kernel/drivers/gpu/drm/vc4/vc4.ko: kernel/drivers/gpu/drm/drm.ko kernel/sound/core/snd-pcm.ko\n"
              "kernel/drivers/gpu/drm/drm.ko:\n"
              "kernel/sound/core/snd-pcm.ko:\n"
              "kernel/sound/soc/codecs/snd-soc-hdmi-codec.ko:\n"
              "kernel/drivers/i2c/busses/i2c-brcmstb.ko:\n"
              "kernel/drivers/input/touchscreen/edt-ft5x06.ko:\n"
              "kernel/drivers/misc/broken.ko:\n"
              "kernel/drivers/misc/already-there.ko:\n"
              "kernel/drivers/misc/new-thing.ko:\n");
    writeFile(modules + "/modules.alias",
              "# Aliases extracted from modules themselves.\n"
              "alias of:N*T*Cbrcm,bcm2711-vc5C* vc4\n"
              "alias of:N*T*Cbrcm,bcm2711-vc5 vc4\n"
              "alias of:N*T*Cbrcm,brcmstb-i2cC* i2c_brcmstb\n"
              "alias of:N*T*Cbrcm,brcmstb-i2c i2c_brcmstb\n"
              "alias i2c:edt-ft5406 edt_ft5x06\n"
              "alias of:N*T*Cedt,edt-ft5406 edt_ft5x06\n"
              "alias of:N*T*Cacme,broken broken\n"
              "alias of:N*T*Cacme,broken2 broken\n"
              "alias of:N*T*Cacme,there already_there\n"
              "alias of:N*T*Cacme,new new_thing\n"
              "alias of:N*T*Cacme,unstaged not_staged\n"
              "alias *:acme-any* new_thing\n");
    writeFile(modules + "/modules.softdep", "softdep vc4 pre: snd-soc-hdmi-codec\n");
    writeFile(root + "/sys/module/drm/refcnt", "1\n");
    writeFile(root + "/sys/devices/platform/gpu/modalias", "of:NgpuT(null)Cbrcm,bcm2711-vc5\n");
    writeFile(root + "/sys/devices/platform/i2c/modalias", "of:Ni2cT(null)Cbrcm,brcmstb-i2c\n");
    writeFile(root + "/sys/devices/platform/bound/modalias", "of:NboundT(null)Cedt,edt-ft5406\n");
    assert(symlink("/nonexistent", (root + "/sys/devices/platform/bound/driver").c_str()) == 0);
    writeFile(root + "/sys/devices/platform/unstaged/modalias", "of:NxT(null)Cacme,unstaged\n");
    writeFile(root + "/sys/devices/platform/broken/modalias", "of:NbT(null)Cacme,broken\n");

    assert(hardwareModuleName("kernel/sound/soc/codecs/snd-soc-hdmi-codec.ko.xz") == "snd_soc_hdmi_codec");

    HardwareModules hardware;
    hardware.setLoader(fakeLoader, nullptr);
    hardware.configure(modules.c_str(), (root + "/sys").c_str());
    assert(hardware.aliasCount() == 11); // The unstaged module's alias is dropped.

    failModule = "broken";
    failWith = ENODEV;
    // vc4 (+ softdep + dependency), i2c-brcmstb, then edt-ft5x06 for the device the bus created.
    assert(hardware.coldplug() == 3);
    assert(loads.size() == 6);
    assert(indexOf("snd_soc_hdmi_codec") < indexOf("vc4") && indexOf("snd_pcm") < indexOf("vc4"));
    assert(indexOf("i2c_brcmstb") < indexOf("edt_ft5x06") && loaded("broken"));
    assert(!loaded("drm") && !loaded("not_staged")); // Already loaded / not staged.

    loads.clear();
    assert(hardware.coldplug() == 0 && loads.empty()); // Nothing is loaded or attempted twice.

    HardwareModules::PollResult result;
    sendEvent(hardware, {"add@/devices/platform/b2", "ACTION=add", "DEVPATH=/devices/platform/b2",
                         "SUBSYSTEM=platform", "MODALIAS=of:Nb2T(null)Cacme,broken2"}, result);
    assert(loads.empty() && result.modulesLoaded == 0); // A failed module is not retried.

    sendEvent(hardware, {"add@/devices/platform/new", "ACTION=add", "DEVPATH=/devices/platform/new",
                         "SUBSYSTEM=platform", "MODALIAS=of:NnT(null)Cacme,new"}, result);
    assert(loads == std::vector<std::string>{"new_thing"} && result.modulesLoaded == 1 && !result.displayChanged);

    sendEvent(hardware, {"remove@/devices/platform/x", "ACTION=remove", "SUBSYSTEM=platform",
                         "MODALIAS=of:NxT(null)Cacme,there"}, result);
    assert(loads.size() == 1); // Only additions load drivers.
    sendEvent(hardware, {"add@/devices/platform/x", "ACTION=add", "SUBSYSTEM=platform",
                         "MODALIAS=of:NxT(null)Cacme,there"}, result);
    assert(loads.size() == 2 && result.modulesLoaded == 2); // EEXIST counts as loaded.

    sendEvent(hardware, {"change@/devices/platform/gpu/drm/card1", "ACTION=change", "SUBSYSTEM=drm", "HOTPLUG=1"},
              result);
    assert(result.displayChanged && loads.size() == 2);

    HardwareModules empty;
    empty.setLoader(fakeLoader, nullptr);
    empty.configure((root + "/missing").c_str(), (root + "/sys").c_str());
    assert(empty.aliasCount() == 0 && empty.coldplug() == 0 && loads.size() == 2);

    std::string cleanup = "rm -rf '" + root + "'";
    assert(system(cleanup.c_str()) == 0);
    puts("hardware module coldplug, dependency, softdep and uevent tests passed");
}
