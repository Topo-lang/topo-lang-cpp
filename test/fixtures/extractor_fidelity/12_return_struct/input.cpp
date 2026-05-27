// Fixture 12: function returning a user-defined struct
// Focus: ConstructExpr emission — when the body returns a constructed Point
// via brace-init or explicit ctor call, the extractor should produce a
// Construct expr with the Point type. Pins the exact form libclang uses.

struct Point2 {
    int x;
    int y;
};

Point2 make_point(int a, int b) {
    Point2 p;
    p.x = a;
    p.y = b;
    return p;
}
