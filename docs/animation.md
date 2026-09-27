# Frame animation

Window > Timeline is Photoshop's Timeline in frame mode. A document can hold frames; each frame is a snapshot of
every layer's visibility, position and opacity, and how long it shows. The layers themselves (pixels, masks, styles,
text) are shared by all frames, exactly as in Photoshop: painting on a layer changes it in every frame that shows it.

## Using it

- **Create Frame Animation** makes the first frame from the layers as they are. **Make Frames From Layers** makes
  a frame per top-level layer, each showing that layer alone (how a stack of cels becomes an animation).
- Click a frame to select it: the layers then show that frame. Hide or show layers, move them, change their opacity,
  and the change goes into the selected frame only. Anything else (painting, filters, new layers) is shared.
- **+** adds a copy of the selected frame after it (Photoshop's New Frame); the bin deletes it; the arrows move it
  earlier or later. Deleting the last frame turns the document back into a still one.
- **Delay** is how long the selected frame shows, in milliseconds; **All** gives every frame that delay.
- The loop menu sets how many times the animation plays: Forever, Once, 3 times, or any number.
- **Play** shows the frames on the canvas at their delays without touching the undo history or the frames; any
  edit, save or export, or Stop, returns to the layers as they were. Saves and autosaves always write the document
  itself, never the frame playback happens to show.

Every change to the frames (select, add, delete, move, delay, looping) is one undo step.

A layer added after a frame was made is not listed in that frame, so it keeps whatever state it has when the frame
shows; select each frame and hide it where it should not appear, as in Photoshop.

## Files

- **Projects** keep the frames in the manifest's `animation` object ([project-format.md](project-format.md)). Older
  projects open as still documents, and the Mac app ignores the key.
- **Animated GIF export** (File > Export Animated GIF, or `document.export` to a `.gif`) writes each frame flattened,
  quantised to its own palette of up to 255 colours (the exact colours when there are few enough, else a median cut)
  with pixels below half opacity transparent, the delays rounded to hundredths of a second and the loop count in the
  NETSCAPE2.0 block (none for Once). A document without frames exports as a still GIF. The LZW encoder follows
  Patchy's (MIT).
- **Animated GIF import** opens each frame as a layer ("Frame N (D ms)") and sets up the timeline: frame N shows
  layer N alone, with the GIF's delays and loop count. Longer GIFs than 2000 frames open as layers without a timeline.
- **Aseprite import** reads every frame: a layer per cel ("Layer (frame N)" when an Aseprite layer has more than
  one), linked cels sharing one layer, and a timeline whose frames show the right cels for the frame durations.
- **PSD**: frames are not written to or read from Photoshop files yet (Photoshop keeps them in the `AnDs` image
  resource and each layer's `shmd` metadata). A PSD export shows the current frame.
- APNG export is not there yet.

## Automation

`timeline.info`, `timeline.frame` and `timeline.set` ([automation.md](automation.md)) do everything the panel does;
`document.export` to a `.gif` writes the animation.
