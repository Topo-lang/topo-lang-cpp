// Fixture 09: friend function definition
// Focus: a `friend` function declared inside a class body and defined at file
// scope. The extractor looks for FunctionDecl definitions; the friend name is
// `makeBox` (file-scope, not a member of Box despite the friend declaration).

class Box {
public:
    int width_;
    friend int getBoxWidth(const Box& b);
};

int getBoxWidth(const Box& b) {
    return b.width_;
}
