#include "DocumentSource.h"
#include <QJsonDocument>
#include <algorithm>

namespace app {

QString DocumentSource::kindName() const {
    switch (kind) {
    case Kind::None: return {};
    case Kind::Project: return QStringLiteral("project");
    case Kind::Layered: return QStringLiteral("layered");
    case Kind::Image: return QStringLiteral("image");
    case Kind::Raw: return QStringLiteral("raw");
    }
    return {};
}

QJsonObject DocumentSource::toJson() const {
    if (!valid()) return {};
    QJsonObject o{{"kind", kindName()}, {"path", path}};
    if (mergedOnly) o["mergedOnly"] = true;
    if (kind == Kind::Layered && path.endsWith(".pdf", Qt::CaseInsensitive)) { o["page"] = pdfPage; o["resolution"] = pdfResolution; }
    if (kind == Kind::Raw) {
        o["settings"] = QJsonDocument::fromJson(QByteArray::fromStdString(raw.normalized().toJson())).object();
        o["asSmartObject"] = rawAsObject;
        o["bitsPerChannel"] = rawBits;
    }
    return o;
}

DocumentSource DocumentSource::fromJson(const QJsonObject& json) {
    DocumentSource s;
    const QString kind = json.value("kind").toString();
    s.kind = kind == "project" ? Kind::Project : kind == "layered" ? Kind::Layered : kind == "image" ? Kind::Image : kind == "raw" ? Kind::Raw : Kind::None;
    s.path = json.value("path").toString();
    s.mergedOnly = json.value("mergedOnly").toBool(false);
    s.pdfPage = std::max(1, json.value("page").toInt(1));
    s.pdfResolution = json.value("resolution").toDouble(150);
    if (s.kind == Kind::Raw) {
        const QJsonObject settings = json.value("settings").toObject();
        if (!compositor::CameraRawSettings::parse(QJsonDocument(settings).toJson(QJsonDocument::Compact).toStdString(), s.raw)) s.raw = {};
        s.rawAsObject = json.value("asSmartObject").toBool(false);
        s.rawBits = json.value("bitsPerChannel").toInt(16) == 8 ? 8 : 16;
    }
    if (!s.valid()) return {};
    return s;
}

} // namespace app
