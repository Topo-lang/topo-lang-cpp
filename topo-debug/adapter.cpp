// topo-debug-{cpp,rust} — real-lldb Extract adapter.
//
// Drives an lldb-debuggable host binary (C++ via -g / Rust via -C debuginfo=2)
// through liblldb, hits a single line-breakpoint, reads a chosen local
// variable from the stopped frame, and emits the bytes + layout descriptor
// on stdout using the Extract wire protocol. The same source file is
// built into two executables (`topo-debug-cpp` and `topo-debug-rust`) — the
// adapter logic is host-language agnostic; the program name is taken from
// `argv[0]` so stderr messages identify themselves correctly.
//
// CLI:
//   topo-debug-cpp --site <file:line> --target <bin>
//                  [--var <name>] [-- <target-args>...]
//
// Wire output (in order, all on stdout):
//   1. JSON line  {"kind":"breakpoint_hit","frame":1,"site":"..."}
//   2. binary frame  type=var_bytes        — raw little-endian bytes of the var
//   3. binary frame  type=layout_descriptor — JSON body {variable,dtype,shape,strides}
//
// Then reads one JSON line `{"op":"continue"}` from stdin and continues the
// process to exit. A 10 s wall clock guards the breakpoint wait.
//
// `--var <name>` selects which in-scope variable to read (default
// `matrix` when no flag is given).
// The driving CLI (`topo debug`) extracts the variable identifier from the
// query AST and passes it through. Supported types: primitive ints/floats
// and N-dimensional arrays of those; anything else exits 4 with a diagnostic.
//
// Lifetime: the entire process wraps lldb::SBDebugger::{Initialize,Terminate}
// in an RAII guard so a failed launch still terminates the debugger cleanly.

#include "topo/Debug/Ipc/BinaryFrame.h"
#include "topo/Debug/Ipc/FrameWriter.h"

#include "lldb/API/SBBreakpoint.h"
#include "lldb/API/SBCommandInterpreter.h"
#include "lldb/API/SBCommandReturnObject.h"
#include "lldb/API/SBData.h"
#include "lldb/API/SBDebugger.h"
#include "lldb/API/SBError.h"
#include "lldb/API/SBEvent.h"
#include "lldb/API/SBFrame.h"
#include "lldb/API/SBLaunchInfo.h"
#include "lldb/API/SBListener.h"
#include "lldb/API/SBProcess.h"
#include "lldb/API/SBStream.h"
#include "lldb/API/SBTarget.h"
#include "lldb/API/SBThread.h"
#include "lldb/API/SBType.h"
#include "lldb/API/SBValue.h"
#include "lldb/lldb-enumerations.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

using topo::debug_ipc::BinaryFrame;
using topo::debug_ipc::BinaryFrameType;
using topo::debug_ipc::ByteSink;
using topo::debug_ipc::byteSinkToStdout;
using topo::debug_ipc::writeBinaryFrame;
using topo::debug_ipc::writeJsonLine;
using nlohmann::json;

namespace {

// Exit codes (4 means "variable type unsupported"):
//   0  ok
//   1  CLI / usage / IO error
//   2  target not found / launch failed
//   3  breakpoint never hit / runtime error
//   4  variable type unsupported

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitLaunch = 2;
constexpr int kExitRuntime = 3;
constexpr int kExitUnsupportedType = 4;

constexpr int kBreakpointWaitMs = 10000;

struct Args {
    std::string site;
    std::string target;
    // Comma-separated list of variable names; defaults to the single-var
    // path "matrix" when no flag is given.
    // The CLI joins all distinct variables referenced in the query AST and
    // passes them here; we emit one VarBytes + LayoutDescriptor pair per
    // entry, in the order received.
    std::vector<std::string> vars;
    std::vector<std::string> targetArgs;
};

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

// Program name set in main() from basename(argv[0]); used in usage + error
// strings so the same binary source can ship as topo-debug-cpp or
// topo-debug-rust without per-binary text edits. Falls back to a stable
// label so anyone reading raw stderr still sees something usable.
std::string g_progName = "topo-debug-cpp";

void printUsage(std::FILE* out) {
    std::fprintf(out,
                 "Usage: %s --site <file:line> --target <binary>\n"
                 "       %*s [--var <name>] [-- <target-args>...]\n",
                 g_progName.c_str(),
                 static_cast<int>(g_progName.size()), "");
}

bool parseArgs(int argc, char** argv, Args& a, std::string& err) {
    bool inTargetArgs = false;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (inTargetArgs) {
            a.targetArgs.push_back(s);
            continue;
        }
        if (s == "--") { inTargetArgs = true; continue; }
        if (s == "-h" || s == "--help") {
            printUsage(stdout);
            std::exit(kExitOk);
        }
        auto eat = [&](const std::string& flag, std::string& dest) -> bool {
            if (s.rfind(flag + "=", 0) == 0) {
                dest = s.substr(flag.size() + 1);
                return true;
            }
            if (s == flag && i + 1 < argc) {
                dest = argv[++i];
                return true;
            }
            return false;
        };
        if (eat("--site", a.site)) continue;
        if (eat("--target", a.target)) continue;
        std::string varCsv;
        if (eat("--var", varCsv)) {
            auto names = splitCsv(varCsv);
            if (names.empty()) { err = "--var list is empty"; return false; }
            for (auto& n : names) {
                if (n.empty()) { err = "--var: empty entry in CSV list"; return false; }
                a.vars.push_back(std::move(n));
            }
            continue;
        }
        err = "unknown argument: " + s;
        return false;
    }
    if (a.site.empty()) { err = "--site is required"; return false; }
    if (a.target.empty()) { err = "--target is required"; return false; }
    if (a.vars.empty()) {
        // Default to single-var "matrix" so existing callers
        // (`tiny_matrix` smoke tests) don't need a flag.
        a.vars.push_back("matrix");
    }
    return true;
}

// Split "file:line" into components. Accepts paths that themselves contain a
// colon (rare on POSIX, common on Windows drive letters) by splitting on the
// *last* ':'. Returns false if `line` is not a positive integer.
bool splitSite(const std::string& site, std::string& file, uint32_t& line,
               std::string& err) {
    auto pos = site.find_last_of(':');
    if (pos == std::string::npos) {
        err = "site '" + site + "' missing ':' (expected file:line)";
        return false;
    }
    file = site.substr(0, pos);
    std::string lineStr = site.substr(pos + 1);
    if (lineStr.empty()) { err = "site '" + site + "' missing line"; return false; }
    try {
        line = static_cast<uint32_t>(std::stoul(lineStr));
    } catch (const std::exception&) {
        err = "site '" + site + "' line is not a number";
        return false;
    }
    if (line == 0) { err = "site '" + site + "' line must be >= 1"; return false; }
    return true;
}

// RAII guard around lldb::SBDebugger::{Initialize,Terminate}.
struct LldbGuard {
    LldbGuard() { lldb::SBDebugger::Initialize(); }
    ~LldbGuard() { lldb::SBDebugger::Terminate(); }
    LldbGuard(const LldbGuard&) = delete;
    LldbGuard& operator=(const LldbGuard&) = delete;
};

// Map an lldb BasicType to our wire dtype string. Returns empty string for
// unsupported types.
std::string basicTypeToDtype(lldb::BasicType bt, uint64_t byteSize) {
    using lldb::BasicType;
    switch (bt) {
        case BasicType::eBasicTypeChar:
        case BasicType::eBasicTypeSignedChar:
            return "i8";
        case BasicType::eBasicTypeUnsignedChar:
            return "u8";
        case BasicType::eBasicTypeShort:
            return byteSize == 2 ? "i16" : "";
        case BasicType::eBasicTypeUnsignedShort:
            return byteSize == 2 ? "u16" : "";
        case BasicType::eBasicTypeInt:
            // C++ `int` on every platform we ship is 32-bit; defend with size.
            return byteSize == 4 ? "i32" : (byteSize == 8 ? "i64" : "");
        case BasicType::eBasicTypeUnsignedInt:
            return byteSize == 4 ? "u32" : (byteSize == 8 ? "u64" : "");
        case BasicType::eBasicTypeLong:
            return byteSize == 8 ? "i64" : (byteSize == 4 ? "i32" : "");
        case BasicType::eBasicTypeUnsignedLong:
            return byteSize == 8 ? "u64" : (byteSize == 4 ? "u32" : "");
        case BasicType::eBasicTypeLongLong:
            return "i64";
        case BasicType::eBasicTypeUnsignedLongLong:
            return "u64";
        case BasicType::eBasicTypeFloat:
            return byteSize == 4 ? "f32" : "";
        case BasicType::eBasicTypeDouble:
            return byteSize == 8 ? "f64" : "";
        default:
            return "";
    }
}

// Walk an SBType peeling array dimensions until a non-array element is left.
// Populates `shape` (outermost first) and returns the leaf element type.
lldb::SBType unwrapArrayShape(lldb::SBType t, std::vector<int64_t>& shape) {
    while (t.IsValid() && t.IsArrayType()) {
        // SBType lacks GetArrayElementCount(); recover the dimension from the
        // byte-size ratio. Both sides come from DWARF so this is exact.
        lldb::SBType elem = t.GetArrayElementType();
        uint64_t total = t.GetByteSize();
        uint64_t elemSize = elem.GetByteSize();
        if (elemSize == 0) {
            shape.push_back(0);
            return elem;
        }
        shape.push_back(static_cast<int64_t>(total / elemSize));
        t = elem;
    }
    return t;
}

// Compute row-major strides (in bytes) from a shape and an element-byte-size.
// strides[i] = product(shape[i+1..]) * elemSize.
std::vector<int64_t> rowMajorStrides(const std::vector<int64_t>& shape,
                                     int64_t elemSize) {
    std::vector<int64_t> strides(shape.size(), 0);
    if (shape.empty()) return strides;
    int64_t acc = elemSize;
    for (size_t i = shape.size(); i-- > 0;) {
        strides[i] = acc;
        acc *= shape[i];
    }
    return strides;
}

// LayoutDescriptor v2 aggregate-leaf support.
//
// If `t` is a struct/class of primitive scalar fields, fill `block` with the
// v2 `struct` JSON object: {name, stride_bytes, fields:[{name,offset,dtype}]}.
// Returns "" on success; otherwise returns a human-readable reason for why
// this aggregate is rejected (bitfield, nested aggregate, pointer/array
// field, unmapped basic type, zero fields). The caller treats non-empty
// errors as the "unsupported variable type" failure path and exits with
// kExitUnsupportedType via dieType(), matching how primitive leaves are
// rejected today (see basicTypeToDtype loop in main()).
//
// One-level only: any field whose own type IsAggregateType() or IsArrayType()
// or IsPointerType() / IsReferenceType() rejects the whole variable. Future
// work: recursive struct flattening and pointer-following.
std::string tryBuildStructBlock(lldb::SBType t, json& block) {
    if (!t.IsValid()) return "leaf type invalid";
    // IsAggregateType also includes arrays/unions; we already peel arrays
    // outside, and union handling needs different semantics (overlapping
    // offsets). Use GetNumberOfFields > 0 to filter to record types.
    if (!t.IsAggregateType()) return "leaf type is not an aggregate";
    uint32_t numFields = t.GetNumberOfFields();
    if (numFields == 0) {
        return "aggregate type has zero DWARF-visible fields";
    }
    // Reject unions: fields share offset 0. A union member offset check
    // would also catch them, but probing via IsTypeComplete + class kind is
    // unreliable across lldb versions, so we infer by duplicate-offset.
    std::vector<bool> seenOffset;
    json fieldsArr = json::array();
    uint64_t prevEnd = 0;
    for (uint32_t i = 0; i < numFields; ++i) {
        lldb::SBTypeMember m = t.GetFieldAtIndex(i);
        if (!m.IsValid()) {
            return "field index " + std::to_string(i) + " is invalid";
        }
        if (m.IsBitfield()) {
            return std::string("field '") + (m.GetName() ? m.GetName() : "?") +
                   "' is a bitfield (unsupported)";
        }
        lldb::SBType ft = m.GetType();
        if (!ft.IsValid()) {
            return std::string("field '") + (m.GetName() ? m.GetName() : "?") +
                   "' has invalid type";
        }
        if (ft.IsAggregateType()) {
            return std::string("field '") + (m.GetName() ? m.GetName() : "?") +
                   "' is a nested aggregate/array (one-level only; "
                   "nested aggregates are future work)";
        }
        if (ft.IsPointerType() || ft.IsReferenceType()) {
            return std::string("field '") + (m.GetName() ? m.GetName() : "?") +
                   "' is a pointer/reference (unsupported)";
        }
        std::string fdtype = basicTypeToDtype(ft.GetBasicType(), ft.GetByteSize());
        if (fdtype.empty()) {
            const char* tn = ft.GetName();
            return std::string("field '") + (m.GetName() ? m.GetName() : "?") +
                   "' type '" + (tn ? tn : "<unknown>") +
                   "' does not map to a primitive dtype";
        }
        uint64_t off = m.GetOffsetInBytes();
        // Duplicate-offset → union-like overlay. A struct's first field
        // sits at 0; subsequent fields must have strictly greater offsets.
        if (i > 0 && off < prevEnd) {
            return std::string("field '") + (m.GetName() ? m.GetName() : "?") +
                   "' offset " + std::to_string(off) +
                   " overlaps prior field end " + std::to_string(prevEnd) +
                   " (union-like layout unsupported)";
        }
        prevEnd = off + ft.GetByteSize();
        json fj = {
            {"name", m.GetName() ? m.GetName() : ""},
            {"offset", off},
            {"dtype", fdtype},
        };
        fieldsArr.push_back(std::move(fj));
    }
    const char* tname = t.GetName();
    block = {
        {"name", tname ? tname : ""},
        {"stride_bytes", t.GetByteSize()},
        {"fields", std::move(fieldsArr)},
    };
    return "";
}

int dieUsage(const std::string& m) {
    std::fprintf(stderr, "%s: %s\n", g_progName.c_str(), m.c_str());
    printUsage(stderr);
    return kExitUsage;
}

int dieLaunch(const std::string& m) {
    std::fprintf(stderr, "%s: %s\n", g_progName.c_str(), m.c_str());
    return kExitLaunch;
}

int dieRuntime(const std::string& m) {
    std::fprintf(stderr, "%s: %s\n", g_progName.c_str(), m.c_str());
    return kExitRuntime;
}

int dieType(const std::string& m) {
    std::fprintf(stderr, "%s: %s\n", g_progName.c_str(), m.c_str());
    return kExitUnsupportedType;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 0 && argv[0]) {
        // Strip directory + .exe suffix (Windows) for clean stderr prefix.
        std::filesystem::path p(argv[0]);
        std::string stem = p.stem().string();
        if (!stem.empty()) g_progName = std::move(stem);
    }

    Args args;
    std::string err;
    if (!parseArgs(argc, argv, args, err)) return dieUsage(err);

    std::string siteFile;
    uint32_t siteLine = 0;
    if (!splitSite(args.site, siteFile, siteLine, err)) return dieUsage(err);

    LldbGuard guard;

    lldb::SBDebugger debugger = lldb::SBDebugger::Create();
    if (!debugger.IsValid()) {
        return dieLaunch("failed to create SBDebugger");
    }
    // Keep lldb's own diagnostics off the IPC channel. By default SBDebugger
    // writes warnings (e.g. "main was compiled with optimization — stepping
    // may behave oddly") to stdout, which contaminates the binary frame
    // stream the topo-debug CLI is parsing. Smoke-test fixtures built `-O0`
    // emit no such warning, but project-mode
    // `topo build` defaults to `-O2 -g` so the warning fires every time.
    // Routing both channels to stderr preserves diagnostics for humans
    // without poisoning the wire.
    debugger.SetOutputFileHandle(stderr, /*transfer_ownership=*/false);
    debugger.SetErrorFileHandle(stderr, /*transfer_ownership=*/false);
    // Silence lldb's "compiled with optimization — stepping may behave oddly"
    // chatter. We're not stepping (we set one breakpoint and read locals
    // exactly once), and dev-mode `topo build` defaults to `-O2 -g`, so the
    // warning fires every time and is pure noise to users of `topo debug`.
    {
        lldb::SBCommandReturnObject ret;
        debugger.GetCommandInterpreter().HandleCommand(
            "settings set target.process.optimization-warnings false", ret);
    }
    // Sync mode: every state transition that crosses an inferior boundary
    // (launch, continue, resume) blocks until the inferior is next stopped.
    // We pair this with a polling loop on the listener for any extra events
    // (such as an entry stop with eStopReasonNone) so we can resume past
    // them without racing the SBProcess state machine.
    debugger.SetAsync(false);

    lldb::SBError createErr;
    lldb::SBTarget target =
        debugger.CreateTarget(args.target.c_str(), /*arch=*/nullptr,
                              /*platform=*/nullptr, /*add_dep_modules=*/true,
                              createErr);
    if (!target.IsValid() || createErr.Fail()) {
        std::string m = "failed to create target for '" + args.target + "'";
        if (createErr.GetCString()) {
            m += std::string(": ") + createErr.GetCString();
        }
        return dieLaunch(m);
    }

    lldb::SBBreakpoint bp =
        target.BreakpointCreateByLocation(siteFile.c_str(), siteLine);
    if (!bp.IsValid() || bp.GetNumLocations() == 0) {
        return dieLaunch("no breakpoint locations resolved for '" + args.site +
                         "' — was the binary built with -g and is the file basename unique?");
    }

    // Launch. Suppress child stdio to keep stdout pristine for our IPC stream.
    std::vector<const char*> targetArgvC;
    for (const auto& s : args.targetArgs) targetArgvC.push_back(s.c_str());
    targetArgvC.push_back(nullptr);

    lldb::SBLaunchInfo launchInfo(args.targetArgs.empty() ? nullptr
                                                          : targetArgvC.data());
    launchInfo.AddSuppressFileAction(/*fd=*/0, /*read=*/true, /*write=*/false);
    launchInfo.AddSuppressFileAction(/*fd=*/1, /*read=*/false, /*write=*/true);
    launchInfo.AddSuppressFileAction(/*fd=*/2, /*read=*/false, /*write=*/true);

    lldb::SBListener listener("topo-debug-cpp");
    launchInfo.SetListener(listener);

    lldb::SBError launchErr;
    lldb::SBProcess process = target.Launch(launchInfo, launchErr);
    if (!process.IsValid() || launchErr.Fail()) {
        std::string m = "launch failed";
        if (launchErr.GetCString()) m += std::string(": ") + launchErr.GetCString();
        return dieLaunch(m);
    }

    // In sync mode `target.Launch` returns once the inferior is stopped for
    // the first time. Loop: if any thread has eStopReasonBreakpoint, take it;
    // otherwise resume and wait for the next sync stop. A wall-clock deadline
    // bounds the total wait.
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kBreakpointWaitMs);
    auto findBreakpointThread = [&]() -> lldb::SBThread {
        uint32_t n = process.GetNumThreads();
        for (uint32_t i = 0; i < n; ++i) {
            lldb::SBThread t = process.GetThreadAtIndex(i);
            if (t.IsValid() && t.GetStopReason() == lldb::eStopReasonBreakpoint) {
                return t;
            }
        }
        return lldb::SBThread();
    };

    lldb::SBThread thread;
    while (true) {
        lldb::StateType state = process.GetState();
        if (state == lldb::eStateExited) {
            return dieRuntime("process exited before breakpoint hit "
                              "(exit code " +
                              std::to_string(process.GetExitStatus()) + ")");
        }
        if (state == lldb::eStateCrashed) {
            process.Kill();
            return dieRuntime("process crashed before breakpoint hit");
        }
        if (state == lldb::eStateStopped) {
            thread = findBreakpointThread();
            if (thread.IsValid()) break;
            // Stopped without a breakpoint reason (e.g. entry stop). Resume
            // until the next sync stop.
            if (std::chrono::steady_clock::now() >= deadline) {
                process.Kill();
                return dieRuntime("timeout waiting for breakpoint hit (" +
                                  std::to_string(kBreakpointWaitMs) + " ms)");
            }
            lldb::SBError contErr = process.Continue();
            if (contErr.Fail()) {
                process.Kill();
                return dieRuntime(std::string("failed to resume after non-bp stop: ") +
                                  (contErr.GetCString() ? contErr.GetCString() : "?"));
            }
            continue;
        }
        // Any other state should not occur in sync mode after Launch returns.
        if (std::chrono::steady_clock::now() >= deadline) {
            process.Kill();
            return dieRuntime("timeout waiting for breakpoint hit (state=" +
                              std::to_string(state) + ")");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    process.SetSelectedThread(thread);

    lldb::SBFrame frame = thread.GetFrameAtIndex(0);
    if (!frame.IsValid()) {
        process.Kill();
        return dieRuntime("no frame 0 at breakpoint");
    }
    thread.SetSelectedFrame(0);

    // --- Emit wire records --------------------------------------------------
    // Multi-var: one `breakpoint_hit` followed by one
    // (VarBytes, LayoutDescriptor) pair per --var entry, in the order
    // received. CLI side drains exactly args.vars.size() pairs.
    ByteSink sink = byteSinkToStdout();

    json hit = {
        {"kind", "breakpoint_hit"},
        {"frame", 1},
        {"site", args.site},
    };
    writeJsonLine(sink, hit);

    for (const std::string& varName : args.vars) {
        lldb::SBValue var = frame.FindVariable(varName.c_str());
        if (!var.IsValid()) {
            process.Kill();
            return dieRuntime("variable '" + varName + "' not in scope at " + args.site);
        }

        lldb::SBType varType = var.GetType();
        if (!varType.IsValid()) {
            process.Kill();
            return dieType("variable '" + varName + "' has invalid type");
        }

        std::vector<int64_t> shape;
        lldb::SBType leafType = unwrapArrayShape(varType, shape);

        // Aggregate-leaf branch.
        // If the leaf is a struct/class of primitives, emit dtype:"struct"
        // and a v2 `struct` JSON block with field offsets. Otherwise fall
        // through to the primitive-leaf path.
        std::string dtype;
        json structBlock;
        bool isStructLeaf = false;
        if (leafType.IsValid() && leafType.IsAggregateType() &&
            leafType.GetNumberOfFields() > 0) {
            std::string structErr = tryBuildStructBlock(leafType, structBlock);
            if (!structErr.empty()) {
                process.Kill();
                // Reuse the existing "unsupported variable type" failure
                // path (kExitUnsupportedType) so callers see the same exit
                // code shape they did for unmappable primitive leaves.
                return dieType("variable '" + varName + "' aggregate leaf: " +
                               structErr);
            }
            dtype = "struct";
            isStructLeaf = true;
        } else {
            dtype = basicTypeToDtype(leafType.GetBasicType(),
                                     leafType.GetByteSize());
            if (dtype.empty()) {
                process.Kill();
                const char* tn = leafType.GetName();
                return dieType("variable '" + varName + "' element type " +
                               (tn ? tn : "<unknown>") +
                               " is not a supported primitive int/float type");
            }
        }

        uint64_t totalBytes = var.GetByteSize();
        std::vector<uint8_t> bytes(totalBytes);
        if (totalBytes > 0) {
            lldb::SBError readErr;
            lldb::SBData data = var.GetData();
            if (!data.IsValid()) {
                process.Kill();
                return dieRuntime("var.GetData() returned invalid SBData");
            }
            size_t n = data.ReadRawData(readErr, /*offset=*/0, bytes.data(),
                                        totalBytes);
            if (readErr.Fail() || n != totalBytes) {
                std::string m = "failed to read raw bytes of '" + varName + "'";
                if (readErr.GetCString()) m += std::string(": ") + readErr.GetCString();
                process.Kill();
                return dieRuntime(m);
            }
        }

        // Compute layout (shape may be empty for a scalar — wire it as 0-d).
        int64_t elemSize = static_cast<int64_t>(leafType.GetByteSize());
        std::vector<int64_t> strides = rowMajorStrides(shape, elemSize);

        BinaryFrame varFrame;
        varFrame.type = BinaryFrameType::VarBytes;
        varFrame.flags = 0;
        varFrame.frameId = 1;
        varFrame.payload = std::move(bytes);
        writeBinaryFrame(sink, varFrame);

        json layout = {
            {"variable", varName},
            {"dtype", dtype},
            {"shape", shape},
            {"strides", strides},
        };
        if (isStructLeaf) {
            // v2 schema: leaf is an aggregate. The Compute side decodes
            // struct elements by stride + field offsets; the raw bytes
            // payload (above) is unchanged — the consumer carves out per-
            // element / per-field slices itself.
            layout["struct"] = std::move(structBlock);
        }
        std::string layoutStr = layout.dump();
        BinaryFrame layoutFrame;
        layoutFrame.type = BinaryFrameType::LayoutDescriptor;
        layoutFrame.flags = 0;
        layoutFrame.frameId = 1;
        layoutFrame.payload.assign(layoutStr.begin(), layoutStr.end());
        writeBinaryFrame(sink, layoutFrame);
    }

    // --- Await control-plane continue --------------------------------------
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            json j = json::parse(line);
            if (j.value("op", std::string{}) == "continue") break;
        } catch (...) {
            // ignore non-JSON lines
        }
    }

    // In sync mode, Continue() returns when the inferior next stops or exits.
    // If it stops on another breakpoint (the adapter registers only one) we
    // still drop out of the wait below; if it exits, GetState() is Exited.
    process.Continue();

    auto exitDeadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kBreakpointWaitMs);
    while (true) {
        lldb::StateType s = process.GetState();
        if (s == lldb::eStateExited || s == lldb::eStateCrashed ||
            s == lldb::eStateDetached) {
            break;
        }
        if (std::chrono::steady_clock::now() >= exitDeadline) {
            process.Kill();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return kExitOk;
}
