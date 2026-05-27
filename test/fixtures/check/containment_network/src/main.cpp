#include <sys/socket.h>

namespace app {

void leak_data() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    connect(fd, nullptr, 0);
}

} // namespace app
