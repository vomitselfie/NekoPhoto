// EditorSession: artboards (folders with a rectangle and a background) and slices (named export rectangles).
#include "EditorSession.h"
#include "ImageConvert.h"
#include "compositor/depth.h"
#include "compositor/png.h"
#include "compositor/render.h"
#include <QDir>
#include <QPainter>
#include <algorithm>

using namespace compositor;

namespace app {

namespace {

bool validRect(int x, int y, int w, int h) {
    return w >= 1 && h >= 1 && w <= maxImageSide && h <= maxImageSide && std::abs(x) <= maxImageSide * 2 && std::abs(y) <= maxImageSide * 2;
}

/// A file name from a layer or slice name: what a file system takes, never empty.
QString fileSafe(const QString& name, const QString& fallback) {
    QString out;
    for (QChar c : name) out += (c.isLetterOrNumber() || c == '-' || c == '_' || c == ' ' || c == '.') ? c : QChar('_');
    out = out.trimmed();
    while (out.startsWith('.')) out.remove(0, 1);
    return out.isEmpty() ? fallback : out;
}

} // namespace

std::vector<const Layer*> EditorSession::artboards() const {
    std::vector<const Layer*> out;
    if (!document_) return out;
    for (const Layer& l : document_->layers) if (l.isGroup && l.artboard) out.push_back(&l);
    return out;
}

std::optional<Uuid> EditorSession::addArtboard(const Artboard& artboard, const QString& name) {
    if (refusedAtDepth("edit.artboard", tr("Artboards"))) return std::nullopt;
    if (!canEditLayers() || document_->layers.size() >= size_t(Document::maxLayers)) return std::nullopt;
    if (!validRect(artboard.x, artboard.y, artboard.width, artboard.height) || artboard.background < 1 || artboard.background > 4) return std::nullopt;
    Layer group(name.isEmpty() ? nextLayerName(document_->layers, QCoreApplication::translate("Names", "Artboard").toStdString()) : name.toStdString(), document_->size());
    group.isGroup = true;
    group.artboard = artboard;
    commitTransform();
    beginEdit(QT_TRANSLATE_NOOP("History", "New Artboard"));
    document_->layers.push_back(group);   // artboards sit at the top level, as in Photoshop
    setActiveLayer(group.id);
    endEdit();
    notifyDocument();
    return group.id;
}

bool EditorSession::setArtboard(const Uuid& id, const Artboard& artboard, bool moveContents, const QString& name) {
    if (refusedAtDepth("edit.artboard", tr("Artboards"))) return false;
    if (!canEditLayers()) return false;
    Layer* group = document_->find(id);
    if (!group || !group->isGroup || !group->artboard) return false;
    if (!validRect(artboard.x, artboard.y, artboard.width, artboard.height) || artboard.background < 1 || artboard.background > 4) return false;
    if (*group->artboard == artboard && (name.isEmpty() || name.toStdString() == group->name)) return true;
    const int dx = artboard.x - group->artboard->x, dy = artboard.y - group->artboard->y;
    commitTransform();
    beginEdit(group->artboard->width == artboard.width && group->artboard->height == artboard.height && (dx || dy) ? QT_TRANSLATE_NOOP("History", "Move Artboard") : QT_TRANSLATE_NOOP("History", "Edit Artboard"));
    group = document_->find(id);
    group->artboard = artboard;
    if (!name.isEmpty()) group->name = name.toStdString();
    if (moveContents && (dx || dy)) {
        // Everything inside goes along, nested folders and their masks too, and the artboard folder's own mask.
        std::set<Uuid> moving = descendantIds(document_->layers, id);
        moving.insert(id);
        translateLayers(*document_, moving, dx, dy);
    }
    endEdit();
    notifyDocument();
    return true;
}

bool EditorSession::removeArtboard(const Uuid& id, bool contents) {
    if (refusedAtDepth("edit.artboard", tr("Artboards"))) return false;
    if (!canEditLayers()) return false;
    const Layer* group = document_->find(id);
    if (!group || !group->isGroup || !group->artboard) return false;
    if (contents) { deleteLayersResolvingClipping({id}, false); return !document_->find(id); }
    commitTransform();
    beginEdit(QT_TRANSLATE_NOOP("History", "Convert Artboard to Folder"));
    document_->find(id)->artboard.reset();
    endEdit();
    notifyDocument();
    return true;
}

std::optional<Uuid> EditorSession::artboardAt(QPointF p) const {
    if (!document_) return std::nullopt;
    for (auto it = document_->layers.rbegin(); it != document_->layers.rend(); ++it) {
        if (!it->isGroup || !it->artboard) continue;
        const Artboard& a = *it->artboard;
        if (p.x() >= a.x && p.y() >= a.y && p.x() < a.x + a.width && p.y() < a.y + a.height) return it->id;
    }
    return std::nullopt;
}

std::optional<uint32_t> EditorSession::addSlice(Slice slice) {
    if (!canEditLayers() || !validRect(slice.x, slice.y, slice.width, slice.height) || document_->slices.size() >= 10000) return std::nullopt;
    if (slice.id == 0 || std::any_of(document_->slices.begin(), document_->slices.end(), [&](const Slice& s) { return s.id == slice.id; })) slice.id = nextSliceId(document_->slices);
    if (slice.name.empty()) slice.name = "slice_" + std::to_string(slice.id);
    beginEdit(QT_TRANSLATE_NOOP("History", "New Slice"));
    document_->slices.push_back(slice);
    endEdit();
    notifyDocument();
    return slice.id;
}

bool EditorSession::setSlice(const Slice& slice) {
    if (!canEditLayers() || !validRect(slice.x, slice.y, slice.width, slice.height)) return false;
    auto it = std::find_if(document_->slices.begin(), document_->slices.end(), [&](const Slice& s) { return s.id == slice.id; });
    if (it == document_->slices.end()) return false;
    if (*it == slice) return true;
    const size_t index = size_t(it - document_->slices.begin());
    beginEdit(QT_TRANSLATE_NOOP("History", "Edit Slice"));
    document_->slices[index] = slice;
    endEdit();
    notifyDocument();
    return true;
}

bool EditorSession::deleteSlice(uint32_t id) {
    if (!canEditLayers()) return false;
    auto it = std::find_if(document_->slices.begin(), document_->slices.end(), [&](const Slice& s) { return s.id == id; });
    if (it == document_->slices.end()) return false;
    const size_t index = size_t(it - document_->slices.begin());
    beginEdit(QT_TRANSLATE_NOOP("History", "Delete Slice"));
    document_->slices.erase(document_->slices.begin() + std::ptrdiff_t(index));
    endEdit();
    notifyDocument();
    return true;
}

// ---- Guides -----------------------------------------------------------------------------------------------

namespace {
const std::vector<Guide> noGuides;
}

const std::vector<Guide>& EditorSession::guides() const { return document_ ? document_->guides : noGuides; }

std::optional<int> EditorSession::addGuide(Guide guide) {
    if (!document_ || document_->guides.size() >= maxGuides || !std::isfinite(guide.position)) return std::nullopt;
    guide.position = guidePosition(guide.position);
    beginEdit(QT_TRANSLATE_NOOP("History", "New Guide"));
    document_->guides.push_back(guide);
    endEdit();
    documentRevision_++;
    emit guidesChanged(); emit historyChanged(); emit titleChanged();
    return int(document_->guides.size()) - 1;
}

bool EditorSession::moveGuide(int index, double position) {
    if (!document_ || index < 0 || size_t(index) >= document_->guides.size() || !std::isfinite(position)) return false;
    position = guidePosition(position);
    if (document_->guides[size_t(index)].position == position) return true;
    beginEdit(QT_TRANSLATE_NOOP("History", "Move Guide"));
    document_->guides[size_t(index)].position = position;
    endEdit();
    documentRevision_++;
    emit guidesChanged(); emit historyChanged(); emit titleChanged();
    return true;
}

bool EditorSession::removeGuide(int index) {
    if (!document_ || index < 0 || size_t(index) >= document_->guides.size()) return false;
    beginEdit(QT_TRANSLATE_NOOP("History", "Delete Guide"));
    document_->guides.erase(document_->guides.begin() + index);
    endEdit();
    documentRevision_++;
    emit guidesChanged(); emit historyChanged(); emit titleChanged();
    return true;
}

bool EditorSession::clearGuides() {
    if (!document_ || document_->guides.empty()) return false;
    beginEdit(QT_TRANSLATE_NOOP("History", "Clear Guides"));
    document_->guides.clear();
    endEdit();
    documentRevision_++;
    emit guidesChanged(); emit historyChanged(); emit titleChanged();
    return true;
}

std::optional<int> EditorSession::guideAt(QPointF p, double tolerance) const {
    if (!document_) return std::nullopt;
    std::optional<int> best;
    double bestDistance = tolerance;
    for (size_t i = 0; i < document_->guides.size(); i++) {
        const Guide& g = document_->guides[i];
        const double d = std::abs((g.vertical() ? p.x() : p.y()) - g.position);
        if (d <= bestDistance) { bestDistance = d; best = int(i); }
    }
    return best;
}

std::shared_ptr<Image> EditorSession::renderRect(const QRect& rect) const {
    if (!document_) return nullptr;
    const QRect r = rect.intersected(QRect(0, 0, document_->width, document_->height));
    if (r.isEmpty()) return nullptr;
    auto out = std::make_shared<Image>(r.width(), r.height());
    RenderOptions options;
    options.region = Rect(r.x(), r.y(), r.width(), r.height());
    render(*document_, options, *out);
    return out;
}

std::shared_ptr<Image16> EditorSession::renderRect16(const QRect& rect) const {
    if (!document_) return nullptr;
    const QRect r = rect.intersected(QRect(0, 0, document_->width, document_->height));
    if (r.isEmpty()) return nullptr;
    auto out = std::make_shared<Image16>(r.width(), r.height());
    RenderOptions options;
    options.region = Rect(r.x(), r.y(), r.width(), r.height());
    render16(*document_, options, *out);
    return out;
}

namespace {

bool writeOne(const Image& image, const QString& path, const QString& format, int quality, double dpi, QString* error) {
    if (format == "png") {
        std::string why;
        if (!writePngImage(path.toStdString(), image, dpi, &why)) { if (error) *error = QString::fromStdString(why); return false; }
        return true;
    }
    // WebP and TIFF keep transparency (WebP at quality 100 is lossless).
    if (format == "webp" || format == "tiff") return writeQtImage(path, format == "webp" ? "webp" : "tiff", toQImage(image), quality, dpi, error);
    // JPEG: over white, as Photoshop's export mattes transparency.
    QImage source = toQImage(image);
    QImage flat(source.size(), QImage::Format_RGB32);
    flat.fill(Qt::white);
    QPainter p(&flat);
    p.drawImage(0, 0, source);
    p.end();
    return writeQtImage(path, "jpeg", flat, quality, dpi, error);
}

QStringList exportRects(const EditorSession& session, const std::vector<std::pair<QString, QRect>>& items, const QString& directory,
                        const QString& format, const QString& prefix, int quality, QString* error, const std::function<QString(const QString&)>& check) {
    QStringList written;
    const QString fmt = format.toLower() == "jpg" ? QStringLiteral("jpeg") : format.toLower();
    const QString fmt2 = fmt == "tif" ? QStringLiteral("tiff") : fmt;
    if (fmt2 != "png" && fmt2 != "jpeg" && fmt2 != "webp" && fmt2 != "tiff") { if (error) *error = QObject::tr("The format must be png, jpeg, webp or tiff."); return written; }
    if ((fmt2 == "webp" || fmt2 == "tiff") && !canWriteImageFormat(fmt2 == "webp" ? "webp" : "tiff")) {
        if (error) *error = QObject::tr("This system has no %1 writer.").arg(fmt2.toUpper());
        return written;
    }
    // The names first, so a refused one stops the export before anything is written. The prefix is cleaned as the
    // names are: no folders, no leading dots.
    const QString safePrefix = prefix.isEmpty() ? QString() : fileSafe(prefix, QString());
    const QString extension = fmt2 == "png" ? ".png" : fmt2 == "jpeg" ? ".jpg" : fmt2 == "webp" ? ".webp" : ".tif";
    QStringList used, paths;
    for (const auto& item : items) {
        QString base = safePrefix + fileSafe(item.first, QStringLiteral("untitled"));
        QString candidate = base;
        for (int n = 2; used.contains(candidate, Qt::CaseInsensitive); n++) candidate = base + "-" + QString::number(n);
        used << candidate;
        paths << QDir(directory).filePath(candidate + extension);
        if (check) {
            if (const QString why = check(paths.back()); !why.isEmpty()) { if (error) *error = why; return written; }
        }
    }
    if (!QDir().mkpath(directory)) { if (error) *error = QObject::tr("Could not create %1.").arg(directory); return written; }
    // A 16-bit document: PNG (and TIFF, where Qt writes 16 bits) at 16 bits, the rest dithered down to 8. A CMYK or Lab
    // document's areas come out in sRGB, through its profile, as its flat exports do.
    const bool deep = session.sampleType() == SampleType::U16;
    const bool deepFile = deep && (fmt2 == "png" || (fmt2 == "tiff" && canWriteDeepTiff()));
    for (size_t i = 0; i < items.size(); i++) {
        const QRect& rect = items[i].second;
        std::shared_ptr<Image16> image16 = deep ? session.renderRect16(rect) : nullptr;
        std::shared_ptr<Image> image = deep ? (image16 && !deepFile ? ditherToEightBit(*image16) : nullptr) : session.renderRect(rect);
        if (!image && !image16) continue;
        const QString& path = paths[qsizetype(i)];
        if (deepFile && fmt2 == "png") {
            std::string why;
            if (!writePngImage16(path.toStdString(), *image16, session.document()->resolution, &why)) { if (error) *error = QString::fromStdString(why); return written; }
        } else if (deepFile) {
            if (!writeQtImage(path, "tiff", toQImage16(*image16), 100, session.document()->resolution, error)) return written;
        } else if (!writeOne(*image, path, fmt2, quality, session.document()->resolution, error)) return written;
        written << path;
    }
    return written;
}

} // namespace

QStringList EditorSession::exportArtboards(const QString& directory, const QString& format, const QString& prefix, int quality, QString* error,
                                           const std::function<QString(const QString&)>& check) {
    std::vector<std::pair<QString, QRect>> items;
    for (const Layer* l : artboards()) {
        if (!l->visible) continue;
        items.push_back({QString::fromStdString(l->name), QRect(l->artboard->x, l->artboard->y, l->artboard->width, l->artboard->height)});
    }
    if (items.empty()) { if (error) *error = tr("The document has no visible artboards."); return {}; }
    return exportRects(*this, items, directory, format, prefix, quality, error, check);
}

QStringList EditorSession::exportSlices(const QString& directory, const QString& format, const QString& prefix, int quality, QString* error,
                                       const std::function<QString(const QString&)>& check) {
    if (!document_) return {};
    std::vector<std::pair<QString, QRect>> items;
    for (const Slice& s : document_->slices) items.push_back({QString::fromStdString(s.name.empty() ? "slice_" + std::to_string(s.id) : s.name), QRect(s.x, s.y, s.width, s.height)});
    if (items.empty()) { if (error) *error = tr("The document has no slices."); return {}; }
    return exportRects(*this, items, directory, format, prefix, quality, error, check);
}

} // namespace app
