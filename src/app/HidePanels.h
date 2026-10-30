// Tab and Shift+Tab in the Standard Screen Mode, as Photoshop's: Tab hides every panel, the tool rail and the options
// bar (the menus, the document tabs and the status bar stay); Shift+Tab hides only the panels docked on the right.
// Either key again gives back the dock layout as it was (saveState()), so a panel that was closed stays closed. Full
// Screen Mode keeps its own Tab (WindowEdges.h): the layout comes back while it is on, and is hidden again after it.
// Tab and Shift+Tab stay with a text field wherever it is, in either mode.
#pragma once
#include <QByteArray>
#include <QObject>

class QWidget;

namespace app {

class MainWindow;

/// A field that takes typed text (a name, a number), where Tab, Shift+Tab and the letters stay with it.
bool takesTypedText(const QWidget* w);

class HidePanels : public QObject {
    Q_OBJECT
public:
    enum Hidden { None, All, Right };

    explicit HidePanels(MainWindow* window);
    ~HidePanels() override;

    /// What Tab (All) or Shift+Tab (Right) hid; None while everything shows. In full screen, what comes back hidden.
    Hidden hidden() const { return hidden_; }
    /// Hides `h` (the layout before it kept), or with None gives back the layout from before.
    void set(Hidden h);
    /// Tab (All), Shift+Tab (Right): hides, or when anything is hidden, gives the layout back.
    void toggle(Hidden h) { set(hidden_ == None ? h : None); }
    /// Whether the tab on screen keeps its options bar hidden.
    bool hidesOptions() const;
    /// The layout to keep when the window closes: the one from before hiding.
    QByteArray normalState() const;

    /// Full screen is coming on: the layout as it was, so its edges hold every panel; what was hidden stays noted.
    void beforeFullScreen();
    /// The window's layout came back after full screen: hidden again as before it.
    void afterFullScreen();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void restore();
    void hideNow();

    MainWindow* window_;
    Hidden hidden_ = None;
    QByteArray saved_;
};

} // namespace app
