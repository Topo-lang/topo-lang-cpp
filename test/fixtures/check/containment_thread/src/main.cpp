#include <pthread.h>

namespace app {

void* worker(void*) { return nullptr; }

void start_background() {
    pthread_t t;
    pthread_create(&t, nullptr, worker, nullptr);
}

} // namespace app
