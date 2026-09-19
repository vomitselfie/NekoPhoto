# Linux port: parity todo

Checked off as each lands on the `linux-port` branch. Ordered by workflow impact.

## Tier 1
- [x] Move tool drags selected pixels (lift, move, Alt duplicates, Ctrl+T transforms the floating pixels)
- [x] Free distort (Ctrl-drag a handle; homography resample on Apply; mask carried along)
- [x] Gradient tool (linear / radial, foreground to background, live preview, opacity)
- [x] Shape tool (rectangle, rounded rectangle, ellipse; redrawn from the style when scaled)
- [x] Image Size resamples layer and mask pixels

## Tier 2
- [x] Blur / Smudge / Liquify tool
- [x] Group transforms (several selected layers or a folder move in one box)
- [x] Layers panel: Alt-drag duplicates, Alt-drag a mask copies it, Alt-click clips
- [x] Hue/Saturation eyedroppers and targeted-adjustment drag
- [x] Multiple projects in tabs; drag layers between projects

## Tier 3
- [x] Opacity number keys, Shift-[ / ] hardness, Shift-U shape kind, Shift-M / Shift-L kinds
- [x] Snapping while resizing; crop ratio presets and Alt symmetric crop
- [x] Load layer pixels / mask as a selection
- [x] Deleting a clipping base asks to bake or release
- [x] Zoom tool cursors, drag-to-zoom out with Alt

## Done differently
- [x] Remove Background: IS-Net ONNX through OpenCV DNN, with the Mac's refinement controls (Apple Vision has no Linux equivalent).
- Golden images from the Mac build: needs `.comp` files saved on a Mac (see docs/linux-port.md).
