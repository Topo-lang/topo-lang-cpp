// `init` and `process` never call each other — the host call graph
// respects the stage ordering declared in .topo.

void init() {
    int local = 0;
    local = local + 1;
    (void)local;
}

void process() {
    int tmp = 42;
    (void)tmp;
}

void run() {
    init();
    process();
}
