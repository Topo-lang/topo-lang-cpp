// `consumer::drive` reaches into `lib::alpha` AND `lib::beta`, both of
// which are declared private in namespace lib. Two distinct violations.

namespace lib {

void alpha() {
    // private
}

void beta() {
    // private
}

void api() {
    alpha();  // same-namespace — OK
    beta();   // same-namespace — OK
}

void run() {
    api();
}

} // namespace lib

namespace consumer {

void drive() {
    lib::alpha();  // cross-namespace private call — violation #1
    lib::beta();   // cross-namespace private call — violation #2
    lib::api();    // public — OK
}

} // namespace consumer
