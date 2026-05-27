#ifndef TOPO_CONFIG_MODEL_H
#define TOPO_CONFIG_MODEL_H

// @stability provisional
// Language-agnostic data model for the layered product config. The
// `ValueKind` enum carries explicit schema-gap placeholders
// (`Datetime`, `Unbridged`) that will be promoted or removed; the
// tier stays `provisional` until those resolve.

// Language-agnostic core of the product runtime configuration: the
// layered b/a/c value model, its merge precedence, per-value provenance,
// the stdlib-typed value contract, the tag system, the two orthogonal
// multi-level permission roles, the unified browse, and the pure-internal
// (d) dev-phase vocabulary.
//
// This header owns *semantics only*. It deliberately has no TOML parser,
// no file I/O and no host-ecosystem behaviour: a bridge decodes its
// ecosystem's TOML into the plain ConfigValue tree this model consumes
// and projects the merged result back. The Python reference splits the
// same way (_config_model.py vs config.py); this C++ port preserves that
// split exactly so the model would read identically in any host runtime.
//
// Why a file separate from the build-time Topo.toml: Topo.toml configures
// the *toolchain build*; this model configures the *built product's*
// runtime behaviour. They answer different questions and share no
// sections; the fixed product file name here is topo-app.toml, and a
// build-toolchain key offered to the product config is a category error
// the boundary guard rejects loudly, naming Topo.toml.
//
// The fourth band d is in the vocabulary but intentionally absent from
// the runtime merge: d is promoted to a plain host constant/inline by
// the toolchain and has zero configuration footprint at runtime, so
// resolving an effective value is a b/a/c-only operation by construction.

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace topo::config {

// Fixed product runtime config filename for this proof of concept. Kept
// in the model (not a bridge) so every host agrees on the boundary name.
inline constexpr const char* PRODUCT_CONFIG_FILENAME = "topo-app.toml";

// The build toolchain owns Topo.toml; these are its section names. A key
// whose first dotted segment is one of these belongs to the build config.
// One explicit list keeps the non-overlap boundary single, not scattered.
inline const std::set<std::string>& build_toolchain_sections() {
    static const std::set<std::string> kSections = {
        "topo",     "build",         "builder",       "parallel",
        "adaptive", "optimize",      "observability", "lifetime",
        "loop_parallel", "types",    "completeness",  "check",
        "test"};
    return kSections;
}

// --- Dynamic config value --------------------------------------------------
//
// The Python reference stores already-decoded plain values (scalar / list
// / dict / datetime). C++ has no built-in dynamic type, so a tagged value
// reproduces that surface. The Datetime tag exists *only* so the type
// contract can reject TOML date/time the same way the reference does: it
// carries no payload because the model never stores one — it is the
// "value with no stdlib bridge" marker. An Unbridged tag stands for any
// other host object with no schema contract (the reference's arbitrary
// Python object case).

class ConfigValue;
using Sequence = std::vector<ConfigValue>;
using Record = std::vector<std::pair<std::string, ConfigValue>>;

enum class ValueKind { Bool, Int, Float, String, Sequence, Record, Datetime, Unbridged };

class ConfigValue {
public:
    ConfigValue() : kind_(ValueKind::Int), int_(0) {}
    // bool must be its own constructor (not folded into the integral one)
    // so it is never silently widened to int — the reference checks bool
    // before int because the two carry different stdlib contracts.
    ConfigValue(bool b) : kind_(ValueKind::Bool), bool_(b) {}
    ConfigValue(int v) : kind_(ValueKind::Int), int_(v) {}
    ConfigValue(std::int64_t v) : kind_(ValueKind::Int), int_(v) {}
    ConfigValue(double v) : kind_(ValueKind::Float), float_(v) {}
    ConfigValue(const char* s) : kind_(ValueKind::String), str_(s) {}
    ConfigValue(std::string s) : kind_(ValueKind::String), str_(std::move(s)) {}
    ConfigValue(Sequence s) : kind_(ValueKind::Sequence), seq_(std::move(s)) {}
    ConfigValue(Record r) : kind_(ValueKind::Record), rec_(std::move(r)) {}

    static ConfigValue datetime() {
        ConfigValue v;
        v.kind_ = ValueKind::Datetime;
        return v;
    }
    static ConfigValue unbridged() {
        ConfigValue v;
        v.kind_ = ValueKind::Unbridged;
        return v;
    }

    ValueKind kind() const { return kind_; }

    bool as_bool() const { return bool_; }
    std::int64_t as_int() const { return int_; }
    double as_float() const { return float_; }
    const std::string& as_string() const { return str_; }
    const Sequence& as_sequence() const { return seq_; }
    Sequence& as_sequence() { return seq_; }
    const Record& as_record() const { return rec_; }
    Record& as_record() { return rec_; }

    bool operator==(const ConfigValue& o) const {
        if (kind_ != o.kind_) return false;
        switch (kind_) {
            case ValueKind::Bool: return bool_ == o.bool_;
            case ValueKind::Int: return int_ == o.int_;
            case ValueKind::Float: return float_ == o.float_;
            case ValueKind::String: return str_ == o.str_;
            case ValueKind::Sequence: return seq_ == o.seq_;
            case ValueKind::Record: return rec_ == o.rec_;
            // Datetime/Unbridged carry no payload; same kind == equal.
            default: return true;
        }
    }
    bool operator!=(const ConfigValue& o) const { return !(*this == o); }

private:
    ValueKind kind_;
    bool bool_ = false;
    std::int64_t int_ = 0;
    double float_ = 0.0;
    std::string str_;
    Sequence seq_;
    Record rec_;
};

// A flat layer map: dotted-key -> already-decoded plain value. std::map
// so every enumeration is sorted (the reference sorts for stable,
// hand-checkable output). Aliased so call sites can name the type
// explicitly and avoid brace-init ambiguity with the copy constructor.
using FlatMap = std::map<std::string, ConfigValue>;

// --- Boundary guard --------------------------------------------------------

class BuildConfigKeyError : public std::runtime_error {
public:
    explicit BuildConfigKeyError(const std::string& m) : std::runtime_error(m) {}
};

inline std::string root_section(const std::string& key) {
    auto dot = key.find('.');
    return dot == std::string::npos ? key : key.substr(0, dot);
}

// Refuse a key that belongs in Topo.toml. The two files share no sections
// by design; accepting a build key here would make a second, silently
// ignored home for it. Rejecting loudly and naming the file it actually
// belongs to keeps the boundary honest.
inline void reject_if_build_config_key(const std::string& key) {
    const std::string section = root_section(key);
    if (build_toolchain_sections().count(section)) {
        throw BuildConfigKeyError(
            "'" + key + "' configures the build toolchain (section '[" + section +
            "]') and belongs in Topo.toml, not the product runtime config (" +
            std::string(PRODUCT_CONFIG_FILENAME) +
            "). The two files share no sections; set this in Topo.toml instead.");
    }
}

// --- Layers + provenance ---------------------------------------------------
//
// The enum values encode merge precedence (higher wins) so the merge
// never hard-codes an ordering separate from layer identity. D is listed
// for vocabulary completeness but is never produced by the runtime merge.

enum class Layer : int {
    D = 0,  // pure-internal; promoted to code, never merged at runtime
    B = 1,  // inlined / hidden TOML default embedded in the artifact
    A = 2,  // external topo-app.toml the user manages
    C = 3,  // in-code explicit injection through the topo interface
};

// Layers that participate in the runtime merge, least to most explicit.
inline const std::vector<Layer>& runtime_merge_order() {
    static const std::vector<Layer> kOrder = {Layer::B, Layer::A, Layer::C};
    return kOrder;
}

struct ResolvedValue {
    ConfigValue value;
    Layer layer;
    bool operator==(const ResolvedValue& o) const {
        return value == o.value && layer == o.layer;
    }
};

// --- Value-type contract ---------------------------------------------------
//
// A config value only enters the model if it has a stdlib bridge type, so
// every value the running product reads has a known contract. Aggregates
// are validated element-wise so a datetime smuggled inside an array or
// table is caught, not just a top-level one.

class UnbridgedValueError : public std::runtime_error {
public:
    explicit UnbridgedValueError(const std::string& m) : std::runtime_error(m) {}
};

inline std::string stdlib_type_of(const ConfigValue& value) {
    switch (value.kind()) {
        case ValueKind::Datetime:
            throw UnbridgedValueError(
                "value of type 'datetime' has no stdlib bridge type — TOML "
                "date/time maps to the not-yet-implemented time_* family (see "
                "the stdlib-bridging-types gap: the time_*/uuid/decimal128 "
                "gap). Accepting it would store a value with no schema "
                "contract; use a bridged scalar instead.");
        case ValueKind::Unbridged:
            throw UnbridgedValueError(
                "value of type 'object' has no stdlib bridge type (see roadmap "
                "the stdlib-bridging-types gap). Only string / integer / float / "
                "bool / array / table values have a schema contract; refusing "
                "to store an uncontracted value.");
        // bool is matched before int — Python's reference checks bool
        // first because its bool is an int subclass; here the tag is
        // already disjoint, but the ordering is kept for parity of intent.
        case ValueKind::Bool: return "bool";
        case ValueKind::Int: return "int";
        case ValueKind::Float: return "float";
        case ValueKind::String: return "str";
        case ValueKind::Sequence:
            for (const auto& el : value.as_sequence()) stdlib_type_of(el);
            return "slice";
        case ValueKind::Record:
            for (const auto& kv : value.as_record()) stdlib_type_of(kv.second);
            return "record";
    }
    throw UnbridgedValueError("value has no stdlib bridge type");
}

// Type-gate a value about to be written under key, re-raising with the
// offending key prepended so a rejection always locates the problem.
inline void validate_value(const std::string& key, const ConfigValue& value) {
    try {
        stdlib_type_of(value);
    } catch (const UnbridgedValueError& exc) {
        throw UnbridgedValueError("config key '" + key + "': " + exc.what());
    }
}

// --- Write protection: impact level + credential gate ----------------------
//
// This gate stops *mistaken* writes to items where a wrong value has
// outsized blast radius — a guard rail, not a secrecy boundary. It is
// identity-independent by construction: the API takes a credential
// *level*, never a principal. The integer scale leaves room for
// intermediate tiers without reshaping callers.

enum class ImpactLevel : int {
    LOW = 0,   // routine; a wrong value is easily noticed and reverted
    HIGH = 1,  // outsized blast radius; a careless write must be deliberate
};

inline const char* impact_name(ImpactLevel l) {
    return l == ImpactLevel::HIGH ? "HIGH" : "LOW";
}

// Credential level a writer must present for an item of a given impact.
// A mutable explicit table (not `impact == HIGH`) so inserting a mid
// level later is a table edit, not a logic rewrite — the reference's
// _REQUIRED_CREDENTIAL_LEVEL, mutable for the same multi-level test.
inline std::map<ImpactLevel, int>& required_credential_level_table() {
    static std::map<ImpactLevel, int> kTable = {
        {ImpactLevel::LOW, 0},
        {ImpactLevel::HIGH, 1},
    };
    return kTable;
}

inline constexpr int NO_CREDENTIAL_LEVEL = 0;

class WriteProtectionError : public std::runtime_error {
public:
    explicit WriteProtectionError(const std::string& m) : std::runtime_error(m) {}
};

// Per-item declaration carrying three orthogonal axes: tags scope
// retrieval only, read_level gates read-visibility, impact drives the
// write mis-operation gate. Stored tags as a set so identity is
// order-independent.
struct ItemPolicy {
    ImpactLevel impact = ImpactLevel::LOW;
    std::set<std::string> tags;
    int read_level = 0;

    ItemPolicy() = default;
    ItemPolicy(ImpactLevel i, std::vector<std::string> t = {}, int rl = 0)
        : impact(i), tags(t.begin(), t.end()), read_level(rl) {}
    // Tag-only / read-level-only conveniences mirroring the Python
    // keyword-argument call sites (ItemPolicy(tags=...) etc.).
    static ItemPolicy with_tags(std::vector<std::string> t) {
        ItemPolicy p;
        p.tags = std::set<std::string>(t.begin(), t.end());
        return p;
    }
    static ItemPolicy with_read_level(int rl) {
        ItemPolicy p;
        p.read_level = rl;
        return p;
    }
    ItemPolicy& set_impact(ImpactLevel i) {
        impact = i;
        return *this;
    }
    ItemPolicy& set_read_level(int rl) {
        read_level = rl;
        return *this;
    }
    ItemPolicy& set_tags(std::vector<std::string> t) {
        tags = std::set<std::string>(t.begin(), t.end());
        return *this;
    }
};

inline int required_credential_level(const ItemPolicy& policy) {
    return required_credential_level_table().at(policy.impact);
}

// The minimum permission level a caller must present to have this item
// enumerated/read. 0 means unrestricted. This is the read-visibility
// tiering role — the orthogonal twin of required_credential_level (the
// write gate). Both consult the same scale; they never collapse.
inline int required_read_level(const ItemPolicy& policy) {
    return policy.read_level;
}

// Pass iff credential_level meets the item's required level. No
// principal/identity parameter exists: the gate compares levels only,
// which is exactly the "guard against accidental writes, not secrecy"
// intent.
inline void authorize_write(const std::string& key, const ItemPolicy& policy,
                            int credential_level = NO_CREDENTIAL_LEVEL) {
    const int needed = required_credential_level(policy);
    if (credential_level < needed) {
        throw WriteProtectionError(
            "config key '" + key + "' is impact=" +
            std::string(impact_name(policy.impact)) + "; writing it requires " +
            "credential level >= " + std::to_string(needed) +
            ", but the write presented level " + std::to_string(credential_level) +
            ". This guard prevents accidental high-impact changes; re-issue " +
            "the write with a sufficient credential level if the change is " +
            "intended.");
    }
}

// --- No-default sentinel for browse ---------------------------------------
//
// Distinguishes "this item has no inlined (b) default" from "the default
// is a null-like value", so a browse consumer can tell the two apart.
// Modelled as an optional<ConfigValue> on the row: empty == _NO_DEFAULT.

// --- Browse row ------------------------------------------------------------

struct BrowseEntry {
    std::string key;
    std::string type;
    std::optional<ConfigValue> default_value;  // empty => no inlined default
    ConfigValue effective;
    Layer layer;
    ImpactLevel impact;
    int required_write_level;
    int required_read_level;
    std::set<std::string> tags;

    bool has_default() const { return default_value.has_value(); }
    bool operator==(const BrowseEntry& o) const {
        return key == o.key && type == o.type &&
               default_value == o.default_value && effective == o.effective &&
               layer == o.layer && impact == o.impact &&
               required_write_level == o.required_write_level &&
               required_read_level == o.required_read_level && tags == o.tags;
    }
};

// --- The a/b/c layers + the merge over them --------------------------------
//
// Each layer is a flat map of dotted-key -> already-decoded plain value.
// TOML parsing is a separate concern: a bridge fills these maps; this
// model only merges and attributes them. Keys are kept in std::map so
// every enumeration is sorted (the reference sorts for stable,
// hand-checkable output).

class LayeredConfig {
public:
    std::map<std::string, ConfigValue> inlined;   // layer b
    std::map<std::string, ConfigValue> external;  // layer a
    std::map<std::string, ConfigValue> injected;  // layer c

    LayeredConfig() = default;
    // explicit so a single braced layer literal cannot be read as a
    // copy-list-init of LayeredConfig itself (which would make the
    // single-argument form ambiguous against the copy constructor).
    explicit LayeredConfig(FlatMap b, FlatMap a = {}, FlatMap c = {})
        : inlined(std::move(b)), external(std::move(a)), injected(std::move(c)) {}

    // Register decoded data as the inlined (b) layer — the artifact
    // embedded default. Decode-only by design: the caller (a bridge)
    // turns TOML into this map and restores it back. Build-toolchain
    // keys are rejected here too so a misplaced key cannot sneak in.
    void install_inlined(const std::map<std::string, ConfigValue>& data) {
        for (const auto& kv : data) reject_if_build_config_key(kv.first);
        inlined = data;
    }

    const std::map<std::string, ConfigValue>& layer_map(Layer layer) const {
        if (layer == Layer::B) return inlined;
        if (layer == Layer::A) return external;
        if (layer == Layer::C) return injected;
        // Layer::D never participates in the runtime merge by construction.
        throw std::logic_error("Layer::D is not a runtime merge layer");
    }

    std::vector<std::string> keys() const {
        std::set<std::string> seen;
        for (Layer layer : runtime_merge_order())
            for (const auto& kv : layer_map(layer)) seen.insert(kv.first);
        return {seen.begin(), seen.end()};
    }

    // Effective value + provenance for one key. Walks layers
    // least-to-most explicit; the last layer carrying the key wins and
    // is the recorded provenance.
    ResolvedValue resolve(const std::string& key) const {
        reject_if_build_config_key(key);
        std::optional<ResolvedValue> winner;
        for (Layer layer : runtime_merge_order()) {
            const auto& m = layer_map(layer);
            auto it = m.find(key);
            if (it != m.end()) winner = ResolvedValue{it->second, layer};
        }
        if (!winner) throw std::out_of_range("config key not set: " + key);
        return *winner;
    }

    std::map<std::string, ResolvedValue> resolve_all() const {
        validate_keys();
        std::map<std::string, ResolvedValue> out;
        for (const auto& k : keys()) out.emplace(k, resolve(k));
        return out;
    }

private:
    void validate_keys() const {
        for (Layer layer : runtime_merge_order())
            for (const auto& kv : layer_map(layer))
                reject_if_build_config_key(kv.first);
    }
};

inline std::map<std::string, ResolvedValue> merge_layers(
    std::map<std::string, ConfigValue> inlined = {},
    std::map<std::string, ConfigValue> external = {},
    std::map<std::string, ConfigValue> injected = {}) {
    LayeredConfig cfg(std::move(inlined), std::move(external), std::move(injected));
    return cfg.resolve_all();
}

inline std::vector<std::tuple<std::string, ConfigValue, Layer>> iter_provenance(
    const std::map<std::string, ResolvedValue>& resolved) {
    std::vector<std::tuple<std::string, ConfigValue, Layer>> out;
    for (const auto& kv : resolved)  // std::map is already key-sorted
        out.emplace_back(kv.first, kv.second.value, kv.second.layer);
    return out;
}

// --- Read/write façade over the layered model ------------------------------
//
// Reads honour b ◁ a ◁ c. Writes land in the external (a) layer — the
// layer a user/agent may author; b and c are owned by other mechanisms.
// Stays language-agnostic: it mutates the decoded external map and reports
// the new value; turning that into topo-app.toml bytes is a bridge concern.

class ConfigStore {
public:
    explicit ConfigStore(LayeredConfig layered = {},
                         std::map<std::string, ItemPolicy> policies = {})
        : cfg_(std::move(layered)), policies_(std::move(policies)) {}

    LayeredConfig& layered() { return cfg_; }
    const LayeredConfig& layered() const { return cfg_; }

    // -- declaration --------------------------------------------------------

    void declare(const std::string& key, const ItemPolicy& policy) {
        reject_if_build_config_key(key);
        policies_[key] = policy;
    }

    ItemPolicy policy_of(const std::string& key) const {
        auto it = policies_.find(key);
        return it == policies_.end() ? ItemPolicy{} : it->second;
    }

    // -- tag + read-visibility query ----------------------------------------
    //
    // One query API, two orthogonal filter dimensions, zero ambient
    // state. It reads the filter from arguments and no identity — so the
    // same method from two sites with different arguments yields
    // different visibility purely from what each site passes.

    // The highest read-level any runtime item requires. A caller
    // presenting this (or above) enumerates *every* runtime item — the
    // top of the scale always sees the whole runtime range, which is
    // what makes the tiered-transparency invariant checkable. 0 when
    // nothing is permission-gated.
    int max_read_level() const {
        int top = 0;
        for (const auto& k : cfg_.keys())
            top = std::max(top, policy_of(k).read_level);
        return top;
    }

    std::vector<std::string> query(const std::vector<std::string>& tags = {},
                                   int credential_level = NO_CREDENTIAL_LEVEL) const {
        std::set<std::string> wanted(tags.begin(), tags.end());
        std::vector<std::string> out;
        for (const auto& key : cfg_.keys()) {
            if (!visible(key, credential_level)) continue;
            if (!wanted.empty() && !is_superset(policy_of(key).tags, wanted))
                continue;
            out.push_back(key);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::map<std::string, ResolvedValue> query_resolved(
        const std::vector<std::string>& tags = {},
        int credential_level = NO_CREDENTIAL_LEVEL) const {
        std::map<std::string, ResolvedValue> out;
        for (const auto& key : query(tags, credential_level))
            out.emplace(key, cfg_.resolve(key));
        return out;
    }

    // -- read ---------------------------------------------------------------

    std::vector<std::string> keys() const { return cfg_.keys(); }

    // Effective value honouring b ◁ a ◁ c; throws if unset and no
    // default supplied (no silent null).
    ConfigValue get(const std::string& key) const { return cfg_.resolve(key).value; }
    ConfigValue get(const std::string& key, const ConfigValue& dflt) const {
        try {
            return cfg_.resolve(key).value;
        } catch (const std::out_of_range&) {
            return dflt;
        }
    }

    ResolvedValue resolve(const std::string& key) const { return cfg_.resolve(key); }
    std::map<std::string, ResolvedValue> resolve_all() const {
        return cfg_.resolve_all();
    }

    // Read honouring the read-visibility tier: below the item's
    // read_level the item is treated as not listable, so a read is
    // refused the same way enumeration hides it. get stays the raw
    // tier-blind accessor; read is the tier-aware door.
    ConfigValue read(const std::string& key,
                     int credential_level = NO_CREDENTIAL_LEVEL) const {
        if (!visible(key, credential_level)) {
            const int needed = policy_of(key).read_level;
            throw WriteProtectionError(
                "config key '" + key + "' requires read level >= " +
                std::to_string(needed) + " to be listed or read; the request " +
                "presented level " + std::to_string(credential_level) +
                ". Permission-gated items are hidden below their tier; " +
                "re-issue with a sufficient level.");
        }
        return cfg_.resolve(key).value;
    }

    // -- write --------------------------------------------------------------
    //
    // Order of checks: build-toolchain key (category error, first), then
    // stdlib contract, then the write-protection gate. Only after all
    // three pass is the external map mutated — a rejected write never
    // leaves a partial state.
    void set(const std::string& key, const ConfigValue& value,
             int credential_level = NO_CREDENTIAL_LEVEL) {
        reject_if_build_config_key(key);
        validate_value(key, value);
        authorize_write(key, policy_of(key), credential_level);
        cfg_.external[key] = value;
    }

    // -- unified browse -----------------------------------------------------
    //
    // Built strictly on the tier-aware door (query_resolved -> query ->
    // policy_of); it never calls the tier-blind resolve_all/resolve/get,
    // so a permission-gated item cannot leak into a lower-level caller's
    // view. Rows are derived live on every call, so a key declared after
    // construction appears with no list to maintain.
    std::vector<BrowseEntry> browse(
        const std::vector<std::string>& tags = {},
        int credential_level = NO_CREDENTIAL_LEVEL) const {
        auto resolved = query_resolved(tags, credential_level);
        std::vector<BrowseEntry> rows;
        for (const auto& kv : resolved) {  // std::map is key-sorted
            const std::string& key = kv.first;
            const ResolvedValue& rv = kv.second;
            ItemPolicy policy = policy_of(key);
            // Default = the inlined (b) built-in when present; absent
            // otherwise. Read from the b map directly (not the tier-blind
            // resolve) so this stays a pure lookup that cannot widen
            // visibility.
            std::optional<ConfigValue> default_value;
            auto bit = cfg_.inlined.find(key);
            if (bit != cfg_.inlined.end()) default_value = bit->second;
            // Type from the contract that already governs every stored
            // value; prefer the effective value, fall back to default so
            // a row still types when both exist.
            std::string value_type;
            try {
                value_type = stdlib_type_of(rv.value);
            } catch (const UnbridgedValueError&) {
                if (default_value)
                    value_type = stdlib_type_of(*default_value);
                else
                    throw;
            }
            rows.push_back(BrowseEntry{
                key, value_type, default_value, rv.value, rv.layer,
                policy.impact, required_credential_level(policy),
                required_read_level(policy), policy.tags});
        }
        return rows;
    }

    // The external-layer map a bridge serialises after a write. Returned
    // by reference so the bridge sees the post-write image; the model
    // never touches files itself.
    std::map<std::string, ConfigValue>& pending_external() {
        return cfg_.external;
    }

private:
    bool visible(const std::string& key, int credential_level) const {
        return credential_level >= policy_of(key).read_level;
    }
    static bool is_superset(const std::set<std::string>& have,
                            const std::set<std::string>& want) {
        return std::includes(have.begin(), have.end(), want.begin(),
                             want.end());
    }

    LayeredConfig cfg_;
    std::map<std::string, ItemPolicy> policies_;
};

// --- Pure-internal (d) band: dev-phase registry, no runtime presence -------
//
// d is the innermost band. Unlike a/b/c it is *not* a runtime config
// value: after toolchain processing it is promoted to a plain host
// constant with zero configuration footprint, which is why Layer::D is
// excluded from runtime_merge_order and the runtime merge never sees it.
// Its tags exist for one purpose only — being discoverable while
// developing — so it gets its own registry, structurally disjoint from
// ConfigStore/LayeredConfig. Nothing on the runtime path holds a
// reference to this type; a runtime build can drop it entirely.

struct DevInternalItem {
    std::string name;
    ConfigValue value;
    std::set<std::string> tags;
};

class DevInternalRegistry {
public:
    // Record a pure-internal datum for dev-phase discovery and return
    // the plain value to be bound as a host constant. The value still
    // must satisfy the stdlib contract, but it is *not* stored as a
    // config item: the returned value is byte-equivalent to a
    // hand-written constant. The build-toolchain guard applies to the
    // name too, so d cannot smuggle a build key either.
    ConfigValue declare(const std::string& name, const ConfigValue& value,
                        const std::vector<std::string>& tags = {}) {
        reject_if_build_config_key(name);
        validate_value(name, value);
        items_[name] = DevInternalItem{
            name, value, std::set<std::string>(tags.begin(), tags.end())};
        return value;
    }

    std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (const auto& kv : items_) out.push_back(kv.first);
        return out;  // std::map => already sorted
    }

    const DevInternalItem& get(const std::string& name) const {
        return items_.at(name);
    }

    // d names whose tag set is a superset of tags (same freely-
    // combinable tag-AND as the runtime query) — the only retrieval d's
    // tags ever serve.
    std::vector<std::string> search(const std::vector<std::string>& tags) const {
        std::set<std::string> wanted(tags.begin(), tags.end());
        std::vector<std::string> out;
        for (const auto& kv : items_) {
            const auto& have = kv.second.tags;
            if (std::includes(have.begin(), have.end(), wanted.begin(),
                              wanted.end()))
                out.push_back(kv.first);
        }
        return out;  // std::map iteration => sorted
    }

private:
    std::map<std::string, DevInternalItem> items_;
};

}  // namespace topo::config

#endif  // TOPO_CONFIG_MODEL_H
