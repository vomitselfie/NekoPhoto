// The Colour Ring (View > Colour Ring, Alt+C): the Tool Ring's compass for the foreground colour, near the pointer and
// never in the mouse's way (RingController.h).
//
//      W  E  R         W  a recent colour        E / X  saturation up / down
//      S (D) F         S / F  hue round the wheel     R / V  brightness up / down
//      X  C  V         D  default colours         C  swap foreground and background
//
// The centre shows the foreground swatch over the background, as the toolbar does; each satellite the colour its key
// would give, with the value now. Hue, saturation and brightness are the colour picker's HSB on the RGB colour the
// foreground is (in CMYK and Lab documents too, where the colours are kept as sRGB and painted through the document's
// profile, as the colour picker's are). Held keys repeat, the wheel steps the value used last, and nothing here is an
// undo step (colours are not in Photoshop's history either).
#pragma once
#include "RingController.h"
#include <QColor>
#include <QList>

class QMenu;

namespace app {

class CommandRegistry;
class ToolRingOverlay;

class ColorRing : public RingController {
    Q_OBJECT
public:
    explicit ColorRing(MainWindow* window);
    /// View > Colour Ring (view.colorRing, Alt+C).
    void registerCommands(CommandRegistry& registry, QMenu* viewMenu);
    ToolRingOverlay* overlay() const { return ringOverlay_; }
    /// The colours the foreground has been when the document was edited or the ring closed, newest first (kept across
    /// sessions, at most `recentCount`).
    const QList<QColor>& recentColors() const { return recent_; }
    static constexpr int recentCount = 12;
    /// The steps: hue in degrees round the wheel, saturation and brightness in percent (onto multiples of the step).
    static constexpr double hueStep = 10, saturationStep = 5, brightnessStep = 5;
    /// The key the wheel steps (the value used last; hue at first).
    char wheelKey() const { return wheelKey_; }

protected:
    void opening() override;
    void closed(bool cancelled) override;
    void refresh() override;
    void press(char key, bool autoRepeat) override;
    bool wantsWheel() const override { return true; }
    void wheelStep(bool more) override;
    void connectSession(EditorSession& session) override;
    QRect keepClear() const override;

private:
    /// Hue 0..360, saturation and brightness 0..100, as the colour picker shows them.
    struct Hsb { double h = 0, s = 0, b = 0; };
    Hsb current() const;
    /// The colour a value key gives from `now` (S F X E V R), 8 bits a channel.
    static QColor stepped(const Hsb& now, char key, Hsb* after = nullptr);
    /// The colour W recalls next, or none.
    std::optional<QColor> nextRecent() const;
    void setForeground(const QColor& color);
    void note(const QColor& color);
    /// A colour as the toolbar's swatches show it (through the monitor profile; grey on a mask).
    QColor shown(const QColor& color) const;

    ToolRingOverlay* ringOverlay_;
    CommandRegistry* registry_ = nullptr;
    /// The HSB the ring set last and the colour it made, so a grey keeps its hue and steps do not drift with rounding.
    Hsb hsb_;
    QColor hsbFor_;
    QList<QColor> recent_;
    int recall_ = 0;          // where W is in recent_
    bool changing_ = false;   // the ring is setting the colour (its own steps are not noted)
    char wheelKey_ = 'F';
};

} // namespace app
