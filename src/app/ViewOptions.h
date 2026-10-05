// The View menu's guide and snapping switches (Photoshop's View > Show, Lock Guides, Snap and Snap To), shared by
// every tab and kept between runs. The guides themselves belong to the document (compositor/guides.h).
#pragma once
#include <QSettings>

namespace app {

struct ViewOptions {
    bool showGuides = true;
    bool lockGuides = false;
    bool smartGuides = true;   // the magenta lines that show a layer or the canvas being snapped to
    bool snap = true;          // View > Snap: off, nothing snaps
    bool snapToGuides = true;
    bool snapToLayers = true;
    bool snapToBounds = true;  // the canvas's edges and centre

    static ViewOptions& get() {
        static ViewOptions options = load();
        return options;
    }
    static ViewOptions load() {
        QSettings s;
        ViewOptions o;
        o.showGuides = s.value("view/showGuides", true).toBool();
        o.lockGuides = s.value("view/lockGuides", false).toBool();
        o.smartGuides = s.value("view/smartGuides", true).toBool();
        o.snap = s.value("view/snap", true).toBool();
        o.snapToGuides = s.value("view/snapToGuides", true).toBool();
        o.snapToLayers = s.value("view/snapToLayers", true).toBool();
        o.snapToBounds = s.value("view/snapToBounds", true).toBool();
        return o;
    }
    void save() const {
        QSettings s;
        s.setValue("view/showGuides", showGuides);
        s.setValue("view/lockGuides", lockGuides);
        s.setValue("view/smartGuides", smartGuides);
        s.setValue("view/snap", snap);
        s.setValue("view/snapToGuides", snapToGuides);
        s.setValue("view/snapToLayers", snapToLayers);
        s.setValue("view/snapToBounds", snapToBounds);
    }
};

} // namespace app
