// G'MIC downloaded on request (GmicStore): the verify-and-unpack path over archives shaped like gmic.eu's win64 zip
// (a top folder holding gmic.exe and its DLLs), stored and deflated, with matching and mismatching size and SHA-256;
// hostile archives refused with nothing left behind; and where GmicRunner looks for the executable, in order.
// No network: the download itself is not run here.
#include "check.h"
#include "Gmic.h"
#include "GmicStore.h"
#include "compositor/zipfile.h"
#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>
#include <string>
#include <utility>
#include <vector>

using namespace app;

namespace {

const std::string top = "gmic-4.0.5-cli-win64/";

std::vector<uint8_t> bytesOf(const std::string& s) { return {s.begin(), s.end()}; }

/// Text that compresses, so the deflated archive really deflates.
std::string filler(const std::string& seed, int lines) {
    std::string out;
    for (int i = 0; i < lines; i++) out += seed + " line " + std::to_string(i % 7) + "\n";
    return out;
}

struct Entry { std::string name, data; };

std::vector<Entry> realShape() {
    return {{top, ""},
            {top + "gmic.exe", "MZ" + filler("gmic executable", 400)},
            {top + "libgmic.dll", "MZ" + filler("a library", 300)},
            {top + "README.txt", filler("G'MIC is free software under the CeCILL 2.1 licence", 20)}};
}

bool writeZip(const QString& path, const std::vector<Entry>& entries, bool deflate) {
    compositor::ZipFileWriter zip;
    if (!zip.open(path.toStdString())) return false;
    for (const Entry& e : entries)
        if (!zip.add(e.name, bytesOf(e.data), deflate && !e.data.empty())) return false;
    return zip.finish();
}

QByteArray readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeAll(const QString& path, const QByteArray& data) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

/// The pinned build's shape for an archive on disk: its own size and hash, as if gmic.eu had served it.
GmicStore::Build buildFor(const QString& archive) {
    const QByteArray data = readAll(archive);
    GmicStore::Build b;
    b.version = QStringLiteral("4.0.5");
    b.url = QStringLiteral("https://example.invalid/gmic.zip");
    b.sha256 = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    b.bytes = data.size();
    b.executable = QStringLiteral("gmic.exe");
    return b;
}

/// Everything in `root`, hidden entries too.
QStringList contents(const QString& root) {
    return QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
}

/// Replaces every occurrence of `from` with `to` (the same length) in the file: names in both the local headers and
/// the central directory, which the writer refuses to make itself.
bool patch(const QString& path, const QByteArray& from, const QByteArray& to) {
    if (from.size() != to.size()) return false;
    QByteArray data = readAll(path);
    if (!data.contains(from)) return false;
    data.replace(from, to);
    return writeAll(path, data);
}

void checkInstalled(bool deflate) {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString archive = dir.filePath("gmic.zip.part"), root = dir.filePath("gmic");
    REQUIRE(writeZip(archive, realShape(), deflate));
    if (deflate) CHECK(QFileInfo(archive).size() < 4000);   // really compressed
    QString error;
    CHECK(GmicStore::install(archive, buildFor(archive), root, &error));
    CHECK(error.isEmpty());
    CHECK(!QFileInfo::exists(archive));
    // The top folder is gone: the files sit in root/<version>, and nothing else is left in root.
    CHECK(contents(root) == QStringList{"4.0.5"});
    CHECK(contents(root + "/4.0.5") == (QStringList{"README.txt", "gmic.exe", "libgmic.dll"}));
    const auto shape = realShape();
    CHECK(readAll(root + "/4.0.5/gmic.exe") == QByteArray::fromStdString(shape[1].data));
    CHECK(readAll(root + "/4.0.5/libgmic.dll") == QByteArray::fromStdString(shape[2].data));
    CHECK(QFileInfo(root + "/4.0.5/gmic.exe").isExecutable());

    // Downloaded again: the copy there is replaced whole.
    std::vector<Entry> newer = realShape();
    newer[1].data = "MZ newer";
    REQUIRE(writeZip(archive, newer, deflate));
    CHECK(GmicStore::install(archive, buildFor(archive), root, &error));
    CHECK(readAll(root + "/4.0.5/gmic.exe") == QByteArray("MZ newer"));
    CHECK(contents(root) == QStringList{"4.0.5"});
}

/// An archive that must be refused: false, an error, the archive deleted and nothing installed or left over.
void checkRefused(const QString& archive, const GmicStore::Build& build, const QString& root, const char* what) {
    QString error;
    const bool ok = GmicStore::install(archive, build, root, &error);
    if (ok) std::fprintf(stderr, "  installed although %s\n", what);
    CHECK(!ok);
    CHECK(!error.isEmpty());
    std::fprintf(stderr, "  %s: %s\n", what, qPrintable(error));
    CHECK(!QFileInfo::exists(archive));
    CHECK(contents(root).isEmpty());
}

} // namespace

TEST_CASE(stored_archive_installs) { checkInstalled(false); }
TEST_CASE(deflated_archive_installs) { checkInstalled(true); }

TEST_CASE(size_or_hash_mismatch_is_refused) {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString archive = dir.filePath("gmic.zip.part"), root = dir.filePath("gmic");
    REQUIRE(QDir().mkpath(root));
    for (int deflate = 0; deflate < 2; deflate++) {
        REQUIRE(writeZip(archive, realShape(), deflate != 0));
        GmicStore::Build wrongHash = buildFor(archive);
        wrongHash.sha256[0] = wrongHash.sha256[0] == QLatin1Char('0') ? QLatin1Char('1') : QLatin1Char('0');
        checkRefused(archive, wrongHash, root, "the SHA-256 differs");

        REQUIRE(writeZip(archive, realShape(), deflate != 0));
        GmicStore::Build wrongSize = buildFor(archive);
        wrongSize.bytes += 1;
        checkRefused(archive, wrongSize, root, "the size differs");

        // A file cut short (an interrupted download) differs in both.
        REQUIRE(writeZip(archive, realShape(), deflate != 0));
        const GmicStore::Build whole = buildFor(archive);
        REQUIRE(writeAll(archive, readAll(archive).left(int(whole.bytes / 2))));
        checkRefused(archive, whole, root, "the file is cut short");
    }
}

TEST_CASE(hostile_archives_are_refused) {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString archive = dir.filePath("gmic.zip.part"), root = dir.filePath("gmic");
    REQUIRE(QDir().mkpath(root));
    // Each archive matches its own hash, as if the pinned file itself were hostile: the unpacking must still hold.
    auto withPlaceholder = [&](const std::string& placeholder) {
        std::vector<Entry> entries = realShape();
        entries.push_back({top + placeholder, "payload"});
        return writeZip(archive, entries, false);
    };

    // A name that climbs out of the folder.
    REQUIRE(withPlaceholder("AAAAAAA"));
    REQUIRE(patch(archive, QByteArray::fromStdString(top + "AAAAAAA"), QByteArray::fromStdString(top + "../../x")));
    checkRefused(archive, buildFor(archive), root, "an entry climbs out (../)");
    CHECK(!QFileInfo::exists(dir.filePath("x")));

    // An absolute name.
    const std::string absolute = "/" + std::string(top.size() + 6, 'y');
    REQUIRE(withPlaceholder("BBBBBBB"));
    REQUIRE(patch(archive, QByteArray::fromStdString(top + "BBBBBBB"), QByteArray::fromStdString(absolute)));
    checkRefused(archive, buildFor(archive), root, "an entry is absolute");

    // A drive-letter name, absolute on Windows.
    REQUIRE(withPlaceholder("CCCCCCC"));
    REQUIRE(patch(archive, QByteArray::fromStdString(top + "CCCCCCC"), QByteArray::fromStdString("C:/" + std::string(top.size() + 4, 'z'))));
    checkRefused(archive, buildFor(archive), root, "an entry names a drive");

    // An entry that claims to be huge (its directory record says 2 GiB).
    {
        std::vector<Entry> entries = realShape();
        entries.push_back({top + "huge.dll", "0123456789abcdef"});
        REQUIRE(writeZip(archive, entries, false));
        QByteArray data = readAll(archive);
        const QByteArray name = QByteArray::fromStdString(top + "huge.dll");
        const qsizetype at = data.lastIndexOf(name);   // the central directory's copy comes last
        REQUIRE(at >= 46);
        const qsizetype record = at - 46;
        REQUIRE(data.mid(record, 4) == QByteArray("PK\x01\x02", 4));
        for (int field : {20, 24}) {   // compressed and uncompressed sizes
            data[record + field + 0] = char(0xF0); data[record + field + 1] = char(0xFF);
            data[record + field + 2] = char(0xFF); data[record + field + 3] = char(0x7F);
        }
        REQUIRE(writeAll(archive, data));
        checkRefused(archive, buildFor(archive), root, "an entry claims 2 GiB");
    }

    // Too much in all: the limits passed in hold (the real ones allow 128 MiB an entry).
    {
        std::vector<Entry> entries = realShape();
        entries.push_back({top + "big.dll", std::string(64 * 1024, 'x')});
        REQUIRE(writeZip(archive, entries, true));
        compositor::ZipLimits tight = GmicStore::limits();
        tight.entryBytes = 32 * 1024;
        QString error;
        CHECK(!GmicStore::install(archive, buildFor(archive), root, &error, tight));
        CHECK(!QFileInfo::exists(archive));
        CHECK(contents(root).isEmpty());
    }

    // Not laid out as gmic.eu's zip is: two top folders, a file at the top, or no gmic.exe.
    REQUIRE(writeZip(archive, {{top + "gmic.exe", "MZ"}, {"other/libgmic.dll", "MZ"}}, false));
    checkRefused(archive, buildFor(archive), root, "two top folders");
    REQUIRE(writeZip(archive, {{"gmic.exe", "MZ"}}, false));
    checkRefused(archive, buildFor(archive), root, "a file at the top");
    REQUIRE(writeZip(archive, {{top + "libgmic.dll", "MZ"}, {top + "bin/gmic.exe", "MZ"}}, false));
    checkRefused(archive, buildFor(archive), root, "no gmic.exe at the top");
    // Not a ZIP at all.
    REQUIRE(writeAll(archive, QByteArray("<html>gmic.eu is down</html>")));
    checkRefused(archive, buildFor(archive), root, "not a ZIP");
}

TEST_CASE(lookup_order) {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString appDir = dir.filePath("app"), dataDir = dir.filePath("data"), pathDir = dir.filePath("bin"), envDir = dir.filePath("env");
    const QString name = GmicRunner::executableName();
    auto makeExecutable = [](const QString& path) {
        QDir().mkpath(QFileInfo(path).path());
        if (!writeAll(path, "#!/bin/sh\necho 'G'MIC test stand-in'\n")) return false;
        return QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ExeOwner | QFileDevice::ExeUser);
    };
    const QString env = envDir + "/custom-" + name, beside = appDir + "/gmic/" + name, downloaded = dataDir + "/gmic/4.0.5/" + name,
                  onPath = pathDir + '/' + name;
    for (const QString& path : {env, beside, downloaded, onPath}) REQUIRE(makeExecutable(path));
    const QByteArray oldPath = qgetenv("PATH"), oldEnv = qgetenv("COMPOSITOR_GMIC");
    GmicRunner::setSearchRootsForTesting(appDir, dataDir);
    qputenv("PATH", QDir::toNativeSeparators(pathDir).toLocal8Bit());
    qputenv("COMPOSITOR_GMIC", QDir::toNativeSeparators(env).toLocal8Bit());

    auto found = GmicRunner::locate();
    CHECK(found.source == "env");
    CHECK(QFileInfo(found.path) == QFileInfo(env));

    // COMPOSITOR_GMIC naming something that is not there falls through to the next place.
    qputenv("COMPOSITOR_GMIC", QDir::toNativeSeparators(envDir + "/missing-" + name).toLocal8Bit());
    found = GmicRunner::locate();
    CHECK(found.source == "beside");
    CHECK(QFileInfo(found.path) == QFileInfo(beside));
    qunsetenv("COMPOSITOR_GMIC");
    CHECK(GmicRunner::locate().source == "beside");

    REQUIRE(QFile::remove(beside));
    found = GmicRunner::locate();
    CHECK(found.source == "downloaded");
    CHECK(QFileInfo(found.path) == QFileInfo(downloaded));
    CHECK(GmicStore::downloaded() == (name == QLatin1String("gmic.exe")));

    // A half-unpacked copy beside it is never used.
    REQUIRE(makeExecutable(dataDir + "/gmic/.unpack-abc123/" + name));
    REQUIRE(QFile::remove(downloaded));
    found = GmicRunner::locate();
    CHECK(found.source == "path");
    CHECK(QFileInfo(found.path) == QFileInfo(onPath));

    REQUIRE(QFile::remove(onPath));
    found = GmicRunner::locate();
    CHECK(found.path.isEmpty());
    CHECK(found.source.isEmpty());

    qputenv("PATH", oldPath);
    if (!oldEnv.isEmpty()) qputenv("COMPOSITOR_GMIC", oldEnv);
    GmicRunner::setSearchRootsForTesting({}, {});
}

TEST_CASE(install_then_remove) {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString dataDir = dir.filePath("data"), archive = dir.filePath("gmic.zip.part");
    GmicRunner::setSearchRootsForTesting(dir.filePath("app"), dataDir);
    // The executable under this platform's name, so the lookup finds it here too.
    std::vector<Entry> entries = realShape();
    entries[1].name = top + GmicRunner::executableName().toStdString();
    REQUIRE(writeZip(archive, entries, true));
    GmicStore::Build build = buildFor(archive);
    build.executable = GmicRunner::executableName();
    QString error;
    REQUIRE(GmicStore::install(archive, build, GmicStore::directory(), &error));
    CHECK(GmicStore::directory() == dataDir + "/gmic");
    CHECK(QFileInfo::exists(GmicRunner::downloadedDirectory() + '/' + build.executable));
    const QByteArray oldPath = qgetenv("PATH"), oldEnv = qgetenv("COMPOSITOR_GMIC");
    qputenv("PATH", QDir::toNativeSeparators(dir.filePath("empty")).toLocal8Bit());
    qunsetenv("COMPOSITOR_GMIC");
    CHECK(GmicRunner::locate().source == "downloaded");
    CHECK(GmicStore::remove(&error));
    CHECK(!QFileInfo::exists(GmicRunner::downloadedDirectory()));
    CHECK(contents(GmicStore::directory()).isEmpty());
    CHECK(GmicRunner::locate().path.isEmpty());
    qputenv("PATH", oldPath);
    if (!oldEnv.isEmpty()) qputenv("COMPOSITOR_GMIC", oldEnv);
    GmicRunner::setSearchRootsForTesting({}, {});
}

TEST_CASE(pinned_build) {
    const GmicStore::Build& b = GmicStore::pinned();
    CHECK(b.url == "https://gmic.eu/files/windows/gmic_4.0.5_cli_win64.zip");
    CHECK(b.bytes == 14561476);
    CHECK(b.sha256.size() == 64);
    CHECK(b.version == GmicRunner::downloadVersion());
    CHECK(b.executable == "gmic.exe");
}

TEST_MAIN()
