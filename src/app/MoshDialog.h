// Filter > Mosh > <category> > <effect>: one of OpenMosh's effects (compositor/mosh.h), its controls made from the
// effect's parameter list, previewing on the canvas and applied as one undo step (docs/mosh.md).
#pragma once
#include "PixelDialog.h"
#include "compositor/mosh.h"
#include <QJsonObject>
#include <QString>
#include <functional>
#include <vector>

class QTimer;

namespace app {

namespace names {
/// A Mosh effect, category, parameter or option name (OpenMosh's English) in the interface language.
QString mosh(std::string_view english);
} // namespace names

class MoshDialog : public PixelDialog {
    Q_OBJECT
public:
    MoshDialog(EditorSession* session, const compositor::mosh::EffectSpec& spec, QWidget* parent = nullptr);
    /// The settings the controls show (for tests and screenshots).
    const compositor::mosh::Settings& settings() const { return settings_; }

protected:
    bool apply() override;

private:
    void schedulePreview();
    void refreshPreview();
    const compositor::mosh::EffectSpec& spec_;
    compositor::mosh::Settings settings_;
    QTimer* previewTimer_;
    std::vector<std::function<void()>> syncers_;   // put each control back in step with settings_
};

/// The request pixels.mosh takes for `settings`: effect, params by key (bools as true/false, choices by index) and seed.
QJsonObject moshRequest(const compositor::mosh::Settings& settings);

} // namespace app
