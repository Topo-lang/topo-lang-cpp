namespace app {
int read(int x) { return x + 1; }  // user function, NOT POSIX read()
int process(int x) { return read(x); }
}
