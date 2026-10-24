// The Brush tool's preset picker: a button showing the current brush (the round tip or a MyPaint preset)
// that drops a filterable list of the presets, grouped by author, with their preview strokes. The list is also the
// left edge's Brushes page (EdgePages.h).
#pragma once
#include <QToolButton>
#include <optional>

class QFrame;
class QLineEdit;
class QTreeWidget;

namespace app {

/// The presets as a filterable list, grouped by author, with an Import Brushes button.
class BrushList : public QWidget {
    Q_OBJECT
public:
    explicit BrushList(QWidget* parent = nullptr);
    /// The preset shown as current (empty: the round tip).
    QString preset() const { return preset_; }
    void setPreset(const QString& id);
    QLineEdit* filterField() const { return filter_; }
    /// Fills the list again from the library (after brushes were imported).
    void refresh();

signals:
    void presetChosen(const QString& id);
    /// The Import Brushes button.
    void importRequested();
    /// Esc in the list or its field.
    void dismissed();

private:
    void rebuild(const QString& filter);
    bool eventFilter(QObject* watched, QEvent* event) override;

    QString preset_;
    QLineEdit* filter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
};

class BrushPicker : public QToolButton {
    Q_OBJECT
public:
    explicit BrushPicker(QWidget* parent = nullptr);
    /// The chosen preset's id, or empty for the round tip.
    QString preset() const { return preset_; }
    void setPreset(const QString& id);
    /// Drops the list (what a click does), for scripts and screenshots.
    void showPicker();
    /// Drops the list at a screen point (the Brush tool's right-click on the canvas, as in Photoshop).
    void showPickerAt(QPoint globalPosition);

signals:
    void presetChosen(const QString& id);
    /// The list's Import Brushes button.
    void importRequested();

private:
    void openPopup(std::optional<QPoint> at = std::nullopt);
    void choose(const QString& id);

    QString preset_;
    QFrame* popup_ = nullptr;
};

} // namespace app
