// Filter > Mosh > <category> > <effect>: one of OpenMosh's effects (nekophoto/mosh.h), its controls made from the
// effect's parameter list, previewing on the canvas and applied as one undo step (docs/mosh.md).
#pragma once
#include "PixelDialog.h"
#include "nekophoto/mosh.h"
#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QComboBox;
class QLineEdit;
class QTimer;

namespace app {

namespace names {
/// A Mosh effect, category, parameter or option name (OpenMosh's English) in the interface language.
QString mosh(std::string_view english);
} // namespace names

/// What a Composite effect reads besides the layer it changes: Overlay's and Mask's other layer, placed on that
/// layer's pixel grid as the two sit in the document, or Caption's text drawn by Qt (white on transparent). Holds the
/// images the Sources point at.
struct MoshExtras {
    std::shared_ptr<nekophoto::Image> aux;
    std::shared_ptr<nekophoto::Image16> aux16;
    float auxWidth = 0, auxHeight = 0;
    nekophoto::mosh::Sources sources() const;
};

/// The extras `spec` needs for a `width` x `height` layer placed by `target`: `auxLayer`'s own pixels (without its
/// mask, opacity or effects) for Overlay and Mask, `text` drawn 8 pixels a line times Scale for Caption.
MoshExtras moshExtras(const nekophoto::mosh::EffectSpec& spec, const nekophoto::mosh::Settings& settings, const nekophoto::Document& document,
                      const std::optional<nekophoto::Uuid>& auxLayer, const QString& text, const nekophoto::LayerTransform& target, int width, int height);

/// The layers Overlay and Mask can read in the dialog: the visible layers with pixels other than `exclude`, top first.
std::vector<const nekophoto::Layer*> moshSourceLayers(const nekophoto::Document& document, const std::optional<nekophoto::Uuid>& exclude);

class MoshDialog : public PixelDialog {
    Q_OBJECT
public:
    MoshDialog(EditorSession* session, const nekophoto::mosh::EffectSpec& spec, QWidget* parent = nullptr);
    /// The settings the controls show (for tests and screenshots).
    const nekophoto::mosh::Settings& settings() const { return settings_; }

protected:
    bool apply() override;

private:
    void schedulePreview();
    void refreshPreview();
    const nekophoto::mosh::EffectSpec& spec_;
    nekophoto::mosh::Settings settings_;
    QTimer* previewTimer_;
    std::vector<std::function<void()>> syncers_;   // put each control back in step with settings_
    QComboBox* layerCombo_ = nullptr;   // Overlay's and Mask's other layer
    QLineEdit* text_ = nullptr;         // Caption's text
    std::optional<nekophoto::Uuid> auxLayer() const;
    QString captionText() const;
    MoshExtras extras() const;
};

/// The request pixels.mosh takes for `settings`: effect, params by key (bools as true/false, choices by index) and seed,
/// with the other layer's id for Overlay and Mask and the text for Caption.
QJsonObject moshRequest(const nekophoto::mosh::Settings& settings, const std::optional<nekophoto::Uuid>& layer = std::nullopt, const QString& text = {});

} // namespace app
