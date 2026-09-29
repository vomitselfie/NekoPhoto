#include "ColorManagement.h"
#include "Platform.h"
#include "Style.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/render.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
#include <mutex>

using namespace compositor;

namespace app::color {

const char* policyKey(EmbeddedPolicy policy) {
    switch (policy) {
    case EmbeddedPolicy::Preserve: return "preserve";
    case EmbeddedPolicy::ConvertToWorking: return "convert";
    case EmbeddedPolicy::Off: return "off";
    }
    return "preserve";
}

std::optional<EmbeddedPolicy> policyFromKey(const QString& key) {
    for (EmbeddedPolicy p : {EmbeddedPolicy::Preserve, EmbeddedPolicy::ConvertToWorking, EmbeddedPolicy::Off})
        if (key == QLatin1String(policyKey(p))) return p;
    return std::nullopt;
}

namespace {

Settings loadSettings() {
    QSettings q;
    Settings s;
    q.beginGroup(QStringLiteral("color"));
    if (auto w = workingSpaceFromKey(q.value("workingSpace", "srgb").toString().toStdString())) s.workingSpace = *w;
    s.workingCmyk = q.value("workingCmyk").toString();
    if (auto intent = renderingIntentFromKey(q.value("conversionIntent", "relative").toString().toStdString())) s.conversionIntent = *intent;
    s.conversionBlackPoint = q.value("conversionBlackPoint", true).toBool();
    if (auto p = policyFromKey(q.value("policy", "preserve").toString())) s.policy = *p;
    s.askMissing = q.value("askMissing", false).toBool();
    s.askMismatch = q.value("askMismatch", false).toBool();
    s.monitorFile = q.value("monitorFile").toString();
    s.useSystemMonitor = q.value("useSystemMonitor", true).toBool();
    s.proofProfile = q.value("proofProfile", "working-cmyk").toString();
    if (auto i = renderingIntentFromKey(q.value("proofIntent", "relative").toString().toStdString())) s.proofIntent = *i;
    s.proofBlackPoint = q.value("proofBlackPoint", true).toBool();
    const QColor gamut(q.value("gamutColor", "#808080").toString());
    if (gamut.isValid()) s.gamutColor = gamut;
    q.endGroup();
    return s;
}

Settings& current() {
    static Settings s = loadSettings();
    return s;
}

std::mutex monitorMutex;
bool monitorRead = false;
ColorProfile monitorCache;
QString monitorSource;

} // namespace

const Settings& settings() { return current(); }

void setSettings(const Settings& s) {
    current() = s;
    QSettings q;
    q.beginGroup(QStringLiteral("color"));
    q.setValue("workingSpace", QString::fromLatin1(workingSpaceKey(s.workingSpace)));
    q.setValue("workingCmyk", s.workingCmyk);
    q.setValue("conversionIntent", QString::fromLatin1(renderingIntentKey(s.conversionIntent)));
    q.setValue("conversionBlackPoint", s.conversionBlackPoint);
    q.setValue("policy", QString::fromLatin1(policyKey(s.policy)));
    q.setValue("askMissing", s.askMissing);
    q.setValue("askMismatch", s.askMismatch);
    q.setValue("monitorFile", s.monitorFile);
    q.setValue("useSystemMonitor", s.useSystemMonitor);
    q.setValue("proofProfile", s.proofProfile);
    q.setValue("proofIntent", QString::fromLatin1(renderingIntentKey(s.proofIntent)));
    q.setValue("proofBlackPoint", s.proofBlackPoint);
    q.setValue("gamutColor", s.gamutColor.name());
    q.endGroup();
    {
        std::lock_guard<std::mutex> lock(monitorMutex);
        monitorRead = false;
    }
    emit notifier()->changed();
}

Notifier* notifier() {
    static Notifier* n = new Notifier;
    return n;
}

const ColorProfile& workingProfile() { return builtinProfile(settings().workingSpace); }

ColorProfile workingCmykProfile() {
    // Read once per file chosen; the bundled profile when none is, or the file is unusable.
    static std::mutex mutex;
    static QString readPath;
    static ColorProfile read;
    const QString path = settings().workingCmyk;
    if (path.isEmpty()) return defaultCmykProfile();
    std::lock_guard<std::mutex> lock(mutex);
    if (path != readPath) {
        readPath = path;
        auto p = readProfileFile(path, nullptr, ProfileKinds::CMYK);
        read = p ? *p : defaultCmykProfile();
    }
    return read;
}

ConvertOptions conversionOptions() {
    ConvertOptions options;
    options.intent = settings().conversionIntent;
    options.blackPointCompensation = settings().conversionBlackPoint;
    return options;
}

QString workingCmykLabel() { return QString::fromStdString(workingCmykProfile().description); }

std::optional<ColorProfile> readProfileFile(const QString& path, QString* error, ProfileKinds kinds) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = QObject::tr("Couldn’t read %1.").arg(QFileInfo(path).fileName()); return std::nullopt; }
    if (file.size() > (qint64(64) << 20)) { if (error) *error = QObject::tr("%1 is too large to be a colour profile.").arg(QFileInfo(path).fileName()); return std::nullopt; }
    const QByteArray bytes = file.readAll();
    auto profile = profileFromIcc(reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size()));
    if (!profile) { if (error) *error = QObject::tr("%1 is not a colour profile.").arg(QFileInfo(path).fileName()); return std::nullopt; }
    const bool rgb = profile->model == ColorModel::RGB, cmyk = profile->model == ColorModel::CMYK;
    if (kinds == ProfileKinds::RGB && !rgb) { if (error) *error = QObject::tr("%1 is not an RGB profile.").arg(QFileInfo(path).fileName()); return std::nullopt; }
    if (kinds == ProfileKinds::CMYK && !cmyk) { if (error) *error = QObject::tr("%1 is not a CMYK profile.").arg(QFileInfo(path).fileName()); return std::nullopt; }
    if (kinds == ProfileKinds::RGBOrCMYK && !rgb && !cmyk) { if (error) *error = QObject::tr("%1 is not an RGB or CMYK profile.").arg(QFileInfo(path).fileName()); return std::nullopt; }
    return profile;
}

std::optional<ColorProfile> profileForKey(const QString& key, QString* error, ProfileKinds kinds) {
    if (key == QLatin1String("none")) return ColorProfile{};
    if (key == QLatin1String("working-cmyk")) {
        if (kinds != ProfileKinds::RGB) return workingCmykProfile();
        if (error) *error = QObject::tr("The Working CMYK is a CMYK profile; an RGB document takes RGB profiles.");
        return std::nullopt;
    }
    if (auto space = workingSpaceFromKey(key.toStdString())) {
        if (kinds != ProfileKinds::CMYK) return builtinProfile(*space);
        if (error) *error = QObject::tr("%1 is an RGB profile; a CMYK profile is needed here.").arg(QString::fromLatin1(workingSpaceName(*space)));
        return std::nullopt;
    }
    if (key.isEmpty()) { if (error) *error = QObject::tr("No profile given."); return std::nullopt; }
    return readProfileFile(key, error, kinds);
}

QString profileLabel(const ColorProfile& profile) {
    if (profile.empty()) return QObject::tr("Untagged RGB (treated as sRGB)");
    return QString::fromStdString(profile.description);
}

/// Headless and offscreen runs have no screen to match: the system's profile is not looked up there (Windows always
/// answers with its default), so scripts and screenshots see the document's values on every platform. A profile file
/// chosen in Preferences still applies.
static bool screenless() {
    const QString platform = QGuiApplication::platformName();
    return platform == QLatin1String("offscreen") || platform == QLatin1String("minimal");
}

ColorProfile monitorProfile() {
    std::lock_guard<std::mutex> lock(monitorMutex);
    if (monitorRead) return monitorCache;
    monitorRead = true;
    monitorCache = {};
    monitorSource = QObject::tr("None: colours are shown as the document's values");
    const Settings& s = settings();
    if (!s.monitorFile.isEmpty()) {
        if (auto p = readProfileFile(s.monitorFile)) { monitorCache = *p; monitorSource = QFileInfo(s.monitorFile).fileName(); }
    } else if (s.useSystemMonitor && !screenless()) {
        const QByteArray bytes = platform::systemMonitorProfile();
        if (auto p = profileFromIcc(reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size())); p && p->model == ColorModel::RGB) {
            monitorCache = *p;
            monitorSource = QObject::tr("System: %1").arg(QString::fromStdString(p->description));
        }
    }
    return monitorCache;
}

QString monitorProfileSource() {
    monitorProfile();
    std::lock_guard<std::mutex> lock(monitorMutex);
    return monitorSource;
}

ColorTransformPtr displayTransform(const Document& document) {
    const Settings& s = settings();
    const PixelFormat input = document.sampleType == SampleType::U16 ? PixelFormat::RGBA16 : PixelFormat::RGBA8;
    if (s.proofColors || s.gamutWarning) {
        ProofSettings proof;
        if (auto p = profileForKey(s.proofProfile, nullptr, ProfileKinds::RGBOrCMYK)) proof.profile = *p;
        proof.intent = s.proofIntent;
        proof.blackPointCompensation = s.proofBlackPoint;
        proof.gamutWarning = s.gamutWarning;
        proof.warning[0] = uint8_t(s.gamutColor.red()); proof.warning[1] = uint8_t(s.gamutColor.green()); proof.warning[2] = uint8_t(s.gamutColor.blue());
        if (auto t = proofTransform(document.profile, monitorProfile(), proof, input, PixelFormat::RGBA8)) return t;
    }
    const ColorProfile monitor = monitorProfile();
    if (document.colorMode != ColorMode::RGB) {
        // A CMYK or Lab document always goes through a transform: to the monitor, or to sRGB without one.
        return transformBetween(document.profile, monitor.empty() ? srgbProfile() : monitor, {RenderingIntent::RelativeColorimetric, true},
                                pixelFormatFor(document.sampleType, document.colorMode), PixelFormat::RGBA8);
    }
    if (monitor.empty() || equivalentProfiles(document.profile, monitor)) return nullptr;
    return transformBetween(document.profile, monitor, {RenderingIntent::RelativeColorimetric, true}, input, PixelFormat::RGBA8);
}

QColor displayColor(const QColor& color, const ColorProfile& document) {
    const ColorProfile monitor = monitorProfile();
    if (monitor.empty() || equivalentProfiles(document, monitor)) return color;
    double rgb[3] = {color.redF(), color.greenF(), color.blueF()};
    convertColor(document, monitor, rgb);
    return QColor::fromRgbF(float(rgb[0]), float(rgb[1]), float(rgb[2]), color.alphaF());
}

// ---- Opening files -------------------------------------------------------------------------------------------------

OpenDecision decideOnOpen(const std::optional<ColorProfile>& embedded, QWidget* parent) {
    const Settings& s = settings();
    const bool canAsk = parent && parent->isVisible();
    OpenDecision d;
    if (!embedded || embedded->empty()) {
        // Untagged: treated as sRGB, and left untagged (unless the person assigns the working space when asked).
        if (canAsk && s.askMissing && !equivalentProfiles({}, workingProfile())) {
            QMessageBox box(QMessageBox::Question, QObject::tr("Missing Profile"),
                            QObject::tr("The document does not have an embedded RGB profile."), QMessageBox::NoButton, parent);
            QPushButton* leave = box.addButton(QObject::tr("Leave as is (treat as sRGB)"), QMessageBox::AcceptRole);
            QPushButton* assign = box.addButton(QObject::tr("Assign working RGB: %1").arg(QString::fromLatin1(workingSpaceName(s.workingSpace))), QMessageBox::ActionRole);
            box.setDefaultButton(leave);
            box.exec();
            if (box.clickedButton() == static_cast<QAbstractButton*>(assign)) d.profile = workingProfile();
        }
        return d;
    }
    const bool matches = equivalentProfiles(*embedded, workingProfile());
    EmbeddedPolicy policy = s.policy;
    if (canAsk && s.askMismatch && !matches) {
        QMessageBox box(QMessageBox::Question, QObject::tr("Embedded Profile Mismatch"),
                        QObject::tr("The document has an embedded colour profile that does not match the working space.\n\nEmbedded: %1\nWorking: %2")
                            .arg(QString::fromStdString(embedded->description), QString::fromLatin1(workingSpaceName(s.workingSpace))),
                        QMessageBox::NoButton, parent);
        QPushButton* keep = box.addButton(QObject::tr("Use the embedded profile"), QMessageBox::AcceptRole);
        QPushButton* convert = box.addButton(QObject::tr("Convert to the working space"), QMessageBox::ActionRole);
        QPushButton* discard = box.addButton(QObject::tr("Discard the embedded profile"), QMessageBox::ActionRole);
        box.setDefaultButton(keep);
        box.exec();
        policy = box.clickedButton() == static_cast<QAbstractButton*>(convert) ? EmbeddedPolicy::ConvertToWorking
               : box.clickedButton() == static_cast<QAbstractButton*>(discard) ? EmbeddedPolicy::Off : EmbeddedPolicy::Preserve;
    }
    switch (policy) {
    case EmbeddedPolicy::Preserve: d.profile = *embedded; break;
    case EmbeddedPolicy::ConvertToWorking:
        if (matches) { d.profile = *embedded; break; }
        d.convert = true;
        d.from = *embedded;
        d.profile = workingProfile();
        break;
    case EmbeddedPolicy::Off:
        // Photoshop's Off keeps a profile that is the working space's and drops any other.
        if (matches) d.profile = *embedded;
        break;
    }
    return d;
}

void applyToDocument(Document& document, const OpenDecision& decision) {
    if (decision.convert) {
        document.profile = decision.from;
        if (convertDocumentProfile(document, decision.profile, {})) return;
    }
    document.profile = decision.profile;
}

AnyImage applyToImage(const AnyImage& image, const OpenDecision& decision) {
    if (!decision.convert) return image;
    return convertForDocument(image, decision.from, decision.profile);
}

std::optional<ColorProfile> embeddedProfile(const QByteArray& icc) {
    if (icc.isEmpty()) return std::nullopt;
    auto profile = profileFromIcc(reinterpret_cast<const uint8_t*>(icc.constData()), size_t(icc.size()));
    if (!profile || profile->model != ColorModel::RGB) return std::nullopt;
    return profile;
}

AnyImage convertForDocument(const AnyImage& image, const std::optional<ColorProfile>& embedded, const ColorProfile& document) {
    const ColorProfile from = embedded.value_or(ColorProfile{});
    if (!image || equivalentProfiles(from, document)) return image;
    if (auto p = image.u8()) { auto copy = std::make_shared<Image>(*p); if (convertImage(*copy, from, document)) return ImagePtr(copy); }
    if (auto p = image.u16()) { auto copy = std::make_shared<Image16>(*p); if (convertImage(*copy, from, document)) return Image16Ptr(copy); }
    return image;
}

// ---- Exports -------------------------------------------------------------------------------------------------------

bool hasNonSrgbProfile(const Document& document) { return !document.profile.empty() && !equivalentProfiles(document.profile, {}); }

ExportPlan exportPlan(const Document& document, bool convertToSrgb, bool embed) {
    ExportPlan plan;
    if (convertToSrgb && hasNonSrgbProfile(document)) { plan.convert = true; plan.from = document.profile; return plan; }
    if (embed) plan.icc = document.profile.icc;
    return plan;
}

void ExportPlan::apply(Image& image) const { if (convert) convertImage(image, from, {}); }
void ExportPlan::apply(Image16& image) const { if (convert) convertImage(image, from, {}); }

std::shared_ptr<Image> flatten8(const Document& document, const ExportPlan& plan) {
    if (document.sampleType == SampleType::U16) {
        auto deep = renderFlattened16(document);
        plan.apply(*deep);
        return ditherToEightBit(*deep);
    }
    auto flat = renderFlattened(document);
    plan.apply(*flat);
    return flat;
}

std::shared_ptr<Image16> flatten16(const Document& document, const ExportPlan& plan) {
    auto deep = renderFlattened16(document);
    plan.apply(*deep);
    return deep;
}

ColorProfile newDocumentProfile() { return equivalentProfiles({}, workingProfile()) ? ColorProfile{} : workingProfile(); }

std::optional<bool> askConvertToSrgb(QWidget* parent, const Document& document, bool defaultOn) {
    if (!hasNonSrgbProfile(document)) return false;
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Export Colour"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* text = new QLabel(QObject::tr("The document’s profile is %1. Browsers and most viewers assume sRGB.").arg(QString::fromStdString(document.profile.description)));
    text->setWordWrap(true);
    layout->addWidget(text);
    auto* convert = new QCheckBox(QObject::tr("Convert to sRGB"));
    convert->setChecked(defaultOn);
    layout->addWidget(convert);
    auto* hint = new QLabel(QObject::tr("Off: the pixels are written as they are, with the document’s profile embedded where the format allows."));
    hint->setWordWrap(true);
    hint->setStyleSheet(hintStyle());
    layout->addWidget(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return convert->isChecked();
}

// ---- Preferences ---------------------------------------------------------------------------------------------------

QWidget* monitorPreferences(QWidget* parent) {
    auto* group = new QGroupBox(QObject::tr("Monitor colour profile"), parent);
    auto* v = new QVBoxLayout(group);
    auto* system = new QCheckBox(QObject::tr("Use the system’s monitor profile when it reports one (X11, Windows)"));
    system->setChecked(settings().useSystemMonitor);
    v->addWidget(system);
    auto* row = new QHBoxLayout;
    auto* source = new QLabel;
    source->setWordWrap(true);
    row->addWidget(source, 1);
    auto* choose = new QPushButton(QObject::tr("Choose Profile…"));
    auto* clear = new QPushButton(QObject::tr("Clear"));
    row->addWidget(choose);
    row->addWidget(clear);
    v->addLayout(row);
    auto* hint = new QLabel(QObject::tr("The canvas converts the document’s colours to this profile. On Wayland, choose your monitor’s ICC file "
                                        "here. With none, colours are shown as the document’s values, as before."));
    hint->setWordWrap(true);
    hint->setStyleSheet(hintStyle());
    v->addWidget(hint);
    auto sync = [source, clear] {
        source->setText(QObject::tr("Current: %1").arg(monitorProfileSource()));
        clear->setEnabled(!settings().monitorFile.isEmpty());
    };
    sync();
    QObject::connect(system, &QCheckBox::toggled, group, [sync](bool on) { Settings s = settings(); s.useSystemMonitor = on; setSettings(s); sync(); });
    QObject::connect(choose, &QPushButton::clicked, group, [group, sync] {
        const QString path = QFileDialog::getOpenFileName(group, QObject::tr("Monitor Profile"), QString(), QObject::tr("ICC profiles (*.icc *.icm)"));
        if (path.isEmpty()) return;
        QString error;
        if (!readProfileFile(path, &error)) { QMessageBox::warning(group, QObject::tr("Monitor Profile"), error); return; }
        Settings s = settings();
        s.monitorFile = path;
        setSettings(s);
        sync();
    });
    QObject::connect(clear, &QPushButton::clicked, group, [sync] { Settings s = settings(); s.monitorFile.clear(); setSettings(s); sync(); });
    return group;
}

} // namespace app::color
