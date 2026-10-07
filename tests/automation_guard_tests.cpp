// The automation socket's limits (src/app/AutomationGuard.h) and the checks on actions read from files
// (ActionLibrary::fromJson): request framing with its size cap, the overwrite rule and write roots, the audit
// line, and actions that would play actions or write files.
#include "check.h"
#include "ActionLibrary.h"
#include "AutomationGuard.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>

using namespace app;

namespace {

QJsonObject step(const QString& method, const QJsonObject& params = {}) { return {{"method", method}, {"params", params}}; }
QJsonObject action(const QString& name, const QJsonArray& steps) { return {{"name", name}, {"steps", steps}}; }

void touch(const QString& path) {
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write("x");
}

} // namespace

TEST_CASE(framing_splits_lines) {
    QByteArray buffer("{\"a\":1}\n\n  {\"b\":2}  \n{\"c\"");
    const automation::Framed framed = automation::takeLines(buffer, 100);
    CHECK(!framed.overflow);
    CHECK_EQ(int(framed.lines.size()), 2);
    CHECK(framed.lines.value(1) == "{\"b\":2}");
    CHECK(buffer == "{\"c\"");
}

TEST_CASE(framing_caps_an_unfinished_request) {
    QByteArray buffer(101, 'x');
    const automation::Framed framed = automation::takeLines(buffer, 100);
    CHECK(framed.overflow);
    CHECK(buffer.isEmpty());
    QByteArray exact(100, 'x');
    CHECK(!automation::takeLines(exact, 100).overflow);
}

TEST_CASE(framing_caps_a_long_line_but_keeps_the_ones_before) {
    QByteArray buffer = QByteArray("ok\n") + QByteArray(200, 'y') + "\nafter\n";
    const automation::Framed framed = automation::takeLines(buffer, 100);
    CHECK(framed.overflow);
    CHECK_EQ(int(framed.lines.size()), 1);
    CHECK(buffer.isEmpty());
    CHECK(automation::maxRequestBytes == qsizetype(64) * 1024 * 1024);
}

TEST_CASE(overwrite_needs_asking) {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    automation::setWriteRoots({});
    const QString file = dir.filePath("a.png");
    CHECK(automation::writeRefusal(file, false).isEmpty());
    touch(file);
    CHECK(automation::writeRefusal(file, false).contains("already exists"));
    CHECK(automation::writeRefusal(file, true).isEmpty());
}

TEST_CASE(write_roots_confine_writes) {
    QTemporaryDir root, other;
    REQUIRE(root.isValid() && other.isValid());
    automation::setWriteRoots({root.path()});
    CHECK(automation::writeRootRefusal(root.filePath("x.png")).isEmpty());
    CHECK(automation::writeRootRefusal(root.filePath("new/deeper/x.png")).isEmpty());
    CHECK(!automation::writeRootRefusal(other.filePath("x.png")).isEmpty());
    CHECK(!automation::writeRootRefusal(root.path() + "/../escape.png").isEmpty());
    CHECK(!automation::writeRootRefusal(root.path() + "-sibling/x.png").isEmpty());
#ifndef _WIN32
    // A folder link inside the root that points out of it, and a final component that is a link.
    REQUIRE(QFile::link(other.path(), root.filePath("out")));
    CHECK(!automation::writeRootRefusal(root.filePath("out/x.png")).isEmpty());
    REQUIRE(QFile::link(other.filePath("target.png"), root.filePath("link.png")));
    CHECK(automation::writeRootRefusal(root.filePath("link.png")).contains("symbolic link"));
#endif
    automation::setWriteRoots({});
    CHECK(automation::writeRootRefusal(other.filePath("x.png")).isEmpty());
}

TEST_CASE(audit_line_names_paths_only) {
    const QJsonObject params{{"path", "/tmp/out.png"}, {"png", QString(5000, 'A')}, {"points", QJsonArray{1, 2, 3}}};
    const QString ok = automation::auditLine("T", "render", params, QJsonObject{{"result", QJsonObject{}}}, 12);
    CHECK(ok.contains("/tmp/out.png"));
    CHECK(!ok.contains("AAAA"));
    CHECK(!ok.contains("points"));
    CHECK(ok.endsWith("\tok\t12ms"));
    const QString bad = automation::auditLine("T", "document.export", {}, QJsonObject{{"error", QJsonObject{{"code", -32602}}}}, 0);
    CHECK(bad.contains("error -32602"));
    CHECK(!bad.contains('\n'));
}

TEST_CASE(actions_cannot_play_actions) {
    QString error;
    CHECK(!ActionLibrary::fromJson(action("loop", {step("actions.play", {{"name", "loop"}})}), &error));
    CHECK(error.contains("actions method"));
    CHECK(!ActionLibrary::fromJson(action("export", {step("actions.export", {{"path", "/tmp/a.json"}})}), &error));
    const QJsonObject nested{{"calls", QJsonArray{QJsonObject{{"method", "actions.play"}, {"params", QJsonObject{{"name", "x"}}}}}}};
    CHECK(!ActionLibrary::fromJson(action("nested", {step("rpc.batch", nested)}), &error));
    // A disabled step is refused too: it can be switched on later.
    QJsonObject off = step("actions.play");
    off["enabled"] = false;
    CHECK(!ActionLibrary::fromJson(action("off", {off}), &error));
    CHECK(ActionLibrary::fromJson(action("fine", {step("pixels.invert")}), &error).has_value());
}

TEST_CASE(written_files_are_listed) {
    QString error;
    auto a = ActionLibrary::fromJson(action("writes", {step("pixels.invert"), step("document.export", {{"path", "/tmp/o.png"}}), step("render", {{"maxSize", 64}}),
                                                       step("render", {{"path", "/tmp/r.png"}}), step("slices.export", {{"directory", "/tmp/s"}}), step("document.save")}),
                                     &error);
    REQUIRE(a.has_value());
    const QStringList files = ActionLibrary::writtenFiles(*a);
    CHECK_EQ(int(files.size()), 4);
    CHECK(files.contains("document.export: /tmp/o.png"));
    CHECK(files.contains("render: /tmp/r.png"));
    CHECK(files.contains("document.save"));
    auto quiet = ActionLibrary::fromJson(action("quiet", {step("pixels.invert"), step("render")}), &error);
    REQUIRE(quiet.has_value());
    CHECK(ActionLibrary::writtenFiles(*quiet).isEmpty());
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    return check::run(1, argv);
}
