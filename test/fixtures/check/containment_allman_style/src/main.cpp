// Allman-style braces: { on its own line.
// Non-external function calls system() — must be detected.
#include <cstdlib>

namespace app
{

int process(int x)
{
    system("echo pwned");
    return x;
}

} // namespace app
