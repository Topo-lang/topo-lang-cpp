#!/usr/bin/env python3
# Non-lldb unit coverage for the native-lldb formatter.
#
# The bundled lldb (topo-llvm/llvm-dev) is built WITHOUT a Python script
# interpreter (bundled lldb has no Python scripting support),
# so the in-debugger e2e cannot run in this environment. This test exercises
# the formatter's pure-Python core directly with stub SB* objects so the
# logic — `*.topo-dbg.json` discovery, symbol matching, summary-template
# rendering, and graceful degradation — stays regression-covered regardless
# of whether a scripting-enabled lldb is present.
#
# Usage:  python3 formatter_unit_test.py <formatter.py> <dbg.json>
# Exit 0 = all pass; non-zero = failure (CTest treats it as a hard fail).

import importlib.util
import json
import os
import sys
import tempfile
import unittest


def _load_formatter(path):
    spec = importlib.util.spec_from_file_location("topo_lldb_formatter", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# --- Stub SB* objects -------------------------------------------------------
# Minimal shims matching the slice of the lldb SBValue/SBType API the
# formatter touches: GetType/IsArrayType/GetName, GetNumChildren/
# GetChildAtIndex, GetValue(AsSigned), GetChildMemberWithName, IsValid.

class StubType:
    def __init__(self, name, is_array=False):
        self._name = name
        self._array = is_array

    def GetName(self):
        return self._name

    def IsArrayType(self):
        return self._array


class StubValue:
    def __init__(self, sbtype, value=None, children=None, fields=None):
        self._t = sbtype
        self._v = value
        self._k = children or []
        self._f = fields or {}

    def IsValid(self):
        return self._t.GetName() != "<invalid>"

    def GetType(self):
        return self._t

    def GetValue(self):
        return self._v

    def GetValueAsSigned(self):
        return int(self._v)

    def GetNumChildren(self):
        return len(self._k)

    def GetChildAtIndex(self, i):
        return self._k[i]

    def GetChildMemberWithName(self, name):
        return self._f.get(name, StubValue(StubType("<invalid>")))


def _int_array(name, values):
    return StubValue(
        StubType("%s[%d]" % (name, len(values)), is_array=True),
        children=[StubValue(StubType("int"), v) for v in values],
    )


class FormatterCoreTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.formatter_path = sys.argv[1]
        cls.dbg_path = sys.argv[2]
        cls.F = _load_formatter(cls.formatter_path)
        with open(cls.dbg_path) as fh:
            cls.doc = json.load(fh)
        cls.sym = cls.doc["symbols"][0]

    # -- dbg.json discovery mirrors Emitter.cpp naming --------------------

    def test_dbg_path_naming(self):
        self.assertEqual(
            self.F.dbg_json_path_for_binary("/x/main"),
            "/x/main.topo-dbg.json",
        )
        self.assertIsNone(self.F.dbg_json_path_for_binary(None))

    def test_load_real_meta(self):
        # load_dbg_meta resolves <binary>.topo-dbg.json — point it at the
        # fixture by stripping the suffix.
        binary = self.dbg_path[: -len(".topo-dbg.json")]
        doc = self.F.load_dbg_meta(binary)
        self.assertIsNotNone(doc)
        self.assertIsInstance(doc.get("symbols"), list)

    # -- symbol matching: topo_name AND qualified host_symbol ------------

    def test_symbol_match_both_names(self):
        topo_name = self.sym["topo_name"]
        host_symbol = self.sym["host_symbol"]
        self.assertIsNotNone(self.F.find_symbol_entry(self.doc, topo_name))
        self.assertIsNotNone(self.F.find_symbol_entry(self.doc, host_symbol))
        # trailing-component match (qualified -> unqualified)
        self.assertIsNotNone(
            self.F.find_symbol_entry(self.doc, host_symbol.split("::")[-1])
        )
        self.assertIsNone(self.F.find_symbol_entry(self.doc, "NotAType"))

    # -- the logical declared view (the whole point of Path 1) ----------

    def test_render_logical_summary(self):
        # Build a struct value whose split column fields are exactly what a
        # SoA transform leaves behind; the formatter must collapse them to
        # the declared summary string, NOT echo the raw arrays.
        verts = _int_array("verts", list(range(1, 7)))      # 6 elements
        tris = _int_array("tris", [(i + 1) * 10 for i in range(9)])  # 9
        mesh = StubValue(
            StubType(self.sym["topo_name"]),
            fields={"verts": verts, "tris": tris},
        )
        rendered = self.F.render_summary(self.sym, mesh, None)
        self.assertEqual(rendered, "<Mesh: 6 verts, 9 tris>")

    def test_query_builtins(self):
        views = self.F.build_view_registry(self.sym)
        verts = _int_array("verts", [1, 2, 3, 4, 5, 6])
        tris = _int_array("tris", [10] * 9)
        mesh = StubValue(
            StubType("Mesh"), fields={"verts": verts, "tris": tris}
        )
        lookup = self.F._make_lookup(mesh, None)
        # sum/min/max/mean over the declared view
        self.assertEqual(
            self.F._resolve_term("sum(verts_view)", lookup, views), ("num", 21)
        )
        self.assertEqual(
            self.F._resolve_term("count(tris_view)", lookup, views), ("num", 9)
        )
        self.assertEqual(
            self.F._resolve_term("max(verts_view)", lookup, views), ("num", 6)
        )
        self.assertEqual(
            self.F._resolve_term("min(verts_view)", lookup, views), ("num", 1)
        )

    # -- graceful degradation: NEVER raise, return None ------------------

    def test_missing_dbg_json(self):
        self.assertIsNone(self.F.load_dbg_meta("/definitely/not/here"))

    def test_malformed_dbg_json(self):
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".topo-dbg.json", delete=False
        ) as fh:
            fh.write("{ this is not valid json ")
            bad = fh.name
        try:
            self.assertIsNone(self.F.load_dbg_meta(bad[: -len(".topo-dbg.json")]))
        finally:
            os.unlink(bad)

    def test_wrong_shape_dbg_json(self):
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".topo-dbg.json", delete=False
        ) as fh:
            json.dump({"schema_version": "1"}, fh)  # no "symbols"
            bad = fh.name
        try:
            self.assertIsNone(self.F.load_dbg_meta(bad[: -len(".topo-dbg.json")]))
        finally:
            os.unlink(bad)

    def test_no_summary_template_degrades(self):
        self.assertIsNone(
            self.F.render_summary({"views": []}, StubValue(StubType("X")), None)
        )

    def test_unresolvable_placeholder_degrades(self):
        # summary references containers that resolve to nothing -> None,
        # so lldb shows its default formatting (no exception).
        verts = _int_array("verts", [1])
        mesh = StubValue(StubType("Mesh"), fields={"verts": verts})  # no tris
        self.assertIsNone(self.F.render_summary(self.sym, mesh, None))

    def test_topo_summary_never_raises_without_lldb(self):
        # Calling the lldb entry point with a bogus object must be caught.
        class Boom:
            def GetTarget(self):
                raise RuntimeError("no debugger here")

        self.assertIsNone(self.F.topo_summary(Boom(), {}))

    # -- summary template grammar (mirrors SummaryRenderer.cpp) ----------

    def test_template_brace_escapes(self):
        self.assertEqual(
            self.F.parse_template("a {{ b }} c"),
            [(False, "a { b } c")],
        )
        self.assertIsNone(self.F.parse_template("unterminated {"))
        self.assertIsNone(self.F.parse_template("nested {a{b}}"))
        self.assertIsNone(self.F.parse_template("empty {}"))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.stderr.write(
            "usage: formatter_unit_test.py <formatter.py> <dbg.json>\n"
        )
        sys.exit(2)
    # Hand the remaining argv to unittest cleanly.
    argv = [sys.argv[0]]
    unittest.main(argv=argv, verbosity=2, exit=True)
