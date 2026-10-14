#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// No bytes are written. Open a test-owned writable sentinel, then attempt a
// loopback-only connection. A sandbox pass requires kernel permission-denial
// errors; connection refusal and a read-only filesystem do not count.
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    errno = 0;
    const int file = open(argv[1], O_WRONLY | O_APPEND);
    const int writeError = errno;
    if (file >= 0) close(file);
    errno = 0;
    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    int networkError = errno;
    int connected = -1;
    if (socketFd >= 0) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(9);
        address.sin_addr.s_addr = htonl(0x7f000001);
        connected = connect(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        networkError = errno;
        close(socketFd);
    }
    const bool writeDenied = file < 0 && (writeError == EACCES || writeError == EPERM);
    const bool networkDenied = (socketFd < 0 || connected < 0) && (networkError == EACCES || networkError == EPERM);
    std::printf("writeDenied=%d errno=%d networkDenied=%d errno=%d\n",
                writeDenied, writeError, networkDenied, networkError);
    return writeDenied && networkDenied ? 0 : 1;
}
