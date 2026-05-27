// CppEmitter stdlib bridging-type tests.
//
// Verifies that the 6 first-batch stdlib types (bool / i64 / f64 / string /
// optional<T> / slice<T>) emit the agreed C++ idioms:
//
//   bool        -> bool
//   i64         -> std::int64_t
//   f64         -> double
//   string      -> std::string_view   (UTF-8, non-owning)
//   optional<T> -> std::optional<T>
//   slice<T>    -> topo::span<const T>
//   bytes       -> topo::span<const std::uint8_t>   (slice<u8>-isomorphic)
//   array<T, N> -> std::array<T, N>
//
// These are unit-level checks on the emitter's `emitType` output via the
// public `emit()` entry point. Cross-language equivalence is deferred until
// the Rust + Java emitters also route stdlib types. The
// transpile-equivalence harness in topo-core would currently fail the
// other-language assertions, so verification stays within the
// per-language test surface, matching the Python pattern.

#include "CppEmitter.h"
#include "topo/Stdlib/Types.h"
#include "topo/Transpile/TranspileModel.h"
#include <gtest/gtest.h>
#include <string>

using namespace topo;
using namespace topo::transpile;

// ---------------------------------------------------------------------------
// Helpers (mirroring the W11 PythonEmitterStdlibTest pattern)
// ---------------------------------------------------------------------------

/// Build a TypeNode for a Batch-1 stdlib scalar type.
static TypeNode stdlibScalar(stdlib::TypeId id) {
    TypeNode t;
    t.nameParts = {stdlib::keywordOf(id)};
    t.stdlibId = id;
    return t;
}

/// Build a TypeNode for a Batch-1 stdlib parameterized type (optional / slice).
static TypeNode stdlibParametric(stdlib::TypeId id, TypeNode inner) {
    TypeNode t;
    t.nameParts = {stdlib::keywordOf(id)};
    t.stdlibId = id;
    t.templateArgs.push_back(std::move(inner));
    return t;
}

/// Build an `array<T, N>` TypeNode. Mirrors how the parser carries it:
/// templateArgs[0] = element type T, templateArgs[1].nonTypeValue = N.
static TypeNode stdlibArray(TypeNode elem, int n) {
    TypeNode t;
    t.nameParts = {stdlib::keywordOf(stdlib::TypeId::Array)};
    t.stdlibId = stdlib::TypeId::Array;
    t.templateArgs.push_back(std::move(elem));
    TypeNode nArg;
    nArg.nonTypeValue = n;
    t.templateArgs.push_back(std::move(nArg));
    return t;
}

/// Build a single-field `record<name: T>` TypeNode (recordFields, not
/// templateArgs — matching the parser/AST representation).
static TypeNode stdlibRecord1(const std::string& fieldName, TypeNode fieldType) {
    TypeNode t;
    t.nameParts = {stdlib::keywordOf(stdlib::TypeId::Record)};
    t.stdlibId = stdlib::TypeId::Record;
    TypeNode::RecordField f;
    f.name = fieldName;
    f.typeBox.push_back(std::move(fieldType));
    t.recordFields.push_back(std::move(f));
    return t;
}

/// Build a minimal module with a single function `boundary(value: T) -> R`
/// and return the emitted C++ source. The body is left empty so the test
/// can string-assert on the rendered signature.
static std::string emitWithParamAndReturn(TypeNode paramType, TypeNode returnType) {
    TranspileModule mod;
    TranspileFunction fn;
    fn.qualifiedName = "boundary";
    fn.returnType = std::move(returnType);
    Parameter p;
    p.type = std::move(paramType);
    p.name = "value";
    fn.params.push_back(std::move(p));
    mod.functions.push_back(std::move(fn));

    CppEmitter emitter;
    return emitter.emit(mod).code;
}

// ---------------------------------------------------------------------------
// Scalar stdlib types
// ---------------------------------------------------------------------------

TEST(CppEmitterStdlib, BoolMapsToCppBool) {
    std::string code = emitWithParamAndReturn(stdlibScalar(stdlib::TypeId::Bool),
                                              stdlibScalar(stdlib::TypeId::Bool));
    EXPECT_NE(code.find("bool boundary(bool value)"), std::string::npos)
        << "Generated code:\n" << code;
}

TEST(CppEmitterStdlib, I64MapsToStdInt64) {
    std::string code = emitWithParamAndReturn(stdlibScalar(stdlib::TypeId::I64),
                                              stdlibScalar(stdlib::TypeId::I64));
    EXPECT_NE(code.find("std::int64_t boundary(std::int64_t value)"),
              std::string::npos)
        << "Generated code:\n" << code;
}

TEST(CppEmitterStdlib, F64MapsToDouble) {
    std::string code = emitWithParamAndReturn(stdlibScalar(stdlib::TypeId::F64),
                                              stdlibScalar(stdlib::TypeId::F64));
    EXPECT_NE(code.find("double boundary(double value)"), std::string::npos)
        << "Generated code:\n" << code;
}

TEST(CppEmitterStdlib, StringMapsToStringView) {
    std::string code = emitWithParamAndReturn(stdlibScalar(stdlib::TypeId::String),
                                              stdlibScalar(stdlib::TypeId::String));
    EXPECT_NE(code.find("std::string_view boundary(std::string_view value)"),
              std::string::npos)
        << "Generated code:\n" << code;
}

// ---------------------------------------------------------------------------
// Parameterized stdlib types
// ---------------------------------------------------------------------------

TEST(CppEmitterStdlib, OptionalEmitsStdOptional) {
    // optional<i64> -> std::optional<std::int64_t>
    auto inner = stdlibScalar(stdlib::TypeId::I64);
    auto opt = stdlibParametric(stdlib::TypeId::Optional, inner);
    std::string code = emitWithParamAndReturn(opt, stdlibScalar(stdlib::TypeId::Bool));
    EXPECT_NE(code.find("std::optional<std::int64_t> value"),
              std::string::npos)
        << "Generated code:\n" << code;
}

TEST(CppEmitterStdlib, SliceEmitsTopoSpanOfConstT) {
    // slice<f64> -> topo::span<const double>
    auto inner = stdlibScalar(stdlib::TypeId::F64);
    auto sl = stdlibParametric(stdlib::TypeId::Slice, inner);
    std::string code = emitWithParamAndReturn(sl, stdlibScalar(stdlib::TypeId::Bool));
    EXPECT_NE(code.find("topo::span<const double> value"),
              std::string::npos)
        << "Generated code:\n" << code;
}

TEST(CppEmitterStdlib, NestedOptionalOfSlice) {
    // optional<slice<i64>> -> std::optional<topo::span<const std::int64_t>>
    auto i64 = stdlibScalar(stdlib::TypeId::I64);
    auto sl = stdlibParametric(stdlib::TypeId::Slice, std::move(i64));
    auto opt = stdlibParametric(stdlib::TypeId::Optional, std::move(sl));
    std::string code = emitWithParamAndReturn(opt, stdlibScalar(stdlib::TypeId::Bool));
    EXPECT_NE(code.find("std::optional<topo::span<const std::int64_t>>"),
              std::string::npos)
        << "Generated code:\n" << code;
}

// ---------------------------------------------------------------------------
// bytes — slice<u8>-isomorphic non-owning byte view
// ---------------------------------------------------------------------------

TEST(CppEmitterStdlib, BytesEmitsSameAsSliceOfU8) {
    // bytes must emit exactly what slice<u8> emits: topo::span<const
    // std::uint8_t>. Build both signatures and assert they render the same
    // host parameter type, so the contract stays pinned to slice<u8>.
    TypeNode bytesTy;
    bytesTy.nameParts = {stdlib::keywordOf(stdlib::TypeId::Bytes)};
    bytesTy.stdlibId = stdlib::TypeId::Bytes;
    std::string bytesCode =
        emitWithParamAndReturn(bytesTy, stdlibScalar(stdlib::TypeId::Bool));

    auto sliceU8 = stdlibParametric(stdlib::TypeId::Slice,
                                    stdlibScalar(stdlib::TypeId::U8));
    std::string sliceCode =
        emitWithParamAndReturn(sliceU8, stdlibScalar(stdlib::TypeId::Bool));

    EXPECT_NE(bytesCode.find("topo::span<const std::uint8_t> value"),
              std::string::npos)
        << "Generated code:\n" << bytesCode;
    EXPECT_NE(sliceCode.find("topo::span<const std::uint8_t> value"),
              std::string::npos)
        << "slice<u8> reference form changed:\n" << sliceCode;
    // bytes carries no template args but still pulls in <topo/span.h> +
    // <cstdint> (the u8 element renders as std::uint8_t).
    EXPECT_NE(bytesCode.find("#include <topo/span.h>"), std::string::npos);
    EXPECT_NE(bytesCode.find("#include <cstdint>"), std::string::npos)
        << "Generated code:\n" << bytesCode;
}

// ---------------------------------------------------------------------------
// array<T, N> — fixed-length inline buffer
// ---------------------------------------------------------------------------

TEST(CppEmitterStdlib, ArrayOfI64EmitsStdArray) {
    // array<i64, 4> -> std::array<std::int64_t, 4>
    auto arr = stdlibArray(stdlibScalar(stdlib::TypeId::I64), 4);
    std::string code =
        emitWithParamAndReturn(arr, stdlibScalar(stdlib::TypeId::Bool));
    EXPECT_NE(code.find("std::array<std::int64_t, 4> value"),
              std::string::npos)
        << "Generated code:\n" << code;
    EXPECT_NE(code.find("#include <array>"), std::string::npos)
        << "Generated code:\n" << code;
    // element is i64 -> still needs <cstdint>
    EXPECT_NE(code.find("#include <cstdint>"), std::string::npos);
}

TEST(CppEmitterStdlib, NestedArrayOfRecord) {
    // array<record<a: i64>, 2> — the Array case must recurse into its
    // element type via emitType (mirroring slice/optional). This pins the
    // std::array<..., 2> wrapper and the <array> header; the inner record
    // rendering is owned by a separate (pre-existing) code path.
    auto rec = stdlibRecord1("a", stdlibScalar(stdlib::TypeId::I64));
    auto arr = stdlibArray(std::move(rec), 2);
    std::string code =
        emitWithParamAndReturn(arr, stdlibScalar(stdlib::TypeId::Bool));
    EXPECT_NE(code.find("std::array<"), std::string::npos)
        << "Generated code:\n" << code;
    EXPECT_NE(code.find(", 2> value"), std::string::npos)
        << "array<..., N> arity not rendered:\n" << code;
    EXPECT_NE(code.find("#include <array>"), std::string::npos)
        << "Generated code:\n" << code;
}

// ---------------------------------------------------------------------------
// Include preamble — conditional on stdlib type usage
// ---------------------------------------------------------------------------

TEST(CppEmitterStdlib, EmitsCstdintHeaderWhenI64Used) {
    std::string code = emitWithParamAndReturn(stdlibScalar(stdlib::TypeId::I64),
                                              stdlibScalar(stdlib::TypeId::Bool));
    EXPECT_NE(code.find("#include <cstdint>"), std::string::npos)
        << "Generated code:\n" << code;
}

TEST(CppEmitterStdlib, EmitsOptionalAndStringViewAndSpanHeaders) {
    // Construct a signature exercising all 4 header-bearing types in one go.
    TranspileModule mod;
    TranspileFunction fn;
    fn.qualifiedName = "boundary";
    fn.returnType = stdlibParametric(stdlib::TypeId::Optional,
                                     stdlibScalar(stdlib::TypeId::I64));
    auto addParam = [&](const std::string& name, TypeNode ty) {
        Parameter p; p.name = name; p.type = std::move(ty);
        fn.params.push_back(std::move(p));
    };
    addParam("name", stdlibScalar(stdlib::TypeId::String));
    addParam("flag", stdlibParametric(stdlib::TypeId::Optional,
                                      stdlibScalar(stdlib::TypeId::Bool)));
    addParam("values", stdlibParametric(stdlib::TypeId::Slice,
                                        stdlibScalar(stdlib::TypeId::F64)));
    mod.functions.push_back(std::move(fn));

    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;

    EXPECT_NE(code.find("#include <cstdint>"), std::string::npos);
    EXPECT_NE(code.find("#include <optional>"), std::string::npos);
    EXPECT_NE(code.find("#include <string_view>"), std::string::npos);
    EXPECT_NE(code.find("#include <topo/span.h>"), std::string::npos)
        << "Generated code:\n" << code;
}

TEST(CppEmitterStdlib, SkipsHeadersWhenNoStdlibTypesUsed) {
    // Function using only legacy `int` — emitter must NOT emit stdlib
    // preamble when no Batch-1 stdlib type is referenced.
    TranspileModule mod;
    TranspileFunction fn;
    fn.qualifiedName = "legacy";
    fn.returnType.nameParts = {"int"};
    Parameter p; p.name = "x"; p.type.nameParts = {"int"};
    fn.params.push_back(std::move(p));
    mod.functions.push_back(std::move(fn));

    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;

    EXPECT_EQ(code.find("#include <cstdint>"), std::string::npos);
    EXPECT_EQ(code.find("#include <string_view>"), std::string::npos);
    EXPECT_EQ(code.find("#include <optional>"), std::string::npos);
    EXPECT_EQ(code.find("#include <topo/span.h>"), std::string::npos)
        << "stdlib preamble leaked into non-stdlib module:\n" << code;
}

// ---------------------------------------------------------------------------
// Combined signature — covers all 6 first-batch types.
// ---------------------------------------------------------------------------

TEST(CppEmitterStdlib, AllSixTypesInOneSignature) {
    TranspileModule mod;
    TranspileFunction fn;
    fn.qualifiedName = "boundary";
    fn.returnType = stdlibParametric(stdlib::TypeId::Optional,
                                     stdlibScalar(stdlib::TypeId::I64));

    auto addParam = [&](const std::string& name, TypeNode ty) {
        Parameter p; p.name = name; p.type = std::move(ty);
        fn.params.push_back(std::move(p));
    };
    addParam("id", stdlibScalar(stdlib::TypeId::I64));
    addParam("name", stdlibScalar(stdlib::TypeId::String));
    addParam("flag", stdlibParametric(stdlib::TypeId::Optional,
                                      stdlibScalar(stdlib::TypeId::Bool)));
    addParam("values", stdlibParametric(stdlib::TypeId::Slice,
                                        stdlibScalar(stdlib::TypeId::F64)));
    // Add a bare bool param so the test covers all 6 keywords explicitly.
    addParam("ready", stdlibScalar(stdlib::TypeId::Bool));
    // Add an f64 too — same reason.
    addParam("score", stdlibScalar(stdlib::TypeId::F64));

    mod.functions.push_back(std::move(fn));

    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;

    // Signature shape — the emitter renders each param `<type> <name>`
    // separated by commas. We assert each fragment independently to avoid
    // coupling to inter-parameter spacing.
    EXPECT_NE(code.find("std::optional<std::int64_t> boundary("),
              std::string::npos) << "Generated code:\n" << code;
    EXPECT_NE(code.find("std::int64_t id"), std::string::npos);
    EXPECT_NE(code.find("std::string_view name"), std::string::npos);
    EXPECT_NE(code.find("std::optional<bool> flag"), std::string::npos);
    EXPECT_NE(code.find("topo::span<const double> values"), std::string::npos);
    EXPECT_NE(code.find("bool ready"), std::string::npos);
    EXPECT_NE(code.find("double score"), std::string::npos);

    // Includes — all 4 header-bearing types are present in the signature.
    EXPECT_NE(code.find("#include <cstdint>"), std::string::npos);
    EXPECT_NE(code.find("#include <optional>"), std::string::npos);
    EXPECT_NE(code.find("#include <string_view>"), std::string::npos);
    EXPECT_NE(code.find("#include <topo/span.h>"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Inheritance hierarchy: TranspileType.baseClasses ->
// C++ `struct S : public Base, public Mixin`. C++ has no class/interface
// split, so baseClassKinds is ignored — every base is `public`.
// ---------------------------------------------------------------------------

static std::string emitCppStructWithBases(const std::string& qname, std::vector<TypeNode> bases) {
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = qname;
    ty.baseClasses = std::move(bases);
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    return emitter.emit(mod).code;
}

static TypeNode plainNamed(const std::string& name) {
    TypeNode t;
    t.nameParts = {name};
    return t;
}

TEST(CppEmitterStdlib, StructSingleBaseEmitsPublic) {
    std::string code = emitCppStructWithBases("Dog", {plainNamed("Animal")});
    EXPECT_NE(code.find("struct Dog : public Animal {"), std::string::npos) << "Generated:\n" << code;
}

TEST(CppEmitterStdlib, StructMultipleBasesEmitsPublicList) {
    std::string code = emitCppStructWithBases("Service", {plainNamed("Base"), plainNamed("Mixin")});
    EXPECT_NE(code.find("struct Service : public Base, public Mixin {"), std::string::npos)
        << "Generated:\n"
        << code;
}

TEST(CppEmitterStdlib, StructEmptyBasesByteIdenticalToPreInheritance) {
    // Pre-inheritance emission was exactly `struct <name> {`.
    std::string code = emitCppStructWithBases("Plain", {});
    EXPECT_NE(code.find("struct Plain {"), std::string::npos) << "Generated:\n" << code;
    EXPECT_EQ(code.find("struct Plain :"), std::string::npos) << "no base-clause expected:\n" << code;
}

// ---------------------------------------------------------------------------
// Declaration-site generic type parameters: TranspileType/Function
// .templateParams -> C++ `template <typename ...>` prefix. Empty list must
// stay byte-identical to the pre-generics output (no prefix at all).
// ---------------------------------------------------------------------------

static TemplateParamDecl typeParam(const std::string& name) {
    TemplateParamDecl p;
    p.kind = TemplateParamDecl::TypeParam;
    p.name = name;
    return p;
}

TEST(CppEmitterStdlib, GenericStructEmitsTemplatePrefix) {
    // Model for `template<typename T> struct Box { T v; };`.
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Box";
    ty.templateParams = {typeParam("T")};
    TranspileField fld;
    fld.name = "v";
    fld.type = plainNamed("T");
    ty.fields.push_back(std::move(fld));
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <typename T>\nstruct Box"), std::string::npos)
        << "Generated:\n"
        << code;
}

TEST(CppEmitterStdlib, NonGenericStructByteIdenticalNoTemplatePrefix) {
    std::string code = emitCppStructWithBases("Plain", {});
    EXPECT_EQ(code.find("template <"), std::string::npos)
        << "no template clause expected for a non-generic struct:\n"
        << code;
}

TEST(CppEmitterStdlib, GenericFunctionEmitsTwoTypeParams) {
    TranspileModule mod;
    TranspileFunction fn;
    fn.qualifiedName = "convert";
    fn.returnType = plainNamed("U");
    fn.templateParams = {typeParam("T"), typeParam("U")};
    mod.functions.push_back(std::move(fn));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <typename T, typename U>"), std::string::npos)
        << "Generated:\n"
        << code;
}

TEST(CppEmitterStdlib, NonGenericFunctionByteIdenticalNoTemplatePrefix) {
    TranspileModule mod;
    TranspileFunction fn;
    fn.qualifiedName = "plainFn";
    fn.returnType = plainNamed("void");
    mod.functions.push_back(std::move(fn));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_EQ(code.find("template <"), std::string::npos)
        << "no template clause expected for a non-generic function:\n"
        << code;
}

// --- C++20 concept constrained-parameter form: `template <Bound T>` ---
// Same wire contract as Rust's <T: Bound>, Java/TS's <T extends Bound>, and
// Python's [T: Bound]; CppEmitter uses the C++20 constrained-parameter form
// rather than `requires Bound<T>` for per-parameter clarity.

TEST(CppEmitterStdlib, GenericStructWithSingleBoundEmitsConstrainedParam) {
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Box";
    TemplateParamDecl tp = typeParam("T");
    tp.constraintType = plainNamed("Sortable");
    ty.templateParams = {tp};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <Sortable T>\nstruct Box"), std::string::npos)
        << "Generated:\n" << code;
}

TEST(CppEmitterStdlib, GenericFunctionWithSingleBoundEmitsConstrainedParam) {
    TranspileModule mod;
    TranspileFunction fn;
    fn.qualifiedName = "pick";
    fn.returnType = plainNamed("T");
    TemplateParamDecl tp = typeParam("T");
    tp.constraintType = plainNamed("Sortable");
    fn.templateParams = {tp};
    mod.functions.push_back(std::move(fn));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <Sortable T>"), std::string::npos)
        << "Generated:\n" << code;
}

TEST(CppEmitterStdlib, MultiSegmentBoundEmitsWithDoubleColon) {
    // A bound carried as a qualified concept path renders with `::`.
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Container";
    TemplateParamDecl tp = typeParam("T");
    TypeNode bound;
    bound.nameParts = {"std", "integral"};
    tp.constraintType = bound;
    ty.templateParams = {tp};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <std::integral T>\nstruct Container"),
              std::string::npos)
        << "Generated:\n" << code;
}

TEST(CppEmitterStdlib, UnboundedTypeParamByteIdenticalToPreBoundsOutput) {
    // Absence of a bound must still emit `template <typename T>` — no stray
    // bound text appears in the clause.
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Box";
    ty.templateParams = {typeParam("T")};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <typename T>\nstruct Box"), std::string::npos)
        << "Generated:\n" << code;
    EXPECT_EQ(code.find("template <typename T = "), std::string::npos)
        << "no default expected when defaultType absent; got:\n" << code;
}

// --- C++17 default type-param: `template <typename T = X>` ---
// Same wire contract as TS's `<T = X>`, Python PEP 696 `[T = X]`, and Rust's
// struct/enum/trait-site default (Rust forbids defaults on free fns). C++17
// permits defaults on both class- and function-templates, so the emitter
// renders uniformly at every template-clause site.

TEST(CppEmitterStdlib, GenericStructWithDefaultEmitsAssign) {
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Box";
    TemplateParamDecl tp = typeParam("T");
    TypeNode def;
    def.nameParts = {"int"};
    tp.defaultType = def;
    ty.templateParams = {tp};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <typename T = int>\nstruct Box"),
              std::string::npos)
        << "Generated:\n" << code;
}

TEST(CppEmitterStdlib, GenericFunctionWithDefaultEmitsAssign) {
    TranspileModule mod;
    TranspileFunction fn;
    fn.qualifiedName = "id";
    fn.returnType = plainNamed("T");
    TemplateParamDecl tp = typeParam("T");
    TypeNode def;
    def.nameParts = {"int"};
    tp.defaultType = def;
    fn.templateParams = {tp};
    mod.functions.push_back(std::move(fn));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <typename T = int>"), std::string::npos)
        << "Generated:\n" << code;
}

TEST(CppEmitterStdlib, MultiSegmentDefaultEmitsWithDoubleColon) {
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Bag";
    TemplateParamDecl tp = typeParam("T");
    TypeNode def;
    def.nameParts = {"std", "string"};
    tp.defaultType = def;
    ty.templateParams = {tp};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <typename T = std::string>\nstruct Bag"),
              std::string::npos)
        << "Generated:\n" << code;
}

// --- Non-type template parameter: `template <int N>` ---
// `kind="nontype"` + constraintType (value type) renders as `Type N`.

TEST(CppEmitterStdlib, NonTypeParamRendersValueTypeAndName) {
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Buffer";
    TemplateParamDecl tp;
    tp.kind = TemplateParamDecl::NonTypeParam;
    tp.name = "N";
    tp.constraintType = plainNamed("int");
    ty.templateParams = {tp};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <int N>\nstruct Buffer"), std::string::npos)
        << "Generated:\n" << code;
}

TEST(CppEmitterStdlib, NonTypeParamMultiSegmentValueTypeUsesDoubleColon) {
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Bag";
    TemplateParamDecl tp;
    tp.kind = TemplateParamDecl::NonTypeParam;
    tp.name = "N";
    TypeNode vt;
    vt.nameParts = {"std", "size_t"};
    tp.constraintType = vt;
    ty.templateParams = {tp};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <std::size_t N>\nstruct Bag"),
              std::string::npos)
        << "Generated:\n" << code;
}

TEST(CppEmitterStdlib, MixedTypeAndNonTypeParamsKeepWireOrder) {
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Arr";
    TemplateParamDecl t = typeParam("T");
    TemplateParamDecl n;
    n.kind = TemplateParamDecl::NonTypeParam;
    n.name = "N";
    n.constraintType = plainNamed("int");
    ty.templateParams = {t, n};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <typename T, int N>\nstruct Arr"),
              std::string::npos)
        << "Generated:\n" << code;
}

TEST(CppEmitterStdlib, NonTypeParamWithDefaultLiteralAppendsAssign) {
    // `template <int N = 10>` — non-type template parameter with default
    // literal. The defaultValue string is emitted verbatim after the
    // parameter declaration.
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Buf";
    TemplateParamDecl tp;
    tp.kind = TemplateParamDecl::NonTypeParam;
    tp.name = "N";
    tp.constraintType = plainNamed("int");
    tp.defaultValue = "10";
    ty.templateParams = {tp};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <int N = 10>\nstruct Buf"),
              std::string::npos)
        << "Generated:\n" << code;
}

TEST(CppEmitterStdlib, NonTypeParamWithoutDefaultUnchanged) {
    // Byte-identical contract: NonTypeParam without `defaultValue` renders
    // with no trailing `= …`.
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Buf";
    TemplateParamDecl tp;
    tp.kind = TemplateParamDecl::NonTypeParam;
    tp.name = "N";
    tp.constraintType = plainNamed("int");
    ty.templateParams = {tp};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <int N>\nstruct Buf"), std::string::npos)
        << "Generated:\n" << code;
    EXPECT_EQ(code.find("= "), std::string::npos)
        << "Absent defaultValue must not introduce an `= …` clause; got:\n"
        << code;
}

TEST(CppEmitterStdlib, BoundAndDefaultEmitInConceptThenAssignOrder) {
    // C++20 constrained-parameter form coexists with C++17 default value:
    // `template <Concept T = X> struct/fn ...`. Both apply per parameter.
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Container";
    TemplateParamDecl tp = typeParam("T");
    TypeNode bound;
    bound.nameParts = {"std", "integral"};
    tp.constraintType = bound;
    TypeNode def;
    def.nameParts = {"int"};
    tp.defaultType = def;
    ty.templateParams = {tp};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(
        code.find("template <std::integral T = int>\nstruct Container"),
        std::string::npos)
        << "Generated:\n" << code;
}

// --- C++20 `requires`-clause multi-bound: trailing requires form ---
// Two or more bounds on the same type-param cannot ride the constrained-
// parameter form (`<Concept T>` permits only one concept), so the emitter
// switches the whole clause to the trailing `requires` form. Single-bound
// + concept-constrained output stays byte-identical to the pre-multi-bound
// path — covered by the byte-identical contract test above.

TEST(CppEmitterStdlib, RequiresMultiBoundEmitsTrailingClause) {
    // Model for `template <typename T> requires Sortable<T> && Hashable<T>
    //            struct Box {};` — the trailing-requires acceptance shape.
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Box";
    TemplateParamDecl tp = typeParam("T");
    tp.constraintType = plainNamed("Sortable");
    tp.extraBounds.push_back(plainNamed("Hashable"));
    ty.templateParams = {tp};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(
        code.find("template <typename T> requires Sortable<T> && Hashable<T>"
                  "\nstruct Box"),
        std::string::npos)
        << "Generated:\n" << code;
    // No constrained-parameter form must leak through when multi-bound.
    EXPECT_EQ(code.find("template <Sortable T"), std::string::npos)
        << "constrained-parameter form must NOT be used for multi-bound:\n"
        << code;
}

TEST(CppEmitterStdlib, RequiresMultiBoundFunctionEmitsTrailingClause) {
    // Same shape on a function template — exercises the `templateClause()`
    // path from `emitFunction`.
    TranspileModule mod;
    TranspileFunction fn;
    fn.qualifiedName = "pick";
    fn.returnType = plainNamed("T");
    TemplateParamDecl tp = typeParam("T");
    tp.constraintType = plainNamed("Sortable");
    tp.extraBounds.push_back(plainNamed("Hashable"));
    fn.templateParams = {tp};
    mod.functions.push_back(std::move(fn));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(
        code.find("template <typename T> requires Sortable<T> && Hashable<T>"),
        std::string::npos)
        << "Generated:\n" << code;
}

TEST(CppEmitterStdlib, RequiresMultiBoundMultiSegmentConceptUsesDoubleColon) {
    // Multi-segment concept paths (`std::integral`) join with `::` inside
    // each `Concept<T>` clause.
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Box";
    TemplateParamDecl tp = typeParam("T");
    TypeNode a;
    a.nameParts = {"std", "integral"};
    tp.constraintType = a;
    TypeNode b;
    b.nameParts = {"my", "ns", "Hashable"};
    tp.extraBounds.push_back(b);
    ty.templateParams = {tp};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(
        code.find("template <typename T> requires std::integral<T> && "
                  "my::ns::Hashable<T>\nstruct Box"),
        std::string::npos)
        << "Generated:\n" << code;
}

TEST(CppEmitterStdlib, SingleBoundUnaffectedByMultiBoundPath) {
    // Byte-identical contract: a single concept-constrained type-param keeps
    // the existing `template <Sortable T>` constrained-parameter form even
    // though the trailing-requires emitter path now exists. This is the
    // explicit guard that the *ByteIdentical* contract is upheld.
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Box";
    TemplateParamDecl tp = typeParam("T");
    tp.constraintType = plainNamed("Sortable");
    ty.templateParams = {tp};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <Sortable T>\nstruct Box"),
              std::string::npos)
        << "single-bound output must NOT switch to trailing-requires form:\n"
        << code;
    EXPECT_EQ(code.find("requires "), std::string::npos)
        << "no requires clause expected for single-bound:\n" << code;
}

// --- Variadic template parameter: `template <typename... Ts>` ---
// A TypeParam flagged `isVariadic` renders as a parameter pack. The flag is
// orthogonal to kind — the param stays kind=TypeParam.

TEST(CppEmitterStdlib, VariadicTypeParamRendersPack) {
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Tuple";
    TemplateParamDecl tp = typeParam("Ts");
    tp.isVariadic = true;
    ty.templateParams = {tp};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <typename... Ts>\nstruct Tuple"),
              std::string::npos)
        << "variadic type param must render as `typename... Ts`:\n" << code;
}

TEST(CppEmitterStdlib, VariadicTypeParamOnFunctionRendersPack) {
    TranspileModule mod;
    TranspileFunction fn;
    fn.qualifiedName = "make";
    fn.returnType = plainNamed("void");
    TemplateParamDecl tp = typeParam("Args");
    tp.isVariadic = true;
    fn.templateParams = {tp};
    mod.functions.push_back(std::move(fn));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <typename... Args>"), std::string::npos)
        << "variadic type param on a function must render as a pack:\n" << code;
}

TEST(CppEmitterStdlib, NonVariadicTypeParamByteIdenticalNoEllipsis) {
    // Byte-identical contract: a TypeParam with `isVariadic=false` (the
    // default) renders with no stray `...`.
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Box";
    ty.templateParams = {typeParam("T")};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <typename T>\nstruct Box"), std::string::npos)
        << "non-variadic type param must be unchanged:\n" << code;
    EXPECT_EQ(code.find("..."), std::string::npos)
        << "no ellipsis expected for a non-variadic param:\n" << code;
}

// --- Template-template parameter: `template <template<typename> class C>` ---
// kind=TemplateTemplateParam renders the nested `template <...>` clause from
// `innerParams` (one `typename` per inner param).

TEST(CppEmitterStdlib, TemplateTemplateParamRendersNestedClause) {
    TranspileModule mod;
    TranspileType ty;
    ty.qualifiedName = "Holder";
    TemplateParamDecl tp;
    tp.kind = TemplateParamDecl::TemplateTemplateParam;
    tp.name = "C";
    TemplateParamDecl inner;
    inner.kind = TemplateParamDecl::TypeParam;
    tp.innerParams = {inner};
    ty.templateParams = {tp};
    mod.types.push_back(std::move(ty));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <template <typename> class C>\nstruct Holder"),
              std::string::npos)
        << "template-template param must render the nested clause:\n" << code;
}

TEST(CppEmitterStdlib, TemplateTemplateParamOnFunctionRendersNestedClause) {
    TranspileModule mod;
    TranspileFunction fn;
    fn.qualifiedName = "wrap";
    fn.returnType = plainNamed("void");
    TemplateParamDecl tp;
    tp.kind = TemplateParamDecl::TemplateTemplateParam;
    tp.name = "C";
    TemplateParamDecl inner;
    inner.kind = TemplateParamDecl::TypeParam;
    tp.innerParams = {inner};
    fn.templateParams = {tp};
    mod.functions.push_back(std::move(fn));
    CppEmitter emitter;
    std::string code = emitter.emit(mod).code;
    EXPECT_NE(code.find("template <template <typename> class C>"),
              std::string::npos)
        << "template-template param on a function must render the nested clause:\n"
        << code;
}
