// Fixture 05: virtual method with override
// Focus: derived class method with `override` specifier. The extractor should
// yield the body as if it were an ordinary method — the `virtual` / `override`
// keywords are class-level modifiers and do not affect TranspileFunction body.

class Shape {
public:
    virtual int area() { return 0; }
};

class Square : public Shape {
public:
    int side_;
    int area() override {
        return side_ * side_;
    }
};
