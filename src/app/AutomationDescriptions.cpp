// What each automation method takes: the table `rpc.describe` answers from and `handle` checks request keys
// against. One entry per registered method (the smoke test calls rpc.describe for every name rpc.methods
// lists). A parameter is `key:type` with `!` when required and `=default`, then a short description;
// entries are separated by `;`. Types: string, number, integer, bool, object, array, color (CSS), layer (a
// layer id from layers.list), or one of a fixed set: `(a|b|c)` spelled out, `<blend>` etc. read from the code.
#include "Automation.h"
#include "AutomationHandlers.h"
#include "compositor/document.h"
#include "compositor/filters.h"
#include "compositor/transform.h"
#include <QRegularExpression>
#include <map>

using namespace compositor;

namespace app::rpc {

namespace {

struct MethodDoc {
    const char* name;
    const char* summary;
    const char* params;
};

const MethodDoc methodDocs[] = {
    // app and tabs
    {"rpc.methods", "Every method name.", ""},
    {"rpc.describe", "What a method does and takes: parameters with types, defaults and valid values. Without a method, a one-line summary of every method.",
     "method:string The method to describe, e.g. layers.set"},
    {"app.info", "The editor's version and protocolVersion, the socket, the number of tabs, and what can run: removeBackground and clickSelect (their models are downloaded), scribble.", ""},
    {"events.subscribe", "Push event lines on this connection when something changes; clients skip them while waiting for replies.",
     "kinds:array Kinds to receive: document, layers, selection, history, tool, view, tabs (default all)"},
    {"events.unsubscribe", "Stop the event lines on this connection.", ""},
    {"tabs.list", "The open tabs: index, title, whether current, modified, path and size.", ""},
    {"tabs.select", "Make a tab current; every other method works on the current tab.", "index:integer! Tab index from tabs.list"},
    {"tabs.new", "Open an empty tab and make it current.", ""},
    {"tabs.close", "Close a tab.", "index:integer! Tab index from tabs.list; discard:bool=false Close even with unsaved changes"},
    // history
    {"history.info", "Whether undo and redo are possible and the names of the next steps.", ""},
    {"history.list", "Every recorded edit: undo oldest first, redo next first.", ""},
    {"history.undo", "Undo edits.", "steps:integer=1 How many"},
    {"history.redo", "Redo edits.", "steps:integer=1 How many"},
    {"history.beginGroup", "Start an edit group: the steps this connection records until history.endGroup become one undo step with this name.",
     "name:string! What Undo will call the step, e.g. Retouch by agent"},
    {"history.endGroup", "Close the edit group and merge its steps into one. They stay separate when someone else edited the document meanwhile (the reply says so).", ""},
    {"rpc.batch", "Run calls in order in one request, stopping at the first error. With a name they become one undo step, and an error takes back what the earlier calls did.",
     "calls:array! {\"method\": ..., \"params\": {...}} objects; name:string Make the calls one undo step with this name, all or nothing"},
    // tools and view
    {"tool.select", "Pick the tool the person sees.", "name:<tool>! The tool"},
    {"colors.set", "Set the foreground and background colours.", "foreground:color Foreground; background:color Background"},
    {"view.zoom", "Zoom the view (not the document).", "zoom:number Zoom factor, 1 = 100%; fit:bool=false Fit the document in the window"},
    {"debug.eye", "Test hook: a pointer event on a layer's eye button in the Layers panel.",
     "id:layer! The layer whose eye is pressed; to:layer The eye the pointer is over; action:(press|move|release)! The event"},
    // documents and canvas
    {"document.info", "The current document: size, resolution, layer count, active layer, path, whether modified.", ""},
    {"document.overview", "The document at a glance, as text: size, selection, undo, and the layer tree top first with each layer's kind, bounds, opacity, blend, mask and id.",
     "maxLayers:integer=80 List at most this many layers"},
    {"document.new", "A new document in the current tab (or a new tab if this one has a document).",
     "width:integer=1920 Pixels; height:integer=1080 Pixels; resolution:number=72 Pixels per inch; emptyLayer:bool=true Start with a blank pixel layer"},
    {"document.open", "Open a project (.comp), Photoshop (.psd/.psb), Clip Studio (.clip) or image file. With a document already open, an image is imported as a layer.",
     "path:string! File path"},
    {"document.import", "Import an image file as a new layer.", "path:string! File path; x:number Left edge in document pixels; y:number Top edge"},
    {"document.save", "Save as a project (.comp).", "path:string Where to save (default: where it was opened or last saved)"},
    {"document.export", "Export as a layered Photoshop .psd (the reply lists what Photoshop cannot carry), or the composite as .png, .jpg, .webp or .tif (the extension decides).",
     "path:string! Output file; quality:integer JPEG and WebP quality 1..100 (100 = lossless WebP; default 85 JPEG, 90 WebP); background:color=#ffffff Behind a JPEG's transparency"},
    {"document.close", "Close the document in the current tab.", "discard:bool=false Close even with unsaved changes"},
    {"canvas.resize", "Change the canvas size, keeping the layers' pixels.",
     "width:integer! Pixels; height:integer! Pixels; anchorX:number=0.5 0 keeps the left edge, 1 the right; anchorY:number=0.5 0 keeps the top, 1 the bottom"},
    {"canvas.crop", "Crop the canvas to a rectangle.", "x:number! Left; y:number! Top; width:number! Width; height:number! Height"},
    {"canvas.flip", "Flip the whole canvas.", "vertical:bool=false Flip top to bottom instead of left to right"},
    {"image.trim", "Cut the canvas down to its content, as Photoshop's Image > Trim (one undo step); trimmed is false when nothing would change or nothing would remain.",
     "basedOn:(transparent|topLeft|bottomRight)=transparent What is trimmed away: transparent pixels, or the colour of that corner; top:bool=true Trim the top; bottom:bool=true; left:bool=true; right:bool=true; "
     "tolerance:integer=0 For the colour modes: how far a channel may be from the corner's (0..255)"},
    {"image.resize", "Resample the whole image (every layer).",
     "width:integer New width (0 keeps the aspect from height); height:integer New height; scale:number Instead of a size: a factor; sampling:(nearest|smooth|high)=high Resampling; resolution:number Pixels per inch to record"},
    // seeing the result
    {"render", "The composite (what an export gives) as PNG, downscaled so its longest side is at most maxSize.",
     "region:object {x, y, width, height} in document pixels; maxSize:number=1024 Longest side in pixels (0 = full size); zoom:number=1 Enlarge a region 2..32 times with square pixels to judge edges (region times zoom within 4096); checkerboard:bool=false Show transparency as a checkerboard; path:string Write the PNG here instead of returning base64"},
    {"screenshot", "The canvas as the person sees it (overlays, selection outline), or the whole window.",
     "window:bool=false The whole window; maxSize:number=1600 Longest side; path:string Write the PNG here instead of returning base64"},
    // layers
    {"layers.list", "The layer tree, top first: id, name, depth, kind, visibility, opacity, blend, transform, mask, text.",
     "thumbnails:bool=false Add each pixel layer's 96 px thumbnail as base64 PNG"},
    {"layers.get", "One layer, as layers.list reports it.", "id:layer! The layer"},
    {"layers.select", "Make a layer (or its mask) active, or select several.",
     "id:layer The layer (the primary one with ids); ids:array Several layer ids; mask:bool=false Select the layer's mask for painting and filters"},
    {"layers.set", "Change a layer's properties.",
     "id:layer! The layer; name:string New name; visible:bool Shown; opacity:number 0..1; blend:<blend> Blend mode; sampling:<sampling> How it is resampled when transformed; clipping:bool Clip to the layer beneath"},
    {"layers.add", "Add a layer above the active one and make it active.",
     "kind:(pixels|group|adjustment|text)=pixels What to add; name:string Its name; below:bool=false Put a pixel layer under the active one instead; "
     "adjustmentKind:<adjustment> For kind adjustment; settings:object For kind adjustment: settings as adjustments.defaults shows them; "
     "text:string For kind text: the content; x:number For kind text: left, default a quarter across; y:number For kind text: top; font:string Font family; size:number Font size in pixels 1..2000; "
     "bold:bool Bold; italic:bool Italic; color:color Text colour (default the foreground); align:(left|center|right) Alignment"},
    {"text.set", "Change a text layer's content or style; the layer must still be text (not painted on).",
     "id:layer The text layer (default the active one); text:string Content; font:string Font family; size:number Pixels 1..2000; bold:bool Bold; italic:bool Italic; color:color Colour; "
     "align:(left|center|right) Alignment; lineSpacing:number Multiple of the line height 0.5..5; letterSpacing:number Pixels -20..100"},
    {"layers.delete", "Delete layers.", "id:layer One layer; ids:array Several layer ids; bakeClipping:bool=true Keep the look of layers clipped to a deleted one by baking them"},
    {"layers.duplicate", "Duplicate a layer above itself.", "id:layer The layer (default the active one)"},
    {"layers.move", "Move a layer in the tree: into a folder, directly above another layer, or to the bottom.",
     "id:layer! The layer; parent:layer The folder to move into (default the top level); above:layer Place directly above this layer; atBottom:bool=false Place at the bottom of the parent"},
    {"layers.reorder", "Move a layer up or down among its siblings.", "id:layer The layer (default the active one); offset:integer! Positive moves up, negative down"},
    {"layers.setTransform", "Place, size, rotate or flip a layer.",
     "id:layer The layer (default the active one); x:number Left; y:number Top; width:number Width; height:number Height; rotation:number Degrees clockwise; scale:number Scale the current size by this factor about its centre; flipX:bool Mirrored left to right; flipY:bool Mirrored top to bottom"},
    {"layers.flip", "Flip a layer's pixels.", "id:layer The layer (default the active one); vertical:bool=false Top to bottom instead of left to right"},
    {"layers.mask", "Add, remove or change a layer mask.",
     "id:layer The layer (default the active one); action:(add|addFromSelection|delete|toggle|invert|apply|link)! What to do; revealing:bool=true For add: white (reveal all) rather than black"},
    {"layers.merge", "Merge the selected layers, or the active layer into the one beneath.", "down:bool=false Merge the active layer down"},
    {"layers.group", "Put the selected layers in a new folder.", ""},
    {"smartObject.convert", "The selected layers (or ids) as one smart object: their PSD becomes its contents, placed where they were.", "ids:array Layer ids (default: the selection)"},
    {"smartObject.place", "Place an image or PSD file as an embedded smart object above the active layer, 1:1 in the middle (scaled to fit).", "path:string! File path"},
    {"smartObject.replace", "Swap a smart object's contents for a file's, in every layer placing them; each keeps its centre and scale.", "id:layer The smart object layer (default: active); path:string! File path"},
    {"smartObject.rasterize", "A smart object as plain pixels.", "id:layer The smart object layer (default: active)"},
    {"smartObject.addFilter", "Add a Smart Filter on top of a smart object's stack (its contents untouched, the filter kept as Photoshop keeps it).",
     "id:layer The smart object layer (default: active); kind:string! gaussian blur, high pass, median, dust and scratches, surface blur, unsharp mask, motion blur, plastic wrap, mosaic, emboss, box blur, radial blur or add noise; "
     "radius:number Pixels (blurs, high pass, median, dust and scratches, surface blur, unsharp mask); threshold:number Levels (dust and scratches, surface blur, unsharp mask); "
     "amount:number Percent (unsharp mask, emboss, add noise) or Radial Blur's amount; angle:number Degrees (motion blur, emboss); distance:number Motion Blur pixels; "
     "highlight:number Plastic Wrap; detail:number Plastic Wrap; smoothness:number Plastic Wrap; cellSize:number Mosaic pixels; height:number Emboss pixels; "
     "samples:number Radial Blur 8, 16 or 32; gaussian:bool Add Noise distribution; monochromatic:bool Add Noise; seed:number Add Noise; "
     "opacity:number=100 Percent; blend:<blend> How it blends over what is below it in the stack"},
    {"layers.warp", "Warp a layer with one of Photoshop's presets: Warp Text on text (style none removes it), a mesh baked into a smart object's placement (redrawn from its contents), bent pixels otherwise.",
     "id:layer The layer (default the active one); style:string arc, arc lower, arc upper, arch, bulge, shell lower, shell upper, flag, wave, fish, rise, fisheye, inflate, squeeze, twist, or none (text); "
     "bend:number=50 Percent -100..100; horizontal:number=0 Horizontal distortion, percent; vertical:number=0 Vertical distortion, percent; orientation:(horizontal|vertical)=horizontal The warp's axis"},
    {"smartObject.editContents", "Open a smart object's contents in a new tab; smartObject.commit in that tab puts them back into every layer placing them.", "id:layer The smart object layer (default: active)"},
    {"smartObject.commit", "In a contents tab, put the contents back into the smart object they came from (as Save does).", ""},
    {"layers.render", "One layer alone as PNG, not composited with the others: with a mask, as it shows (mask applied, over the layer's bounds in document pixels); otherwise its own pixels.",
     "id:layer! The layer; masked:bool=true Apply the layer's mask; false gives the raw pixels; maxSize:number=1024 Longest side; path:string Write the PNG here instead of returning base64"},
    {"adjustments.get", "An adjustment layer's settings.", "id:layer The adjustment layer (default the active one)"},
    {"adjustments.set", "Change an adjustment layer's settings (keys not given keep their values).",
     "id:layer The adjustment layer (default the active one); settings:object! Settings, shaped as adjustments.defaults shows"},
    {"adjustments.defaults", "The default settings of an adjustment kind: the shape adjustments.set and pixels.adjust take.", "kind:<adjustment>! The kind"},
    // pixels
    {"pixels.adjust", "Apply an adjustment destructively to the active layer's pixels, inside the selection.",
     "kind:<adjustment>! The adjustment; settings:object Settings over the defaults (see adjustments.defaults)"},
    {"pixels.filter", "Run a filter on the active layer's pixels, inside the selection; settings not given keep the dialog's last values.",
     "kind:<filter>! The filter; radius:number Gaussian Blur radius in pixels; angle:number Motion Blur angle in degrees; distance:number Motion Blur distance in pixels; "
     "amount:number Add Noise amount in percent; gaussian:bool Add Noise: Gaussian rather than uniform; monochromatic:bool Add Noise: grey noise; seed:integer=1 Add Noise seed; "
     "distortion:number Lens Correction distortion -100..100; bicubic:bool Lens Correction: sharper resample"},
    {"pixels.cameraRaw", "Filter > Camera Raw Filter on the active layer's pixels, inside the selection; replies with the normalized settings it applied.",
     "settings:object! The grade, keys as the model (defaults leave the image alone): whiteBalance (Custom|Auto), temperature, tint, exposure -5..5, "
     "contrast, highlights, shadows, whites, blacks, vibrance, saturation, texture, clarity, dehaze (-100..100), glow 0..100, glowStyle (Diffusion|Bloom|Halation), "
     "glowRange, glowSpread, glowWarmth, vignetteAmount, vignetteStyle (Highlight Priority|Color Priority|Paint Overlay), vignetteMidpoint, vignetteRoundness, "
     "vignetteFeather, vignetteHighlights, grainAmount, grainSize, grainRoughness, and objects curve {shadows, darks, lights, highlights, shadowSplit, darkSplit, "
     "lightSplit, rgb, red, green, blue ([[x, y], ...] on 0..1), refineSaturation}, mixer {hue, saturation, luminance ({reds, oranges, yellows, greens, aquas, "
     "blues, purples, magentas} or 8 numbers), points}, grading {shadows, midtones, highlights, global ({hue, saturation, luminance}), blending, balance}, "
     "detail {sharpenAmount 0..150, sharpenRadius, sharpenDetail, sharpenMasking, noiseLuminance, noiseLuminanceDetail, noiseLuminanceContrast, noiseColor, "
     "noiseColorDetail, noiseColorSmoothness}, optics {removeChromaticAberration, enableLensProfile, profileDistortion, profileVignetting, distortion, "
     "purpleAmount, purpleHueLow, purpleHueHigh, greenAmount, greenHueLow, greenHueHigh, vignetteAmount, vignetteMidpoint}, geometry {upright (Off|Guided), "
     "projection (Perspective|Rectilinear), vertical, horizontal, rotate -45..45, aspect, scale, offsetX, offsetY, constrainCrop, guides [{startX, startY, endX, "
     "endY} on 0..1 from the lower left]}, calibration {process 1..6, shadowTint, redHue, redSaturation, greenHue, greenSaturation, blueHue, blueSaturation}; "
     "seed:integer=1 Grain seed"},
    {"pixels.invert", "Invert the active layer's colours inside the selection.", ""},
    {"pixels.fill", "Fill the selection (or the whole layer) with a colour.", "color:color=#000000 The colour"},
    {"pixels.clear", "Clear the selection (or the whole layer) to transparent.", ""},
    {"pixels.contentAwareFill", "Fill the selection from its surroundings (needs a selection).", ""},
    {"pixels.gmic", "Run a G'MIC command line on the active layer, inside the selection. Only catalogue filters and common built-ins followed by numbers are allowed.",
     "command:string! E.g. \"fx_bokeh 3,8,0,30\", see gmic.filters; timeoutMs:integer=300000 Give up after this long"},
    {"gmic.filters", "The G'MIC filter catalogue with parameters and defaults; filters that do not work here are left out.",
     "search:string Only filters whose name or command contains this; all:bool=false Include the unsupported filters, with the reason"},
    {"pixels.removeBackground", "Mask the active layer's background away with the background-removal model (enable and download it in Preferences first).",
     "refine:bool=true Refine the edge; refineEdges:number Edge refinement radius; contrast:number Edge contrast; shiftEdge:number Move the edge in (negative) or out; "
     "matting:number Band width in pixels in which hair opacity is solved; cleanup:bool Remove half-transparent specks touching no edge (default true); "
     "decontaminate:bool Edge pixels take the subject's own colour (default true); detail:bool=false Run the model again on full-resolution windows along the edge of a large photo; "
     "detailWindows:number=12 At most this many windows; flip:bool Average the mask with the mirrored image's (default the preference)"},
    // selection
    {"selection.info", "The selection: whether there is one, its bounds and area.", ""},
    {"selection.render", "The selection as a greyscale PNG mask.", "maxSize:number=1024 Longest side; path:string Write the PNG here instead of returning base64"},
    {"selection.all", "Select the whole canvas.", ""},
    {"selection.none", "Deselect.", ""},
    {"selection.invert", "Invert the selection.", ""},
    {"selection.rect", "Select a rectangle or ellipse.",
     "x:number! Left; y:number! Top; width:number! Width; height:number! Height; ellipse:bool=false An ellipse in that box; mode:<selectionMode>=replace How it combines with the current selection"},
    {"selection.polygon", "Select a polygon.", "points:array! At least three [x, y] points; mode:<selectionMode>=replace How it combines"},
    {"selection.wand", "Magic wand: select similar colours around a point.",
     "x:number! Point; y:number! Point; tolerance:integer Colour tolerance 0..255 (default the tool's); contiguous:bool Only connected pixels (default the tool's); "
     "sampleAll:bool Sample the composite rather than the active layer; sampleRadius:integer Average a square of this radius; "
     "edgeAware:bool Contiguous only: follow the image (shading and texture stay in, edges between similar colours hold); default the tool's setting, on; false is the classic per-channel tolerance; "
     "refineEdge:bool Unmix the edge so a line's fringe is partly selected, and pixels.clear right after leaves the line its own colour (the tool's setting, on); "
     "mode:<selectionMode>=replace How it combines"},
    {"selection.scribble", "Quick Select: strokes over the subject and over the background; the selection follows the image's edges.",
     "foreground:array Strokes over what to select, each a list of [x, y] points; background:array Strokes over what to leave out; size:integer Stroke width in pixels; "
     "refine:integer Edge refinement 0..40; clear:bool=false Forget earlier strokes first; mode:<selectionMode>=replace How it combines"},
    {"selection.subject", "Click select with the EfficientSAM model (app.info reports clickSelect when it can run).",
     "foreground:array [x, y] points on the subject; background:array [x, y] points to leave out; box:array [x0, y0, x1, y1] around the subject; refine:integer Edge refinement 0..40; "
     "clear:bool=true Forget earlier clicks first; mode:<selectionMode>=replace How it combines"},
    {"selection.fromLayer", "Select a layer's opaque pixels (or its mask).",
     "id:layer The layer (default the active one); mask:bool=false From the layer's mask; mode:<selectionMode>=replace How it combines"},
    {"selection.grow", "Grow (or shrink, negative) the selection.", "amount:integer! Pixels"},
    {"selection.feather", "Soften the selection's edge.", "radius:number! Pixels"},
    {"selection.smooth", "Smooth the selection's outline.", "radius:integer! Pixels"},
    {"selection.border", "Select a band along the selection's edge.", "width:integer! Pixels"},
    // painting
    {"brush.presets", "The MyPaint brush presets and imported brushes: id, name, group, size, whether an eraser.", "group:string Only this group"},
    {"brush.import", "Import brushes: Photoshop .abr, Procreate .brushset/.brush, Clip Studio .sut, or images as tips.",
     "path:string One file; paths:array Several files"},
    {"brush.stroke", "Paint a stroke through points on the active layer (or its mask); the person's tool and settings are put back afterwards.",
     "points:array! [x, y] points in document pixels; tool:(brush|eraser|healing|clone|smudge|blur|liquify)=brush The tool; size:number Diameter in pixels; hardness:number 0..1; opacity:number 0..1; "
     "color:color Paint colour (default the foreground); mask:bool=false Paint the active layer's mask; erase:bool=false Erase with the brush; source:object {x, y} Clone source; "
     "preset:string A preset id from brush.presets, or round; pressure:number=0.5 Pen pressure 0..1; pressures:array One pressure per point"},
    {"gradient.draw", "Draw a gradient on the active layer, inside the selection.",
     "x0:number! Start; y0:number! Start; x1:number! End; y1:number! End; shape:(linear|radial)=linear Shape; "
     "style:(foreground-to-transparent|foreground-to-background)=foreground-to-transparent Colours; reversed:bool=false Swap the ends; opacity:number=1 0..1; foreground:color Start colour; background:color End colour"},
    {"shape.draw", "Add a shape layer.",
     "x:number! Left; y:number! Top; width:number! Width; height:number! Height; kind:(rectangle|ellipse)=rectangle Shape; cornerRadius:number=0 Rounded corners in pixels; color:color Fill (default the foreground)"},
};

struct Param {
    QString name, type, description;
    bool required = false;
    std::optional<QString> fallback;
    QStringList values;
};

QStringList valuesNamed(const QString& list) {
    QStringList out;
    if (list == "blend") out = blendModeNames() << QStringLiteral("Pass Through");   // Pass Through: folders only
    else if (list == "sampling") for (int i = 0; i < 3; i++) out << QString::fromUtf8(samplingName(Sampling(i)));
    else if (list == "adjustment") for (int i = 0; i < 6; i++) out << QString::fromUtf8(adjustmentKindName(AdjustmentKind(i)));
    else if (list == "filter") for (int i = 0; i < 4; i++) out << QString::fromUtf8(filterKindName(FilterKind(i)));
    else if (list == "tool") out = toolNames();
    else if (list == "selectionMode") out = {"replace", "add", "subtract", "intersect"};
    return out;
}

std::vector<Param> parse(const char* spec) {
    std::vector<Param> out;
    static const QRegularExpression entry(R"(^(\w+):(\w+|\([^)]*\)|<\w+>)(!)?(?:=(\S+))?\s*(.*)$)");
    for (const QString& part : QString::fromUtf8(spec).split(';', Qt::SkipEmptyParts)) {
        auto m = entry.match(part.trimmed());
        if (!m.hasMatch()) continue;
        Param p;
        p.name = m.captured(1);
        QString type = m.captured(2);
        if (type.startsWith('(')) { p.values = type.mid(1, type.size() - 2).split('|'); p.type = "enum"; }
        else if (type.startsWith('<')) { p.values = valuesNamed(type.mid(1, type.size() - 2)); p.type = "enum"; }
        else p.type = type;
        p.required = !m.captured(3).isEmpty();
        if (!m.captured(4).isEmpty()) p.fallback = m.captured(4);
        p.description = m.captured(5);
        out.push_back(std::move(p));
    }
    return out;
}

const std::map<QString, std::pair<const MethodDoc*, std::vector<Param>>>& table() {
    static const auto parsed = [] {
        std::map<QString, std::pair<const MethodDoc*, std::vector<Param>>> t;
        for (const MethodDoc& d : methodDocs) t[QString::fromUtf8(d.name)] = {&d, parse(d.params)};
        return t;
    }();
    return parsed;
}

} // namespace

const QStringList& toolNames() {
    static const QStringList names{"move", "marquee", "lasso", "wand", "quickselect", "crop", "brush", "healing", "clone", "smudge",
                                   "gradient", "shape", "text", "eyedropper", "hand", "zoom"};
    return names;
}

bool isDescribed(const QString& method) { return table().count(method) > 0; }

QJsonObject describeMethod(const QString& method) {
    auto it = table().find(method);
    if (it == table().end()) fail("no method '" + method + "'; rpc.describe without a method lists them all", invalidParams);
    QJsonArray params;
    for (const Param& p : it->second.second) {
        QJsonObject o{{"name", p.name}, {"type", p.type}, {"required", p.required}, {"description", p.description}};
        if (p.fallback) o["default"] = *p.fallback;
        if (!p.values.isEmpty()) o["values"] = QJsonArray::fromStringList(p.values);
        params.append(o);
    }
    return {{"method", method}, {"summary", QString::fromUtf8(it->second.first->summary)}, {"params", params}};
}

QJsonObject describeAll() {
    QJsonObject out;
    for (const auto& [name, entry] : table()) out[name] = QString::fromUtf8(entry.first->summary);
    return out;
}

QString unknownParameter(const QString& method, const QJsonObject& params) {
    auto it = table().find(method);
    if (it == table().end()) return {};
    for (auto key = params.begin(); key != params.end(); ++key) {
        bool known = false;
        for (const Param& p : it->second.second) known = known || p.name == key.key();
        if (known) continue;
        QStringList names;
        for (const Param& p : it->second.second) names << p.name;
        return names.isEmpty() ? QStringLiteral("%1 takes no parameters, not '%2'").arg(method, key.key())
                               : QStringLiteral("%1 has no parameter '%2'; it takes %3").arg(method, key.key(), names.join(", "));
    }
    return {};
}

QString missingParameter(const QString& method, const QJsonObject& params) {
    auto it = table().find(method);
    if (it == table().end()) return {};
    for (const Param& p : it->second.second)
        if (p.required && (!params.contains(p.name) || params.value(p.name).isNull()))
            return QStringLiteral("%1 needs '%2' (%3)").arg(method, p.name, p.description);
    return {};
}

} // namespace app::rpc
