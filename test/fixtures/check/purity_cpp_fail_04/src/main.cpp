// Three parallel-stage functions each write a different global.
// Expected: 3 purity errors.

static int buffer = 0;
static int processed = 0;
static int totalBytes = 0;

void producer() {
    buffer = 1;  // write global #1
}

void consumer() {
    processed = processed + 1;  // write global #2
}

void sideEffect() {
    totalBytes += 100;  // write global #3 (compound assignment)
}
