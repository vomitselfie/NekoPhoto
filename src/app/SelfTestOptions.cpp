// move-options: the Move tool's transform fields in the options bar follow the active layer. On an adjustment layer,
// which has no pixels to place, they are empty and greyed and say why (they showed the canvas-sized box it nominally
// has, 0.01 % or the layer before's numbers); on a pixel layer they show where it is and how big again.
#include "SelfTestSupport.h"
#include "EditorSession.h"
#include "MainWindow.h"
#include "ToolOptionsBar.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <cstdio>
#include <map>

using namespace nekophoto;

namespace app {

int moveOptions(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    w.resize(1400, 900);
    w.show();
    QApplication::processEvents();
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "move-options: %s\n", what); failures++; } };
    ToolOptionsBar* bar = nullptr;
    for (ToolOptionsBar* b : w.findChildren<ToolOptionsBar*>()) if (b->isVisibleTo(&w)) bar = b;
    if (!bar) { std::fprintf(stderr, "move-options: no options bar\n"); return 1; }
    // The transform fields, by what they are ("Left", "Width", ...).
    std::map<QString, QDoubleSpinBox*> fields;
    for (QDoubleSpinBox* f : bar->findChildren<QDoubleSpinBox*>())
        if (const QString tip = f->property("tip").toString(); !tip.isEmpty()) fields[tip] = f;
    QDoubleSpinBox* width = fields[QStringLiteral("Width")];
    expect(fields.size() == 7 && width, "the Move tool's seven transform fields are not there");
    if (fields.size() != 7 || !width) return 1;
    s.selectTool(Tool::Move);

    const Layer* paint = nullptr;
    const Layer* adjustment = nullptr;
    for (const Layer& l : s.document()->layers) {
        if (l.name == "Paint") paint = &l;
        if (l.adjustment && !adjustment) adjustment = &l;
    }
    expect(paint && adjustment, "the demo has no Paint layer or no adjustment layer");
    if (!paint || !adjustment) return 1;
    const Uuid paintId = paint->id, adjustmentId = adjustment->id;

    // A pixel layer first, so the fields hold numbers the adjustment layer must not keep showing.
    s.selectLayer(paintId);
    QApplication::processEvents();
    expect(!width->cleanText().isEmpty() && width->toolTip() == QLatin1String("Width"), "on a pixel layer the Width field is empty or lost its tooltip");

    s.selectLayer(adjustmentId);
    QApplication::processEvents();
    for (const auto& [tip, f] : fields) {
        if (!f->cleanText().isEmpty()) std::fprintf(stderr, "move-options: on an adjustment layer %s shows \"%s\"\n", qPrintable(tip), qPrintable(f->text()));
        expect(f->cleanText().isEmpty(), "on an adjustment layer a transform field shows a number");
        expect(!f->isEnabled(), "on an adjustment layer a transform field can be edited");
        expect(f->toolTip() == QLatin1String("An adjustment layer has no pixels to transform"), "on an adjustment layer a transform field does not say why it is empty");
    }

    s.selectLayer(paintId);
    QApplication::processEvents();
    const Layer* back = s.document()->find(paintId);
    expect(back && width->isEnabled() && width->toolTip() == QLatin1String("Width") && std::abs(width->value() - back->transform.size.width) < 0.05
               && width->cleanText() == QString::number(back->transform.size.width, 'f', 1),
           "back on the pixel layer the Width field does not show its width again");
    std::printf("move-options: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

} // namespace app
