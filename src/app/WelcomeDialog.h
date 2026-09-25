// The first-run introduction: the splash, then what carries over from the apps people come from
// (Photoshop, Clip Studio Paint, Procreate, Krita and GIMP), then ways to start. Shown once, on the first
// ordinary launch; Help > Welcome to NekoPhoto opens it again.
#pragma once
#include <QDialog>
#include <QVector>

class QLabel;
class QPushButton;
class QStackedWidget;

namespace app {

class WelcomeDialog : public QDialog {
    Q_OBJECT
public:
    explicit WelcomeDialog(QWidget* parent = nullptr);

    /// Whether this user has seen it (QSettings welcome/shown).
    static bool shown();
    static void markShown();

signals:
    void openRequested();
    void newCanvasRequested();
    void importBrushesRequested();

private:
    void go(int page);
    QStackedWidget* pages_;
    QVector<QLabel*> dots_;
    QPushButton* back_;
    QPushButton* next_;
    QPushButton* skip_;
};

} // namespace app
