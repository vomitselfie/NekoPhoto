#include "ColorDialogs.h"
#include "ColorManagement.h"
#include "Style.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

using namespace compositor;

namespace app::color {

namespace {

constexpr WorkingSpace spaces[] = {WorkingSpace::SRGB, WorkingSpace::AdobeRGB, WorkingSpace::DisplayP3, WorkingSpace::ProPhoto};

QString spaceLabel(WorkingSpace s) { return QString::fromLatin1(workingSpaceName(s)); }

/// A profile chooser: the Working CMYK (when `kinds` takes CMYK), the four working spaces, `extra` (the document's own
/// profile when it is none of them), and "Load…", which reads an ICC file and adds it. Each item's data is a key for
/// profileForKey, or the profile itself.
class ProfileCombo : public QComboBox {
public:
    explicit ProfileCombo(QWidget* parent, ProfileKinds kinds = ProfileKinds::RGB) : QComboBox(parent), kinds_(kinds) {
        if (kinds != ProfileKinds::RGB) addItem(QObject::tr("Working CMYK: %1").arg(workingCmykLabel()), QStringLiteral("working-cmyk"));
        if (kinds != ProfileKinds::CMYK)
            for (WorkingSpace s : spaces) addItem(spaceLabel(s), QString::fromLatin1(workingSpaceKey(s)));
        insertSeparator(count());
        addItem(QObject::tr("Load…"), QStringLiteral("__load__"));
        connect(this, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
            if (itemData(index).toString() != QLatin1String("__load__")) { last_ = index; return; }
            const QString path = QFileDialog::getOpenFileName(this, QObject::tr("Load Profile"), QString(), QObject::tr("ICC profiles (*.icc *.icm)"));
            QString error;
            auto profile = path.isEmpty() ? std::nullopt : readProfileFile(path, &error, kinds_);
            if (!profile) {
                if (!path.isEmpty()) QMessageBox::warning(this, QObject::tr("Load Profile"), error);
                setCurrentIndex(last_);
                return;
            }
            insertItem(0, QString::fromStdString(profile->description), path);
            setCurrentIndex(0);
            last_ = 0;
        });
    }
    /// Adds `profile` (a document's) when it is not one of the working spaces, and selects it or its match.
    void select(const ColorProfile& profile) {
        if (auto s = matchingWorkingSpace(profile); s && (profile.empty() || profile == builtinProfile(*s))) {
            setCurrentIndex(findData(QString::fromLatin1(workingSpaceKey(*s))));
        } else if (!profile.empty()) {
            own_ = profile;
            insertItem(0, QString::fromStdString(profile.description), QStringLiteral("__own__"));
            setCurrentIndex(0);
        }
        last_ = currentIndex();
    }
    void selectKey(const QString& key) {
        int index = findData(key);
        if (index < 0 && !key.isEmpty()) {
            if (auto p = readProfileFile(key, nullptr, kinds_)) { insertItem(0, QString::fromStdString(p->description), key); index = 0; }
        }
        setCurrentIndex(std::max(0, index));
        last_ = currentIndex();
    }
    QString key() const { return currentData().toString(); }
    std::optional<ColorProfile> profile() const {
        if (key() == QLatin1String("__own__")) return own_;
        return profileForKey(key(), nullptr, kinds_);
    }

private:
    ProfileKinds kinds_ = ProfileKinds::RGB;
    int last_ = 0;
    ColorProfile own_;
};

QComboBox* intentCombo(QWidget* parent, RenderingIntent current) {
    auto* combo = new QComboBox(parent);
    combo->addItem(QObject::tr("Perceptual"), int(RenderingIntent::Perceptual));
    combo->addItem(QObject::tr("Relative Colorimetric"), int(RenderingIntent::RelativeColorimetric));
    combo->setCurrentIndex(current == RenderingIntent::Perceptual ? 0 : 1);
    return combo;
}

QDialogButtonBox* okCancel(QDialog* dialog) {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

QLabel* hint(const QString& text) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet(hintStyle());
    return l;
}

} // namespace

bool showColorSettings(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Color Settings"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* working = new QComboBox;
    for (WorkingSpace s : spaces) working->addItem(spaceLabel(s), int(s));
    working->setCurrentIndex(working->findData(int(settings().workingSpace)));
    form->addRow(QObject::tr("Working space (RGB):"), working);
    // Working CMYK: the bundled ISO Coated v2 300% (FOGRA39), or any CMYK ICC file.
    auto* cmyk = new QComboBox;
    cmyk->addItem(QString::fromStdString(defaultCmykProfile().description), QString());
    if (!settings().workingCmyk.isEmpty()) {
        if (auto p = readProfileFile(settings().workingCmyk, nullptr, ProfileKinds::CMYK)) cmyk->addItem(QString::fromStdString(p->description), settings().workingCmyk);
    }
    cmyk->insertSeparator(cmyk->count());
    cmyk->addItem(QObject::tr("Load…"), QStringLiteral("__load__"));
    cmyk->setCurrentIndex(std::max(0, cmyk->findData(settings().workingCmyk)));
    int lastCmyk = cmyk->currentIndex();
    QObject::connect(cmyk, QOverload<int>::of(&QComboBox::activated), &dialog, [cmyk, &dialog, &lastCmyk](int index) {
        if (cmyk->itemData(index).toString() != QLatin1String("__load__")) { lastCmyk = index; return; }
        const QString path = QFileDialog::getOpenFileName(&dialog, QObject::tr("Load Profile"), QString(), QObject::tr("ICC profiles (*.icc *.icm)"));
        QString error;
        auto profile = path.isEmpty() ? std::nullopt : readProfileFile(path, &error, ProfileKinds::CMYK);
        if (!profile) {
            if (!path.isEmpty()) QMessageBox::warning(&dialog, QObject::tr("Load Profile"), error);
            cmyk->setCurrentIndex(lastCmyk);
            return;
        }
        cmyk->insertItem(1, QString::fromStdString(profile->description), path);
        cmyk->setCurrentIndex(1);
        lastCmyk = 1;
    });
    form->addRow(QObject::tr("Working CMYK:"), cmyk);
    auto* policy = new QComboBox;
    policy->addItem(QObject::tr("Preserve Embedded Profiles"), int(EmbeddedPolicy::Preserve));
    policy->addItem(QObject::tr("Convert to Working RGB"), int(EmbeddedPolicy::ConvertToWorking));
    policy->addItem(QObject::tr("Off"), int(EmbeddedPolicy::Off));
    policy->setCurrentIndex(policy->findData(int(settings().policy)));
    form->addRow(QObject::tr("Color management policy (RGB):"), policy);
    // Conversion Options: what Image > Mode uses between RGB, CMYK and Lab.
    auto* intent = new QComboBox;
    intent->addItem(QObject::tr("Perceptual"), int(RenderingIntent::Perceptual));
    intent->addItem(QObject::tr("Relative Colorimetric"), int(RenderingIntent::RelativeColorimetric));
    intent->addItem(QObject::tr("Saturation"), int(RenderingIntent::Saturation));
    intent->addItem(QObject::tr("Absolute Colorimetric"), int(RenderingIntent::AbsoluteColorimetric));
    intent->setCurrentIndex(std::max(0, intent->findData(int(settings().conversionIntent))));
    form->addRow(QObject::tr("Conversion intent:"), intent);
    auto* conversionBpc = new QCheckBox(QObject::tr("Use Black Point Compensation"));
    conversionBpc->setChecked(settings().conversionBlackPoint);
    form->addRow(QString(), conversionBpc);
    layout->addLayout(form);
    layout->addWidget(hint(QObject::tr("Images without a profile are treated as sRGB. New documents take the working space.")));
    auto* missing = new QCheckBox(QObject::tr("Ask when opening a file without a profile"));
    missing->setChecked(settings().askMissing);
    auto* mismatch = new QCheckBox(QObject::tr("Ask when opening a file whose profile is not the working space"));
    mismatch->setChecked(settings().askMismatch);
    layout->addWidget(missing);
    layout->addWidget(mismatch);
    layout->addWidget(okCancel(&dialog));
    if (dialog.exec() != QDialog::Accepted) return false;
    Settings s = settings();
    s.workingSpace = WorkingSpace(working->currentData().toInt());
    s.workingCmyk = cmyk->currentData().toString();
    s.policy = EmbeddedPolicy(policy->currentData().toInt());
    s.conversionIntent = RenderingIntent(intent->currentData().toInt());
    s.conversionBlackPoint = conversionBpc->isChecked();
    s.askMissing = missing->isChecked();
    s.askMismatch = mismatch->isChecked();
    setSettings(s);
    return true;
}

std::optional<ColorProfile> askAssignProfile(QWidget* parent, const ColorProfile& current) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Assign Profile"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QObject::tr("Assign Profile:")));
    auto* none = new QRadioButton(QObject::tr("Don’t Color Manage This Document (treated as sRGB)"));
    auto* working = new QRadioButton(QObject::tr("Working RGB: %1").arg(spaceLabel(settings().workingSpace)));
    auto* other = new QRadioButton(QObject::tr("Profile:"));
    auto* combo = new ProfileCombo(&dialog);
    combo->select(current);
    auto* row = new QHBoxLayout;
    row->addWidget(other);
    row->addWidget(combo, 1);
    layout->addWidget(none);
    layout->addWidget(working);
    layout->addLayout(row);
    (current.empty() ? none : current == workingProfile() ? working : other)->setChecked(true);
    QObject::connect(combo, QOverload<int>::of(&QComboBox::activated), other, [other](int) { other->setChecked(true); });
    layout->addWidget(hint(QObject::tr("Assigning changes how the colours are interpreted, not the pixel values. Convert to Profile keeps the colours' appearance instead.")));
    layout->addWidget(okCancel(&dialog));
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    if (none->isChecked()) return ColorProfile{};
    if (working->isChecked()) return workingProfile();
    return combo->profile();
}

std::optional<ConvertChoice> askConvertProfile(QWidget* parent, const ColorProfile& current) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Convert to Profile"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    form->addRow(QObject::tr("Source space:"), new QLabel(profileLabel(current)));
    auto* combo = new ProfileCombo(&dialog);
    combo->select(workingProfile());
    form->addRow(QObject::tr("Destination space:"), combo);
    auto* intent = intentCombo(&dialog, RenderingIntent::RelativeColorimetric);
    form->addRow(QObject::tr("Intent:"), intent);
    layout->addLayout(form);
    auto* bpc = new QCheckBox(QObject::tr("Use Black Point Compensation"));
    bpc->setChecked(true);
    layout->addWidget(bpc);
    layout->addWidget(hint(QObject::tr("Every layer’s pixels and the colours of text, shapes, fills, styles and adjustments are converted, "
                                       "so the document looks the same in its new profile.")));
    layout->addWidget(okCancel(&dialog));
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    auto profile = combo->profile();
    if (!profile) return std::nullopt;
    return ConvertChoice{*profile, {RenderingIntent(intent->currentData().toInt()), bpc->isChecked()}};
}

bool showProofSetup(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Customize Proof Condition"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* combo = new ProfileCombo(&dialog, ProfileKinds::RGBOrCMYK);
    combo->selectKey(settings().proofProfile);
    form->addRow(QObject::tr("Device to Simulate:"), combo);
    auto* intent = intentCombo(&dialog, settings().proofIntent);
    form->addRow(QObject::tr("Rendering Intent:"), intent);
    QColor gamut = settings().gamutColor;
    auto* colour = new QPushButton;
    auto paint = [colour, &gamut] { colour->setStyleSheet(QStringLiteral("background: %1; min-width: 48px;").arg(gamut.name())); };
    paint();
    QObject::connect(colour, &QPushButton::clicked, &dialog, [&dialog, &gamut, paint] {
        const QColor chosen = QColorDialog::getColor(gamut, &dialog, QObject::tr("Gamut Warning Colour"));
        if (chosen.isValid()) { gamut = chosen; paint(); }
    });
    form->addRow(QObject::tr("Gamut warning colour:"), colour);
    layout->addLayout(form);
    auto* bpc = new QCheckBox(QObject::tr("Black Point Compensation"));
    bpc->setChecked(settings().proofBlackPoint);
    layout->addWidget(bpc);
    layout->addWidget(hint(QObject::tr("View ▸ Proof Colors shows the document as it would look on this device; View ▸ Gamut Warning marks the colours it cannot show.")));
    layout->addWidget(okCancel(&dialog));
    if (dialog.exec() != QDialog::Accepted) return false;
    Settings s = settings();
    s.proofProfile = combo->key() == QLatin1String("__own__") ? QStringLiteral("working-cmyk") : combo->key();
    s.proofIntent = RenderingIntent(intent->currentData().toInt());
    s.proofBlackPoint = bpc->isChecked();
    s.gamutColor = gamut;
    setSettings(s);
    return true;
}

} // namespace app::color
