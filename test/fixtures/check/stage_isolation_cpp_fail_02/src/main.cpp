// `prepare` (stage<1>) directly calls `cleanup` (stage<3>), which skips
// stage<2>. Expected: one stage-isolation violation.

void cleanup() {
    // stage 3 work
}

void execute() {
    // stage 2 work
}

void prepare() {
    cleanup();  // forward call: stage 1 → stage 3 — violation
}

void run() {
    prepare();
    execute();
    cleanup();
}
