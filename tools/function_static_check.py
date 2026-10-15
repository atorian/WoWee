#!/usr/bin/env python3
"""Mutable statics inside the functions of the interface and the addon bindings.

    tools/function_static_check.py           # count, and list each one
    tools/function_static_check.py --quiet   # the count alone

WHY

A `static` inside a function is state that belongs to the process. Written in
an addon binding or an interface panel, it is almost always state that belongs
to the character instead: which quest groups are collapsed, which dungeons are
ticked in the LFG list, how the auction browse is sorted, which tab of a panel
was last shown. Nothing clears it when the character logs out - the Lua state
that asked for it is destroyed and built again, the static is not - so the next
character logs in and finds the last one's choices still made. That is what
switching characters looked like before core::CharacterSession took that state
over (addons::InterfaceState).

Each one compiles, reads like a cache, and is correct for exactly as long as
one character is played per run of the client.

WHAT IT DOES

Strips comments and string literals from every .cpp and .hpp under src/ui,
include/ui, src/addons and include/addons, walks the braces, and reports each
`static` declaration whose enclosing scope is a function body (a lambda
counts). `static const` and `static constexpr` are not state and are not
counted; neither is a table of string literals, nor a pointer to const that is
itself const. sweep_guard holds the counts where they were left, the addon
bindings and the interface apart; they only go down.

What was left on 2026-10-15 is process-wide by intent - a log said once per
id, a re-entrancy guard, the key bindings and chat layout that are written to
the config root, DBC and texture caches, per-frame scratch - and better still
with an owner that says so than in a function, which is why the ceiling is a
ratchet and not a list of exemptions.

WHAT IT CANNOT SEE

Mutable state at namespace scope or in a class's static members: those are
declared where an owner could be read off them, and are a separate question.
A declaration hidden behind a macro.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ROOTS = ["src/ui", "include/ui", "src/addons", "include/addons"]

# What a brace opens, read off the text before it.
TYPE_SCOPE = re.compile(r"\b(namespace|class|struct|union|enum)\b[^()]*$")
EXTERN_BLOCK = re.compile(r"\bextern\s*$")
STATIC = re.compile(r"\bstatic\b")


def strip_code(text):
    """The text with comments, string and character literals blanked out.

    Lengths and newlines are kept, so an offset still names the same line.
    Raw strings are handled because the Lua the bindings install is written in
    them, and a brace inside Lua is not one of ours.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            end = text.find("\n", i)
            end = n if end == -1 else end
            out.append(" " * (end - i))
            i = end
        elif c == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            end = n if end == -1 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[i:end]))
            i = end
        elif c == "R" and nxt == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            open_paren = text.find("(", i + 2)
            delim = text[i + 2:open_paren]
            end = text.find(")" + delim + '"', open_paren)
            end = n if end == -1 else end + len(delim) + 2
            out.append(re.sub(r"[^\n]", " ", text[i:end]))
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == "\\":
                    j += 1
                elif text[j] == "\n":
                    break
                j += 1
            end = min(j + 1, n)
            out.append(" " * (end - i))
            i = end
        else:
            out.append(c)
            i += 1
    return "".join(out)


def is_mutable(declaration):
    """Whether a `static ...` declaration, up to its name, declares state."""
    words = declaration.split()
    words = [w for w in words if w not in ("static", "inline", "thread_local")]
    if not words:
        return False
    if words[0] == "constexpr":
        return False
    if words[0] == "const":
        rest = " ".join(words[1:])
        # A table of string literals - `static const char* kNames[] = {...}` -
        # is a constant table by every use made of it.
        if "[" in rest:
            return False
        # A pointer to const is still a pointer that can be moved, unless it
        # is const itself.
        if "*" in rest:
            return not re.search(r"\*\s*const\b", rest)
        return False
    return True


def function_statics(path):
    """(line, declaration) for every mutable static inside a function body."""
    code = strip_code(path.read_text(encoding="utf-8", errors="ignore"))
    found = []
    # Each open brace: True when it opens a body of code, False for a
    # namespace, a class, an enum or an extern block.
    stack = []
    statement_start = 0
    i, n = 0, len(code)
    while i < n:
        c = code[i]
        if c == "{":
            header = code[statement_start:i]
            is_type = bool(TYPE_SCOPE.search(header)) or bool(EXTERN_BLOCK.search(header))
            stack.append(not is_type)
            statement_start = i + 1
        elif c == "}":
            if stack:
                stack.pop()
            statement_start = i + 1
        elif c == ";":
            statement_start = i + 1
        elif c == "s" and code.startswith("static", i) and STATIC.match(code, i) \
                and (i == 0 or not (code[i - 1].isalnum() or code[i - 1] == "_")) \
                and any(stack):
            # Only at the start of a statement: anything else is part of an
            # expression, which a declaration cannot be.
            before = code[statement_start:i].strip()
            if before == "" or before.endswith(":"):
                end = i
                while end < n and code[end] not in ";={(":
                    end += 1
                declaration = " ".join(code[i:end].split())
                if is_mutable(declaration):
                    found.append((code.count("\n", 0, i) + 1, declaration))
        i += 1
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--quiet", action="store_true", help="print the count alone")
    args = parser.parse_args()

    files = []
    for root in ROOTS:
        base = ROOT / root
        if base.is_dir():
            files += sorted(p for p in base.rglob("*") if p.suffix in (".cpp", ".hpp", ".h"))
    total = []
    for path in files:
        for line, declaration in function_statics(path):
            total.append((path.relative_to(ROOT).as_posix(), line, declaration))

    # Counted per layer as well as together, so the addon bindings - where the
    # character's interface state lives - hold their own ceiling.
    addons = [t for t in total if "/addons/" in t[0]]
    ui = [t for t in total if "/ui/" in t[0]]
    print(f"{len(files)} interface and addon source files\n")
    print(f"{len(total)} mutable static(s) inside a function body")
    print(f"{len(addons)} of them in the addon bindings (src/addons, include/addons)")
    print(f"{len(ui)} of them in the interface (src/ui, include/ui)")
    if not args.quiet:
        for rel, line, declaration in total:
            print(f"  {rel}:{line}: {declaration}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
