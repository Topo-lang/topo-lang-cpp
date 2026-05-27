// Regression test for topo-lang-cpp-preprocessor-scanner-hardcoded-tmp.
//
// CppPreprocessorScanner used to write its preprocessed output to a
// hardcoded ``/tmp/topo-pp-scan.cpp``. Two failure modes the fix
// closes:
//
//   1. Symlink-attack TOCTOU — a pre-created symlink at the
//      hardcoded path could redirect the write into any user-writable
//      file. The fix uses ``topo::platform::TempFile`` which atomically
//      creates the file with O_CREAT|O_EXCL on POSIX / CREATE_NEW on
//      Windows; a pre-existing symlink causes the create to fail and
//      the helper picks the next candidate name.
//   2. Concurrent-worker collision — ``topo-check --jobs N`` runs
//      multiple scanners in parallel, all clobbering the same path
//      and reading whichever content lost the race. The fix gives
//      each scanner instance a unique pid+counter-namespaced filename.
//
// Driving the full scanner needs clang on PATH and is gated behind
// TOPO_ENABLE_LLVM. The narrower regression here exercises the
// underlying ``topo::platform::TempFile`` invariant the scanner now
// relies on: N concurrent threads each create-write-read a temp file
// without losing data to a sibling's overwrite.

#include "topo/Platform/TempFile.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

TEST(PreprocessorScannerTempFileRegression, ConcurrentTempFilesDoNotCollide) {
    // Each worker creates a unique TempFile, writes its tag, then reads
    // the file back and asserts the tag survived. Pre-fix this was a
    // single hardcoded path: any concurrent worker would clobber a
    // sibling's content and the read-back assertion would fail.
    constexpr int kWorkers = 8;
    constexpr int kRoundsPerWorker = 16;

    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    threads.reserve(kWorkers);
    for (int w = 0; w < kWorkers; ++w) {
        threads.emplace_back([w, &mismatches]() {
            for (int r = 0; r < kRoundsPerWorker; ++r) {
                topo::platform::TempFile tmp("topo-pp-scan-test", ".cpp");
                std::ostringstream payload;
                payload << "worker=" << w << " round=" << r
                        << " path=" << tmp.path().string();
                std::string body = payload.str();
                {
                    std::ofstream ofs(tmp.path());
                    ASSERT_TRUE(ofs.is_open());
                    ofs << body;
                }
                std::ifstream ifs(tmp.path());
                ASSERT_TRUE(ifs.is_open());
                std::string readback((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
                if (readback != body) mismatches.fetch_add(1);
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(mismatches.load(), 0)
        << "concurrent TempFile instances are sharing a name and clobbering "
           "each other — the hardcoded /tmp/topo-pp-scan.cpp behaviour has "
           "regressed";
}

TEST(PreprocessorScannerTempFileRegression, TempFilesAreUniquePerInstance) {
    // Sanity check the unique-name probe: 64 sequential TempFile
    // constructions in the same process produce 64 distinct paths.
    std::set<std::string> seen;
    for (int i = 0; i < 64; ++i) {
        topo::platform::TempFile tmp("topo-pp-scan-test", ".cpp");
        auto [it, inserted] = seen.insert(tmp.path().string());
        EXPECT_TRUE(inserted) << "duplicate TempFile path: " << *it;
        EXPECT_TRUE(fs::exists(tmp.path())) << "TempFile should exist on disk";
    }
}
