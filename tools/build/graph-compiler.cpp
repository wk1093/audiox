#include "audio/graph_native.hpp"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Object.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/resource.h>
#include <unistd.h>

using namespace audiox::graph;
namespace {
bool transfer(int fd, void *data, size_t size, bool writing) {
    auto *bytes = static_cast<unsigned char *>(data);
    while (size) {
        const ssize_t count = writing ? write(fd, bytes, size) : read(fd, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}
}
int main() {
    // Compilation must never compete with the realtime scheduler.
    if (setpriority(PRIO_PROCESS, 0, 10)) {
        std::fprintf(stderr, "[GRAPH] compiler priority: %s\n", std::strerror(errno));
        return 1;
    }
    Schedule schedule{};
    if (!transfer(STDIN_FILENO, &schedule, sizeof(schedule), false) ||
        !schedule.generation || schedule.count > kMaxOperations) {
        std::fprintf(stderr, "[GRAPH] invalid compiler request\n");
        return 1;
    }
    for (unsigned i = 0; i < schedule.count; ++i)
        if ((schedule.operations[i].kind != OperationKind::Route &&
             schedule.operations[i].kind != OperationKind::Node) ||
            schedule.operations[i].index >=
                (schedule.operations[i].kind == OperationKind::Route ? 256U : 64U)) {
            std::fprintf(stderr, "[GRAPH] invalid compiler operation\n");
            return 1;
        }
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) {
        std::fprintf(stderr, "[GRAPH] LLVM native target initialization failed\n");
        return 1;
    }
    char *triple = LLVMGetDefaultTargetTriple();
    char *cpu = LLVMGetHostCPUName();
    char *features = LLVMGetHostCPUFeatures();
    LLVMTargetRef target{};
    char *error{};
    if (LLVMGetTargetFromTriple(triple, &target, &error)) {
        std::fprintf(stderr, "[GRAPH] LLVM target: %s\n", error);
        LLVMDisposeMessage(error);
        return 1;
    }
    LLVMTargetMachineRef machine = LLVMCreateTargetMachine(target, triple, cpu, features,
        LLVMCodeGenLevelAggressive, LLVMRelocPIC, LLVMCodeModelSmall);
    if (!machine) {
        std::fprintf(stderr, "[GRAPH] LLVM target machine creation failed\n");
        return 1;
    }
    LLVMContextRef context = LLVMContextCreate();
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("audiox_graph", context);
    LLVMSetTarget(module, triple);
    LLVMTargetDataRef layout = LLVMCreateTargetDataLayout(machine);
    char *layoutText = LLVMCopyStringRepOfTargetData(layout);
    LLVMSetDataLayout(module, layoutText);
    LLVMDisposeMessage(layoutText);
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
        LLVMValueRef values[]{LLVMGetParam(entry, 0), LLVMConstInt(integer, op.index, false)};
        LLVMBuildCall2(builder, callback,
            LLVMGetParam(entry, op.kind == OperationKind::Route ? 1 : 2), values, 2, "");
    }
    LLVMBuildRetVoid(builder);
    int result = 1;
    LLVMMemoryBufferRef object{};
    const bool invalidModule = LLVMVerifyModule(module, LLVMReturnStatusAction, &error);
    if (invalidModule)
        std::fprintf(stderr, "[GRAPH] invalid LLVM IR: %s\n", error);
    LLVMDisposeMessage(error);
    error = nullptr;
    if (!invalidModule && LLVMTargetMachineEmitToMemoryBuffer(machine, module, LLVMObjectFile,
                                                  &error, &object)) {
        std::fprintf(stderr, "[GRAPH] LLVM emission: %s\n", error);
        LLVMDisposeMessage(error);
    } else if (!invalidModule) {
        LLVMBinaryRef binary = LLVMCreateBinary(object, context, &error);
        if (!binary) {
            std::fprintf(stderr, "[GRAPH] LLVM object: %s\n", error);
            LLVMDisposeMessage(error);
        } else {
            LLVMSectionIteratorRef section = LLVMObjectFileCopySectionIterator(binary);
            while (!LLVMObjectFileIsSectionIteratorAtEnd(binary, section)) {
                const char *name = LLVMGetSectionName(section);
                if (name && !std::strcmp(name, ".text")) {
                    const uint64_t size = LLVMGetSectionSize(section);
                    LLVMRelocationIteratorRef relocation = LLVMGetRelocations(section);
                    const bool relocatable = LLVMIsRelocationIteratorAtEnd(section, relocation);
                    LLVMDisposeRelocationIterator(relocation);
                    bool entryAtStart = false;
                    LLVMSymbolIteratorRef symbol = LLVMObjectFileCopySymbolIterator(binary);
                    while (!LLVMObjectFileIsSymbolIteratorAtEnd(binary, symbol)) {
                        const char *symbolName = LLVMGetSymbolName(symbol);
                        if (symbolName && !std::strcmp(symbolName, "audiox_graph") &&
                            LLVMGetSectionContainsSymbol(section, symbol) &&
                            LLVMGetSymbolAddress(symbol) == LLVMGetSectionAddress(section)) {
                            entryAtStart = true;
                            break;
                        }
                        LLVMMoveToNextSymbol(symbol);
                    }
                    LLVMDisposeSymbolIterator(symbol);
                    // Only position-independent, self-contained text is executable.
                    if (entryAtStart && relocatable && size && size <= kMaxNativeBytes) {
                        NativeHeader header{kNativeMagic, static_cast<uint32_t>(size)};
                        if (transfer(STDOUT_FILENO, &header, sizeof(header), true) &&
                            transfer(STDOUT_FILENO,
                                     const_cast<char *>(LLVMGetSectionContents(section)), size, true))
                            result = 0;
                    } else {
                        std::fprintf(stderr, "[GRAPH] unsupported LLVM entry/text size/relocations\n");
                    }
                    break;
                }
                LLVMMoveToNextSection(section);
            }
            LLVMDisposeSectionIterator(section);
            LLVMDisposeBinary(binary);
        }
        if (result) std::fprintf(stderr, "[GRAPH] no usable native graph emitted\n");
        LLVMDisposeMemoryBuffer(object);
    }
    LLVMDisposeBuilder(builder);
    LLVMDisposeModule(module);
    LLVMContextDispose(context);
    LLVMDisposeTargetData(layout);
    LLVMDisposeTargetMachine(machine);
    LLVMDisposeMessage(triple);
    LLVMDisposeMessage(cpu);
    LLVMDisposeMessage(features);
    return result;
}
