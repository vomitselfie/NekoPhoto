# CMYK and Lab

**English** · [日本語](#日本語)

A document is in one colour mode, as in Photoshop: **RGB Color**, **CMYK Color** or **Lab Color**, at 8 or 16 bits
per channel (Photoshop has no 32-bit CMYK or Lab, and neither does NekoPhoto). The mode is what the pixels are: a
CMYK document holds cyan, magenta, yellow and black ink, a Lab document lightness and the a and b colour axes.
Profiles and the display are in [color-management.md](color-management.md); the design is in
[high-bit-depth-plan.md](high-bit-depth-plan.md), "P7 plan: CMYK and Lab".

## Image ▸ Mode

**Image ▸ Mode ▸ RGB Color, CMYK Color, Lab Color** convert the document, as one undo step:

- Every layer's pixels go from the document's profile to the new mode's: the **Working CMYK** (Edit ▸ Color
  Settings) for CMYK, **Lab D50** for Lab, the **working space** for RGB. The rendering intent and black point
  compensation are Color Settings' **Conversion Options** (Relative Colorimetric with black point compensation, as
  Photoshop's defaults are). Black generation (how much black ink replaces cyan, magenta and yellow) is the CMYK
  profile's.
- Colours the document keeps as values are converted too: text, shape fills and strokes, artboard backgrounds, layer
  style colours and gradients, Photo Filter and Gradient Map colours, and the **foreground and background colours**.
  They stay RGB values (sRGB in a CMYK or Lab document); converting to CMYK brings them into the press's gamut.
- **Levels and Curves lose their per-channel settings** (red, green and blue mean something else in CMYK and Lab);
  the composite settings stay.
- An **adjustment layer the new mode does not offer** (Vibrance and Black & White in CMYK and Lab; Hue/Saturation,
  Color Balance and Selective Color in Lab; Exposure in CMYK) is **kept, hidden and marked**; converting back to a mode
  that offers it shows it again as it was.
- Masks, alpha and spot channels and the selection are not colour and stay as they are.
- A document that would not fit the byte budget in the new mode (a CMYK layer holds four fifths the pixels of an RGB
  one) is refused with the reason; a 32-bit document converts to 8 or 16 bits first.

Undo returns the document exactly as it was. Converting RGB to CMYK and back does not return the same colours:
colours outside the press's gamut stay mapped into it, as in Photoshop. RGB to Lab and back at 16 bits returns the
values of an 8-bit source exactly; at 8 bits, 8-bit Lab's steps move some saturated colours near the edge of sRGB by
a few levels.

## What a CMYK or Lab document draws

- Pixel layers with opacity, pixel and vector masks, clipping masks, folders (Pass Through and isolated), artboards,
  vector shapes' strokes, gradient and pattern fill layers, and hidden layers, at 8 and 16 bits.
- **Blend modes.** CMYK offers every mode, as Photoshop does. The separable ones work on each ink as stored
  (inverted, so Multiply adds ink the way it darkens in RGB), with the same arithmetic as RGB's. Hue, Saturation,
  Color, Luminosity, Darker Color and Lighter Color need Photoshop renders of CMYK files to calibrate against, which
  NekoPhoto does not have yet: until then they **draw as Normal**, and the Layers panel says so. Lab offers every mode
  but Color Dodge, Color Burn, Darken, Lighten, Difference, Exclusion, Subtract and Divide (Adobe's list; they are greyed
  out, and a file that has one draws it as Normal). Normal and Dissolve are RGB's; Luminosity takes L from the layer
  and a and b from below, Color the other way round, Hue and Saturation work in L, chroma and hue; the separable modes
  apply to L, a and b as stored.
- **Adjustment layers**: Invert, Levels, Curves (their composite settings), Brightness/Contrast and Posterize draw, on
  every ink in CMYK and on L in Lab (Invert and Posterize on every channel). The other kinds are kept and written back
  to PSD but are not drawn yet.
- **Layer styles** are kept and written back but not drawn in CMYK and Lab yet.
- The canvas always goes through a colour transform: the document's profile to the monitor profile, or to sRGB when
  none is known, in the same pass that reduces the frame to 8 bits. Layer thumbnails are drawn the same way.

## Channels

The Channels panel lists the mode's colour channels: **CMYK, Cyan, Magenta, Yellow, Black** (Ctrl+2, then Ctrl+3 to
Ctrl+6; alpha channels from Ctrl+7) or **Lab, Lightness, a, b** (Ctrl+2 to Ctrl+5). One channel alone shows in gray
(a CMYK plate with its ink dark, as Photoshop shows it; a and b gray where neutral); several CMYK inks show as inks on
white; several Lab channels show their colour with the hidden ones neutral. Ctrl-click (or Alt+Ctrl+number) loads a
CMYK channel as a selection of its ink, a Lab channel as its value. With some colour channels the target, **Fill**
(Edit ▸ Fill, Alt+Backspace, Ctrl+Backspace) changes only them; the brushes and the other editing tools come with the
editing ports (step E).

## Photoshop files

- CMYK (mode 4) and Lab (mode 9) PSDs **open in their own mode** at 8 and 16 bits: the layers' planes as stored, the
  CMYK profile (resource 1039) as the document's, alpha and spot channels. Duotone, Multichannel, Indexed, Bitmap and
  Grayscale files are converted to RGB, as before.
- Saving writes the document's mode, depth and profile. A layer that has not changed goes back **byte for byte**
  (its channels as they were stored), and so do the adjustment blocks and everything else the file held that
  NekoPhoto does not model.
- Checked against: Patchy's `photoshop-cmyk-style-colors.psd` (the one Photoshop-saved CMYK file available) opens as
  CMYK and comes back byte for byte; constructed CMYK and Lab files at 8 and 16 bits (layers, a mask, an adjustment
  layer, a spot channel; `tests/psd_modes_tests.cpp`) are read back by an independent decoder. Constructed files are a
  weaker check than Photoshop's own; Photoshop-saved CMYK and Lab files are welcome.

## Not yet

Painting, the retouching tools, most adjustments and filters, transforms of pixels, text and shapes as editable
objects, and layer styles in CMYK and Lab (greyed out with "Not available in CMYK mode"); exporting CMYK or Lab to
PNG, JPEG, TIFF and the other formats (projects and PSD save them); CMYK JPEG and TIFF; CMYK's non-separable blend
modes. Camera Raw, G'MIC and the MyPaint brushes stay RGB only.

## Automation

`image.mode` takes `colorMode` (`rgb`, `cmyk`, `lab`) with or without `bits`; `document.info` reports `colorMode`;
`color.settings` reads and sets the Conversion Options (`intent`, `blackPointCompensation`); `channels.*` name the
colour channels `cyan` ... `black` and `lightness`, `a`, `b`, and the composite `cmyk` or `lab`
([automation.md](automation.md)).

## For developers

- `convertDocumentMode` and `convertModeColor` (`colormgmt.h`, `colormode_convert.cpp`); `adjustmentOfferedInMode`,
  `isDormantAdjustment` (the marker is a `dormantInMode` key in the adjustment's settings); `modeThumbnail`.
- Rendering: `render_exec_deep.inc` is written once over a policy with a channel count; `ModeOps<S, M>`
  (`render_modes.h`) is the CMYK and Lab policy, instantiated in `render_exec_c8.cpp` (8-bit CMYK), `render_exec_lab8.cpp`
  and `render_exec_modes16.cpp`; the RGB executors are untouched. The blend kernels are `blend_c8.cpp` and
  `blend_modes16.cpp` over `blend_modes.inc`; `blendModeFor` holds the calibration hook for CMYK's non-separable modes.
  `renderNative` renders at the document's layout.
- PSD: `assembleMode8/16` in `psd.cpp`, `setPixelsMode` and `writeMergedMode` in `psd_writer.cpp`; the plane carry is
  `PsdLayerCarry::planes`.
- Tests: `colormodes_render_tests`, `psd_modes_tests`, CMYK and Lab scenes in `render_hash_tests`, and
  `psd_roundtrip` (CMYK and Lab files must reopen in their mode with every layer channel byte for byte).

## 日本語

ドキュメントのカラーモードは Photoshop と同じく 1 つです:**RGB カラー**、**CMYK カラー**、**Lab カラー**。それぞれ
8 bit または 16 bit/チャンネルです(Photoshop と同じく 32 bit の CMYK・Lab はありません)。

### イメージ ▸ モード

**イメージ ▸ モード ▸ RGB カラー、CMYK カラー、Lab カラー** はドキュメントを変換します(1 回の取り消しで元に戻ります)。

- すべてのレイヤーのピクセルを、CMYK は**作業用 CMYK**、Lab は **Lab D50**、RGB は**作業用スペース**に変換します。
  マッチング方法と黒点の補正は、カラー設定の**変換オプション**(既定は Photoshop と同じ「相対的な色域を維持」と黒点の
  補正)を使います。墨版の生成は CMYK プロファイルに従います。
- 値として持つ色(テキスト、シェイプの塗りと線、アートボードの背景、レイヤースタイルの色とグラデーション、レンズ
  フィルターとグラデーションマップの色、**描画色と背景色**)も変換します。CMYK への変換では印刷の色域に収まります。
- **レベル補正とトーンカーブはチャンネルごとの設定がリセット**されます(複合チャンネルの設定は残ります)。
- **新しいモードにない調整レイヤー**(CMYK と Lab の自然な彩度・白黒、Lab の色相・彩度・カラーバランス・特定色域の
  選択、CMYK の露光量)は**非表示にして印を付けて残し**、そのレイヤーがあるモードに戻すと元どおり表示されます。
- マスク、アルファチャンネル、スポットカラーチャンネル、選択範囲は変わりません。

### CMYK・Lab ドキュメントの表示

- ピクセルレイヤー、不透明度、ピクセルマスクとベクトルマスク、クリッピングマスク、グループ、アートボード、
  グラデーション・パターンの塗りつぶしレイヤーを 8/16 bit で合成します。
- **描画モード**:CMYK ではすべてのモードを使えます。分離可能なモードは各インキに RGB と同じ計算で適用します。
  色相・彩度・カラー・輝度・カラー比較(暗)・カラー比較(明)は Photoshop の CMYK の結果と照合できるまで**通常として
  描画**し、レイヤーパネルにその旨を表示します。Lab では覆い焼きカラー・焼き込みカラー・比較(暗)・比較(明)・
  差の絶対値・除外・減算・除算は使えません(Adobe の説明のとおり)。
- 調整レイヤーは階調の反転、レベル補正、トーンカーブ(複合チャンネル)、明るさ・コントラスト、ポスタリゼーションを
  描画します。そのほかの調整レイヤーとレイヤースタイルは保持して PSD に書き戻しますが、まだ描画しません。
- カンバスは常にドキュメントのプロファイルからモニタープロファイル(不明なら sRGB)へ変換して表示します。

### チャンネル

チャンネルパネルには **CMYK、シアン、マゼンタ、イエロー、ブラック**(Ctrl+2、Ctrl+3〜6)または **Lab、明度、a、b**
が並びます。1 つだけ表示するとグレー(CMYK の版はインキを暗く)で表示します。一部のカラーチャンネルを選んでいるとき、
**塗りつぶし**はそのチャンネルだけに適用されます。

### Photoshop ファイル

CMYK(モード 4)と Lab(モード 9)の PSD は 8/16 bit のまま**そのモードで開き**、CMYK のプロファイル(リソース 1039)も
読み込みます。保存するとドキュメントのモード・ビット数・プロファイルで書き出し、変更していないレイヤーは**バイト単位で
そのまま**戻ります。ダブルトーン、マルチチャンネル、インデックスカラー、モノクロ 2 階調、グレースケールは従来どおり
RGB に変換します。

### 未対応

CMYK・Lab でのペイント、レタッチ、多くの色調補正とフィルター、変形、編集可能なテキストとシェイプ、レイヤースタイルの
描画(「CMYK モードでは使用できません」と表示)、PNG・JPEG・TIFF などへの書き出し、CMYK の JPEG と TIFF、CMYK の
分離不可能な描画モード。
