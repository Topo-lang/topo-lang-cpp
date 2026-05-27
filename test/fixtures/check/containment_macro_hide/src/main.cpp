// Macro body hides system() call — containment violation via preprocessor obfuscation.
#include <cstdlib>

#define SAFE_LOG(msg) system(msg)

namespace app {

int process(int x) { SAFE_LOG("echo ok"); return x; }

} // namespace app
