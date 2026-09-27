// The typed image skeleton for high bit depth (docs/high-bit-depth-plan.md, P1): sample traits, ImageT,
// GrayImageT, the AnyImage / AnyGray holders that layers, masks and the selection keep, and the supports() registry.
#include "check.h"
#include "compositor/imaget.h"
#include "compositor/supports.h"
#include <type_traits>

using namespace compositor;

static_assert(std::is_same_v<ImageOf<SampleType::U8>, Image>);
static_assert(std::is_same_v<GrayOf<SampleType::U8>, GrayImage>);
static_assert(std::is_same_v<ImageOf<SampleType::U16>::Sample, uint16_t>);
static_assert(std::is_same_v<GrayOf<SampleType::F32>::Sample, float>);
static_assert(SampleTraits<SampleType::U16>::one == 32768);
static_assert(sampleBytes(SampleType::F32) == 4);

TEST_CASE(typed_image_layout) {
    Image16 a(3, 2);
    CHECK_EQ(a.channels(), 4);
    CHECK_EQ(a.stride(), 3 * 4 * 2);
    CHECK_EQ(a.byteCount(), size_t(3 * 2 * 4 * 2));
    CHECK_EQ(int(a.pixel(1, 1)[0]), 0);
    const uint16_t white[4] = {32768, 32768, 32768, 32768};
    a.fill(white);
    CHECK_EQ(int(a.pixel(2, 1)[3]), 32768);
    ImageF cmyk(2, 2, 5);
    CHECK_EQ(cmyk.stride(), 2 * 5 * 4);
    CHECK(cmyk.pixel(1, 0) == cmyk.row(0) + 5);
    Image16 tooBig(maxImageSide + 1, 1);
    CHECK(tooBig.isEmpty());
    Gray16 g(4, 4, 100);
    CHECK_EQ(int(g.at(3, 3)), 100);
    Gray16 copy = g;
    CHECK(copy == g);
}

TEST_CASE(any_image_holds_each_depth) {
    AnyImage none;
    CHECK(!none);
    CHECK(none.sampleType() == SampleType::U8);
    auto eight = std::make_shared<Image>(2, 2);
    AnyImage a = eight;                 // a shared_ptr<Image> converts, as 8-bit code assigns them
    CHECK(bool(a));
    CHECK(a.sampleType() == SampleType::U8);
    CHECK(a.u8().get() == eight.get());
    CHECK(!a.u16());
    CHECK(a.identity() == eight.get());
    AnyImage deep = Image16Ptr(std::make_shared<Image16>(2, 2));
    CHECK(deep.sampleType() == SampleType::U16);
    CHECK(!deep.u8());                  // 8-bit code sees no pixels in a deeper buffer
    CHECK(bool(deep));
    int width = visit([](const auto& p) { return p ? p->width() : -1; }, deep);
    CHECK_EQ(width, 2);
    AnyImage same = a;
    CHECK(same == a);
    CHECK(!(same == deep));
    same.reset();
    CHECK(!same);
    AnyImage null = nullptr;
    CHECK(!null);
    AnyGray mask = std::make_shared<GrayImage>(1, 1, 255);
    CHECK(mask.u8()->at(0, 0) == 255);
    AnyGray floatMask = GrayFPtr(std::make_shared<GrayF>(1, 1, 1.0f));
    CHECK(floatMask.sampleType() == SampleType::F32);
    CHECK(floatMask.f32()->at(0, 0) == 1.0f);
}

TEST_CASE(support_registry_is_eight_bit_only) {
    // Every feature supports 8-bit; nothing is ported deeper yet, listed or not.
    CHECK(supports("render.document", SampleType::U8));
    CHECK(!supports("render.document", SampleType::U16));
    CHECK(supports("filter.never-heard-of-it", SampleType::U8));
    CHECK(!supports("filter.never-heard-of-it", SampleType::F32));
    for (int k = 0; k < adjustmentKindCount; k++) {
        CHECK(supports(AdjustmentKind(k), SampleType::U8));
        CHECK(!supports(AdjustmentKind(k), SampleType::U16));
    }
    size_t count = 0;
    const FeatureSupport* table = featureSupportTable(count);
    for (size_t i = 0; i < count; i++) CHECK(table[i].types & onlyEightBit);
}

TEST_MAIN()
