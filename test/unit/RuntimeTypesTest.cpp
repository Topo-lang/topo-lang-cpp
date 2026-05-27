#include <topo/array.h>
#include <topo/span.h>
#include <topo/slot.h>
#include <topo/table.h>
#include <topo/view.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <vector>

// ============================================================================
// Static assertions: trivially copyable
// ============================================================================

static_assert(std::is_trivially_copyable_v<topo::array<int, 4>>, "topo::array<int,4> must be trivially copyable");
static_assert(std::is_trivially_copyable_v<topo::array<double, 2>>, "topo::array<double,2> must be trivially copyable");

static_assert(std::is_trivially_copyable_v<topo::span<int>>, "topo::span<int> must be trivially copyable");
static_assert(std::is_trivially_copyable_v<topo::span<const double>>,
              "topo::span<const double> must be trivially copyable");

static_assert(std::is_trivially_copyable_v<topo::slot<int>>, "topo::slot<int> must be trivially copyable");
static_assert(std::is_trivially_copyable_v<topo::slot<float>>, "topo::slot<float> must be trivially copyable");

// topo::table is NOT trivially copyable (heap-owning container)
static_assert(!std::is_trivially_copyable_v<topo::table<int, float>>,
              "topo::table must not be trivially copyable (owns heap memory)");

// topo::view IS trivially copyable and pointer-sized
static_assert(std::is_trivially_copyable_v<topo::view<int>>, "topo::view<int> must be trivially copyable");
static_assert(sizeof(topo::view<int>) == sizeof(const int*), "topo::view<int> must be pointer-sized");

// ============================================================================
// topo::array tests
// ============================================================================

TEST(Array, OperatorBracket) {
    topo::array<int, 3> a = {10, 20, 30};
    EXPECT_EQ(a[0], 10);
    EXPECT_EQ(a[1], 20);
    EXPECT_EQ(a[2], 30);

    a[1] = 99;
    EXPECT_EQ(a[1], 99);
}

TEST(Array, At) {
    topo::array<int, 3> a = {1, 2, 3};
    EXPECT_EQ(a.at(0), 1);
    EXPECT_EQ(a.at(2), 3);
}

TEST(Array, FrontBack) {
    topo::array<int, 4> a = {5, 6, 7, 8};
    EXPECT_EQ(a.front(), 5);
    EXPECT_EQ(a.back(), 8);
}

TEST(Array, SizeEmpty) {
    topo::array<int, 3> a = {1, 2, 3};
    EXPECT_EQ(a.size(), 3u);
    EXPECT_FALSE(a.empty());
}

TEST(Array, RangeFor) {
    topo::array<int, 4> a = {1, 2, 3, 4};
    int sum = 0;
    for (int x : a)
        sum += x;
    EXPECT_EQ(sum, 10);
}

TEST(Array, Fill) {
    topo::array<int, 5> a = {};
    a.fill(42);
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i], 42);
    }
}

TEST(Array, Data) {
    topo::array<int, 3> a = {10, 20, 30};
    int* ptr = a.data();
    EXPECT_EQ(ptr[0], 10);
    EXPECT_EQ(ptr[2], 30);
}

TEST(Array, StructuredBindings) {
    topo::array<int, 3> a = {100, 200, 300};
    auto& [x, y, z] = a;
    EXPECT_EQ(x, 100);
    EXPECT_EQ(y, 200);
    EXPECT_EQ(z, 300);

    // Modification through binding
    x = 999;
    EXPECT_EQ(a[0], 999);
}

TEST(Array, Constexpr) {
    constexpr topo::array<int, 3> a = {1, 2, 3};
    static_assert(a[0] == 1);
    static_assert(a.size() == 3);
    static_assert(a.front() == 1);
    static_assert(a.back() == 3);
}

// ============================================================================
// topo::span tests
// ============================================================================

TEST(Span, FromPointerAndSize) {
    int buf[] = {10, 20, 30, 40};
    topo::span<int> s(buf, 4);
    EXPECT_EQ(s.size(), 4u);
    EXPECT_EQ(s[0], 10);
    EXPECT_EQ(s[3], 40);
}

TEST(Span, FromCArray) {
    int buf[] = {1, 2, 3};
    topo::span<int> s(buf);
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s[0], 1);
    EXPECT_EQ(s[2], 3);
}

TEST(Span, FromTopoArray) {
    topo::array<int, 3> a = {5, 6, 7};
    topo::span<int> s(a);
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s[0], 5);
    EXPECT_EQ(s[2], 7);

    // Mutation through span
    s[1] = 99;
    EXPECT_EQ(a[1], 99);
}

TEST(Span, FromVector) {
    std::vector<int> v = {10, 20, 30, 40, 50};
    topo::span<int> s(v);
    EXPECT_EQ(s.size(), 5u);
    EXPECT_EQ(s[4], 50);
}

TEST(Span, Empty) {
    topo::span<int> s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(s.data(), nullptr);
}

TEST(Span, RangeFor) {
    int buf[] = {1, 2, 3, 4};
    topo::span<int> s(buf);
    int sum = 0;
    for (int x : s)
        sum += x;
    EXPECT_EQ(sum, 10);
}

TEST(Span, Subspan) {
    int buf[] = {10, 20, 30, 40, 50};
    topo::span<int> s(buf);

    auto sub = s.subspan(1, 3);
    EXPECT_EQ(sub.size(), 3u);
    EXPECT_EQ(sub[0], 20);
    EXPECT_EQ(sub[2], 40);
}

TEST(Span, First) {
    int buf[] = {1, 2, 3, 4, 5};
    topo::span<int> s(buf);

    auto f = s.first(2);
    EXPECT_EQ(f.size(), 2u);
    EXPECT_EQ(f[0], 1);
    EXPECT_EQ(f[1], 2);
}

TEST(Span, Last) {
    int buf[] = {1, 2, 3, 4, 5};
    topo::span<int> s(buf);

    auto l = s.last(2);
    EXPECT_EQ(l.size(), 2u);
    EXPECT_EQ(l[0], 4);
    EXPECT_EQ(l[1], 5);
}

TEST(Span, ConstSpan) {
    const int buf[] = {10, 20, 30};
    topo::span<const int> s(buf);
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s[1], 20);
}

// ============================================================================
// topo::slot tests
// ============================================================================

TEST(Slot, DefaultEmpty) {
    topo::slot<int> s;
    EXPECT_FALSE(s.has_value());
    EXPECT_FALSE(static_cast<bool>(s));
}

TEST(Slot, ConstructWithValue) {
    topo::slot<int> s(42);
    EXPECT_TRUE(s.has_value());
    EXPECT_EQ(s.value(), 42);
}

TEST(Slot, ValueOr) {
    topo::slot<int> empty;
    EXPECT_EQ(empty.value_or(99), 99);

    topo::slot<int> full(42);
    EXPECT_EQ(full.value_or(99), 42);
}

TEST(Slot, Reset) {
    topo::slot<int> s(42);
    EXPECT_TRUE(s.has_value());

    s.reset();
    EXPECT_FALSE(s.has_value());
}

TEST(Slot, Emplace) {
    topo::slot<int> s;
    EXPECT_FALSE(s.has_value());

    s.emplace(77);
    EXPECT_TRUE(s.has_value());
    EXPECT_EQ(s.value(), 77);

    // Emplace over existing value
    s.emplace(88);
    EXPECT_EQ(s.value(), 88);
}

TEST(Slot, ResetThenEmplace) {
    topo::slot<int> s(10);
    s.reset();
    s.emplace(20);
    EXPECT_TRUE(s.has_value());
    EXPECT_EQ(s.value(), 20);
}

TEST(Slot, FloatType) {
    topo::slot<float> s(3.14f);
    EXPECT_TRUE(s.has_value());
    EXPECT_FLOAT_EQ(s.value(), 3.14f);
}

// ============================================================================
// topo::table tests
// ============================================================================

TEST(Table, ConstructAndSize) {
    topo::table<int, float> t(100);
    EXPECT_EQ(t.size(), 100u);
    EXPECT_EQ(t.column_count, 2u);
    EXPECT_EQ(t.alignment(), 64u);
}

TEST(Table, ColumnAccessAndContiguity) {
    topo::table<int, float> t(4);
    int* col0 = t.column<0>();
    float* col1 = t.column<1>();

    // Write through column pointers
    for (int i = 0; i < 4; ++i) {
        col0[i] = i * 10;
        col1[i] = static_cast<float>(i) * 1.5f;
    }

    // Read back
    EXPECT_EQ(col0[0], 0);
    EXPECT_EQ(col0[3], 30);
    EXPECT_FLOAT_EQ(col1[1], 1.5f);
    EXPECT_FLOAT_EQ(col1[3], 4.5f);

    // Contiguity: elements are adjacent in memory
    EXPECT_EQ(reinterpret_cast<char*>(&col0[1]) - reinterpret_cast<char*>(&col0[0]),
              static_cast<std::ptrdiff_t>(sizeof(int)));
}

TEST(Table, ColumnAlignment) {
    topo::table<int, float, double> t(32);
    auto addr0 = reinterpret_cast<std::uintptr_t>(t.column<0>());
    auto addr1 = reinterpret_cast<std::uintptr_t>(t.column<1>());
    auto addr2 = reinterpret_cast<std::uintptr_t>(t.column<2>());

    EXPECT_EQ(addr0 % 64, 0u) << "Column 0 must be 64-byte aligned";
    EXPECT_EQ(addr1 % 64, 0u) << "Column 1 must be 64-byte aligned";
    EXPECT_EQ(addr2 % 64, 0u) << "Column 2 must be 64-byte aligned";
}

TEST(Table, CustomAlignment) {
    topo::basic_table<32, int, float> t(16);
    EXPECT_EQ(t.alignment(), 32u);

    auto addr0 = reinterpret_cast<std::uintptr_t>(t.column<0>());
    auto addr1 = reinterpret_cast<std::uintptr_t>(t.column<1>());
    EXPECT_EQ(addr0 % 32, 0u) << "Column 0 must be 32-byte aligned";
    EXPECT_EQ(addr1 % 32, 0u) << "Column 1 must be 32-byte aligned";
}

TEST(Table, RowAccess) {
    topo::table<int, float> t(3);
    t.column<0>()[0] = 10;
    t.column<1>()[0] = 1.5f;
    t.column<0>()[1] = 20;
    t.column<1>()[1] = 2.5f;

    auto r0 = t.row(0);
    EXPECT_EQ(r0.get<0>(), 10);
    EXPECT_FLOAT_EQ(r0.get<1>(), 1.5f);

    auto r1 = t.row(1);
    EXPECT_EQ(r1.get<0>(), 20);
    EXPECT_FLOAT_EQ(r1.get<1>(), 2.5f);
}

TEST(Table, SetRow) {
    topo::table<int, float> t(2);
    topo::tuple<int, float> row(42, 3.14f);
    t.set_row(0, row);

    EXPECT_EQ(t.column<0>()[0], 42);
    EXPECT_FLOAT_EQ(t.column<1>()[0], 3.14f);

    // set_row on second row
    t.set_row(1, topo::tuple<int, float>(99, 2.71f));
    EXPECT_EQ(t.column<0>()[1], 99);
    EXPECT_FLOAT_EQ(t.column<1>()[1], 2.71f);
}

TEST(Table, ColumnSpanCompatibility) {
    topo::table<int, float> t(4);
    for (int i = 0; i < 4; ++i)
        t.column<0>()[i] = i * 100;

    topo::span<int> s = t.column_span<0>();
    EXPECT_EQ(s.size(), 4u);
    EXPECT_EQ(s[0], 0);
    EXPECT_EQ(s[3], 300);

    // Mutation through span reflects in table
    s[2] = 999;
    EXPECT_EQ(t.column<0>()[2], 999);
}

TEST(Table, ColumnIndependentAllocation) {
    topo::table<int, float> t(8);
    auto* c0 = reinterpret_cast<char*>(t.column<0>());
    auto* c1 = reinterpret_cast<char*>(t.column<1>());

    // Columns should not overlap: distance > column size
    auto distance = (c0 > c1) ? (c0 - c1) : (c1 - c0);
    auto minSize = std::min(8 * sizeof(int), 8 * sizeof(float));
    EXPECT_GE(static_cast<std::size_t>(distance), minSize) << "Column allocations must not overlap";
}

// ============================================================================
// topo::view tests
// ============================================================================

TEST(View, GetValue) {
    int x = 42;
    topo::view<int> v(x);
    EXPECT_EQ(v.get(), 42);
}

TEST(View, DereferenceOperator) {
    double d = 3.14;
    topo::view<double> v(d);
    EXPECT_DOUBLE_EQ(*v, 3.14);
}

TEST(View, ArrowOperator) {
    struct Point {
        int x;
        int y;
    };
    Point p{10, 20};
    topo::view<Point> v(p);
    EXPECT_EQ(v->x, 10);
    EXPECT_EQ(v->y, 20);
}

TEST(View, ImplicitConversion) {
    int x = 77;
    topo::view<int> v(x);
    const int& ref = v;
    EXPECT_EQ(ref, 77);
}

TEST(View, ReflectsOriginal) {
    int x = 1;
    topo::view<int> v(x);
    EXPECT_EQ(v.get(), 1);

    x = 100;
    EXPECT_EQ(v.get(), 100) << "view should reflect changes to the original";
}

TEST(View, CopyableView) {
    int x = 55;
    topo::view<int> v1(x);
    topo::view<int> v2 = v1; // copy
    EXPECT_EQ(v2.get(), 55);

    // Both point to the same object
    x = 66;
    EXPECT_EQ(v1.get(), 66);
    EXPECT_EQ(v2.get(), 66);
}
