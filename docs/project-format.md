# NekoPhoto project format

A NekoPhoto project comes in two forms that hold the same files under the same names:

- **A `.nekophoto` file** (since 1.8.7, and what new projects and Save As write by default): one ZIP file,
  described below.
- **A `.comp` folder**, the project package NekoPhoto wrote before 1.8.7 and Compositor for macOS writes: a folder
  holding `manifest.json`, an `images/` folder of `<layer UUID>.png` assets and, for newer features, the other files
  the manifest names. These still open, and save back as folders; Save As can write one too.

The path decides the form: a path ending in `.nekophoto` is written as a file, any other as a folder. Both are
replaced atomically: a `.comp` is written as a sibling folder that is then swapped in, a `.nekophoto` as
`<name>.nekophoto.saving` beside the target, which is flushed to disk, read back, and renamed over the old file. A save
that fails leaves the previous project as it was.

## The `.nekophoto` file

A ZIP file (no ZIP64, so at most 4 GiB and 65,535 entries). Its entries, in this order:

| Entry | Contents |
| --- | --- |
| `mimetype` | `application/vnd.nekophoto.document`, ASCII, no newline; stored (not compressed) and always first, as in ODF and EPUB, so the bytes at offset 30 read `mimetypeapplication/vnd.nekophoto.document` and identify the file without unpacking it |
| `nekophoto.json` | the container header, below |
| `manifest.json` | the manifest, exactly as in a `.comp` folder (everything after "The manifest" below) |
| `images/…`, `channels/…`, `smartobjects/…`, `profile.icc`, `encoded.icc` | the files the manifest names, under the same relative paths as in a `.comp` folder, byte for byte |
| `previews/composite.png` | the flattened image, scaled down to at most 1,024 pixels on its long side, for file managers and thumbnailers; never read back |

JSON entries are deflated; images and other entries are stored, since PNGs are already compressed. Names are UTF-8,
`/`-separated and relative. A reader refuses names with `..`, `.` or empty parts, absolute names, backslashes, colons,
names that repeat (also differing only in case), encrypted or ZIP64 entries, and entries that claim more data, or a
higher compression ratio, than the limits allow.

`nekophoto.json`:

```json
{
  "format": "NekoPhoto Document",
  "format_id": "org.nekophoto.document",
  "version": 1,
  "minimum_reader_version": 1,
  "writer": "NekoPhoto 1.8.7"
}
```

`version` is the container's version and moves apart from both the app's version (`writer`, for information) and the
manifest's own `version`. A reader opens a file whose `minimum_reader_version` is at most the container version it
knows (1), and refuses a newer one with a message asking for a newer NekoPhoto. Entries it does not know (another
version's additions) are kept with the document and written back unchanged when it is saved as `.nekophoto` again.

The MIME type is `application/vnd.nekophoto.document` (glob `*.nekophoto`, and the magic above); a `.comp` folder
keeps `application/x-compositor-project`.

## The manifest

The manifest identifies `com.compositor.project`, a version from `1` to `9` (a save writes the lowest version that
holds the document: `7` while it needs nothing newer, so Compositor for macOS keeps opening it; see versions 8 and 9
below), and the working space. It stores document UUID, pixel dimensions, active layer UUID, and layers in bottom-to-top order. Each layer stores its UUID, name, visibility, transform (origin, size, clockwise rotation, flips, sampling), and optional image filename. Blank layers have no image asset.

Embedded PNGs preserve source pixels and transparency; transforms remain separate. Projects survive moving or deleting imported source photos. Saving replaces the project atomically (above). Unsupported versions, invalid metadata, missing assets, unsafe paths, and oversized data are rejected before replacing the live document.

Limits in Compositor for macOS (NekoPhoto opens and saves up to a gigapixel of layers): 30,000 pixels per canvas/image side, 100 million total source pixels, 10,000 layers, 4 MiB manifest, 512 MiB per encoded asset. See `ProjectStore.swift` for validation.

Undo history and viewport are session-only. Opening fits the canvas, restores selection, and starts with clean history. Future editable features must extend the schema and round-trip tests. PNG export is a flattened derivative and does not mark project edits saved.

Image Size adds optional `resolution` (pixels/inch, 1–9600). Older manifests without it default to 72. This additive field retains version 1 compatibility. Both PNG and JPEG exports include document resolution metadata. Resampling stores the new layer pixels and bounds; undo retains the prior sources only during the current session.

Version 2 adds optional `parentID` and `isGroup` on layer records. A group has no image file. Root nodes have no parent; children refer to an existing group. Array order defines bottom-to-top sibling order; renderers traverse each group as a contiguous subtree. Visibility is inherited without changing child flags. Cycles, missing/non-group parents, image-bearing groups, and nesting beyond 64 ancestor levels are rejected. Group ancestors permit room for leaf nodes at the deepest level. Group metadata survives image/canvas resizing and cropping. Older app builds reject version 2 rather than misrender grouped documents. Collapse state is not serialized.

Version 3 adds optional per-layer `opacity` (finite 0–1) and `blendMode` (Normal, Multiply, Screen, Overlay, Darken, Lighten, Difference, Color Dodge, Color Burn). Missing fields default to full opacity and Normal. Group records currently require those defaults; their children can have independent effects. Effects are applied during compositing and retained as metadata when resizing sources. Files declaring older versions cannot contain non-default appearance values.

Version 4 adds optional `maskFile` and `maskEnabled` fields to individual layers. Mask filenames must be `<layer UUID>.mask.png` under `images/`; enabled defaults to true when a mask exists. Records without masks omit both fields. Groups cannot carry masks in this version. Files declaring versions 1–3 cannot contain mask metadata.

Masks store 8-bit grayscale coverage without alpha (white reveals, black hides). Their normalized extent matches the image’s local rectangle, so the same layer transform applies to both. A uniform 1×1 mask is valid and avoids allocating full-resolution pixels before painting. Nonuniform mask pixels and a thumbnail are immutable assets shared by history. Image Size resamples them with the image transform; Canvas Size and Crop preserve their pixels. Up to 100 million mask pixels may be stored in addition to the existing 100 million image pixels; per-side and per-file limits also apply to masks. Disabled masks remain embedded and editable but do not affect compositing. Image-versus-mask target selection is session-only and reopens on image pixels.

Version 5 adds optional `maskSourceID`: the UUID of a non-group layer supplying live alpha in document coordinates. It multiplies the target’s alpha alongside its enabled raster mask. Source pixels, transform, opacity, raster mask and upstream live masks contribute coverage; visibility and RGB color do not. Sources remain independent layers. Missing references, self-links, cycles, group endpoints and chains over 256 nodes are rejected. Deletion can bake the live coverage into dependent image pixels (retaining their raster masks) or remove the links, as one undoable operation. Links survive image/canvas resize and crop. Older versions default to no live mask; older app builds reject v5.

UI terminology: these alpha links are clipping masks. Option-click assigns the lower sibling’s base or releases the connection. Multiple clipped layers share one base, show indented above it, and release when moved outside the contiguous stack. The underlying `maskSourceID` representation is unchanged.

Version 6 allows `maskFile` and `maskEnabled` on group records. A folder has no image, so its mask covers the folder's own transform rectangle (the canvas size when the folder was created); Image Size resamples it through that transform, and Canvas Size and Crop preserve its pixels, exactly as for layer masks. Groups are pass-through, so an enabled folder mask multiplies the coverage of every descendant layer, together with that layer's own mask and any enclosing folders' masks; clipping-mask coverage is unaffected. Files declaring versions 1–5 cannot give a group a mask, and older app builds reject v6.

Version 8 (NekoPhoto) gives folders their own `opacity` and `blendMode`, and `passThrough` (default `true`):
a pass-through folder's children blend straight into what is below it and its opacity fades their result back
toward that backdrop; `passThrough: false` isolates the children and composites the folder's result in its
blend mode and opacity, as Photoshop does. A save writes version 7 whenever no folder uses these, so the Mac app
(which reads up to 7) still opens it; version 7 files cannot give a folder non-default values.

Version 8 also carries the document's depth (docs/bit-depth.md): `"sampleType": "u16"` makes every layer's
`imageFile` a 16-bit RGBA PNG and every `maskFile` a 16-bit grayscale PNG (0..65535 on disk, 0..32768 in memory,
the values mapped on the way in and out). An 8-bit document writes no `sampleType` and its PNGs as before; a manifest
without the key, or with `"u8"`, is 8-bit, and `sampleType` in a manifest below version 8 is refused as damage. The
byte budgets apply: a 16-bit project holds half the pixels of an 8-bit one.

`"sampleType": "f32"` (still version 8) is a 32-bit document ([bit-depth.md](bit-depth.md#32-bits-per-channel)): its
layers, masks and channels have no PNG form, and are float sidecars instead: `imageFile` is `<id>.f32z`, `maskFile`
`<id>.mask.f32z` and a channel's `file` `channels/<id>.f32z`. Each is a 24-byte header, `"NPF32Z"`, 0, 1 (magic and
version), the width and height as little-endian u32, u8 channels (4 for R, G, B and alpha, 1 for a gray), u8 32, u8 1
(zlib), a zero byte and a zero u32, then one zlib stream of the planes one after the other, rows top-down, each row in
PSD's predictor for 32-bit channels: the row's floats split into four byte planes (most significant first, big-endian),
then every byte replaced by its difference from the one before it in the row. The samples are as held: premultiplied
linear light, colour unbounded, alpha and grays 0..1; NaN and infinities are cleaned on load. `"encodedProfile"` says
which profile the document goes back to at 8 or 16 bits: `"encoded.icc"` (that file, verbatim) or `"untagged"`; without
the key it is the profile's gamma counterpart. The byte budgets hold a quarter of the 8-bit pixels.

Version 8 also carries a colour profile (docs/color-management.md): `"colorSpace": "icc"` with `"profile":
"profile.icc"` means the document's profile is the package's `profile.icc`, the ICC bytes kept verbatim. An untagged
document writes `"colorSpace": "sRGB"` and no profile, as before; `"icc"` below version 8, or a `profile` key with
`"sRGB"`, is refused as damage, and an unreadable `profile.icc` leaves the document untagged.

Version 8 also carries alpha and spot channels ([channels.md](channels.md)): a `channels` array of `{id, name, kind`
(`"alpha"` or `"spot"`)`, color` (`[r, g, b]`, 0..1)`, opacity, colorIndicates` (`"masked"` or `"selected"`)`, file}`,
where `file` is `channels/<id>.png`, a gray PNG of the canvas's size at the document's depth, and
`channels/<id>.psdcarry` keeps what a PSD said about the channel. A missing or wrongly sized channel PNG is damage;
`channels` below version 8 is refused; a document without channels writes no key.

Version 9 is a CMYK or Lab document ([color-management.md](color-management.md)): `"colorMode": "cmyk"` or `"lab"`.
An RGB document writes no `colorMode` and stays at version 8 or below, so older releases keep opening it; the key below
version 9, or another value, is refused as damage. Masks and channels are gray PNGs in every mode.

- **Lab** layers are 4-sample PNGs at the document's depth holding L, a, b and alpha as they are (a and b offset by 128
  at 8 bits and 16384 at 16); the manifest's `colorMode` is what says they are Lab.
- **CMYK** layers have no PNG form: `imageFile` is `<id>.cmyk`, a 24-byte header then the five planes compressed.
  The header is `"NPCMYK"`, 0, 1 (magic and version), the width and height as little-endian u32, then u8 channels
  (5), u8 bits (8 or 16), u8 compression (1 zlib, 2 zstd), a zero byte and a zero u32. The planes are C, M, Y, K and
  alpha one after the other, rows top-down, 16-bit samples little-endian in 0..32768; the samples are as the document
  holds them: premultiplied, the inks inverted (0 is full ink, as in PSD). NekoPhoto writes zlib and reads zstd too
  when built with it. A colour sample above its alpha is clamped to it on load.
- `profile.icc`, when present, must be a profile of the document's mode (a CMYK profile for CMYK); another is dropped.
- The loader counts each layer's decoded bytes (samples times channels) against `ProjectLoadLimits::layerBytes` and
  the document's own budget, which is bytes: a CMYK layer holds four fifths the pixels of an RGB one.

## Frame animation (NekoPhoto)

A document with frames (Window > Timeline) adds an `animation` object to the manifest; readers that do not know it
ignore it, and a manifest without it is a still document, so no format version changes:

```json
"animation": {
  "loopCount": 0,
  "current": 1,
  "frames": [
    {"delay": 100, "layers": {"<layer UUID>": {"visible": true, "x": 0, "y": 0, "opacity": 1}}}
  ]
}
```

`loopCount` is how many times the animation plays (0: forever); `current` is the frame the layers' own visibility,
origin and opacity show; each frame's `delay` is in milliseconds and `layers` holds each layer's visibility, transform
origin (`x`, `y`) and opacity in that frame. A layer a frame does not list keeps its own state when the frame shows.
Entries for layers that no longer exist are dropped on load, and a damaged `animation` object is dropped rather than
refusing the project (see `src/core/include/compositor/animation.h`).
