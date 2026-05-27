// External functions use dangerous APIs — allowed because declared external in .topo.
// Non-external function is pure computation.

namespace app {

int read_file(int id) {
    auto f = fopen("data.txt", "r");
    if (f) fclose(f);
    return 0;
}

int run_command(int code) {
    return system("echo ok");
}

int process(int x) {
    return x * 2;
}

} // namespace app
