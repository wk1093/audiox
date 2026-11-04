#include "audio/plugin_package.hpp"
#include <cassert>
#include <cstring>
#include <elf.h>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace audiox::plugin;

namespace {
void putAt(std::vector<uint8_t> &bytes, size_t at, uint64_t value, size_t count) {
    for (size_t i = 0; i < count; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (i * 8));
}
std::vector<uint8_t> module() {
    std::vector<uint8_t> bytes(120);
    std::memcpy(bytes.data(), ELFMAG, SELFMAG);
    bytes[EI_CLASS] = ELFCLASS64; bytes[EI_DATA] = ELFDATA2LSB; bytes[EI_VERSION] = EV_CURRENT;
    putAt(bytes, 16, ET_DYN, 2);
#if defined(__aarch64__)
    putAt(bytes, 18, EM_AARCH64, 2);
#else
    putAt(bytes, 18, EM_X86_64, 2);
#endif
    putAt(bytes, 20, EV_CURRENT, 4); putAt(bytes, 32, 64, 8);
    putAt(bytes, 52, 64, 2); putAt(bytes, 54, 56, 2); putAt(bytes, 56, 1, 2);
    putAt(bytes, 64, PT_LOAD, 4); putAt(bytes, 96, bytes.size(), 8);
    putAt(bytes, 104, bytes.size(), 8);
    return bytes;
}
Entry textEntry(uint32_t kind, const char *name, const std::string &text) {
    return Entry{kind, name, std::vector<uint8_t>(text.begin(), text.end())};
}
std::vector<Entry> entries(const std::string &id = "plugin_gain") {
    return {textEntry(Metadata, "axm.txt", "id=" + id +
            "\nname=Gain\nversion=1.0\napi=1\nparam=gain|Gain|0|2|1\n"),
            Entry{Module, "module.so", module()}, textEntry(Asset, "index.html", "<p>Gain</p>"),
            textEntry(Asset, "controls.js", "window.gain=1;"),
            textEntry(Asset, "style.css", "body{color:white}")};
}
std::vector<uint8_t> encode(const std::vector<Entry> &source) {
    std::vector<uint8_t> bytes; std::string error;
    assert(buildPackage(source, bytes, error) && error.empty());
    return bytes;
}
void write(const std::string &path, const std::string &text) {
    std::ofstream stream(path, std::ios::binary); stream << text;
    assert(stream.good());
}
}

int main() {
    Package package;
    PackageMetadata metadata;
    std::string error;
    auto source = entries();
    auto bytes = encode(source);
    assert(parsePackage(bytes.data(), bytes.size(), package, error, true));
    assert(std::string(package.metadata.id) == "plugin_gain" && package.metadata.paramCount == 1);
    assert(package.metadata.params[0].defaultValue == 1);
    for (const char *id : {"", "gain", "plugin_", "plugin_1", "plugin_A", "plugin_a-b",
                           "plugin_abcdefghijklmnopq", "plugin_../x"})
        assert(!validId(id));
    assert(validId("plugin_abcdefghijklmnop"));
    for (const char *name : {"", "../x.js", "a..js", ".js", "a/b.js", "a\\b.js",
                             "module.so", "axm.txt", "a.png", "a.JS", "a b.js", "A.js"})
        assert(!validAssetName(name));
    assert(validAssetName("index.html") && validAssetName("controls_2.js"));
    for (const std::string &text : std::vector<std::string>{
            std::string("id=plugin_gain\nname=X\nversion=1\n"),
            "id=plugin_gain\nname=X\nversion=1\napi=2\n",
            "id=plugin_gain\nname=X\nversion=1\napi=1\napi=1\n",
            "id=plugin_gain\nname=X\nversion=1\napi=1\nunknown=x\n",
            "id=plugin_gain\nname=X\nversion=1\napi=1\n\n",
            "id=plugin_gain\r\nname=X\nversion=1\napi=1\n",
            "id=plugin_gain\nname=X\nversion=1\napi=1\nparam=x|X|nan|1|0\n",
            "id=plugin_gain\nname=X\nversion=1\napi=1\nparam=x|X|1|1|1\n",
            "id=plugin_gain\nname=X\nversion=1\napi=1\nparam=x|X|0|1|2\n",
            "id=plugin_gain\nname=X\nversion=1\napi=1\nparam=x|X|0|1|0\nparam=x|X|0|1|0\n",
            "id=plugin_gain\nname=X\nversion=1\napi=1\nparam=x|X|0|1|0|x\n"})
        assert(!parseMetadata(text, metadata, error));
    for (size_t length = 0; length < bytes.size(); ++length)
        assert(!parsePackage(bytes.data(), length, package, error));
    assert(!parsePackage(nullptr, bytes.size(), package, error));
    for (auto mutation : std::vector<std::pair<size_t, uint8_t>>{
            {0, 'X'}, {8, 2}, {12, 0}, {12, 19}, {16, 99}, {20, 255},
            {24, 255}, {32, static_cast<uint8_t>(bytes[32] ^ 1)}}) {
        auto corrupt = bytes; corrupt[mutation.first] = mutation.second;
        assert(!parsePackage(corrupt.data(), corrupt.size(), package, error));
    }
    auto corrupt = bytes; corrupt.push_back(0);
    assert(!parsePackage(corrupt.data(), corrupt.size(), package, error));
    std::vector<uint8_t> out;
    auto invalid = source; invalid.push_back(source[2]);
    assert(!buildPackage(invalid, out, error));
    invalid = source; invalid[2].name = "../bad.js";
    assert(!buildPackage(invalid, out, error));
    invalid = source; invalid[0].name = "other.txt";
    assert(!buildPackage(invalid, out, error));
    invalid = source; invalid.erase(invalid.begin() + 1);
    assert(!buildPackage(invalid, out, error));
    invalid = source; invalid[2].data.resize(kMaxAssetBytes + 1);
    assert(!buildPackage(invalid, out, error));
    invalid = source; invalid[0].data.resize(kMaxMetadataBytes + 1);
    assert(!buildPackage(invalid, out, error));
    invalid = source; invalid[1].data.resize(kMaxModuleBytes + 1);
    assert(!buildPackage(invalid, out, error));
    invalid = source;
    for (unsigned i = 0; i < 16; ++i)
        invalid.push_back(textEntry(Asset, ("extra" + std::to_string(i) + ".js").c_str(), ""));
    assert(!buildPackage(invalid, out, error));
    invalid = source; invalid[0].data = std::vector<uint8_t>{'a', 0, 'b'};
    assert(!buildPackage(invalid, out, error));
    for (size_t at : {size_t(EI_CLASS), size_t(EI_DATA), size_t(16), size_t(18),
                      size_t(32), size_t(52), size_t(54), size_t(56), size_t(96)}) {
        invalid = source; invalid[1].data[at] = 255;
        assert(!buildPackage(invalid, out, error));
    }
    invalid = source; putAt(invalid[1].data, 40, 119, 8);
    putAt(invalid[1].data, 58, 64, 2); putAt(invalid[1].data, 60, 1, 2);
    assert(!buildPackage(invalid, out, error));
    invalid = source;
#if defined(__aarch64__)
    putAt(invalid[1].data, 18, EM_X86_64, 2);
#else
    putAt(invalid[1].data, 18, EM_AARCH64, 2);
#endif
    auto otherArch = encode(invalid);
    assert(parsePackage(otherArch.data(), otherArch.size(), package, error));
    assert(!parsePackage(otherArch.data(), otherArch.size(), package, error, true));

    const std::string base = "tests/.plugin-package-" + std::to_string(getpid());
    const std::string root = base + "/plugins";
    assert(!std::filesystem::exists(base));
    std::vector<PackageMetadata> installed;
    std::string asset;
    assert(listPackages(root, installed, error) && installed.empty());
    assert(!std::filesystem::exists(base));
    assert(!listPackages(base + "/../bad", installed, error));
    assert(!listPackages(std::string("tests/bad\0hidden", 16), installed, error));
    assert(!installPackage(otherArch.data(), otherArch.size(), root, error));
    assert(!std::filesystem::exists(base));
    const mode_t previousMask = umask(0077);
    assert(installPackage(bytes.data(), bytes.size(), root, error));
    umask(previousMask);
    struct stat installedMode{};
    assert(stat((root + "/plugin_gain/module.so").c_str(), &installedMode) == 0);
    assert((installedMode.st_mode & 0777) == 0644);
    assert(stat(root.c_str(), &installedMode) == 0 && (installedMode.st_mode & 0777) == 0755);
    assert(stat((root + "/plugin_gain").c_str(), &installedMode) == 0 &&
           (installedMode.st_mode & 0777) == 0755);
    assert(!installPackage(bytes.data(), bytes.size(), root, error));
    assert(listPackages(root, installed, error) && installed.size() == 1);
    assert(readPluginAsset("plugin_gain", "index.html", root, asset, error) && asset == "<p>Gain</p>");
    assert(!readPluginAsset("plugin_gain", "module.so", root, asset, error));
    assert(!readPluginAsset("../bad", "index.html", root, asset, error));
    assert(!readPluginAsset("plugin_gain", "../axm.txt", root, asset, error));
    const std::string plugin = root + "/plugin_gain";
    write(plugin + "/controls.js", "corrupt");
    assert(!listPackages(root, installed, error));
    assert(!readPluginAsset("plugin_gain", "index.html", root, asset, error));
    write(plugin + "/controls.js", "window.gain=1;");
    assert(listPackages(root, installed, error));
    assert(link((plugin + "/controls.js").c_str(), (base + "/linked.js").c_str()) == 0);
    assert(!listPackages(root, installed, error));
    assert(!readPluginAsset("plugin_gain", "controls.js", root, asset, error));
    assert(unlink((base + "/linked.js").c_str()) == 0);
    assert(listPackages(root, installed, error));
    write(plugin + "/unexpected.js", "");
    assert(!listPackages(root, installed, error));
    assert(unlink((plugin + "/unexpected.js").c_str()) == 0);
    const std::string manifest = plugin + "/.checksums";
    std::ifstream manifestInput(manifest, std::ios::binary);
    const std::string originalManifest((std::istreambuf_iterator<char>(manifestInput)), {});
    assert(!originalManifest.empty());
    write(manifest, originalManifest + "extra");
    assert(!listPackages(root, installed, error));
    write(manifest, originalManifest);
    assert(listPackages(root, installed, error));
    write(plugin + "/module.so", "not ELF");
    assert(!listPackages(root, installed, error));
    assert(removePackage("plugin_gain", root, error));
    assert(installPackage(bytes.data(), bytes.size(), root, error));
    assert(!removePackage("../bad", root, error));
    assert(unlink((plugin + "/index.html").c_str()) == 0);
    assert(symlink("../../secret.html", (plugin + "/index.html").c_str()) == 0);
    write(base + "/secret.html", "secret");
    assert(!readPluginAsset("plugin_gain", "index.html", root, asset, error));
    assert(!listPackages(root, installed, error));
    assert(!removePackage("plugin_gain", root, error));
    assert(unlink((plugin + "/index.html").c_str()) == 0);
    write(plugin + "/index.html", "<p>Gain</p>");
    assert(removePackage("plugin_gain", root, error));
    assert(!removePackage("plugin_gain", root, error));
    assert(symlink("plugins", (base + "/alias").c_str()) == 0);
    assert(!listPackages(base + "/alias", installed, error));
    assert(!installPackage(bytes.data(), bytes.size(), base + "/alias", error));
    assert(unlink((root + "/.lock").c_str()) == 0);
    assert(symlink("../secret.html", (root + "/.lock").c_str()) == 0);
    assert(!listPackages(root, installed, error));
    assert(!installPackage(bytes.data(), bytes.size(), root, error));
    assert(unlink((root + "/.lock").c_str()) == 0);
    assert(symlink("../../secret.html", (root + "/plugin_gain").c_str()) == 0);
    assert(!installPackage(bytes.data(), bytes.size(), root, error));
    assert(!readPluginAsset("plugin_gain", "index.html", root, asset, error));
    assert(!removePackage("plugin_gain", root, error));
    assert(unlink((root + "/plugin_gain").c_str()) == 0);
    for (unsigned i = 0; i < kMaxInstalled; ++i) {
        const std::string id = "plugin_p" + std::to_string(i);
        auto archive = encode(entries(id));
        assert(installPackage(archive.data(), archive.size(), root, error));
    }
    assert(listPackages(root, installed, error) && installed.size() == kMaxInstalled);
    assert(!installPackage(bytes.data(), bytes.size(), root, error));
    for (const auto &item : installed) assert(removePackage(item.id, root, error));
    assert(listPackages(root, installed, error) && installed.empty());
    for (unsigned i = 0; i < 65; ++i) write(root + "/.stage-" + std::to_string(i), "");
    assert(!listPackages(root, installed, error));
    std::filesystem::remove_all(base);
    return 0;
}
