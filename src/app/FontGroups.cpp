#include "FontGroups.h"
#include <algorithm>
#include <map>

namespace app {

namespace {

QString representative(const QString& key, const QStringList& members) {
    QString best;
    for (const QString& m : members) {
        if (m.compare(key, Qt::CaseInsensitive) == 0) return m;
        if (best.isEmpty() || m.size() < best.size()) best = m;
    }
    return best;
}

std::vector<FontGroup> groupBy(const QStringList& families, int words, int threshold, int depth) {
    // Buckets by the leading `words` words, in the order the (sorted) families come.
    std::map<QString, QStringList> buckets;
    QStringList order;
    for (const QString& f : families) {
        QStringList parts = f.split(' ', Qt::SkipEmptyParts);
        QString key = parts.size() <= words ? f : parts.mid(0, words).join(' ');
        if (!buckets.count(key)) order << key;
        buckets[key] << f;
    }
    std::vector<FontGroup> nodes;
    for (const QString& key : order) {
        const QStringList& members = buckets[key];
        if (members.size() < threshold || depth >= 2) {
            for (const QString& m : members) nodes.push_back({m, m, {m}, {}});
            continue;
        }
        FontGroup group;
        group.label = key;
        group.family = representative(key, members);
        group.members = members;
        group.children = groupBy(members, words + 1, threshold, depth + 1);
        nodes.push_back(std::move(group));
    }
    return nodes;
}

} // namespace

std::vector<FontGroup> groupFonts(const QStringList& families, int threshold) {
    QStringList sorted = families;
    std::sort(sorted.begin(), sorted.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    return groupBy(sorted, 1, std::max(2, threshold), 0);
}

QString fontGroupLabel(const QString& name, int members) { return QStringLiteral("%1  (%2)").arg(name).arg(members); }

} // namespace app
