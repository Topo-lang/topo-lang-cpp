#!/usr/bin/env python3
# Verifies that `topo init` for C++
# and Rust emits a `.lldbinit` that `command script import`s the language's
# `lldb_formatter.py`, and writes that formatter into the scaffold.
#
# Usage: init_lldbinit_driver.py <topo-init> <language> <src-filename>

import os
import subprocess
import sys
import tempfile

CPP_SRC = """\
namespace app {
class Mesh {
public:
    int vertexCount() const;
    void rebuild();
};
void run();
}  // namespace app
"""

RUST_SRC = """\
pub struct Mesh { pub vertices: i32 }
impl Mesh { pub fn rebuild(&mut self) {} }
pub fn run() {}
"""


def main():
    if len(sys.argv) != 4:
        sys.stderr.write("usage: <topo-init> <language> <src-filename>\n")
        sys.exit(2)
    topo_init, lang, src_name = sys.argv[1:]
    src = CPP_SRC if lang == "cpp" else RUST_SRC

    work = tempfile.mkdtemp(prefix="topo-init-lldb-")
    try:
        os.makedirs(os.path.join(work, "src"))
        with open(os.path.join(work, "src", src_name), "w") as fh:
            fh.write(src)
        r = subprocess.run(
            [topo_init, "--project", work, "--language", lang],
            capture_output=True, text=True, encoding="utf-8", timeout=60,
        )
        if r.returncode != 0:
            sys.stderr.write(r.stdout + r.stderr)
            sys.stderr.write("topo-init failed\n")
            sys.exit(1)

        lldbinit = os.path.join(work, ".lldbinit")
        formatter = os.path.join(work, "topo", "lldb_formatter.py")
        if not os.path.isfile(lldbinit):
            sys.stderr.write("FAIL: .lldbinit not generated\n")
            sys.exit(1)
        if not os.path.isfile(formatter):
            sys.stderr.write("FAIL: topo/lldb_formatter.py not generated\n")
            sys.exit(1)
        init_text = open(lldbinit).read()
        if "command script import topo/lldb_formatter.py" not in init_text:
            sys.stderr.write(
                "FAIL: .lldbinit lacks the formatter import line:\n%s\n"
                % init_text)
            sys.exit(1)
        fmt_text = open(formatter).read()
        expect = 'HOST_LANGUAGE = "%s"' % lang
        if expect not in fmt_text:
            sys.stderr.write(
                "FAIL: embedded formatter missing %r\n" % expect)
            sys.exit(1)
        # Sanity: the embedded script is importable Python.
        compile(fmt_text, formatter, "exec")
        print("OK: %s topo init emitted .lldbinit + %s formatter" %
              (lang, lang))
        sys.exit(0)
    finally:
        import shutil
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
