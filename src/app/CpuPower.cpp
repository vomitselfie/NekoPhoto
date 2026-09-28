#include "CpuPower.h"
#include "Platform.h"
#include "compositor/parallel.h"
#include <QCoreApplication>
#include <QSettings>
#include <algorithm>
#include <cmath>

namespace app::cpupower {

namespace {

const char* const key = "performance/cpuPower";
QString applied = QStringLiteral("all");

QString valid(const QString& level) {
    const QString l = level.trimmed().toLower();
    return levels().contains(l) ? l : QString();
}

/// The share of the machine's threads each level keeps, and how far it steps back from other programs.
double share(const QString& level) { return level == "high" ? 0.75 : level == "medium" ? 0.5 : level == "low" ? 0.25 : 1.0; }
int priorityDrop(const QString& level) { return level == "low" ? 2 : level == "medium" ? 1 : 0; }

} // namespace

QStringList levels() { return {QStringLiteral("all"), QStringLiteral("high"), QStringLiteral("medium"), QStringLiteral("low")}; }

QString setting() {
    const QString stored = valid(QSettings().value(key).toString());
    return stored.isEmpty() ? QStringLiteral("all") : stored;
}

void setSetting(const QString& level) {
    if (const QString l = valid(level); !l.isEmpty()) QSettings().setValue(key, l);
}

QString label(const QString& level) {
    if (level == "high") return QCoreApplication::translate("CpuPower", "High");
    if (level == "medium") return QCoreApplication::translate("CpuPower", "Medium");
    if (level == "low") return QCoreApplication::translate("CpuPower", "Low");
    return QCoreApplication::translate("CpuPower", "All");
}

int cores(const QString& level) {
    const int machine = std::min(64, compositor::hardwareThreads());
    return std::clamp(int(std::ceil(machine * share(level))), 1, machine);
}

QString describe(const QString& level) {
    const int machine = std::min(64, compositor::hardwareThreads());
    const QString used = QCoreApplication::translate("CpuPower", "%1: %2 of %3 cores").arg(label(level)).arg(cores(level)).arg(machine);
    if (priorityDrop(level) == 0) return used;
    return QCoreApplication::translate("CpuPower", "%1, gives way to other programs").arg(used);
}

void apply() {
    QString level = valid(qEnvironmentVariable("NEKOPHOTO_CPU"));
    if (level.isEmpty()) level = setting();
    applied = level;
    if (level != "all") compositor::setWorkerLimit(cores(level));
    platform::lowerProcessPriority(priorityDrop(level));
}

QString current() { return applied; }

} // namespace app::cpupower
