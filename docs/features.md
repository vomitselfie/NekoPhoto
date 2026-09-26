# What NekoPhoto can do

**English** · [日本語](#日本語)

Everything the Mac app does, with Linux key names. Keyboard shortcuts are
listed in [linux-port.md](linux-port.md#keyboard-shortcuts).

## Layers
- Layers and folders with opacity and all 27 of Photoshop's blend modes plus Pass Through, drawn with its calibrated byte arithmetic (Vivid Light, Linear Light, Hard Mix, Darker/Lighter Color and the rest match Photoshop captures; Dissolve dithers by document position)
- Layer masks: paint, fill, invert, blur and feather; link or unlink them from the layer
- Clipping masks and folder masks
- Adjustment layers: Hue/Saturation, Levels, Curves, Exposure, Gradient Map, Grain
- Merge Down, Merge Layers and Merge Group (Ctrl+E)
- Duplicate, rename, reorder and nest by drag and drop; drag layers between open projects

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
- Content-Aware Fill, which continues edges and patterns and can extend an image past its borders

## Painting and retouching
- Brush and eraser with size, hardness and opacity; Shift for straight lines
- 196 MyPaint brushes (pencils, inks, charcoal, paint, smudging, by the MyPaint team, David Revoy, Ramón Miranda, Tanda and others) that follow pen pressure and tilt; pick one from the Brush tool's options bar. Your own `.myb` presets go in the app's `brushes` folder
- Import your brushes (File > Import Brushes, or the button under the brush list): Photoshop `.abr` with its presets and dynamics, Procreate `.brushset` and `.brush` with shape, grain and pencil settings, Clip Studio `.sut` with its tip images, paper textures and settings, or any image as a tip. What a brush uses that cannot be carried over is listed after the import
- Spot Healing Brush and Clone Stamp; both ignore what a layer mask hides
- Smudge, Liquify and Blur, on pixels or masks, and Sharpen
- Warp Cage (Edit ▸ Warp Cage, Photoshop's custom warp): drag the 16 points of a 4 × 4 mesh over a layer with a live preview; pixels bend for good, a smart object keeps it as its own editable Custom warp (written to PSD as Photoshop's), and a shape layer bends as a path
- Vector shape layers (Photoshop's shape layers, editable and written to PSD as such): the Shape tool's rectangle, ellipse, polygon, star, line and custom shapes with fill and stroke (colour, width, alignment, dashes) editable from the options bar; the Pen (P) and Direct Selection (A); the Paths panel (Work Path and saved paths: fill, stroke, load as selection, make from selection, make shape layer) (docs/vector-tools.md)
- Dodge, Burn (Shadows, Midtones, Highlights; Protect Tones) and Sponge (desaturate or saturate), painted through the brush with Exposure or Flow as its opacity; their curves are shaped after Photoshop's behaviour, not measured against it
- Paint Bucket (Shift+G) with Tolerance, Contiguous, Anti-alias and All Layers, inside the selection
- Open camera RAW files (CR2, CR3, NEF, ARW, RAF, ORF, RW2, DNG and more) through LibRaw, developed with the camera's white balance into sRGB; Filter ▸ Camera Raw then adjusts them
- Healing Brush (the healing tool's Sampled type: Alt-click a source, and the copied texture takes the tone around the stroke) and Patch (select the blemish, drag the selection to the area to copy from)
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
- Smart Filters on smart objects, edited as in Photoshop's Layers panel: each filter's settings (double-click), blending options, on/off per filter or for the whole stack, reorder, delete or clear them, and paint, show, invert, disable or delete the shared filter mask; PSD export keeps them ([smart-objects.md](smart-objects.md))
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
- Open Affinity documents (`.afphoto`, `.afdesign`, `.afpub`, and Affinity 3's `.af`) with their pixel layers, groups, masks, clipped layers, opacity, visibility and blend modes; vector shapes and artboards come in as pixels, and text, adjustments and effects are listed as left out (see [affinity-import.md](affinity-import.md))
- Open Aseprite `.ase`/`.aseprite` sprites with the first frame's layers, folders, opacity and blend modes
- Open animated GIFs with each frame as a layer ("Frame N (D ms)", frame 1 visible), and icons (`.ico`, `.cur`) with each size as a layer
- Open PNG, JPEG, TIFF, TGA, WebP and more; drop an image on the canvas to add it as a layer, or on the tab strip to open it
- Projects of up to a gigapixel of layers; the Mac app opens projects up to 100 megapixels
- Export PNG, TIFF, TGA, a multi-size Windows icon (16, 32, 48 and 256 px), or JPEG and WebP with a live preview (WebP keeps transparency, and is lossless at quality 100)
- Crash recovery: unsaved changes are autosaved in the background every few minutes (Preferences sets how often, or turns it off) and offered back after a crash; your own files are never touched
- Several projects in tabs; opening a file from the file manager adds a tab to the running window
- Crop, Canvas Size and Image Size; Image > Trim cuts the canvas to its content (by transparency or a corner's colour, on the sides you choose); rulers and a pixel grid
- An open project follows its package on disk: when another app or an agent writes the `.comp`, the tab reloads in place (a package merely touched, or caught half written, is left alone, and unsaved work is never replaced without asking)

## Automation
- Scripts and AI agents can drive the editor through a socket or MCP; see [automation.md](automation.md)

---

## 日本語

[English](#what-nekophoto-can-do) · **日本語**

Mac 版と同じ機能を、Linux のキー表記で使えます。キーボードショートカットは
[linux-port.md](linux-port.md#keyboard-shortcuts)(英語)に一覧があります。

### レイヤー
- 描画モードと不透明度を持つレイヤーとフォルダー
- レイヤーマスク:描画、塗りつぶし、反転、ぼかし、境界のぼかし。レイヤーとのリンクの切り替え
- クリッピングマスクとフォルダーマスク
- 調整レイヤー:色相・彩度、レベル補正、トーンカーブ、露光量、グラデーションマップ、粒子
- 下のレイヤーと結合、レイヤーを結合、グループを結合(Ctrl+E)
- ドラッグ&ドロップで複製・名前変更・並べ替え・入れ子。開いているプロジェクト間でもレイヤーを移動可能

### 変形
- 解像度を落とさずに移動・拡大縮小・回転・反転
- 自由な形に(Ctrl を押しながらハンドルをドラッグ)
- 複数のレイヤーやフォルダーをまとめて変形
- カンバスや他のレイヤーへのスナップとガイド
- オプションバーで位置・サイズ・角度を数値指定

### 選択範囲
- 長方形選択・楕円選択、なげなわ・多角形選択、自動選択
- クイック選択(Shift+W):被写体をなぞるか、クリックするだけ(クリック用の 48 MB のモデルはオプションバーからダウンロード)。選択範囲は画像の輪郭に合わせて調整されます
- 追加(Shift)、削除(Alt)、共通範囲(Shift+Alt)
- 拡張、縮小、境界をぼかす、滑らかに、境界線、選択範囲を反転。レイヤーやマスクから選択範囲を作成
- コンテンツに応じた塗りつぶし:輪郭や模様をつなげ、画像の外側への拡張にも使えます

### 描画とレタッチ
- サイズ・硬さ・不透明度を指定できるブラシと消しゴム。Shift で直線
- 筆圧と傾きに反応する MyPaint ブラシ 196 種類(鉛筆、インク、木炭、絵の具、ぼかし。MyPaint チーム、David Revoy、Ramón Miranda、Tanda ほか)。ブラシツールのオプションバーから選べます。自作の `.myb` はアプリの `brushes` フォルダーに置けます
- ブラシの読み込み(ファイル > ブラシを読み込み、またはブラシ一覧の下のボタン):Photoshop の `.abr`(プリセットとシェイプダイナミクス)、Procreate の `.brushset`・`.brush`(シェイプ、グレイン、ペンシル設定)、クリップスタジオの `.sut`(先端画像、用紙テクスチャ、設定)、任意の画像を先端として。引き継げなかった設定は読み込み後に表示されます
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
- スマートオブジェクトのスマートフィルターを Photoshop のレイヤーパネルと同じように編集:各フィルターの設定(ダブルクリック)、描画オプション、フィルターごと・全体のオン/オフ、並べ替え、削除・すべて消去、共有フィルターマスクへの描画・表示・反転・無効化・削除。PSD に書き出しても保たれます
- カンバス上でのライブプレビュー(選択範囲があればその中だけ)

### 背景を削除
- 初期状態ではオフ。編集 > 環境設定 でオンにするとモデルを一度だけダウンロードします
- 処理はすべて手元のマシンで行い、画像はどこにも送信されません
- 詳細オプション:輪郭の調整、髪や毛並みの抽出、細かなノイズの除去、輪郭の色の補正、大きな写真向けの高解像度ディテール処理。Quality の「Best」で、髪や毛並み向けの設定をまとめて有効にできます

### ファイルとカンバス
- Photoshop の PSD/PSB を、レイヤー・フォルダー・マスク・描画モード・主な調整レイヤーを保ったまま開けます。引き継げなかった要素は開いた後に一覧表示されます
- レイヤー付きの Photoshop PSD に書き出せます(ファイル > Photoshop ドキュメントとして書き出し):レイヤー、フォルダー、マスク、クリッピング、描画モード、レベル補正・トーンカーブ・露光量・色相/彩度の調整レイヤーと統合画像。Photoshop で再現できない要素は書き出す前に一覧表示されます
- クリップスタジオの `.clip` を、レイヤー・フォルダー・マスク・クリッピング・不透明度・描画モードを保ったまま開けます(ベクターやテキストのレイヤーは画像として読み込みます)
- Aseprite の `.ase`/`.aseprite` を、最初のフレームのレイヤー・フォルダー・不透明度・描画モードを保ったまま開けます
- アニメーション GIF は各フレームをレイヤーとして(「Frame N (D ms)」、フレーム 1 のみ表示)、アイコン(`.ico`、`.cur`)は各サイズをレイヤーとして開けます
- PNG、JPEG、TIFF、TGA、WebP などを開けます。カンバスにドロップするとレイヤーとして追加、タブバーにドロップすると新しいドキュメントとして開きます
- レイヤー合計 1 ギガピクセルまでのプロジェクト(Mac 版で開けるのは 1 億画素まで)
- PNG・TIFF・TGA・複数サイズの Windows アイコン(16/32/48/256 px)書き出し、プレビュー付きの JPEG・WebP 書き出し(WebP は透明部分を保持し、品質 100 で可逆圧縮)
- クラッシュからの復元:未保存の変更を数分ごとにバックグラウンドで自動保存し、異常終了の後に復元を提案します(間隔の変更やオフは環境設定で)。元のファイルには触れません
- タブで複数のプロジェクト。ファイルマネージャーから開いたファイルは起動中のウィンドウにタブとして追加
- 切り抜き、カンバスサイズ、画像解像度。イメージ > トリミングで内容に合わせてカンバスを切り詰め(透明部分または角の色で、選んだ辺のみ)。定規とピクセルグリッド
- 開いているプロジェクトはディスク上の変更に追従します:他のアプリやエージェントが `.comp` を書き換えると、タブがその場で読み込み直します(触れただけの変更や書き込み途中は無視し、未保存の作業は確認なしに置き換えません)

### 自動化
- スクリプトや AI エージェントからソケットまたは MCP 経由で操作できます。詳しくは [automation.md](automation.md)(英語)
