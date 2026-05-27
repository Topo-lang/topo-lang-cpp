# Topo native-lldb formatter (zero-install).
#
# A user who runs plain `lldb a.out` (no Topo process, no `topo debug`) and
# inspects a Topo-optimized variable would otherwise see the raw post-transform
# layout — e.g. a `Mesh` that DataLayoutPass turned SoA shows up as a pile of
# split column arrays / bare `(span<float>) ...`. This script registers an lldb
# type-summary provider that reads the project's `*.topo-dbg.json` reverse
# mapping (emitted by `topo build` next to the binary, exactly as
# topo-core/lib/Debug/Emitter.cpp / the topo-debug CLI name it) and instead
# prints the *logical declared view*: the `summary` template the user wrote in
# the `.topo` source, with `{ <query> }` placeholders evaluated against the
# stopped frame.
#
# Loaded by the project's `.lldbinit` via:
#     command script import <path>/lldb_formatter.py
#
# Degrades gracefully: if `*.topo-dbg.json` is absent, malformed, has no
# matching symbol, or any evaluation step fails, the provider returns None and
# lldb falls back to its default formatting. No exception is allowed to escape
# into the debugger session.
#
# C++ host: lldb's DWARF names map directly to the .topo `topo_name` /
# `host_symbol` (host_symbol defaults to topo_name unless a backend
# overrides it). Supported leaf dtypes mirror the topo-debug-cpp adapter:
# i8/i16/i32/i64, u8/u16/u32/u64, f32/f64, and N-d arrays of those.

import json
import os

try:
    import lldb  # provided by the lldb that imports this script
except ImportError:  # running under a plain interpreter (unit path)
    lldb = None

HOST_LANGUAGE = "cpp"

# ---------------------------------------------------------------------------
# *.topo-dbg.json discovery + parse
# ---------------------------------------------------------------------------


def dbg_json_path_for_binary(binary_path):
    """Mirror Emitter.cpp / topo-debug CLI naming: `<binary>.topo-dbg.json`
    sits next to the executable. `topo build` writes
    `<output>.topo-dbg.json`; `topo debug` auto-discovers
    `<target>.topo-dbg.json`. We do the same so the zero-install path needs
    no extra flags."""
    if not binary_path:
        return None
    return binary_path + ".topo-dbg.json"


def load_dbg_meta(binary_path):
    """Return the parsed dbg-meta dict, or None on any failure (missing file,
    unreadable, malformed JSON, wrong shape). Never raises."""
    path = dbg_json_path_for_binary(binary_path)
    if not path or not os.path.isfile(path):
        return None
    try:
        with open(path, "r") as fh:
            doc = json.load(fh)
    except (OSError, ValueError):
        return None
    if not isinstance(doc, dict):
        return None
    if not isinstance(doc.get("symbols"), list):
        return None
    return doc


def find_symbol_entry(doc, type_name):
    """Match an lldb type name to a `symbols[]` entry. By default
    `host_symbol == topo_name`; a backend may later set host_symbol to the
    mangled/qualified host name, so we accept either. We also accept a
    trailing-component match (`geom::Mesh` vs `Mesh`) because lldb may report
    either the qualified or the unqualified name depending on build flags."""
    if not type_name:
        return None
    tail = type_name.split("::")[-1].strip()
    for sym in doc.get("symbols", []):
        if not isinstance(sym, dict):
            continue
        topo_name = sym.get("topo_name")
        host_symbol = sym.get("host_symbol")
        for cand in (topo_name, host_symbol):
            if not cand:
                continue
            if cand == type_name or cand == tail:
                return sym
            if cand.split("::")[-1] == tail:
                return sym
    return None


def build_view_registry(sym):
    """views[]: name -> (container, is_sliced, start, end). Mirrors
    loadViewRegistry() in topo-core/tools/topo-debug/main.cpp."""
    registry = {}
    for v in sym.get("views", []) or []:
        if not isinstance(v, dict):
            continue
        name = v.get("name")
        expr = v.get("expr")
        if not name or not isinstance(expr, dict):
            continue
        container = expr.get("container")
        if not container:
            continue
        kind = expr.get("kind", "field")
        if kind == "slice":
            start = expr.get("start")
            end = expr.get("end")
            if not isinstance(start, int) or not isinstance(end, int):
                continue
            registry[name] = (container, True, start, end)
        else:
            registry[name] = (container, False, 0, 0)
    return registry


# ---------------------------------------------------------------------------
# Summary template tokeniser — mirrors topo-core/lib/Debug/SummaryRenderer.cpp
# grammar: `{expr}` placeholders; `{{` / `}}` escape literal braces.
# ---------------------------------------------------------------------------


def parse_template(tmpl):
    """Return a list of (is_placeholder, text) segments, or None on a malformed
    template (so the caller can degrade to default formatting)."""
    segments = []
    lit = []
    i = 0
    n = len(tmpl)
    while i < n:
        c = tmpl[i]
        if c == "{":
            if i + 1 < n and tmpl[i + 1] == "{":
                lit.append("{")
                i += 2
                continue
            if lit:
                segments.append((False, "".join(lit)))
                lit = []
            j = i + 1
            depth = 1
            while j < n and depth > 0:
                if tmpl[j] == "{":
                    return None
                if tmpl[j] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            if depth != 0:
                return None
            expr = tmpl[i + 1:j].strip()
            if not expr:
                return None
            segments.append((True, expr))
            i = j + 1
        elif c == "}":
            if i + 1 < n and tmpl[i + 1] == "}":
                lit.append("}")
                i += 2
                continue
            return None
        else:
            lit.append(c)
            i += 1
    if lit:
        segments.append((False, "".join(lit)))
    return segments


# ---------------------------------------------------------------------------
# Minimal query-expression evaluator over the stopped frame.
#
# Path 1 has no Topo process, so the formatter resolves the `.topo` summary
# placeholders itself. Supported (mirroring the topo-debug query builtins that
# the adapter handles for the scalar/array dtype set):
#   sum(x) count(x) min(x) max(x) mean(x) shape(x) dtype(x) sample(x)
# where x is a host array variable or a declared view name; plus + - * /
# binary arithmetic between two such terms. Anything outside this grammar
# makes the whole summary degrade to default formatting (return None).
# ---------------------------------------------------------------------------

_INT_DTYPES = {
    "char": "i8", "signed char": "i8", "unsigned char": "u8",
    "short": "i16", "unsigned short": "u16",
    "int": "i32", "unsigned int": "u32", "unsigned": "u32",
    "long": "i64", "unsigned long": "u64",
    "long long": "i64", "unsigned long long": "u64",
    "int8_t": "i8", "uint8_t": "u8", "int16_t": "i16", "uint16_t": "u16",
    "int32_t": "i32", "uint32_t": "u32", "int64_t": "i64", "uint64_t": "u64",
    # Rust spellings accepted too so the shared core stays language-neutral.
    "i8": "i8", "i16": "i16", "i32": "i32", "i64": "i64",
    "u8": "u8", "u16": "u16", "u32": "u32", "u64": "u64",
}
_FLOAT_DTYPES = {"float": "f32", "double": "f64", "f32": "f32", "f64": "f64"}


def _leaf_type_name(sbtype):
    name = sbtype.GetName() or ""
    return name.strip()


def _array_elements(sbvalue):
    """Flatten an SBValue array (N-d) into a flat python list of numeric leaves
    plus the per-dimension shape. Returns (values, shape, dtype) or None."""
    shape = []
    cur = sbvalue
    # Drill through nested array types collecting dimensions.
    while cur.GetType().IsArrayType():
        n = cur.GetNumChildren()
        shape.append(n)
        if n == 0:
            return [], shape, None
        cur = cur.GetChildAtIndex(0)

    def collect(v, dtype_box):
        t = v.GetType()
        if t.IsArrayType():
            out = []
            for k in range(v.GetNumChildren()):
                out.extend(collect(v.GetChildAtIndex(k), dtype_box))
            return out
        tname = _leaf_type_name(t)
        if tname in _FLOAT_DTYPES:
            dtype_box[0] = _FLOAT_DTYPES[tname]
            return [float(v.GetValue() or 0.0)]
        if tname in _INT_DTYPES:
            dtype_box[0] = _INT_DTYPES[tname]
            return [int(v.GetValueAsSigned())]
        # Unsupported leaf (struct / pointer / etc.) — bail.
        raise ValueError("unsupported leaf type: %s" % tname)

    dtype_box = [None]
    try:
        values = collect(sbvalue, dtype_box)
    except ValueError:
        return None
    return values, shape, dtype_box[0]


def _slice_values(values, start, end):
    if start < 0 or end < start or end > len(values):
        return None
    return values[start:end]


def _resolve_term(term, lookup, views):
    """Resolve `fn(var_or_view)` to a python scalar/string. `lookup(name)`
    returns an SBValue for a container name (a struct field of the bound
    value, or a frame variable). Returns (kind, value) where kind is 'num'
    or 'str', or None on any failure."""
    term = term.strip()
    if "(" not in term or not term.endswith(")"):
        return None
    fn = term[: term.index("(")].strip()
    arg = term[term.index("(") + 1: -1].strip()
    if not arg:
        return None

    # Resolve arg: a declared view name expands to container[start:end];
    # otherwise it's a container (struct field / host variable) name.
    if arg in views:
        container, sliced, start, end = views[arg]
        var = lookup(container)
        if var is None or not var.IsValid():
            return None
        flat = _array_elements(var)
        if flat is None:
            return None
        values, shape, dtype = flat
        if sliced:
            values = _slice_values(values, start, end)
            if values is None:
                return None
            shape = [len(values)]
    else:
        var = lookup(arg)
        if var is None or not var.IsValid():
            return None
        flat = _array_elements(var)
        if flat is None:
            return None
        values, shape, dtype = flat

    if fn == "sum":
        return ("num", sum(values))
    if fn == "count":
        return ("num", len(values))
    if fn == "min":
        return ("num", min(values)) if values else None
    if fn == "max":
        return ("num", max(values)) if values else None
    if fn == "mean":
        return ("num", (sum(values) / len(values))) if values else None
    if fn == "shape":
        return ("str", "[" + ", ".join(str(d) for d in shape) + "]")
    if fn == "dtype":
        return ("str", dtype or "?")
    if fn == "sample":
        head = values[:8]
        return ("str", ", ".join(_fmt_num(x) for x in head))
    return None


def _fmt_num(x):
    if isinstance(x, float):
        if x == int(x):
            return str(int(x))
        return repr(x)
    return str(x)


def _make_lookup(valobj, frame):
    """Build a container-name → SBValue resolver. A declared view's container
    is resolved first as a child (struct field) of the bound value — the
    realistic SoA case where `Mesh` is the variable and its column arrays are
    fields — then as a frame variable (the project_simple case where the
    array is a top-level local). Returns None for an unknown name."""
    def lookup(name):
        if valobj is not None:
            try:
                child = valobj.GetChildMemberWithName(name)
                if child is not None and child.IsValid():
                    return child
            except Exception:  # noqa: BLE001
                pass
        if frame is not None:
            try:
                v = frame.FindVariable(name)
                if v is not None and v.IsValid():
                    return v
            except Exception:  # noqa: BLE001
                pass
        return None
    return lookup


def _eval_placeholder(expr, lookup, views):
    """Evaluate one `{...}` expression. Supports a single term or
    `term <op> term` with op in + - * /. Returns the formatted string, or
    None to degrade."""
    for op in ("+", "-", "*", "/"):
        # Only split on a top-level operator (no parens on either side of it).
        depth = 0
        for idx, ch in enumerate(expr):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif ch == op and depth == 0 and idx > 0:
                lhs = _resolve_term(expr[:idx], lookup, views)
                rhs = _resolve_term(expr[idx + 1:], lookup, views)
                if (lhs is None or rhs is None
                        or lhs[0] != "num" or rhs[0] != "num"):
                    return None
                a, b = lhs[1], rhs[1]
                if op == "+":
                    return _fmt_num(a + b)
                if op == "-":
                    return _fmt_num(a - b)
                if op == "*":
                    return _fmt_num(a * b)
                if op == "/":
                    if b == 0:
                        return None
                    return _fmt_num(a / b)
    single = _resolve_term(expr, lookup, views)
    if single is None:
        return None
    return single[1] if single[0] == "str" else _fmt_num(single[1])


def render_summary(sym, valobj, frame):
    """Render the symbol's `summary_template`. Container names resolve against
    the bound value's fields first, then the frame's locals. Returns the
    logical declared-view string, or None to degrade to default formatting."""
    tmpl = sym.get("summary_template")
    if not isinstance(tmpl, str) or not tmpl:
        return None
    segments = parse_template(tmpl)
    if segments is None:
        return None
    views = build_view_registry(sym)
    lookup = _make_lookup(valobj, frame)
    out = []
    for is_ph, text in segments:
        if not is_ph:
            out.append(text)
            continue
        val = _eval_placeholder(text, lookup, views)
        if val is None:
            return None
        out.append(val)
    return "".join(out)


# ---------------------------------------------------------------------------
# lldb type-summary provider entry point
# ---------------------------------------------------------------------------


def _binary_path_for_target(target):
    if not target or not target.IsValid():
        return None
    exe = target.GetExecutable()
    if not exe or not exe.IsValid():
        return None
    d = exe.GetDirectory() or ""
    f = exe.GetFilename() or ""
    if not f:
        return None
    return os.path.join(d, f) if d else f


def topo_summary(valobj, internal_dict):
    """lldb summary callback. Must never raise. Returning None tells lldb to
    fall back to its own default rendering (graceful degrade); a string is
    shown as the logical declared view."""
    try:
        binary_path = _binary_path_for_target(valobj.GetTarget())
        doc = load_dbg_meta(binary_path)
        if doc is None:
            return None
        sbtype = valobj.GetType()
        type_name = (sbtype.GetName() or "").strip()
        sym = find_symbol_entry(doc, type_name)
        if sym is None:
            return None
        frame = valobj.GetFrame()
        rendered = render_summary(sym, valobj, frame)
        if rendered is None:
            return None
        return rendered
    except Exception:  # noqa: BLE001 — never crash the debugger session
        return None


def _registered_type_names(doc):
    names = set()
    for sym in doc.get("symbols", []):
        if not isinstance(sym, dict):
            continue
        for key in ("topo_name", "host_symbol"):
            v = sym.get(key)
            if v:
                names.add(v)
                names.add(v.split("::")[-1])
    return sorted(n for n in names if n)


def _register_for_doc(debugger, doc):
    """Register the name-keyed `topo_summary` for every declared type. lldb's
    summary callback returns None for non-Topo values, so even a broad name
    match degrades cleanly."""
    mod = __name__
    count = 0
    for name in _registered_type_names(doc):
        debugger.HandleCommand(
            'type summary add "%s" --python-function %s.topo_summary '
            "--category topo" % (name, mod)
        )
        count += 1
    return count


def topo_formatter_refresh(debugger, command, result, internal_dict):
    """`topo-formatter-refresh` — re-scan the selected target's
    `*.topo-dbg.json` and (re)register summaries. Useful if the target is
    created/changed after this script was imported. Never raises."""
    try:
        target = debugger.GetSelectedTarget()
        binary_path = _binary_path_for_target(target)
        doc = load_dbg_meta(binary_path)
        if doc is None:
            result.AppendMessage(
                "topo: no readable *.topo-dbg.json next to the target; "
                "default lldb formatting is in effect."
            )
            return
        n = _register_for_doc(debugger, doc)
        debugger.HandleCommand("type category enable topo")
        result.AppendMessage("topo: registered %d declared type(s)." % n)
    except Exception as exc:  # noqa: BLE001
        result.AppendMessage("topo: formatter refresh skipped (%s)" % exc)


def __lldb_init_module(debugger, internal_dict):
    """Called by lldb when this script is imported (via `.lldbinit`'s
    `command script import`). When `lldb a.out` is run, the target is created
    from argv *before* the local `.lldbinit` is sourced, so the selected
    target — and thus the sibling `*.topo-dbg.json` — is already resolvable
    here. Absence of the artifact is a clean no-op (default formatting)."""
    try:
        debugger.HandleCommand(
            "command script add -f %s.topo_formatter_refresh "
            "topo-formatter-refresh" % __name__
        )
    except Exception:  # noqa: BLE001
        pass
    try:
        target = debugger.GetSelectedTarget()
        binary_path = _binary_path_for_target(target)
        doc = load_dbg_meta(binary_path)
        if doc is not None:
            _register_for_doc(debugger, doc)
            debugger.HandleCommand("type category enable topo")
    except Exception:  # noqa: BLE001 — never break script import
        pass
