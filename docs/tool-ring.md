# The Tool Ring

**English** · [日本語](#日本語)

View ▸ Tool Ring (**Alt+D**) shows a compass of quick actions for the current tool next to the pointer. Its nine
circles sit as the keys do around D on the keyboard:

```
  W   E   R
  S  (D)  F
  X   C   V
```

The centre (D) shows the tool; each circle shows its key, an icon and, for a value, the value now ("60 px", "80%").
Press a circle's letter to use it. Values follow one rule: **towards F and R (right, up) is more, towards S and V
(left, down) is less**, so each value takes one of the pairs S/F, X/E or V/R and a "+" or "−" beside the key says
which end it is. Holding a key repeats it; the mouse wheel, while the ring is open, steps the value used last.

The ring is a toggle: it stays open after Alt+D and closes on Esc, on Alt+D again, on choosing another tool, or a few
seconds after the last ring key (Edit ▸ Preferences ▸ Tool Ring: 3 seconds unless changed; "Never" keeps it open).
While it is open its nine letters belong to it (V does not pick the Move tool, X does not swap the colours, F does not
change the screen mode); every other key and every modifier works as usual, so Ctrl+Z, Space, Alt's Eyedropper and
the digit keys are untouched.

**The mouse is never interrupted.** The ring takes no clicks (they go to the canvas under it), never takes the
keyboard focus, and can open in the middle of a brush stroke without ending it. It appears near the pointer, away
from where the pointer is heading, inside the canvas and clear of the brush outline, and moves aside only when the
pointer comes at it, so it does not follow every small movement. It works in Full Screen Mode too, over the canvas.

A greyed circle cannot run now; pressing its key shows why under the ring (no other brush preset used yet, a brush
preset that sets its own hardness, a stroke being painted). Changing a tool option (size, opacity, feather) is not an
undo step, as in Photoshop; a circle that edits the document (Flip, Rotate, Bring Forward, Inverse, Deselect) is one
undo step, as its menu command is.

## The rings

| Tool | W | E | R | S | D | F | X | C | V |
|---|---|---|---|---|---|---|---|---|---|
| Brush, Eraser | Previous brush preset | Hardness + | Opacity + | Size − | (tool name) | Size + | Hardness − | Brush ↔ Eraser | Opacity − |
| Spot Healing, Clone Stamp, Blur/Smudge, Dodge/Burn | Fit on Screen | Hardness + | Opacity + | Size − | (tool name) | Size + | Hardness − | Show or hide panels | Opacity − |
| Marquee, Lasso | Deselect | New Selection | Add to Selection | Feather − | Quick Mask | Feather + | Intersect | Inverse | Subtract |
| Magic Wand | Deselect | New Selection | Add to Selection | Tolerance − | Quick Mask | Tolerance + | Intersect | Inverse | Subtract |
| Quick Select | Deselect | Clear the strokes | Mark the subject | Size − | Quick Mask | Size + | Remove the last stroke | Inverse | Mark the background |
| Move | Flip Horizontal | Free Transform | Bring Forward | Rotate 90° left | (tool name) | Rotate 90° right | Flip Vertical | Auto-Select | Send Backward |
| Type | Bold | Next recent font | Italic | Size − | (tool name) | Size + | Previous recent font | Alignment (left, centre, right) | |
| Paint Bucket | Contiguous | All Layers | Opacity + | Tolerance − | (tool name) | Tolerance + | Swap colours | Default colours | Opacity − |
| Gradient | Reverse | Fit on Screen | Opacity + | Zoom out | (tool name) | Zoom in | Swap colours | Default colours | Opacity − |
| Every other tool | Show or hide panels | Fit on Screen | 100% | Zoom out | (tool name) | Zoom in | Swap colours | Default colours | |

The size steps are Photoshop's `[` and `]`; opacity moves in tens (as the digit keys) down to 1%; hardness in
quarters (as Shift+`[` `]`); Feather through 0, 1, 2, 3, 5, 8, 10, 15, 20, 30, 50, 75, 100, 150, 200, 250 px;
Tolerance in fives. With a type layer selected, the Type ring's size, font, bold, italic and alignment change that
layer (one undo step each, presses in quick succession merging as Ctrl+Shift+> does), otherwise they set what new type
takes.

The selection mode and Feather are also in the options bar, as Photoshop's four mode buttons and its Feather field:
the mode chosen there is what a plain drag does (Shift, Alt and Shift+Alt still choose for one drag), and Feather
softens each new marquee or lasso shape before it combines with the selection.

Every circle runs a command of Edit ▸ Keyboard Shortcuts (the ring's own steps are under Tools and Other Keys:
Increase Brush Size, Decrease Feather and so on, with no keys until you give them some), and the rings are defined as
data in one place (`src/app/ToolRingCommands.cpp`), ready for a later version to let you choose your own.

## Tablets

**Pen buttons.** Edit ▸ Preferences ▸ Tool Ring ▸ Pen button can make one of the pen's side buttons open and close
the ring (off by default): the lower button (which pans otherwise) or the upper one.

**ExpressKeys and other tablet keys** belong to the tablet driver, not to applications, so NekoPhoto cannot take them
over. Map them in the driver to keystrokes instead:

- one key to **Alt+D** (the ring), and
- others to the ring's letters, for example **F** and **S** for bigger and smaller, **R** and **V** for more and less
  opacity. With the ring closed those keys are the ordinary tool shortcuts again, so map them only if that suits you,
  or map them to the ring's commands' own keys after giving them some in Edit ▸ Keyboard Shortcuts.

Where to map them: on Linux, the desktop's tablet settings (GNOME Settings ▸ Wacom Tablet, KDE System Settings ▸
Graphic Tablet), `xsetwacom set "<pad name>" Button 1 "key +alt d -alt"` on X11, or OpenTabletDriver's bindings
(Keyboard binding: `LeftAlt+D`); on Windows, the Wacom Tablet Properties (Application-specific settings for
nekophoto.exe: Keystroke… Alt+D), XP-Pen's or Huion's driver panel likewise.

---

## 日本語

表示 ▸ ツールリング(**Alt+D**)は、現在のツールのよく使う操作をポインターのそばにコンパスの形で表示します。9 つの円は、
キーボード上で D の周りに並ぶキーと同じ配置です:

```
  W   E   R
  S  (D)  F
  X   C   V
```

中央(D)はツールを表し、周りの円にはキー、アイコン、値のある操作には現在の値(「60 px」「80%」)が表示されます。円の文字キーを
押すとその操作をします。値はひとつの規則に従います:**F と R(右・上)の方向が大きく、S と V(左・下)の方向が小さく**。
値はそれぞれ S/F、X/E、V/R のどれかの組を使い、キーの横の「+」「−」がどちらの端かを示します。キーを押し続けると繰り返し、
リングを開いている間はマウスホイールで最後に使った値を変えられます。

リングは押すたびに開閉します:Alt+D のあとは開いたままで、Esc、もう一度 Alt+D、別のツールを選んだとき、または最後のリングの
キーから数秒後(編集 ▸ 環境設定 ▸ ツールリング:初期設定は 3 秒、「閉じない」で開いたまま)に閉じます。開いている間は 9 つの
文字キーがリングのものになり(V で移動ツールに切り替わらず、X で描画色と背景色が入れ替わらず、F でスクリーンモードが
変わりません)、ほかのキーと修飾キーはいつもどおり使えます。Ctrl+Z、Space、Alt のスポイト、数字キーもそのままです。

**マウスの操作は決して妨げません。** リングはクリックを受け取らず(下のカンバスに届きます)、キーボードのフォーカスも
取らず、ブラシのストロークの途中で開いてもストロークは途切れません。ポインターの近く、ポインターの進む方向の反対側、
カンバスの内側でブラシの輪郭にかからない位置に現れ、ポインターが近づいたときだけ脇へよけるので、小さな動きのたびに
ついて回ることはありません。フルスクリーンモードでもカンバスの上に表示されます。

グレーの円はいまは使えません。そのキーを押すと理由がリングの下に表示されます(ほかのブラシプリセットをまだ使っていない、
硬さを自分で決めるブラシプリセット、ストロークを描画中など)。ツールオプション(直径、不透明度、ぼかし)の変更は Photoshop と
同じく取り消しの単位になりません。ドキュメントを編集する円(反転、回転、前面へ、選択範囲を反転、選択を解除)はメニューの
コマンドと同じく 1 つの取り消し単位です。

### リング

| ツール | W | E | R | S | D | F | X | C | V |
|---|---|---|---|---|---|---|---|---|---|
| ブラシ、消しゴム | 前のブラシプリセット | 硬さ + | 不透明度 + | 直径 − | (ツール名) | 直径 + | 硬さ − | ブラシ ↔ 消しゴム | 不透明度 − |
| スポット修復ブラシ、コピースタンプ、ぼかし/指先、覆い焼き/焼き込み | 画面サイズに合わせる | 硬さ + | 不透明度 + | 直径 − | (ツール名) | 直径 + | 硬さ − | パネルの表示/非表示 | 不透明度 − |
| 選択範囲ツール、なげなわ | 選択を解除 | 新しい選択範囲 | 選択範囲に追加 | ぼかし − | クイックマスク | ぼかし + | 共通範囲 | 選択範囲を反転 | 一部削除 |
| 自動選択 | 選択を解除 | 新しい選択範囲 | 選択範囲に追加 | 許容値 − | クイックマスク | 許容値 + | 共通範囲 | 選択範囲を反転 | 一部削除 |
| クイック選択 | 選択を解除 | ストロークを消去 | 被写体を描く | 直径 − | クイックマスク | 直径 + | 最後のストロークを取り消す | 選択範囲を反転 | 背景を描く |
| 移動 | 水平方向に反転 | 自由変形 | 前面へ | 90° 左に回転 | (ツール名) | 90° 右に回転 | 垂直方向に反転 | 自動選択 | 背面へ |
| 文字 | 太字 | 次の最近使ったフォント | 斜体 | サイズ − | (ツール名) | サイズ + | 前の最近使ったフォント | 揃え(左・中央・右) | |
| 塗りつぶし | 隣接 | 全レイヤー | 不透明度 + | 許容値 − | (ツール名) | 許容値 + | 描画色と背景色を入れ替え | 初期設定の描画色と背景色 | 不透明度 − |
| グラデーション | 逆順 | 画面サイズに合わせる | 不透明度 + | ズームアウト | (ツール名) | ズームイン | 描画色と背景色を入れ替え | 初期設定の描画色と背景色 | 不透明度 − |
| そのほかのツール | パネルの表示/非表示 | 画面サイズに合わせる | 100% | ズームアウト | (ツール名) | ズームイン | 描画色と背景色を入れ替え | 初期設定の描画色と背景色 | |

直径は Photoshop の `[` `]` と同じ刻み、不透明度は数字キーと同じく 10% ずつ(最小 1%)、硬さは Shift+`[` `]` と同じく 25% ずつ、
ぼかしは 0、1、2、3、5、8、10、15、20、30、50、75、100、150、200、250 px、許容値は 5 ずつ変わります。文字レイヤーを選んで
いると、文字リングのサイズ・フォント・太字・斜体・揃えはそのレイヤーを変えます(それぞれ 1 つの取り消し単位で、続けて押すと
Ctrl+Shift+> と同じく 1 つにまとまります)。選んでいなければ、新しく入力する文字の設定になります。

選択範囲の作成方法とぼかしはオプションバーにもあります(Photoshop の 4 つのボタンと「ぼかし」欄):そこで選んだ方法が
普通のドラッグの動作になり(Shift、Alt、Shift+Alt はそのドラッグだけ優先されます)、ぼかしは新しい長方形・楕円・なげなわの
形を選択範囲と組み合わせる前にぼかします。

どの円も 編集 ▸ キーボードショートカット のコマンドを実行します(リング専用の操作は「ツール」と「その他のキー」にあり、
ブラシの直径を大きく、ぼかしを小さく などはキーを割り当てるまでキーがありません)。リングの中身は 1 か所
(`src/app/ToolRingCommands.cpp`)にデータとしてまとめてあり、のちのバージョンで自分で選べるようにする準備ができています。

### タブレット

**ペンのボタン。** 編集 ▸ 環境設定 ▸ ツールリング ▸ ペンのボタン で、ペンのサイドスイッチの 1 つでリングを開閉できます
(初期設定はオフ):下のスイッチ(普段はスクロール)か上のスイッチです。

**ファンクションキー(ExpressKey など)** はアプリケーションではなくタブレットのドライバーが扱うため、NekoPhoto から
変えることはできません。ドライバーでキー入力に割り当ててください:

- 1 つを **Alt+D**(リング)に、
- ほかをリングの文字キーに。たとえば **F** と **S** で大きく・小さく、**R** と **V** で不透明度を上げる・下げる。リングを
  閉じているとそれらはいつものツールのショートカットになるので、それでよければ割り当てるか、編集 ▸ キーボードショートカット
  でリングのコマンドにキーを割り当て、そのキーをドライバーで使ってください。

割り当てる場所:Linux ではデスクトップのタブレットの設定(GNOME 設定 ▸ Wacom タブレット、KDE システム設定 ▸ ペンタブレット)、
X11 では `xsetwacom set "<パッド名>" Button 1 "key +alt d -alt"`、または OpenTabletDriver のバインディング(Keyboard の
`LeftAlt+D`)。Windows ではワコム タブレットのプロパティ(nekophoto.exe 用のアプリケーション別設定:キーストローク… Alt+D)、
XP-Pen や Huion のドライバーの設定画面も同様です。
