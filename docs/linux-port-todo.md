# Linux port: parity todo

Checked off as each lands on the `linux-port` branch. Ordered by workflow impact.

## Tier 1
- [ ] Move tool drags selected pixels (lift, move, Alt duplicates, Ctrl+T transforms the floating pixels)
- [ ] Free distort (Ctrl-drag a handle; homography resample on Apply; mask carried along)
- [ ] Gradient tool (linear / radial, foreground to background, live preview, opacity)
- [ ] Shape tool (rectangle, rounded rectangle, ellipse; redrawn from the style when scaled)
- [ ] Image Size resamples layer and mask pixels

## Tier 2
- [ ] Blur / Smudge / Liquify tool
- [ ] Group transforms (several selected layers or a folder move in one box)
- [ ] Layers panel: Alt-drag duplicates, Alt-drag a mask copies it, Alt-click clips
- [ ] Hue/Saturation eyedroppers and targeted-adjustment drag
- [ ] Multiple projects in tabs; drag layers between projects

## Tier 3
- [ ] Opacity number keys, Shift-[ / ] hardness, Shift-U shape kind, Shift-M / Shift-L kinds
- [ ] Snapping while resizing; crop ratio presets and Alt symmetric crop
- [ ] Load layer pixels / mask as a selection
- [ ] Deleting a clipping base asks to bake or release
- [ ] Zoom tool cursors, drag-to-zoom out with Alt

## Cannot port as-is
- Remove Background: uses Apple Vision. Left out; an ONNX (U²-Net) backend is possible later.
- Golden images from the Mac build: needs `.comp` files saved on a Mac (see docs/linux-port.md).
