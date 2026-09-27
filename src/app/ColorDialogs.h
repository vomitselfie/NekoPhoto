// Edit > Color Settings, Assign Profile and Convert to Profile, and View > Proof Setup (docs/color-management.md).
#pragma once
#include "compositor/colormgmt.h"
#include <optional>

class QWidget;

namespace app::color {

/// Edit > Color Settings: the working space and the policies; saved when accepted.
bool showColorSettings(QWidget* parent);

/// Edit > Assign Profile: none (untagged), the working space, or another profile. Nullopt when cancelled.
std::optional<compositor::ColorProfile> askAssignProfile(QWidget* parent, const compositor::ColorProfile& current);

/// Edit > Convert to Profile: the destination, intent and black point compensation. Nullopt when cancelled.
struct ConvertChoice {
    compositor::ColorProfile profile;
    compositor::ConvertOptions options;
};
std::optional<ConvertChoice> askConvertProfile(QWidget* parent, const compositor::ColorProfile& current);

/// View > Proof Setup > Custom: the proofing profile, intent, black point compensation and gamut warning colour.
bool showProofSetup(QWidget* parent);

} // namespace app::color
