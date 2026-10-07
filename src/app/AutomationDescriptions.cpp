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
    {"app.info", "The editor's version and protocolVersion, the socket, the number of tabs, and what can run: removeBackground and clickSelect (their models are downloaded), scribble, raw (camera RAW files) and pdf (PDF files open).", ""},
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
    {"color.sample", "The Eyedropper: sample the composite at a pixel and make it the foreground (or background) colour. Answers the colour and the document's own values: linear R, G, B at 32 bits, C, M, Y, K ink percentages in CMYK, L, a, b in Lab.",
     "x:number! Document pixel column; y:number! Document pixel row; background:bool=false Set the background colour instead"},
    {"color.settings", "Edit > Color Settings, the monitor profile and View > Proof Setup: read them, or change the keys given (docs/color-management.md). Untagged images are always treated as sRGB; new documents take the working space.",
     "workingSpace:(srgb|adobe-rgb|display-p3|prophoto) The RGB working space; workingCmyk:string The Working CMYK: default (ISO Coated v2 300%, FOGRA39, bundled) or a CMYK ICC file; "
     "intent:(perceptual|relative|saturation|absolute) Conversion Options: the intent Image > Mode converts between RGB, CMYK and Lab with; blackPointCompensation:bool Conversion Options: black point compensation; "
     "policy:(preserve|convert|off) What happens to a file's embedded profile when it opens; "
     "askMissing:bool Ask when a file has no profile; askMismatch:bool Ask when its profile is not the working space; "
     "monitorProfile:string An ICC file for the monitor (\"\" to use the system's); useSystemMonitor:bool Read the system's monitor profile (X11, Windows); "
     "proofProfile:string What to simulate: working-cmyk (the default, Photoshop's), a working space, or an RGB or CMYK ICC file; proofIntent:(perceptual|relative) How colours go to it; proofBlackPoint:bool Black point compensation; "
     "proofColors:bool View > Proof Colors; gamutWarning:bool View > Gamut Warning; gamutColor:color The warning's colour"},
    {"view.zoom", "Zoom the view (not the document).", "zoom:number Zoom factor, 1 = 100%; fit:bool=false Fit the document in the window"},
    {"view.exposure", "A 32-bit document's view (View > 32-bit Preview Options, the status bar's exposure): how the canvas shows its linear values, not the pixels; not an undo step. Returns the view; with no parameters, only reads it.",
     "exposure:number Stops, -20..20; gamma:number 0.1..9.99; method:(exposure-gamma|highlight-compression) Exposure and Gamma, or Highlight Compression"},
    {"debug.eye", "Test hook: a pointer event on a layer's eye button in the Layers panel.",
     "id:layer! The layer whose eye is pressed; to:layer The eye the pointer is over; action:(press|move|release)! The event"},
    {"debug.dragSmartFilter", "Test hook: drag a Smart Filter row in the Layers panel and release it above or below another entry row (refused unless in the same stack).",
     "id:layer! The smart object; index:integer! The dragged entry (running order); onto:integer! The entry row it is released on; ontoId:layer That row's smart object (default id); position:(above|below)=above Which half of the row"},
    // actions
    {"actions.list", "The recorded actions (Window > Actions): each one's steps (method, params, enabled, label), whether one is recording, and the library file.",
     "name:string Only this action"},
    {"actions.record", "Start or stop recording an action: while it records, every editing request (and the menu commands, dialogs and brush strokes the person makes) is added as a step.",
     "action:(start|stop)! What to do; name:string For start: the action to record into (created, or appended to when it exists)"},
    {"actions.play", "Play an action on the current document: its enabled steps in order, stopping at the first error (the reply says which). Steps that stay in one document become one undo step named after the action.",
     "name:string! The action; times:integer=1 Play it this many times"},
    {"actions.batch", "File > Automate > Batch: open every image in a folder in its own tab, play the action, export the result to another folder and close the tab. The reply lists what was written, what failed (and at which stage) and what was skipped.",
     "name:string! The action; input:string! Folder of images, PSDs or projects (.nekophoto files, .comp folders); output:string! Folder to write to (made if missing); format:(png|jpg|webp|tif|psd|gif|tga)=png What to export; overwrite:bool=false Replace files already there"},
    {"actions.save", "Create an action, or replace the one with this name, from steps: how an agent edits, reorders or toggles steps (actions.list gives them to start from).",
     "name:string! The action; steps:array! {\"method\", \"params\", \"enabled\"} objects, as actions.list shows them"},
    {"actions.delete", "Delete an action from the library.", "name:string! The action"},
    {"actions.import", "Import actions from a JSON file (actions.export's format, or one action object); names already used get a number.", "path:string! The file"},
    {"actions.export", "Write actions to a JSON file to share or back up.", "path:string! Where to write; name:string One action; names:array Several (default every action); overwrite:bool=false Replace an existing file"},
    // frame animation
    {"timeline.info", "The frame animation (Window > Timeline): each frame's delay and the layers it shows, the current frame (-1 without frames) and the loop count (0 forever).", ""},
    {"timeline.frame", "Change the frames (one undo step each, except select, which is none, as in Photoshop): create the first from the layers as they are, fromLayers (a frame per top-level layer), duplicate the current frame (Photoshop's New Frame), select one (the layers then show it; edits to visibility, position and opacity go into it), delete, move, or clear the animation.",
     "action:(create|fromLayers|duplicate|select|delete|move|clear)! What to do; index:integer The frame (default the current one); to:integer For move: where it goes"},
    {"timeline.set", "Set a frame's delay and the loop count.",
     "index:integer The frame (default the current one; -1 every frame); delay:integer Milliseconds 0..655350; loopCount:integer 0 forever, 1 once, n times"},
    // documents and canvas
    {"document.info", "The current document: size, resolution, layer count, active layer, path, whether modified.", ""},
    {"document.histogram", "The Histogram panel's numbers: 256 bins of one channel (alpha-weighted, 16-bit rounded to 8, 32-bit encoded at exposure 0) with Mean, Std Dev, Median, Pixels and the cache level counted.",
     "channel:string The channel by name in the document's mode: rgb (or composite), red, green, blue, luminosity; cmyk, cyan, magenta, yellow, black; lightness, a, b (default the composite, lightness in Lab); "
     "source:(entire|layer|adjustment)=entire Entire Image, the Selected Layer's pixels, or the Adjustment Composite (the active adjustment layer and everything below); "
     "cached:bool=false Count a reduced render as the panel does (its cache level) instead of every pixel"},
    {"document.overview", "The document at a glance, as text: size, selection, undo, and the layer tree top first with each layer's kind, bounds, opacity, blend, mask and id.",
     "maxLayers:integer=80 List at most this many layers"},
    {"document.new", "A new document in the current tab (or a new tab if this one has a document).",
     "width:integer=1920 Pixels; height:integer=1080 Pixels; resolution:number=72 Pixels per inch; emptyLayer:bool=true Start with a blank pixel layer"},
    {"document.open", "Open a project (a .nekophoto file or a .comp folder), Photoshop (.psd/.psb), Clip Studio (.clip), Affinity (.afphoto/.afdesign/.afpub/.af), Aseprite (.ase/.aseprite, its frames as a timeline), SVG (.svg/.svgz: shapes as editable vector shape layers, the rest as pixels), PDF (.pdf: one page as a pixel layer, when app.info reports pdf), icon (.ico/.cur, a layer per size), animated GIF (a layer per frame), image or camera RAW file (.tga included; RAW when app.info reports raw). Layered files open in a new tab and the reply lists their layers and notes; with a document already open, an image is imported as a layer. A camera RAW file always opens in a new tab, developed without the Camera Raw dialog: as shot, or with settings; asSmartObject makes Camera Raw's Open Object (a smart object keeping the RAW file and the settings). A PSD or PSB whose layers are past the budget can open as its merged image alone with mergedOnly (a new, untitled document).",
     "path:string! File path; page:integer=1 PDF only: the page to open (1 is the first); resolution:number=150 PDF only: pixels per inch, 18..1200; "
     "settings:object Camera RAW only: the develop, keys as pixels.cameraRaw's settings. White balance for RAW: temperature in kelvin 2000..50000 with tint -150..150 (or rawTemperature and rawTint), whiteBalance As Shot|Auto|Custom or a preset the file records (Daylight|Cloudy|Shade|Tungsten|Fluorescent|Flash); Auto without numbers solves the white point; nothing about white balance opens As Shot; a temperature within -100..100 is the older form, relative to as shot. The reply's settings carry rawTemperature and rawTint; "
     "asSmartObject:bool=false Camera RAW only: open as a smart object whose source is the RAW file and the settings; "
     "bitsPerChannel:integer=16 Camera RAW only: 8 or 16; "
     "mergedOnly:bool=false PSD/PSB only: open the merged image Photoshop stored, as one layer, without the layers"},
    {"document.import", "Import an image file as a new layer.", "path:string! File path; x:number Left edge in document pixels; y:number Top edge"},
    {"document.save", "Save as a project: a single .nekophoto file, or a .comp project folder when the path ends in .comp (the extension decides; a path with neither gets .nekophoto).", "path:string Where to save (default: where it was opened or last saved, in the same form); overwrite:bool=false Replace an existing file or folder at path when it is not where the document already lives"},
    {"document.export", "Export as a layered Photoshop .psd (the reply lists what Photoshop cannot carry), as .svg (vector shape layers as paths, folders as groups, other layers as embedded PNGs; the reply counts them and lists what became images), the timeline's frames as an animated .gif (the composite when there are none), or the composite as .png, .jpg, .webp, .tif, .tga or .ico (16, 32, 48 and 256 px; the extension decides).",
     "path:string! Output file; quality:integer JPEG and WebP quality 1..100 (100 = lossless WebP; default 85 JPEG, 90 WebP); background:color=#ffffff Behind a JPEG's transparency; "
     "embedProfile:bool=true Embed the document's colour profile (PNG, JPEG, WebP, TIFF; PSD always carries it); convertToSrgb:bool Convert to sRGB first, for the web (default true for GIF, false otherwise); overwrite:bool=false Replace an existing file at path"},
    {"document.revert", "File > Revert: read the file the document was opened from or last saved to again (any format it opens: project, PSD/PSB, Clip Studio, Affinity, SVG, PDF, an image, camera RAW with the Camera Raw settings it opened with), the way it opened, as one undo step named Revert; history.undo brings the edits back. The reply says reverted false when nothing changed since it was opened or saved, and undoable false when the history could not keep the document as it was.", ""},
    {"document.close", "Close the document in the current tab.", "discard:bool=false Close even with unsaved changes"},
    {"canvas.resize", "Change the canvas size, keeping the layers' pixels.",
     "width:integer! Pixels; height:integer! Pixels; anchorX:number=0.5 0 keeps the left edge, 1 the right; anchorY:number=0.5 0 keeps the top, 1 the bottom"},
    {"canvas.crop", "Crop the canvas to a rectangle.", "x:number! Left; y:number! Top; width:number! Width; height:number! Height; "
     "ratio:string The Crop tool's ratio, W:H such as 16:9 (or a number, width / height): the largest box of that shape centred in the rectangle"},
    {"canvas.flip", "Flip the whole canvas.", "vertical:bool=false Flip top to bottom instead of left to right"},
    {"image.trim", "Cut the canvas down to its content, as Photoshop's Image > Trim (one undo step); trimmed is false when nothing would change or nothing would remain.",
     "basedOn:(transparent|topLeft|bottomRight)=transparent What is trimmed away: transparent pixels, or the colour of that corner; top:bool=true Trim the top; bottom:bool=true; left:bool=true; right:bool=true; "
     "tolerance:integer=0 For the colour modes: how far a channel may be from the corner's (0..255)"},
    {"document.profile", "The document's colour profile: get it, assign one (Edit > Assign Profile: the tag only, the pixel values stay) or convert to one (Edit > Convert to Profile: every layer's pixels and the colours of text, shapes, styles and adjustments, and the foreground and background colours, so the document looks the same). One undo step.",
     "action:(get|assign|convert)=get What to do; profile:string srgb, adobe-rgb, display-p3, prophoto, working (Color Settings' working space: the Working CMYK in a CMYK document), working-cmyk (CMYK documents), none (assign only: untagged, treated as sRGB, or as the Working CMYK in a CMYK document) or an ICC file's path (of the document's mode); "
     "intent:(perceptual|relative)=relative Convert only: the rendering intent; blackPointCompensation:bool=true Convert only"},
    {"image.mode", "Image > Mode: convert the document to 8, 16 or 32 bits per channel, or to RGB Color, CMYK Color or Lab Color, each as one undo step (with both, the colour mode first, except when leaving 32 bits). "
     "A 16-bit document holds half the pixels of an 8-bit one within the same memory, a 32-bit one a quarter; what has not been ported to the depth yet is refused on it (docs/bit-depth.md). "
     "8 or 16 bits to 32 linearise through the profile's curve; from 32 bits HDR Toning applies (method, exposure, gamma; the defaults give an 8- or 16-bit-sourced document back exactly). 32 bits is RGB only, as in Photoshop. "
     "A colour mode conversion uses Color Settings' Working CMYK, working space, intent and black point compensation; adjustment layers the new mode does not offer are kept hidden and come back on converting back (docs/color-modes.md).",
     "bits:integer 8, 16 or 32; colorMode:(rgb|cmyk|lab) The colour mode; method:(exposure-gamma|highlight-compression)=exposure-gamma From 32 bits: HDR Toning's method; exposure:number=0 From 32 bits, Exposure and Gamma: stops, -20..20; gamma:number=1 From 32 bits, Exposure and Gamma: 0.1..9.99"},
    {"image.resize", "Resample the whole image (every layer).",
     "width:integer New width (0 keeps the aspect from height); height:integer New height; scale:number Instead of a size: a factor; sampling:(nearest|smooth|high)=high Resampling; resolution:number Pixels per inch to record"},
    // seeing the result
    {"render", "The composite (what an export gives) as PNG, downscaled so its longest side is at most maxSize.",
     "region:object {x, y, width, height} in document pixels; maxSize:number=1024 Longest side in pixels (0 = full size); zoom:number=1 Enlarge a region 2..32 times with square pixels to judge edges (region times zoom within 4096); checkerboard:bool=false Show transparency as a checkerboard; path:string Write the PNG here instead of returning base64; overwrite:bool=false Replace an existing file at path"},
    {"screenshot", "The canvas as the person sees it (overlays, selection outline), or the whole window.",
     "window:bool=false The whole window; maxSize:number=1600 Longest side; path:string Write the PNG here instead of returning base64; overwrite:bool=false Replace an existing file at path"},
    // layers
    {"layers.list", "The layer tree, top first: id, name, depth, kind, visibility, opacity, blend, transform, mask, text.",
     "thumbnails:bool=false Add each pixel layer's 96 px thumbnail as base64 PNG"},
    {"layers.get", "One layer, as layers.list reports it, with its Blend If ranges when it has any.", "id:layer! The layer"},
    {"layers.style", "A layer's effects (Photoshop's layer style): each kind as a list, the ones switched off too.", "id:layer! The layer"},
    {"layers.setStyle", "Replace a layer's effects, shaped as layers.style shows (settings left out take Photoshop's defaults; an empty object clears the style).",
     "id:layer! The layer; style:object! dropShadows, innerShadows, outerGlows, innerGlows, bevels, satins, colorOverlays, gradientOverlays, patternOverlays, strokes (lists), visible, maskHidesEffects, blendInteriorAsGroup"},
    {"layers.applyStyle", "Give a layer an imported style preset (presets.list): its effects replace the layer's, and the document gets the patterns the style uses. Blending options in the preset are not applied.",
     "id:layer! The layer; style:string! The style preset's name"},
    {"layers.cage", "A layer's warp cage (Edit ▸ Warp Cage): 16 [x, y] control points of a 4 x 4 Bezier mesh, row by row, in document pixels.", "id:layer! The layer"},
    {"layers.setCage", "Warp a layer through a cage (Photoshop's Custom warp): pixels are bent for good, a smart object keeps it as its own editable warp.",
     "id:layer! The layer; points:array! 16 [x, y] points, row by row, as layers.cage gives them"},
    {"layers.select", "Make a layer (or its mask) active, or select several.",
     "id:layer The layer (the primary one with ids); ids:array Several layer ids; mask:bool=false Select the layer's mask for painting and filters"},
    {"layers.set", "Change a layer's properties.",
     "id:layer The layer (the active one when left out); name:string New name; visible:bool Shown; opacity:number 0..1; blend:<blend> Blend mode; sampling:<sampling> How it is resampled when transformed; clipping:bool Clip to the layer beneath; "
     "blendIf:object Blending Options' Blend If: channels (gray, red, green, blue; gray, cyan, magenta, yellow, black; lightness, a, b) each with thisLayer and/or underlying as [black low, black high, white low, white high] 0..255 (split handles when low differs from high); channels left out stay; reset true clears the others first"},
    {"layers.add", "Add a layer above the active one and make it active.",
     "kind:(pixels|group|adjustment|text)=pixels What to add; name:string Its name; below:bool=false Put a pixel layer under the active one instead; "
     "adjustmentKind:<adjustment> For kind adjustment; settings:object For kind adjustment: settings as adjustments.defaults shows them; "
     "text:string For kind text: the content; x:number For kind text: left, default a quarter across; y:number For kind text: top; font:string Font family; size:number Font size in pixels 1..2000; "
     "bold:bool Bold; italic:bool Italic; color:color Text colour (default the foreground); align:(left|center|right) Alignment"},
    {"text.set", "Change a text layer's content or style; the layer must still be text (not painted on).",
     "id:layer The text layer (default the active one); text:string Content; font:string Font family; size:number Pixels 1..2000; bold:bool Bold; italic:bool Italic; color:color Colour; "
     "align:(left|center|right) Alignment; lineSpacing:number Multiple of the line height 0.5..5; letterSpacing:number Pixels -20..100"},
    {"text.styleRange", "Style some letters of a text layer (Photoshop's Character panel on a selection): the fields given change, the rest stay.",
     "id:layer The text layer (default the active one); start:integer=0 First letter, in UTF-16 units of the text; length:integer How many UTF-16 units (default to the end); "
     "font:string Font family; size:number Pixels 1..2000; bold:bool Bold (clears weight); weight:integer 0 or 100..900 (bold follows from 600); italic:bool Italic; color:color Colour; "
     "letterSpacing:number Tracking, extra pixels per letter; baselineShift:number Pixels up; leading:number Baseline to baseline in pixels, 0 auto; "
     "caps:(normal|small|all) Capitals; underline:bool Underline; strikethrough:bool Strikethrough"},
    {"layers.delete", "Delete layers.", "id:layer One layer (default the active one); ids:array Several layer ids; bakeClipping:bool=true Keep the look of layers clipped to a deleted one by baking them"},
    {"layers.duplicate", "Duplicate a layer above itself.", "id:layer The layer (default the active one)"},
    {"layers.viaCopy", "Layer via Copy: the selected pixels of the active layer as a new layer above it (the whole layer when nothing is selected).", ""},
    {"layers.copy", "Copy whole layers, as Edit > Copy with layers selected and no selection: the layers and folders with their masks, styles, text, shapes, smart objects and adjustments go to the layer clipboard every tab shares (other apps get them flattened).",
     "ids:array The layers to copy (default the selected ones)"},
    {"layers.paste", "Paste copied layers into the current document above the active layer, converted to its profile and depth; one undo step. Answers the new layers' ids.", ""},
    {"layers.move", "Move a layer in the tree: into a folder, directly above another layer, or to the bottom.",
     "id:layer! The layer; parent:layer The folder to move into (default the top level); above:layer Place directly above this layer; atBottom:bool=false Place at the bottom of the parent"},
    {"layers.reorder", "Move a layer up or down among its siblings.", "id:layer The layer (default the active one); offset:integer! Positive moves up, negative down"},
    {"layers.setTransform", "Place, size, rotate or flip a layer. For a folder the values are its contents' box, and everything inside moves with it.",
     "id:layer The layer (default the active one); x:number Left; y:number Top; width:number Width; height:number Height; rotation:number Degrees clockwise; scale:number Scale the current size by this factor about its centre; flipX:bool Mirrored left to right; flipY:bool Mirrored top to bottom"},
    {"layers.flip", "Flip a layer's pixels.", "id:layer The layer (default the active one); vertical:bool=false Top to bottom instead of left to right"},
    {"layers.mask", "Add, remove or change a layer mask.",
     "id:layer The layer (default the active one); action:(add|addFromSelection|delete|toggle|invert|apply|link)! What to do; revealing:bool=true For add: white (reveal all) rather than black"},
    {"layers.merge", "Merge the selected layers, or the active layer into the one beneath; visible: true merges every visible layer (Layer > Merge Visible).", "down:bool=false Merge the active layer down; visible:bool=false Merge all visible layers into one; hidden layers stay"},
    {"layers.group", "Put the selected layers in a new folder.", ""},
    {"artboards.list", "The document's artboards: folders with a rectangle and a background, their children clipped to it.", ""},
    {"artboards.add", "A new, empty artboard at the top of the layer stack (Photoshop's Artboard tool).",
     "x:integer=0 Left edge; y:integer=0 Top edge; width:integer! Width in pixels; height:integer! Height in pixels; name:string Its name (default Artboard N); background:color=white white, black, transparent or a CSS colour"},
    {"artboards.set", "Move, resize, rename or recolour an artboard; a move takes its layers along unless moveContents is false.",
     "id:layer! The artboard; x:integer Left edge; y:integer Top edge; width:integer Width; height:integer Height; name:string New name; background:color white, black, transparent or a CSS colour; moveContents:bool=true Move the layers inside with it"},
    {"artboards.delete", "Turn an artboard back into a plain folder, or delete it with everything in it.", "id:layer! The artboard; contents:bool=false Delete its layers too"},
    {"artboards.export", "File > Export Artboards to Files: each visible artboard as its own image, named after it.",
     "directory:string! Folder to write into (created when missing); format:string=png png, jpeg, webp or tiff; prefix:string File name prefix (cleaned as the names are: no folders); quality:integer=90 JPEG and WebP quality 1..100; overwrite:bool=false Replace files that exist (else nothing is written)"},
    {"slices.list", "The document's slices (named rectangles for export, kept in PSDs as Photoshop's slices).", ""},
    {"slices.add", "A new user slice.",
     "x:integer=0 Left edge; y:integer=0 Top edge; width:integer! Width; height:integer! Height; name:string Its name (default slice_N); url:string Link; target:string Link target; altTag:string Alt text"},
    {"slices.set", "Change a slice's rectangle or fields.",
     "id:integer! The slice id; x:integer Left edge; y:integer Top edge; width:integer Width; height:integer Height; name:string Name; url:string Link; target:string Link target; altTag:string Alt text"},
    {"slices.delete", "Delete a slice.", "id:integer! The slice id"},
    {"slices.export", "File > Export Slices: each slice as its own image, named after it.",
     "directory:string! Folder to write into (created when missing); format:string=png png, jpeg, webp or tiff; prefix:string File name prefix (cleaned as the names are: no folders); quality:integer=90 JPEG and WebP quality 1..100; overwrite:bool=false Replace files that exist (else nothing is written)"},
    {"guides.list", "The ruler guides (View > Show > Guides), in the order they were made: index, orientation and position in document pixels. They are saved in projects and PSDs.", ""},
    {"guides.add", "A new ruler guide (View > New Guide…); one undo step. Positions keep to 1/32 pixel, as in a PSD.",
     "orientation:(vertical|horizontal)! A vertical guide is an x position, a horizontal one a y; position:number! Document pixels from the left or top edge (may lie outside the canvas)"},
    {"guides.move", "Move a ruler guide; one undo step.", "index:integer! The guide's index from guides.list; position:number! Its new position in document pixels"},
    {"guides.delete", "Delete a ruler guide, or all of them (View > Clear Guides); one undo step.",
     "index:integer The guide's index from guides.list; all:bool=false Every guide"},
    {"smartObject.convert", "The selected layers (or ids) as one smart object: their PSD becomes its contents, placed where they were.", "ids:array Layer ids (default: the selection)"},
    {"smartObject.place", "Place an image or PSD file as an embedded smart object above the active layer, 1:1 in the middle (scaled to fit).", "path:string! File path"},
    {"smartObject.replace", "Swap a smart object's contents for a file's, in every layer placing them; each keeps its centre and scale.", "id:layer The smart object layer (default: active); path:string! File path"},
    {"smartObject.rasterize", "A smart object as plain pixels.", "id:layer The smart object layer (default: active)"},
    {"smartObject.viaCopy", "Layer > Smart Objects > New Smart Object via Copy: a copy of the smart object above it whose contents are its own (editing one leaves the other; Duplicate Layer shares them). Its warp, Smart Filters and filter mask come along.",
     "id:layer The smart object layer (default: active)"},
    {"smartObject.addFilter", "Add a Smart Filter on top of a smart object's stack (its contents untouched, the filter kept as Photoshop keeps it).",
     "id:layer The smart object layer (default: active); kind:string! gaussian blur, high pass, median, dust and scratches, surface blur, unsharp mask, motion blur, plastic wrap, mosaic, emboss, box blur, radial blur or add noise; "
     "radius:number Pixels (blurs, high pass, median, dust and scratches, surface blur, unsharp mask); threshold:number Levels (dust and scratches, surface blur, unsharp mask); "
     "amount:number Percent (unsharp mask, emboss, add noise) or Radial Blur's amount; angle:number Degrees (motion blur, emboss); distance:number Motion Blur pixels; "
     "highlight:number Plastic Wrap; detail:number Plastic Wrap; smoothness:number Plastic Wrap; cellSize:number Mosaic pixels; height:number Emboss pixels; "
     "samples:number Radial Blur 8, 16 or 32; gaussian:bool Add Noise distribution; monochromatic:bool Add Noise; seed:number Add Noise; "
     "opacity:number=100 Percent; blend:<blend> How it blends over what is below it in the stack"},
    {"smartObject.filters", "A smart object's Smart Filters: the stack's switch, its shared mask, and each entry in running order (index 0 is applied first) with its settings, switch, opacity and blend; drawn false marks one NekoPhoto does not draw (the stack is then read-only).",
     "id:layer The smart object layer (default: active)"},
    {"smartObject.setFilter", "Change one Smart Filter (its settings, switch, opacity, blend), or with no index switch the whole stack; one undo step. Settings not given keep their values.",
     "id:layer The smart object layer (default: active); index:integer Entry in running order (smartObject.filters); enabled:bool On or off (the entry, or the stack without index); radius:number Pixels (blurs, high pass, median, dust and scratches, surface blur, unsharp mask); threshold:number Levels (dust and scratches, surface blur, unsharp mask); "
     "amount:number Percent (unsharp mask, emboss, add noise) or Radial Blur's amount; angle:number Degrees (motion blur, emboss); distance:number Motion Blur pixels; "
     "highlight:number Plastic Wrap; detail:number Plastic Wrap; smoothness:number Plastic Wrap; cellSize:number Mosaic pixels; height:number Emboss pixels; "
     "samples:number Radial Blur 8, 16 or 32; gaussian:bool Add Noise distribution; monochromatic:bool Add Noise; seed:number Add Noise; "
     "opacity:number Percent; blend:<blend> How it blends over what is below it in the stack"},
    {"smartObject.removeFilter", "Delete one Smart Filter, or all of them (Clear Smart Filters); the last one takes the stack and its mask with it.",
     "id:layer The smart object layer (default: active); index:integer Entry in running order; all:bool=false Every Smart Filter"},
    {"smartObject.moveFilter", "Move a Smart Filter to another place in the running order.",
     "id:layer The smart object layer (default: active); index:integer! Entry to move; to:integer! Its new place (0 runs first)"},
    {"smartObject.filterMask", "The Smart Filters' shared mask: turn it on or off, invert it, delete it (all white), or select it for painting (brush.stroke with mask true, fills, gradients and filters then work on it; selecting a layer ends that).",
     "id:layer The smart object layer (default: active); action:(enable|disable|invert|delete|select|deselect)! What to do; show:bool=false With select: show the mask on the canvas"},
    {"layers.warp", "Warp a layer with one of Photoshop's presets: Warp Text on text (style none removes it), a mesh baked into a smart object's placement (redrawn from its contents), bent pixels otherwise.",
     "id:layer The layer (default the active one); style:string arc, arc lower, arc upper, arch, bulge, shell lower, shell upper, flag, wave, fish, rise, fisheye, inflate, squeeze, twist, or none (text); "
     "bend:number=50 Percent -100..100; horizontal:number=0 Horizontal distortion, percent; vertical:number=0 Vertical distortion, percent; orientation:(horizontal|vertical)=horizontal The warp's axis"},
    {"smartObject.editContents", "Open a smart object's contents in a new tab; smartObject.commit in that tab puts them back into every layer placing them. A smart object made from a camera RAW file (document.open with asSmartObject) is developed again instead, with settings (or its own), as one undo step; the reply carries its settings.",
     "id:layer The smart object layer (default: active); settings:object Camera RAW smart objects only: the new develop, keys as pixels.cameraRaw's settings (replacing the old ones). White balance for RAW: temperature in kelvin 2000..50000 with tint -150..150 (or rawTemperature and rawTint), whiteBalance As Shot|Auto|Custom or a preset the file records (Daylight|Cloudy|Shade|Tungsten|Fluorescent|Flash); Auto without numbers solves the white point; nothing about white balance opens As Shot; a temperature within -100..100 is the older form, relative to as shot"},
    {"smartObject.commit", "In a contents tab, put the contents back into the smart object they came from (as Save does).", ""},
    {"layers.render", "One layer alone as PNG, not composited with the others: with a mask, as it shows (mask applied, over the layer's bounds in document pixels); otherwise its own pixels.",
     "id:layer! The layer; masked:bool=true Apply the layer's mask; false gives the raw pixels; maxSize:number=1024 Longest side; path:string Write the PNG here instead of returning base64; overwrite:bool=false Replace an existing file at path"},
    {"adjustments.get", "An adjustment layer's settings.", "id:layer The adjustment layer (default the active one)"},
    {"adjustments.set", "Change an adjustment layer's settings (keys not given keep their values).",
     "id:layer The adjustment layer (default the active one); settings:object! Settings, shaped as adjustments.defaults shows"},
    {"adjustments.defaults", "The default settings of an adjustment kind: the shape adjustments.set and pixels.adjust take.", "kind:<adjustment>! The kind"},
    // pixels
    {"pixels.adjust", "Apply an adjustment destructively to the active layer's pixels, inside the selection.",
     "kind:<adjustment>! The adjustment; settings:object Settings over the defaults (see adjustments.defaults)"},
    {"pixels.filter", "Run a filter on the active layer's pixels, inside the selection; settings not given take the filter's defaults. "
                     "The distortions and Offset work inside the selection's bounds (the whole layer without one); Clouds paints between the "
                     "foreground and background colours.",
     "kind:<filter>! The filter; radius:number In pixels: Gaussian Blur, Box Blur, Surface Blur, Dust & Scratches, Median, Unsharp Mask, High Pass, Maximum, Minimum; "
     "angle:number In degrees: Motion Blur, Emboss, Twirl; distance:number Motion Blur distance in pixels; "
     "amount:number Add Noise and Unsharp Mask in percent, Emboss in percent, Radial Blur 1..100, Pinch, Spherize, Ripple, ZigZag and Shear -100..100 (Ripple -999..999); "
     "gaussian:bool Add Noise: Gaussian rather than uniform; monochromatic:bool Add Noise: grey noise; seed:integer=1 The pattern of Add Noise, Wave, Clouds and Difference Clouds; "
     "distortion:number Lens Correction distortion -100..100; bicubic:bool Lens Correction: sharper resample; "
     "threshold:integer Dust & Scratches, Surface Blur, Unsharp Mask threshold in levels; height:integer Emboss height in pixels; "
     "cellSize:integer Mosaic cell size in pixels; quality:(draft|good|best) Radial Blur quality; "
     "mode:string Spherize: normal, horizontalOnly or verticalOnly, Polar Coordinates: rectangularToPolar or polarToRectangular; "
     "size:(small|medium|large) Ripple size; style:(aroundCenter|outFromCenter|pondRipples) ZigZag style; ridges:number ZigZag ridges 0..20; "
     "type:(sine|triangle|square) Wave type; generators:integer Wave generators; wavelengthMin:number Wave; wavelengthMax:number Wave; "
     "amplitudeMin:number Wave; amplitudeMax:number Wave; undefinedAreas:(wrap|repeat|transparent) Wave, Shear and Offset (transparent: Offset only); "
     "preserve:(squareness|roundness) Minimum and Maximum; horizontal:integer Offset right in pixels; vertical:integer Offset down in pixels"},
    {"pixels.mosh", "Filter > Mosh: one of OpenMosh's effects on the active layer's pixels, inside the selection "
                    "(docs/mosh.md lists every effect and its parameters). Replies with the settings it applied.",
     "effect:string! An OpenMosh id: glitch (soft-glitch, hard-glitch, decimate, data-mosh, splitter, jitter, slices, shake, pixel-sort, strobe), "
     "distort (wave, bulge, stretch, push, luma-mesh, transform-3d, tile, kaleidoscope, mirror, wobble, smear, twirl, optical-flow), "
     "retro (pixelate, scanlines, vhs, super8, cga-8bit, crt, dither, bad-tv, dot-screen, halftone, ascii), "
     "stylize (bleach, edges, emboss, vignette, noise-displace, watercolor, zoom-blur, glow, light-streak, feedback), "
     "color (color-correction, duotone, solarize, chromatic-warp, sepia), composite (overlay, mask, mask-blocks, chroma-key, caption); "
     "params:object Parameters by OpenMosh's keys (e.g. {\"low\": 0.2, "
     "\"vertical\": true}); numbers, booleans for switches, an index or an option's name for a choice; the rest keep their defaults; "
     "seed:number=0 The random pattern of a seeded effect, 0..100; "
     "layer:string The id of the layer overlay blends over and mask reads its mask from (required for those two), where it lies over the "
     "active layer in the document; text:string The text caption stamps (required for caption)"},
    {"pixels.cameraRaw", "Filter > Camera Raw Filter on the active layer's pixels, inside the selection; replies with the normalized settings it applied.",
     "settings:object! The grade, keys as the model (defaults leave the image alone): whiteBalance (Custom|Auto), temperature, tint (relative, -100..100), exposure -5..5, "
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
    {"pixels.copy", "Edit > Copy: the active layer's pixels (or its mask's) inside the selection, or all of them, to the clipboard; other apps get them at 8 bits, sRGB. layers.copy copies whole layers.", ""},
    {"pixels.copyMerged", "Edit > Copy Merged: every visible layer's pixels inside the selection (or the whole canvas) to the clipboard.", ""},
    {"pixels.cut", "Edit > Cut: copy the selected pixels of the active layer to the clipboard and clear them (needs a selection).", ""},
    {"pixels.paste", "Edit > Paste of pixels: what pixels.copy, pixels.cut or another app put on the clipboard, as a new layer above the active one (where they were copied from, or centred), converted to the document's mode, profile and depth; into the channel being edited when one is. Drops the selection. Answers the active layer.", ""},
    {"pixels.contentAwareFill", "Content-Aware Fill: fill the selection from its surroundings (needs a selection).",
     "sampling:(auto|all)=auto Where it copies from: a window around the selection, or a wider one (slower); "
     "output:(current|new)=current Fill the active layer, or put only the filled pixels on a new layer above it"},
    {"pixels.contentAwareMove", "Content-Aware Move: move the selected pixels of the active layer dx, dy; the hole they leave is filled from its surroundings and the patch blended into its new place. The selection follows.",
     "dx:number! Horizontal offset; dy:number! Vertical offset; mode:(move|extend)=move Extend leaves the original and adds the copy; "
     "adaptation:integer=2 0 very strict .. 4 very loose: how far the patch's tone adapts and how wide a seam is blended"},
    {"pixels.contentAwareScale", "Content-Aware Scale the active layer by seam carving: low-detail seams are removed or duplicated so the subject keeps its proportions.",
     "width:integer New width in pixels; height:integer New height in pixels; widthPercent:number=100 Or the width in percent; heightPercent:number=100 Or the height in percent; "
     "protectSelection:bool=false Keep the selected pixels"},
    {"pixels.gmic", "Run a G'MIC command line on the active layer, inside the selection. Only catalogue filters and common built-ins followed by numbers are allowed.",
     "command:string! E.g. \"fx_bokeh 3,8,0,30\", see gmic.filters; timeoutMs:integer=300000 Give up after this long, 1..600000 milliseconds"},
    {"gmic.filters", "The G'MIC filter catalogue with parameters and defaults; filters that do not work here are left out.",
     "search:string Only filters whose name or command contains this; all:bool=false Include the unsupported filters, with the reason"},
    {"pixels.removeBackground", "Mask the active layer's background away with the background-removal model (enable and download it in Preferences first).",
     "refine:bool=true Refine the edge; refineEdges:number Edge refinement radius; contrast:number Edge contrast; shiftEdge:number Move the edge in (negative) or out; "
     "matting:number Band width in pixels in which hair opacity is solved; cleanup:bool Remove half-transparent specks touching no edge (default true); "
     "decontaminate:bool Edge pixels take the subject's own colour (default true); detail:bool=false Run the model again on full-resolution windows along the edge of a large photo; "
     "detailWindows:number=12 At most this many windows; flip:bool Average the mask with the mirrored image's (default the preference)"},
    // selection
    {"selection.info", "The selection: whether there is one, its bounds and area.", ""},
    {"selection.render", "The selection as a greyscale PNG mask.", "maxSize:number=1024 Longest side; path:string Write the PNG here instead of returning base64; overwrite:bool=false Replace an existing file at path"},
    {"selection.all", "Select the whole canvas.", ""},
    {"selection.none", "Deselect.", ""},
    {"selection.invert", "Invert the selection.", ""},
    {"selection.reselect", "Select > Reselect: bring back the last selection after it was dropped (on the same canvas, at the same depth).", ""},
    {"selection.quickMask", "Quick Mask: the selection as a mask to paint (brush.stroke with mask true, white selects; gradients, fills and filters too), then back.",
     "on:bool Enter (true) or leave (false, the mask becoming the selection); left out, it toggles"},
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
    // channels
    {"channels.list", "The Channels panel: the colour channels edits write to (activeColors) and the canvas shows, the alpha channel being edited (target), "
     "whether Quick Mask is on, and every alpha and spot channel (id, name, kind, colour, opacity, colorIndicates, visible, target).", ""},
    {"channels.new", "New Channel: a black alpha channel (nothing selected), or with fromSelection the selection saved as one; it becomes the target.",
     "name:string The channel's name (default Alpha N); fromSelection:bool=false Save the selection into it"},
    {"channels.duplicate", "Duplicate an alpha or spot channel, placed after it.", "id:string! The channel; name:string The copy's name (default \"<name> copy\")"},
    {"channels.delete", "Delete an alpha or spot channel.", "id:string! The channel"},
    {"channels.select", "Make a channel the target, as a click in the Channels panel: rgb (all colour channels, the usual case), red, green or blue (edits then "
     "change only it and it shows alone in grey), or an alpha channel's id (painted like Quick Mask: brush.stroke with mask true, white selects).",
     "channel:string! rgb, red, green, blue or a channel id; extend:bool=false Shift-click: add a colour channel to the target, or show an alpha channel beside it"},
    {"channels.set", "Channel Options and the eye: rename, colour, opacity, what the colour indicates, order, visibility (colour channels: visibility only).",
     "channel:string! rgb, red, green, blue or a channel id; name:string New name; color:string #rrggbb overlay colour; opacity:number Overlay opacity 0..1; "
     "colorIndicates:(masked|selected) What the colour shows (the gray is inverted so the channel keeps what it selects); index:integer New place among the channels; "
     "visible:bool Show or hide it"},
    {"channels.saveSelection", "Select > Save Selection: the selection into a new alpha channel, or into an existing one combined in a mode.",
     "id:string Into this alpha channel (default a new one); name:string A new channel's name; mode:<selectionMode>=replace How it combines with the channel"},
    {"channels.loadSelection", "Select > Load Selection, or a Ctrl-click on a channel's thumbnail: a channel, the composite's luminosity (rgb), a colour channel, "
     "or a layer's transparency or mask, as the selection.",
     "channel:string rgb, red, green, blue or a channel id; layer:layer Instead, a layer's transparency; mask:bool=false With layer: its mask; invert:bool=false Invert it first; "
     "mode:<selectionMode>=replace How it combines; shift:bool The thumbnail's Shift (add; with alt, intersect); alt:bool The thumbnail's Alt (subtract)"},
    // painting
    {"brush.presets", "The MyPaint brush presets and imported brushes: id, name, group, size, whether an eraser.", "group:string Only this group"},
    {"brush.import", "Import brushes: Photoshop .abr, Procreate .brushset/.brush, Clip Studio .sut, or images as tips.",
     "path:string One file; paths:array Several files"},
    {"presets.import", "Import Photoshop presets into the library: styles (.asl, with the patterns they use), patterns (.pat, also added to the open document) and gradients (.grd). A style or gradient with an existing name replaces it.",
     "path:string One file; paths:array Several files"},
    {"presets.list", "The imported presets: styles (name and the pattern ids they use), gradients (name, colour and opacity stops) and patterns (id, name, size).",
     "kind:(styles|gradients|patterns) Only this kind"},
    {"presets.remove", "Remove an imported style, gradient or pattern from the library (the document keeps patterns it was given).", "kind:(style|gradient|pattern)! Which list; name:string! The preset's name (a pattern's id or name)"},
    {"brush.stroke", "Paint a stroke through points on the active layer (or its mask); the person's tool and settings are put back afterwards.",
     "points:array! [x, y] points in document pixels; tool:(brush|eraser|healing|healingbrush|clone|smudge|blur|sharpen|liquify|dodge|burn|sponge)=brush The tool (healingbrush heals from source, as clone copies; for dodge, burn and sponge opacity is the Exposure or Flow); size:number Diameter in pixels; hardness:number 0..1; opacity:number 0..1; "
     "color:color Paint colour (default the foreground); mask:bool=false Paint the active layer's mask; erase:bool=false Erase with the brush; source:object {x, y} Clone and Healing Brush source; "
     "preset:string A preset id from brush.presets, or round; pressure:number=0.5 Pen pressure 0..1; pressures:array One pressure per point; "
     "tilts:array One [tiltX, tiltY] per point, degrees from upright; twists:array One barrel rotation per point, degrees; times:array One time per point, seconds (8 ms apart by default); seed:number The tip brushes' jitter seed, to repeat a stroke exactly; viewScale:number=1 The zoom the stroke is taken as drawn at (2 is 200%), which speed-on-screen dynamics read; "
     "smoothing:number=0 Brush and Eraser: the stabiliser, 0..100 (the options bar's Smoothing); pulledString:bool=false The brush moves only once the string is taut; strokeCatchUp:bool=true The brush keeps closing on a paused pen; "
     "catchUpOnEnd:bool=false The stroke ends where the pen lifted; adjustForZoom:bool=true The stabiliser's reach is on the screen; inputSmoothing:number=0 Tablet jitter filter, 0..100; pressureSmoothing:number=0 Pressure filter, 0..100; "
     "range:(shadows|midtones|highlights)=midtones Dodge and Burn: the tones they work on; protectTones:bool=true Dodge and Burn keep the colour; saturate:bool=false Sponge saturates instead"},
    {"pixels.patch", "Patch: replace the selection's pixels on the active layer with those dx, dy away, their tone matched to the selection's edge.",
     "dx:number! Horizontal offset to copy from; dy:number! Vertical offset to copy from"},
    {"pixels.bucket", "Paint Bucket: fill the pixels like the one at x, y on the active layer (or its mask), inside the selection.",
     "x:number! Document x; y:number! Document y; color:color Fill colour (default the foreground); opacity:number=1 0..1; tolerance:integer=32 0..255; "
     "contiguous:bool=true Only pixels connected to the point; antialias:bool=true Soften the edge; allLayers:bool=false Compare with the document as shown, not the active layer"},
    {"gradient.draw", "Draw a gradient on the active layer, inside the selection.",
     "x0:number! Start; y0:number! Start; x1:number! End; y1:number! End; shape:(linear|radial)=linear Shape; "
     "style:(foreground-to-transparent|foreground-to-background)=foreground-to-transparent Colours; reversed:bool=false Swap the ends; opacity:number=1 0..1; foreground:color Start colour; background:color End colour; preset:string An imported gradient preset by name (presets.list), which replaces style; "
     "interpolation:(classic|perceptual|linear)=classic Photoshop's Method: the space the colours blend in (Classic the stored values, Linear linear light, Perceptual Oklab)"},
    {"shape.draw", "Add a vector shape layer (editable: shape.get, shape.set, and PSD's own shape layer on export).",
     "x:number! Left (a line's start); y:number! Top; width:number Width; height:number Height; kind:(rectangle|ellipse|polygon|star|line|custom)=rectangle Shape; "
     "cornerRadius:number=0 Rectangle corners; sides:integer=5 Polygon or star points; star:number Star inset 0..0.99; x2:number Line end x; y2:number Line end y; weight:number=4 Line weight; "
     "name:string Custom shape (Heart, Star, Arrow, Speech Bubble, Check Mark, Lightning), or else the layer's name; color:color Fill (default the foreground); fill:bool=true Filled; "
     "stroke:bool Stroked; strokeWidth:number Pixels; strokeColor:color Stroke colour; strokeAlign:(inside|center|outside) Where the stroke sits; strokeDashes:array Dash pattern in stroke widths; "
     "fillType:(color|gradient|pattern) Fill paint; gradient:string A gradient preset by name (presets.list; empty: foreground to background); gradientType:(linear|radial|angle|reflected|diamond) Gradient shape; gradientAngle:number Degrees; pattern:string One of the document's patterns (id or name); strokeType:(color|gradient|pattern) Stroke paint; strokeGradient:string Stroke gradient preset; strokePattern:string Stroke pattern; op:(combine|subtract|intersect|exclude) Add it to the active shape layer as a component combined this way instead of making a layer"},
    {"paths.list", "The document's paths (Photoshop's Paths panel): the Work Path (id 1025) and saved paths, each with its knots.", ""},
    {"paths.set", "Make or replace a path: a new saved path (name), the Work Path (work true), or path id replaced.",
     "path:array! Subpaths as shape.get gives them; id:integer An existing path to replace; name:string Name (a new path, or a rename); work:bool=false Make it the Work Path"},
    {"paths.select", "Choose the path the Pen and Direct Selection work on (id left out: none, the active shape layer's path).", "id:integer The path"},
    {"paths.delete", "Delete a path.", "id:integer! The path"},
    {"paths.fill", "Fill a path on the active layer with the foreground colour at the brush's opacity.", "id:integer! The path"},
    {"paths.stroke", "Stroke a path on the active layer with the brush's size and opacity in the foreground colour.", "id:integer! The path"},
    {"paths.toSelection", "Load a path as the selection.", "id:integer! The path; mode:<selectionMode>=replace How it combines"},
    {"paths.toShape", "Make a vector shape layer from a path, filled with the foreground colour.", "id:integer! The path"},
    {"paths.addAnchor", "Add an anchor on the target path's outline (the chosen path, or the active shape layer's), the curve split so its shape stays.",
     "x:number! Document x; y:number! Document y; radius:number=6 How far from the outline the point may be"},
    {"paths.deleteAnchor", "Delete the target path's anchor at a point.", "x:number! Document x; y:number! Document y; radius:number=6 How far from the anchor the point may be"},
    {"paths.fromSelection", "Make the Work Path from the selection's outline.", "tolerance:number=1 How far (pixels) the path may stray to use fewer points"},
    {"shape.get", "A vector shape layer's path, fill and stroke.", "id:layer! The layer"},
    {"shape.set", "Change a vector shape layer: its path, fill or stroke (keys as shape.draw's; path as shape.get gives it).",
     "id:layer! The layer; path:array Subpaths {closed, op, knots: [[inX, inY, x, y, outX, outY] or [x, y], ...]}; color:color Fill; fill:bool Filled; stroke:bool Stroked; "
     "strokeWidth:number Pixels; strokeColor:color Colour; strokeAlign:(inside|center|outside) Placement; strokeDashes:array Dash pattern; "
     "fillType:(color|gradient|pattern) Fill paint; gradient:string A gradient preset by name (presets.list; empty: foreground to background); gradientType:(linear|radial|angle|reflected|diamond) Gradient shape; gradientAngle:number Degrees; pattern:string One of the document's patterns (id or name); strokeType:(color|gradient|pattern) Stroke paint; strokeGradient:string Stroke gradient preset; strokePattern:string Stroke pattern; live:object A live rectangle's or ellipse's properties {group, x, y, width, height, radius, radii: [topLeft, topRight, bottomRight, bottomLeft]}"},
    {"paths.setOperation", "Change how a component of the target path (the chosen path, the targeted vector mask, or the active shape's) combines with those before it.",
     "subpath:integer! A subpath of the component (its index in the path); op:(combine|subtract|intersect|exclude)! Photoshop's path operation"},
    {"paths.mergeComponents", "Merge Shape Components: flatten the target path's components into add-only outlines (curves become corner points).", ""},
    {"vectorMask.get", "A layer's own vector mask (not a shape layer's path): its subpaths and whether it is inverted.", "id:layer! The layer"},
    {"vectorMask.set", "Give a layer a vector mask or replace it (Layer > Vector Mask): Reveal All, Hide All, the chosen path, or a path.",
     "id:layer! The layer; mode:(revealAll|hideAll|currentPath|path) What it starts as (default path when path is given, else revealAll); path:array Subpaths as paths.list gives them; inverted:bool Hide inside instead"},
    {"vectorMask.delete", "Delete a layer's vector mask.", "id:layer! The layer"},
    {"vectorMask.target", "Make a layer's vector mask the target path for the Pen, Direct Selection and paths.addAnchor, setOperation and mergeComponents.", "id:layer! The layer"},
    {"text.toPath", "Type > Create Work Path: a text layer's glyph outlines as the Work Path (id 1025).", "id:layer! The text layer"},
    {"text.toShape", "Type > Convert to Shape: a text layer becomes a shape layer of its glyph outlines, filled with its colour.", "id:layer! The text layer"},
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
    else if (list == "adjustment") for (int i = 0; i < adjustmentKindCount; i++) out << QString::fromUtf8(adjustmentKindName(AdjustmentKind(i)));
    else if (list == "filter") for (int i = 0; i < filterKindCount; i++) out << QString::fromUtf8(filterKindName(FilterKind(i)));
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
                                   "gradient", "shape", "text", "eyedropper", "hand", "zoom", "artboard", "slice"};
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
