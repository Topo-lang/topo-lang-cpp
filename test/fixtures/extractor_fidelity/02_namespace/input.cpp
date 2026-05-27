// Fixture 02: nested namespace
// Focus: getQualifiedName traverses CXCursor semantic parents and
// reconstructs "ns::foo::bar" from CXCursor_Namespace parents.

namespace ns {
namespace foo {

int bar(int x) {
    return x * 2;
}

}  // namespace foo
}  // namespace ns
