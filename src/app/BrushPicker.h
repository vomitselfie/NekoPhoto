// The Brush tool's preset picker: a button showing the current brush (the round tip or a MyPaint preset)
// that drops a filterable list of the presets, grouped by author, with their preview strokes.
#pragma once
#include <QToolButton>

class QFrame;
class QLineEdit;
class QTreeWidget;

namespace app {

class BrushPicker : public QToolButton {
    Q_OBJECT
public:
    explicit BrushPicker(QWidget* parent = nullptr);
    /// The chosen preset's id, or empty for the round tip.
    QString preset() const { return preset_; }
    void setPreset(const QString& id);
    /// Drops the list (what a click does), for scripts and screenshots.
    void showPicker();

signals:
    void presetChosen(const QString& id);
    /// The list's Import Brushes button.
    void importRequested();

private:
    void openPopup();
    void rebuild(const QString& filter);
    void choose(const QString& id);
    bool eventFilter(QObject* watched, QEvent* event) override;

    QString preset_;
    QFrame* popup_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
};

} // namespace app
