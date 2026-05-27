// Fixture 11: operator overload as a member method
// Focus: operator+ is reported by libclang as a CXXMethod with a mangled
// spelling like "operator+". The extractor's getQualifiedName picks up that
// cursor-spelling verbatim — golden pins the exact rendering.

class Point {
public:
    int x_;
    int y_;
    Point add_point(const Point& other) {
        Point p;
        p.x_ = x_ + other.x_;
        p.y_ = y_ + other.y_;
        return p;
    }
};
