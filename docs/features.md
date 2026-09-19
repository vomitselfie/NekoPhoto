# What compositor-linux can do

The full feature list. It mirrors the Mac app's, with Linux key names.

## Layers
- Layers and folders, with blend modes and opacity
- Layer masks: paint, fill, invert, blur and feather them; link or unlink them to transform a mask on its own
- Clipping masks and folder masks
- Adjustment layers: Hue/Saturation, Levels, Curves, Exposure, Gradient Map and Grain
- Merge Down, Merge Layers and Merge Group (Ctrl+E)
- Duplicate, rename inline, reorder and nest by drag and drop; Alt-drag to duplicate
- New Layer Below (or Ctrl-click the + button) for a fresh background
- Drag layers between open projects

## Transform
- Non-destructive move, scale, rotate and flip; images keep their full resolution however small you make them
- Free distort (Ctrl-drag a handle), with Shift to lock to an axis
- Transform several layers, or a whole folder, together
- Snapping to canvas and layer edges and centers, with guides
- Exact values for position, size, scale and angle, stepped with the arrow keys
- Flip Layer and Flip Canvas, horizontal and vertical

## Selections
- Rectangle and Ellipse Marquee, Freehand and Polygonal Lasso, and Magic Wand
- Add to (Shift), subtract from (Alt) and intersect with (Shift+Alt) selections, move the outline, or move and duplicate the pixels inside
- Magic Wand tolerance, contiguous, sample all layers, and Sample Size (point, 3 by 3, 5 by 5)
- Expand, Contract, Feather (Shift+F6), Smooth, Border and Invert, with Photoshop's circular kernels; load a layer's pixels or a mask as a selection
- Delete or Backspace clears the selected pixels
- Content-Aware Fill (PatchMatch synthesis, so edges and patterns continue), which can also extend an image past its edges

## Painting and retouching
- Brush with size, hardness and opacity, `[` and `]` for size, digits for opacity, Shift for straight lines
- Spot Healing Brush (content-aware)
- Clone Stamp, aligned or not, sampling one layer or all of them
- Smudge, Liquify and Blur, on pixels or masks
- Gradient tool and Shape tool (rectangles, rounded rectangles and ellipses)
- Text tool: click to add text in any installed font (size, bold, italic, colour, alignment, line and letter spacing), editable later (double-click the text on the canvas or its thumbnail in the Layers panel, or Layer > Edit Text…) as long as the layer is not painted on; text layers stay pixels in the project so the Mac app opens them. The font picker folds families that share a name (the hundreds of Noto variants) into one expandable row, filters as you type, and keeps recent fonts on top
- Eyedropper and a full colour picker; X swaps the colours, D resets them

## Adjustments and filters
- Levels (with Auto), Curves, Hue/Saturation, Exposure, Gradient Map, Grain and Invert
- Gaussian Blur and Motion Blur that spread past a layer's edges
- Add Noise, Lens Correction and Remove Background
- Filter > G'MIC (Ctrl+Shift+G): the G'MIC filter framework's catalogue (over 700 filters once downloaded) with auto-built controls, live preview and an editable command line; needs the `gmic` package (or libgmic in-process, opt-in)
- Live previews, limited to the selection when there is one

## Canvas and files
- Multiple projects in tabs
- Rulers (Ctrl+R), pixel grid when zoomed in, sharp downsampling when zoomed out
- Crop with snapping and ratio presets; Alt for symmetric cropping
- Canvas Size (with anchor and relative mode) and Image Size (with scale and resampling choice: Lanczos-3, triangle or nearest)
- Import JPEG, PNG, TIFF and WebP, including dropped images from other apps
- Open Photoshop PSD and PSB files (File > Open Photoshop File…, or drop one on the window): layers with names, positions, opacity, blend modes, visibility, folders, clipping and masks; Levels, Curves, Hue/Saturation, Exposure and Gradient Map adjustment layers become ours; solid fills, text and smart objects arrive as pixels; 16- and 32-bit files are reduced to 8 bits; anything left behind (layer styles, vector masks, other adjustments) is listed after the import
- Export PNG, or JPEG with a live preview; Copy Merged
- `.comp` projects open in the Mac app and vice versa
- Photoshop-style keyboard shortcuts throughout; `docs/linux-port.md` lists them

## Remove Background
Off by default. Edit > Preferences turns it on and downloads a segmentation
model (IS-Net, the U2Net portrait model or the small U2Net from the rembg
project, or PP-HumanSeg from OpenCV's model zoo, all Apache-2.0) into the app's
data folder. The model runs locally through OpenCV's DNN module; nothing is
uploaded. The Advanced panel refines the mask against the image's edges and can
solve hair opacity in a band around the edge (Matting).

## Automation
An automation socket and an MCP bridge let scripts and AI agents drive the
editor; see `docs/automation.md`.
