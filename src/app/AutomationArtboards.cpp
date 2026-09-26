// Automation methods: artboards and slices. Registered from AutomationServer::registerHandlers (Automation.cpp).
#include "Automation.h"
#include "AutomationHandlers.h"
#include "MainWindow.h"
#include <QColor>
#include <algorithm>

using namespace compositor;
using namespace app::rpc;

namespace app {

namespace rpc {

QJsonObject artboardJson(const Layer& l) {
    const Artboard& a = *l.artboard;
    QString background = a.background == Artboard::White ? "white" : a.background == Artboard::Black ? "black" : a.background == Artboard::Transparent ? "transparent"
        : QColor::fromRgbF(float(a.red), float(a.green), float(a.blue)).name();
    return QJsonObject{{"id", qs(l.id)}, {"name", qs(l.name)}, {"x", a.x}, {"y", a.y}, {"width", a.width}, {"height", a.height},
                       {"background", background}, {"preset", qs(a.presetName)}, {"visible", l.visible}};
}

} // namespace rpc

namespace {

QJsonObject sliceJson(const Slice& s) {
    return QJsonObject{{"id", int(s.id)}, {"name", qs(s.name)}, {"x", s.x}, {"y", s.y}, {"width", s.width}, {"height", s.height},
                       {"url", qs(s.url)}, {"target", qs(s.target)}, {"altTag", qs(s.altTag)}};
}

void backgroundFrom(const QJsonObject& p, Artboard& a) {
    if (!has(p, "background")) return;
    const QString b = str(p, "background").trimmed().toLower();
    if (b == "white") a.background = Artboard::White;
    else if (b == "black") a.background = Artboard::Black;
    else if (b == "transparent") a.background = Artboard::Transparent;
    else {
        const QColor c(b);
        if (!c.isValid()) fail("background must be white, black, transparent or a CSS colour", invalidParams);
        a.background = Artboard::Other;
        a.red = c.redF(); a.green = c.greenF(); a.blue = c.blueF();
    }
}

void rectFrom(const QJsonObject& p, int& x, int& y, int& width, int& height) {
    if (has(p, "x")) x = integer(p, "x");
    if (has(p, "y")) y = integer(p, "y");
    if (has(p, "width")) width = integer(p, "width");
    if (has(p, "height")) height = integer(p, "height");
    if (width < 1 || height < 1 || width > maxImageSide || height > maxImageSide) fail("width and height must be 1.." + QString::number(maxImageSide), invalidParams);
}

const Layer& artboardOf(const Document& doc, const QJsonObject& p) {
    const Layer* l = doc.find(str(p, "id").toStdString());
    if (!l || !l->artboard) fail("no artboard with that id; artboards.list shows them", invalidParams);
    return *l;
}

const Slice& sliceOf(const Document& doc, const QJsonObject& p) {
    const int id = integer(p, "id");
    for (const Slice& s : doc.slices) if (int(s.id) == id) return s;
    fail("no slice with that id; slices.list shows them", invalidParams);
}

void textFields(const QJsonObject& p, Slice& s) {
    if (has(p, "name")) s.name = str(p, "name").toStdString();
    if (has(p, "url")) s.url = str(p, "url").toStdString();
    if (has(p, "target")) s.target = str(p, "target").toStdString();
    if (has(p, "altTag")) s.altTag = str(p, "altTag").toStdString();
}

QJsonObject exported(const QStringList& files, const QString& error) {
    if (!error.isEmpty()) fail(error);
    QJsonArray list;
    for (const QString& f : files) list.append(f);
    return QJsonObject{{"files", list}, {"count", int(files.size())}};
}

} // namespace

void AutomationServer::registerArtboardHandlers() {
    MainWindow* w = window_;
    const SessionOf session{w};
    const DocumentOf document{w};

    // ---- artboards
    add("artboards.list", [session, document](const QJsonObject&) {
        const Document& doc = document();
        QJsonArray list;
        for (const Layer* l : session()->artboards()) {
            QJsonObject o = artboardJson(*l);
            o["children"] = int(descendantIds(doc.layers, l->id).size());
            list.append(o);
        }
        return QJsonObject{{"artboards", list}};
    });
    add("artboards.add", [session, document](const QJsonObject& p) {
        document();
        Artboard a;
        a.presetName = "Custom";
        rectFrom(p, a.x, a.y, a.width, a.height);
        backgroundFrom(p, a);
        auto id = session()->addArtboard(a, str(p, "name", QString()));
        if (!id) fail("the artboard could not be added");
        return artboardJson(*document().find(*id));
    });
    add("artboards.set", [session, document](const QJsonObject& p) {
        const Layer& l = artboardOf(document(), p);
        Artboard a = *l.artboard;
        rectFrom(p, a.x, a.y, a.width, a.height);
        backgroundFrom(p, a);
        if (!session()->setArtboard(l.id, a, flag(p, "moveContents", true), str(p, "name", QString()))) fail("the artboard could not be changed");
        return artboardJson(*document().find(l.id));
    });
    add("artboards.delete", [session, document](const QJsonObject& p) {
        const Uuid id = artboardOf(document(), p).id;
        if (!session()->removeArtboard(id, flag(p, "contents", false))) fail("the artboard could not be removed");
        return QJsonObject{{"removed", qs(id)}};
    });
    add("artboards.export", [session, document](const QJsonObject& p) {
        document();
        QString error;
        const QStringList files = session()->exportArtboards(str(p, "directory"), str(p, "format", "png"), str(p, "prefix", QString()), integer(p, "quality", 90), &error);
        return exported(files, error);
    });

    // ---- slices
    add("slices.list", [document](const QJsonObject&) {
        QJsonArray list;
        for (const Slice& s : document().slices) list.append(sliceJson(s));
        return QJsonObject{{"slices", list}};
    });
    add("slices.add", [session, document](const QJsonObject& p) {
        document();
        Slice s;
        s.id = 0;
        rectFrom(p, s.x, s.y, s.width, s.height);
        textFields(p, s);
        auto id = session()->addSlice(s);
        if (!id) fail("the slice could not be added");
        for (const Slice& added : document().slices) if (added.id == *id) return sliceJson(added);
        return QJsonObject{};
    });
    add("slices.set", [session, document](const QJsonObject& p) {
        Slice s = sliceOf(document(), p);
        rectFrom(p, s.x, s.y, s.width, s.height);
        textFields(p, s);
        if (!session()->setSlice(s)) fail("the slice could not be changed");
        return sliceJson(s);
    });
    add("slices.delete", [session, document](const QJsonObject& p) {
        const uint32_t id = sliceOf(document(), p).id;
        if (!session()->deleteSlice(id)) fail("the slice could not be deleted");
        return QJsonObject{{"removed", int(id)}};
    });
    add("slices.export", [session, document](const QJsonObject& p) {
        document();
        QString error;
        const QStringList files = session()->exportSlices(str(p, "directory"), str(p, "format", "png"), str(p, "prefix", QString()), integer(p, "quality", 90), &error);
        return exported(files, error);
    });
}

} // namespace app
