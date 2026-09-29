// Colour management in the app (docs/color-management.md): Edit > Color Settings (the working space and the
// policies), the monitor profile, View > Proof Setup and Proof Colors, the canvas's display transform, what happens
// to a file's embedded profile when it opens, and the profile an export embeds. The colour maths is the core's
// (compositor/colormgmt.h).
#pragma once
#include "compositor/colormgmt.h"
#include "compositor/imaget.h"
#include <QByteArray>
#include <QColor>
#include <QObject>
#include <QString>
#include <optional>

class QWidget;

namespace compositor { struct Document; }

namespace app::color {

/// Photoshop's "RGB" policy for a file's embedded profile.
enum class EmbeddedPolicy { Preserve, ConvertToWorking, Off };
const char* policyKey(EmbeddedPolicy policy);   // "preserve", "convert", "off"
std::optional<EmbeddedPolicy> policyFromKey(const QString& key);

struct Settings {
    compositor::WorkingSpace workingSpace = compositor::WorkingSpace::SRGB;
    /// Working CMYK: an ICC file's path, or empty for the bundled ISO Coated v2 300% (basICColor, FOGRA39).
    QString workingCmyk;
    EmbeddedPolicy policy = EmbeddedPolicy::Preserve;
    /// Photoshop's "Ask When Opening" for a missing profile and a profile other than the working space.
    bool askMissing = false, askMismatch = false;
    /// Preferences: the monitor profile, an ICC file chosen by hand; empty: the system's (X11's _ICC_PROFILE, Windows'
    /// display profile), when it reports one.
    QString monitorFile;
    bool useSystemMonitor = true;
    /// View > Proof Setup: "working-cmyk" (Photoshop's default proof, the Working CMYK), a working-space key ("srgb",
    /// ...) or an ICC file's path (RGB or CMYK); the intent and black point compensation for the proof; the gamut
    /// warning's colour.
    QString proofProfile = QStringLiteral("working-cmyk");
    compositor::RenderingIntent proofIntent = compositor::RenderingIntent::RelativeColorimetric;
    bool proofBlackPoint = true;
    QColor gamutColor{128, 128, 128};
    /// View > Proof Colors and Gamut Warning (for every window, for this run).
    bool proofColors = false, gamutWarning = false;
};

const Settings& settings();
/// Stores the settings (QSettings, except the two View toggles) and tells every canvas.
void setSettings(const Settings& settings);

/// Emits changed() whenever the settings, the monitor profile or the proof change.
class Notifier : public QObject {
    Q_OBJECT
signals:
    void changed();
};
Notifier* notifier();

/// The working space's profile.
const compositor::ColorProfile& workingProfile();
/// The Working CMYK profile: the chosen ICC file, or the bundled one (also when the file cannot be read).
compositor::ColorProfile workingCmykProfile();
/// How Color Settings names the Working CMYK: its description.
QString workingCmykLabel();
/// The monitor's profile (the Preferences file, else the system's), empty when none is known: then the canvas shows
/// document values as they are. Read once and kept until the settings change.
compositor::ColorProfile monitorProfile();
/// Where the monitor profile came from, for Preferences ("System: <name>", "<file>", "None").
QString monitorProfileSource();

/// The profiles a chooser takes: RGB (documents, the monitor), RGB or CMYK (a proof), CMYK (Working CMYK).
enum class ProfileKinds { RGB, RGBOrCMYK, CMYK };
/// An ICC file as a profile; nullopt with `error` when it is not a usable profile of `kinds`.
std::optional<compositor::ColorProfile> readProfileFile(const QString& path, QString* error = nullptr, ProfileKinds kinds = ProfileKinds::RGB);
/// A working-space key ("srgb", "adobe-rgb", "display-p3", "prophoto"), "none" (untagged), "working-cmyk" (when
/// `kinds` takes CMYK) or an ICC file's path.
std::optional<compositor::ColorProfile> profileForKey(const QString& key, QString* error = nullptr, ProfileKinds kinds = ProfileKinds::RGB);
/// How the menus name a document's profile: its description, or "Untagged RGB (sRGB)".
QString profileLabel(const compositor::ColorProfile& profile);

/// The canvas transform for `document`: document profile to monitor profile, through the proof when Proof Colors
/// is on. Null (nothing to do) when no monitor profile is known or the two are the same, and no proof is on: the
/// canvas then shows exactly what it showed before colour management.
compositor::ColorTransformPtr displayTransform(const compositor::Document& document);
/// A colour of the document (its values) as the screen should show it (swatches, previews).
QColor displayColor(const QColor& color, const compositor::ColorProfile& document);

/// What to do with a file's profile as it opens: the profile the document carries, and whether its pixels are
/// converted to it first (from `from`). Asks when Color Settings says so and `parent` is on screen.
struct OpenDecision {
    compositor::ColorProfile profile;
    bool convert = false;
    compositor::ColorProfile from;
};
OpenDecision decideOnOpen(const std::optional<compositor::ColorProfile>& embedded, QWidget* parent);
void applyToDocument(compositor::Document& document, const OpenDecision& decision);
compositor::AnyImage applyToImage(const compositor::AnyImage& image, const OpenDecision& decision);
/// An image's embedded profile as Qt read it (JPEG APP2, PNG iCCP, TIFF), or a PNG's iCCP; nullopt when untagged or
/// not a usable RGB profile.
std::optional<compositor::ColorProfile> embeddedProfile(const QByteArray& icc);
/// An image placed into a document (import, paste of a file): converted from its own profile to the document's.
compositor::AnyImage convertForDocument(const compositor::AnyImage& image, const std::optional<compositor::ColorProfile>& embedded, const compositor::ColorProfile& document);

/// Exports: whether the pixels go to sRGB for the web, and the profile embedded (empty: none, as an untagged sRGB file).
struct ExportPlan {
    bool convert = false;
    compositor::ColorProfile from;
    std::vector<uint8_t> icc;
    void apply(compositor::Image& image) const;
    void apply(compositor::Image16& image) const;
    QByteArray iccBytes() const { return QByteArray(reinterpret_cast<const char*>(icc.data()), qsizetype(icc.size())); }
};
ExportPlan exportPlan(const compositor::Document& document, bool convertToSrgb, bool embed = true);
/// Whether an export has anything to convert: the document is tagged with a profile other than sRGB.
bool hasNonSrgbProfile(const compositor::Document& document);
/// For the web formats: asks whether to convert to sRGB when the document has another profile; nullopt when
/// cancelled, the answer (or false without asking) otherwise.
std::optional<bool> askConvertToSrgb(QWidget* parent, const compositor::Document& document, bool defaultOn);

/// The composite for an export at 8 bits (a 16-bit document converted first, then dithered down) and at 16.
std::shared_ptr<compositor::Image> flatten8(const compositor::Document& document, const ExportPlan& plan);
std::shared_ptr<compositor::Image16> flatten16(const compositor::Document& document, const ExportPlan& plan);

/// A new document's profile: the working space's, or untagged (sRGB) while the working space is sRGB.
compositor::ColorProfile newDocumentProfile();

/// The Preferences group for the monitor profile.
QWidget* monitorPreferences(QWidget* parent = nullptr);

} // namespace app::color
