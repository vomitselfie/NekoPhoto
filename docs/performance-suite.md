# Performance suite and code reports

Three tools that measure the code rather than test it. None of them fails a build: CI prints what they find, and a
person reads it. They steer refactoring and speed work; the tests ([work-counters.md](work-counters.md) among them)
are what hold the line.

## tools/perf_suite.py: how much work an operation does

```bash
python3 tools/perf_suite.py                                     # full size, three runs per scenario
python3 tools/perf_suite.py --quick                             # CI's size, one run
python3 tools/perf_suite.py --compare tools/perf_baseline.json  # and the change against the baseline
python3 tools/perf_suite.py --only open-psd,save-psd --runs 1   # some scenarios
python3 tools/perf_suite.py --profile build-perf/profiles       # also the hot functions and their call paths
```

It needs a release build in `build/` (`--build` for another). On a shared machine run it through `nice` (or
`../.tools/heavy.sh` here); the counts do not depend on the load.

### What it measures

Each scenario runs in a process of its own, with settings and data in `build-perf/home` (the suite's own, so every run
starts from the same state and yours are left alone), offscreen, and the window at 1400 × 1000. For each it reports:

- **instructions:u**: the user-space instructions of every thread, from `perf stat -e instructions:u`. Only the
  operation is counted: perf starts with its counters off (`--delay=-1`), and the suite switches them on just before
  the request and off once the editor has gone quiet after the reply (perf's control fifo), so start-up, opening the
  test document and work left over from the step before are not in the count. The developer benchmarks switch perf
  themselves around the part they measure: the suite passes the fifo's paths in `NEKOPHOTO_PERF_CONTROL`
  (src/app/Bench.cpp). The start-up scenarios count from exec to the first reply on the socket.
- **work counters**: the operation's renders, layer draws, adjustment passes, reductions built, dabs and Smart Filter
  passes ([work-counters.md](work-counters.md)), read before and after with the `debug.counters` automation hook, or
  printed by the benchmark as a `work:` line. They are exact and the same on any machine.
- **peak RSS**: the process's, from `/usr/bin/time -f %M`, and the operation's own: the high-water mark reset through
  `/proc/<pid>/clear_refs` just before the request and read after it.

Wall-clock times are kept in the JSON as `wall_s_informational` and never compared.

### The scenarios

| Name | What |
|---|---|
| startup-window | exec to an idle window (offscreen) answering its first request |
| startup-headless | the same, `--headless` |
| open-psd | open a synthetic 8000 × 6000 PSD of 42 layers (2000 × 1500 with `--quick`) |
| open-real-psd | open a real PSD given with `--real-psd` (or `NEKOPHOTO_PERF_REAL_PSD`); full size only, the file never committed |
| brush-round | `--bench-brush round` on a 4000 × 3000 document: five strokes of 80 moves through the canvas |
| brush-mypaint | `--bench-brush classic/dry_brush` on 1086 × 1448 |
| adjust-drag | `--bench-adjust`: 30 ticks of a Levels slider over five 4096 × 4096 layers, each repainted |
| filter-gaussian | Gaussian Blur, radius 50, on the synthetic PSD's base layer |
| content-aware-fill | Content-Aware Fill of a region of the base layer (1/13 × 1/10 of the canvas) |
| save-nekophoto | save the opened synthetic PSD as a .nekophoto project |
| save-psd | export it as a PSD |
| undo-redo | undo and redo an Invert of the whole base layer |

The synthetic PSD is made by the editor itself over its socket the first time it is needed (a textured base, 28
painted layers the size of what they hold, in five folders, one nested, with blend modes, masks and clipping; Levels,
Curves, Hue/Saturation and Color Balance layers; a smart object with a Smart Filter; two text layers, one styled) and
kept in `build-perf/`, named after the generator's code. The editor's PSD writer shapes the file too: after changing it,
delete `build-perf/synthetic-*` so a comparison opens the same file.

### Noise, the baseline and comparing

`tools/perf_baseline.json` holds a `full` and a `quick` section, each with the machine, compiler and commit it was
taken on. `--compare` prints, per scenario, the instruction count and its change, every work counter that changed and
any peak RSS change past `--rss-threshold` (10 %). An instruction change counts as real past `--threshold` (2 %) or the
runs' own spread, whichever is larger. Instruction counts repeat to within about 0.2 % here; the brush benchmarks, whose
events are paced by a timer, vary by a few percent, and their work counters vary slightly between runs too, so their
spread is kept per counter and allowed for. A count from another CPU or compiler is not comparable with the
baseline's (the code differs); the work counters still are.

To refresh the baseline after a change that is meant to move the numbers, on a quiet machine:

```bash
python3 tools/perf_suite.py --runs 3 --write-baseline tools/perf_baseline.json
python3 tools/perf_suite.py --quick --runs 3 --write-baseline tools/perf_baseline.json
```

and say in the commit message what moved and why. `--json` keeps a run, `--load` reads one back (to compare or store
it without running again), `--markdown` writes the table.

### CI

The GCC job runs `--quick --compare tools/perf_baseline.json` after the tests. GitHub's runners are virtual machines
that usually expose no hardware performance counters; the suite tries perf (installing it and lowering
`kernel.perf_event_paranoid`), and when `instructions:u` cannot be counted it says so and reports the work counters and
peak RSS alone. Counting instructions under valgrind (callgrind) instead was considered and left out: it is some fifty
times slower, runs the threads one at a time and takes other code paths (no AVX-512), so its counts would match neither
perf's nor the baseline's. The step is `continue-on-error` and never fails the build.

### Baseline

Taken 2026-10-29 on an AMD Ryzen AI 9 HX 370 (24 threads), GCC 16.2, Release, at 43a8029 with this suite; the median
of three runs. Instructions in billions; peak RSS of the operation (the process's for the start-up and benchmarks).

| Scenario | instructions:u, full | quick | peak RSS, full | quick | work counters (full) |
|---|---:|---:|---:|---:|---|
| adjust-drag | 7.78 | 6.47 | 592 MB | 145 MB | render.calls 31 layer.draws 10 adjustment.passes 31 |
| brush-mypaint | 3.86 | 4.72 | 138 MB | 117 MB | render.calls 411 layer.draws 822 mip.builtPixels 2.3M |
| brush-round | 3.75 | 4.38 | 260 MB | 123 MB | render.calls 406 layer.draws 812 mip.builtPixels 19.3M brush.dabs 11.6k |
| content-aware-fill | 542.06 | 20.88 | 1852 MB | 233 MB | render.calls 3 layer.draws 99 adjustment.passes 9 mip.builtPixels 16.0M |
| filter-gaussian | 33.34 | 6.06 | 2757 MB | 372 MB | render.calls 3 layer.draws 99 adjustment.passes 9 mip.builtPixels 17.4M |
| open-psd | 31.95 | 6.39 | 1856 MB | 215 MB | render.calls 4 layer.draws 132 adjustment.passes 12 mip.builtPixels 106.4M |
| open-real-psd | 12.47 | n/a | 454 MB | n/a | render.calls 7 layer.draws 97 adjustment.passes 8 mip.builtPixels 10.2M |
| save-nekophoto | 219.05 | 17.33 | 1855 MB | 269 MB | render.calls 2 layer.draws 66 adjustment.passes 6 |
| save-psd | 137.46 | 8.80 | 2853 MB | 329 MB | render.calls 1 layer.draws 33 adjustment.passes 3 |
| startup-headless | 1.72 | 1.72 | 91 MB | 92 MB | - |
| startup-window | 1.72 | 1.72 | 91 MB | 91 MB | - |
| undo-redo | 5.24 | 5.14 | 1844 MB | 233 MB | render.calls 4 layer.draws 132 adjustment.passes 12 |

## tools/code_shape.py: file sizes, function lengths, header fan-in

```bash
python3 tools/code_shape.py [--top 25] [--json shape.json] [--markdown shape.md]
```

Over our own sources under `src/` and `tests/` (not `src/third_party`):

- **files** by lines, and lines of code (neither blank nor only a comment);
- **functions** by length: every function body at namespace or class scope, found by matching braces once comments,
  literals and preprocessor lines are blanked out, from the line its signature starts on to its closing brace (lambdas
  inside count towards their function). A heuristic, not a parser: good enough to rank, not to count exactly;
- **headers** by fan-in: per header of ours, the translation units that include it directly (their own `#include`
  lines, resolved against each unit's include paths from `build/compile_commands.json`) and transitively (the
  dependencies the compiler recorded, `ninja -t deps`). The transitive count is how many units a change to it rebuilds.
  It needs a built build folder and skips this part, saying so, without one.

CI's GCC job prints the top 15 of each list.

## tools/dead_code.py: candidates for dead code, with the evidence

```bash
../.tools/heavy.sh python3 tools/dead_code.py --prepare          # once: two builds and a coverage test run (heavy)
python3 tools/dead_code.py --json dead.json --markdown dead.md   # the report from what they hold
```

`--prepare` configures and builds `build-gc` and `build-cov` (git-ignored, -j4) and runs the whole ctest, then
`tools/rpc_smoke.py`, in `build-cov`. Set `PATCHY_FIXTURES`, `PHOTOSHOP_CORPUS` and `PSD_TOOLS_CORPUS` first for the
corpus oracles to run (and count) too. Five kinds of evidence, each with how far it can be trusted:

1. **Unreferenced functions**: `build-gc` is built at -O0 (nothing inlined away) with `-ffunction-sections` and linked
   with `--gc-sections` by GNU ld; the report links every executable again with `--print-gc-sections`, `--trace` and a
   link map. A function of ours (a strong definition,
   not an inline or template copy) that every executable dropped, or whose object none loaded, is *unreferenced*; one
   the app dropped and a test kept is *test-only*. Virtual functions are reached through vtables and slots through
   moc's tables, so they count as referenced while their class is; inline header functions are only emitted where used
   and never show. Unused copies of library templates (Qt's and the standard library's helpers a file instantiated) are
   left out: only names in `app::`, `nekophoto::` or plain C names are listed.
2. **Never executed**: `build-cov` is built with `--coverage`; gcov's counts after ctest and rpc_smoke give each file's
   line coverage and the functions no test ran. Never run by a test is not never run by a person; a function both
   unreferenced and never executed is the strongest candidate. At -O0, GCC 15 and later turn on the standard library's
   assertions (`_GLIBCXX_ASSERTIONS`): a test that trips one fails here though it passes in a release build, and the
   report counts what ran up to it. That is a finding of its own (a `std::clamp` with its bounds reversed, say); the
   timing tests (hostile_input_tests, seamcarve_tests, the work-counter and motion self-tests) also fail at -O0's speed.
3. **QSettings keys** written and never read, or read and never written (only defaulted), from literal keys, constants
   in the same file, groups and literal prefixes of keys built at run time; the accesses it cannot resolve are listed.
   Self-tests are left out. A key whose name appears as a string elsewhere is marked low confidence.
4. **Resources** in the .qrc files and `qt_add_resources` lists that no source, stylesheet or code-built path mentions.
5. **Translations** no longer in the code (lupdate over the sources into a copy of each .ts; `check_translations.py`
   fails on these too).

Nothing in the report is removed by the tool; deleting a candidate is a change of its own, with the evidence in its
commit message.
