# 16 bits per channel

**English** · [日本語](#日本語)

A document is 8 or 16 bits per channel, as in Photoshop: every layer, mask and the selection have the same depth.
16 bits keep smooth gradients smooth through blending and masking, where 8 bits can band. Image ▸ Mode ▸
8 Bits/Channel and 16 Bits/Channel switch a document between the two, as one undo step.

This page says what works in a 16-bit document today and what is still to come. The design is in
[high-bit-depth-plan.md](high-bit-depth-plan.md).

## Getting a 16-bit document

- **Image ▸ Mode ▸ 16 Bits/Channel** converts the open document. Converting an 8-bit document is exact: back to
  8 bits, every pixel is what it was.
- **Opening a 16-bit file**: a 16-bit RGB or grayscale Photoshop file (PSD, PSB), a 16-bit PNG, or a 16-bit TIFF
  opens as a 16-bit document. Other deep files (32-bit, CMYK and Lab) are still converted to 8 bits, with a note.
- **Projects** keep their depth: a 16-bit project stores its layers and masks as 16-bit PNGs (project format
  version 8, [project-format.md](project-format.md)). Older projects open as 8-bit.

Like Photoshop, NekoPhoto holds 16-bit samples in the range 0 to 32768, so blending is exact integer maths.
Files store 0 to 65535; the values are mapped when a file is read and written.

## What works at 16 bits now

- **Rendering**: every blend mode, opacity, layer and folder masks (at 16 bits), vector masks, clipping masks,
  folders (Pass Through and isolated, faded by their opacity), artboards, shape layers and fill layers, Dissolve,
  adjustment layers and layer styles. Vector masks and shapes are rasterised with 32,768 steps of edge coverage, and
  gradient fills are drawn from the gradient's exact colours, so a long ramp has no 8-bit steps.
- **Adjustments**, on pixels (Image ▸ Adjustments, Invert) and as adjustment layers: Levels, Curves, Hue/Saturation,
  Exposure, Gradient Map, Grain, Brightness/Contrast, Vibrance, Color Balance, Black & White, Photo Filter, Channel
  Mixer, Selective Color, Posterize, Threshold, Color Lookup and Invert. Each works on the exact colour, so a smooth
  16-bit gradient stays smooth through a strong Levels or Curves. Posterize and Threshold choose their steps as the
  8-bit ones do; the Levels histogram has 256 bins at either depth.
- **Filters**: Gaussian Blur, Motion Blur, Add Noise (the same pattern for the same seed) and Lens Correction.
- **Selections**: the Rectangular and Elliptical Marquee, Lasso and Polygonal Lasso, Magic Wand (with Refine Edge and
  the smart wand) and Quick Select tools; Select All, Deselect, Inverse, Expand, Contract, Feather, Smooth and Border;
  Load as Selection from a layer or its mask; Edit in Quick Mask Mode; moving the selection outline; Add Mask from
  Selection. The selection itself is 16-bit, so a feather has 32,768 steps instead of 256. The tools that read the
  image (Magic Wand, Quick Select, Select Subject) read it as the canvas shows it: Tolerance counts 8-bit levels, as
  in Photoshop.
- **Editing pixels**: Fill (with the foreground or background colour) and Clear through the selection; Cut, Copy,
  Copy Merged, Paste and Layer via Copy (other apps get 8 bits on the clipboard; pixels pasted from a document of the
  other depth are converted); Content-Aware Fill, Content-Aware Scale, and Content-Aware Move through automation
  (their patch search and seams are chosen on the pixels rounded to 8 bits, and the pixels they copy are 16-bit);
  Free Transform of selected pixels.
- **Painting and retouching**: the Brush in every engine (the round tip, imported tip brushes and the MyPaint
  presets) and the Eraser, on pixels, on layer masks and in Quick Mask; Clone Stamp and the Healing Brush (they copy
  16-bit pixels), Spot Healing and the Patch tool; Blur, Sharpen, Smudge and Liquify; Dodge, Burn and Sponge; the
  Gradient tool and the Paint Bucket; and moving or duplicating selected pixels with the Move tool. A stroke builds up
  at 16 bits, so a soft brush's edge or a long gradient has thousands of steps where 8 bits have a few dozen. The tools
  that choose what to change read the image as the canvas shows it: the Paint Bucket's Tolerance counts 8-bit levels,
  and Spot Healing and Patch pick the patch they copy on the pixels rounded to 8 bits, as Content-Aware Fill does.
- **Image Size** (each resampling method), **Crop**, the Crop tool, Crop to Selection and **Trim**; **Canvas Size**
  and Flip Canvas.
- **Layers**: new layers and folders, delete, duplicate, group, rename, reorder, move in and out of folders,
  visibility, opacity, blend mode, clipping, the resampling mode, and Merge Down (merged at 16 bits). Deleting a
  clipping base keeps the clipped layers' look, baked into their pixels at 16 bits.
- **Text**: the Type tool, Edit Text, text in several styles, Warp Text, Create Work Path and Convert to Shape. The
  text is painted at 16 bits per channel (Qt's 16-bit raster; its glyph antialiasing has 256 steps, as at 8 bits).
  A 16-bit PSD's type layers open as text and export as Photoshop type layers.
- **Shapes and paths**: the Shape tools (live rectangles and ellipses, polygons, lines, custom shapes) with solid,
  gradient and pattern fills and strokes, path operations, the Pen and Direct Selection tools, the Paths panel, vector
  masks on any layer, Fill Path and Stroke Path (at 16 bits), and Make Work Path from a selection.
- **Layer styles**: the Layer Style dialog and all ten effects (Drop Shadow, Inner Shadow, Outer and Inner Glow,
  Bevel & Emboss, Satin, Color, Gradient and Pattern Overlay, Stroke), on layers and folders, drawn at 16 bits;
  Copy, Paste and Clear Layer Style and style presets. At 1:1 they are within a level of their 8-bit render.
- **Transforming layers**: the Move tool, Free Transform (move, scale, rotate), Distort and Perspective, Edit ▸ Warp,
  Warp Cage, and Flip Layer. A warp bends the 16-bit pixels; the cage previews from an 8-bit copy while you drag.
- **Layer masks**: Reveal All, Hide All, enable and disable, link, invert, apply and delete, and painting them.
- **Smart objects**: Place Embedded, Convert to Smart Object, Edit Contents, Replace Contents, Rasterize, moving,
  scaling, Edit ▸ Warp and the Warp Cage on them. A smart object's contents keep their own depth: an 8-bit PNG placed
  in a 16-bit document stays an 8-bit source (its instances are drawn at 16 bits), and a 16-bit PNG, TIFF or PSB placed
  in an 8-bit document stays 16-bit, so converting the document back to 16 bits later loses nothing. Convert to Smart
  Object in a 16-bit document makes a 16-bit PSB; Edit Contents opens the contents at their own depth.
- **Smart Filters**: every Smart Filter NekoPhoto draws, except Unsharp Mask (below), is drawn at 16 bits on the
  instance: Gaussian, Motion, Box, Surface and Radial Blur, High Pass, Median, Dust & Scratches, Add Noise, Mosaic,
  Emboss and Plastic Wrap, with their opacity, blend mode and the shared filter mask. On an 8-bit image each is
  within a level of its 8-bit result ([smart-objects.md](smart-objects.md) has the figures).
- **Importing** an image as a layer (it takes the document's depth).
- **Saving and exporting**: projects at 16 bits (a 16-bit smart object source is kept as a 16-bit PNG beside the
  project); Photoshop PSD at 16 bits, smart objects included; PNG at 16 bits; TIFF at 16 bits (through
  Qt's TIFF plugin, which writes 16 bits); JPEG, WebP, TGA, ICO and GIF are 8-bit formats, so they get the document
  dithered down to 8 bits, and the status bar says so.
- **Automation**: `image.mode` converts; `document.info` reports `bits`; the selection, `pixels.adjust`,
  `pixels.filter`, `pixels.fill`, `pixels.clear`, the content-aware methods, `image.resize`, `image.trim`,
  `canvas.crop`, `layers.warp`, `layers.setCage`, `brush.stroke` (every tool it takes), `gradient.draw`,
  `pixels.bucket`, `pixels.patch`, `layers.merge`, `text.*`, `shape.draw`, `shape.set`, `paths.*`, `vectorMask.*`,
  `layers.setStyle`, `layers.applyStyle` and every `smartObject.*` method work on a 16-bit document
  ([automation.md](automation.md)).

A 16-bit PSD that NekoPhoto opened and exports again as PSD keeps each unedited layer's channel data byte for byte,
so a round trip does not lose Photoshop's full 16 bits; a layer you edit (and a PSB) is written from its 0..32768
values, which drops the lowest of the file's 16 bits. PNG and TIFF are always written from 0..32768.

A 16-bit PSD does not get Photoshop's Smart Filter cache (it is 8-bit data; Photoshop rebuilds it). Opening a 16-bit
PSD that has none, NekoPhoto draws each Smart Filter stack over the whole canvas with its filter mask all white, so the
filters stay editable; a filter mask painted in NekoPhoto is not kept in a 16-bit PSD (it is in a project and an 8-bit
PSD).

## Not yet: greyed out in a 16-bit document

What has not been ported is greyed out, with the tooltip "Not available in 16-bit yet", and automation answers
"<method> is not available for 16-bit documents yet". For now, convert to 8 bits for these:

- Camera Raw Filter, G'MIC and Remove Background;
- Unsharp Mask as a Smart Filter ("Unsharp Mask is not available as a Smart Filter in 16-bit documents yet"): its
  16-bit version is two to four levels from the 8-bit one at usual amounts, more than the one level the others keep,
  so it waits for a calibration of its own. An instance that already has it shows the pixels it was saved with;
- artboards, and the timeline;
- SVG export, and exporting artboards and slices.

Colour management works at both depths: a 16-bit document keeps its profile, converts with Convert to Profile at
16 bits, and is shown through the monitor profile in the same pass that reduces it to the screen's 8 bits
([color-management.md](color-management.md)).

32-bit float, channels, CMYK and Lab follow
([high-bit-depth-plan.md](high-bit-depth-plan.md), section 9).

## Memory

The size limits are memory limits, so a 16-bit document holds half the pixels of an 8-bit one: up to 50 megapixels
per image, layer or mask (100 at 8 bits), and 500 megapixels of layers in all (1,000 at 8 bits). Converting a document
that would not fit is refused, with the reason.

---

## 日本語

[English](#16-bits-per-channel) · **日本語**

Photoshop と同じく、ドキュメントは 8 bit/チャンネルか 16 bit/チャンネルです。レイヤー・マスク・選択範囲はすべて
同じビット数を持ちます。16 bit では描画モードやマスクを重ねてもグラデーションが滑らかなままです(8 bit では
階調の段差が出ることがあります)。イメージ ▸ モード ▸ 8 bit/チャンネル、16 bit/チャンネル で切り替えます(取り消しは
1 回)。

### 16 bit のドキュメントを作るには

- **イメージ ▸ モード ▸ 16 bit/チャンネル** で開いているドキュメントを変換します。8 bit からの変換は正確で、8 bit に
  戻せばすべてのピクセルが元どおりです。
- **16 bit のファイルを開く**:16 bit の RGB/グレースケールの Photoshop ファイル(PSD、PSB)、16 bit の PNG、16 bit の
  TIFF は 16 bit のドキュメントとして開きます。それ以外の深いファイル(32 bit、CMYK、Lab)は注記つきで 8 bit に
  変換します。
- **プロジェクト**はビット数を保ちます(レイヤーとマスクを 16 bit の PNG で保存、プロジェクト形式バージョン 8)。
  以前のプロジェクトは 8 bit で開きます。

Photoshop と同じく 16 bit の値は 0〜32768 で保持し、合成は正確な整数演算です。ファイルは 0〜65535 で保存するため、
読み書きのときに変換します。

### 16 bit で使えるもの

- **表示**:すべての描画モード、不透明度、レイヤーマスクとグループのマスク(16 bit)、ベクトルマスク、クリッピング
  マスク、グループ(通過と分離、不透明度)、アートボード、シェイプレイヤーと塗りつぶしレイヤー、ディザ合成、調整
  レイヤー、レイヤースタイル。ベクトルマスクとシェイプの縁は 32,768 段階で描き、グラデーションの塗りは正確な色から
  描くので、長いグラデーションにも 8 bit の段差が出ません。
- **色調補正**(ピクセルへの適用:イメージ ▸ 色調補正と階調の反転、および調整レイヤー):レベル補正、トーンカーブ、
  色相・彩度、露光量、グラデーションマップ、粒子、明るさ・コントラスト、自然な彩度、カラーバランス、白黒、
  レンズフィルター、チャンネルミキサー、特定色域の選択、ポスタリゼーション、2 階調化、カラールックアップ、
  階調の反転。どれも正確な色で計算するので、滑らかな 16 bit のグラデーションは強いレベル補正やトーンカーブでも
  滑らかなままです。ポスタリゼーションと 2 階調化は 8 bit と同じ段階を選びます。レベル補正のヒストグラムは
  どちらのビット数でも 256 段階です。
- **フィルター**:ぼかし(ガウス)、ぼかし(移動)、ノイズを加える(同じシードで同じパターン)、レンズ補正。
- **選択範囲**:長方形選択・楕円形選択、なげなわ・多角形選択、自動選択(エッジの調整とスマート自動選択を含む)、
  クイック選択の各ツール。すべてを選択、選択を解除、選択範囲を反転、拡張、縮小、境界をぼかす、滑らかに、
  境界線。レイヤーやマスクから選択範囲を読み込む、クイックマスクモードで編集、選択範囲の枠の移動、選択範囲から
  マスクを追加。選択範囲そのものが 16 bit なので、ぼかしの段階は 256 ではなく 32,768 です。画像を読むツール
  (自動選択、クイック選択、被写体を選択)はカンバスの表示どおりに読みます(許容値は Photoshop と同じく 8 bit の
  段階)。
- **ピクセルの編集**:選択範囲を通した塗りつぶし(描画色・背景色)と消去。カット、コピー、結合部分をコピー、
  ペースト、選択範囲をコピーしたレイヤー(他のアプリのクリップボードには 8 bit。別のビット数のドキュメントから
  ペーストしたピクセルは変換します)。コンテンツに応じた塗りつぶし、コンテンツに応じて拡大・縮小、コンテンツに
  応じた移動(自動化から。パッチの探索とシームは 8 bit に丸めた画像で決め、コピーするピクセルは 16 bit)。選択した
  ピクセルの自由変形。
- **ペイントとレタッチ**:すべてのエンジンのブラシ(円形の先端、読み込んだ先端ブラシ、MyPaint のプリセット)と
  消しゴム(ピクセル、レイヤーマスク、クイックマスク)。コピースタンプと修復ブラシ(16 bit のピクセルをコピー)、
  スポット修復ブラシとパッチツール。ぼかし・シャープ・指先・ゆがみ。覆い焼き・焼き込み・スポンジ。グラデーション
  ツールと塗りつぶしツール。移動ツールでの選択したピクセルの移動と複製。ストロークは 16 bit で重なるので、ソフト
  ブラシの縁や長いグラデーションの階調は 8 bit の数十段階ではなく数千段階になります。変更する場所を選ぶツールは
  カンバスの表示どおりに画像を読みます(塗りつぶしツールの許容値は 8 bit の段階、スポット修復ブラシとパッチは
  コンテンツに応じた塗りつぶしと同じく 8 bit に丸めた画像でコピー元を選びます)。
- **画像解像度**(各補間方法)、**切り抜き**、切り抜きツール、選択範囲で切り抜き、**トリミング**。**カンバス
  サイズ**、カンバスの反転。
- **レイヤー**:新規レイヤー・グループ、削除、複製、グループ化、名前の変更、重ね順、グループへの出し入れ、
  表示/非表示、不透明度、描画モード、クリッピング、補間方法、下のレイヤーと結合(16 bit で結合)。クリッピングの
  ベースを削除するときは、クリップされたレイヤーの見た目を 16 bit でピクセルに焼き込みます。
- **テキスト**:文字ツール、テキストを編集、スタイルの混在したテキスト、ワープテキスト、作業用パスを作成、シェイプに
  変換。テキストは 16 bit/チャンネルで描画します(Qt の 16 bit 描画。文字のアンチエイリアスは 8 bit と同じ 256 段階)。
  16 bit の PSD のテキストレイヤーはテキストとして開き、Photoshop のテキストレイヤーとして書き出します。
- **シェイプとパス**:シェイプツール(ライブの長方形・楕円、多角形、ライン、カスタムシェイプ)、単色・グラデーション・
  パターンの塗りと線、パスの演算、ペンツールとパス選択ツール、パスパネル、あらゆるレイヤーのベクトルマスク、パスの
  塗りつぶしと境界線(16 bit)、選択範囲から作業用パス。
- **レイヤースタイル**:レイヤースタイルダイアログと 10 種類の効果(ドロップシャドウ、シャドウ(内側)、光彩(外側・
  内側)、ベベルとエンボス、サテン、カラー・グラデーション・パターンオーバーレイ、境界線)をレイヤーとグループに
  16 bit で描画。レイヤースタイルのコピー・ペースト・消去とスタイルのプリセット。100% 表示では 8 bit の描画と
  1 段階以内です。
- **レイヤーの変形**:移動ツール、自由変形(移動・拡大縮小・回転)、自由な形に・遠近法、編集 ▸ ワープ、ワープ
  ケージ、レイヤーの反転。ワープは 16 bit のピクセルを変形します(ケージのドラッグ中は 8 bit のコピーで
  プレビュー)。
- **レイヤーマスク**:すべての領域を表示/隠す、有効/無効、リンク、反転、適用、削除、マスクへのペイント。
- **スマートオブジェクト**:埋め込みを配置、スマートオブジェクトに変換、コンテンツを編集、内容を置き換え、ラスタライズ、
  移動・拡大縮小、編集 ▸ ワープとワープケージ。スマートオブジェクトの内容は元のビット数のままです。16 bit の
  ドキュメントに配置した 8 bit の PNG は 8 bit のソースのまま(インスタンスは 16 bit で描画)、8 bit のドキュメントに
  配置した 16 bit の PNG・TIFF・PSB は 16 bit のままなので、後でドキュメントを 16 bit に戻しても失われません。16 bit
  のドキュメントでスマートオブジェクトに変換すると 16 bit の PSB になり、コンテンツを編集は内容をそのビット数で開きます。
- **スマートフィルター**:NekoPhoto が描画するスマートフィルターは、アンシャープマスク(後述)を除いてすべて
  インスタンス上で 16 bit で描画します:ぼかし(ガウス)、ぼかし(移動)、ぼかし(ボックス)、ぼかし(表面)、ぼかし
  (放射状)、ハイパス、中間値、ダスト&スクラッチ、ノイズを加える、モザイク、エンボス、ラップ。不透明度、描画モード、
  共有のフィルターマスクも含みます。8 bit の画像ではそれぞれ 8 bit の結果と 1 段階以内です(数値は
  [smart-objects.md](smart-objects.md))。
- **画像の読み込み**(ドキュメントのビット数に合わせます)。
- **保存と書き出し**:16 bit のプロジェクト(16 bit のスマートオブジェクトのソースは 16 bit の PNG として保存)、
  スマートオブジェクトを含む 16 bit の PSD、16 bit の PNG、16 bit の TIFF(Qt の TIFF プラグイン経由)。
  JPEG・WebP・TGA・ICO・GIF は 8 bit の形式なので、ディザをかけて 8 bit に変換し、ステータスバーでお知らせします。
- **自動化**:`image.mode` で変換、`document.info` の `bits` でビット数がわかります。選択範囲、`pixels.adjust`、
  `pixels.filter`、`pixels.fill`、`pixels.clear`、コンテンツに応じた各メソッド、`image.resize`、`image.trim`、
  `canvas.crop`、`layers.warp`、`layers.setCage`、`brush.stroke`(指定できるすべてのツール)、`gradient.draw`、
  `pixels.bucket`、`pixels.patch`、`layers.merge`、すべての `smartObject.*` メソッドも 16 bit のドキュメントで
  使えます。

NekoPhoto で開いた 16 bit の PSD を PSD に書き出すと、編集していないレイヤーのチャンネルデータはバイト単位でそのまま
戻るので、Photoshop の 16 bit の値は失われません。編集したレイヤー(と PSB)は 0〜32768 の値から書き出すため、
ファイルの 16 bit のうち最下位の 1 bit が落ちます。PNG と TIFF は常に 0〜32768 の値から書き出します。

16 bit の PSD には Photoshop のスマートフィルターのキャッシュを書き出しません(8 bit のデータで、Photoshop が作り
直します)。キャッシュのない 16 bit の PSD を開くと、NekoPhoto は各スマートフィルターをカンバス全体に、フィルター
マスクをすべて白として描画するので、フィルターは編集できるままです。NekoPhoto で描いたフィルターマスクは 16 bit の
PSD には残りません(プロジェクトと 8 bit の PSD には残ります)。

### まだ使えないもの(16 bit のドキュメントではグレー表示)

移植していない機能はグレー表示になり、ツールチップに「16 bit/チャンネルではまだ使用できません」と表示されます。
自動化では「<メソッド> is not available for 16-bit documents yet」が返ります。当面は 8 bit に変換して使ってください。

- Camera Raw フィルター、G'MIC、背景を削除
- スマートフィルターとしてのアンシャープマスク(「Unsharp Mask is not available as a Smart Filter in 16-bit
  documents yet」):16 bit 版は通常の量で 8 bit 版と 2〜4 段階ずれ、ほかのフィルターが保つ 1 段階を超えるため、専用の
  調整を待ちます。すでに適用されているインスタンスは保存されたときのピクセルを表示します
- アートボード、タイムライン
- SVG の書き出し、アートボードとスライスの書き出し

カラーマネジメントはどちらのビット数でも使えます。16 bit のドキュメントもプロファイルを持ち、プロファイル変換は 16 bit の
まま行い、画面の 8 bit への変換と同じ処理でモニタープロファイルを通して表示します([color-management.md](color-management.md))。

この後に 32 bit 浮動小数点、チャンネル、CMYK と Lab が続きます。

### メモリ

サイズの上限はメモリの上限なので、16 bit のドキュメントが持てるピクセル数は 8 bit の半分です:画像・レイヤー・
マスク 1 枚あたり 5,000 万画素(8 bit は 1 億)、レイヤー合計 5 億画素(8 bit は 10 億)。収まらないドキュメントの
変換は理由を添えて中止します。
