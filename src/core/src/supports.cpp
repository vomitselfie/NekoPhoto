#include "compositor/supports.h"
#include <string>

namespace compositor {

namespace {
// One line per feature ported beyond 8-bit, e.g. {"adjustment.Levels", onlyEightBit | sampleTypeBit(SampleType::U16)}.
// In P1 nothing goes beyond 8-bit: the document renderer is listed so P2 has the line to widen (RenderExec<U16>).
constexpr FeatureSupport table[] = {
    {"render.document", onlyEightBit},
};
}   // namespace

const FeatureSupport* featureSupportTable(size_t& count) {
    count = sizeof(table) / sizeof(table[0]);
    return table;
}

SampleTypes supportedSampleTypes(std::string_view feature) {
    for (const FeatureSupport& f : table) if (f.feature == feature) return SampleTypes(f.types | onlyEightBit);
    return onlyEightBit;
}

bool supports(AdjustmentKind kind, SampleType type) {
    return supports(std::string("adjustment.") + adjustmentKindName(kind), type);
}

} // namespace compositor
