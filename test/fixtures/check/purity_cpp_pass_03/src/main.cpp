// Sequential stages: `init` and `finalize` are NOT in a parallel stage
// (they run at stage<1> and stage<2> respectively), so writes to file-scope
// globals are allowed.

static int gState = 0;
static int gCount = 0;

void init() {
    gState = 1;
    gCount = gCount + 1;
}

void finalize() {
    gState = 2;
    gCount = gCount + 10;
}
