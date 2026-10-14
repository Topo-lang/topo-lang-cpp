// Emit a prepared preprocessor stream without host text-mode translation.
// This gives the scanner deterministic LF and CRLF subprocess output while
// real-Clang tests separately verify the actual compiler's line markers.
#include <array>
#include <cstdio>
#include <fstream>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
    if (argc < 2) return 2;
#ifdef _WIN32
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 3;
#endif
    std::ifstream input(argv[argc - 1], std::ios::binary);
    if (!input) return 4;
    std::array<char, 4096> bytes;
    while (input.read(bytes.data(), bytes.size()) || input.gcount()) {
        const auto count = static_cast<size_t>(input.gcount());
        if (std::fwrite(bytes.data(), 1, count, stdout) != count) return 5;
    }
    return input.bad() || std::fflush(stdout) != 0 ? 6 : 0;
}
