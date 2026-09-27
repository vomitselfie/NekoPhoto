// Display names for what the core and the automation API name in English: blend modes, adjustment and filter
// kinds, channels, and undo steps. The English names stay the API (the manifest, JSON-RPC, recorded actions);
// these give the interface's words for them, translated in the "Names" and "History" contexts.
#pragma once
#include "compositor/document.h"
#include "compositor/filters.h"
#include <QCoreApplication>
#include <QString>
#include <string>

namespace app::names {

/// Any English name the core hands out (blendModeName, adjustmentKindName, filterKindName, levelsChannelName,
/// colorRangeName, samplingName) in the interface language; unknown names come back unchanged.
QString core(const char* english);
inline QString core(const std::string& english) { return core(english.c_str()); }
QString blendMode(compositor::BlendMode mode);
QString adjustmentKind(compositor::AdjustmentKind kind);
QString filterKind(compositor::FilterKind kind);
QString levelsChannel(int channel);
QString colorRange(int range);

/// An undo step's name (stored in English, as history.list reports it) for the Edit menu.
QString history(const QString& english);
inline QString history(const std::string& english) { return history(QString::fromStdString(english)); }

/// A new layer's base name ("Layer", "Folder", "Text", ...) in the interface language: the name a new layer gets
/// is document content, as in Photoshop, so it follows the language (automation requests get English).
inline QString layerBase(const char* english) { return QCoreApplication::translate("Names", english); }

} // namespace app::names
