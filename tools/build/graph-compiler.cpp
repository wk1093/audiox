#include "audio/graph_native.hpp"
#include "audio/plugin_native.hpp"

#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/Core.h>
#include <llvm-c/Linker.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#include <llvm/ExecutionEngine/RuntimeDyld.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <sys/resource.h>
#include <unistd.h>

using namespace audiox::graph;
namespace {
bool transfer(int fd, const void *data, size_t size, bool writing) {
    auto *bytes = static_cast<const unsigned char *>(data);
    while (size) {
        const ssize_t count = writing ? write(fd, bytes, size)
            : read(fd, const_cast<unsigned char *>(bytes), size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}

// Relocations use the runtime's reserved address, never the compiler's own
// libc addresses. No generated code or constructors execute in this process.
class RemoteImage final : public llvm::RTDyldMemoryManager {
    const CompileRequest &request_;
    audiox::plugin::PluginNativeHeader *plugin_;
  public:
    std::vector<uint8_t> bytes = std::vector<uint8_t>(kMaxNativeBytes);
    std::vector<uint8_t *> sections;
    size_t size{};
    bool invalid{};
    explicit RemoteImage(const CompileRequest &request,
                         audiox::plugin::PluginNativeHeader *plugin = nullptr)
        : request_(request), plugin_(plugin) {}
    uint8_t *allocate(uintptr_t length, unsigned alignment, bool executable = false) {
        if (plugin_) alignment = std::max(alignment, static_cast<unsigned>(sysconf(_SC_PAGESIZE)));
        alignment = std::max(alignment, 16U);
        const size_t offset = (size + alignment - 1) & ~(static_cast<size_t>(alignment) - 1);
        if (offset > bytes.size() || length > bytes.size() - offset) {
            std::fprintf(stderr, "[GRAPH] native DSP image exceeds capacity\n");
            std::exit(1);
        }
        auto *memory = bytes.data() + offset;
        size = offset + length;
        if (plugin_) {
            if (plugin_->sectionCount == audiox::plugin::kMaxPluginSections) {
                invalid = true;
                return memory;
            }
            plugin_->sections[plugin_->sectionCount++] = {
                static_cast<uint32_t>(offset), static_cast<uint32_t>(length),
                static_cast<uint32_t>(executable)};
        }
        sections.push_back(memory);
        return memory;
    }
    uint8_t *allocateCodeSection(uintptr_t size, unsigned alignment, unsigned,
                                 llvm::StringRef) override { return allocate(size, alignment, true); }
    uint8_t *allocateDataSection(uintptr_t size, unsigned alignment, unsigned,
                                 llvm::StringRef name, bool readOnly) override {
        if (!plugin_ && !readOnly && size && name != ".got") {
            std::fprintf(stderr, "[GRAPH] mutable DSP section rejected: %s\n", name.str().c_str());
            invalid = true;
        }
        return allocate(size, alignment);
    }
    void registerEHFrames(uint8_t *, uint64_t, size_t) override {}
    void deregisterEHFrames() override {}
    bool finalizeMemory(std::string *) override { return invalid; }
    uint64_t getSymbolAddress(const std::string &name) override {
        for (unsigned i = 0; i < request_.importCount; ++i)
            if (name == request_.imports[i].name) return request_.imports[i].address;
        std::fprintf(stderr, "[GRAPH] unresolved DSP import: %s\n", name.c_str());
        invalid = true;
        return 0;
    }
};

bool compilePlugin(const CompileRequest &request, const char *path) {
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) return false;
    LLVMContextRef context = LLVMContextCreate();
    LLVMMemoryBufferRef input{};
    LLVMModuleRef module{};
    char *error{};
    bool ok = !LLVMCreateMemoryBufferWithContentsOfFile(path, &input, &error);
    if (!ok) {
        std::fprintf(stderr, "[PLUGIN] bitcode input: %s\n", error);
        LLVMDisposeMessage(error);
    }
    if (ok) {
        ok = !LLVMParseBitcodeInContext2(context, input, &module);
        LLVMDisposeMemoryBuffer(input);
        if (!ok) std::fprintf(stderr, "[PLUGIN] invalid LLVM bitcode\n");
    }
    char *triple = LLVMGetDefaultTargetTriple();
    LLVMTargetRef target{};
    LLVMTargetMachineRef machine{};
    if (ok) {
        const std::string native = triple, supplied = LLVMGetTarget(module);
        ok = supplied.substr(0, supplied.find('-')) == native.substr(0, native.find('-'));
        if (!ok) std::fprintf(stderr, "[PLUGIN] bitcode architecture mismatch; rebuild required\n");
    }
    if (ok) {
        const auto entry = LLVMGetNamedFunction(module, "audiox_plugin_v1");
        ok = entry && LLVMCountBasicBlocks(entry) &&
             LLVMGetTypeKind(LLVMGetReturnType(LLVMGlobalGetValueType(entry))) == LLVMPointerTypeKind &&
             LLVMCountParams(entry) == 0;
        if (!ok) std::fprintf(stderr, "[PLUGIN] missing or invalid audiox_plugin_v1 ABI entry\n");
        if (LLVMGetNamedGlobal(module, "llvm.global_ctors") ||
            LLVMGetNamedGlobal(module, "llvm.global_dtors")) {
            std::fprintf(stderr, "[PLUGIN] global constructors/destructors are unsupported\n");
            ok = false;
        }
        for (auto global = LLVMGetFirstGlobal(module); global; global = LLVMGetNextGlobal(global))
            if (LLVMIsThreadLocal(global)) {
                std::fprintf(stderr, "[PLUGIN] thread-local globals are unsupported\n");
                ok = false;
            }
    }
    if (ok) {
        error = nullptr;
        ok = !LLVMVerifyModule(module, LLVMReturnStatusAction, &error);
        if (!ok) std::fprintf(stderr, "[PLUGIN] invalid bitcode IR: %s\n", error);
        LLVMDisposeMessage(error);
    }
    if (ok) {
        error = nullptr;
        ok = !LLVMGetTargetFromTriple(triple, &target, &error);
        if (!ok) {
            std::fprintf(stderr, "[PLUGIN] native target: %s\n", error);
            LLVMDisposeMessage(error);
        }
    }
    if (ok) {
        char *cpu = LLVMGetHostCPUName(), *features = LLVMGetHostCPUFeatures();
        machine = LLVMCreateTargetMachine(target, triple, cpu, features,
            LLVMCodeGenLevelAggressive, LLVMRelocPIC, LLVMCodeModelLarge);
        LLVMDisposeMessage(cpu);
        LLVMDisposeMessage(features);
        ok = machine;
        for (auto fn = LLVMGetFirstFunction(module); fn; fn = LLVMGetNextFunction(fn)) {
            LLVMRemoveStringAttributeAtIndex(fn, LLVMAttributeFunctionIndex, "target-cpu", 10);
            LLVMRemoveStringAttributeAtIndex(fn, LLVMAttributeFunctionIndex, "target-features", 15);
        }
    }
    LLVMMemoryBufferRef object{};
    if (ok) {
        error = nullptr;
        ok = !LLVMTargetMachineEmitToMemoryBuffer(machine, module, LLVMObjectFile, &error, &object);
        if (!ok) std::fprintf(stderr, "[PLUGIN] native emission: %s\n", error);
        LLVMDisposeMessage(error);
    }
    if (ok) {
        auto parsed = llvm::object::ObjectFile::createObjectFile(
            llvm::MemoryBufferRef(llvm::StringRef(LLVMGetBufferStart(object), LLVMGetBufferSize(object)), "plugin"));
        if (!parsed) {
            llvm::logAllUnhandledErrors(parsed.takeError(), llvm::errs(), "[PLUGIN] object: ");
            ok = false;
        } else {
            audiox::plugin::PluginNativeHeader header{};
            RemoteImage image(request, &header);
            llvm::RuntimeDyld linker(image, image);
            linker.loadObject(**parsed);
            for (auto *section : image.sections)
                linker.mapSectionAddress(section, request.address + (section - image.bytes.data()));
            linker.resolveRelocations();
            const auto entry = linker.getSymbol("audiox_plugin_v1").getAddress();
            ok = !image.invalid && !linker.hasError() && entry >= request.address &&
                 entry - request.address < image.size;
            if (linker.hasError())
                std::fprintf(stderr, "[PLUGIN] relocation failure: %s\n", linker.getErrorString().str().c_str());
            header.magic = audiox::plugin::kPluginNativeMagic;
            header.size = static_cast<uint32_t>(image.size);
            header.entryOffset = static_cast<uint32_t>(entry - request.address);
            if (ok) ok = transfer(STDOUT_FILENO, &header, sizeof(header), true) &&
                         transfer(STDOUT_FILENO, image.bytes.data(), image.size, true);
        }
    }
    if (object) LLVMDisposeMemoryBuffer(object);
    if (machine) LLVMDisposeTargetMachine(machine);
    if (module) LLVMDisposeModule(module);
    LLVMDisposeMessage(triple);
    LLVMContextDispose(context);
    return ok;
}

bool compile(const CompileRequest &request, const char *directory) {
    const auto &schedule = request.schedule;
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) return false;
    char *triple = LLVMGetDefaultTargetTriple();
    const std::string nativeTriple = triple;
    const auto nativeArch = nativeTriple.substr(0, nativeTriple.find('-'));
    char *cpu = LLVMGetHostCPUName();
    char *features = LLVMGetHostCPUFeatures();
    LLVMTargetRef target{};
    char *error{};
    if (LLVMGetTargetFromTriple(triple, &target, &error)) {
        std::fprintf(stderr, "[GRAPH] LLVM target: %s\n", error);
        LLVMDisposeMessage(error);
        return false;
    }
    LLVMTargetMachineRef machine = LLVMCreateTargetMachine(target, triple, cpu, features,
        LLVMCodeGenLevelAggressive, LLVMRelocPIC, LLVMCodeModelLarge);
    LLVMDisposeMessage(triple);
    LLVMDisposeMessage(cpu);
    LLVMDisposeMessage(features);
    if (!machine) return false;
    LLVMContextRef context = LLVMContextCreate();
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("audiox_graph", context);
    const char *sources[]{"gain", "distortion", "pitch", "reverb", "gate", "dynamics",
                          "equalizer", "noise_reduction", "native_kernels", "native_graph"};
    bool ok = true;
    for (const char *source : sources) {
        const std::string path = std::string(directory) + "/" + source + ".bc";
        LLVMMemoryBufferRef input{};
        LLVMModuleRef kernel{};
        if (LLVMCreateMemoryBufferWithContentsOfFile(path.c_str(), &input, &error)) {
            std::fprintf(stderr, "[GRAPH] DSP bitcode %s: %s\n", path.c_str(), error);
            LLVMDisposeMessage(error);
            error = nullptr;
            ok = false;
            break;
        }
        if (LLVMParseBitcodeInContext2(context, input, &kernel)) {
            std::fprintf(stderr, "[GRAPH] invalid DSP bitcode: %s\n", path.c_str());
            ok = false;
        } else {
            const std::string kernelTriple = LLVMGetTarget(kernel);
            if (kernelTriple.substr(0, kernelTriple.find('-')) != nativeArch) {
                std::fprintf(stderr, "[GRAPH] DSP bitcode architecture mismatch: %s\n", path.c_str());
                LLVMDisposeModule(kernel);
                ok = false;
            } else if (LLVMLinkModules2(module, kernel)) {
                std::fprintf(stderr, "[GRAPH] DSP bitcode link failed: %s\n", path.c_str());
                ok = false;
            }
        }
        LLVMDisposeMemoryBuffer(input);
        if (!ok) break;
    }
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef integer = LLVMInt32TypeInContext(context);
    LLVMTypeRef callbackArgs[]{pointer, integer};
    LLVMTypeRef callback = LLVMFunctionType(LLVMVoidTypeInContext(context), callbackArgs, 2, false);
    LLVMTypeRef args[]{pointer, pointer, pointer};
    LLVMTypeRef signature = LLVMFunctionType(LLVMVoidTypeInContext(context), args, 3, false);
    LLVMValueRef entry = LLVMAddFunction(module, "audiox_graph", signature);
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
    LLVMPositionBuilderAtEnd(builder, LLVMAppendBasicBlockInContext(context, entry, "entry"));
    for (unsigned i = 0; i < schedule.count; ++i) {
        const auto &op = schedule.operations[i];
        if (schedule.direct) {
            if (op.kind == OperationKind::Route) {
                const auto &route = schedule.routes[op.index];
                LLVMValueRef fn = LLVMGetNamedFunction(module, "audiox_native_route");
                LLVMValueRef values[]{LLVMGetParam(entry, 0), LLVMConstInt(integer, route.src, false),
                    LLVMConstInt(integer, route.dst, false), LLVMConstInt(integer, route.srcChannel, false),
                    LLVMConstInt(integer, route.dstChannel, false)};
                if (!fn) { ok = false; break; }
                LLVMBuildCall2(builder, LLVMGlobalGetValueType(fn), fn, values, 5, "");
            } else {
                LLVMValueRef fn = LLVMGetNamedFunction(module, "audiox_native_node");
                LLVMValueRef values[]{LLVMGetParam(entry, 0), LLVMConstInt(integer, op.index, false)};
                if (!fn) { ok = false; break; }
                LLVMBuildCall2(builder, LLVMGlobalGetValueType(fn), fn, values, 2, "");
            }
        } else {
            LLVMValueRef values[]{LLVMGetParam(entry, 0), LLVMConstInt(integer, op.index, false)};
            LLVMBuildCall2(builder, callback,
                LLVMGetParam(entry, op.kind == OperationKind::Route ? 1 : 2), values, 2, "");
        }
    }
    LLVMBuildRetVoid(builder);
    for (const auto &setting : {std::pair<const char *, uint32_t>{"audiox_dsp_sample_rate", schedule.sampleRate},
                               {"audiox_dsp_block_frames", schedule.frames}}) {
        LLVMValueRef global = LLVMGetNamedGlobal(module, setting.first);
        LLVMValueRef value = global ? LLVMGetInitializer(global) : nullptr;
        if (!value || !LLVMIsAConstantInt(value) || LLVMConstIntGetZExtValue(value) != setting.second) {
            std::fprintf(stderr, "[GRAPH] DSP bitcode configuration mismatch: %s\n", setting.first);
            ok = false;
        }
    }
    // Only entry points remain externally visible; O3 can inline the actual DSP
    // implementations into their typed wrappers and eliminate unused helpers.
    for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn; fn = LLVMGetNextFunction(fn)) {
        const char *name = LLVMGetValueName(fn);
        if (!std::strncmp(name, "audiox_native_", 14)) {
            const unsigned kind = LLVMGetEnumAttributeKindForName("alwaysinline", 12);
            LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(context, kind, 0));
        }
        LLVMRemoveStringAttributeAtIndex(fn, LLVMAttributeFunctionIndex, "target-cpu", 10);
        LLVMRemoveStringAttributeAtIndex(fn, LLVMAttributeFunctionIndex, "target-features", 15);
        if (!std::strncmp(name, "audiox_dsp_", 11) && LLVMCountParams(fn) == 6)
            LLVMReplaceAllUsesWith(LLVMGetParam(fn, 2), LLVMConstInt(integer, schedule.frames, false));
        if (LLVMCountBasicBlocks(fn) && std::strcmp(name, "audiox_graph") &&
            std::strncmp(name, "audiox_dsp_", 11))
            LLVMSetLinkage(fn, LLVMInternalLinkage);
    }
    LLVMMemoryBufferRef object{};
    if (ok && LLVMVerifyModule(module, LLVMReturnStatusAction, &error)) {
        std::fprintf(stderr, "[GRAPH] invalid DSP IR: %s\n", error);
        ok = false;
    }
    LLVMDisposeMessage(error);
    error = nullptr;
    if (ok) {
        LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
        LLVMErrorRef passError = LLVMRunPasses(module, "default<O3>", machine, options);
        LLVMDisposePassBuilderOptions(options);
        if (passError) {
            char *text = LLVMGetErrorMessage(passError);
            std::fprintf(stderr, "[GRAPH] DSP optimization: %s\n", text);
            LLVMDisposeErrorMessage(text);
            ok = false;
        }
    }
    if (ok && LLVMTargetMachineEmitToMemoryBuffer(machine, module, LLVMObjectFile, &error, &object)) {
        std::fprintf(stderr, "[GRAPH] DSP emission: %s\n", error);
        LLVMDisposeMessage(error);
        ok = false;
    }
    if (ok) {
        auto parsed = llvm::object::ObjectFile::createObjectFile(
            llvm::MemoryBufferRef(llvm::StringRef(LLVMGetBufferStart(object), LLVMGetBufferSize(object)), "graph"));
        if (!parsed) {
            llvm::logAllUnhandledErrors(parsed.takeError(), llvm::errs(), "[GRAPH] DSP object: ");
            ok = false;
        } else {
            RemoteImage image(request);
            llvm::RuntimeDyld linker(image, image);
            linker.loadObject(**parsed);
            for (auto *section : image.sections)
                linker.mapSectionAddress(section, request.address + (section - image.bytes.data()));
            linker.resolveRelocations();
            ok = !image.invalid && !linker.hasError();
            if (linker.hasError())
                std::fprintf(stderr, "[GRAPH] DSP relocations: %s\n", linker.getErrorString().str().c_str());
            NativeHeader header{};
            header.magic = kNativeMagic;
            header.size = static_cast<uint32_t>(image.size);
            const auto offset = [&](const std::string &name, uint32_t &out) {
                const uint64_t address = linker.getSymbol(name).getAddress();
                if (address < request.address || address - request.address >= image.size) return false;
                out = static_cast<uint32_t>(address - request.address);
                return true;
            };
            ok = ok && offset("audiox_graph", header.entryOffset);
            for (unsigned i = 0; i < audiox::effects::kBuiltinDspCount; ++i)
                ok = offset("audiox_dsp_" + std::to_string(i), header.dspOffsets[i]) && ok;
            if (ok) ok = transfer(STDOUT_FILENO, &header, sizeof(header), true) &&
                         transfer(STDOUT_FILENO, image.bytes.data(), image.size, true);
        }
        LLVMDisposeMemoryBuffer(object);
    }
    LLVMDisposeBuilder(builder);
    LLVMDisposeModule(module);
    LLVMContextDispose(context);
    LLVMDisposeTargetMachine(machine);
    return ok;
}
}

int main(int argc, char **argv) {
    if (setpriority(PRIO_PROCESS, 0, 10)) {
        std::fprintf(stderr, "[GRAPH] compiler priority: %s\n", std::strerror(errno));
        return 1;
    }
    CompileRequest request{};
    if (argc == 3 && !std::strcmp(argv[1], "--plugin")) {
        if (!transfer(STDIN_FILENO, &request, sizeof(request), false) ||
            !request.address || request.importCount > kMaxImports) return 1;
        for (unsigned i = 0; i < request.importCount; ++i)
            if (!std::memchr(request.imports[i].name, 0, sizeof(request.imports[i].name))) return 1;
        return compilePlugin(request, argv[2]) ? 0 : 1;
    }
    if (argc != 2 || !transfer(STDIN_FILENO, &request, sizeof(request), false) ||
        !request.schedule.generation || request.schedule.count > kMaxOperations || !request.schedule.frames ||
        !request.address || request.importCount > kMaxImports) {
        std::fprintf(stderr, "[GRAPH] invalid compiler request\n");
        return 1;
    }
    for (unsigned i = 0; i < request.importCount; ++i)
        if (!std::memchr(request.imports[i].name, 0, sizeof(request.imports[i].name))) return 1;
    for (unsigned i = 0; i < request.schedule.count; ++i) {
        const auto &op = request.schedule.operations[i];
        if ((op.kind != OperationKind::Route && op.kind != OperationKind::Node) ||
            op.index >= (op.kind == OperationKind::Route ? 256U : 64U)) {
            std::fprintf(stderr, "[GRAPH] invalid compiler operation\n");
            return 1;
        }
        if (request.schedule.direct && op.kind == OperationKind::Route) {
            const auto &route = request.schedule.routes[op.index];
            if (route.src >= kGraphNodes || route.dst >= kGraphNodes ||
                route.srcChannel >= kGraphChannels || route.dstChannel >= kGraphChannels) {
                std::fprintf(stderr, "[GRAPH] invalid direct route\n");
                return 1;
            }
        }
    }
    const bool ok = compile(request, argv[1]);
    if (!ok) std::fprintf(stderr, "[GRAPH] no usable native DSP graph emitted\n");
    return ok ? 0 : 1;
}
