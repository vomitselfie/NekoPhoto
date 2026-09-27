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
  folders (Pass Through and isolated, faded by their opacity), artboards, shape layers and Dissolve. Layer styles
  and adjustment layers are shown too; they are worked out at 8 bits for now (the pixels they do not touch keep
  their 16 bits).
- **Layers**: new layers and folders, delete, duplicate, group, rename, reorder, move in and out of folders,
  visibility, opacity, blend mode, clipping, and the resampling mode.
- **Moving and transforming whole layers**: the Move tool, Free Transform (move, scale, rotate), and Flip Layer.
  No pixels are resampled: a layer keeps its pixels and their placement.
- **Layer masks**: Reveal All, Hide All, enable and disable, link, invert and delete.
- **Canvas**: Canvas Size and Flip Canvas.
- **Importing** an image as a layer (it takes the document's depth).
- **Saving and exporting**: projects at 16 bits; Photoshop PSD at 16 bits; PNG at 16 bits; TIFF at 16 bits (through
  Qt's TIFF plugin, which writes 16 bits); JPEG, WebP, TGA, ICO and GIF are 8-bit formats, so they get the document
  dithered down to 8 bits, and the status bar says so.
- **Automation**: `image.mode` converts; `document.info` reports `bits`
  ([automation.md](automation.md)).

A 16-bit PSD that NekoPhoto opened and exports again as PSD keeps each unedited layer's channel data byte for byte,
so a round trip does not lose Photoshop's full 16 bits; a layer you edit (and a PSB) is written from its 0..32768
values, which drops the lowest of the file's 16 bits. PNG and TIFF are always written from 0..32768.

## Not yet: greyed out in a 16-bit document

What has not been ported is greyed out, with the tooltip "Not available in 16-bit yet", and automation answers
"<method> is not available for 16-bit documents yet". For now, convert to 8 bits for these:

- painting and retouching tools (brushes, eraser, healing, clone, smudge, dodge and burn, gradient, paint bucket),
  and the shape and text tools;
- selections and their tools (marquee, lasso, magic wand, Quick Select, Select menu, Quick Mask);
- adjustments (Image ▸ Adjustments and new adjustment layers) and filters, Camera Raw and G'MIC;
- cut, copy and paste, fill and clear of pixels, content-aware tools, Warp and distort;
- Image Size, Crop and Trim;
- layer styles, smart objects and Smart Filters, vector masks and paths, artboards, and the timeline;
- SVG export, and exporting artboards and slices.

The next phase ports painting, adjustments, filters, transforms that resample, fills, gradients and selections to
16 bits; colour management, 32-bit float, channels, CMYK and Lab follow ([high-bit-depth-plan.md](high-bit-depth-plan.md),
section 9).

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
  マスク、グループ(通過と分離、不透明度)、アートボード、シェイプレイヤー、ディザ合成。レイヤースタイルと調整
  レイヤーも表示されます(当面は 8 bit で計算し、影響しないピクセルは 16 bit のまま)。
- **レイヤー**:新規レイヤー・グループ、削除、複製、グループ化、名前の変更、重ね順、グループへの出し入れ、
  表示/非表示、不透明度、描画モード、クリッピング、補間方法。
- **レイヤー全体の移動と変形**:移動ツール、自由変形(移動・拡大縮小・回転)、レイヤーの反転。ピクセルは再サンプル
  しません。
- **レイヤーマスク**:すべての領域を表示/隠す、有効/無効、リンク、反転、削除。
- **カンバス**:カンバスサイズ、カンバスの反転。
- **画像の読み込み**(ドキュメントのビット数に合わせます)。
- **保存と書き出し**:16 bit のプロジェクト、16 bit の PSD、16 bit の PNG、16 bit の TIFF(Qt の TIFF プラグイン経由)。
  JPEG・WebP・TGA・ICO・GIF は 8 bit の形式なので、ディザをかけて 8 bit に変換し、ステータスバーでお知らせします。
- **自動化**:`image.mode` で変換、`document.info` の `bits` でビット数がわかります。

NekoPhoto で開いた 16 bit の PSD を PSD に書き出すと、編集していないレイヤーのチャンネルデータはバイト単位でそのまま
戻るので、Photoshop の 16 bit の値は失われません。編集したレイヤー(と PSB)は 0〜32768 の値から書き出すため、
ファイルの 16 bit のうち最下位の 1 bit が落ちます。PNG と TIFF は常に 0〜32768 の値から書き出します。

### まだ使えないもの(16 bit のドキュメントではグレー表示)

移植していない機能はグレー表示になり、ツールチップに「16 bit/チャンネルではまだ使用できません」と表示されます。
自動化では「<メソッド> is not available for 16-bit documents yet」が返ります。当面は 8 bit に変換して使ってください。

- ペイントとレタッチのツール(ブラシ、消しゴム、修復、コピースタンプ、指先、覆い焼きと焼き込み、グラデーション、
  塗りつぶし)、シェイプとテキストのツール
- 選択範囲とそのツール(長方形選択、なげなわ、自動選択、クイック選択、選択範囲メニュー、クイックマスク)
- 色調補正(イメージ ▸ 色調補正、新規調整レイヤー)とフィルター、Camera Raw、G'MIC
- ピクセルのカット・コピー・ペースト、塗りつぶしと消去、コンテンツに応じたツール、ワープと自由な形に変形
- 画像解像度、切り抜き、トリミング
- レイヤースタイル、スマートオブジェクトとスマートフィルター、ベクトルマスクとパス、アートボード、タイムライン
- SVG の書き出し、アートボードとスライスの書き出し

次の段階でペイント、色調補正、フィルター、再サンプルする変形、塗りつぶし、グラデーション、選択範囲を 16 bit に
移植します。その後にカラーマネジメント、32 bit 浮動小数点、チャンネル、CMYK と Lab が続きます。

### メモリ

サイズの上限はメモリの上限なので、16 bit のドキュメントが持てるピクセル数は 8 bit の半分です:画像・レイヤー・
マスク 1 枚あたり 5,000 万画素(8 bit は 1 億)、レイヤー合計 5 億画素(8 bit は 10 億)。収まらないドキュメントの
変換は理由を添えて中止します。
