// Stage isolation violation: `init` (stage<1>) calls `process` (stage<2>).
// Top-level C++ functions so the extractor produces simple names matching
// the .topo calledFunctions list. Forward declaration of `process` verifies
// that the extractor correctly recognizes `; ` as a forward decl and does
// not treat the following `init()` body as part of `process`.

void process();  // forward decl

void init() {
    process();  // forward stage call — violation
}

void process() {
    // stage 2 work
}

void run() {
    init();
    process();
}
