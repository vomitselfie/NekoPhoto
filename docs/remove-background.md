# Remove Background: measurements

Findings from the `matte_tool` harness (`tests/matte_tool.cpp`) that decided what Remove Background does. The
datasets are research-licensed and stay local; the repository's tests use synthetic composites only.

## A second refinement pass (September 2026): not adopted

The question: after the full pipeline (model, detail pass, matting), can the final matte's own signals pick a
few more regions worth refining again? A pass like that only pays if some signal finds the remaining error
better than the count of the coarse mask's soft pixels, which already ranks the detail pass's windows.

`matte_tool signals` writes, per tile of the final matte, the summed value of each candidate signal and the
tile's error against the truth:

- the matting residual |C − (aF + (1 − a)B)| / (|F − B| + 0.05) (`MatteDebug::uncertainty`);
- candidate instability: the pairs chosen around a band pixel, scored on its own colour, and the spread of the
  opacities of the best four (`MatteDebug::instability`);
- the final alpha's local variance (5 × 5);
- the residual normalised by the local colour spread instead of the pair's separation;
- each of these added to the soft-pixel count, the weight picked on a tuning split.

AIM-500 (500 photographs, 1080–2066 px), isnet-general-use, detail pass of 12 windows, matting band 16. The
images were split by a hash of their names: 297 to choose the weights, 203 held out. Held-out results, tiles of
256 px (3529 tiles with edge pixels); capture@k is the mean share of an image's error held by the k tiles a
signal ranks highest:

| Signal | Spearman | AUC (worst 10%) | capture@1 | capture@2 | capture@4 |
|---|---|---|---|---|---|
| Coarse soft count (today) | 0.741 | **0.843** | 0.178 | 0.316 | 0.510 |
| Final soft count | 0.812 | 0.761 | 0.181 | 0.315 | 0.520 |
| Residual | 0.650 | 0.719 | 0.165 | 0.278 | 0.472 |
| Candidate instability | 0.675 | 0.759 | 0.166 | 0.287 | 0.488 |
| Local alpha variance | 0.591 | 0.584 | 0.126 | 0.233 | 0.399 |
| Colour-normalised residual | 0.644 | 0.739 | 0.165 | 0.276 | 0.476 |
| Soft + 0.5 × residual | 0.767 | 0.827 | 0.188 | 0.323 | 0.521 |
| Soft + 1 × instability | 0.765 | 0.825 | 0.183 | 0.318 | 0.522 |
| Soft + 1 × final soft | 0.800 | 0.823 | 0.189 | **0.326** | 0.523 |
| Ideal ranking (the true error) | 1 | 1 | 0.280 | 0.448 | 0.655 |

Tiles of 128 px agree (capture@2: today 0.169, best combination 0.179, ideal 0.278). Per band pixel, the
alpha's softness (AUC 0.810 for an error over 0.1) and its local variance (0.808) beat the residual (0.718),
the colour-normalised residual (0.661) and candidate instability (0.652).

No signal alone ranks regions as well as today's count. The combinations gain at most 0.01 of capture, far from
the ideal ranking's 0.45, and they lose AUC. A second pass steered by them would mostly revisit what the detail
pass already covered, so it was not built and the pipeline is unchanged. Closing the gap needs a signal that
knows something the matte does not: a second model's opinion, or labelled data at higher resolution.

To repeat: `matte_tool signals <pairs dir> <model> --band 16 --detail 12 --mask-cache <dir> --tile 256 --out <prefix> --jobs 8`.
