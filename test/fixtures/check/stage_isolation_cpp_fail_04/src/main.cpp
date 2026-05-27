// `acquire` (stage<1>) and `transform` (stage<2>) both call `store`
// (stage<3>) directly — two forward-stage violations.

void store() {
    // stage 3
}

void transform() {
    store();  // violation #1: stage 2 → stage 3
}

void acquire() {
    store();  // violation #2: stage 1 → stage 3
}

void run() {
    acquire();
    transform();
    store();
}
