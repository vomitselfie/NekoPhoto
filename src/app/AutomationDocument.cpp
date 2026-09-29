// Automation methods: document. Registered from AutomationServer::registerHandlers (Automation.cpp).
#include "compositor/vectorlayer.h"
#include "TextLayer.h"
#include "Automation.h"
#include "ColorManagement.h"
#include "compositor/gif.h"
#include "AutomationHandlers.h"
#include "CanvasWidget.h"
#include "ImageConvert.h"
#include "MainWindow.h"
#include "compositor/ico.h"
#include "compositor/png.h"
#include "compositor/raw.h"
#include "compositor/psd_writer.h"
#include "compositor/tga.h"
#include "compositor/svg.h"
#include "VectorFiles.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
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
                      {"bits", QString::fromLatin1(sampleTypeName(doc.sampleType)).toInt()}, {"colorMode", QString::fromLatin1(colorModeKey(doc.colorMode))},
                      {"modified", s->isModified()}, {"title", s->title()}, {"tab", w->currentTabIndex()},
                      {"profile", color::profileLabel(doc.profile)}};
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
                bits << (l.isLiveText() ? "text" : (l.isLiveShape() || compositor::isVectorShapeLayer(l)) ? "shape" : "pixels");
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
        if (const BudgetCheck check = Document::canCreate(width, height, SampleType::U8); !check) fail(qs(check.message()), invalidParams);
        if (session()->hasDocument()) w->newTab();
        session()->createDocument(width, height, num(p, "resolution", 72), flag(p, "emptyLayer", true));
        session()->adoptProfile(color::newDocumentProfile());
        return QJsonObject{{"tab", w->currentTabIndex()}, {"width", width}, {"height", height}};
    });
    add("document.open", [w, session](const QJsonObject& p) {
        QString path = QFileInfo(str(p, "path")).absoluteFilePath();
        if (!QFileInfo::exists(path)) fail("no such file: " + path, invalidParams);
        const bool pdf = path.endsWith(".pdf", Qt::CaseInsensitive), svg = path.endsWith(".svg", Qt::CaseInsensitive) || path.endsWith(".svgz", Qt::CaseInsensitive);
        if (pdf) {
            // One open's page and resolution (VectorFiles.h); the open resets them.
            app::PdfOpenOptions& options = app::pdfOpenOptions();
            options.page = integer(p, "page", 1);
            options.resolution = num(p, "resolution", 150);
            options.pageGiven = true;
            if (options.page < 1) { app::pdfOpenOptions() = {}; fail("page must be 1 or more", invalidParams); }
            if (options.resolution < 18 || options.resolution > 1200) { app::pdfOpenOptions() = {}; fail("resolution must be 18..1200", invalidParams); }
        } else if (has(p, "page") || has(p, "resolution")) fail("page and resolution apply to PDF files", invalidParams);
        if (compositor::isRawPath(path.toStdString())) {
            // Camera RAW: developed without the dialog (as shot, or with `settings`), into a new tab.
            if (!compositor::rawSupported()) fail("this build cannot open camera RAW files (LibRaw was not found)");
            MainWindow::RawOpenRequest request;
            if (has(p, "settings")) {
                std::string why;
                const QJsonObject given = obj(p, "settings");
                if (!CameraRawSettings::parse(QJsonDocument(given).toJson(QJsonDocument::Compact).toStdString(), request.settings, &why)) fail("settings." + QString::fromStdString(why), invalidParams);
                if (request.settings.whiteBalance == CameraRawWhiteBalance::Auto && !given.contains("temperature") && !given.contains("tint")) {
                    std::string ignored;
                    if (auto solved = compositor::rawAutoBalance(compositor::readRawFileBytes(path.toStdString(), &ignored))) {
                        request.settings.temperature = (*solved)[0];
                        request.settings.tint = (*solved)[1];
                    }
                }
            }
            request.asSmartObject = flag(p, "asSmartObject", false);
            request.bitsPerChannel = integer(p, "bitsPerChannel", 16);
            if (request.bitsPerChannel != 8 && request.bitsPerChannel != 16) fail("bitsPerChannel must be 8 or 16", invalidParams);
            QString error;
            if (!w->openRawFile(path, &error, &request)) fail(error.isEmpty() ? QStringLiteral("the RAW file could not be opened") : error);
            EditorSession* s = session();
            const QJsonObject applied = QJsonDocument::fromJson(QByteArray::fromStdString(request.settings.normalized().toJson())).object();
            return QJsonObject{{"tab", w->currentTabIndex()}, {"title", s->title()}, {"width", s->document()->width}, {"height", s->document()->height},
                               {"layers", int(s->document()->layers.size())}, {"bitsPerChannel", request.bitsPerChannel}, {"smartObject", request.asSmartObject},
                               {"settings", applied}};
        }
        if (has(p, "settings") || has(p, "asSmartObject") || has(p, "bitsPerChannel")) fail("settings, asSmartObject and bitsPerChannel apply to camera RAW files", invalidParams);
        const bool psd = path.endsWith(".psd", Qt::CaseInsensitive) || path.endsWith(".psb", Qt::CaseInsensitive);
        if (has(p, "mergedOnly") && !psd) fail("mergedOnly applies to .psd and .psb files", invalidParams);
        if (psd) w->nextPsdMergedOnly = flag(p, "mergedOnly", false);
        const EditorSession* before = session();
        const std::string beforeDocument = before->hasDocument() ? before->document()->id : std::string();
        w->openPath(path);
        EditorSession* s = session();
        QJsonObject out{{"tab", w->currentTabIndex()}, {"title", s->title()}, {"width", s->hasDocument() ? s->document()->width : 0}, {"height", s->hasDocument() ? s->document()->height : 0}};
        if (pdf || svg) {
            app::pdfOpenOptions() = {};
            if (!s->hasDocument() || (s == before && s->document()->id == beforeDocument)) fail(pdf ? (app::pdfSupported() ? "the PDF could not be opened" : "this build cannot open PDF files (Qt PDF was not found)") : "the SVG could not be opened");
            out["layers"] = int(s->document()->layers.size());
            out["notes"] = QJsonArray::fromStringList(w->lastImportNotes());
        } else if (isLayeredPath(path)) {
            if (!s->hasDocument()) fail("the file could not be imported: " + QFileInfo(path).fileName());
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
        session()->endTemporaryLayers();   // the Quick Mask and filter-mask layers are never written
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
        const bool deep = doc.sampleType == SampleType::U16;
        // A 32-bit document goes to 16 bits (PNG, TIFF) or 8 tone-mapped at exposure 0: values above white clip.
        const bool floatDocument = doc.sampleType == SampleType::F32;
        const QString toneNote = QStringLiteral("tone-mapped from 32 bits per channel at exposure 0 (values above white are clipped)");
        // The document's profile is embedded (PNG, JPEG, WebP, TIFF); convertToSrgb converts instead, as the web formats
        // want (the default for GIF, which has no profile).
        const bool gif = suffix == "gif";
        const color::ExportPlan plan = color::exportPlan(doc, flag(p, "convertToSrgb", gif), flag(p, "embedProfile", true));
        if (suffix == "svg" && floatDocument) fail("SVG export is not available for 32-bit documents yet");
        if (suffix == "svg") {
            // Shape layers as paths, folders as groups, the rest as embedded PNGs (compositor/svg.h).
            SvgExportSummary summary;
            std::string error;
            if (!compositor::exportSvg(doc, path.toStdString(), &summary, &error)) fail("couldn't write " + path + ": " + qs(error));
            QJsonArray notes;
            for (auto& n : summary.notes) notes.append(qs(n));
            return QJsonObject{{"path", path}, {"width", doc.width}, {"height", doc.height}, {"shapes", summary.shapes}, {"images", summary.images}, {"groups", summary.groups}, {"notes", notes},
                               {"bits", deep ? 16 : 8}};
        }
        if (suffix == "gif") {
            // The timeline's frames as an animated GIF, or the composite as a still one.
            session()->endFramePreview();
            std::string error;
            std::optional<Document> converted;
            if (plan.convert) { converted = document(); convertDocumentProfile(*converted, {}, {}); }
            if (!writeDocumentGif(path.toStdString(), converted ? *converted : document(), &error)) fail("couldn't write " + path + ": " + qs(error));
            const Document& shown = document();
            QJsonObject out{{"path", path}, {"width", shown.width}, {"height", shown.height}, {"frames", std::max(1, int(shown.animation.frames.size()))}, {"bits", 8}};
            if (deep) out["note"] = "reduced from 16 to 8 bits per channel with dithering";
            if (floatDocument) out["note"] = toneNote;
            return out;
        }
        // A 16-bit document as a 16-bit PNG (and TIFF, when Qt writes one); the 8-bit formats get it dithered down.
        if ((deep || floatDocument) && (suffix == "png" || ((suffix == "tif" || suffix == "tiff") && canWriteDeepTiff()))) {
            auto image = color::flatten16(doc, plan);
            if (!image) fail("nothing to export");
            std::string error;
            if (suffix == "png") { if (!writePngImage16(path.toStdString(), *image, doc.resolution, &error, plan.icc.empty() ? nullptr : &plan.icc)) fail("couldn't write " + path + ": " + qs(error)); }
            else { QString qerror; if (!writeQtImage(path, "tiff", toQImage16(*image), 100, doc.resolution, &qerror, plan.iccBytes())) fail("couldn't write " + path + ": " + qerror); }
            QJsonObject out{{"path", path}, {"width", image->width()}, {"height", image->height()}, {"bits", 16}};
            if (floatDocument) out["note"] = toneNote;
            return out;
        }
        auto flat = color::flatten8(doc, plan);
        if (!flat) fail("nothing to export");
        if (suffix == "png") {
            std::string error;
            if (!writePngImage(path.toStdString(), *flat, session()->document()->resolution, &error, plan.icc.empty() ? nullptr : &plan.icc)) fail("couldn't write " + path + ": " + qs(error));
        } else if (suffix == "jpg" || suffix == "jpeg") {
            QImage image(flat->width(), flat->height(), QImage::Format_RGB32);
            image.fill(QColor(str(p, "background", QStringLiteral("#ffffff"))));
            QPainter painter(&image);
            painter.drawImage(0, 0, wrapImage(*flat));
            painter.end();
            QString error;
            if (!writeQtImage(path, "jpeg", image, integer(p, "quality", 85), session()->document()->resolution, &error, plan.iccBytes())) fail("couldn't write " + path + ": " + error);
        } else if ((suffix == "webp" || suffix == "tif" || suffix == "tiff") && canWriteImageFormat(suffix == "webp" ? "webp" : "tiff")) {
            // WebP and TIFF keep transparency; WebP at quality 100 is lossless.
            QString error;
            if (!writeQtImage(path, suffix == "webp" ? "webp" : "tiff", toQImage(*flat), integer(p, "quality", 90), session()->document()->resolution, &error, plan.iccBytes()))
                fail("couldn't write " + path + ": " + error);
        } else if (suffix == "tga") {
            std::string error;
            if (!writeTgaImage(path.toStdString(), *flat, &error)) fail("couldn't write " + path + ": " + qs(error));
        } else if (suffix == "ico") {
            std::string error;
            if (!writeIco(path.toStdString(), *flat, defaultIcoSizes, &error, &doc)) fail("couldn't write " + path + ": " + qs(error));
        } else fail("path must end in .psd, .psb, .svg, .png, .jpg, .jpeg, .webp, .tif, .tiff, .gif, .tga or .ico", invalidParams);
        QJsonObject out{{"path", path}, {"width", flat->width()}, {"height", flat->height()}, {"bits", 8}, {"profile", plan.icc.empty() ? QJsonValue::Null : QJsonValue(QString::fromStdString(encodedProfileOf(doc).description))}, {"convertedToSrgb", plan.convert}};
        if (deep) out["note"] = "reduced from 16 to 8 bits per channel with dithering";
        if (floatDocument) out["note"] = toneNote;
        return out;
    });
    add("document.profile", [session, document](const QJsonObject& p) {
        // Edit > Assign Profile (the tag only) and Convert to Profile (pixels and stored colours); one undo step each.
        document();
        const QString action = str(p, "action", QStringLiteral("get"));
        if (action != "get" && action != "assign" && action != "convert") fail("action must be get, assign or convert", invalidParams);
        if (action != "get") {
            if (!has(p, "profile")) fail("give profile: srgb, adobe-rgb, display-p3, prophoto, working, working-cmyk, none (assign only) or an ICC file's path", invalidParams);
            QString key = str(p, "profile"), error;
            // A document takes profiles of its own mode: RGB ones, or CMYK ones in a CMYK document. Lab documents are in
            // Lab D50 and have no other profile.
            const ColorMode mode = document().colorMode;
            if (mode == ColorMode::Lab) fail("a Lab document's values are Lab D50; it takes no other profile", invalidParams);
            const color::ProfileKinds kinds = mode == ColorMode::CMYK ? color::ProfileKinds::CMYK : color::ProfileKinds::RGB;
            if (key == "working") key = mode == ColorMode::CMYK ? QStringLiteral("working-cmyk") : QString::fromLatin1(workingSpaceKey(color::settings().workingSpace));
            if (key == "none" && action == "convert") key = mode == ColorMode::CMYK ? QStringLiteral("working-cmyk") : QStringLiteral("srgb");
            auto profile = color::profileForKey(key, &error, kinds);
            if (!profile) fail(error, invalidParams);
            if (action == "assign") session()->assignProfile(*profile);
            else {
                ConvertOptions options;
                auto intent = renderingIntentFromKey(str(p, "intent", QStringLiteral("relative")).toStdString());
                if (!intent || (*intent != RenderingIntent::Perceptual && *intent != RenderingIntent::RelativeColorimetric)) fail("intent must be perceptual or relative", invalidParams);
                options.intent = *intent;
                options.blackPointCompensation = flag(p, "blackPointCompensation", true);
                if (!session()->convertToProfile(*profile, options, &error)) fail(error);
            }
        } else if (has(p, "profile") || has(p, "intent") || has(p, "blackPointCompensation")) fail("profile, intent and blackPointCompensation go with assign or convert", invalidParams);
        const Document& doc = document();
        auto space = matchingWorkingSpace(doc.profile);
        return QJsonObject{{"profile", color::profileLabel(doc.profile)}, {"tagged", !doc.profile.empty()},
                           {"workingSpace", space ? QJsonValue(QString::fromLatin1(workingSpaceKey(*space))) : QJsonValue::Null},
                           {"bytes", int(doc.profile.icc.size())}, {"undo", session()->canUndo() ? QJsonValue(session()->undoName()) : QJsonValue::Null}};
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
        if (const BudgetCheck check = Document::canCreate(width, height, document().sampleType); !check) fail(qs(check.message()), invalidParams);
        session()->resizeCanvas(width, height, std::clamp(num(p, "anchorX", 0.5), 0.0, 1.0), std::clamp(num(p, "anchorY", 0.5), 0.0, 1.0));
        return QJsonObject{{"width", session()->document()->width}, {"height", session()->document()->height}};
    });
    add("canvas.crop", [session, document](const QJsonObject& p) {
        document();
        QRectF rect(num(p, "x"), num(p, "y"), num(p, "width"), num(p, "height"));
        if (has(p, "ratio")) {
            // The Crop tool's ratio: "16:9", "16x9" or a number (width / height); the largest box of that shape
            // centred in the rectangle.
            const QJsonValue v = p.value("ratio");
            double ratio = v.toDouble(0);
            if (v.isString()) {
                const QStringList parts = v.toString().split(QRegularExpression(QStringLiteral("\\s*[:x×/]\\s*")));
                bool ok1 = false, ok2 = false;
                ratio = parts.size() == 2 ? parts[0].toDouble(&ok1) / parts[1].toDouble(&ok2) : v.toString().toDouble(&ok1);
                if (!ok1 || (parts.size() == 2 && !ok2)) ratio = 0;
            }
            if (!(ratio > 0) || !std::isfinite(ratio)) fail("ratio must be W:H (such as 16:9) or a positive number", invalidParams);
            rect = CanvasWidget::fitCropRatio(rect, ratio);
        }
        session()->cropTo(rect);
        return QJsonObject{{"width", session()->document()->width}, {"height", session()->document()->height}};
    });
    add("canvas.flip", [session, document](const QJsonObject& p) { document(); session()->flipCanvas(!flag(p, "vertical", false)); return QJsonObject{}; });
    add("image.mode", [session, document](const QJsonObject& p) {
        // Image > Mode > 8, 16 or 32 Bits/Channel: every layer, mask and the selection converted, one undo step, from 32
        // bits with HDR Toning's settings (the defaults: the values as they are); and RGB Color, CMYK Color or Lab Color
        // (docs/color-modes.md), its own undo step: before the depth, or after it when leaving 32 bits (CMYK and Lab
        // have no 32 bits).
        document();
        if (!has(p, "bits") && !has(p, "colorMode")) fail("give bits (8, 16 or 32) and/or colorMode (rgb, cmyk or lab)", invalidParams);
        View32 toning;
        if (has(p, "method")) {
            auto method = toneMethodFromKey(str(p, "method").toStdString());
            if (!method) fail("method must be exposure-gamma or highlight-compression", invalidParams);
            toning.method = *method;
        }
        if (has(p, "exposure")) toning.exposure = num(p, "exposure");
        if (has(p, "gamma")) toning.gamma = num(p, "gamma");
        if (!(toning.exposure >= View32::minExposure && toning.exposure <= View32::maxExposure)) fail("exposure must be -20..20", invalidParams);
        if (!(toning.gamma >= View32::minGamma && toning.gamma <= View32::maxGamma)) fail("gamma must be 0.1..9.99", invalidParams);
        if (!toning.isDefault() && (!has(p, "bits") || document().sampleType != SampleType::F32)) fail("method, exposure and gamma are HDR Toning's: they apply from 32 bits", invalidParams);
        QString error;
        auto convertDepth = [&] {
            const int bits = integer(p, "bits");
            if (bits != 8 && bits != 16 && bits != 32) fail("bits must be 8, 16 or 32", invalidParams);
            const SampleType target = bits == 32 ? SampleType::F32 : bits == 16 ? SampleType::U16 : SampleType::U8;
            if (!session()->convertMode(target, &error, toning.isDefault() ? nullptr : &toning)) fail(error.isEmpty() ? QStringLiteral("the document could not be converted") : error);
        };
        const bool depthFirst = has(p, "bits") && document().sampleType == SampleType::F32;
        if (depthFirst) convertDepth();
        if (has(p, "colorMode")) {
            const QString key = str(p, "colorMode").toLower();
            const std::optional<ColorMode> mode = key == "rgb" ? std::optional(ColorMode::RGB) : key == "cmyk" ? std::optional(ColorMode::CMYK)
                                                  : key == "lab" ? std::optional(ColorMode::Lab) : std::nullopt;
            if (!mode) fail("colorMode must be rgb, cmyk or lab", invalidParams);
            if (!session()->convertColorMode(*mode, &error)) fail(error.isEmpty() ? QStringLiteral("the document could not be converted") : error);
        }
        if (has(p, "bits") && !depthFirst) convertDepth();
        const Document& now = document();
        const int bits = now.sampleType == SampleType::U16 ? 16 : now.sampleType == SampleType::F32 ? 32 : 8;
        return QJsonObject{{"bits", bits}, {"colorMode", QString::fromLatin1(colorModeKey(now.colorMode))}, {"width", now.width}, {"height", now.height}, {"layerBytes", double(now.layerBytes())},
                           {"layerBudgetBytes", double(Document::projectPixelBudgetAt(now.sampleType) * 4 * (long long)sampleBytes(now.sampleType))},
                           {"layerPixelBudget", double(Document::projectPixelBudgetAt(now.sampleType))}};
    });
    add("image.resize", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        int width = integer(p, "width", 0), height = integer(p, "height", 0);
        if (has(p, "scale")) { double k = num(p, "scale"); width = int(std::lround(doc.width * k)); height = int(std::lround(doc.height * k)); }
        else if (width > 0 && height <= 0) height = int(std::lround(double(width) * doc.height / doc.width));
        else if (height > 0 && width <= 0) width = int(std::lround(double(height) * doc.width / doc.height));
        if (!Document::validDimension(width) || !Document::validDimension(height)) fail("give width and/or height (1..30000) or scale", invalidParams);
        if (const BudgetCheck check = Document::canCreate(width, height, doc.sampleType); !check) fail(qs(check.message()), invalidParams);
        QString sampling = str(p, "sampling", QStringLiteral("high")).toLower();
        int mode = sampling.startsWith("near") ? 0 : sampling.startsWith("smooth") || sampling == "bilinear" ? 1 : 2;
        session()->resizeImage(width, height, num(p, "resolution", doc.resolution), mode);
        return QJsonObject{{"width", session()->document()->width}, {"height", session()->document()->height}};
    });
    add("image.trim", [session](const QJsonObject& p) {
        TrimOptions o;
        const QString by = str(p, "basedOn", QStringLiteral("transparent")).toLower();
        if (by.startsWith("transparent")) o.basedOn = TrimOptions::BasedOn::TransparentPixels;
        else if (by.startsWith("topleft") || by.startsWith("top left") || by.startsWith("top-left")) o.basedOn = TrimOptions::BasedOn::TopLeftColor;
        else if (by.startsWith("bottomright") || by.startsWith("bottom right") || by.startsWith("bottom-right")) o.basedOn = TrimOptions::BasedOn::BottomRightColor;
        else fail("basedOn must be transparent, topLeft or bottomRight", invalidParams);
        o.top = !has(p, "top") || flag(p, "top", true);
        o.bottom = !has(p, "bottom") || flag(p, "bottom", true);
        o.left = !has(p, "left") || flag(p, "left", true);
        o.right = !has(p, "right") || flag(p, "right", true);
        o.tolerance = uint8_t(std::clamp(integer(p, "tolerance", 0), 0, 255));
        const bool trimmed = session()->trim(o);
        return QJsonObject{{"trimmed", trimmed}, {"width", session()->document()->width}, {"height", session()->document()->height}};
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
