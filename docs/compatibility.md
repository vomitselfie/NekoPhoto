# Compatibility & correctness

**English** · [日本語](#日本語)

What NekoPhoto checks, how often, and where it still falls short. The first table is counted from the repository on
every build; the other numbers were gathered by running the tools on 2026-09-27 against NekoPhoto 1.6.1, not copied
from other pages. The commands are beside each one so you can run them again.

## At a glance

Counted from the repository by `tools/compat_table.py` (ctest's `compat_table_check` fails when this table is stale;
`python3 tools/compat_table.py --write` regenerates it):

<!-- BEGIN GENERATED at-a-glance: tools/compat_table.py --write; do not edit by hand -->
| Check | Result | How to rerun |
|---|---|---|
| Photoshop as the oracle | **52 of 72** Patchy files render within 2 levels of what Photoshop shows on 99% of pixels, mean under 1 level: Photoshop's own flatten beside the file for 48 of them, the merged image stored in the file for the rest (the floor in `tests/psd_oracle.txt`; it may only rise) | `ctest -R psd_composite_oracle` with Patchy beside this checkout (or `PATCHY_FIXTURES`) |
| Render hashes | **697 scenes**: 156 at 8-bit RGB, 155 at 16-bit RGB, 120 at 32-bit RGB, 71 at 8-bit CMYK, 71 at 16-bit CMYK, 62 at 8-bit Lab, 62 at 16-bit Lab; each rendered on the worker pool and serially | `ctest -R render_hash_tests` |
| Golden images | **6 golden test cases over 21 reference PNGs** in `tests/golden/` | `ctest -R golden_tests` |
| Brush parity | **526 baseline rows**: 294 presets over 16 stroke fixtures | `ctest -R brush_parity` |
| Test suites | **89 CTest tests** registered (725 `TEST_CASE`s); a few need optional dependencies | `ctest --test-dir build` |
| Capability matrix | **115 features** in 7 modes and depths, generated from `supports()` (the table below) | `ctest -R mode_matrix_check` |
| Automation | **183 methods**, 182 of them called in `tools/rpc_smoke.py`; every method sent hostile parameters by `tools/rpc_panic_hunt.py` | `python3 tools/rpc_smoke.py <socket>`, `python3 tools/rpc_panic_hunt.py` |
| Fuzz targets | **11 libFuzzer targets** (PSD and its block parsers, the smaller readers) | [fuzzing.md](fuzzing.md) |
| Compiler warnings | none: CI builds with `-Werror` on GCC and Clang | `-DCOMPOSITOR_WARNINGS_AS_ERRORS=ON` |

| Mode | native | native, partly through RGB | greyed: Photoshop lacks | greyed: not yet |
|---|---:|---:|---:|---:|
| RGB8 | 115 | 0 | 0 | 0 |
| RGB16 | 115 | 0 | 0 | 0 |
| RGB32 | 72 | 0 | 11 | 32 |
| CMYK8 | 86 | 17 | 8 | 4 |
| CMYK16 | 86 | 17 | 8 | 4 |
| Lab8 | 83 | 17 | 11 | 4 |
| Lab16 | 83 | 17 | 11 | 4 |
<!-- END GENERATED at-a-glance -->

Measured by hand over the corpus (2026-09-27, NekoPhoto 1.6.1):

| Check | Result | How to rerun |
|---|---|---|
| PSD round trip over Patchy's fixtures | **117 of 117 files pass**, 3,675 carried blocks back byte for byte, 20 type layers opened as editable text | `build/tests/psd_roundtrip ../Patchy/test-fixtures/psd` |
| The same converted to 16 bits | **118 of 118 files pass** (Patchy's and K.psd): every carried block comes back from a 16-bit export too (3,975 with K.psd) | `PSD_ROUNDTRIP_16=1 build/tests/psd_roundtrip ../Patchy/test-fixtures/psd ../K.psd` |
| Fill layers against Photoshop's pixels | every gradient or pattern fill layer that stores Photoshop's own pixels is drawn by NekoPhoto and compared (Linear, Radial and Reflected gradients and pattern fills within a level; Angle and Diamond have no Photoshop-saved file yet) | `PSD_ROUNDTRIP_FILLS=1 build/tests/psd_roundtrip file.psd` |
| Styled folders, stage by stage | `NEKOPHOTO_DUMP_FOLDERS=<dir>` writes each styled folder's stages while rendering (backdrop, exterior effects, children, after opacity and Fill, interior effects, the effects' shape) as PNGs, to find where a render first differs from Photoshop's | any render, e.g. `document.export` |
| 16-bit PSD round trip | an unedited layer's 16-bit channels come back **byte for byte** | `ctest -R depth_format_tests` |
| Colour profiles | the ICC profile (resource 1039) of **64 of 64** tagged RGB PSDs in the corpus is written back **byte for byte**; conversions match Little CMS and the published sRGB and Adobe RGB matrices; untagged 8-bit documents render bit for bit as before | `ctest -R colormgmt_tests` ([color-management.md](color-management.md)) |

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

`psd_composite_oracle` (in CTest; skipped without the checkout) renders each file from its layers and compares the
result with what Photoshop shows for it: the largest channel difference, the share of pixels more than 2 levels off
and the mean. Where Patchy keeps Photoshop's own flatten of the file beside it (a `.bmp` of the same name, 48 files)
that is the reference, compared in RGB with NekoPhoto's render matted on white as Photoshop flattens; the merged image
stored inside a file can be stale (Patchy's notes: Photoshop saving headless). Otherwise the reference is the merged
image Photoshop stored in the file ("Maximize Compatibility", 24 more files), compared premultiplied. Each line of
the run says which reference it used; the other 45 files have neither. `tests/psd_oracle.txt` holds the floor of files
within tolerance (it may only rise) and `tests/patchy-manifest.txt` pins the Patchy commit and the SHA-256 of each
fixture and flatten; a checkout that differs is reported, not failed.

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
  satin, colour, gradient and pattern overlays, strokes (several per layer), knockout, blend-interior options,
  drawn as Photoshop draws them and written back as its bytes; gradients in Photoshop's Perceptual, Linear and
  Classic methods ([layer-styles.md](layer-styles.md#gradient-methods)).
- **Blend If**: This Layer and Underlying Layer ranges per channel with split points, on layers, adjustment layers
  and folders, drawn at every depth and edited in Blending Options; unedited ranges go back byte for byte
  ([layer-styles.md](layer-styles.md#blend-if)).
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

Collected from the pages above; each is also listed before you export or in the export's notes. Features that
deliberately work differently from Photoshop (Quick Select, Content-Aware Fill, healing, brush textures, Smudge,
some G'MIC filters) are listed in [legal-boundaries.md](legal-boundaries.md).

- **Not verified in Photoshop.** The files match what Photoshop wrote, structure for structure, but they have
  not been opened in Photoshop itself, so a warning on open or a re-layout of our type layers there is not ruled
  out.
- **Export is RGB, at 8 or 16 bits per channel.** A 16-bit RGB or grayscale file opens and exports at 16 bits
  ([bit-depth.md](bit-depth.md)); an unedited layer keeps its channel data byte for byte, an edited one (or a PSB) is
  written from NekoPhoto's 0..32768, dropping the file's lowest bit. A 32-bit, CMYK or Lab file opens converted to
  8-bit RGB and exports as that, without its original colour profile. 16-bit and PSB Smart Filter caches are left for
  Photoshop to rebuild. No Photoshop-saved 16-bit file is in the corpus: the 16-bit round trip is checked on a file
  built the way Photoshop lays one out, and on the corpus converted to 16 bits.
- **Written as pixels**: scaled, rotated or flipped layers are resampled into place; shape layers made in
  NekoPhoto; flipped text; adjustments Photoshop has no equivalent for (Grain, a Gradient Map made or edited here, Hue/Saturation on
  the plain scale) become a pixel layer of their result; a layer clipped to one not directly beneath it is
  written unclipped.
- **Folder opacity and blend modes** set in NekoPhoto are written as set but shown differently than in Photoshop.
- **Not carried**: resolution (ours is written), thumbnails, the ID seed; guides, slices and paths once the canvas
  size changes; a live shape's origination once it moves.
- **Layer styles not drawn yet**: contours on shadows and glows (drawn linear), noise and jitter, Dissolve (drawn
  as Normal), "Layer Mask Hides Effects". An Outer Bevel's texture is 5.2 levels off on average.
- **Vector masks**: shape feather is 5.4 levels off (Photoshop feathers the shape as one render); gradient and
  pattern strokes are not drawn.
- **Smart objects**: no dragging a Smart Filter between smart objects, no editing a stack that holds a filter not
  drawn here, no linked filter mask, no relinking of linked files.

How close the drawing is, as a mean difference per pixel on a 0-255 scale against Photoshop's own renders of the
fixtures: most layer styles 0.00-0.9, vector masks and shapes 0.00-1.3, styled folders 0.00-2.6, Blend If 0.52
(within 2 levels), Perceptual and Linear gradients 0.57 and 0.08 (against ag-psd's Photoshop-saved files)
([layer-styles.md](layer-styles.md#how-close), [vector-masks.md](vector-masks.md#how-close)).
Blend If in CMYK and Lab, and Perceptual and Linear gradients between CMYK inks, have no Photoshop-saved file to
check against.

## Continuous integration

Every push and pull request runs:

| Job | Runner | What it does |
|---|---|---|
| GCC | Ubuntu 24.04 | `-Werror` build, all CTest suites (unit, golden, render hashes, hostile input, translations), offscreen smoke test, automation socket (`tools/rpc_smoke.py`) and MCP bridge (`tools/mcp_smoke.py`) smoke tests, the automation panic hunt (`tools/rpc_panic_hunt.py`: hostile parameters for every method; no crash, hang or malformed reply) and crash recovery (`tools/recovery_check.py`) |
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
| The 4000 × 3000 document at 16 bits / reduced for the screen | 230 ms / 262 ms |

Application-level timings (opening a 70 MB PSD in 0.7 s and so on) are in the [README](../README.md#performance).

---

## 日本語

[English](#compatibility--correctness) · **日本語**

NekoPhoto が何をどのくらいの頻度で検証しているか、そしてまだ足りない点をまとめたページです。意図的に Photoshop と異なる
動作をする機能(クイック選択、コンテンツに応じた塗りつぶし、修復、ブラシのテクスチャ、指先ツール、一部の G'MIC フィルター)は
[legal-boundaries.md](legal-boundaries.md#日本語) にまとめています。数値は 2026-09-27 に
NekoPhoto 1.6.1 でツールを実行して集計したものです。

- **PSD の往復**: [Patchy](https://github.com/SethRobinson/Patchy) の MIT ライセンスのテストファイル 117 個(2 個を除き
  Photoshop 2026 で保存)すべてが合格し、3,675 個のブロックがバイト単位で変化なく戻りました。テキストレイヤー 20 個は編集可能なテキストとして開きます。
<!-- BEGIN GENERATED at-a-glance-ja: tools/compat_table.py --write; do not edit by hand -->
- **Photoshop の表示との比較**: Patchy のファイル 72 個(うち 48 個はファイルに添えられた Photoshop 自身の統合結果、残りはファイル内の統合画像と比較)のうち **52 個**が、99% のピクセルで 2 レベル以内・平均 1 レベル未満(下限は `tests/psd_oracle.txt`、下げることはできません)。
- **描画のハッシュ**: 697 シーン。**ゴールデン画像**: 6 テスト・参照 PNG 21 枚。**ブラシの基準値**: 526 行。**テストスイート**: CTest 89 個(725 `TEST_CASE`)。
- **自動操作**: メソッド 183 個、うち 182 個を `tools/rpc_smoke.py` で呼び出し、すべてに `tools/rpc_panic_hunt.py` が不正な引数を送ります。**ファジング**: libFuzzer のターゲット 11 個。
<!-- END GENERATED at-a-glance-ja -->
- **対応している PSD の要素**: レイヤーとグループ、描画モード、マスク(レイヤーマスク・ベクターマスク・両方・濃度とぼかし)、
  クリッピング、調整レイヤー、レイヤースタイル、ブレンド条件(このレイヤー・下になっているレイヤー、チャンネルごと、分割した
  スライダー。Photoshop の描画との差は 2 レベル以内)、グラデーションの方法(知覚的・リニア・クラシック)、シェイプ、
  編集可能なテキスト、スマートオブジェクトとスマートフィルター、PSB。
- **16 bit**: 16 bit の PSD は 16 bit のまま開いて書き出し、編集していないレイヤーのチャンネルデータはバイト単位で戻ります。
  上のテストファイルを 16 bit に変換して書き出しても、118 個すべてで引き継いだブロックが戻ります([bit-depth.md](bit-depth.md))。
- **カラープロファイル**: テストファイルのうちプロファイル付きの RGB の PSD 64 個すべてで、ICC プロファイル(リソース 1039)が
  バイト単位でそのまま戻ります。変換は Little CMS と sRGB・Adobe RGB の公開された行列に一致します([color-management.md](color-management.md))。
- **既知の差異**: Photoshop 本体で開いての確認はまだです。書き出しは RGB(8 bit/チャンネルまたは 16 bit/チャンネル)。変形したレイヤーや Photoshop に
  相当するもののない調整はピクセルとして書き出されます。一部のレイヤースタイル(シャドウ・光彩の輪郭、ノイズ、ディザ合成)は
  まだ描画されません。
- **CI**: GCC と Clang(Ubuntu 24.04、`-Werror`)、Windows(MSYS2 の MinGW-w64)でビルドとテスト、画面なしのスモークテスト、
  自動操作ソケットと MCP のテストを実行します。リリースでは Ubuntu 22.04 で AppImage を作って起動を確認します。
  悪意あるファイルのテストは毎回実行し、ファジングはローカルで行います([fuzzing.md](fuzzing.md)、英語)。
- **ベンチマーク**: AMD Ryzen AI 9 HX 370 で 4000 × 3000・12 レイヤーの描画が 207 ミリ秒など([benchmarks.md](benchmarks.md)、英語)。
