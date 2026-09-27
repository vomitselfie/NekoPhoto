// EditorSession: Assign Profile and Convert to Profile (docs/color-management.md).
#include "EditorSession.h"
#include "compositor/colormgmt.h"

using namespace compositor;

namespace app {

bool EditorSession::assignProfile(const ColorProfile& profile) {
    if (!document_) return false;
    if (document_->profile == profile) return true;
    commitTransform();
    beginEdit(QT_TRANSLATE_NOOP("History", "Assign Profile"));
    document_->profile = profile;
    endEdit();
    notifyDocument();
    return true;
}

bool EditorSession::convertToProfile(const ColorProfile& profile, const ConvertOptions& options, QString* errorText) {
    if (!document_) return false;
    commitTransform();
    // A stroke or a floating edit holds pixels in the old profile.
    cancelBrush();
    const ColorProfile from = document_->profile;
    Document converted = *document_;
    std::string why;
    if (!convertDocumentProfile(converted, profile, options, &why)) {
        if (errorText) *errorText = QString::fromStdString(why);
        return false;
    }
    beginEdit(QT_TRANSLATE_NOOP("History", "Convert to Profile"));
    *document_ = std::move(converted);
    conformToSampleType(*document_);
    endEdit();
    // The foreground and background colours are values in the document's space too.
    for (QColor* c : {&foregroundColor, &backgroundColor}) {
        double rgb[3] = {c->redF(), c->greenF(), c->blueF()};
        convertColor(from, profile, rgb, options);
        *c = QColor::fromRgbF(float(rgb[0]), float(rgb[1]), float(rgb[2]), c->alphaF());
    }
    notifyDocument();
    emit toolChanged();   // the swatches show the converted colours
    return true;
}

void EditorSession::adoptProfile(const ColorProfile& profile) {
    if (!document_ || document_->profile == profile) return;
    document_->profile = profile;
    notifyDocument();
}

} // namespace app
