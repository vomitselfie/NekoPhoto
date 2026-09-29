#include "Names.h"
#include "compositor/adjustments.h"
#include "compositor/transform.h"

namespace app::names {

namespace {

// Every English name the core gives out, so lupdate lists them in the "Names" context. The core stays the only
// source of the English (the API); a name missing here simply shows in English.
[[maybe_unused]] const char* const kCoreNames[] = {
    // blendModeName
    QT_TRANSLATE_NOOP("Names", "Normal"), QT_TRANSLATE_NOOP("Names", "Dissolve"), QT_TRANSLATE_NOOP("Names", "Darken"),
    QT_TRANSLATE_NOOP("Names", "Multiply"), QT_TRANSLATE_NOOP("Names", "Color Burn"), QT_TRANSLATE_NOOP("Names", "Linear Burn"),
    QT_TRANSLATE_NOOP("Names", "Darker Color"), QT_TRANSLATE_NOOP("Names", "Lighten"), QT_TRANSLATE_NOOP("Names", "Screen"),
    QT_TRANSLATE_NOOP("Names", "Color Dodge"), QT_TRANSLATE_NOOP("Names", "Linear Dodge (Add)"), QT_TRANSLATE_NOOP("Names", "Lighter Color"),
    QT_TRANSLATE_NOOP("Names", "Overlay"), QT_TRANSLATE_NOOP("Names", "Soft Light"), QT_TRANSLATE_NOOP("Names", "Hard Light"),
    QT_TRANSLATE_NOOP("Names", "Vivid Light"), QT_TRANSLATE_NOOP("Names", "Linear Light"), QT_TRANSLATE_NOOP("Names", "Pin Light"),
    QT_TRANSLATE_NOOP("Names", "Hard Mix"), QT_TRANSLATE_NOOP("Names", "Difference"), QT_TRANSLATE_NOOP("Names", "Exclusion"),
    QT_TRANSLATE_NOOP("Names", "Subtract"), QT_TRANSLATE_NOOP("Names", "Divide"), QT_TRANSLATE_NOOP("Names", "Hue"),
    QT_TRANSLATE_NOOP("Names", "Saturation"), QT_TRANSLATE_NOOP("Names", "Color"), QT_TRANSLATE_NOOP("Names", "Luminosity"),
    QT_TRANSLATE_NOOP("Names", "Pass Through"),
    // adjustmentKindName
    QT_TRANSLATE_NOOP("Names", "Hue/Saturation"), QT_TRANSLATE_NOOP("Names", "Levels"), QT_TRANSLATE_NOOP("Names", "Curves"),
    QT_TRANSLATE_NOOP("Names", "Exposure"), QT_TRANSLATE_NOOP("Names", "Gradient Map"), QT_TRANSLATE_NOOP("Names", "Grain"),
    QT_TRANSLATE_NOOP("Names", "Invert"), QT_TRANSLATE_NOOP("Names", "Brightness/Contrast"), QT_TRANSLATE_NOOP("Names", "Posterize"),
    QT_TRANSLATE_NOOP("Names", "Threshold"), QT_TRANSLATE_NOOP("Names", "Black & White"), QT_TRANSLATE_NOOP("Names", "Color Balance"),
    QT_TRANSLATE_NOOP("Names", "Vibrance"), QT_TRANSLATE_NOOP("Names", "Photo Filter"), QT_TRANSLATE_NOOP("Names", "Channel Mixer"),
    QT_TRANSLATE_NOOP("Names", "Selective Color"), QT_TRANSLATE_NOOP("Names", "Color Lookup"),
    // filterKindName
    QT_TRANSLATE_NOOP("Names", "Gaussian Blur"), QT_TRANSLATE_NOOP("Names", "Motion Blur"), QT_TRANSLATE_NOOP("Names", "Add Noise"),
    QT_TRANSLATE_NOOP("Names", "Lens Correction"),
    // levelsChannelName, colorRangeName, samplingName
    QT_TRANSLATE_NOOP("Names", "RGB"), QT_TRANSLATE_NOOP("Names", "Red"), QT_TRANSLATE_NOOP("Names", "Green"), QT_TRANSLATE_NOOP("Names", "Blue"),
    QT_TRANSLATE_NOOP("Names", "CMYK"), QT_TRANSLATE_NOOP("Names", "Cyan"), QT_TRANSLATE_NOOP("Names", "Magenta"), QT_TRANSLATE_NOOP("Names", "Yellow"),
    QT_TRANSLATE_NOOP("Names", "Black"), QT_TRANSLATE_NOOP("Names", "Lightness"), QT_TRANSLATE_NOOP("Names", "a"), QT_TRANSLATE_NOOP("Names", "b"),
    QT_TRANSLATE_NOOP("Names", "Master"), QT_TRANSLATE_NOOP("Names", "Reds"), QT_TRANSLATE_NOOP("Names", "Yellows"),
    QT_TRANSLATE_NOOP("Names", "Greens"), QT_TRANSLATE_NOOP("Names", "Cyans"), QT_TRANSLATE_NOOP("Names", "Blues"),
    QT_TRANSLATE_NOOP("Names", "Magentas"), QT_TRANSLATE_NOOP("Names", "Oranges"), QT_TRANSLATE_NOOP("Names", "Aquas"),
    QT_TRANSLATE_NOOP("Names", "Purples"),
    QT_TRANSLATE_NOOP("Names", "Nearest"), QT_TRANSLATE_NOOP("Names", "Smooth"), QT_TRANSLATE_NOOP("Names", "High quality"),
    // New layers' base names (layerBase)
    QT_TRANSLATE_NOOP("Names", "Layer"), QT_TRANSLATE_NOOP("Names", "Folder"), QT_TRANSLATE_NOOP("Names", "Text"),
    QT_TRANSLATE_NOOP("Names", "Artboard"),
};

/// Undo step names made from a pattern: the pattern's translation with its argument translated too.
[[maybe_unused]] const char* const kHistoryPatterns[] = {
    QT_TRANSLATE_NOOP("History", "New %1 Adjustment"),
};

} // namespace

QString core(const char* english) { return QCoreApplication::translate("Names", english); }
QString blendMode(compositor::BlendMode mode) { return core(compositor::blendModeName(mode)); }
QString adjustmentKind(compositor::AdjustmentKind kind) { return core(compositor::adjustmentKindName(kind)); }
QString filterKind(compositor::FilterKind kind) { return core(compositor::filterKindName(kind)); }
QString levelsChannel(int channel) { return core(compositor::levelsChannelName(channel)); }
QString levelsChannel(int channel, compositor::ColorMode mode) { return core(compositor::levelsChannelName(channel, mode)); }
QString colorRange(int range) { return core(compositor::colorRangeName(range)); }

QString history(const QString& english) {
    const QByteArray key = english.toUtf8();
    QString t = QCoreApplication::translate("History", key.constData());
    if (t != english) return t;
    t = QCoreApplication::translate("Names", key.constData());
    if (t != english) return t;
    for (const char* pattern : kHistoryPatterns) {
        const QString p = QString::fromUtf8(pattern);
        const int at = p.indexOf(QLatin1String("%1"));
        const QString before = p.left(at), after = p.mid(at + 2);
        if (english.size() > before.size() + after.size() && english.startsWith(before) && english.endsWith(after)) {
            const QString argument = english.mid(before.size(), english.size() - before.size() - after.size());
            return QCoreApplication::translate("History", pattern).arg(history(argument));
        }
    }
    return english;
}

} // namespace app::names
