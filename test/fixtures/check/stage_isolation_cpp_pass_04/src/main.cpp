// Would be a violation under `mode = "force"` — `init` (stage<1>) calls
// `process` (stage<2>). With stage_isolation off, no errors are produced.

void process() {
}

void init() {
    process();  // would violate — but mode=off
}

void run() {
    init();
    process();
}
