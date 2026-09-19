// Grouping a font list for a picker: families sharing a leading name ("Noto Sans Arabic", "Noto Sans
// Armenian", ... two hundred of them on a Linux desktop) fold under that name, then by their first two
// words within it; a group stands for one representative family until it is expanded.
#pragma once
#include <QString>
#include <QStringList>
#include <vector>

namespace app {

struct FontGroup {
    QString label;                   // the family, or the shared leading name of a group
    QString family;                  // the family this row stands for (a group's representative)
    QStringList members;             // every family under this row (one for a plain family)
    std::vector<FontGroup> children; // empty for a plain family
    bool isGroup() const { return !children.empty(); }
};

/// The grouping: under one leading word when at least `threshold` families share it, then by the first
/// two words within a group; sorted case-insensitively. The representative is the family named exactly
/// like the group, else the shortest.
std::vector<FontGroup> groupFonts(const QStringList& families, int threshold = 4);

/// A group row's text: the name and how many families it holds.
QString fontGroupLabel(const QString& name, int members);

} // namespace app
