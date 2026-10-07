// What the Filter menu's grid filters (compositor/filters.h, applyGridFilter) share between their dialog and
// pixels.filter: their settings as automation's keys, and the context a run needs from the session.
#pragma once
#include "EditorSession.h"
#include "compositor/filters.h"
#include <QJsonObject>
#include <QStringList>

namespace app {

/// `params`' keys for `kind` read over `settings` (keys not given keep their values). False, with `error`, for a value
/// that is not one of a choice's names.
bool readGridFilterSettings(compositor::FilterKind kind, const QJsonObject& params, compositor::FilterSettings& settings, QString* error);
/// The settings as pixels.filter takes them (the step a dialog runs and an action records), with `seed` where the
/// filter has a pattern.
QJsonObject gridFilterStep(compositor::FilterKind kind, const compositor::FilterSettings& settings, uint32_t seed);
/// Whether `kind` draws a pattern from its seed (Wave, Clouds, Difference Clouds).
bool gridFilterSeeded(compositor::FilterKind kind);
/// The run's context for `source` placed at `transform` in the session's document: its mode, the selection's bounds
/// on the grid (`coverage`, none: the whole grid), the grid's place in the document, the colours and `seed`.
compositor::GridFilterContext gridFilterContext(const EditorSession* session, const compositor::LayerTransform& transform,
                                                const compositor::AnyGray& coverage, uint32_t seed);
/// The names a choice key takes for `kind` (empty when it has none), for the dialogs and the error messages.
QStringList gridFilterChoiceNames(compositor::FilterKind kind, const char* key);

} // namespace app
