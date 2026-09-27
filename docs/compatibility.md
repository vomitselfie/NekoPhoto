# Compatibility & correctness

**English** · [日本語](#日本語)

What NekoPhoto checks, how often, and where it still falls short. The numbers were gathered by running the tools
on 2026-09-27 against NekoPhoto 1.6.1, not copied from other pages; the commands are beside each one so you can
run them again.

## At a glance

| Check | Result (2026-09-27) | How to rerun |
|---|---|---|
| PSD round trip over Patchy's fixtures | **117 of 117 files pass**, 3,675 carried blocks back byte for byte, 20 type layers opened as editable text | `build/tests/psd_roundtrip ../Patchy/test-fixtures/psd` |
| Render hashes | **133 scenes** (blend modes, brushes, filters, adjustments, golden scenes), each rendered on the worker pool and serially | `ctest -R render_hash_tests` |
| Golden images | **6 golden test cases over 21 reference PNGs** in `tests/golden/` | `ctest -R golden_tests` |
| Test suites | **38 CTest suites** (326 `TEST_CASE`s), 38 of 38 passing | `ctest --test-dir build` |
| Compiler warnings | none: CI builds with `-Werror` on GCC and Clang | `-DCOMPOSITOR_WARNINGS_AS_ERRORS=ON` |

## The PSD corpus

The fixtures are the MIT-licensed test files of [Patchy](https://github.com/SethRobinson/Patchy)
(`test-fixtures/psd`): 117 PSD and PSB files, all but two saved by Photoshop 2026, each built to pin down
one construct. Among them: 17 text files (point, box, vertical, rotated, right-to-left, warped, tracking and
leading), 19 smart objects and Smart Filters, 3 PSB, and the rest layer styles (every effect, global light,
contours, patterns), shapes and vector masks, both masks with density and feather, Blend If, clipping, folder
effects and opacity, adjustment layers (Curves, Levels, Hue/Saturation, Color Balance, Brightness/Contrast,
Posterize, Threshold, Invert), saved channels and paths, and CMYK style colours.

`psd_roundtrip` opens each file, exports it as PSD, compares the two record by record (every carried block,
Blend If, the mask section, blend key, flags, folder state, opacity and clipping, the image resources and the
global blocks) and reopens the export. On this run every file passed and nothing was lost.

Beyond Patchy's corpus, `tests/psd_writer_tests.cpp` covers the edit cases (painted, moved, opacity changed, a
project save, a canvas change), and every layered PSD from Photoshop and Clip Studio the project has (up to 54
layers in 16 folders at 4096 × 4096) exports, reopens in a fresh process and renders with a maximum difference of
zero ([psd-export.md](psd-export.md#how-it-is-tested)).

## Supported PSD constructs

Read, drawn and written back:

- **Layers and folders**: names (Unicode), positions past the canvas edge, visibility, opacity and Fill,
  all Photoshop blend modes' keys, nested folders (pass-through and isolated), closed state, colour labels, locks.
- **Masks**: layer and folder masks (also disabled), both masks on one layer, mask density and feather,
  vector masks on any layer, clipping masks ([vector-masks.md](vector-masks.md)).
- **Adjustment layers**: Levels, Curves, Exposure and Hue/Saturation as Photoshop adjustment layers; the rest
  (Brightness/Contrast, Color Balance, Selective Color ...) carried unchanged.
- **Layer styles**: drop and inner shadow, outer and inner glow, bevel and emboss (with texture and contour),
  satin, colour, gradient and pattern overlays, strokes (several per layer), Blend If, knockout, blend-interior
  options, drawn as Photoshop draws them and written back as its bytes ([layer-styles.md](layer-styles.md)).
- **Shapes**: solid, gradient and pattern fill layers, strokes (inside, centred, outside; dashes, caps, joins),
  path booleans, live rectangles ([vector-masks.md](vector-masks.md)).
- **Text**: point and box text, several style runs, tracking, leading, alignment, rotation, the fifteen Warp Text
  presets open as editable text, and NekoPhoto text is written as Photoshop type layers
  ([psd-roundtrip.md](psd-roundtrip.md#text)).
- **Smart objects**: embedded PSD/PSB, PNG and other images, nested up to four deep, placed, moved, scaled,
  replaced and edited; thirteen Smart Filters drawn and editable; linked files carried
  ([smart-objects.md](smart-objects.md)).
- **Document**: colour profile, XMP and EXIF, captions, print settings, layer comps, guides, slices, paths,
  plug-in resources, patterns and linked-file data carried; CMYK files converted through their own ICC profile.
- **PSB** (large documents) read and written.

## Known fidelity gaps

Collected from the pages above; each is also listed before you export or in the export's notes.

- **Not verified in Photoshop.** The files match what Photoshop wrote, structure for structure, but they have
  not been opened in Photoshop itself, so a warning on open or a re-layout of our type layers there is not ruled
  out.
- **Export is 8 bits per channel, RGB.** A CMYK, Lab or grayscale file opens converted and exports as RGB,
  without its original colour profile. 16-bit and PSB Smart Filter caches are left for Photoshop to rebuild.
- **Written as pixels**: scaled, rotated or flipped layers are resampled into place; shape layers made in
  NekoPhoto; flipped text; adjustments Photoshop has no equivalent for (Grain, Gradient Map, Hue/Saturation on
  the plain scale) become a pixel layer of their result; a layer clipped to one not directly beneath it is
  written unclipped.
- **Folder opacity and blend modes** set in NekoPhoto are written as set but shown differently than in Photoshop.
- **Not carried**: resolution (ours is written), thumbnails, the ID seed; guides, slices and paths once the canvas
  size changes; a live shape's origination once it moves.
- **Layer styles not drawn yet**: contours on shadows and glows (drawn linear), noise and jitter, Dissolve (drawn
  as Normal), "Layer Mask Hides Effects". A bevel with a non-monotone contour is 6.1 levels off on average.
- **Vector masks**: shape feather is 5.4 levels off (Photoshop feathers the shape as one render); gradient and
  pattern strokes are not drawn.
- **Smart objects**: no dragging a Smart Filter between smart objects, no editing a stack that holds a filter not
  drawn here, no linked filter mask, no relinking of linked files.

How close the drawing is, as a mean difference per pixel on a 0-255 scale against Photoshop's own renders of the
fixtures: most layer styles 0.00-0.9, vector masks and shapes 0.00-1.3, styled folders 0.00-2.6
([layer-styles.md](layer-styles.md#how-close), [vector-masks.md](vector-masks.md#how-close)).

## Continuous integration

Every push and pull request runs:

| Job | Runner | What it does |
|---|---|---|
| GCC | Ubuntu 24.04 | `-Werror` build, all CTest suites (unit, golden, render hashes, hostile input, translations), offscreen smoke test, automation socket (`tools/rpc_smoke.py`) and MCP bridge (`tools/mcp_smoke.py`) smoke tests |
| Clang | Ubuntu 24.04 | the same with Clang |
| Windows | windows-latest, MSYS2 UCRT64 MinGW-w64 | `-Werror` build, the tests, offscreen smoke test, `tools/rpc_smoke.py` over the named pipe, the portable zip |

A release tag also builds the AppImage and tarball on Ubuntu 22.04 (the oldest supported base), runs the tests,
starts the packaged AppImage offscreen (`--version` and a demo screenshot), and builds and tests the Windows zip.

**Hostile input**: `tests/hostile_input_tests.cpp` (in CTest, so on every push) keeps each crash found by fuzzing
as a regression test. **Fuzzing** itself runs locally, not in CI: libFuzzer targets for the PSD reader and its
block parsers, TGA, ICO, GIF, Aseprite, Affinity, SVG, preset files and colour lookups, under AddressSanitizer
and UndefinedBehaviorSanitizer ([fuzzing.md](fuzzing.md)).

## Benchmarks

AMD Ryzen AI 9 HX 370 (12 cores, 24 worker threads), Manjaro, GCC 16, Release build; median of 9 runs of
`build/tests/bench_core`, measured 2026-09-26 ([benchmarks.md](benchmarks.md)):

| Operation | Median |
|---|---:|
| Render a 4000 × 3000 document, 12 layers | 207 ms |
| The same at 0.25 | 20 ms |
| A 1024 × 768 region at 1:1 | 30 ms |
| 80 px brush stroke across 4000 × 3000, hard / soft | 20 ms / 45 ms |
| Gaussian blur on 4000 × 3000, radius 2 / 20 | 50 ms / 85 ms |
| Levels / Curves / Hue/Saturation | 1.7 ms / 1.7 ms / 18.6 ms |

Application-level timings (opening a 70 MB PSD in 0.7 s and so on) are in the [README](../README.md#performance).

---

## 日本語

[English](#compatibility--correctness) · **日本語**

NekoPhoto が何をどのくらいの頻度で検証しているか、そしてまだ足りない点をまとめたページです。数値は 2026-09-27 に
NekoPhoto 1.6.1 でツールを実行して集計したものです。

- **PSD の往復**: [Patchy](https://github.com/SethRobinson/Patchy) の MIT ライセンスのテストファイル 117 個(2 個を除き
  Photoshop 2026 で保存)すべてが合格し、3,675 個のブロックがバイト単位で変化なく戻りました。テキストレイヤー 20 個は編集可能なテキストとして開きます。
- **描画のハッシュ**: 133 シーン。**ゴールデン画像**: 6 テスト・参照 PNG 21 枚。**テストスイート**: CTest 38 個(すべて合格)。
- **対応している PSD の要素**: レイヤーとグループ、描画モード、マスク(レイヤーマスク・ベクターマスク・両方・濃度とぼかし)、
  クリッピング、調整レイヤー、レイヤースタイル、シェイプ、編集可能なテキスト、スマートオブジェクトとスマートフィルター、PSB。
- **既知の差異**: Photoshop 本体で開いての確認はまだです。書き出しは 8 ビット RGB のみ。変形したレイヤーや Photoshop に
  相当するもののない調整はピクセルとして書き出されます。一部のレイヤースタイル(シャドウ・光彩の輪郭、ノイズ、ディザ合成)は
  まだ描画されません。
- **CI**: GCC と Clang(Ubuntu 24.04、`-Werror`)、Windows(MSYS2 の MinGW-w64)でビルドとテスト、画面なしのスモークテスト、
  自動操作ソケットと MCP のテストを実行します。リリースでは Ubuntu 22.04 で AppImage を作って起動を確認します。
  悪意あるファイルのテストは毎回実行し、ファジングはローカルで行います([fuzzing.md](fuzzing.md)、英語)。
- **ベンチマーク**: AMD Ryzen AI 9 HX 370 で 4000 × 3000・12 レイヤーの描画が 207 ミリ秒など([benchmarks.md](benchmarks.md)、英語)。
