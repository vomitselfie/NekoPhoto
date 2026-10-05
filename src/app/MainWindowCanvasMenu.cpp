// The canvas's context menu, as Photoshop's: what it offers follows the active tool and what is under the pointer.
// Commands the menu bar already has are the menu bar's own QActions (so shortcuts, enabled state, the command path
// and Actions recording are the same); a clearer label for one ("Disable Layer Mask") triggers that action. What
// does not apply is left out rather than greyed.
#include "BrushPicker.h"
#include "CanvasWidget.h"
#include "MainWindow.h"
#include "ToolOptionsBar.h"
#include <QMenu>
#include <cmath>

using namespace compositor;

namespace app {

namespace {
double turned(double degrees) {
    double r = std::fmod(degrees, 360.0);
    if (r > 180) r -= 360;
    if (r <= -180) r += 360;
    return r;
}
} // namespace

QMenu* MainWindow::buildCanvasMenu(QPointF view, QWidget* parent) {
    auto* menu = new QMenu(parent);
    EditorSession* s = session_;
    CanvasWidget* canvas = canvas_;
    if (!s || !canvas || !s->hasDocument()) return menu;
    const QPointF doc = canvas->documentPoint(view);
    // A menu bar action, when it applies here.
    auto add = [&](const char* key, bool applies = true) {
        QAction* a = named(QString::fromLatin1(key));
        if (a && applies && a->isEnabled()) menu->addAction(a);
    };
    // The same command under the label this menu gives it.
    auto as = [&](const QString& label, const char* key, bool applies = true) {
        QAction* a = named(QString::fromLatin1(key));
        if (!a || !applies || !a->isEnabled()) return;
        // The shortcut shown, not bound twice (the menu bar's action keeps it).
        const QString keys = a->shortcut().toString(QKeySequence::NativeText);
        menu->addAction(keys.isEmpty() ? label : label + '\t' + keys, a, &QAction::trigger);
    };
    auto separate = [&] { if (!menu->isEmpty() && !menu->actions().constLast()->isSeparator()) menu->addSeparator(); };

    // Typing: the text's own commands.
    if (canvas->typeEditing()) { canvas->addTypeMenu(menu); return menu; }

    // A transform in progress (Free Transform, a distortion, a selection's pixels): Photoshop's transform menu.
    if (const auto& edit = s->transformEdit()) {
        const bool distorting = edit->corners.has_value();
        auto adjust = [s](auto change) {
            if (!s->transformEdit() || s->transformEdit()->corners) return;
            LayerTransform t = s->transformEdit()->draft;
            change(t);
            s->previewTransform(t);
        };
        QAction* free = menu->addAction(tr("Free Transform"));
        free->setCheckable(true);
        free->setChecked(!distorting);
        free->setEnabled(false);
        if (!distorting && !edit->mask) menu->addAction(tr("Distort"), this, [s] { s->beginDistort(); });
        else if (distorting) { QAction* d = menu->addAction(tr("Distort")); d->setCheckable(true); d->setChecked(true); d->setEnabled(false); }
        if (!distorting) {
            menu->addSeparator();
            menu->addAction(tr("Rotate 180°"), this, [adjust] { adjust([](LayerTransform& t) { t.rotation = turned(t.rotation + 180); }); });
            menu->addAction(tr("Rotate 90° Clockwise"), this, [adjust] { adjust([](LayerTransform& t) { t.rotation = turned(t.rotation + 90); }); });
            menu->addAction(tr("Rotate 90° Counter Clockwise"), this, [adjust] { adjust([](LayerTransform& t) { t.rotation = turned(t.rotation - 90); }); });
            menu->addSeparator();
            // Across the screen's axes, whatever the box's rotation.
            menu->addAction(tr("Flip Horizontal"), this, [adjust] { adjust([](LayerTransform& t) { t.flipX = !t.flipX; t.rotation = turned(-t.rotation); }); });
            menu->addAction(tr("Flip Vertical"), this, [adjust] { adjust([](LayerTransform& t) { t.flipY = !t.flipY; t.rotation = turned(-t.rotation); }); });
        }
        menu->addSeparator();
        menu->addAction(tr("Apply Transform") + '\t' + QKeySequence(Qt::Key_Return).toString(QKeySequence::NativeText), this, [s] { s->commitTransformCommand(); });
        menu->addAction(tr("Cancel Transform") + '\t' + QKeySequence(Qt::Key_Escape).toString(QKeySequence::NativeText), this, [s] { s->cancelTransform(); });
        return menu;
    }

    const auto& document = *s->document();
    const bool hasSelection = document.selection && document.selection->coverage;
    const Layer* active = s->activeLayer();
    switch (s->tool()) {
    case Tool::Move: case Tool::Artboard: {
        // The layers with pixels under the pointer, topmost first; choosing one selects it (Photoshop's list).
        const std::vector<Uuid> under = s->layersAt(doc, 24);
        std::vector<Uuid> folders;
        for (const Uuid& id : under) {
            const Layer* l = document.find(id);
            if (!l) continue;
            QAction* pick = menu->addAction(QString::fromStdString(l->name), this, [s, id] { s->selectLayer(id); });
            pick->setCheckable(true);
            pick->setChecked(s->activeLayerId() == id);
            // The folders holding it, innermost first, once each.
            for (std::optional<Uuid> parent = l->parentId; parent;) {
                const Layer* folder = document.find(*parent);
                if (!folder) break;
                if (std::find(folders.begin(), folders.end(), folder->id) == folders.end()) folders.push_back(folder->id);
                parent = folder->parentId;
            }
        }
        if (!folders.empty()) {
            separate();
            for (const Uuid& id : folders)
                if (const Layer* folder = document.find(id))
                    menu->addAction(tr("Select Group “%1”").arg(QString::fromStdString(folder->name)), this, [s, id] { s->selectLayer(id); });
        }
        if (!active) break;
        // What can be done to the active layer.
        separate();
        add("edit.freeTransform", s->canTransform());
        add("layer.duplicate");
        add("layer.delete");
        add("layer.merge", s->canMergeLayers());
        add("layer.clipping", s->canToggleClippingMask(active->id));
        separate();
        if (active->isLiveSmartObject()) {
            add("smart.edit");
            add("smart.replace");
            as(tr("Rasterize Layer"), "smart.rasterize");
        } else if (!active->isGroup) add("smart.convert");
        if (active->isLiveText()) {
            separate();
            add("layer.editText");
            add("type.workPath");
            add("type.shape");
        }
        if (active->mask) {
            separate();
            as(active->mask->enabled ? tr("Disable Layer Mask") : tr("Enable Layer Mask"), "mask.toggle");
            as(tr("Invert Layer Mask"), "mask.invert");
            as(tr("Apply Layer Mask"), "mask.apply");
            as(tr("Delete Layer Mask"), "mask.delete");
        } else {
            separate();
            as(tr("Add Layer Mask"), "mask.add");
        }
        break;
    }
    case Tool::Marquee: case Tool::Lasso: case Tool::Wand: case Tool::Scribble:
        if (hasSelection) {
            add("select.deselect");
            as(tr("Select Inverse"), "select.inverse");
            add("select.feather");
            separate();
            as(tr("Free Transform"), "edit.freeTransform", s->canTransformSelection());
            add("select.save");
            separate();
            add("layer.viaCopy", active && !active->isGroup);
            add("edit.cut");
            add("edit.copy");
            separate();
            add("edit.fillForeground");
            add("edit.fillBackground");
            add("edit.contentAwareFill", s->canAdjustPixels());
        } else {
            as(tr("Select All"), "select.all");
            add("select.reselect", s->canReselect());
            if (active) {
                separate();
                add("layer.duplicate");
                add("layer.merge", s->canMergeLayers());
            }
        }
        break;
    case Tool::Pen: case Tool::DirectSelect:
        canvas->addPathMenu(menu, view);
        break;
    default:
        break;
    }
    // No trailing separator.
    while (!menu->isEmpty() && menu->actions().constLast()->isSeparator()) menu->removeAction(menu->actions().constLast());
    return menu;
}

void MainWindow::showCanvasMenu(QPointF view) {
    if (!session_ || !canvas_ || !session_->hasDocument()) return;
    const QPoint global = canvas_->mapToGlobal(view.toPoint());
    // The Brush and Eraser: Photoshop's brush preset picker, at the pointer.
    if (session_->tool() == Tool::Brush && !canvas_->typeEditing() && !session_->transformEdit()) {
        if (options_) if (auto* picker = options_->findChild<BrushPicker*>()) { picker->showPickerAt(global); return; }
    }
    QMenu* menu = buildCanvasMenu(view, this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (menu->isEmpty()) { delete menu; return; }
    menu->popup(global);
}

} // namespace app
