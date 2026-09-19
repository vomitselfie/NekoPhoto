# MCP and automation: what is there and what to do next

The state of the agent side in September 2026, after the background-removal
work landed, and a ranked list of improvements with effort in days. The
method reference is `docs/automation.md`, the practice `docs/agent-guide.md`.

## What exists

- A JSON-RPC socket in the editor (`--rpc`, `--headless`, or the Automation
  preference) with 74 methods: documents and tabs, layers and masks,
  adjustments, filters and G'MIC, painting, selections including Quick Select,
  Remove Background with every panel setting and the detail pass, canvas and
  history, view state, and events.
- `mcp/compositor_mcp.py`: a stdio MCP bridge with 54 tools over those methods
  and a generic `rpc` tool for the rest; renders, layer renders and screenshots
  come back as PNG images; it launches the editor when nothing listens, headless
  without a display, and reconnects once when the editor goes away.
- The single-instance handoff (September 2026): when the editor is already open,
  the bridge's launch hands its request to it and the running window starts
  listening, so the agent works in the document the person is looking at.
- `tools/rpc_smoke.py` exercises every method in CI; `.mcp.json` registers the
  bridge for Claude Code in this checkout.

## Ranked

| # | Improvement | Why | Where | Days |
|---|---|---|---|---|
| 1 | **An MCP smoke test in CI.** Drive the bridge over stdio with the MCP Python client: list tools, open the demo, render, run a few edits, undo. The socket is tested every push; the bridge is not. | The bridge is the piece agents actually touch, and the SDK it pins moves. | `tools/mcp_smoke.py`, a CI step that installs `mcp<2` | ½ |
| 2 | **Close the gaps between socket and bridge.** `tabs_new`, `tabs_close`, `layers_get`, `layers_flip`, `layers_reorder`, `canvas_flip`, `colors_set`, `tool_select`, `view_zoom`, `history_info`, `selection_all`, `selection_none`, `selection_invert` as first-class tools, so the generic `rpc` tool is never needed for ordinary work. | Tool descriptions are what the model reads; a method hidden behind `rpc` is one it will not find. | `mcp/compositor_mcp.py` | ¼ |
| 3 | **Tool annotations.** Mark the read-only tools (`render`, `layers_list`, `document_info`, ...) and the destructive ones (`document_close`, `pixels_clear`, `document_save` over an existing file) with the MCP annotations the 1.x SDK carries, and give tools titles. | Clients use them to skip confirmations on safe calls and ask on unsafe ones. | The bridge | ¼ |
| 4 | **A one-call overview.** `document_overview`: the document's size, the layer tree as indented text (kind, visibility, opacity, blend, mask, live text, size), the active layer, the selection's bounds, the history length, optionally with a small render. | The agent guide's "look before touching" is three calls today; most sessions start with them. | A `document.overview` method and its tool | ¼ |
| 5 | **Edit groups and batches.** `history.group` begin/end on the socket, so a sequence of calls becomes one undo step ("Retouch by agent"), and a `batch` tool that sends a list of calls in one round trip and stops at the first error. | Agents make many small calls; the person then undoes them one at a time. | `Automation.cpp` around `beginEdit`/`endEdit`, the bridge | ½ |
| 6 | **Progress for slow calls.** The detail pass, a G'MIC filter on a big layer and a model download take seconds to minutes with no sign of life. Emit progress events from the socket where the work reports it, and forward them as MCP progress notifications. | Clients cut long calls off; a progress stream keeps them waiting. | Events of kind `progress`, `Context.report_progress` in the bridge | ½–1 |
| 7 | **Small helpers agents ask for.** `color_at` (sample a document pixel), `fonts_list` (families for `text_set`, grouped as the picker groups them), `layer_bounds` in document pixels, `render` of a layer *with* its mask applied. | Each is a question an agent otherwise answers by rendering and guessing. | Methods and tools | ¼ each |
| 8 | **Errors that say what to do.** Map the editor's refusals to messages with the next step: the model is off ("enable it in Preferences or call `remove_background` after `compositor-linux --download-model isnet`"), no document, a locked headless dialog. | Today's messages are correct but terse; an agent retries blindly. | `fail()` sites in `Automation.cpp` | ¼ |
| 9 | **Resources and a prompt.** Expose the current render and the layer list as MCP resources, and one prompt that encodes the agent guide's loop. | Cheap; useful for clients that browse resources rather than call tools. | The bridge | ¼ |
| 10 | **Recipes for the new tools.** Remove Background with the detail pass and matting, Quick Select by scribble, the text tool, PSD import notes, in `docs/agent-guide.md`. | The guide predates all of them. | Docs | ¼ |

## Learned from the first agent session (September 2026)

An agent drove the editor on the person's display through the socket: opened a
photo, ran Remove Background with matting, found the foliage the model had
left on the subject with a colour search over a render, wand-selected it and
filled it with Content-Aware Fill, imported a second photo, sent it to the
bottom and fitted it to the canvas, exported and saved. Two things it hit:

- **Spot healing and Content-Aware Fill sampled pixels the mask hides.** Near
  the silhouette of a masked layer the healer pulled in the old, nearly black
  background. Fixed the same day: both healers take the layer mask's visible
  pixels as a validity map and treat what it hides as unknown, the way they
  treat transparent pixels.
- **Wrong-shape parameters fail late and tersely.** `layers.add` with a text
  object instead of a string returned "parameter 'text' must be a string" only
  after the guess; see item 8, and the one-call overview of item 4 would have
  shown the shape of an existing text layer.

Not planned: PSD export (a writer is a project of its own, unrelated to the
bridge), and remote transports (the socket is local by design; an agent on
another machine has no business in a person's editor).

## Suggested order

1, 2 and 3 in one pass (a day): the bridge becomes tested, complete and
self-describing. Then 4 and 5 (a day) for fewer round trips and tidy undo. Then
6 when a slow call actually bites, and the rest as they come up.
