# What NekoPhoto can do

**English** · [日本語](#日本語)

Everything NekoPhoto does, on Linux and Windows. Keyboard shortcuts follow Photoshop's and are
listed in [linux-port.md](linux-port.md#keyboard-shortcuts).

## Layers
- Layers and folders with opacity and all 27 of Photoshop's blend modes plus Pass Through, drawn with its calibrated byte arithmetic (Vivid Light, Linear Light, Hard Mix, Darker/Lighter Color and the rest match Photoshop captures; Dissolve dithers by document position)
- Layer masks: paint, fill, invert, blur and feather; link or unlink them from the layer
- Clipping masks and folder masks
- Adjustment layers: Hue/Saturation, Levels, Curves, Exposure, Gradient Map, Grain
- Merge Down, Merge Layers and Merge Group (Ctrl+E)
- Duplicate, rename, reorder and nest by drag and drop; drag layers between open projects
- Copy and paste whole layers between documents, as Photoshop: Edit ▸ Copy with layers selected and no selection copies them and the folders' contents with their masks, vector masks, styles, text, shapes, smart objects (and their sources), adjustments, blending and clipping; Paste in any open document puts them above the active layer as one undo step, converted to its colour profile and depth (other apps get the layers flattened)

## Transform
- Move, scale, rotate and flip without losing resolution
- Free distort (Ctrl-drag a handle)
- Transform several layers, or a whole folder, at once
- Snapping to the canvas and other layers, with guides
- Exact position, size and angle in the options bar

## Selections
- Rectangle and ellipse marquee, freehand and polygonal lasso, and an edge-aware magic wand: shading and texture stay in, edges between similar colours hold, and the tolerance can be changed right after a click
- Quick Select (Shift+W): scribble over the subject, or click it (a 48 MB model, downloaded from the options bar); the selection snaps to the image's edges
- Add (Shift), subtract (Alt) and intersect (Shift+Alt)
- Expand, Contract, Feather, Smooth, Border, Invert; load a layer or mask as a selection
- Channels (Window ▸ Channels, [details](channels.md)): alpha channels as saved selections (Select ▸ Save Selection and Load Selection in every combine mode, Ctrl-click a thumbnail with Shift, Alt or both), painted like Quick Mask, with Channel Options; one colour channel in gray (Ctrl+3, 4, 5) that the brush, fills, adjustments, filters and paste then change alone; spot channels kept from PSD files; channels in PSD files and projects, at 8 and 16 bits
- Content-Aware Fill, which continues edges and patterns and can extend an image past its borders; its dialog (Edit > Content-Aware Fill…) chooses the sampling area (Auto, the whole layer, or Custom painted with an include/exclude brush), previews on the canvas and can output to a new layer ([content-aware.md](content-aware.md))
- Content-Aware Scale (Edit menu): seam carving narrows, widens, shortens or heightens a layer while the parts that carry detail keep their proportions; Protect keeps the selection, with a live preview ([details](content-aware-scale.md))

## Painting and retouching
- Brush and eraser with size, hardness and opacity; Shift for straight lines
- 196 MyPaint brushes (pencils, inks, charcoal, paint, smudging, by the MyPaint team, David Revoy, Ramón Miranda, Tanda and others) that follow pen pressure and tilt; pick one from the Brush tool's options bar. Your own `.myb` presets go in the app's `brushes` folder
- Import your brushes (File > Import Brushes, or the button under the brush list): Photoshop `.abr` with its presets and dynamics, Procreate `.brushset` and `.brush` with shape, grain and pencil settings, Clip Studio `.sut` with its tip images, paper textures and settings, or any image as a tip. What a brush uses that cannot be carried over is listed after the import
- Brush dynamics for tip brushes (the Dynamics… button in the Brush tool's options bar): a pressure curve with a minimum for size and for flow, Density by spacing (the stroke looks as dense at any spacing), and Mouse speed as pressure, a simulated pressure for mouse users, off by default. Imported brushes bring their own dynamics too: pressure, pen tilt, fade and jitter on size, flow, opacity, angle, roundness and scatter, and tablet tilt, barrel rotation and speed are all read. Procreate brushes bring speed on size, opacity and spacing, tilt on size, opacity, bleed and roundness, azimuth and barrel roll turning the tip (following the stroke on a pen without barrel rotation), and moving grain that rolls with the stroke. A tip that turns with the stroke starts turned the way the stroke goes, from its first dab. Pen tilt shapes the tip turns any tip brush into a tilted pencil that flattens and turns the way the pen leans ([brush-engine.md](brush-engine.md))
- Smoothing for the Brush and Eraser, as in Photoshop: a Smoothing % in the options bar steadies the line, which trails the pen, with Pulled String Mode, Stroke Catch-Up, Catch-Up On Stroke End and Adjust For Zoom under Dynamics…, next to two lighter filters of their own: input smoothing (steadies a jittery tablet with next to no lag) and pressure smoothing. All off by default ([brush-engine.md](brush-engine.md#smoothing))
- Spot Healing Brush and Clone Stamp; both ignore what a layer mask hides
- Smudge, Liquify and Blur, on pixels or masks, and Sharpen
- Warp Cage (Edit ▸ Warp Cage, Photoshop's custom warp): drag the 16 points of a 4 × 4 mesh over a layer with a live preview; pixels bend for good, a smart object keeps it as its own editable Custom warp (written to PSD as Photoshop's), and a shape layer bends as a path
- Vector shape layers (Photoshop's shape layers, editable and written to PSD as such): the Shape tool's rectangle, ellipse, polygon, star, line and custom shapes with fill and stroke (colour, width, alignment, dashes) editable from the options bar; gradient and pattern fills and strokes; path operations (Combine, Subtract Front Shape, Intersect, Exclude, Merge Shape Components); live rectangles (per-corner radii) and ellipses with their properties in the Shape bar; the Pen (P) and Direct Selection (A); vector masks on any layer (Layer ▸ Vector Mask); Type ▸ Create Work Path and Convert to Shape; the Paths panel (Work Path and saved paths: fill, stroke, load as selection, make from selection, make shape layer) (docs/vector-tools.md)
- Dodge, Burn (Shadows, Midtones, Highlights; Protect Tones) and Sponge (desaturate or saturate), painted through the brush with Exposure or Flow as its opacity; their curves are shaped after Photoshop's behaviour, not measured against it
- Paint Bucket (Shift+G) with Tolerance, Contiguous, Anti-alias and All Layers, inside the selection
- Open camera RAW files (CR2, CR3, NEF, ARW, RAF, ORF, RW2, DNG and more) through LibRaw, developed with the camera's white balance into sRGB; Filter ▸ Camera Raw then adjusts them
- Healing Brush (the healing tool's Sampled type: Alt-click a source, and the copied texture takes the tone around the stroke) and Patch (select the blemish, drag the selection to the area to copy from)
- Content-Aware Move (the healing tool's last type): select an object, or draw round it with the tool's lasso, and drag it; the hole it leaves is filled from its surroundings and the patch blended into its new place. Extend mode leaves the original; Adaptation (Very Strict to Very Loose) sets how far the patch's tone follows its new place and how wide a seam is blended
- Quick Mask (Q, or Select ▸ Edit in Quick Mask Mode): the selection as a red overlay to paint with every mask tool, white selecting; saving or exporting leaves it first
- Gradients and shapes (rectangles, rounded rectangles, ellipses); the Gradient tool draws Foreground to Background, Foreground to Transparent or any multi-stop gradient imported from a Photoshop `.grd` file (docs/presets.md)
- Text in any installed font, editable until you paint on the layer
- Style text letter by letter, as with Photoshop's Type tool: select letters in the text editor and change their font, size, weight, bold and italic, colour, tracking, baseline shift, leading, caps, underline and strikethrough; the Character section shows the style at the cursor or over the selection (blank where it is mixed), with nothing selected a change applies to all the text, and the styles go out to PSD as Photoshop's own style runs
- Eyedropper and colour picker

## Adjustments and filters
- Seventeen adjustment layers, also as destructive adjustments: Levels, Curves, Hue/Saturation, Exposure, Gradient Map, Grain, and Photoshop's Invert, Brightness/Contrast, Posterize, Threshold, Color Balance, Black & White, Vibrance, Photo Filter, Channel Mixer, Selective Color and Color Lookup (.cube, .3dl and ICC LUTs), read from and written to PSD as Photoshop's own ([adjustment-layers.md](adjustment-layers.md))
- Levels (with Auto), Curves, Hue/Saturation, Exposure, Gradient Map, Grain, Invert
- Gaussian Blur and Motion Blur, which spread past a layer's edges
- Add Noise and Lens Correction
- Filter > Camera Raw Filter (Shift+Ctrl+A): white balance, tone, presence, curve, colour mixer, colour grading, detail, optics, geometry, effects and calibration on a layer's pixels ([camera-raw.md](camera-raw.md))
- Filter > G'MIC: over 850 filters with their own controls, when `gmic` is installed (Update Filters fetches the catalogue; the few that cannot work here, such as those that resize the image or make layers, are hidden unless Show all filters is on)
- Filter > Mosh: OpenMosh's 54 glitch, distortion, retro, stylize, colour and composite effects (Pixel Sort, Data-Mosh, Hard Glitch, VHS, CRT, Halftone, Kaleidoscope, Glow, Light Streak, Feedback, Optical-Flow, Ascii, ChromaKey and more), with a live preview and a reroll for the random ones, at 8 and 16 bits; Overlay and Mask read another layer where it lies over this one, and Caption stamps text ([mosh.md](mosh.md))
- Smart Filters on smart objects, edited as in Photoshop's Layers panel: each filter's settings (double-click), blending options, on/off per filter or for the whole stack, reorder by dragging (or from the menu), delete or clear them, and paint, show, invert, disable or delete the shared filter mask; PSD export keeps them ([smart-objects.md](smart-objects.md))
- Live previews on the canvas, limited to the selection when there is one

## Remove Background
- Off until you turn it on in Edit > Preferences, which downloads a model once
- Runs on your machine; nothing is uploaded
- Advanced options: refine the edges, solve hair and fur, remove speckles, clean the edge colours, and a detail pass at full resolution for large photos; Quality > Best turns on everything for hair and fur in one step

## Files and canvas
- Open Photoshop PSD and PSB files with their layers, folders, masks, blend modes, most adjustment layers, layer styles drawn as Photoshop draws them (docs/layer-styles.md), folders isolated and faded as in Photoshop, smart objects that keep their source (docs/smart-objects.md), vector masks and shape layers drawn from their paths (docs/vector-masks.md) and simple text as editable text; what cannot be kept is listed after opening
- Layer ▸ Layer Style: Photoshop's Layer Style dialog for drop and inner shadows, outer and inner glows, bevel and emboss, satin, colour, gradient and pattern overlays and stroke, with Copy, Paste and Clear Layer Style; styles export to PSD as Photoshop's own (docs/layer-styles.md)
- Photoshop presets (File ▸ Import Presets…): layer styles from `.asl` files, applied from Layer ▸ Layer Style ▸ Apply Style with the patterns they use; patterns from `.pat` files for pattern overlays and bevel textures; gradients from `.grd` files for the Gradient tool and the Layer Style dialog's gradients. They stay in the library across sessions (docs/presets.md)
- Export layered Photoshop PSD files (File > Export as Photoshop Document): layers, folders, masks, clipping, blend modes and Levels, Curves, Exposure and Hue/Saturation adjustment layers, with a merged image; text layers as editable Photoshop text; a PSD you opened keeps its layer styles, editable text, smart objects and vector masks on the way back out while they still match their layers (docs/psd-roundtrip.md); anything Photoshop cannot carry is listed before you export (docs/psd-export.md)
- Open Clip Studio `.clip` projects with their layers, folders, masks, clipping, opacity and blend modes (vector and text layers come in as their pixels)
- Open SVG files (`.svg`, `.svgz`) as layers: paths and basic shapes with solid fills and strokes become editable vector shape layers, groups become folders with their opacity; gradients, patterns, text, images, filters, clip paths and masks come in as pixel layers drawn by Qt SVG (docs/svg-pdf.md)
- Export SVG (File > Export SVG): vector shape layers as paths with their fill and stroke, folders as groups, every other layer as an embedded PNG, so the file looks like the document (docs/svg-pdf.md)
- Open a PDF page as a pixel layer at a chosen resolution (Qt PDF; a multi-page file asks which page) (docs/svg-pdf.md)
- Open Affinity documents (`.afphoto`, `.afdesign`, `.afpub`, and Affinity 3's `.af`) with their pixel layers, groups, masks, clipped layers, opacity, visibility and blend modes, and artistic and frame text as editable text layers; vector shapes and artboards come in as pixels, and adjustments and effects are listed as left out (see [affinity-import.md](affinity-import.md))
- Open Aseprite `.ase`/`.aseprite` sprites with their layers, folders, opacity and blend modes; a sprite of several frames opens with a layer per cel and its frames on the Timeline, with their durations
- Open animated GIFs with each frame as a layer ("Frame N (D ms)", frame 1 visible) and the frames, delays and looping on the Timeline, and icons (`.ico`, `.cur`) with each size as a layer
- Frame animation (Window > Timeline), as Photoshop's Timeline in frame mode: frames that each show the layers with their own visibility, position and opacity for a delay you set; New, Delete and reorder frames, Make Frames From Layers, play it on the canvas, choose how many times it loops. Frames are saved in the project, and File > Export Animated GIF writes them (docs/animation.md)
- Actions (Window > Actions, Alt+F9), as Photoshop's Actions panel: record menu commands, filters, adjustments, selections and brush strokes as you work, play them back on any document, switch steps off, reorder, delete or edit them, and share them as JSON; File > Automate > Batch runs an action over a folder of files and saves the results in the format you pick (docs/actions.md)
- 16 bits per channel (Image ▸ Mode ▸ 8 Bits/Channel, 16 Bits/Channel): 16-bit PSD, PNG and TIFF files open as 16-bit documents, and every blend mode, mask, clipping mask and folder renders at 16 bits; an unedited layer of a 16-bit PSD goes back out byte for byte. Adjustments and adjustment layers, the built-in filters, selections and the Select menu, fill, the clipboard, the content-aware tools, Image Size, Crop, Trim, Distort and Warp work at 16 bits too, and so do painting and retouching: the brushes in every engine and the eraser (on pixels, masks and the Quick Mask), Clone Stamp, the healing tools and Patch, Blur, Sharpen, Smudge, Dodge, Burn and Sponge, gradients, the Paint Bucket, moving selected pixels, Merge Down and Apply Layer Mask. Text (painted at 16 bits, rich text, Warp Text, PSD type layers), shapes and live shapes with their fills and strokes, the Pen and paths, vector masks, Fill and Stroke Path, and layer styles (all ten effects and folder styles, drawn at 16 bits) work at 16 bits too, and so do smart objects (their contents keep their own depth: Place Embedded, Convert to a 16-bit PSB, Edit Contents, Replace, Rasterize, warps, 16-bit PSD and projects) and their Smart Filters, drawn at 16 bits. Camera Raw (float colour, rounded once), G'MIC (float both ways), Remove Background (a 16-bit mask), artboards, the timeline, SVG export and exporting artboards and slices work at 16 bits too: nothing is greyed out in a 16-bit document ([bit-depth.md](bit-depth.md))
- Colour management, as in Photoshop: Edit ▸ Color Settings (sRGB, Adobe RGB (1998), Display P3 or ProPhoto RGB as the working space; preserve, convert or drop embedded profiles; untagged images are sRGB), Assign Profile and Convert to Profile (perceptual or relative colorimetric, black point compensation), the canvas shown through your monitor profile (X11, Windows, or a file in Preferences), View ▸ Proof Setup, Proof Colors and Gamut Warning; PSD, PNG, JPEG, WebP and TIFF keep their profiles in and out, and web exports can convert to sRGB ([color-management.md](color-management.md))
- Open PNG, JPEG, TIFF, TGA, WebP and more; drop an image on the canvas to add it as a layer, or on the tab strip to open it
- Projects of up to a gigapixel of layers; the Mac app opens projects up to 100 megapixels
- Export PNG, TIFF, TGA, a multi-size Windows icon (16, 32, 48 and 256 px), or JPEG and WebP with a live preview (WebP keeps transparency, and is lossless at quality 100)
- Crash recovery: unsaved changes are autosaved in the background every few minutes (Preferences sets how often, or turns it off) and offered back after a crash; your own files are never touched
- Several projects in tabs; opening a file from the file manager adds a tab to the running window
- The Crop tool as Photoshop's: ratio presets (Original Ratio, 1:1, 4:5, 5:7, 2:3, 3:2, 4:3, 16:9, 9:16) or a typed W and H, Swap (X), the rule-of-thirds grid while dragging, Alt to drag from the centre, Shift to keep the box's shape, snapping to the canvas edges and centre and to layers, and the box starts on the selection when there is one
- Crop, Canvas Size and Image Size; Image > Trim cuts the canvas to its content (by transparency or a corner's colour, on the sides you choose); rulers and a pixel grid
- Artboards, as in Photoshop: the Artboard tool (Shift+V) drags out a named rectangle with a white, black, transparent or custom background whose layers are clipped to it; drag inside one to move it with its contents, an edge or corner to resize it; File > Export Artboards to Files writes each as PNG or JPEG; PSD artboards open and export as Photoshop's own, and projects keep them ([artboards-slices.md](artboards-slices.md))
- Slices: the Slice tool (Shift+C) draws named rectangles, File > Export Slices writes each as PNG or JPEG; a PSD's slices (resource 1050) open as editable slices and go back out with the export ([artboards-slices.md](artboards-slices.md))
- An open project follows its package on disk: when another app or an agent writes the `.comp`, the tab reloads in place (a package merely touched, or caught half written, is left alone, and unsaved work is never replaced without asking)

- CPU power (Edit > Preferences > Performance): All, High, Medium or Low. Fewer cores for NekoPhoto's work, and at Medium and Low a lower priority, so a render or another heavy program running beside it gets the CPU first; `NEKOPHOTO_CPU=low` for one run
- The interface in English or Japanese (Photoshop's Japanese terms): it follows the desktop's language, or Edit > Preferences > Language picks one; `--lang ja` for one run ([translating.md](translating.md) explains adding a language)

## Working faster
- Scrubby labels, as in Photoshop: drag the label beside a number (Size, Opacity, a filter's Radius, a Layer Style's Distance, the transform fields, Camera Raw's sliders) left or right to change it, with Shift for fine steps and Alt or Ctrl for coarse ones; a click on the label still types in the field, and one drag is one undo step
- Opening a file that could not be carried over whole (PSD, PSB, Clip Studio, Affinity, SVG, PDF, imported brushes) shows a bar over the canvas instead of a dialog: how many things changed and the first of them, Details for the full list, Undo Open to close the document again
- Shift + a tool's letter steps through its group, as in Photoshop (Shift+J: Spot Healing, Healing Brush, Patch, Content-Aware Move; Shift+O: Dodge, Burn, Sponge; and the others in [linux-port.md](linux-port.md#keyboard-shortcuts))

## Automation
- Scripts and AI agents can drive the editor through a socket or MCP; see [automation.md](automation.md)

---

## 日本語

[English](#what-nekophoto-can-do) · **日本語**

NekoPhoto の機能の一覧です(Linux・Windows)。キーボードショートカットは Photoshop に合わせてあり、
[linux-port.md](linux-port.md#keyboard-shortcuts)(英語)に一覧があります。

### レイヤー
- 描画モードと不透明度を持つレイヤーとグループ
- レイヤーマスク:描画、塗りつぶし、反転、ぼかし、境界のぼかし。レイヤーとのリンクの切り替え
- クリッピングマスクとグループのマスク
- 調整レイヤー:色相・彩度、レベル補正、トーンカーブ、露光量、グラデーションマップ、粒子
- 下のレイヤーと結合、レイヤーを結合、グループを結合(Ctrl+E)
- ドラッグ&ドロップで複製・名前変更・並べ替え・入れ子。開いているプロジェクト間でもレイヤーを移動可能
- ドキュメント間でレイヤーごとコピー&ペースト(Photoshop と同じ):選択範囲がなくレイヤーを選択しているときの 編集 ▸ コピー で、レイヤーとグループの中身をマスク・ベクトルマスク・スタイル・テキスト・シェイプ・スマートオブジェクト(ソースごと)・調整レイヤー・描画モード・クリッピングを保ったままコピーし、開いているどのドキュメントでもペーストで作業中のレイヤーの上に 1 回の取り消し単位で追加します。カラープロファイルとビット数はペースト先に合わせて変換します(他のアプリには統合した画像を渡します)

### 変形
- 解像度を落とさずに移動・拡大縮小・回転・反転
- 自由な形に(Ctrl を押しながらハンドルをドラッグ)
- 複数のレイヤーやグループをまとめて変形
- カンバスや他のレイヤーへのスナップとガイド
- オプションバーで位置・サイズ・角度を数値指定

### 選択範囲
- 長方形選択・楕円選択、なげなわ・多角形選択、自動選択
- クイック選択(Shift+W):被写体をなぞるか、クリックするだけ(クリック用の 48 MB のモデルはオプションバーからダウンロード)。選択範囲は画像の輪郭に合わせて調整されます
- 追加(Shift)、削除(Alt)、共通範囲(Shift+Alt)
- 拡張、縮小、境界をぼかす、滑らかに、境界線、選択範囲を反転。レイヤーやマスクから選択範囲を作成
- チャンネル(ウィンドウ ▸ チャンネル、[詳細](channels.md)):選択範囲を保存したアルファチャンネル(選択範囲 ▸ 選択範囲を保存・選択範囲を読み込むは追加・一部削除・共通範囲にも対応、サムネールの Ctrl+クリックと Shift・Alt)。クイックマスクと同じように描画でき、チャンネルオプションでマスク範囲・選択範囲、カラー、不透明度を設定。カラーチャンネルを 1 つだけグレーで表示(Ctrl+3、4、5)すると、ブラシ、塗りつぶし、色調補正、フィルター、ペーストがそのチャンネルだけに適用されます。PSD のスポットカラーチャンネルは保持。チャンネルは PSD とプロジェクトに保存され、8 ビットと 16 ビットの両方で使えます
- コンテンツに応じた塗りつぶし:輪郭や模様をつなげ、画像の外側への拡張にも使えます

### 描画とレタッチ
- サイズ・硬さ・不透明度を指定できるブラシと消しゴム。Shift で直線
- 筆圧と傾きに反応する MyPaint ブラシ 196 種類(鉛筆、インク、木炭、絵の具、ぼかし。MyPaint チーム、David Revoy、Ramón Miranda、Tanda ほか)。ブラシツールのオプションバーから選べます。自作の `.myb` はアプリの `brushes` フォルダーに置けます
- ブラシの読み込み(ファイル > ブラシを読み込み、またはブラシ一覧の下のボタン):Photoshop の `.abr`(プリセットとシェイプダイナミクス)、Procreate の `.brushset`・`.brush`(シェイプ、グレイン、ペンシル設定)、クリップスタジオの `.sut`(先端画像、用紙テクスチャ、設定)、任意の画像を先端として。引き継げなかった設定は読み込み後に表示されます
- 先端ブラシのダイナミクス(ブラシツールのオプションバーの「ダイナミクス...」):サイズと流量それぞれの筆圧カーブと最小値、「間隔に合わせて濃度を保つ」(どの間隔でも同じ濃さ)、マウス用の擬似筆圧「マウスの速度を筆圧として使う」(初期設定はオフ)。読み込んだブラシは自身のダイナミクスも持ち込みます:サイズ、流量、不透明度、角度、真円率、散布への筆圧・ペンの傾き・フェード・ジッター。タブレットの傾き、ペンの回転、速度もすべて読み取ります。Procreate のブラシは、速度によるサイズ・不透明度・間隔、傾きによるサイズ・不透明度・にじみ・真円率、傾きの方向とペンの回転による先端の回転(回転を検出しないペンではストロークの向きに従います)、ストロークとともに動くグレインを持ち込みます。ストロークの向きに回る先端は、描き始めからストロークの向きを向きます。「ペンの傾きで先端の形を変える」で、どの先端ブラシも寝かせた鉛筆のように、傾けると平たくなり傾けた向きに回ります([brush-engine.md](brush-engine.md))
- ブラシと消しゴムのスムージング(Photoshop と同じく):オプションバーの「スムージング」(%)で線がペンの後を追って安定します。「ダイナミクス...」には「ひもを引くモード」「ストロークのキャッチアップ」「ストローク終点のキャッチアップ」「ズームの調整」と、別々の軽いフィルターとして「入力のスムージング」(タブレットのぶれをほとんど遅れなしに抑えます)と「筆圧のスムージング」があります。初期設定はすべてオフです([brush-engine.md](brush-engine.md#smoothing))
- スポット修復ブラシとコピースタンプ(どちらもレイヤーマスクで隠れた部分は使いません)
- 指先ツール、ゆがみ、ぼかしツール(ピクセルにもマスクにも使えます)
- グラデーションとシェイプ(長方形、角丸長方形、楕円)
- インストール済みの任意のフォントでテキスト。レイヤーに描画するまでは再編集可能
- スポイトとカラーピッカー

### 色調補正とフィルター
- 調整レイヤー 17 種(破壊的な色調補正としても):レベル補正、トーンカーブ、色相・彩度、露光量、グラデーションマップ、粒子に加え、Photoshop の階調の反転、明るさ・コントラスト、ポスタリゼーション、2 階調化、カラーバランス、白黒、自然な彩度、レンズフィルター、チャンネルミキサー、特定色域の選択、カラールックアップ(.cube・.3dl・ICC)。PSD では Photoshop 自身の調整レイヤーとして読み書きします
- レベル補正(自動補正付き)、トーンカーブ、色相・彩度、露光量、グラデーションマップ、粒子、階調の反転
- ぼかし(ガウス)とぼかし(移動):レイヤーの端の外まで広がります
- ノイズを加える、レンズ補正
- フィルター > Camera Raw フィルター(Shift+Ctrl+A):ホワイトバランス、階調、外観、トーンカーブ、カラーミキサー、カラーグレーディング、ディテール、光学、ジオメトリ、効果、キャリブレーションをレイヤーのピクセルに適用
- フィルター > G'MIC:`gmic` をインストールすると、850 種類以上のフィルターを専用の設定画面で使えます(画像サイズを変えるものやレイヤーを作るものなど、ここで使えないものは「Show all filters」をオンにしない限り非表示)
- フィルター > Mosh:OpenMosh のグリッチ・変形・レトロ・表現手法・カラー・合成のエフェクト 54 種(ピクセルソート、データモッシュ、ハードグリッチ、VHS、CRT、ハーフトーン、万華鏡、グロー、ライトストリーク、フィードバック、オプティカルフロー、アスキー、クロマキーなど)。ライブプレビュー付きで、ランダムなものはシードを振り直せます。8 ビットと 16 ビットに対応。オーバーレイとマスクはほかのレイヤーを重なる位置で読み込み、キャプションはテキストを描き込みます
- スマートオブジェクトのスマートフィルターを Photoshop のレイヤーパネルと同じように編集:各フィルターの設定(ダブルクリック)、描画オプション、フィルターごと・全体のオン/オフ、ドラッグ(またはメニュー)での並べ替え、削除・すべて消去、共有フィルターマスクへの描画・表示・反転・無効化・削除。PSD に書き出しても保たれます
- カンバス上でのライブプレビュー(選択範囲があればその中だけ)

### 背景を削除
- 初期状態ではオフ。編集 > 環境設定 でオンにするとモデルを一度だけダウンロードします
- 処理はすべて手元のマシンで行い、画像はどこにも送信されません
- 詳細オプション:輪郭の調整、髪や毛並みの抽出、細かなノイズの除去、輪郭の色の補正、大きな写真向けの高解像度ディテール処理。Quality の「Best」で、髪や毛並み向けの設定をまとめて有効にできます

### ファイルとカンバス
- Photoshop の PSD/PSB を、レイヤー・グループ・マスク・描画モード・主な調整レイヤーを保ったまま開けます。引き継げなかった要素は開いた後に一覧表示されます
- レイヤー付きの Photoshop PSD に書き出せます(ファイル > Photoshop ドキュメントとして書き出し):レイヤー、グループ、マスク、クリッピング、描画モード、レベル補正・トーンカーブ・露光量・色相/彩度の調整レイヤーと統合画像。Photoshop で再現できない要素は書き出す前に一覧表示されます
- クリップスタジオの `.clip` を、レイヤー・フォルダー・マスク・クリッピング・不透明度・描画モードを保ったまま開けます(ベクターやテキストのレイヤーは画像として読み込みます)
- Aseprite の `.ase`/`.aseprite` を、レイヤー・グループ・不透明度・描画モードを保ったまま開けます。複数フレームのスプライトはセルごとのレイヤーとタイムラインのフレーム(表示時間つき)として開きます
- アニメーション GIF は各フレームをレイヤーとして(「Frame N (D ms)」、フレーム 1 のみ表示)、フレーム・表示時間・ループ回数はタイムラインに、アイコン(`.ico`、`.cur`)は各サイズをレイヤーとして開けます
- フレームアニメーション(ウィンドウ > タイムライン):Photoshop のフレームモードのタイムラインと同じく、各フレームがレイヤーの表示・位置・不透明度と表示時間を持ちます。フレームの追加・削除・並べ替え、レイヤーからフレームを作成、カンバス上での再生、ループ回数の指定。フレームはプロジェクトに保存され、ファイル > アニメーション GIF を書き出し で書き出せます
- アクション(ウィンドウ > アクション、Alt+F9):メニューのコマンド、フィルター、色調補正、選択範囲、ブラシのストロークを記録して、どのドキュメントにも再生できます。ステップのオン/オフ・並べ替え・削除・編集、JSON での読み込みと書き出し。ファイル > 自動処理 > バッチ でフォルダー内のファイルにまとめて適用し、選んだ形式で保存します
- 16 bit/チャンネル(イメージ ▸ モード ▸ 8 bit/チャンネル、16 bit/チャンネル):16 bit の PSD・PNG・TIFF は 16 bit のドキュメントとして開き、すべての描画モード・マスク・クリッピングマスク・グループを 16 bit で合成します。16 bit の PSD の編集していないレイヤーはバイト単位でそのまま書き出されます。色調補正と調整レイヤー、組み込みのフィルター、選択範囲と選択範囲メニュー、塗りつぶし、クリップボード、コンテンツに応じた各機能、画像解像度、切り抜き、トリミング、自由な形に変形とワープも 16 bit で使えます。ペイントとレタッチ(すべてのエンジンのブラシと消しゴム(ピクセル・マスク・クイックマスク)、コピースタンプ、修復ツールとパッチ、ぼかし・シャープ・指先、覆い焼き・焼き込み・スポンジ、グラデーション、塗りつぶしツール、選択したピクセルの移動、下のレイヤーと結合、レイヤーマスクを適用)も 16 bit で使えます。テキスト(16 bit で描画、スタイルの混在、ワープテキスト、PSD のテキストレイヤー)、シェイプとライブシェイプ(塗りと線)、ペンとパス、ベクトルマスク、パスの塗りつぶしと境界線、レイヤースタイル(10 種類の効果とグループのスタイル、16 bit で描画)も 16 bit で使えます。スマートオブジェクト(内容は元のビット数のまま:埋め込みを配置、16 bit の PSB への変換、コンテンツを編集、置き換え、ラスタライズ、ワープ、16 bit の PSD とプロジェクト)とそのスマートフィルター(16 bit で描画)も使えます。Camera Raw(浮動小数点の色で処理し 1 回だけ丸める)、G'MIC(浮動小数点で受け渡し)、背景を削除(16 bit のマスク)、アートボード、タイムライン、SVG の書き出し、アートボードとスライスの書き出しも 16 bit で使えます。16 bit のドキュメントでグレー表示になるものはありません([bit-depth.md](bit-depth.md))
- カラーマネジメント(Photoshop と同じ):編集 ▸ カラー設定(作業用スペースは sRGB・Adobe RGB (1998)・Display P3・ProPhoto RGB、埋め込みプロファイルの保持・変換・破棄、プロファイルのない画像は sRGB)、プロファイルの指定とプロファイル変換(知覚的・相対的な色域を維持、黒点の補正)、モニタープロファイルを通したカンバス表示(X11、Windows、または環境設定で選んだファイル)、表示 ▸ 校正設定・色の校正・色域外警告。PSD・PNG・JPEG・WebP・TIFF はプロファイルを読み書きし、Web 向けの書き出しでは sRGB に変換できます([color-management.md](color-management.md))
- PNG、JPEG、TIFF、TGA、WebP などを開けます。カンバスにドロップするとレイヤーとして追加、タブバーにドロップすると新しいドキュメントとして開きます
- レイヤー合計 1 ギガピクセルまでのプロジェクト(Mac 版で開けるのは 1 億画素まで)
- PNG・TIFF・TGA・複数サイズの Windows アイコン(16/32/48/256 px)書き出し、プレビュー付きの JPEG・WebP 書き出し(WebP は透明部分を保持し、品質 100 で可逆圧縮)
- アートボード:アートボードツール(Shift+V)で名前と背景色(白・黒・透明・任意の色)を持つ矩形を作り、中のレイヤーはその範囲で切り抜かれます。ファイル > アートボードを書き出しで PNG/JPEG に書き出し、PSD のアートボードは Photoshop 形式のまま読み書きします
- スライス:スライスツール(Shift+C)で名前付きの矩形を作り、ファイル > スライスを書き出しで PNG/JPEG に書き出します。PSD のスライス(リソース 1050)も編集できる形で読み書きします
- クラッシュからの復元:未保存の変更を数分ごとにバックグラウンドで自動保存し、異常終了の後に復元を提案します(間隔の変更やオフは環境設定で)。元のファイルには触れません
- タブで複数のプロジェクト。ファイルマネージャーから開いたファイルは起動中のウィンドウにタブとして追加
- 切り抜きツール(Photoshop と同じ):比率のプリセット(元の縦横比、1:1、4:5、5:7、2:3、3:2、4:3、16:9、9:16)または幅と高さの入力、高さと幅を入れ替え(X)、ドラッグ中の三分割グリッド、Alt で中心から、Shift で縦横比を保持、カンバスの端と中心やレイヤーへのスナップ。選択範囲があればその範囲から始まります
- 切り抜き、カンバスサイズ、画像解像度。イメージ > トリミングで内容に合わせてカンバスを切り詰め(透明部分または角の色で、選んだ辺のみ)。定規とピクセルグリッド
- 開いているプロジェクトはディスク上の変更に追従します:他のアプリやエージェントが `.comp` を書き換えると、タブがその場で読み込み直します(触れただけの変更や書き込み途中は無視し、未保存の作業は確認なしに置き換えません)

- CPU パワー(編集 > 環境設定 > パフォーマンス):すべて・高・中・低。NekoPhoto が使うコア数を減らし、中と低では優先度も下げるので、横で動かしているレンダリングなどの重い処理に CPU を譲ります。その回だけなら `NEKOPHOTO_CPU=low`
- 画面表示は日本語と英語(用語は Photoshop 日本語版に準拠)。デスクトップの言語に合わせるか、編集 > 環境設定 > 言語 で選べます。`--lang ja` でその回だけ切り替えることもできます

### 操作
- スクラブ(Photoshop と同じ):数値の横のラベル(直径、不透明度、フィルターの半径、レイヤースタイルの距離、変形の数値欄、Camera Raw のスライダー)を左右にドラッグして値を変えられます。Shift で細かく、Alt または Ctrl で大きく動きます。ラベルのクリックでは従来どおり数値を入力でき、1 回のドラッグは 1 つの取り消し単位です
- そのままでは引き継げない要素のあるファイル(PSD、PSB、クリップスタジオ、Affinity、SVG、PDF、読み込んだブラシ)を開くと、ダイアログではなくカンバス上部のバーで知らせます:変更の件数と最初の 1 件、詳細で全件の一覧、開くの取り消しでドキュメントを閉じます
- Shift + ツールのキーで同じグループのツールを順に切り替えます(Photoshop と同じ。Shift+J:スポット修復ブラシ・修復ブラシ・パッチ・コンテンツに応じた移動、Shift+O:覆い焼き・焼き込み・スポンジ。ほかは [linux-port.md](linux-port.md#keyboard-shortcuts))

### 自動化
- スクリプトや AI エージェントからソケットまたは MCP 経由で操作できます。詳しくは [automation.md](automation.md)(英語)
