// Automation methods: document. Registered from AutomationServer::registerHandlers (Automation.cpp).
#include "TextLayer.h"
#include "Automation.h"
#include "AutomationHandlers.h"
#include "CanvasWidget.h"
#include "ImageConvert.h"
#include "compositor/png.h"
#include "compositor/psd_writer.h"
#include <QFileInfo>
#include <QPainter>
#include <algorithm>
#include <cmath>

using namespace compositor;
using namespace app::rpc;

namespace app {

void AutomationServer::registerDocumentHandlers() {
    MainWindow* w = window_;
    const SessionOf session{w};
    const DocumentOf document{w};

    // ---- document
    add("document.info", [w, session, document](const QJsonObject&) {
        const Document& doc = document();
        EditorSession* s = session();
        QJsonObject o{{"width", doc.width}, {"height", doc.height}, {"resolution", doc.resolution}, {"layers", int(doc.layers.size())},
                      {"modified", s->isModified()}, {"title", s->title()}, {"tab", w->currentTabIndex()}};
        if (!s->projectPath().isEmpty()) o["path"] = s->projectPath();
        if (auto id = s->activeLayerId()) { o["activeLayer"] = qs(*id); o["maskSelected"] = s->isMaskSelected(); }
        if (doc.selection && !doc.selection->isEmpty()) o["selection"] = rectJson(doc.selection->bounds());
        o["undo"] = s->canUndo() ? QJsonValue(s->undoName()) : QJsonValue::Null;
        o["redo"] = s->canRedo() ? QJsonValue(s->redoName()) : QJsonValue::Null;
        return o;
    });
    add("document.overview", [w, session, document](const QJsonObject& p) {
        // Everything an agent reads before touching a document, as text: one line per layer, top first.
        const Document& doc = document();
        EditorSession* s = session();
        const int limit = std::max(1, integer(p, "maxLayers", 80));
        QStringList lines;
        lines << QStringLiteral("%1 x %2 px at %3 ppi, %4 layers%5%6").arg(doc.width).arg(doc.height).arg(doc.resolution).arg(doc.layers.size())
                     .arg(s->isModified() ? ", unsaved changes" : "").arg(s->projectPath().isEmpty() ? QString() : ", " + s->projectPath());
        if (doc.selection && !doc.selection->isEmpty()) {
            const Rect b = doc.selection->bounds();
            lines << QStringLiteral("Selection: %1 x %2 at (%3, %4)").arg(b.width).arg(b.height).arg(b.x).arg(b.y);
        } else lines << "Selection: none";
        lines << "Undo: " + (s->canUndo() ? s->undoName() : QStringLiteral("nothing")) + (s->canRedo() ? "; redo: " + s->redoName() : QString());
        lines << "Layers, top first (* active):";
        const auto active = s->activeLayerId();
        const auto entries = hierarchyEntries(doc.layers, true);
        int shown = 0;
        for (const HierarchyEntry& e : entries) {
            if (shown == limit) { lines << QStringLiteral("... %1 more (layers.list has them all)").arg(entries.size() - shown); break; }
            const Layer& l = *e.layer;
            QStringList bits;
            if (l.isGroup) bits << "folder";
            else if (l.adjustment) bits << QString::fromUtf8(adjustmentKindName(l.adjustment->kind)) + " adjustment";
            else {
                bits << (l.isLiveText() ? "text" : l.isLiveShape() ? "shape" : "pixels");
                const Rect r = l.transform.bounds();
                bits << QStringLiteral("%1 x %2 at (%3, %4)").arg(r.width).arg(r.height).arg(r.x).arg(r.y);
                if (!l.asset || !l.asset->image) bits << "blank";
            }
            if (l.opacity < 1) bits << QStringLiteral("%1%").arg(std::lround(l.opacity * 100));
            if (l.blendMode != BlendMode::Normal) bits << QString::fromUtf8(blendModeName(l.blendMode));
            if (!l.visible) bits << "hidden";
            if (l.mask) bits << (l.mask->enabled ? "mask" : "mask off");
            if (l.maskSourceId) bits << "clipped";
            if (l.transform.rotation != 0) bits << QStringLiteral("rotated %1").arg(l.transform.rotation);
            if (l.isLiveText()) bits << "\"" + qs(l.text->text).left(40) + "\"";
            lines << QStringLiteral("%1%2 %3 (%4) id %5").arg(QString(e.depth * 2 + 1, ' '), active == l.id ? "*" : "-", qs(l.name), bits.join(", "), qs(l.id));
            shown++;
        }
        return QJsonObject{{"overview", lines.join('\n')}, {"width", doc.width}, {"height", doc.height}, {"layers", int(doc.layers.size())},
                           {"activeLayer", active ? qs(*active) : QString()}, {"tab", w->currentTabIndex()}};
    });
    add("document.new", [w, session](const QJsonObject& p) {
        int width = integer(p, "width", 1920), height = integer(p, "height", 1080);
        if (!Document::validDimension(width) || !Document::validDimension(height)) fail("width and height must be 1..30000", invalidParams);
        if (session()->hasDocument()) w->newTab();
        session()->createDocument(width, height, num(p, "resolution", 72), flag(p, "emptyLayer", true));
        return QJsonObject{{"tab", w->currentTabIndex()}, {"width", width}, {"height", height}};
    });
    add("document.open", [w, session](const QJsonObject& p) {
        QString path = QFileInfo(str(p, "path")).absoluteFilePath();
        if (!QFileInfo::exists(path)) fail("no such file: " + path, invalidParams);
        w->openPath(path);
        EditorSession* s = session();
        QJsonObject out{{"tab", w->currentTabIndex()}, {"title", s->title()}, {"width", s->hasDocument() ? s->document()->width : 0}, {"height", s->hasDocument() ? s->document()->height : 0}};
        if (path.endsWith(".psd", Qt::CaseInsensitive) || path.endsWith(".psb", Qt::CaseInsensitive) || path.endsWith(".clip", Qt::CaseInsensitive)) {
            if (!s->hasDocument()) fail(path.endsWith(".clip", Qt::CaseInsensitive) ? "the Clip Studio file could not be imported" : "the Photoshop file could not be imported");
            out["layers"] = int(s->document()->layers.size());
            out["notes"] = QJsonArray::fromStringList(w->lastImportNotes());
        }
        return out;
    });
    add("document.import", [w](const QJsonObject& p) {
        // An image file as a new layer (a first import creates the canvas).
        QString path = QFileInfo(str(p, "path")).absoluteFilePath(), error;
        std::optional<QPointF> at;
        if (has(p, "x") && has(p, "y")) at = QPointF(num(p, "x"), num(p, "y"));
        if (!w->importImageFile(path, at, &error)) fail(error);
        const Layer* l = w->session()->activeLayer();
        return l ? layerJson(*l, 0) : QJsonObject{};
    });
    add("document.save", [w, session, document](const QJsonObject& p) {
        document();
        QString path = has(p, "path") ? QFileInfo(str(p, "path")).absoluteFilePath() : session()->projectPath();
        if (path.isEmpty()) fail("the document has no path yet; pass path", invalidParams);
        if (!path.endsWith(".comp", Qt::CaseInsensitive)) path += ".comp";
        QString error;
        if (!session()->saveProject(path, &error)) fail(error);
        w->noteRecent(path);
        // Compositor for macOS opens projects up to 100 megapixels of layers in total.
        return QJsonObject{{"path", path}, {"macCompatible", session()->document()->fitsMacBudget()}};
    });
    add("document.export", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        QString path = QFileInfo(str(p, "path")).absoluteFilePath();
        QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix == "psd" || suffix == "psb") {
            // Layered: what Photoshop cannot carry comes back in the reply, the way the export dialog lists it.
            PsdExportSummary summary;
            std::string error;
            PsdExportOptions options = app::psdExportOptions();
            options.large = suffix == "psb";
            if (!exportPsd(doc, path.toStdString(), options, &summary, &error)) fail("couldn't write " + path + ": " + qs(error));
            QJsonArray warnings, notes;
            for (auto& w : summary.warnings) warnings.append(qs(w));
            for (auto& n : summary.notes) notes.append(qs(n));
            return QJsonObject{{"path", path}, {"width", doc.width}, {"height", doc.height}, {"layers", summary.layers}, {"folders", summary.folders},
                               {"masks", summary.masks}, {"clipped", summary.clipped}, {"adjustments", summary.adjustments}, {"texts", summary.texts}, {"smartObjects", summary.smartObjects}, {"warnings", warnings}, {"notes", notes}};
        }
        auto flat = session()->flattened();
        if (!flat) fail("nothing to export");
        if (suffix == "png") {
            std::string error;
            if (!writePngImage(path.toStdString(), *flat, session()->document()->resolution, &error)) fail("couldn't write " + path + ": " + qs(error));
        } else if (suffix == "jpg" || suffix == "jpeg") {
            QImage image(flat->width(), flat->height(), QImage::Format_RGB32);
            image.fill(QColor(str(p, "background", QStringLiteral("#ffffff"))));
            QPainter painter(&image);
            painter.drawImage(0, 0, wrapImage(*flat));
            painter.end();
            QString error;
            if (!writeQtImage(path, "jpeg", image, integer(p, "quality", 85), session()->document()->resolution, &error)) fail("couldn't write " + path + ": " + error);
        } else if ((suffix == "webp" || suffix == "tif" || suffix == "tiff") && canWriteImageFormat(suffix == "webp" ? "webp" : "tiff")) {
            // WebP and TIFF keep transparency; WebP at quality 100 is lossless.
            QString error;
            if (!writeQtImage(path, suffix == "webp" ? "webp" : "tiff", toQImage(*flat), integer(p, "quality", 90), session()->document()->resolution, &error))
                fail("couldn't write " + path + ": " + error);
        } else fail("path must end in .psd, .psb, .png, .jpg, .jpeg, .webp, .tif or .tiff", invalidParams);
        return QJsonObject{{"path", path}, {"width", flat->width()}, {"height", flat->height()}};
    });
    add("document.close", [session](const QJsonObject& p) {
        if (session()->isModified() && !flag(p, "discard", false)) fail("the document has unsaved changes; save first or pass discard: true");
        session()->closeDocument();
        return QJsonObject{};
    });

    // ---- canvas
    add("canvas.resize", [session, document](const QJsonObject& p) {
        document();
        int width = integer(p, "width"), height = integer(p, "height");
        if (!Document::validDimension(width) || !Document::validDimension(height)) fail("width and height must be 1..30000", invalidParams);
        session()->resizeCanvas(width, height, std::clamp(num(p, "anchorX", 0.5), 0.0, 1.0), std::clamp(num(p, "anchorY", 0.5), 0.0, 1.0));
        return QJsonObject{{"width", session()->document()->width}, {"height", session()->document()->height}};
    });
    add("canvas.crop", [session, document](const QJsonObject& p) {
        document();
        session()->cropTo(QRectF(num(p, "x"), num(p, "y"), num(p, "width"), num(p, "height")));
        return QJsonObject{{"width", session()->document()->width}, {"height", session()->document()->height}};
    });
    add("canvas.flip", [session, document](const QJsonObject& p) { document(); session()->flipCanvas(!flag(p, "vertical", false)); return QJsonObject{}; });
    add("image.resize", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        int width = integer(p, "width", 0), height = integer(p, "height", 0);
        if (has(p, "scale")) { double k = num(p, "scale"); width = int(std::lround(doc.width * k)); height = int(std::lround(doc.height * k)); }
        else if (width > 0 && height <= 0) height = int(std::lround(double(width) * doc.height / doc.width));
        else if (height > 0 && width <= 0) width = int(std::lround(double(height) * doc.width / doc.height));
        if (!Document::validDimension(width) || !Document::validDimension(height)) fail("give width and/or height (1..30000) or scale", invalidParams);
        QString sampling = str(p, "sampling", QStringLiteral("high")).toLower();
        int mode = sampling.startsWith("near") ? 0 : sampling.startsWith("smooth") || sampling == "bilinear" ? 1 : 2;
        session()->resizeImage(width, height, num(p, "resolution", doc.resolution), mode);
        return QJsonObject{{"width", session()->document()->width}, {"height", session()->document()->height}};
    });

    // ---- seeing the result
    add("render", [session, document](const QJsonObject& p) {
        // The composite (what an export would give) or a region of it, downscaled so its longest side is maxSize.
        const Document& doc = document();
        Rect region = doc.rect();
        if (has(p, "region")) {
            QJsonObject r = obj(p, "region");
            region = Rect(num(r, "x"), num(r, "y"), num(r, "width"), num(r, "height"));
            region = region.intersection(doc.rect());
            if (region.width < 1 || region.height < 1) fail("region lies outside the document", invalidParams);
        }
        // zoom > 1 renders at full size and enlarges with square pixels, to judge edges and seams exactly.
        const double zoom = num(p, "zoom", 1);
        if (!(zoom >= 1 && zoom <= 32)) fail("zoom must be 1..32", invalidParams);
        if (zoom > 1 && std::max(region.width, region.height) * zoom > 4096) fail(QStringLiteral("region times zoom must stay within 4096 pixels; render a smaller region (at most %1 pixels across)").arg(int(4096 / zoom)), invalidParams);
        double maxSize = zoom > 1 ? 0 : num(p, "maxSize", 1024);
        double scale = maxSize > 0 ? std::min(1.0, maxSize / std::max(region.width, region.height)) : 1.0;
        int w = std::max(1, int(std::lround(region.width * scale))), h = std::max(1, int(std::lround(region.height * scale)));
        Image out(w, h);
        RenderOptions options;
        options.region = region;
        options.scale = scale;
        Overrides overrides = session()->renderOverrides();
        render(doc, options, out, &overrides);
        if (flag(p, "checkerboard", false)) {
            // Transparency over a checkerboard, as the canvas shows it.
            QImage flat(w, h, QImage::Format_RGB32);
            QPainter painter(&flat);
            for (int y = 0; y < h; y += 8) for (int x = 0; x < w; x += 8) painter.fillRect(x, y, 8, 8, ((x / 8 + y / 8) % 2) ? QColor(204, 204, 204) : Qt::white);
            painter.drawImage(0, 0, wrapImage(out));
            painter.end();
            out = *fromQImage(flat);
        }
        if (zoom > 1) {
            out = *fromQImage(wrapImage(out).scaled(int(std::lround(w * zoom)), int(std::lround(h * zoom)), Qt::IgnoreAspectRatio, Qt::FastTransformation));
            scale = zoom;
        }
        return deliverPng(out, p, {{"region", rectJson(region)}, {"scale", scale}});
    });
    add("screenshot", [w, session](const QJsonObject& p) {
        QImage image = (flag(p, "window", false) ? w->grab() : w->canvasAt(w->currentTabIndex())->grab()).toImage();
        double maxSize = num(p, "maxSize", 1600);
        if (maxSize > 0 && std::max(image.width(), image.height()) > maxSize) image = image.scaled(int(maxSize), int(maxSize), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        return deliverPng(*fromQImage(image), p, {{"zoom", session()->viewport.zoom}});
    });

}

} // namespace app
