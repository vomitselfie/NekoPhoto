// The bar over the canvas after a file opens with notes: what was converted or left out, without a dialog in
// the way. Details lists every note; Undo Open closes the document again.
#pragma once
#include <QFrame>
#include <QStringList>

class QLabel;
class QPushButton;
class QToolButton;

namespace app {

class ImportBanner : public QFrame {
    Q_OBJECT
public:
    explicit ImportBanner(QWidget* parent = nullptr);

    /// Shows the bar: `summary` (with %1 for the first note, already counted) and Details listing `notes` under
    /// `heading` in a window titled `title`. `canUndo` shows Undo Open.
    void present(const QString& summary, const QString& title, const QString& heading, const QStringList& notes, bool canUndo);
    const QStringList& notes() const { return notes_; }

signals:
    void undoRequested();
    void dismissed();

protected:
    void resizeEvent(QResizeEvent*) override;

private:
    void relayText();
    void showDetails();
    QLabel* text_;
    QPushButton* undo_;
    QString summary_, title_, heading_;
    QStringList notes_;
};

} // namespace app
