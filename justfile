default: full

COLORIZE_BUILD := "2>&1 | sed -E -e 's/(missed:)/\\x1b[31m\\1\\x1b[0m/g' -e 's/(optimized:)/\\x1b[32m\\1\\x1b[0m/g' -e 's#(" + justfile_directory() + "/(src|include)/[^:]+:[0-9]+:[0-9]+:)#\\x1b[95m\\1\\x1b[0m#g'"

full:
    DATA_FILE=data/bg_big.bin ./scripts/benchmark.sh

quick runs="2":
    RUNS={{runs}} ./scripts/benchmark.sh

bench *versions:
    ./scripts/benchmark.sh {{versions}}

profile *versions:
    ./scripts/benchmark.sh -p {{versions}}

build *versions:
    ./scripts/build_all.sh {{versions}}

bootstrap:
    ./bootstrap.sh

lshaz *dirs:
    ./scripts/lshaz_check.sh {{dirs}}

perf-report dir="":
    ./scripts/perf_report.sh {{dir}}

build-type type *versions:
    ./scripts/build_all.sh --type {{type}} {{versions}}

build-one type version:
    ./scripts/build_all.sh --type {{type}} --stream {{version}} {{COLORIZE_BUILD}}

release *versions:
    just build-type Release {{versions}}

relwithdebinfo *versions:
    just build-type RelWithDebInfo {{versions}}

debug *versions:
    just build-type Debug {{versions}}

asan *versions:
    just build-type AsanDebug {{versions}}

tsan *versions:
    just build-type TsanDebug {{versions}}

valgrind *versions:
    just build-type ValgrindDebug {{versions}}

vg *args:
    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
      --track-fds=yes --read-var-info=yes --num-callers=50 --error-exitcode=1 \
      --free-is-write=yes --fair-sched=yes --run-libc-freeres=no --partial-loads-ok=no {{args}}

diagnose type version *args:
    ./scripts/run_variant.sh {{type}} {{version}} {{args}}

deep-profile n="4" *versions="":
    ./scripts/deep_profile.sh {{n}} {{versions}}

multi-bench runs="6" top_n="6":
    ./scripts/multi_dataset_benchmark.sh {{runs}} {{top_n}}

deep-profile-big n="4" *versions="":
    ./scripts/deep_profile.sh --data-file data/bg_big.bin {{n}} {{versions}}

open-graphs tag="":
    #!/usr/bin/env bash
    set -euo pipefail
    dir="reports/graphs"
    [[ -n "{{tag}}" ]] && dir="reports/graphs_{{tag}}"
    if [[ ! -f "$dir/index.html" ]]; then
        echo "ERROR: $dir/index.html not found - run 'just full'/'just bench' (or 'just multi-bench' for a tag) first" >&2
        exit 1
    fi
    xdg-open "$dir/index.html"

open-deep-profile:
    #!/usr/bin/env bash
    set -euo pipefail
    f="$(find reports/deep_profile -maxdepth 3 -name perf_index.html -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)" || true
    if [[ -z "$f" ]]; then
        echo "ERROR: no deep-profile dashboard found - run 'just deep-profile' first" >&2
        exit 1
    fi
    xdg-open "$f"

open-profile:
    #!/usr/bin/env bash
    set -euo pipefail
    f="$(find reports/prefix_output -maxdepth 3 -name perf_index.html -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)" || true
    if [[ -z "$f" ]]; then
        echo "ERROR: no interactive-profile dashboard found - run 'just profile <version>' (with a perf-record prefix assigned) first" >&2
        exit 1
    fi
    xdg-open "$f"

clean:
    rm -rf src/*/build src/*/build_* reports
