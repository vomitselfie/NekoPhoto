// The font picker's grouping: a Linux font list with hundreds of Noto variants folds into a few rows.
#include "check.h"
#include "FontGroups.h"

using namespace app;

TEST_CASE(fonts_sharing_a_leading_name_fold_into_one_row) {
    QStringList families;
    for (const char* script : {"Arabic", "Armenian", "Bengali", "Cherokee", "Devanagari", "Georgian"}) { families << QString("Noto Sans %1").arg(script); families << QString("Noto Serif %1").arg(script); }
    families << "Noto Sans" << "Noto Serif" << "Noto Sans Mono" << "Noto Color Emoji" << "DejaVu Sans" << "DejaVu Sans Mono" << "DejaVu Serif" << "DejaVu Math TeX Gyre"
             << "Liberation Mono" << "Liberation Sans" << "Liberation Serif" << "Cantarell" << "Bebas Neue" << "Adwaita Sans" << "Adwaita Mono";
    auto groups = groupFonts(families, 4);
    // Top level: Adwaita (2) and Liberation (3) stay plain rows, Bebas Neue and Cantarell too; DejaVu and Noto fold.
    QStringList labels;
    for (const FontGroup& g : groups) labels << (g.isGroup() ? "[" + g.label + "]" : g.label);
    CHECK(labels.contains("[DejaVu]"));
    CHECK(labels.contains("[Noto]"));
    CHECK(labels.contains("Liberation Sans"));
    CHECK(labels.contains("Cantarell"));
    CHECK(!labels.contains("[Liberation]"));
    const FontGroup* noto = nullptr;
    for (const FontGroup& g : groups) if (g.label == "Noto") noto = &g;
    REQUIRE(noto != nullptr);
    CHECK_EQ(noto->members.size(), 16);
    CHECK_EQ(noto->family.toStdString(), std::string("Noto Sans"));   // the shortest stands for the group
    // Inside Noto: Sans and Serif fold again by their first two words; Color Emoji stays a plain row.
    QStringList inner;
    for (const FontGroup& g : noto->children) inner << (g.isGroup() ? "[" + g.label + "]" : g.label);
    CHECK(inner.contains("[Noto Sans]"));
    CHECK(inner.contains("[Noto Serif]"));
    CHECK(inner.contains("Noto Color Emoji"));
    const FontGroup* sans = nullptr;
    for (const FontGroup& g : noto->children) if (g.label == "Noto Sans") sans = &g;
    REQUIRE(sans != nullptr);
    CHECK_EQ(sans->family.toStdString(), std::string("Noto Sans"));   // named exactly like the group
    CHECK(sans->members.contains("Noto Sans Mono"));
    // No third level: the members of Noto Sans are plain rows.
    for (const FontGroup& g : sans->children) CHECK(!g.isGroup());
    // Sorted case-insensitively, and the label tells the count.
    CHECK_EQ(fontGroupLabel("Noto", 16).toStdString(), std::string("Noto  (16)"));
    CHECK_EQ(groups.front().label.toStdString(), std::string("Adwaita Mono"));
}

TEST_CASE(small_lists_stay_flat) {
    auto groups = groupFonts({"Sans", "Serif", "Mono"}, 4);
    CHECK_EQ(groups.size(), 3u);
    for (const FontGroup& g : groups) { CHECK(!g.isGroup()); CHECK_EQ(g.members.size(), 1); }
}

TEST_MAIN()
