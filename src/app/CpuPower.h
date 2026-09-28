// How much of the machine NekoPhoto uses: Edit > Preferences > Performance > CPU power (All, High, Medium, Low), or
// NEKOPHOTO_CPU for one run. Fewer cores for the worker pool, and at Medium and Low a lower process priority, so a
// render or another heavy program running beside NekoPhoto gets the CPU first. Applied once at start-up, before the
// first parallel loop makes the pool.
#pragma once
#include <QString>
#include <QStringList>

namespace app::cpupower {

/// The stored choice: "all", "high", "medium" or "low".
QString setting();
void setSetting(const QString& level);
/// The choices, most power first.
QStringList levels();
/// The choice's name for Preferences ("All", "High", …), translated.
QString label(const QString& level);
/// Cores the pool gets at `level` on this machine.
int cores(const QString& level);
/// One line for Preferences: what `level` means here ("Medium: 12 of 24 cores, gives way to other programs").
QString describe(const QString& level);
/// Sets the pool's size and the process priority from NEKOPHOTO_CPU, else the stored choice. Call once, early.
void apply();
/// The level apply() settled on.
QString current();

} // namespace app::cpupower
