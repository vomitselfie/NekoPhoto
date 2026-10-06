# Where NekoPhoto deliberately works differently

**English** · [日本語](#日本語)

NekoPhoto follows Photoshop's tools and shortcuts closely, but a few features work differently on purpose. Some of
the ways Photoshop does these things are covered by patents that are still in force, so NekoPhoto uses older,
published methods instead, or leaves the feature out. This page lists what you will notice. It is a description of
the program, not legal advice.

## Selections

- **Quick Select updates when you release the mouse.** While you drag, you see your stroke; the selection is worked
  out once, when you let go. Photoshop grows the selection while you are still dragging.
- **Quick Select's edge refinement is a smoothing of the outline.** The Refine value smooths the selection's shape;
  it does not pull the edge onto the image's colours.

## Content-Aware Fill, healing and Content-Aware Move

- **Content-Aware Fill uses classic exemplar inpainting.** It fills the selection from its edge inwards, and for
  each small patch it looks through every candidate in a window around it (about 100 pixels each way, or about 200
  with *Wide Area*) and copies the best match, then evens out the tone. The same selection always gives the same
  result. Sources are never rotated, scaled or mirrored, and nothing is copied from beyond the window, so a fill
  that needs material from far away can look different from Photoshop's.
- **There is no hand-painted sampling area.** Content-Aware Fill offers *Auto* and *Wide Area*. Photoshop's Custom
  sampling brush (and the `custom` sampling of `pixels.contentAwareFill`) is not available.
- **Healing shows its result when you release the mouse.** While you paint with the Spot Healing Brush or the Healing
  Brush you see the stroke; the healed result appears when you let go.
- **Spot Healing's Proximity Match** looks through every nearby position in a fixed window and takes the best
  match, so the same spot always heals the same way.
- **Remove Background's edge matting** chooses foreground and background colour samples for each edge pixel on its
  own, from a fixed set of the samples closest in colour. Hair and fur edges can come out slightly different from
  earlier versions.

## Brushes

- **Brush textures (grain) stay fixed to the canvas.** A texture never travels with the stroke or turns with each
  dab, and pen pressure, speed, tilt or direction do not change its depth or angle. Procreate brushes with *moving*
  grain import with their grain fixed to the canvas (as *texturized* grain), and the import report says so.
- **Opacity and flow never follow the pen's speed.** Size, spacing and the other shape settings can still follow
  speed. Procreate's *speed → opacity* setting is not imported, and the "mouse speed as pressure" option paints at
  full pressure. MyPaint presets that tie opacity to speed paint without that link.
- **The Smudge tool carries one colour.** It picks up the average colour under the brush and keeps blending it along
  the stroke, rather than dragging a copy of the pixels. Long smudges look softer than Photoshop's.
- **MyPaint presets** paint with one smudge colour and ordinary colour mixing: the multiple smudge buckets and the
  spectral paint mode of some MyPaint 2 presets are not used.

## G'MIC

- **G'MIC's patch-based inpainting and patch-matching filters are not offered.** The filter browser leaves them out
  (even with *Show all filters*), and commands that use them are refused when typed or run by automation. G'MIC's
  other filters, including its diffusion-based inpainting, are unaffected.

## Names

Adobe and Photoshop are trademarks of Adobe Inc. NekoPhoto is not affiliated with or endorsed by Adobe; Photoshop is
named only to describe compatibility. Clip Studio Paint is a trademark of CELSYS, Inc., Procreate of Savage
Interactive Pty Ltd, and MyPaint, Krita, GIMP and G'MIC belong to their respective projects.

---

## 日本語

[English](#where-nekophoto-deliberately-works-differently) · **日本語**

# NekoPhoto が意図的に Photoshop と異なる動作をする機能

NekoPhoto は Photoshop のツールやショートカットにできるだけ合わせていますが、いくつかの機能は意図的に異なる動作をします。
Photoshop の方式の一部は現在も有効な特許で保護されているため、NekoPhoto では公開済みの古い手法を使うか、その機能を
省いています。ここでは使っていて気づく違いをまとめます。プログラムの説明であり、法的な助言ではありません。

## 選択範囲

- **クイック選択はマウスを離したときに更新されます。** ドラッグ中はストロークだけが表示され、離したときに一度だけ選択範囲を
  計算します。Photoshop はドラッグ中に選択範囲を広げていきます。
- **クイック選択の境界の調整は輪郭の平滑化です。** 「調整」の値は選択範囲の形をなめらかにするもので、画像の色に合わせて
  境界を動かすことはしません。

## コンテンツに応じた塗りつぶし・修復・コンテンツに応じた移動

- **コンテンツに応じた塗りつぶしは、古典的な exemplar ベースの修復を使います。** 選択範囲の縁から内側へ埋めていき、小さな
  パッチごとに周囲の範囲(各方向に約 100 ピクセル、「広い範囲」では約 200 ピクセル)の候補をすべて調べて最も合うものを
  コピーし、最後に色調をそろえます。同じ選択範囲からは常に同じ結果になります。コピー元を回転・拡大縮小・反転することは
  なく、範囲より遠くからはコピーしないため、離れた場所の素材が必要な場合は Photoshop と結果が異なることがあります。
- **手で塗るサンプリング範囲はありません。** 「自動」と「広い範囲」を選べます。Photoshop のカスタムのサンプリングブラシ
  (`pixels.contentAwareFill` の `custom`)は使えません。
- **修復はマウスを離したときに結果が表示されます。** スポット修復ブラシや修復ブラシで描いている間はストロークが表示され、
  離したときに修復結果が現れます。
- **スポット修復の「近似色に合わせる」** は決まった範囲の近くの位置をすべて調べて最も合うものを使うので、同じ場所は常に
  同じように修復されます。
- **背景を削除の境界のマット処理** は、境界の各ピクセルごとに、色の近い決まった数のサンプルから前景と背景の色を選びます。
  髪や毛の境界が以前のバージョンと少し異なることがあります。

## ブラシ

- **ブラシのテクスチャ(グレイン)はカンバスに固定されます。** テクスチャがストロークと一緒に動いたり、描点ごとに回転した
  りすることはなく、筆圧・速度・傾き・方向でテクスチャの深さや角度は変わりません。Procreate の「移動」グレインのブラシは、
  グレインをカンバスに固定して(「テクスチャ化」グレインとして)読み込み、読み込みの報告にその旨が表示されます。
- **不透明度と流量はペンの速度に従いません。** サイズや間隔などの形の設定は今までどおり速度に従います。Procreate の
  「速度 → 不透明度」は読み込まれず、「マウスの速度を筆圧として使う」設定は筆圧最大で描きます。不透明度を速度に結び付けた
  MyPaint のプリセットは、その結び付きなしで描きます。
- **指先ツールは 1 色を運びます。** ブラシの下の平均色を拾い、ストロークに沿ってその色を混ぜていきます。ピクセルの写しを
  引きずるわけではないため、長くこすると Photoshop より柔らかくなります。
- **MyPaint のプリセット** は 1 つのにじみ色と通常の混色で描きます。MyPaint 2 の一部のプリセットにある複数のにじみバケツと
  スペクトル混色は使いません。

## G'MIC

- **G'MIC のパッチベースの修復(inpaint)とパッチマッチングのフィルターは表示されません。** フィルターブラウザーには
  (「Show all filters」をオンにしても)表示されず、それらを使うコマンドは入力しても自動化から実行しても拒否されます。
  拡散ベースの修復を含む、その他の G'MIC フィルターには影響しません。

## 名称

Adobe と Photoshop は Adobe Inc. の商標です。NekoPhoto は Adobe とは関係がなく、Adobe の承認も受けていません。Photoshop の
名前は互換性を説明するためだけに使っています。CLIP STUDIO PAINT は株式会社セルシスの、Procreate は Savage Interactive Pty Ltd
の商標で、MyPaint・Krita・GIMP・G'MIC はそれぞれのプロジェクトのものです。
