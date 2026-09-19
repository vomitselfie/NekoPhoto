#include "ModelStore.h"
#include "compositor/subject.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>

namespace app {

const std::vector<ModelInfo>& ModelStore::models() {
    static const std::vector<ModelInfo> list{
        {"isnet", "isnet-general-use.onnx", "https://github.com/danielgatis/rembg/releases/download/v0.0.0/isnet-general-use.onnx",
         "60920e99c45464f2ba57bee2ad08c919a52bbf852739e96947fbb4358c0d964a", 178648008, QObject::tr("IS-Net (general use)"),
         QObject::tr("Best quality of the permissively licensed models; keeps hair and whiskers. 1024 px input, about half a second per image.")},
        {"u2net_human_seg", "u2net_human_seg.onnx", "https://github.com/danielgatis/rembg/releases/download/v0.0.0/u2net_human_seg.onnx",
         "01eb6a29a5c4d8edb30b56adad9bb3a2a0535338e480724a213e0acfd2d1c73c", 175997641, QObject::tr("U2Net portrait"),
         QObject::tr("Trained on people; the better choice for portraits and full-body shots. 320 px input, about a quarter of a second per image.")},
        {"u2netp", "u2netp.onnx", "https://github.com/danielgatis/rembg/releases/download/v0.0.0/u2netp.onnx",
         "309c8469258dda742793dce0ebea8e6dd393174f89934733ecc8b14c76f4ddd8", 4574861, QObject::tr("U2Net small"),
         QObject::tr("A tiny model with coarser edges; quick to download and run.")},
        {"pphumanseg", "human_segmentation_pphumanseg_2023mar.onnx", "https://github.com/opencv/opencv_zoo/raw/main/models/human_segmentation_pphumanseg/human_segmentation_pphumanseg_2023mar.onnx",
         "552d8a984054e59b5d773d24b9b12022b22046ceb2bbc4c9aaeaceb36a9ddf24", 6163938, QObject::tr("PP-HumanSeg (instant)"),
         QObject::tr("Baidu's people segmenter from OpenCV's model zoo: a coarse mask in a few milliseconds. When downloaded it also shows an instant preview while a slower model runs.")},
    };
    return list;
}

const ModelInfo* ModelStore::modelById(const QString& id) {
    for (auto& m : models()) if (m.id == id) return &m;
    return nullptr;
}

bool ModelStore::enabled() { return QSettings().value("ai/removeBackground", false).toBool(); }
void ModelStore::setEnabled(bool on) { QSettings().setValue("ai/removeBackground", on); }

const ModelInfo& ModelStore::selected() {
    const ModelInfo* m = modelById(QSettings().value("ai/model", "isnet").toString());
    return m ? *m : models().front();
}

void ModelStore::setSelected(const QString& id) { if (modelById(id)) QSettings().setValue("ai/model", id); }

bool ModelStore::supported() { return compositor::subjectModelSupported(); }
bool ModelStore::ready() { return enabled() && supported() && isPresent(selected()); }

QString ModelStore::directory() {
    QString env = qEnvironmentVariable("COMPOSITOR_MODEL_DIR");
    if (!env.isEmpty()) return env;
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/models";
}

QString ModelStore::pathFor(const ModelInfo& model) { return directory() + "/" + model.name; }

bool ModelStore::isPresent(const ModelInfo& model) {
    QFileInfo info(pathFor(model));
    return info.exists() && info.size() == model.bytes;
}

bool ModelStore::remove(const ModelInfo& model) { return QFile::remove(pathFor(model)); }

ModelStore::Download ModelStore::download(const ModelInfo& model, QObject* context, std::function<void(qint64, qint64)> progress, std::function<void(QString, QString)> done) {
    if (isPresent(model)) { done(pathFor(model), {}); return {[] {}}; }
    QDir().mkpath(directory());
    auto* manager = new QNetworkAccessManager(context);
    QNetworkRequest request{QUrl(model.url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = manager->get(request);
    // Written to a .part file and renamed into place once verified (QSaveFile's commit uses a hard link, which
    // some sandboxes deny).
    auto* file = new QFile(pathFor(model) + ".part", reply);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reply->abort();
        reply->deleteLater();
        manager->deleteLater();
        done({}, QObject::tr("Couldn’t write to %1.").arg(directory()));
        return {[] {}};
    }
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, file, hash] { QByteArray chunk = reply->readAll(); file->write(chunk); hash->addData(chunk); });
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [progress, model](qint64 received, qint64 total) { if (progress) progress(received, total > 0 ? total : model.bytes); });
    QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
        manager->deleteLater();
        QString error;
        auto discard = [file] { file->close(); file->remove(); };
        if (reply->error() == QNetworkReply::OperationCanceledError) { discard(); reply->deleteLater(); done({}, {}); return; }
        if (reply->error() != QNetworkReply::NoError) error = reply->errorString();
        else {
            QByteArray chunk = reply->readAll();
            file->write(chunk);
            hash->addData(chunk);
            if (QString::fromLatin1(hash->result().toHex()) != model.sha256) error = QObject::tr("The downloaded file didn’t match its expected checksum.");
        }
        if (!error.isEmpty()) { discard(); reply->deleteLater(); done({}, error); return; }
        file->close();
        QFile::remove(pathFor(model));
        if (!file->rename(pathFor(model))) { discard(); reply->deleteLater(); done({}, QObject::tr("Couldn’t save the model file into %1.").arg(directory())); return; }
        reply->deleteLater();
        done(pathFor(model), {});
    });
    QPointer<QNetworkReply> handle(reply);
    return {[handle] { if (handle) handle->abort(); }};
}

} // namespace app
