# Colour management

**English** · [日本語](#日本語)

NekoPhoto manages colour the way Photoshop does: a document is in one colour profile, its pixels and colours are
values in that profile, and they are converted only at the edges: when a file opens, when you assign or convert a
profile, when the canvas is drawn on your monitor, when you proof, and when you export. Blending and every edit work
on the document's own values. The design is in [high-bit-depth-plan.md](high-bit-depth-plan.md), section 5.

With the defaults nothing changes from before: an untagged document is sRGB, and when no monitor profile is known
the canvas shows the document's values exactly as it always did.

## The document's profile

Each document has a profile, or none. **Untagged** documents (most images on the web, new documents while the
working space is sRGB) are treated as sRGB. A profile is kept as the ICC bytes it came with, so an unchanged profile
goes back out byte for byte. `document.info` and the Convert to Profile dialog show which one a document has.

Where the profile lives:

- **Photoshop files**: image resource 1039 (the ICC profile) is read into the document and written back unchanged;
  after Assign or Convert, the new profile is written in its place. An untagged document writes none.
- **Projects** keep it as `profile.icc` in the package, with `"colorSpace": "icc"` in the manifest (project format
  version 8; [project-format.md](project-format.md)). Untagged projects are as before.
- **PNG** (iCCP), **JPEG** (APP2), **TIFF** and **WebP** carry it when they have one; opening them reads it
  through the core's PNG reader or Qt's image plugins.

## Edit ▸ Color Settings

A simplified form of Photoshop's dialog (Ctrl+Shift+K):

- **Working space (RGB)**: sRGB IEC61966-2.1 (the default), Adobe RGB (1998), Display P3 or ProPhoto RGB. New
  documents take it (they stay untagged while it is sRGB).
- **Color management policy (RGB)**, for a file's embedded profile as it opens:
  - **Preserve Embedded Profiles** (the default): the document keeps the file's profile.
  - **Convert to Working RGB**: the pixels are converted to the working space, which the document then carries.
  - **Off**: the profile is dropped (kept only when it is the working space), and the values are read as sRGB.
- **Untagged images are treated as sRGB.**
- **Ask when opening** a file without a profile (leave it, or assign the working space) or with one other than the
  working space (use it, convert, or discard it). Both are off by default.

The four working spaces are made by Little CMS from their published primaries, white points and tone curves (sRGB's
curve, Adobe RGB's gamma of 563/256, ProPhoto's 1.8, Display P3 with sRGB's curve); no ICC file is bundled.

## Edit ▸ Assign Profile and Convert to Profile

- **Assign Profile** changes only which profile the values are read in: the pixel values stay, so the colours look
  different. Choose Don't Color Manage This Document (untagged, sRGB), the working space, or any profile, including
  an ICC file (Load…). One undo step.
- **Convert to Profile** keeps the colours' appearance and changes the values: every layer's pixels (at 8 or 16 bits)
  and the colours the document stores as numbers are converted: text and shape colours, vector shape fills and
  strokes, artboard backgrounds, layer style colours and gradients, Photo Filter and Gradient Map colours, and the
  foreground and background colours. Masks are not colour and stay. The rendering intent is **Perceptual** or
  **Relative Colorimetric** (the default), with **Use Black Point Compensation** (on). One undo step.

A smart object's placed pixels are converted; its source stays as it was embedded. Layer patterns, and blocks
carried from a PSD that NekoPhoto does not model, are not converted.

## The display

The canvas is drawn from the document's profile to your monitor's profile (relative colorimetric, black point
compensation). For a 16-bit document the conversion is fused with the reduction to the screen's 8 bits, so it costs
one pass.

The monitor profile comes from, in order:

1. a file chosen in **Edit ▸ Preferences ▸ Monitor colour profile** (the way to set one on Wayland, which has no
   monitor-profile property);
2. the system's, when **Use the system's monitor profile** is on (the default): X11's `_ICC_PROFILE` on the root
   window (set by colord and other colour managers), Windows' display profile.

When no monitor profile is known, or it is the document's own, the canvas is not transformed at all, and 8-bit
documents are drawn bit for bit as before. The foreground and background swatches are shown through the same
transform; the values you pick and see in the colour dialogs are the document's.

## View ▸ Proof Setup, Proof Colors and Gamut Warning

**Proof Setup ▸ Custom…** chooses a device to simulate (a working space or an ICC file), its rendering intent,
black point compensation, and the gamut warning's colour. **Proof Colors** (Ctrl+Y) then shows the document as it
would look on that device, and **Gamut Warning** (Ctrl+Shift+Y) paints the colours that device cannot show in the
warning colour. Both apply to every window and last until you turn them off; they change only the view.

## The eyedropper

The Eyedropper samples the document's values (what the layers hold, in the document's profile), not what the screen
shows; the swatch displays them through the monitor profile.

## Exports

| Format | Profile |
|---|---|
| PSD | resource 1039, the document's profile (none when untagged) |
| PNG | iCCP; untagged documents write the sRGB chunks as before |
| JPEG, WebP, TIFF | embedded through Qt's writers |
| GIF | none: converted to sRGB by default |
| TGA, ICO, SVG | none: the values as they are |

For the web formats (PNG, JPEG, WebP, GIF), a document with a profile other than sRGB asks whether to **Convert to
sRGB** first (off by default, on for GIF); converted files carry no profile, which viewers read as sRGB.

## Automation

- `document.profile`: `action` get (the default), `assign` or `convert`; `profile` `srgb`, `adobe-rgb`,
  `display-p3`, `prophoto`, `working`, `none` (assign only) or an ICC file's path; `intent` `perceptual` or
  `relative`; `blackPointCompensation`.
- `color.settings`: reads and sets the working space, the policy, the two prompts, the monitor profile, and the
  proof (`proofProfile`, `proofIntent`, `proofBlackPoint`, `proofColors`, `gamutWarning`, `gamutColor`).
- `document.export` takes `embedProfile` (true) and `convertToSrgb` (true for GIF, false otherwise).

The MCP tools are `document_profile` and `color_settings` ([automation.md](automation.md)).

## For developers

`src/core/include/compositor/colormgmt.h` is the one place that builds transforms for documents: a thread-safe LRU
cache of Little CMS transforms (the 24 most recent), `convertImage` for 8- and 16-bit premultiplied pixels (made
straight for the transform and premultiplied again; 16-bit pixels go through Little CMS's float pipeline, since its
16-bit one precalculates a grid that is coarse near the gamut's edges), `convertDocumentProfile`, soft-proofing
transforms, and `TransferCurve`, a profile's tone curve both ways (`documentTransfer(document)`) for code that works
in linear light. `RenderOptions::display` carries the canvas's transform into the renderer. The app side is
`src/app/ColorManagement.{h,cpp}` (settings, monitor profile, policies, export) and `ColorDialogs.cpp`.

Tests: `colormgmt_tests` checks the transforms against Little CMS itself and against the published sRGB and Adobe RGB
matrices (sRGB red is 219, 0, 0 in Adobe RGB, green 144, 255, 60), round trips through Adobe RGB, Display P3 and
ProPhoto at 8 and 16 bits, premultiplied pixels, the fused 16-to-8 display path, proofing and the gamut warning,
Convert to Profile over a document, and the profile round trips through PSD, projects and PNG.

---

## 日本語

[English](#colour-management) · **日本語**

NekoPhoto のカラーマネジメントは Photoshop と同じ考え方です。ドキュメントは 1 つのカラープロファイルを持ち、ピクセルと
カラーはそのプロファイルの値です。変換するのは境目だけ:ファイルを開くとき、プロファイルの指定・変換、モニターへの
表示、色の校正、書き出しです。合成と編集はすべてドキュメントの値のまま行います。

既定では以前と何も変わりません。タグなしのドキュメントは sRGB として扱い、モニタープロファイルがわからないときは
カンバスはドキュメントの値をそのまま表示します。

### ドキュメントのプロファイル

- **Photoshop ファイル**:イメージリソース 1039(ICC プロファイル)を読み込み、変更がなければバイト単位でそのまま
  書き戻します。プロファイルの指定・変換の後は新しいプロファイルを書き出します。
- **プロジェクト**:パッケージ内の `profile.icc`(マニフェストは `"colorSpace": "icc"`、形式バージョン 8)。
- **PNG**(iCCP)・**JPEG**(APP2)・**TIFF**・**WebP**:埋め込まれたプロファイルを読み書きします。

### 編集 ▸ カラー設定

- **作業用スペース(RGB)**:sRGB IEC61966-2.1(既定)、Adobe RGB (1998)、Display P3、ProPhoto RGB。新規ドキュメントに
  指定します(sRGB のときはタグなし)。
- **カラーマネジメントポリシー(RGB)**:埋め込まれたプロファイルを保持(既定)、作業用 RGB に変換、オフ。
- **プロファイルのない画像は sRGB として扱います。**
- プロファイルがないとき・作業用スペースと異なるときに確認する(どちらも既定はオフ)。

### 編集 ▸ プロファイルの指定、プロファイル変換

- **プロファイルの指定**:値の解釈だけを変えます(ピクセルの値はそのまま)。取り消しは 1 回。
- **プロファイル変換**:見た目を保って値を変換します。すべてのレイヤーのピクセル(8/16 bit)、テキスト・シェイプ・
  ベクトルシェイプ・アートボードの背景・レイヤースタイル・レンズフィルター・グラデーションマップのカラー、描画色と
  背景色。マッチング方法は **知覚的** または **相対的な色域を維持**(既定)、**黒点の補正を使用**(オン)。取り消しは 1 回。

### 表示

カンバスはドキュメントのプロファイルからモニターのプロファイルに変換して表示します(16 bit のドキュメントは 8 bit への
変換と同じ 1 回の処理で)。モニタープロファイルは、環境設定で選んだファイル(Wayland ではこれを使います)、または
システムのもの(X11 の `_ICC_PROFILE`、Windows のディスプレイプロファイル)です。わからないとき、またはドキュメントと
同じときは変換しないので、8 bit のドキュメントは以前とまったく同じに表示されます。

### 表示 ▸ 校正設定、色の校正、色域外警告

**校正設定 ▸ カスタム…** でシミュレートするデバイス、マッチング方法、黒点の補正、警告色を選びます。**色の校正**
(Ctrl+Y)でそのデバイスでの見え方を、**色域外警告**(Ctrl+Shift+Y)で表現できないカラーを警告色で示します。表示だけ
が変わります。

### スポイト

スポイトはドキュメントの値を取ります。スウォッチはモニタープロファイルを通して表示します。

### 書き出し

PSD はリソース 1039、PNG・JPEG・WebP・TIFF は埋め込み、GIF はプロファイルを持たないので既定で sRGB に変換します。
Web 向けの形式(PNG・JPEG・WebP・GIF)では、sRGB 以外のプロファイルのドキュメントを **sRGB に変換** するか確認します
(GIF は既定でオン)。

### 自動化

`document.profile`(取得・指定・変換)、`color.settings`、`document.export` の `embedProfile` と `convertToSrgb`。
MCP ツールは `document_profile` と `color_settings` です。
