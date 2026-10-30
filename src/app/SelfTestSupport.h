// What the window's self-tests share: the helpers (SelfTest.cpp) and the checks runSelfTest runs by name, each in
// the SelfTest*.cpp file for its theme.
#pragma once
#include "SelfTest.h"
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

class QAction;
class QDialog;

namespace app {

class EditorSession;
class MainWindow;

void buildDemoDocument(EditorSession& session);   // main.cpp

namespace selftest {

/// The document as text, without its random ids: every layer's name, kind, placement, look and pixels, the
/// selection, the guides, the composite, and the history's step names.
QString describe(EditorSession& s);
/// Makes the layer named `name` active; false (and a message) when there is none.
bool select(EditorSession& s, const char* name);
/// The window's action with that object name, or null (and a message).
QAction* action(MainWindow& w, const char* name);
/// A menu bar item by its path of titles ("Layer", "Layer Mask", "Invert"), mnemonics and shortcuts left out.
QAction* menuItem(MainWindow& w, const QStringList& path);
/// Answers the next modal dialog (an input dialog, Canvas Size, Trim ...): `fill` sets its fields, then OK.
void answerModal(std::function<void(QDialog*)> fill);

template <class T> T* nth(QObject* parent, int index) {
    const auto all = parent->findChildren<T*>();
    return index < all.size() ? all[index] : nullptr;
}

} // namespace selftest

// The checks: each returns the process status (0 passed).
int commandPath(MainWindow& w);          // SelfTestCommandPath.cpp
int menuCommands(MainWindow& w);         // SelfTestMenuCommands.cpp
int fileCommands(MainWindow& w);         // SelfTestFiles.cpp
int exportAs(MainWindow& w);             // SelfTestFiles.cpp
int heldKeys(MainWindow& w);             // SelfTestKeys.cpp
int registry(MainWindow& w);             // SelfTestKeys.cpp
int keyboardShortcuts(MainWindow& w);    // SelfTestKeys.cpp
int guides(MainWindow& w);               // SelfTestInterface.cpp
int canvasMenus(MainWindow& w);          // SelfTestInterface.cpp
int search(MainWindow& w);               // SelfTestInterface.cpp
int preferences(MainWindow& w);          // SelfTestInterface.cpp
int workCounters(MainWindow& window);    // SelfTestWork.cpp
int fullScreen(MainWindow& window);      // SelfTestFullScreen.cpp
int hidePanels(MainWindow& window);      // SelfTestHidePanels.cpp
int edgePages(MainWindow& window);       // SelfTestEdges.cpp
int forbiddenCursor(MainWindow& window); // SelfTestForbidden.cpp
int toolRing(MainWindow& window);        // SelfTestToolRing.cpp
int colorRing(MainWindow& window);       // SelfTestColorRing.cpp
int layerPicker(MainWindow& window);     // SelfTestLayerPicker.cpp
int menuDump(MainWindow& window);        // SelfTestDump.cpp
int optionsDump(MainWindow& window);     // SelfTestDump.cpp
int interfaceMotion(MainWindow& window); // SelfTestMotion.cpp

} // namespace app
