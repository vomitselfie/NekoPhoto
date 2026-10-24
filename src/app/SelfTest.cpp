// In-app checks that need the whole window, run by ctest through `nekophoto --self-test <name>`: runSelfTest
// below picks one by name. The checks are in SelfTest*.cpp by theme; the helpers they share are here
// (SelfTestSupport.h).
#include "SelfTestSupport.h"
#include "EditorSession.h"
#include "MainWindow.h"
#include "nekophoto/vectorlayer.h"
#include <QApplication>
#include <QDialog>
#include <QMenu>
#include <QMenuBar>
#include <QTimer>
#include <cstdio>
#include <functional>

using namespace nekophoto;

namespace app {

namespace {

uint64_t hashBytes(const uint8_t* data, size_t size, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < size; i++) { h ^= data[i]; h *= 1099511628211ull; }
    return h;
}

} // namespace

namespace selftest {

/// The document as text, without its random ids: every layer's name, kind, placement, look and pixels, the
/// selection, the guides, the composite, and the history's step names.
QString describe(EditorSession& s) {
    QStringList out;
    const Document& d = *s.document();
    for (const Layer& l : d.layers) {
        uint64_t pixels = 0, mask = 0;
        if (l.asset && l.asset->image && l.asset->image.u8()) pixels = hashBytes(l.asset->image.u8()->data(), l.asset->image.u8()->byteCount());
        if (l.mask && l.mask->asset.image.u8()) mask = hashBytes(l.mask->asset.image.u8()->data(), l.mask->asset.image.u8()->byteCount());
        const Layer* parent = l.parentId ? d.find(*l.parentId) : nullptr;
        out << QString("layer %1 in %2 group %3 at %4,%5 %6x%7 rot %8 flip %9%10 opacity %11 blend %12 visible %13 pixels %14 %15x%16 mask %17")
                   .arg(QString::fromStdString(l.name), parent ? QString::fromStdString(parent->name) : QStringLiteral("-")).arg(l.isGroup)
                   .arg(l.transform.origin.x, 0, 'g', 17).arg(l.transform.origin.y, 0, 'g', 17).arg(l.transform.size.width, 0, 'g', 17).arg(l.transform.size.height, 0, 'g', 17)
                   .arg(l.transform.rotation, 0, 'g', 17).arg(l.transform.flipX).arg(l.transform.flipY).arg(l.opacity).arg(int(l.blendMode)).arg(l.visible)
                   .arg(pixels).arg(l.asset && l.asset->image ? l.asset->image.width() : 0).arg(l.asset && l.asset->image ? l.asset->image.height() : 0).arg(mask);
        if (l.fill != 1) out << QString("layer %1 fill %2").arg(QString::fromStdString(l.name)).arg(l.fill);
    }
    // Clipping, resampling, smart objects and a mask's state, which the pixels alone do not show.
    for (const Layer& l : d.layers)
        out << QString("  %1: clipped %2 sampling %3 smart %4 mask on %5 adjustment %6").arg(QString::fromStdString(l.name)).arg(l.maskSourceId.has_value()).arg(int(l.transform.sampling))
                   .arg(l.smartObject.has_value()).arg(l.mask ? l.mask->enabled : false).arg(l.adjustment.has_value());
    uint64_t coverage = 0;
    if (d.selection && d.selection->coverage.u8()) coverage = hashBytes(d.selection->coverage.u8()->data(), d.selection->coverage.u8()->byteCount());
    out << QString("selection %1 %2").arg(d.selection.has_value()).arg(coverage);
    out << QString("size %1x%2 mode %3 depth %4").arg(d.width).arg(d.height).arg(int(d.colorMode)).arg(int(d.sampleType));
    QStringList paths, channels;
    for (const DocumentPath& path : s.paths()) paths << QString::fromStdString(path.name) + ":" + QString::number(path.path.subpaths.size());
    for (const Channel& c : d.channels) channels << QString::fromStdString(c.name) + ":" + QString::number(c.image.u8() ? hashBytes(c.image.u8()->data(), c.image.u8()->byteCount()) : 0);
    out << "paths " + paths.join(", ");
    out << "channels " + channels.join(", ");
    out << QString("guides %1").arg(d.guides.size());
    // The colours, the vector masks and the effects, which the pixels alone do not show either.
    out << QString("colors %1 %2").arg(s.foregroundColor.name(QColor::HexArgb), s.backgroundColor.name(QColor::HexArgb));
    for (const Layer& l : d.layers)
        if (hasLayerVectorMask(l) || hasAnyEffect(s.layerStyle(l.id)))
            out << QString("  %1: vector mask %2 style %3").arg(QString::fromStdString(l.name)).arg(hasLayerVectorMask(l)).arg(QString::fromStdString(layerStyleToJson(s.layerStyle(l.id))));
    if (auto flat = s.flattened()) out << QString("composite %1").arg(hashBytes(flat->data(), flat->byteCount()));
    QStringList history;
    for (const std::string& n : s.undoNames()) history << QString::fromStdString(n);
    out << "history " + history.join(" | ");
    return out.join('\n');
}

bool select(EditorSession& s, const char* name) {
    for (const Layer& l : s.document()->layers) if (l.name == name) { s.selectLayer(l.id); return true; }
    std::fprintf(stderr, "no layer named %s\n", name);
    return false;
}

QAction* action(MainWindow& w, const char* name) {
    QAction* a = w.findChild<QAction*>(QString::fromLatin1(name));
    if (!a) std::fprintf(stderr, "no action %s\n", name);
    return a;
}

/// A menu bar item by its path of titles ("Layer", "Layer Mask", "Invert"), mnemonics and shortcuts left out.
QAction* menuItem(MainWindow& w, const QStringList& path) {
    QList<QAction*> actions = w.menuBar()->actions();
    QAction* found = nullptr;
    for (int i = 0; i < path.size(); i++) {
        found = nullptr;
        for (QAction* a : actions)
            if (a->text().remove('&').section('\t', 0, 0) == path[i]) { found = a; break; }
        if (!found || (i + 1 < path.size() && !found->menu())) { std::fprintf(stderr, "no menu item %s\n", qPrintable(path.join(" > "))); return nullptr; }
        if (i + 1 < path.size()) {
            emit found->menu()->aboutToShow();
            actions = found->menu()->actions();
        }
    }
    return found;
}

/// Answers the next modal dialog (an input dialog, Canvas Size, Trim ...): `fill` sets its fields, then OK.
void answerModal(std::function<void(QDialog*)> fill) {
    QTimer::singleShot(0, [fill] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { std::fprintf(stderr, "no modal dialog to answer\n"); return; }
        if (fill) fill(dialog);
        dialog->accept();
    });
}

} // namespace selftest

int runSelfTest(MainWindow& window, const QString& name) {
    if (name == QLatin1String("command-path")) {
        const int first = commandPath(window);
        const int menus = menuCommands(window);
        const int files = fileCommands(window);
        return registry(window) || menus || files || first ? 1 : 0;
    }
    if (name == QLatin1String("guides")) return guides(window);
    if (name == QLatin1String("canvas-menus")) return canvasMenus(window);
    if (name == QLatin1String("search")) return search(window);
    if (name == QLatin1String("held-keys")) return heldKeys(window);
    if (name == QLatin1String("export-as")) return exportAs(window);
    if (name == QLatin1String("preferences")) return preferences(window);
    if (name == QLatin1String("work-counters")) return workCounters(window);
    if (name == QLatin1String("shortcuts")) return keyboardShortcuts(window);
    if (name == QLatin1String("full-screen")) return fullScreen(window);
    if (name == QLatin1String("hide-panels")) return hidePanels(window);
    if (name == QLatin1String("edges")) return edgePages(window);
    if (name == QLatin1String("forbidden-cursor")) return forbiddenCursor(window);
    if (name == QLatin1String("tool-ring")) return toolRing(window);
    if (name == QLatin1String("color-ring")) return colorRing(window);
    if (name == QLatin1String("layer-picker")) return layerPicker(window);
    std::fprintf(stderr, "unknown self-test %s (command-path, guides, canvas-menus, search, held-keys, export-as, preferences, work-counters, shortcuts, full-screen, hide-panels, edges, forbidden-cursor, tool-ring, color-ring, layer-picker)\n", qPrintable(name));
    return 2;
}

} // namespace app
