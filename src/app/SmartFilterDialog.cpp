#include "SmartFilterDialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QMessageBox>
#include <QVBoxLayout>
#include <cmath>
#include <functional>

using namespace compositor;

namespace app {

bool SmartFilterDialog::canEdit(const EditorSession* session, const Uuid& layerId, int index) {
    if (!session || !session->canEditSmartFilters(layerId)) return false;
    auto stack = session->smartFilters(layerId);
    return stack && index >= 0 && index < int(stack->entries.size()) && !std::holds_alternative<std::monostate>(stack->entries[size_t(index)].parameters);
}

SmartFilterDialog::SmartFilterDialog(EditorSession* session, Uuid layerId, int index, Page page, QWidget* parent)
    : QDialog(parent), session_(session), layerId_(std::move(layerId)), index_(index) {
    auto stack = session->smartFilters(layerId_);
    if (stack && index >= 0 && index < int(stack->entries.size())) entry_ = stack->entries[size_t(index)];
    const QString name = QString::fromStdString(entry_.name).remove(QStringLiteral("...")).replace(QStringLiteral("&&"), QStringLiteral("&"));
    setWindowTitle(page == Page::Blending ? tr("Blending Options (%1)").arg(name) : tr("%1 (Smart Filter)").arg(name));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    auto number = [&](const QString& label, double min, double max, int decimals, double value, std::function<void(double)> set, const QString& suffix = {}) {
        auto* spin = new QDoubleSpinBox;
        spin->setRange(min, max);
        spin->setDecimals(decimals);
        spin->setValue(value);
        spin->setSuffix(suffix);
        spin->setKeyboardTracking(false);
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, set](double v) { set(v); schedulePreview(); });
        form->addRow(label, spin);
    };
    auto check = [&](const QString& label, bool value, std::function<void(bool)> set) {
        auto* box = new QCheckBox(label);
        box->setChecked(value);
        connect(box, &QCheckBox::toggled, this, [this, set](bool on) { set(on); schedulePreview(); });
        form->addRow(QString(), box);
    };
    auto integer = [](double v) { return int32_t(std::lround(v)); };
    if (page == Page::Blending) {
        auto* mode = new QComboBox;
        for (int m : blendModeMenuOrder()) {
            if (m < 0) mode->insertSeparator(mode->count());
            else mode->addItem(QString::fromUtf8(blendModeName(BlendMode(m))), m);
        }
        mode->setCurrentIndex(std::max(0, mode->findData(int(entry_.blend))));
        connect(mode, QOverload<int>::of(&QComboBox::activated), this, [this, mode](int i) {
            if (mode->itemData(i).isValid()) { entry_.blend = BlendMode(mode->itemData(i).toInt()); schedulePreview(); }
        });
        form->addRow(tr("Mode"), mode);
        number(tr("Opacity"), 0, 100, 0, std::round(entry_.opacity * 100), [this](double v) { entry_.opacity = v / 100; }, QStringLiteral("%"));
    } else {
        using namespace smartfilter;
        std::visit([&](auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, GaussianBlur> || std::is_same_v<T, HighPass>)
                number(tr("Radius"), 0.1, 1000, 1, p.radius, [&p](double v) { p.radius = v; }, tr(" px"));
            else if constexpr (std::is_same_v<T, Median>) number(tr("Radius"), 1, 500, 0, p.radius, [&p](double v) { p.radius = v; }, tr(" px"));
            else if constexpr (std::is_same_v<T, DustAndScratches>) {
                number(tr("Radius"), 1, 500, 0, p.radius, [&p, integer](double v) { p.radius = integer(v); }, tr(" px"));
                number(tr("Threshold"), 0, 255, 0, p.threshold, [&p, integer](double v) { p.threshold = integer(v); });
            } else if constexpr (std::is_same_v<T, SurfaceBlur>) {
                number(tr("Radius"), 1, 100, 0, p.radius, [&p](double v) { p.radius = v; }, tr(" px"));
                number(tr("Threshold"), 2, 255, 0, p.threshold, [&p, integer](double v) { p.threshold = integer(v); });
            } else if constexpr (std::is_same_v<T, UnsharpMask>) {
                number(tr("Amount"), 1, 500, 0, p.amount, [&p](double v) { p.amount = v; }, QStringLiteral("%"));
                number(tr("Radius"), 0.1, 1000, 1, p.radius, [&p](double v) { p.radius = v; }, tr(" px"));
                number(tr("Threshold"), 0, 255, 0, p.threshold, [&p, integer](double v) { p.threshold = integer(v); });
            } else if constexpr (std::is_same_v<T, MotionBlur>) {
                number(tr("Angle"), -360, 360, 0, p.angle, [&p, integer](double v) { p.angle = integer(v); }, QStringLiteral("°"));
                number(tr("Distance"), 1, 999, 0, p.distance, [&p, integer](double v) { p.distance = integer(v); }, tr(" px"));
            } else if constexpr (std::is_same_v<T, PlasticWrap>) {
                number(tr("Highlight Strength"), 0, 20, 0, p.highlight, [&p, integer](double v) { p.highlight = integer(v); });
                number(tr("Detail"), 1, 15, 0, p.detail, [&p, integer](double v) { p.detail = integer(v); });
                number(tr("Smoothness"), 1, 15, 0, p.smoothness, [&p, integer](double v) { p.smoothness = integer(v); });
            } else if constexpr (std::is_same_v<T, Mosaic>) number(tr("Cell Size"), 2, 200, 0, p.cellSize, [&p, integer](double v) { p.cellSize = integer(v); }, tr(" px"));
            else if constexpr (std::is_same_v<T, Emboss>) {
                number(tr("Angle"), -360, 360, 0, p.angle, [&p, integer](double v) { p.angle = integer(v); }, QStringLiteral("°"));
                number(tr("Height"), 1, 100, 0, p.height, [&p, integer](double v) { p.height = integer(v); }, tr(" px"));
                number(tr("Amount"), 1, 500, 0, p.amount, [&p, integer](double v) { p.amount = integer(v); }, QStringLiteral("%"));
            } else if constexpr (std::is_same_v<T, BoxBlur>) number(tr("Radius"), 1, 2000, 0, p.radius, [&p](double v) { p.radius = v; }, tr(" px"));
            else if constexpr (std::is_same_v<T, RadialBlur>) {
                number(tr("Amount"), 1, 100, 0, p.amount, [&p, integer](double v) { p.amount = integer(v); });
                auto* quality = new QComboBox;
                quality->addItem(tr("Draft"), 8);
                quality->addItem(tr("Good"), 16);
                quality->addItem(tr("Best"), 32);
                quality->setCurrentIndex(p.samples <= 8 ? 0 : p.samples <= 16 ? 1 : 2);
                connect(quality, QOverload<int>::of(&QComboBox::activated), this, [this, &p, quality](int i) { p.samples = quality->itemData(i).toInt(); schedulePreview(); });
                form->addRow(tr("Quality"), quality);
            } else if constexpr (std::is_same_v<T, AddNoise>) {
                number(tr("Amount"), 0.1, 400, 1, p.amount, [&p](double v) { p.amount = v; }, QStringLiteral("%"));
                check(tr("Gaussian"), p.gaussian, [&p](bool on) { p.gaussian = on; });
                check(tr("Monochromatic"), p.monochromatic, [&p](bool on) { p.monochromatic = on; });
            }
        }, entry_.parameters);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    previewTimer_.setSingleShot(true);
    previewTimer_.setInterval(120);
    connect(&previewTimer_, &QTimer::timeout, this, &SmartFilterDialog::preview);
}

SmartFilterDialog::~SmartFilterDialog() {
    if (previewing_ && session_) session_->clearPixelPreview();
}

void SmartFilterDialog::schedulePreview() { previewTimer_.start(); }

void SmartFilterDialog::preview() {
    // The stack drawn with the change on a copy of the document, shown in place of the layer's pixels.
    if (!session_ || !session_->document()) return;
    auto stack = session_->smartFilters(layerId_);
    const Document& current = *session_->document();
    const Layer* layer = current.find(layerId_);
    if (!stack || !layer || index_ < 0 || index_ >= int(stack->entries.size())) return;
    stack->entries[size_t(index_)] = entry_;
    Document copy = current;
    Layer* target = copy.find(layerId_);
    if (!target || !compositor::setSmartFilters(copy, *target, *stack, nullptr)) return;
    session_->setPixelPreview(target->asset->image, target->transform, layerId_);
    previewing_ = true;
}

void SmartFilterDialog::done(int result) {
    previewTimer_.stop();
    if (previewing_ && session_) { session_->clearPixelPreview(); previewing_ = false; }
    if (result == QDialog::Accepted && session_) {
        QString error;
        if (!session_->setSmartFilterEntry(layerId_, index_, entry_, &error)) { QMessageBox::warning(this, windowTitle(), error); return; }
    }
    QDialog::done(result);
}

} // namespace app
