// Display names for what the core and the automation API name in English: blend modes, adjustment and filter
// kinds, channels, and undo steps. The English names stay the API (the manifest, JSON-RPC, recorded actions);
// these give the interface's words for them, translated in the "Names" and "History" contexts.
#pragma once
#include "nekophoto/document.h"
#include "nekophoto/filters.h"
#include <QCoreApplication>
#include <QString>
#include <string>

namespace app::names {

/// Any English name the core hands out (blendModeName, adjustmentKindName, filterKindName, levelsChannelName,
/// colorRangeName, samplingName) in the interface language; unknown names come back unchanged.
QString core(const char* english);
inline QString core(const std::string& english) { return core(english.c_str()); }
QString blendMode(nekophoto::BlendMode mode);
QString adjustmentKind(nekophoto::AdjustmentKind kind);
QString filterKind(nekophoto::FilterKind kind);
QString levelsChannel(int channel);
/// The Levels and Curves channel in a document of `mode` ("Cyan", "Lightness"); empty for a slot the mode has not.
QString levelsChannel(int channel, nekophoto::ColorMode mode);
QString colorRange(int range);

/// An undo step's name (stored in English, as history.list reports it) for the Edit menu.
QString history(const QString& english);
inline QString history(const std::string& english) { return history(QString::fromStdString(english)); }

/// A new layer's base name ("Layer", "Folder", "Text", ...) in the interface language: the name a new layer gets
/// is document content, as in Photoshop, so it follows the language (automation requests get English).
inline QString layerBase(const char* english) { return QCoreApplication::translate("Names", english); }

} // namespace app::names
