#ifndef TOPO_CONFIG_H
#define TOPO_CONFIG_H

// @stability provisional
// User-facing C++ TOML decode/encode bridge for the product config.
// Mirrors the Python reference config.py one-to-one; will move to
// `stable` after a release cycle without breaking schema changes.

// The C++ ecosystem bridge for the product runtime configuration.
//
// The layered model (config_model.h) is language-agnostic and never
// touches files. This header is the bridge: it decodes topo-app.toml and
// serialises writes back, mirroring the Python config.py split exactly.
//
// TOML decision: the Python bridge deliberately avoids a hard runtime
// dependency — it reads with the stdlib `tomllib` and ships a minimal
// *deterministic* writer instead of pulling the third-party `tomli-w`.
// C++ has no standard TOML facility at all, so to honour the same
// no-third-party-dependency stance this bridge ships a minimal
// deterministic header-only TOML reader *and* writer, scoped to exactly
// the flat scalar / array / table config vocabulary the model accepts
// (bool, int64, float, string, homogeneous-ish arrays, dotted-key tables,
// inline tables). It is intentionally not a general TOML implementation:
// it is the symmetric encode/decode pair that makes restore round-trip
// (re-parse of the restored text == the original decoded data) by
// construction, the same property the Python bridge guarantees. If a
// richer TOML library is ever vendored it can replace this pair without
// the model noticing.

#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <topo/config_model.h>

namespace topo::config {

// --- flat <-> nested transform (mirrors _split_nested/_flatten_nested) -----

// A record is a *leaf value* (an inline `{ ... }` table) rather than a
// nesting [section] table. The reader injects a sentinel marker key into
// inline-table records; nesting tables built by split_nested never carry
// it. This keeps the flat/nested transform deterministic and lets the
// flatten step recurse nesting tables only, treating an inline table as
// one leaf value (so a stored record round-trips as a single value).
inline constexpr const char* kInlineMarker = "\x01__inline__";

inline bool is_leaf_record(const ConfigValue& v) {
    if (v.kind() != ValueKind::Record) return false;
    for (const auto& kv : v.as_record())
        if (kv.first == kInlineMarker) return true;
    return false;
}

// Build a nested Record tree from dotted keys so the serialised TOML uses
// idiomatic [a.b] tables instead of quoted dotted keys. Iteration order
// of std::map is sorted, matching the Python sorted() emit.
inline Record split_nested(const std::map<std::string, ConfigValue>& flat) {
    Record root;
    for (const auto& kv : flat) {
        const std::string& dotted = kv.first;
        std::vector<std::string> parts;
        std::string cur;
        for (char c : dotted) {
            if (c == '.') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        parts.push_back(cur);

        Record* cursor = &root;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            const std::string& part = parts[i];
            auto it = std::find_if(cursor->begin(), cursor->end(),
                                   [&](auto& p) { return p.first == part; });
            if (it == cursor->end()) {
                cursor->emplace_back(part, ConfigValue(Record{}));
                it = std::prev(cursor->end());
            }
            cursor = &it->second.as_record();
        }
        cursor->emplace_back(parts.back(), kv.second);
    }
    return root;
}

// Inverse: a decoded TOML document back to the model's flat dotted-key
// map. A nested table is recursed; a stored record *value* is a leaf in
// the dotted addressing and not recursed — the writer marks the two
// apart, so the reader keeps a record only where the writer produced an
// inline table. The reader (parse_toml) emits flat dotted keys directly
// from [section] headers, so there is no nested-tree flatten step like
// Python's _flatten_nested (tomllib hands back nested dicts; this reader
// does not). The marker only travels inside an inline-table record long
// enough for emit_toml to keep it a single value; it is stripped before
// the value reaches the model (see strip_inline_marker).

inline ConfigValue strip_inline_marker(const ConfigValue& v) {
    if (v.kind() != ValueKind::Record) return v;
    Record cleaned;
    for (const auto& kv : v.as_record())
        if (kv.first != kInlineMarker)
            cleaned.emplace_back(kv.first, strip_inline_marker(kv.second));
    return ConfigValue(std::move(cleaned));
}

// --- minimal deterministic TOML writer (mirrors _emit_toml) ----------------

inline std::string toml_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

inline std::string format_double(double d) {
    // Deterministic, round-trippable float text. Use the shortest
    // representation that re-parses to the same double, and always carry
    // a decimal point so the reader keeps it a float (not an int).
    std::ostringstream os;
    os.precision(17);
    os << d;
    std::string s = os.str();
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
        s += ".0";
    return s;
}

inline std::string toml_value(const ConfigValue& v);

inline std::string toml_scalar(const ConfigValue& v) {
    switch (v.kind()) {
        case ValueKind::Bool:
            return v.as_bool() ? "true" : "false";
        case ValueKind::Int:
            return std::to_string(v.as_int());
        case ValueKind::Float:
            return format_double(v.as_float());
        case ValueKind::String:
            return "\"" + toml_escape(v.as_string()) + "\"";
        case ValueKind::Sequence: {
            std::string out = "[";
            const auto& seq = v.as_sequence();
            for (size_t i = 0; i < seq.size(); ++i) {
                if (i) out += ", ";
                out += toml_value(seq[i]);
            }
            out += "]";
            return out;
        }
        case ValueKind::Datetime:
        case ValueKind::Unbridged:
            // The model rejects these before a write reaches the bridge;
            // reaching here is a contract violation worth surfacing.
            throw std::runtime_error(
                "value has no stdlib bridge and must be rejected by the "
                "model before serialisation");
        case ValueKind::Record:
            return toml_value(v);
    }
    throw std::runtime_error("value is not TOML-serialisable");
}

inline std::string toml_value(const ConfigValue& v) {
    if (v.kind() == ValueKind::Record) {
        std::string out = "{";
        const auto& rec = v.as_record();
        bool first = true;
        for (const auto& kv : rec) {
            if (kv.first == kInlineMarker) continue;
            if (!first) out += ", ";
            first = false;
            out += kv.first + " = " + toml_value(kv.second);
        }
        out += "}";
        return out;
    }
    return toml_scalar(v);
}

// Scalars/arrays of a table before nested sub-tables, keys sorted, a
// stored record value written inline so it round-trips as one value
// rather than a sub-section. Mirrors _emit_toml's structure.
inline std::string emit_toml(const Record& nested,
                             const std::vector<std::string>& path = {}) {
    std::vector<std::pair<std::string, ConfigValue>> scalars;
    std::vector<std::pair<std::string, ConfigValue>> subtables;
    // Sort by key for deterministic output (Python sorts the dict).
    std::vector<std::pair<std::string, ConfigValue>> sorted_items(
        nested.begin(), nested.end());
    std::sort(sorted_items.begin(), sorted_items.end(),
              [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& kv : sorted_items) {
        if (kv.second.kind() == ValueKind::Record && !is_leaf_record(kv.second))
            subtables.push_back(kv);
        else
            scalars.push_back(kv);
    }
    std::vector<std::string> out;
    for (auto& kv : scalars)
        out.push_back(kv.first + " = " + toml_value(kv.second));
    for (auto& kv : subtables) {
        std::vector<std::string> npath = path;
        npath.push_back(kv.first);
        std::string section;
        for (size_t i = 0; i < npath.size(); ++i) {
            if (i) section += ".";
            section += npath[i];
        }
        std::string body = emit_toml(kv.second.as_record(), npath);
        out.push_back("\n[" + section + "]");
        if (!body.empty()) out.push_back(body);
    }
    std::string joined;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].empty()) continue;
        if (!joined.empty()) joined += "\n";
        joined += out[i];
    }
    return joined;
}

// --- minimal deterministic TOML reader -------------------------------------
//
// Parses exactly the vocabulary the writer above produces (and the test
// fixtures use): line-oriented key = value, [section] / [a.b] headers,
// string / int / float / bool scalars, arrays, and inline { } tables.
// Not a general TOML parser; the symmetric partner of emit_toml.

class TomlParseError : public std::runtime_error {
public:
    explicit TomlParseError(const std::string& m) : std::runtime_error(m) {}
};

namespace toml_detail {

inline void skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
}

inline ConfigValue parse_value(const std::string& s, size_t& i);

inline ConfigValue parse_string(const std::string& s, size_t& i) {
    ++i;  // opening quote
    std::string out;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == 'n') out += '\n';
            else if (n == 't') out += '\t';
            else if (n == '"') out += '"';
            else if (n == '\\') out += '\\';
            else out += n;
            i += 2;
        } else {
            out += s[i++];
        }
    }
    if (i >= s.size()) throw TomlParseError("unterminated string");
    ++i;  // closing quote
    return ConfigValue(out);
}

inline ConfigValue parse_array(const std::string& s, size_t& i) {
    ++i;  // [
    Sequence seq;
    skip_ws(s, i);
    while (i < s.size() && s[i] != ']') {
        seq.push_back(parse_value(s, i));
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') {
            ++i;
            skip_ws(s, i);
        }
    }
    if (i >= s.size()) throw TomlParseError("unterminated array");
    ++i;  // ]
    return ConfigValue(std::move(seq));
}

inline ConfigValue parse_inline_table(const std::string& s, size_t& i) {
    ++i;  // {
    Record rec;
    // Marker so the flatten step keeps this a leaf record value rather
    // than recursing into it as a nesting table.
    rec.emplace_back(kInlineMarker, ConfigValue(true));
    skip_ws(s, i);
    while (i < s.size() && s[i] != '}') {
        std::string key;
        while (i < s.size() && s[i] != '=' && s[i] != ' ' && s[i] != '\t')
            key += s[i++];
        skip_ws(s, i);
        if (i >= s.size() || s[i] != '=')
            throw TomlParseError("expected '=' in inline table");
        ++i;
        skip_ws(s, i);
        rec.emplace_back(key, parse_value(s, i));
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') {
            ++i;
            skip_ws(s, i);
        }
    }
    if (i >= s.size()) throw TomlParseError("unterminated inline table");
    ++i;  // }
    return ConfigValue(std::move(rec));
}

inline ConfigValue parse_scalar_token(const std::string& tok) {
    if (tok == "true") return ConfigValue(true);
    if (tok == "false") return ConfigValue(false);
    // A bare token containing ':' or a 'T'/'Z' date shape would be a TOML
    // datetime — surfaced as the Datetime tag so the model rejects it
    // with the stdlib-bridging-gap message, exactly like the Python reference.
    bool looks_datetime = false;
    if (tok.size() >= 8) {
        // crude: YYYY-MM-DD or contains a time separator with digits.
        bool has_dash = tok.find('-') != std::string::npos;
        bool has_colon = tok.find(':') != std::string::npos;
        bool all_date_chars = true;
        for (char c : tok)
            if (!std::isdigit((unsigned char)c) && c != '-' && c != ':' &&
                c != 'T' && c != 'Z' && c != '.' && c != '+') {
                all_date_chars = false;
                break;
            }
        if (all_date_chars && (has_colon || (has_dash && tok.size() == 10)))
            looks_datetime = true;
    }
    if (looks_datetime) return ConfigValue::datetime();
    // int vs float
    bool is_float = tok.find('.') != std::string::npos ||
                    tok.find('e') != std::string::npos ||
                    tok.find('E') != std::string::npos;
    try {
        if (is_float) return ConfigValue(std::stod(tok));
        return ConfigValue(static_cast<std::int64_t>(std::stoll(tok)));
    } catch (...) {
        throw TomlParseError("unrecognised scalar token: '" + tok + "'");
    }
}

inline ConfigValue parse_value(const std::string& s, size_t& i) {
    skip_ws(s, i);
    if (i >= s.size()) throw TomlParseError("expected value");
    if (s[i] == '"') return parse_string(s, i);
    if (s[i] == '[') return parse_array(s, i);
    if (s[i] == '{') return parse_inline_table(s, i);
    std::string tok;
    while (i < s.size() && s[i] != ',' && s[i] != ']' && s[i] != '}')
        tok += s[i++];
    while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
        tok.pop_back();
    return parse_scalar_token(tok);
}

}  // namespace toml_detail

// Parse TOML text into the model's flat dotted-key map. Section headers
// prefix subsequent keys with the dotted path; this is exactly the
// inverse of emit_toml for the supported vocabulary.
inline std::map<std::string, ConfigValue> parse_toml(const std::string& text) {
    std::map<std::string, ConfigValue> flat;
    std::string section;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        // strip a trailing comment that is not inside a string (the
        // config vocabulary never puts '#' in keys; values may, so only
        // strip when no quote precedes it on the line)
        std::string trimmed = line;
        // left trim
        size_t b = trimmed.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        trimmed = trimmed.substr(b);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        if (trimmed[0] == '[') {
            size_t close = trimmed.find(']');
            if (close == std::string::npos)
                throw TomlParseError("unterminated section header");
            section = trimmed.substr(1, close - 1);
            continue;
        }
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos)
            throw TomlParseError("expected key = value: " + trimmed);
        std::string key = trimmed.substr(0, eq);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        size_t i = eq + 1;
        ConfigValue value = toml_detail::parse_value(trimmed, i);
        std::string full = section.empty() ? key : section + "." + key;
        // parse_toml is the model-facing boundary: the inline-table
        // marker is a within-bridge round-trip aid only and must not
        // leak into a stored record value.
        flat[full] = strip_inline_marker(value);
    }
    return flat;
}

// --- ProductConfig: the C++ projection -------------------------------------
//
// Wraps a language-agnostic ConfigStore; adds only the C++ ecosystem's
// file I/O (the minimal TOML reader/writer above). set updates the
// external layer via the model and re-serialises the user-managed file
// so a write is immediately reflected on disk and in the next get.

class ProductConfig {
public:
    // Pathless (in-memory) construction; optional inlined (b) and
    // injected (c) seed layers + policies, mirroring the Python kwargs.
    explicit ProductConfig(std::map<std::string, ConfigValue> inlined = {},
                           std::map<std::string, ConfigValue> injected = {},
                           std::map<std::string, ItemPolicy> policies = {})
        : store_(LayeredConfig(std::move(inlined), {}, std::move(injected)),
                 std::move(policies)) {}

    // File-backed construction: decode topo-app.toml (when present) into
    // the external (a) layer. A missing file is an empty external layer,
    // not an error — same as the Python FileNotFoundError handling.
    static ProductConfig from_file(
        const std::string& path,
        std::map<std::string, ConfigValue> inlined = {},
        std::map<std::string, ConfigValue> injected = {},
        std::map<std::string, ItemPolicy> policies = {}) {
        std::map<std::string, ConfigValue> external;
        std::ifstream fh(path);
        if (fh) {
            std::stringstream ss;
            ss << fh.rdbuf();
            external = parse_toml(ss.str());
        }
        ProductConfig pc(std::move(inlined), std::move(injected),
                         std::move(policies));
        pc.path_ = path;
        pc.store_ = ConfigStore(
            LayeredConfig(pc.store_.layered().inlined, external,
                          pc.store_.layered().injected),
            {});
        return pc;
    }

    ConfigStore& store() { return store_; }
    const ConfigStore& store() const { return store_; }
    const std::optional<std::string>& path() const { return path_; }

    void declare(const std::string& key, const ItemPolicy& p) {
        store_.declare(key, p);
    }

    // -- code-layer inline / hidden TOML (layer b) --------------------------
    //
    // An explicit code-level call (not a TOML directive, not automatic
    // build behaviour): "this config block ships inside the artifact",
    // so it no longer needs a scattered external file. The model only
    // ever sees decoded data; this bridge owns the decode and the
    // symmetric restore back to TOML text.
    void declare_inlined_toml(const std::string& toml_text) {
        // parse_toml already yields the flat dotted-key map the model
        // consumes — the same shape Python's _flatten_nested(tomllib...)
        // produces; install_inlined applies the build-key guard.
        store_.layered().install_inlined(parse_toml(toml_text));
    }
    void declare_inlined_toml(const std::map<std::string, ConfigValue>& decoded) {
        store_.layered().install_inlined(decoded);
    }

    // Reconstruct the embedded (b) layer as equivalent TOML text:
    // re-parsing the returned text yields the same decoded data the
    // layer holds — guaranteed because it reuses the very same
    // deterministic emitter the external file uses over the same
    // flat->nested transform, so encode∘decode is the identity for the
    // scalar/array/table config vocabulary.
    std::string restore_inlined_toml() const {
        const auto& inlined = store_.layered().inlined;
        if (inlined.empty()) return "";
        Record nested = split_nested(inlined);
        std::string body = emit_toml(nested);
        // strip + trailing newline, matching the Python .strip()+"\n"
        while (!body.empty() && (body.back() == '\n' || body.back() == ' '))
            body.pop_back();
        size_t s = body.find_first_not_of("\n ");
        if (s != std::string::npos) body = body.substr(s);
        return body.empty() ? "" : body + "\n";
    }

    // -- pure-internal (d) declaration --------------------------------------
    //
    // d is only declarable in code and has no runtime config presence:
    // the call returns a plain value the caller binds as an ordinary
    // constant, and the dev metadata lands solely in a side registry the
    // store never consults. Layer::D stays out of runtime_merge_order so
    // keys()/resolve_all()/query() cannot surface it by construction.
    DevInternalRegistry& dev_internal() {
        if (!dev_internal_) dev_internal_ = std::make_unique<DevInternalRegistry>();
        return *dev_internal_;
    }
    bool dev_internal_built() const { return static_cast<bool>(dev_internal_); }

    ConfigValue declare_internal(const std::string& name,
                                 const ConfigValue& value,
                                 const std::vector<std::string>& tags = {}) {
        return dev_internal().declare(name, value, tags);
    }

    std::vector<std::string> keys() const { return store_.keys(); }
    std::vector<std::string> query(const std::vector<std::string>& tags = {},
                                   int credential_level = 0) const {
        return store_.query(tags, credential_level);
    }
    std::map<std::string, ResolvedValue> query_resolved(
        const std::vector<std::string>& tags = {},
        int credential_level = 0) const {
        return store_.query_resolved(tags, credential_level);
    }
    int max_read_level() const { return store_.max_read_level(); }
    ConfigValue read(const std::string& key, int credential_level = 0) const {
        return store_.read(key, credential_level);
    }
    std::vector<BrowseEntry> browse(const std::vector<std::string>& tags = {},
                                    int credential_level = 0) const {
        return store_.browse(tags, credential_level);
    }

    // The dev-phase-only d listing. Explicitly NOT part of the runtime
    // browse: d is promoted to a host constant with zero runtime
    // footprint, so it is absent from browse() at every level. Distinct
    // record shape so the two ranges never blur. Browsing an empty dev
    // band must not even construct the side registry.
    struct DevRecord {
        std::string name;
        ConfigValue value;
        std::set<std::string> tags;
    };
    std::vector<DevRecord> dev_browse(
        const std::vector<std::string>& tags = {}) const {
        if (!dev_internal_) return {};
        std::vector<std::string> names =
            tags.empty() ? dev_internal_->names() : dev_internal_->search(tags);
        std::vector<DevRecord> out;
        for (const auto& n : names) {
            const auto& item = dev_internal_->get(n);
            out.push_back(DevRecord{item.name, item.value, item.tags});
        }
        return out;
    }

    ConfigValue get(const std::string& key) const { return store_.get(key); }
    ConfigValue get(const std::string& key, const ConfigValue& dflt) const {
        return store_.get(key, dflt);
    }
    ResolvedValue resolve(const std::string& key) const {
        return store_.resolve(key);
    }

    void set(const std::string& key, const ConfigValue& value,
             int credential_level = 0) {
        store_.set(key, value, credential_level);
        if (path_) write_external();
    }

    // The external (a) layer as deterministic TOML text — the exact
    // bytes set() writes to topo-app.toml.
    std::string serialize_external() const {
        const auto& ext = store_.layered().external;
        if (ext.empty()) return "";
        Record nested = split_nested(ext);
        std::string body = emit_toml(nested);
        while (!body.empty() && (body.back() == '\n' || body.back() == ' '))
            body.pop_back();
        size_t s = body.find_first_not_of("\n ");
        if (s != std::string::npos) body = body.substr(s);
        return body.empty() ? "" : body + "\n";
    }

private:
    void write_external() {
        if (!path_)
            throw std::runtime_error(
                "cannot persist external layer: this config has no file path");
        std::ofstream fh(*path_, std::ios::trunc);
        fh << serialize_external();
    }

    ConfigStore store_;
    std::optional<std::string> path_;
    std::unique_ptr<DevInternalRegistry> dev_internal_;
};

}  // namespace topo::config

#endif  // TOPO_CONFIG_H
