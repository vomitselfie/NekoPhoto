#include "Style.h"
#include "AdjustmentsPanel.h"
#include <QVBoxLayout>

using namespace compositor;

namespace app {

AdjustmentsPanel::AdjustmentsPanel(EditorSession* session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    title_ = new QLabel;
    title_->setWordWrap(true);
    layout->addWidget(title_);
    editor_ = new AdjustmentEditor;
    editor_->setSession(session_);
    layout->addWidget(editor_);
    layout->addStretch();
    connect(editor_, &AdjustmentEditor::editStarted, this, [this] { if (layerId_) { session_->beginAdjustmentEdit(); editing_ = true; } });
    connect(editor_, &AdjustmentEditor::settingsChanged, this, [this](const AdjustmentSettings& s) { if (layerId_) session_->setAdjustment(*layerId_, s); });
    connect(editor_, &AdjustmentEditor::editFinished, this, [this] { if (editing_) { editing_ = false; session_->endAdjustmentEdit(); } });
    connect(session_, &EditorSession::layersChanged, this, &AdjustmentsPanel::sync);
    sync();
}

void AdjustmentsPanel::sync() {
    if (editing_) return;
    const Layer* layer = session_->activeLayer();
    if (!layer || !layer->adjustment) {
        layerId_.reset();
        editor_->setVisible(false);
        title_->setAlignment(Qt::AlignCenter);
        title_->setStyleSheet(hintStyle());
        title_->setText(tr("Select an adjustment layer to edit it here.\nAdd one from the Layers panel or Layer > New Adjustment Layer."));
        return;
    }
    auto settings = session_->adjustmentSettings(layer->id);
    if (!settings) { editor_->setVisible(false); title_->setText(tr("This adjustment can't be edited here.")); return; }
    bool changedLayer = layerId_ != layer->id;
    layerId_ = layer->id;
    title_->setText(QStringLiteral("<b>%1</b> — %2").arg(QString::fromUtf8(adjustmentKindName(settings->kind)), QString::fromStdString(layer->name)));
    editor_->setVisible(true);
    if (changedLayer || !(editor_->settings() == *settings)) editor_->setSettings(*settings);
    if (settings->kind == AdjustmentKind::Levels && changedLayer) {
        // The histogram of what lies beneath the adjustment.
        Document below = *session_->document();
        int index = below.indexOf(layer->id);
        below.layers.erase(below.layers.begin() + index, below.layers.end());
        auto flattened = renderFlattened(below);
        editor_->setHistogram(levelsHistogram(*flattened, nullptr));
    }
}

} // namespace app
