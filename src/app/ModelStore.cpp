#include "ModelStore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProgressDialog>
#include <QSaveFile>
#include <QStandardPaths>

namespace app {

const ModelInfo& ModelStore::primary() {
    static const ModelInfo info{"isnet-general-use.onnx", "https://github.com/danielgatis/rembg/releases/download/v0.0.0/isnet-general-use.onnx",
                                "60920e99c45464f2ba57bee2ad08c919a52bbf852739e96947fbb4358c0d964a", 178648008, "IS-Net (general use)"};
    return info;
}

const ModelInfo& ModelStore::small() {
    static const ModelInfo info{"u2netp.onnx", "https://github.com/danielgatis/rembg/releases/download/v0.0.0/u2netp.onnx",
                                "309c8469258dda742793dce0ebea8e6dd393174f89934733ecc8b14c76f4ddd8", 4574861, "U2Net small"};
    return info;
}

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

void ModelStore::ensure(const ModelInfo& model, QWidget* parent, std::function<void(QString, QString)> done) {
    if (isPresent(model)) { done(pathFor(model), {}); return; }
    QMessageBox ask(parent);
    ask.setIcon(QMessageBox::Question);
    ask.setWindowTitle(QObject::tr("Download the background removal model?"));
    ask.setText(QObject::tr("Remove Background needs the %1 model, a %2 MB download (Apache-2.0 licensed, from the rembg project). It is kept in %3 and downloaded once.")
                    .arg(model.label).arg(model.bytes / 1048576).arg(directory()));
    ask.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    ask.setDefaultButton(QMessageBox::Yes);
    if (ask.exec() != QMessageBox::Yes) { done({}, {}); return; }
    QDir().mkpath(directory());
    auto* manager = new QNetworkAccessManager(parent);
    auto* progress = new QProgressDialog(QObject::tr("Downloading %1…").arg(model.label), QObject::tr("Cancel"), 0, 100, parent);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    QNetworkRequest request{QUrl(model.url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = manager->get(request);
    auto* file = new QSaveFile(pathFor(model), reply);
    if (!file->open(QIODevice::WriteOnly)) { done({}, QObject::tr("Couldn’t write to %1.").arg(directory())); reply->abort(); return; }
    auto* hash = new QCryptographicHash(QCryptographicHash::Sha256);
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, file, hash] { QByteArray chunk = reply->readAll(); file->write(chunk); hash->addData(chunk); });
    QObject::connect(reply, &QNetworkReply::downloadProgress, progress, [progress, &model](qint64 received, qint64 total) {
        qint64 size = total > 0 ? total : model.bytes;
        progress->setValue(int(received * 100 / std::max<qint64>(1, size)));
    });
    QObject::connect(progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
        progress->deleteLater();
        manager->deleteLater();
        QString error;
        if (reply->error() == QNetworkReply::OperationCanceledError) { file->cancelWriting(); delete hash; reply->deleteLater(); done({}, {}); return; }
        if (reply->error() != QNetworkReply::NoError) error = reply->errorString();
        else {
            QByteArray chunk = reply->readAll();
            file->write(chunk);
            hash->addData(chunk);
            if (QString::fromLatin1(hash->result().toHex()) != model.sha256) error = QObject::tr("The downloaded file didn’t match its expected checksum.");
        }
        delete hash;
        if (!error.isEmpty()) { file->cancelWriting(); reply->deleteLater(); done({}, error); return; }
        if (!file->commit()) { reply->deleteLater(); done({}, QObject::tr("Couldn’t save the model file.")); return; }
        reply->deleteLater();
        done(pathFor(model), {});
    });
}

} // namespace app
