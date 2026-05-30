// Self-contained C++20 parity test for the product runtime configuration
// library. Mirrors, case-for-case, the five Python acceptance suites:
//
//   test_config_model.py          -> MergePrecedence + TopoTomlBoundary
//   test_config_rw.py             -> ReadWrite + ValueTypeContract + WriteGate
//   test_config_tags_perm.py      -> TagQuery + Tiering + WriteGateMultiLevel
//   test_config_inline_internal.py-> Inline*/PureInternal*
//   test_config_browse.py         -> Browse schema/transparency/dev-listing
//
// Compiled with the *system* compiler, no project CMake, no LLVM. Build:
//   clang++ -std=c++20 -I topo-lang-cpp/runtime/include \
//           topo-lang-cpp/runtime/test/test_config.cpp -o /tmp/topo_cfg_test
//   /tmp/topo_cfg_test
//
// A tiny assertion harness (no GTest dependency) keeps this standalone.

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <unistd.h>

#include <topo/config.h>

using namespace topo::config;

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_case = "";

void check(bool cond, const char* expr, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL [%s] line %d: %s\n", g_case, line, expr);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

template <typename Fn>
bool throws(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (...) {
        return true;
    }
}

// Returns the what() of whatever the callable throws, or "" if it does
// not throw — used to assert message content (key naming, stdlib-bridging gap).
template <typename Fn>
std::string throws_msg(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "<non-std exception>";
    }
    return "";
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

std::vector<std::pair<std::string, std::function<void()>>> g_cases;
struct Reg {
    Reg(const char* name, std::function<void()> fn) {
        g_cases.emplace_back(name, std::move(fn));
    }
};
#define CASE(name)                                          \
    static void name();                                     \
    static Reg reg_##name(#name, name);                     \
    static void name()

// ============================================================================
// test_config_model.py — frozen b/a/c merge, provenance, Topo.toml boundary
// ============================================================================

LayeredConfig sample_layered() {
    return LayeredConfig(
        {{"log.level", "warn"}, {"cache.size", 64}, {"retry.count", 1}},
        {{"cache.size", 256}, {"retry.count", 3}, {"feature.flag", true}},
        {{"retry.count", 9}, {"tracing.enabled", false}});
}

CASE(each_key_unique_effective_and_provenance) {
    auto r = sample_layered().resolve_all();
    CHECK(r["log.level"].value == ConfigValue("warn"));
    CHECK(r["log.level"].layer == Layer::B);
    CHECK(r["cache.size"].value == ConfigValue(std::int64_t{256}));
    CHECK(r["cache.size"].layer == Layer::A);
    CHECK(r["feature.flag"].value == ConfigValue(true));
    CHECK(r["feature.flag"].layer == Layer::A);
    CHECK(r["retry.count"].value == ConfigValue(std::int64_t{9}));
    CHECK(r["retry.count"].layer == Layer::C);
    CHECK(r["tracing.enabled"].value == ConfigValue(false));
    CHECK(r["tracing.enabled"].layer == Layer::C);
}

CASE(keys_enumerates_every_layer_once_sorted) {
    auto k = sample_layered().keys();
    std::vector<std::string> want = {"cache.size", "feature.flag", "log.level",
                                     "retry.count", "tracing.enabled"};
    CHECK(k == want);
}

CASE(iter_provenance_triples) {
    auto triples = iter_provenance(sample_layered().resolve_all());
    CHECK(triples.size() == 5);
    CHECK(std::get<0>(triples[0]) == "cache.size");
    CHECK(std::get<2>(triples[0]) == Layer::A);
    CHECK(std::get<0>(triples[3]) == "retry.count");
    CHECK(std::get<1>(triples[3]) == ConfigValue(std::int64_t{9}));
    CHECK(std::get<2>(triples[3]) == Layer::C);
}

CASE(merge_layers_helper_matches) {
    auto r = merge_layers({{"x", 1}}, {{"x", 2}}, {{"x", 3}});
    CHECK(r["x"].value == ConfigValue(std::int64_t{3}));
    CHECK(r["x"].layer == Layer::C);
}

CASE(unknown_key_raises) {
    CHECK(throws([] { sample_layered().resolve("does.not.exist"); }));
}

CASE(d_layer_is_not_a_runtime_merge_layer) {
    CHECK(throws([] { LayeredConfig().layer_map(Layer::D); }));
}

CASE(build_section_key_rejected_points_to_topo_toml) {
    auto m = throws_msg([] { reject_if_build_config_key("build.language"); });
    CHECK(contains(m, "Topo.toml"));
    CHECK(contains(m, PRODUCT_CONFIG_FILENAME));
}

CASE(feature_mode_section_keys_rejected) {
    for (const char* k : {"parallel.mode", "adaptive.min_trigger_ns",
                          "optimize.indirection", "check.jobs", "topo.root"})
        CHECK(throws([&] { reject_if_build_config_key(k); }));
}

CASE(build_key_in_a_layer_rejected_on_resolve_all) {
    LayeredConfig c({}, {{"build.standard", "c++20"}});
    auto m = throws_msg([&] { c.resolve_all(); });
    CHECK(contains(m, "Topo.toml"));
}

CASE(product_key_with_similar_name_not_rejected) {
    reject_if_build_config_key("checkout.timeout_ms");
    reject_if_build_config_key("testing_endpoint.url");
    LayeredConfig c({{"checkout.timeout_ms", 5000}});
    CHECK(c.resolve("checkout.timeout_ms").value ==
          ConfigValue(std::int64_t{5000}));
}

// ============================================================================
// test_config_rw.py — get/set, stdlib contract, identity-independent gate
// ============================================================================

CASE(set_then_get_through_store) {
    ConfigStore s(LayeredConfig(FlatMap{{"log.level", "warn"}}));
    s.set("log.level", "debug");
    CHECK(s.get("log.level") == ConfigValue("debug"));
    CHECK(s.resolve("log.level").layer == Layer::A);
    s.layered().injected["log.level"] = ConfigValue("trace");
    CHECK(s.get("log.level") == ConfigValue("trace"));
    CHECK(s.resolve("log.level").layer == Layer::C);
}

CASE(get_default_only_when_no_layer_sets_key) {
    ConfigStore s(LayeredConfig(FlatMap{{"present", 1}}));
    CHECK(s.get("absent", ConfigValue(42)) == ConfigValue(std::int64_t{42}));
    CHECK(throws([&] { s.get("absent"); }));
    CHECK(s.get("present", ConfigValue(99)) == ConfigValue(std::int64_t{1}));
}

CASE(set_reflected_in_serialized_external_toml) {
    std::string path = "/tmp/topo_cfg_rw_" + std::to_string(::getpid()) + ".toml";
    {
        auto pc = ProductConfig::from_file(path);
        pc.set("cache.size", 256);
        pc.set("log.level", "debug");
        pc.set("feature.flags", ConfigValue(Sequence{ConfigValue("a"),
                                                     ConfigValue("b")}));
    }
    // A fresh ProductConfig over the same file reads it back through the
    // bridge's own minimal reader (the round-trip the Python suite does
    // through stdlib tomllib).
    auto pc2 = ProductConfig::from_file(path);
    CHECK(pc2.get("cache.size") == ConfigValue(std::int64_t{256}));
    CHECK(pc2.get("log.level") == ConfigValue("debug"));
    Sequence flags = {ConfigValue("a"), ConfigValue("b")};
    CHECK(pc2.get("feature.flags") == ConfigValue(flags));
    std::remove(path.c_str());
}

CASE(keys_enumerates_all_layers) {
    ConfigStore s(LayeredConfig({{"a.x", 1}}, {}, {{"c.z", 3}}));
    s.set("b.y", 2);
    std::vector<std::string> want = {"a.x", "b.y", "c.z"};
    CHECK(s.keys() == want);
}

CASE(stdlib_scalars_accepted) {
    ConfigStore s;
    s.set("s", "str");
    s.set("i", 7);
    s.set("f", 1.5);
    s.set("b", true);
    s.set("arr", ConfigValue(Sequence{1, 2, 3}));
    Record rec = {{"id", ConfigValue(1)}, {"amount", ConfigValue(2.0)}};
    s.set("rec", ConfigValue(rec));
    CHECK(s.get("rec") == ConfigValue(rec));
}

CASE(datetime_rejected_points_to_stdlib_gap) {
    ConfigStore s;
    auto m = throws_msg([&] { s.set("event.at", ConfigValue::datetime()); });
    CHECK(contains(m, "event.at"));
    CHECK(contains(m, "stdlib-bridging-types"));
    CHECK(contains(m, "time_*"));
}

CASE(datetime_nested_in_array_rejected) {
    ConfigStore s;
    auto m = throws_msg([&] {
        s.set("schedule.points", ConfigValue(Sequence{ConfigValue::datetime()}));
    });
    CHECK(contains(m, "schedule.points"));
}

CASE(non_stdlib_object_rejected_and_located) {
    ConfigStore s;
    auto m = throws_msg([&] { s.set("weird.value", ConfigValue::unbridged()); });
    CHECK(contains(m, "weird.value"));
    CHECK(contains(m, "stdlib-bridging-types"));
}

CASE(build_toolchain_key_still_rejected_on_write) {
    ConfigStore s;
    auto m = throws_msg([&] { s.set("build.standard", "c++20"); });
    CHECK(contains(m, "Topo.toml"));
}

ConfigStore gate_store() {
    ConfigStore s;
    s.declare("db.dsn", ItemPolicy(ImpactLevel::HIGH));
    s.declare("ui.theme", ItemPolicy(ImpactLevel::LOW));
    return s;
}

CASE(high_impact_write_without_credential_rejected) {
    auto s = gate_store();
    auto m = throws_msg([&] { s.set("db.dsn", "postgres://prod"); });
    CHECK(contains(m, "db.dsn"));
    CHECK(contains(m, "HIGH"));
    CHECK(!contains(m, "human"));
    CHECK(!contains(m, "agent"));
}

CASE(high_impact_write_with_credential_succeeds) {
    auto s = gate_store();
    s.set("db.dsn", "postgres://prod", 1);
    CHECK(s.get("db.dsn") == ConfigValue("postgres://prod"));
}

CASE(low_impact_write_needs_no_credential) {
    auto s = gate_store();
    s.set("ui.theme", "dark");
    CHECK(s.get("ui.theme") == ConfigValue("dark"));
}

CASE(undeclared_item_defaults_to_low_impact) {
    ConfigStore s;
    s.set("anything.unlisted", 1);
    CHECK(s.get("anything.unlisted") == ConfigValue(std::int64_t{1}));
}

CASE(gate_is_identity_independent_behaviourally) {
    // The signature carries no principal — enforced at the type level
    // (set/authorize_write take only a credential level). Behavioural
    // equivalence: two callers, same level, same result.
    auto a = gate_store();
    auto b = gate_store();
    CHECK(throws([&] { a.set("db.dsn", "x", 0); }));
    CHECK(throws([&] { b.set("db.dsn", "x", 0); }));
    a.set("db.dsn", "ok", 1);
    b.set("db.dsn", "ok", 1);
    CHECK(a.get("db.dsn") == b.get("db.dsn"));
}

// ============================================================================
// test_config_tags_perm.py — tags, two orthogonal permission roles
// ============================================================================

ConfigStore tagged_store() {
    ConfigStore s(LayeredConfig({{"log.level", "warn"},
                                 {"net.timeout_ms", 5000},
                                 {"net.retries", 3},
                                 {"cache.size", 256},
                                 {"db.dsn", "postgres://local"},
                                 {"secret.api_key", "k-xxx"}}));
    s.declare("log.level", ItemPolicy::with_tags({"obs"}));
    s.declare("net.timeout_ms", ItemPolicy::with_tags({"network", "tuning"}));
    s.declare("net.retries", ItemPolicy::with_tags({"network"}));
    s.declare("cache.size", ItemPolicy::with_tags({"tuning"}));
    s.declare("db.dsn",
              ItemPolicy(ImpactLevel::HIGH, {"network"}, 2));
    s.declare("secret.api_key",
              ItemPolicy(ImpactLevel::HIGH, {"network"}, 3));
    return s;
}

CASE(single_tag_returns_exact_subset) {
    auto s = tagged_store();
    std::vector<std::string> want = {"net.retries", "net.timeout_ms"};
    CHECK(s.query({"network"}) == want);
}

CASE(multi_tag_is_AND_combination) {
    auto s = tagged_store();
    std::vector<std::string> want = {"net.timeout_ms"};
    CHECK(s.query({"network", "tuning"}) == want);
    CHECK(s.query({"tuning", "network"}) == want);
}

CASE(no_tag_returns_all_non_permission_items) {
    auto s = tagged_store();
    std::vector<std::string> want = {"cache.size", "log.level", "net.retries",
                                     "net.timeout_ms"};
    CHECK(s.query() == want);
}

CASE(tag_with_no_match_returns_empty) {
    auto s = tagged_store();
    CHECK(s.query({"does-not-exist"}).empty());
}

CASE(query_resolved_carries_values_and_provenance) {
    auto s = tagged_store();
    auto rv = s.query_resolved({"tuning"});
    CHECK(rv.size() == 2);
    CHECK(rv.count("net.timeout_ms") && rv.count("cache.size"));
    CHECK(rv["cache.size"].value == ConfigValue(std::int64_t{256}));
}

CASE(gated_item_hidden_without_credential) {
    auto s = tagged_store();
    auto q = s.query();
    CHECK(std::find(q.begin(), q.end(), "db.dsn") == q.end());
    CHECK(std::find(q.begin(), q.end(), "secret.api_key") == q.end());
    CHECK(throws([&] { s.read("db.dsn"); }));
}

CASE(each_level_sees_that_levels_complete_range) {
    auto s = tagged_store();
    auto l2 = s.query({}, 2);
    CHECK(std::find(l2.begin(), l2.end(), "db.dsn") != l2.end());
    CHECK(std::find(l2.begin(), l2.end(), "secret.api_key") == l2.end());
    CHECK(s.read("db.dsn", 2) == ConfigValue("postgres://local"));
    CHECK(throws([&] { s.read("secret.api_key", 2); }));
}

CASE(tiered_transparency_highest_level_enumerates_everything) {
    auto s = tagged_store();
    int top = s.max_read_level();
    CHECK(top == 3);
    auto enumerated = s.query({}, top);
    std::set<std::string> e(enumerated.begin(), enumerated.end());
    auto all = s.keys();
    std::set<std::string> a(all.begin(), all.end());
    CHECK(e == a);
    for (const auto& k : s.keys()) s.read(k, top);  // all readable at top
}

CASE(tag_filter_and_read_tier_are_orthogonal) {
    auto s = tagged_store();
    int top = s.max_read_level();
    std::vector<std::string> want_top = {"db.dsn", "net.retries",
                                         "net.timeout_ms", "secret.api_key"};
    CHECK(s.query({"network"}, top) == want_top);
    std::vector<std::string> want_none = {"net.retries", "net.timeout_ms"};
    CHECK(s.query({"network"}) == want_none);
}

CASE(two_callsites_different_args_different_visibility) {
    auto s = tagged_store();
    auto site_one = s.query({"network"});
    auto site_two = s.query({"network"}, s.max_read_level());
    CHECK(site_one != site_two);
    CHECK(std::find(site_one.begin(), site_one.end(), "db.dsn") ==
          site_one.end());
    CHECK(std::find(site_two.begin(), site_two.end(), "db.dsn") !=
          site_two.end());
}

CASE(mid_level_threshold_via_required_credential_table) {
    // Insert a mid threshold by extending the explicit table, not by
    // rewriting logic — proves the scale is multi-level. Mirrors the
    // Python test mutating _REQUIRED_CREDENTIAL_LEVEL then restoring it.
    auto& table = required_credential_level_table();
    auto saved = table;
    table[ImpactLevel::HIGH] = 2;
    {
        ConfigStore s;
        s.declare("db.dsn", ItemPolicy(ImpactLevel::HIGH));
        CHECK(throws([&] { s.set("db.dsn", "x", 1); }));
        s.set("db.dsn", "ok", 2);
        CHECK(s.get("db.dsn") == ConfigValue("ok"));
    }
    table = saved;
}

CASE(read_level_and_write_gate_are_independent_fields) {
    ConfigStore s;
    s.declare("public.but.guarded",
              ItemPolicy(ImpactLevel::HIGH, {}, 0));
    s.declare("gated.but.cheap", ItemPolicy(ImpactLevel::LOW, {}, 2));
    CHECK(required_read_level(s.policy_of("public.but.guarded")) == 0);
    CHECK(throws([&] { s.set("public.but.guarded", 1); }));
    CHECK(required_read_level(s.policy_of("gated.but.cheap")) == 2);
    s.set("gated.but.cheap", 1);  // write gate does not bite
    CHECK(throws([&] { s.read("gated.but.cheap"); }));  // read tier bites
}

CASE(product_config_query_passthrough) {
    ProductConfig pc({{"a", 1}, {"b", 2}});
    pc.declare("b", ItemPolicy::with_tags({"x"}));
    pc.declare("a", ItemPolicy::with_read_level(2));
    std::vector<std::string> b_only = {"b"};
    CHECK(pc.query() == b_only);
    CHECK(pc.query({"x"}) == b_only);
    CHECK(pc.max_read_level() == 2);
    std::vector<std::string> ab = {"a", "b"};
    CHECK(pc.query({}, 2) == ab);
    CHECK(pc.read("a", 2) == ConfigValue(std::int64_t{1}));
}

// ============================================================================
// test_config_inline_internal.py — inline (b) round-trip, pure-internal (d)
// ============================================================================

const std::string kTomlSrc =
    "log_level = \"info\"\n"
    "retries = 3\n"
    "ratio = 0.5\n"
    "enabled = true\n"
    "\n"
    "[net]\n"
    "host = \"example.com\"\n"
    "ports = [80, 443]\n";

CASE(inline_declared_defaults_need_no_external_file) {
    ProductConfig pc;  // pathless
    pc.declare_inlined_toml(kTomlSrc);
    CHECK(!pc.path().has_value());
    CHECK(pc.get("log_level") == ConfigValue("info"));
    CHECK(pc.get("net.host") == ConfigValue("example.com"));
    Sequence ports = {ConfigValue(std::int64_t{80}), ConfigValue(std::int64_t{443})};
    CHECK(pc.get("net.ports") == ConfigValue(ports));
    for (const auto& k : pc.keys()) CHECK(pc.resolve(k).layer == Layer::B);
}

CASE(accepts_already_decoded_mapping_too) {
    ProductConfig pc;
    pc.declare_inlined_toml(std::map<std::string, ConfigValue>{
        {"a", 1}, {"nested.b", 2}});
    CHECK(pc.get("a") == ConfigValue(std::int64_t{1}));
    CHECK(pc.get("nested.b") == ConfigValue(std::int64_t{2}));
}

CASE(restore_yields_toml_reparsing_to_identical_data) {
    ProductConfig pc;
    pc.declare_inlined_toml(kTomlSrc);
    std::string restored = pc.restore_inlined_toml();
    auto reparsed = parse_toml(restored);
    auto original = parse_toml(kTomlSrc);
    CHECK(reparsed == original);
}

CASE(restore_is_idempotent_under_reparse) {
    ProductConfig pc;
    pc.declare_inlined_toml(kTomlSrc);
    std::string once = pc.restore_inlined_toml();
    ProductConfig pc2;
    pc2.declare_inlined_toml(once);
    CHECK(parse_toml(pc2.restore_inlined_toml()) == parse_toml(once));
}

CASE(empty_inline_restores_to_empty) {
    ProductConfig pc;
    pc.declare_inlined_toml(std::map<std::string, ConfigValue>{});
    CHECK(pc.restore_inlined_toml() == "");
    CHECK(parse_toml(pc.restore_inlined_toml()).empty());
}

CASE(inlined_items_still_enumerate_under_normal_rules) {
    ProductConfig pc;
    pc.declare_inlined_toml(kTomlSrc);
    auto keys = pc.keys();
    for (const char* k : {"log_level", "retries", "ratio", "enabled",
                          "net.host", "net.ports"})
        CHECK(std::find(keys.begin(), keys.end(), k) != keys.end());
    pc.declare("retries", ItemPolicy::with_tags({"tuning"}));
    pc.declare("net.host", ItemPolicy::with_read_level(2));
    std::vector<std::string> retries_only = {"retries"};
    CHECK(pc.query({"tuning"}) == retries_only);
    auto q0 = pc.query();
    CHECK(std::find(q0.begin(), q0.end(), "net.host") == q0.end());
    auto q2 = pc.query({}, 2);
    CHECK(std::find(q2.begin(), q2.end(), "net.host") != q2.end());
    auto rv = pc.query_resolved();
    CHECK(rv.count("log_level"));
    CHECK(rv["log_level"].value == ConfigValue("info"));
}

CASE(a_and_c_still_override_inlined_b) {
    ProductConfig pc({}, {{"retries", 99}});  // injected c
    pc.declare_inlined_toml(kTomlSrc);
    CHECK(pc.get("retries") == ConfigValue(std::int64_t{99}));
    CHECK(pc.resolve("retries").layer == Layer::C);
    pc.set("log_level", "debug");
    CHECK(pc.get("log_level") == ConfigValue("debug"));
    CHECK(pc.resolve("log_level").layer == Layer::A);
    CHECK(pc.get("ratio") == ConfigValue(0.5));
    CHECK(pc.resolve("ratio").layer == Layer::B);
}

CASE(inline_layer_rejects_build_toolchain_key) {
    ProductConfig pc;
    CHECK(throws([&] {
        pc.declare_inlined_toml(std::map<std::string, ConfigValue>{
            {"build.language", "python"}});
    }));
}

CASE(declared_internal_is_dev_searchable_by_name_and_tag) {
    ProductConfig pc;
    auto v = pc.declare_internal("MAX_BUF", 4096, {"perf", "memory"});
    CHECK(v == ConfigValue(std::int64_t{4096}));
    auto names = pc.dev_internal().names();
    CHECK(std::find(names.begin(), names.end(), "MAX_BUF") != names.end());
    std::vector<std::string> mb = {"MAX_BUF"};
    CHECK(pc.dev_internal().search({"perf"}) == mb);
    CHECK(pc.dev_internal().search({"perf", "memory"}) == mb);
    CHECK(pc.dev_internal().search({"unrelated"}).empty());
    CHECK(pc.dev_internal().get("MAX_BUF").value == ConfigValue(std::int64_t{4096}));
}

CASE(internal_absent_from_every_runtime_surface) {
    ProductConfig pc({{"public.k", 1}});
    pc.declare_internal("SECRET_TUNING", 7, {"internal"});
    auto k = pc.keys();
    CHECK(std::find(k.begin(), k.end(), "SECRET_TUNING") == k.end());
    auto q = pc.query();
    CHECK(std::find(q.begin(), q.end(), "SECRET_TUNING") == q.end());
    auto q999 = pc.query({}, 999);
    CHECK(std::find(q999.begin(), q999.end(), "SECRET_TUNING") == q999.end());
    CHECK(!pc.store().resolve_all().count("SECRET_TUNING"));
    CHECK(!pc.query_resolved({}, 999).count("SECRET_TUNING"));
    CHECK(throws([&] { pc.get("SECRET_TUNING"); }));
}

CASE(promoted_value_is_a_plain_constant) {
    ProductConfig pc;
    auto v = pc.declare_internal("RATE", 0.25);
    CHECK(v.kind() == ValueKind::Float);
    CHECK(v == ConfigValue(0.25));
}

CASE(layer_d_stays_out_of_runtime_merge) {
    const auto& order = runtime_merge_order();
    CHECK(std::find(order.begin(), order.end(), Layer::D) == order.end());
    LayeredConfig c({{"k", 1}});
    CHECK(throws([&] { c.layer_map(Layer::D); }));
}

CASE(internal_value_still_honours_stdlib_contract) {
    ProductConfig pc;
    CHECK(throws([&] { pc.declare_internal("WHEN", ConfigValue::datetime()); }));
}

CASE(dev_registry_is_disjoint_from_store_type) {
    DevInternalRegistry reg;
    reg.declare("X", 1, {"t"});
    ConfigStore s(LayeredConfig(FlatMap{{"X", 2}}));
    CHECK(s.get("X") == ConfigValue(std::int64_t{2}));
    CHECK(reg.get("X").value == ConfigValue(std::int64_t{1}));
}

// ============================================================================
// test_config_browse.py — unified browse, tiered transparency, dev listing
// ============================================================================

ConfigStore browse_store() {
    ConfigStore s(LayeredConfig(
        {{"log.level", "warn"},
         {"net.timeout_ms", 1000},
         {"cache.size", 256},
         {"db.dsn", "postgres://default"}},
        {{"net.timeout_ms", 5000}},
        {{"cache.size", 512}, {"feature.flag", true}}));
    s.declare("log.level", ItemPolicy::with_tags({"obs"}));
    s.declare("net.timeout_ms",
              ItemPolicy(ImpactLevel::HIGH, {"network", "tuning"}, 0));
    s.declare("cache.size", ItemPolicy::with_tags({"tuning"}));
    s.declare("feature.flag", ItemPolicy::with_tags({"features"}));
    s.declare("db.dsn", ItemPolicy(ImpactLevel::HIGH, {"network"}, 2));
    return s;
}

std::map<std::string, BrowseEntry> by_key(const std::vector<BrowseEntry>& rows) {
    std::map<std::string, BrowseEntry> m;
    for (const auto& r : rows) m.emplace(r.key, r);
    return m;
}

CASE(every_documented_field_present_and_correct) {
    auto s = browse_store();
    auto by = by_key(s.browse({}, s.max_read_level()));

    const auto& log = by.at("log.level");
    CHECK(log.type == "str");
    CHECK(log.has_default() && *log.default_value == ConfigValue("warn"));
    CHECK(log.effective == ConfigValue("warn"));
    CHECK(log.layer == Layer::B);
    CHECK(log.impact == ImpactLevel::LOW);
    CHECK(log.required_write_level == 0);
    CHECK(log.required_read_level == 0);
    CHECK((log.tags == std::set<std::string>{"obs"}));

    const auto& net = by.at("net.timeout_ms");
    CHECK(net.type == "int");
    CHECK(*net.default_value == ConfigValue(std::int64_t{1000}));
    CHECK(net.effective == ConfigValue(std::int64_t{5000}));
    CHECK(net.layer == Layer::A);
    CHECK(net.impact == ImpactLevel::HIGH);
    CHECK(net.required_write_level == 1);
    CHECK(net.required_read_level == 0);
    CHECK((net.tags == std::set<std::string>{"network", "tuning"}));

    const auto& cache = by.at("cache.size");
    CHECK(cache.type == "int");
    CHECK(*cache.default_value == ConfigValue(std::int64_t{256}));
    CHECK(cache.effective == ConfigValue(std::int64_t{512}));
    CHECK(cache.layer == Layer::C);

    const auto& flag = by.at("feature.flag");
    CHECK(flag.type == "bool");
    CHECK(!flag.has_default());  // no inlined default -> sentinel
    CHECK(flag.effective == ConfigValue(true));
    CHECK(flag.layer == Layer::C);

    const auto& dsn = by.at("db.dsn");
    CHECK(dsn.required_read_level == 2);
    CHECK(dsn.required_write_level == 1);
    CHECK(dsn.type == "str");
}

CASE(gated_item_absent_below_tier_present_at_and_above) {
    auto s = browse_store();
    auto k0 = by_key(s.browse({}, 0));
    CHECK(!k0.count("db.dsn"));
    CHECK(by_key(s.browse({}, 2)).count("db.dsn"));
    CHECK(by_key(s.browse({}, 5)).count("db.dsn"));
}

CASE(top_level_browse_equals_complete_runtime_key_set) {
    auto s = browse_store();
    auto rows = s.browse({}, s.max_read_level());
    std::vector<std::string> browsed;
    for (const auto& r : rows) browsed.push_back(r.key);
    std::sort(browsed.begin(), browsed.end());
    CHECK(browsed == s.keys());
}

CASE(each_level_is_exactly_that_levels_complete_range) {
    auto s = browse_store();
    std::vector<std::string> zero;
    for (const auto& r : s.browse({}, 0)) zero.push_back(r.key);
    std::sort(zero.begin(), zero.end());
    std::vector<std::string> expected;
    for (const auto& k : s.keys())
        if (s.policy_of(k).read_level == 0) expected.push_back(k);
    std::sort(expected.begin(), expected.end());
    CHECK(zero == expected);
}

CASE(browse_does_not_use_resolve_all_to_leak) {
    auto s = browse_store();
    CHECK(s.resolve_all().count("db.dsn"));  // tier-blind would surface it
    auto browsed = by_key(s.browse({}, 0));
    CHECK(!browsed.count("db.dsn"));  // tier-aware door must not
}

CASE(same_level_yields_identical_browse) {
    auto s = browse_store();
    CHECK(s.browse({}, 1) == s.browse({}, 1));
}

CASE(key_added_after_construction_auto_appears) {
    auto s = browse_store();
    CHECK(!by_key(s.browse({}, 0)).count("late.added"));
    s.set("late.added", "hi");
    CHECK(by_key(s.browse({}, 0)).count("late.added"));
}

CASE(d_absent_from_runtime_browse_at_every_level) {
    ProductConfig cfg({{"log.level", "warn"}});
    cfg.declare_internal("BUILD_SALT", "abc123", {"crypto"});
    cfg.declare_internal("MAX_WIDGETS", 64, {"limits"});
    for (int level : {0, 1, 99}) {
        auto k = by_key(cfg.browse({}, level));
        CHECK(!k.count("BUILD_SALT"));
        CHECK(!k.count("MAX_WIDGETS"));
    }
    auto keys = cfg.keys();
    CHECK(std::find(keys.begin(), keys.end(), "BUILD_SALT") == keys.end());
}

CASE(d_present_only_in_dev_listing_and_tag_searchable) {
    ProductConfig cfg;
    cfg.declare_internal("BUILD_SALT", "abc123", {"crypto"});
    cfg.declare_internal("MAX_WIDGETS", 64, {"limits"});
    std::set<std::string> listed;
    for (const auto& r : cfg.dev_browse()) listed.insert(r.name);
    CHECK((listed == std::set<std::string>{"BUILD_SALT", "MAX_WIDGETS"}));
    auto crypto = cfg.dev_browse({"crypto"});
    CHECK(crypto.size() == 1 && crypto[0].name == "BUILD_SALT");
    CHECK(crypto[0].value == ConfigValue("abc123"));
    CHECK((crypto[0].tags == std::set<std::string>{"crypto"}));
}

CASE(no_d_declared_yields_empty_listing_without_registry) {
    ProductConfig cfg;
    CHECK(cfg.dev_browse().empty());
    CHECK(!cfg.dev_internal_built());  // browsing must not build it
}

CASE(bridge_browse_is_passthrough_to_model) {
    ProductConfig cfg({{"a.x", 1}, {"b.y", "two"}}, {{"a.x", 9}});
    cfg.declare("b.y", ItemPolicy(ImpactLevel::LOW, {"t"}, 1));
    auto low = by_key(cfg.browse({}, 0));
    CHECK(low.size() == 1 && low.count("a.x"));
    auto full = cfg.browse({}, cfg.max_read_level());
    auto fm = by_key(full);
    CHECK(fm.count("a.x") && fm.count("b.y"));
    const auto& ax = fm.at("a.x");
    CHECK(*ax.default_value == ConfigValue(std::int64_t{1}));
    CHECK(ax.effective == ConfigValue(std::int64_t{9}));
    CHECK(ax.layer == Layer::C);
}

}  // namespace

int main() {
    for (auto& [name, fn] : g_cases) {
        g_case = name.c_str();
        try {
            fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::printf("  FAIL [%s]: uncaught exception: %s\n", name.c_str(),
                        e.what());
        }
    }
    std::printf("\n%d cases, %d checks, %d failures\n",
                static_cast<int>(g_cases.size()), g_checks, g_failures);
    if (g_failures == 0) std::printf("ALL PASS\n");
    return g_failures == 0 ? 0 : 1;
}
