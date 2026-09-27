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
  folders (Pass Through and isolated, faded by their opacity), artboards, shape layers, Dissolve and adjustment
  layers. Layer styles are shown too; they are worked out at 8 bits for now (the pixels they do not touch keep
  their 16 bits).
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
- **Image Size** (each resampling method), **Crop**, the Crop tool, Crop to Selection and **Trim**; **Canvas Size**
  and Flip Canvas.
- **Layers**: new layers and folders, delete, duplicate, group, rename, reorder, move in and out of folders,
  visibility, opacity, blend mode, clipping, and the resampling mode.
- **Transforming layers**: the Move tool, Free Transform (move, scale, rotate), Distort and Perspective, Edit ▸ Warp,
  Warp Cage, and Flip Layer. A warp bends the 16-bit pixels; the cage previews from an 8-bit copy while you drag.
- **Layer masks**: Reveal All, Hide All, enable and disable, link, invert and delete.
- **Importing** an image as a layer (it takes the document's depth).
- **Saving and exporting**: projects at 16 bits; Photoshop PSD at 16 bits; PNG at 16 bits; TIFF at 16 bits (through
  Qt's TIFF plugin, which writes 16 bits); JPEG, WebP, TGA, ICO and GIF are 8-bit formats, so they get the document
  dithered down to 8 bits, and the status bar says so.
- **Automation**: `image.mode` converts; `document.info` reports `bits`; the selection, `pixels.adjust`,
  `pixels.filter`, `pixels.fill`, `pixels.clear`, the content-aware methods, `image.resize`, `image.trim`,
  `canvas.crop`, `layers.warp` and `layers.setCage` work on a 16-bit document ([automation.md](automation.md)).

A 16-bit PSD that NekoPhoto opened and exports again as PSD keeps each unedited layer's channel data byte for byte,
so a round trip does not lose Photoshop's full 16 bits; a layer you edit (and a PSB) is written from its 0..32768
values, which drops the lowest of the file's 16 bits. PNG and TIFF are always written from 0..32768.

## Not yet: greyed out in a 16-bit document

What has not been ported is greyed out, with the tooltip "Not available in 16-bit yet", and automation answers
"<method> is not available for 16-bit documents yet". For now, convert to 8 bits for these:

- painting and retouching tools (brushes, eraser, healing and the Patch tool, clone, smudge, blur and sharpen, dodge
  and burn, gradient, paint bucket), moving selected pixels with the Move tool, and the shape and text tools;
- Camera Raw Filter, G'MIC and Remove Background;
- Merge Down and Merge Visible, Apply layer mask, and baking a clipping mask into pixels;
- layer styles, smart objects and Smart Filters, vector masks and paths, artboards, and the timeline;
- SVG export, and exporting artboards and slices.

Colour management works at both depths: a 16-bit document keeps its profile, converts with Convert to Profile at
16 bits, and is shown through the monitor profile in the same pass that reduces it to the screen's 8 bits
([color-management.md](color-management.md)).

Painting comes next, on the new brush engine; 32-bit float, channels, CMYK and Lab follow
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
  マスク、グループ(通過と分離、不透明度)、アートボード、シェイプレイヤー、ディザ合成、調整レイヤー。レイヤー
  スタイルも表示されます(当面は 8 bit で計算し、影響しないピクセルは 16 bit のまま)。
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
- **画像解像度**(各補間方法)、**切り抜き**、切り抜きツール、選択範囲で切り抜き、**トリミング**。**カンバス
  サイズ**、カンバスの反転。
- **レイヤー**:新規レイヤー・グループ、削除、複製、グループ化、名前の変更、重ね順、グループへの出し入れ、
  表示/非表示、不透明度、描画モード、クリッピング、補間方法。
- **レイヤーの変形**:移動ツール、自由変形(移動・拡大縮小・回転)、自由な形に・遠近法、編集 ▸ ワープ、ワープ
  ケージ、レイヤーの反転。ワープは 16 bit のピクセルを変形します(ケージのドラッグ中は 8 bit のコピーで
  プレビュー)。
- **レイヤーマスク**:すべての領域を表示/隠す、有効/無効、リンク、反転、削除。
- **画像の読み込み**(ドキュメントのビット数に合わせます)。
- **保存と書き出し**:16 bit のプロジェクト、16 bit の PSD、16 bit の PNG、16 bit の TIFF(Qt の TIFF プラグイン経由)。
  JPEG・WebP・TGA・ICO・GIF は 8 bit の形式なので、ディザをかけて 8 bit に変換し、ステータスバーでお知らせします。
- **自動化**:`image.mode` で変換、`document.info` の `bits` でビット数がわかります。選択範囲、`pixels.adjust`、
  `pixels.filter`、`pixels.fill`、`pixels.clear`、コンテンツに応じた各メソッド、`image.resize`、`image.trim`、
  `canvas.crop`、`layers.warp`、`layers.setCage` も 16 bit のドキュメントで使えます。

NekoPhoto で開いた 16 bit の PSD を PSD に書き出すと、編集していないレイヤーのチャンネルデータはバイト単位でそのまま
戻るので、Photoshop の 16 bit の値は失われません。編集したレイヤー(と PSB)は 0〜32768 の値から書き出すため、
ファイルの 16 bit のうち最下位の 1 bit が落ちます。PNG と TIFF は常に 0〜32768 の値から書き出します。

### まだ使えないもの(16 bit のドキュメントではグレー表示)

移植していない機能はグレー表示になり、ツールチップに「16 bit/チャンネルではまだ使用できません」と表示されます。
自動化では「<メソッド> is not available for 16-bit documents yet」が返ります。当面は 8 bit に変換して使ってください。

- ペイントとレタッチのツール(ブラシ、消しゴム、修復とパッチツール、コピースタンプ、指先、ぼかしとシャープ、
  覆い焼きと焼き込み、グラデーション、塗りつぶしツール)、移動ツールでの選択したピクセルの移動、シェイプと
  テキストのツール
- Camera Raw フィルター、G'MIC、背景を削除
- 下のレイヤーと結合・表示レイヤーを結合、レイヤーマスクを適用、クリッピングマスクのピクセルへの焼き込み
- レイヤースタイル、スマートオブジェクトとスマートフィルター、ベクトルマスクとパス、アートボード、タイムライン
- SVG の書き出し、アートボードとスライスの書き出し

カラーマネジメントはどちらのビット数でも使えます。16 bit のドキュメントもプロファイルを持ち、プロファイル変換は 16 bit の
まま行い、画面の 8 bit への変換と同じ処理でモニタープロファイルを通して表示します([color-management.md](color-management.md))。

次はペイントを新しいブラシエンジンで移植します。その後に 32 bit 浮動小数点、チャンネル、CMYK と Lab が続きます。

### メモリ

サイズの上限はメモリの上限なので、16 bit のドキュメントが持てるピクセル数は 8 bit の半分です:画像・レイヤー・
マスク 1 枚あたり 5,000 万画素(8 bit は 1 億)、レイヤー合計 5 億画素(8 bit は 10 億)。収まらないドキュメントの
変換は理由を添えて中止します。
