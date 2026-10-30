// The edges' own pages (EdgePages.h), for what has no panel of its own: the left edge's Colour page (the foreground
// and background squares, a hue and saturation picker, swatches and the recent colours), the top edge's Documents page
// (the open documents as small pictures, the zoom, and the current one's size, mode and depth) and the bottom edge's
// Info (the pointer's position, the document's size, mode and depth). The window gives them their values and acts on
// what they ask (WindowEdges.cpp): they keep nothing themselves.
#pragma once
#include <QColor>
#include <QImage>
#include <QList>
#include <QString>
#include <QWidget>
#include <algorithm>
#include <vector>

class QComboBox;
class QHBoxLayout;
class QLabel;

namespace app {

class ColorSwatches;

/// A saturation and value square beside a hue bar, for the foreground colour.
class HsvPicker : public QWidget {
    Q_OBJECT
public:
    explicit HsvPicker(QWidget* parent = nullptr);
    QColor color() const { return QColor::fromHsvF(float(h_), float(s_), float(v_)); }
    void setColor(const QColor& c);
    QSize sizeHint() const override { return {200, 132}; }
    QSize minimumSizeHint() const override { return {120, 90}; }
signals:
    /// While dragging (the swatch follows), and once on release (the colour is set then).
    void colorChanging(const QColor& c);
    void colorPicked(const QColor& c);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
private:
    QRect squareRect() const;
    QRect hueRect() const;
    void pick(QPoint p);
    double h_ = 0, s_ = 0, v_ = 0;
    enum { None, Square, Hue } dragging_ = None;
    QImage square_;
    double squareHue_ = -1;
};

/// A grid of colour cells, ten to a row, as wide as it is given; a click sets the foreground, Ctrl+click the background
/// (Photoshop's Swatches).
class SwatchGrid : public QWidget {
    Q_OBJECT
public:
    explicit SwatchGrid(QWidget* parent = nullptr);
    void setColors(const QList<QColor>& colors);
    QList<QColor> colors() const { return colors_; }
    static constexpr int columns = 10, gap = 3;
    /// A cell's side at `width`.
    static int cell(int width) { return std::clamp((width - (columns - 1) * gap) / columns, 12, 24); }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return {columns * (12 + gap), 12}; }
signals:
    void chosen(const QColor& c, bool background);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
private:
    QList<QColor> colors_;
};

class ColourPage : public QWidget {
    Q_OBJECT
public:
    explicit ColourPage(QWidget* parent = nullptr);
    /// The colours as the swatches show them (through the monitor profile, greys on a mask).
    void setColors(const QColor& foreground, const QColor& background);
    void setRecent(const QList<QColor>& recent);
    HsvPicker* picker() const { return picker_; }
    SwatchGrid* swatches() const { return swatches_; }
    SwatchGrid* recent() const { return recent_; }
    ColorSwatches* squares() const { return squares_; }
    /// Photoshop's default swatch set: the primaries and secondaries, their light and dark tones, and greys.
    static QList<QColor> defaultSwatches();
signals:
    void foregroundPicked(const QColor& c);
    void backgroundPicked(const QColor& c);
    void swapRequested();
    void defaultsRequested();
    /// The Color Picker dialog for the foreground (or the background).
    void dialogRequested(bool background);
private:
    ColorSwatches* squares_;
    HsvPicker* picker_;
    SwatchGrid* swatches_;
    SwatchGrid* recent_;
    QLabel* recentLabel_;
};

class DocumentsPage : public QWidget {
    Q_OBJECT
public:
    struct Document {
        QString title;
        QImage thumbnail;   // empty: a blank tab
        bool current = false;
    };
    explicit DocumentsPage(QWidget* parent = nullptr);
    void setDocuments(const std::vector<Document>& documents);
    /// The current document's zoom (1 is 100%) and its size, mode and depth as one line.
    void setCurrent(double zoom, const QString& description);
    int count() const { return int(tiles_.size()); }
    QWidget* tile(int index) const { return tiles_[size_t(index)]; }
signals:
    void documentChosen(int index);
    /// A zoom from the list (0: fit on screen).
    void zoomChosen(double zoom);
private:
    QHBoxLayout* row_;
    std::vector<QWidget*> tiles_;
    QComboBox* zoom_;
    QLabel* description_;
};

class InfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit InfoPanel(QWidget* parent = nullptr);
    void setPointer(QPointF documentPoint);
    void setDocument(const QString& size, const QString& mode, const QString& zoom);
private:
    QLabel* pointer_;
    QLabel* size_;
    QLabel* mode_;
    QLabel* zoom_;
};

} // namespace app
