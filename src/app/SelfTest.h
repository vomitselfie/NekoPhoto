// In-app checks run by ctest through `nekophoto --self-test <name>` (SelfTest.cpp).
#pragma once
#include <QString>

namespace app {

class MainWindow;

/// Runs check `name` against `window` and returns the process status (0 passed, 1 failed, 2 unknown).
int runSelfTest(MainWindow& window, const QString& name);

} // namespace app
