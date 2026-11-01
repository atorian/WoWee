#!/usr/bin/env bash
# test.sh - Run the Catch2 unit tests and/or clang-tidy linter.
#
# Usage:
#   ./test.sh                    # run both lint and tests (default)
#   ./test.sh --lint             # run clang-tidy on what changed from origin/master
#   ./test.sh --lint-all         # run clang-tidy on every source file
#   ./test.sh --test             # run ctest unit tests only
#   ./test.sh --lint --test      # explicit: run both
#   ./test.sh --asan             # run ctest under ASAN/UBSan (requires build_asan/)
#   ./test.sh --test --asan      # same as above
#   ./test.sh --gpu              # run the tests that draw on the GPU, two at a time
#   FIX=1 ./test.sh --lint       # apply clang-tidy fix suggestions (use with care)
#
# Exit code is non-zero if a test fails or clang-tidy reports one of the checks
# .clang-tidy lists under WarningsAsErrors; its other checks are advice.
#
# --lint compares the working tree with LINT_BASE (default: where HEAD left
# origin/master) and checks each changed source file, and for each changed
# header one source file that includes it.
#
# Build directories:
#   build/       - Release build used for normal ctest  (cmake -DCMAKE_BUILD_TYPE=Release)
#                  and the compile commands lint reads (LINT_BUILD_DIR overrides)
#   build_asan/  - Debug+ASAN build used with --asan    (cmake -DCMAKE_BUILD_TYPE=Debug
#                                                              -DWOWEE_ENABLE_ASAN=ON
#                                                              -DWOWEE_BUILD_TESTS=ON)
# End of usage.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
RUN_LINT=0
LINT_ALL=0
RUN_TEST=0
RUN_ASAN=0
RUN_GPU=0

for arg in "$@"; do
    case "$arg" in
        --lint) RUN_LINT=1 ;;
        --lint-all) RUN_LINT=1; LINT_ALL=1 ;;
        --test) RUN_TEST=1 ;;
        --asan) RUN_ASAN=1; RUN_TEST=1 ;;
        --gpu) RUN_GPU=1; RUN_TEST=1 ;;
        --help|-h)
            sed -n '2,/^# End of usage\./p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg"
            echo "Usage: $0 [--lint] [--lint-all] [--test] [--asan] [--gpu]"
            exit 1
            ;;
    esac
done

# Default: run both when no flags provided
if [[ $RUN_LINT -eq 0 && $RUN_TEST -eq 0 ]]; then
    RUN_LINT=1
    RUN_TEST=1
fi

# ---------------------------------------------------------------------------
# ── UNIT TESTS ─────────────────────────────────────────────────────────────
# ---------------------------------------------------------------------------
OVERALL_FAILED=0

if [[ $RUN_TEST -eq 1 ]]; then
    if [[ $RUN_ASAN -eq 1 ]]; then
        BUILD_TEST_DIR="$SCRIPT_DIR/build_asan"
        TEST_LABEL="ASAN+UBSan"
    else
        BUILD_TEST_DIR="$SCRIPT_DIR/build"
        TEST_LABEL="Release"
    fi

    if [[ ! -d "$BUILD_TEST_DIR" ]]; then
        echo "Build directory not found: $BUILD_TEST_DIR"
        if [[ $RUN_ASAN -eq 1 ]]; then
            echo "Configure it with:"
            echo "  cmake -B build_asan -DCMAKE_BUILD_TYPE=Debug -DWOWEE_ENABLE_ASAN=ON -DWOWEE_BUILD_TESTS=ON"
        else
            echo "Run cmake first:  cmake -B build -DCMAKE_BUILD_TYPE=Release -DWOWEE_BUILD_TESTS=ON"
        fi
        exit 1
    fi

    # Check that CTestTestfile.cmake exists (tests were configured)
    if [[ ! -f "$BUILD_TEST_DIR/CTestTestfile.cmake" ]]; then
        echo "CTestTestfile.cmake not found in $BUILD_TEST_DIR - tests not configured."
        echo "Re-run cmake with -DWOWEE_BUILD_TESTS=ON"
        exit 1
    fi

    if [[ $RUN_GPU -eq 1 ]]; then
        # ctest's gpu configuration holds them, out of a plain run; two at a
        # time, as their locks allow no more.
        TEST_KIND="GPU tests"
        CTEST_ARGS=(-C gpu -L gpu --parallel 2)
    else
        # In parallel: 9 s serially, 4 s on 11 cores.
        NPROC=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
        TEST_KIND="unit tests"
        CTEST_ARGS=(--parallel "$NPROC")
    fi

    echo "──────────────────────────────────────────────"
    echo " Running $TEST_KIND [$TEST_LABEL]"
    echo "──────────────────────────────────────────────"
    if ! (cd "$BUILD_TEST_DIR" && ctest --output-on-failure "${CTEST_ARGS[@]}"); then
        OVERALL_FAILED=1
        echo ""
        echo "One or more $TEST_KIND FAILED."
    else
        echo ""
        echo "All $TEST_KIND passed."
    fi
fi

# ---------------------------------------------------------------------------
# ── LINT ───────────────────────────────────────────────────────────────────
# ---------------------------------------------------------------------------
if [[ $RUN_LINT -eq 0 ]]; then
    exit $OVERALL_FAILED
fi

# Dependency check
CLANG_TIDY=""
for candidate in clang-tidy clang-tidy-18 clang-tidy-17 clang-tidy-16 clang-tidy-15 clang-tidy-14; do
    if command -v "$candidate" >/dev/null 2>&1; then
        CLANG_TIDY="$candidate"
        break
    fi
done

if [[ -z "$CLANG_TIDY" ]]; then
    echo "clang-tidy not found. Install it with:"
    echo "  sudo apt-get install clang-tidy"
    exit 1
fi

echo "──────────────────────────────────────────────"
echo " Running clang-tidy lint"
echo "──────────────────────────────────────────────"
echo "Using: $($CLANG_TIDY --version | head -1)"

# run-clang-tidy runs checks in parallel; fall back to sequential if absent.
RUN_CLANG_TIDY=""
for candidate in run-clang-tidy run-clang-tidy-18 run-clang-tidy-17 run-clang-tidy-16 run-clang-tidy-15 run-clang-tidy-14; do
    if command -v "$candidate" >/dev/null 2>&1; then
        RUN_CLANG_TIDY="$candidate"
        break
    fi
done

# Build database check
LINT_BUILD_DIR="${LINT_BUILD_DIR:-$SCRIPT_DIR/build}"
COMPILE_COMMANDS="$LINT_BUILD_DIR/compile_commands.json"
if [[ ! -f "$COMPILE_COMMANDS" ]]; then
    echo "compile_commands.json not found at $COMPILE_COMMANDS"
    echo "Run cmake first:  cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    exit 1
fi

# clang-tidy cannot read a precompiled header built by a different compiler,
# and it usually is one: the PCH comes from whatever CXX cmake picked, while
# clang-tidy comes from whatever LLVM is on PATH. Mismatched, every file fails
# with "PCH file uses an older format that is no longer supported" and the real
# diagnostics never appear.
#
# So lint against a copy of the database with the PCH arguments removed. The
# headers are still included normally -- the PCH is only a parse cache.
TIDY_DB_DIR="$LINT_BUILD_DIR/.tidy"
mkdir -p "$TIDY_DB_DIR"
python3 - "$COMPILE_COMMANDS" "$TIDY_DB_DIR/compile_commands.json" <<'PYEOF'
import json, re, sys
src, dst = sys.argv[1], sys.argv[2]
db = json.load(open(src))
strip = re.compile(r'\s+-Xclang\s+-include-pch\s+-Xclang\s+\S+|\s+-include-pch\s+\S+|\s+-Winvalid-pch')
for entry in db:
    if 'command' in entry:
        entry['command'] = strip.sub('', entry['command'])
    if 'arguments' in entry:
        args, out = entry['arguments'], []
        i = 0
        while i < len(args):
            if args[i] == '-include-pch':
                i += 2
            elif args[i] == '-Xclang' and i + 1 < len(args) and args[i + 1] == '-include-pch':
                i += 4
            else:
                out.append(args[i]); i += 1
        entry['arguments'] = out
json.dump(db, open(dst, 'w'))
PYEOF
COMPILE_COMMANDS="$TIDY_DB_DIR/compile_commands.json"

# The sources the build compiles, relative to the tree: a file outside the
# build (an optional SDK's probe, say) has no flags to parse it with.
python3 - "$COMPILE_COMMANDS" "$SCRIPT_DIR" > "$TIDY_DB_DIR/built_files.txt" <<'PYEOF'
import json, os, sys
db, root = sys.argv[1], os.path.realpath(sys.argv[2])
for entry in json.load(open(db)):
    path = os.path.realpath(os.path.join(entry.get('directory', ''), entry['file']))
    print(os.path.relpath(path, root).replace(os.sep, '/'))
PYEOF

# ---------------------------------------------------------------------------
# Source files to check (first-party only)
# ---------------------------------------------------------------------------
# Read with a while loop rather than mapfile: mapfile is bash 4, and macOS
# ships bash 3.2 as /bin/bash. The shebang finds a newer bash when one is
# installed, so this only bites on a machine without one - where it aborts
# under set -e with "mapfile: command not found" and no lint runs at all.
#
# A header is not a translation unit, so a changed one is checked through a
# source file that includes it: what clang-tidy reports in a header it reports
# from any file that reads it.
list_lint_files() {
    if [[ $LINT_ALL -eq 1 ]]; then
        find "$SCRIPT_DIR/src" -type f \( -name '*.cpp' -o -name '*.cxx' -o -name '*.cc' \)
        return
    fi
    local base changed f rel
    base="${LINT_BASE:-$(git merge-base HEAD origin/master)}"
    changed="$( { git diff --name-only --diff-filter=ACMR "$base" -- src include
                  git ls-files --others --exclude-standard -- src include; } | sort -u)"
    for f in $changed; do
        case "$f" in
            src/*.cpp|src/*.cxx|src/*.cc)
                echo "$SCRIPT_DIR/$f" ;;
            include/*.hpp|include/*.h|src/*.hpp|src/*.h)
                rel="${f#*/}"
                grep -rlF --include='*.cpp' "\"$rel\"" "$SCRIPT_DIR/src" | head -1 || true ;;
        esac
    done
}

SOURCE_FILES=()
while IFS= read -r src_file; do
    [[ -n "$src_file" ]] || continue
    if ! grep -qxF "${src_file#"$SCRIPT_DIR"/}" "$TIDY_DB_DIR/built_files.txt"; then
        echo "Not linted, not in the build: ${src_file#"$SCRIPT_DIR"/}"
        continue
    fi
    SOURCE_FILES+=("$src_file")
done < <(list_lint_files | sort -u)

if [[ ${#SOURCE_FILES[@]} -eq 0 ]]; then
    if [[ $LINT_ALL -eq 1 ]]; then
        echo "No source files found in src/"
        exit 1
    fi
    echo "No source files changed from ${LINT_BASE:-origin/master}; nothing to lint."
    exit $OVERALL_FAILED
fi

echo "Linting ${#SOURCE_FILES[@]} source files..."

# ---------------------------------------------------------------------------
# Resolve GCC C++ stdlib include paths for clang-tidy
#
# compile_commands.json is generated by GCC. When clang-tidy processes those
# commands with its own clang driver it cannot locate GCC's libstdc++ headers.
# We query GCC for its include search list and forward each path as an
# -isystem extra argument so clang-tidy can find <vector>, <string>, etc.
# ---------------------------------------------------------------------------
EXTRA_TIDY_ARGS=()   # for direct clang-tidy:  --extra-arg=...
EXTRA_RUN_ARGS=()    # for run-clang-tidy:     -extra-arg=...
if command -v gcc >/dev/null 2>&1; then
    # Prepend clang's own resource include dir first so clang uses its own
    # versions of xmmintrin.h, ia32intrin.h, etc. rather than GCC's.
    clang_resource_inc="$($CLANG_TIDY -print-resource-dir 2>/dev/null || true)/include"
    if [[ -d "$clang_resource_inc" ]]; then
        EXTRA_TIDY_ARGS+=("--extra-arg=-isystem${clang_resource_inc}")
        EXTRA_RUN_ARGS+=("-extra-arg=-isystem${clang_resource_inc}")
    fi

    while IFS= read -r inc_path; do
        [[ -d "$inc_path" ]] || continue
        # Skip the GCC compiler built-in include dir - clang's resource dir above
        # provides compatible replacements for xmmintrin.h, ia32intrin.h, etc.
        [[ "$inc_path" == */gcc/* ]] && continue
        EXTRA_TIDY_ARGS+=("--extra-arg=-isystem${inc_path}")
        EXTRA_RUN_ARGS+=("-extra-arg=-isystem${inc_path}")
    done < <(
        gcc -E -x c++ - -v < /dev/null 2>&1 \
        | sed -n '/#include <\.\.\.> search starts here:/,/End of search list\./p' \
        | grep '^ ' \
        | sed 's/^ //'
    )
fi

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
FIX="${FIX:-0}"
FIX_FLAG=""
if [[ "$FIX" == "1" ]]; then
    FIX_FLAG="-fix"
    echo "Fix mode enabled - applying suggested fixes."
fi

LINT_FAILED=0

if [[ $LINT_ALL -eq 0 ]]; then
    # The changed files only: a few, so one clang-tidy per file, all at once.
    NPROC=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
    printf '%s\n' "${SOURCE_FILES[@]}" | xargs -P "$NPROC" -I{} \
        "$CLANG_TIDY" -p "$TIDY_DB_DIR" $FIX_FLAG "${EXTRA_TIDY_ARGS[@]}" {} \
        || LINT_FAILED=1
elif [[ -n "$RUN_CLANG_TIDY" ]]; then
    echo "Running via $RUN_CLANG_TIDY (parallel)..."
    # run-clang-tidy takes a source-file regex; match our src/ tree
    SRC_REGEX="$(echo "$SCRIPT_DIR/src" | sed 's|/|\\/|g')"
    "$RUN_CLANG_TIDY" \
        -clang-tidy-binary "$CLANG_TIDY" \
        -p "$TIDY_DB_DIR" \
        $FIX_FLAG \
        "${EXTRA_RUN_ARGS[@]}" \
        "$SRC_REGEX" || LINT_FAILED=$?
else
    echo "run-clang-tidy not found; running sequentially..."
    for f in "${SOURCE_FILES[@]}"; do
        "$CLANG_TIDY" \
            -p "$TIDY_DB_DIR" \
            $FIX_FLAG \
            "${EXTRA_TIDY_ARGS[@]}" \
            "$f" || LINT_FAILED=$?
    done
fi

# ---------------------------------------------------------------------------
# Result
# ---------------------------------------------------------------------------
if [[ $LINT_FAILED -ne 0 ]]; then
    echo ""
    echo "clang-tidy reported issues. Fix them or add suppressions in .clang-tidy."
    OVERALL_FAILED=1
else
    echo ""
    echo "Lint passed."
fi

exit $OVERALL_FAILED
