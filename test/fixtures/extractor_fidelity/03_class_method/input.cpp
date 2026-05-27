// Fixture 03: class member method
// Focus: CXCursor_CXXMethod body extraction + implicit `this` member access.

class Counter {
public:
    int value_;

    int increment() {
        value_ = value_ + 1;
        return value_;
    }
};
