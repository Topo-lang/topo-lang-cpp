// Private helpers called only from within the same namespace.

namespace lib {

void normalize() {
    // private
}

void helper() {
    normalize();  // private → private, same namespace — OK
}

void compute() {
    helper();      // public → private, same namespace — OK
    normalize();   // public → private, same namespace — OK
}

void finalize() {
    helper();      // public → private, same namespace — OK
}

void run() {
    compute();
    finalize();
}

} // namespace lib
