# What NekoPhoto can do

**English** · [日本語](#日本語)

Everything the Mac app does, with Linux key names. Keyboard shortcuts are
listed in [linux-port.md](linux-port.md#keyboard-shortcuts).

## Layers
- Layers and folders with blend modes and opacity
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
- Quick Select (Q): scribble over the subject, or click it (a 48 MB model, downloaded from the options bar); the selection snaps to the image's edges
- Add (Shift), subtract (Alt) and intersect (Shift+Alt)
- Expand, Contract, Feather, Smooth, Border, Invert; load a layer or mask as a selection
- Content-Aware Fill, which continues edges and patterns and can extend an image past its borders

## Painting and retouching
- Brush and eraser with size, hardness and opacity; Shift for straight lines
- 196 MyPaint brushes (pencils, inks, charcoal, paint, smudging, by the MyPaint team, David Revoy, Ramón Miranda, Tanda and others) that follow pen pressure and tilt; pick one from the Brush tool's options bar. Your own `.myb` presets go in the app's `brushes` folder
- Import your brushes (File > Import Brushes, or the button under the brush list): Photoshop `.abr` with its presets and dynamics, Procreate `.brushset` and `.brush` with shape, grain and pencil settings, Clip Studio `.sut` with its tip images, paper textures and settings, or any image as a tip. What a brush uses that cannot be carried over is listed after the import
- Spot Healing Brush and Clone Stamp; both ignore what a layer mask hides
- Smudge, Liquify and Blur, on pixels or masks
- Gradients and shapes (rectangles, rounded rectangles, ellipses)
- Text in any installed font, editable until you paint on the layer
- Eyedropper and colour picker

## Adjustments and filters
- Seventeen adjustment layers, also as destructive adjustments: Levels, Curves, Hue/Saturation, Exposure, Gradient Map, Grain, and Photoshop's Invert, Brightness/Contrast, Posterize, Threshold, Color Balance, Black & White, Vibrance, Photo Filter, Channel Mixer, Selective Color and Color Lookup (.cube, .3dl and ICC LUTs), read from and written to PSD as Photoshop's own ([adjustment-layers.md](adjustment-layers.md))
- Levels (with Auto), Curves, Hue/Saturation, Exposure, Gradient Map, Grain, Invert
- Gaussian Blur and Motion Blur, which spread past a layer's edges
- Add Noise and Lens Correction
- Filter > Camera Raw Filter (Shift+Ctrl+A): white balance, tone, presence, curve, colour mixer, colour grading, detail, optics, geometry, effects and calibration on a layer's pixels ([camera-raw.md](camera-raw.md))
- Filter > G'MIC: over 850 filters with their own controls, when `gmic` is installed (Update Filters fetches the catalogue; the few that cannot work here, such as those that resize the image or make layers, are hidden unless Show all filters is on)
- Live previews on the canvas, limited to the selection when there is one

## Remove Background
- Off until you turn it on in Edit > Preferences, which downloads a model once
- Runs on your machine; nothing is uploaded
- Advanced options: refine the edges, solve hair and fur, remove speckles, clean the edge colours, and a detail pass at full resolution for large photos; Quality > Best turns on everything for hair and fur in one step

## Files and canvas
- Open Photoshop PSD and PSB files with their layers, folders, masks, blend modes, most adjustment layers, layer styles drawn as Photoshop draws them (docs/layer-styles.md), folders isolated and faded as in Photoshop, smart objects that keep their source (docs/smart-objects.md), vector masks and shape layers drawn from their paths (docs/vector-masks.md) and simple text as editable text; what cannot be kept is listed after opening
- Export layered Photoshop PSD files (File > Export as Photoshop Document): layers, folders, masks, clipping, blend modes and Levels, Curves, Exposure and Hue/Saturation adjustment layers, with a merged image; text layers as editable Photoshop text; a PSD you opened keeps its layer styles, editable text, smart objects and vector masks on the way back out while they still match their layers (docs/psd-roundtrip.md); anything Photoshop cannot carry is listed before you export (docs/psd-export.md)
- Open Clip Studio `.clip` projects with their layers, folders, masks, clipping, opacity and blend modes (vector and text layers come in as their pixels)
- Open PNG, JPEG, TIFF, WebP and more; drop an image on the canvas to add it as a layer, or on the tab strip to open it
- Projects of up to a gigapixel of layers; the Mac app opens projects up to 100 megapixels
- Export PNG, TIFF, or JPEG and WebP with a live preview (WebP keeps transparency, and is lossless at quality 100)
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
- クイック選択(Q):被写体をなぞるか、クリックするだけ(クリック用の 48 MB のモデルはオプションバーからダウンロード)。選択範囲は画像の輪郭に合わせて調整されます
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
- カンバス上でのライブプレビュー(選択範囲があればその中だけ)

### 背景を削除
- 初期状態ではオフ。編集 > 環境設定 でオンにするとモデルを一度だけダウンロードします
- 処理はすべて手元のマシンで行い、画像はどこにも送信されません
- 詳細オプション:輪郭の調整、髪や毛並みの抽出、細かなノイズの除去、輪郭の色の補正、大きな写真向けの高解像度ディテール処理。Quality の「Best」で、髪や毛並み向けの設定をまとめて有効にできます

### ファイルとカンバス
- Photoshop の PSD/PSB を、レイヤー・フォルダー・マスク・描画モード・主な調整レイヤーを保ったまま開けます。引き継げなかった要素は開いた後に一覧表示されます
- レイヤー付きの Photoshop PSD に書き出せます(ファイル > Photoshop ドキュメントとして書き出し):レイヤー、フォルダー、マスク、クリッピング、描画モード、レベル補正・トーンカーブ・露光量・色相/彩度の調整レイヤーと統合画像。Photoshop で再現できない要素は書き出す前に一覧表示されます
- クリップスタジオの `.clip` を、レイヤー・フォルダー・マスク・クリッピング・不透明度・描画モードを保ったまま開けます(ベクターやテキストのレイヤーは画像として読み込みます)
- PNG、JPEG、TIFF、WebP などを開けます。カンバスにドロップするとレイヤーとして追加、タブバーにドロップすると新しいドキュメントとして開きます
- レイヤー合計 1 ギガピクセルまでのプロジェクト(Mac 版で開けるのは 1 億画素まで)
- PNG・TIFF 書き出し、プレビュー付きの JPEG・WebP 書き出し(WebP は透明部分を保持し、品質 100 で可逆圧縮)
- クラッシュからの復元:未保存の変更を数分ごとにバックグラウンドで自動保存し、異常終了の後に復元を提案します(間隔の変更やオフは環境設定で)。元のファイルには触れません
- タブで複数のプロジェクト。ファイルマネージャーから開いたファイルは起動中のウィンドウにタブとして追加
- 切り抜き、カンバスサイズ、画像解像度。イメージ > トリミングで内容に合わせてカンバスを切り詰め(透明部分または角の色で、選んだ辺のみ)。定規とピクセルグリッド
- 開いているプロジェクトはディスク上の変更に追従します:他のアプリやエージェントが `.comp` を書き換えると、タブがその場で読み込み直します(触れただけの変更や書き込み途中は無視し、未保存の作業は確認なしに置き換えません)

### 自動化
- スクリプトや AI エージェントからソケットまたは MCP 経由で操作できます。詳しくは [automation.md](automation.md)(英語)
