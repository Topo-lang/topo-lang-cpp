#ifndef TOPO_APP_CONFIG_H
#define TOPO_APP_CONFIG_H

// @stability provisional
// User-facing config bridge for topo-app. Tracks the Python reference
// config.py one-to-one; will settle once the topo-app config grid
// is fully delivered.

// The single unified configuration entry: topo::app::config(app).
//
// snapshot() and emit_topo() are two views of the *same* logic
// structure: the snapshot is the human/agent overview, the .topo is the
// toolchain-consumable contract. They are kept consistent by
// construction because both derive from the same Graph — the
// snapshot and the emitted .topo are two views of the same
// logic structure. One-to-one
// with the Python reference's config.py Config.

#include <optional>
#include <string>
#include <vector>

#include <topo/app.h>
#include <topo/app_emit.h>
#include <topo/app_readback.h>

namespace topo::app {

// A plain, structured snapshot of the whole graph. Strings rather than a
// dynamic value tree keep the bridge minimal while still surfacing every
// handler's In/Out and every connection in one place.
struct Snapshot {
    struct HandlerView {
        std::string name;
        std::optional<std::string> in;  // empty == source handler (void)
        std::string out;
    };
    struct EdgeView {
        std::string from;
        std::string to;  // "void" for a terminal edge
    };
    struct FlowView {
        std::string name;
        std::vector<EdgeView> edges;
    };

    std::string namespace_name;
    std::vector<HandlerView> handlers;
    std::optional<FlowView> flow;
};

class Config {
public:
    explicit Config(const App& app) : app_(app) {}

    const Graph& graph() const { return app_.graph(); }

    Snapshot snapshot() const {
        const Graph& g = app_.graph();
        Snapshot s;
        s.namespace_name = g.namespace_name();
        for (const auto& h : g.handlers()) {
            Snapshot::HandlerView hv;
            hv.name = h.name;
            if (h.in_type.has_value()) hv.in = h.in_type->topo();
            hv.out = h.out_type.topo();
            s.handlers.push_back(std::move(hv));
        }
        if (g.has_flow()) {
            Snapshot::FlowView fv;
            fv.name = g.flow().name;
            for (const auto& e : g.flow().edges)
                fv.edges.push_back(
                    {e.source, e.is_terminal() ? "void" : *e.target});
            s.flow = std::move(fv);
        }
        return s;
    }

    // The round-trippable .topo view of the same structure.
    std::string emit_topo(const std::optional<std::string>& path =
                              std::nullopt) const {
        std::string text = topo::app::emit_topo(app_.graph());
        if (path.has_value()) {
            std::ofstream f(*path);
            f << text;
        }
        return text;
    }

    // Emit then read back through the real parser. Returns graph'.
    Graph roundtrip() const { return read_topo(emit_topo()); }

private:
    const App& app_;
};

// The one topo::app::config(app) entry topo-app names.
inline Config config(const App& app) { return Config(app); }

}  // namespace topo::app

#endif  // TOPO_APP_CONFIG_H
