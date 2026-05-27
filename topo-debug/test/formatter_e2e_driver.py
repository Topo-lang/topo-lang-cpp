#!/usr/bin/env python3
# End-to-end driver for the native-lldb formatter.
#
# Verifies acceptance criterion 1: with the formatter loaded via the
# project's `.lldbinit`, `lldb -b -o 'frame variable mesh'` prints the
# logical declared summary string (NOT the raw post-transform layout);
# without it, output is unchanged.
#
# This needs an lldb built WITH a Python script interpreter. The bundled
# topo-llvm/llvm-dev lldb is script-free. When the
# provided lldb cannot run `script`, the driver prints
# `SKIPPED: <reason>` and exits 0 — CTest's SKIP_REGULAR_EXPRESSION
# turns that into an honest skip (not a phantom pass, not a failure).
#
# Usage:
#   formatter_e2e_driver.py <lldb> <topo-build> <fixture-dir> <formatter.py>
#                           <var> <break-spec> <expected-substring>

import os
import shutil
import subprocess
import sys
import tempfile


def _skip(reason):
    print("SKIPPED: " + reason)
    sys.exit(0)


def _lldb_has_scripting(lldb):
    try:
        out = subprocess.run(
            [lldb, "-b", "-o", "script 1+1"],
            capture_output=True, text=True, encoding="utf-8", timeout=60,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        _skip("could not invoke lldb (%s)" % exc)
    combined = (out.stdout or "") + (out.stderr or "")
    if "Embedded script interpreter unavailable" in combined:
        return False
    if "without scripting language support" in combined:
        return False
    return True


def main():
    if len(sys.argv) != 8:
        sys.stderr.write("bad arg count\n")
        sys.exit(2)
    (lldb, topo_build, fixture_dir, formatter_py,
     var, break_spec, expected) = sys.argv[1:]

    if not os.path.isfile(lldb):
        _skip("lldb binary not found at %s" % lldb)
    if not _lldb_has_scripting(lldb):
        _skip(
            "bundled lldb was built without a Python script interpreter; "
            "`command script import` cannot load the formatter "
            "(bundled lldb has no Python scripting support)"
        )

    work = tempfile.mkdtemp(prefix="topo-fmt-e2e-")
    try:
        for fn in os.listdir(fixture_dir):
            shutil.copy(os.path.join(fixture_dir, fn),
                        os.path.join(work, fn))
        build = subprocess.run(
            [topo_build], cwd=work, capture_output=True, text=True, encoding="utf-8",
            timeout=120,
        )
        if build.returncode != 0:
            sys.stderr.write(build.stdout + build.stderr)
            sys.stderr.write("topo build failed\n")
            sys.exit(1)
        binary = os.path.join(work, "main")
        dbg = binary + ".topo-dbg.json"
        if not os.path.isfile(binary) or not os.path.isfile(dbg):
            sys.stderr.write("expected %s and %s\n" % (binary, dbg))
            sys.exit(1)

        # Baseline: NO formatter — raw lldb output must NOT contain the
        # logical summary (proves the formatter is what produces it / no
        # global side effects).
        base = subprocess.run(
            [lldb, "-b",
             "-o", "breakpoint set --file main.cpp --line %s" % break_spec,
             "-o", "run",
             "-o", "frame variable %s" % var,
             "-o", "quit", binary],
            cwd=work, capture_output=True, text=True, encoding="utf-8", timeout=120,
        )
        base_out = base.stdout + base.stderr
        if expected in base_out:
            sys.stderr.write(base_out)
            sys.stderr.write(
                "baseline already contains the summary — fixture invalid\n")
            sys.exit(1)

        # With formatter loaded via a generated .lldbinit.
        lldbinit = os.path.join(work, ".lldbinit")
        with open(lldbinit, "w") as fh:
            fh.write("command script import %s\n" % formatter_py)
        run = subprocess.run(
            [lldb, "-b",
             "-o", "command source %s" % lldbinit,
             "-o", "breakpoint set --file main.cpp --line %s" % break_spec,
             "-o", "run",
             "-o", "frame variable %s" % var,
             "-o", "quit", binary],
            cwd=work, capture_output=True, text=True, encoding="utf-8", timeout=120,
        )
        out = run.stdout + run.stderr
        if expected not in out:
            sys.stderr.write(out)
            sys.stderr.write(
                "\nFAIL: expected logical summary %r not in lldb output\n"
                % expected)
            sys.exit(1)
        print("OK: lldb printed the logical declared view %r" % expected)
        sys.exit(0)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
