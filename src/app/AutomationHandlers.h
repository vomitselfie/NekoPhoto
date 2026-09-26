// Shared by the files that register the automation methods (Automation*.cpp): request errors, parameter
// access, JSON views of the model, and the lookups every handler starts from. Not part of the app's
// interface; include Automation.h for that.
#pragma once
#include "EditorSession.h"
#include "MainWindow.h"
#include "compositor/document.h"
#include "compositor/filters.h"
#include "compositor/selection.h"
#include "compositor/transform.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace app::rpc {

struct RpcError : std::runtime_error {
    int code;
    RpcError(int c, const QString& message) : std::runtime_error(message.toStdString()), code(c) {}
};
constexpr int invalidParams = -32602, methodNotFound = -32601, appError = -32000;

[[noreturn]] void fail(const QString& message, int code = appError);

// ---- Parameter access ---------------------------------------------------------------------------

bool has(const QJsonObject& p, const char* key);
double num(const QJsonObject& p, const char* key, std::optional<double> fallback = std::nullopt);
int integer(const QJsonObject& p, const char* key, std::optional<int> fallback = std::nullopt);
QString str(const QJsonObject& p, const char* key, std::optional<QString> fallback = std::nullopt);
bool flag(const QJsonObject& p, const char* key, bool fallback);
QJsonObject obj(const QJsonObject& p, const char* key);
QString qs(const std::string& s);
compositor::SelectionMode selectionMode(const QJsonObject& p);
/// Every adjustment kind's name, comma separated (for messages).
QString adjustmentKindList();
std::optional<compositor::AdjustmentKind> adjustmentKindNamed(QString name);
std::optional<compositor::FilterKind> filterKindNamed(QString name);
/// "Color Dodge", "color dodge", "colorDodge" and "color-dodge" alike.
std::optional<compositor::BlendMode> blendModeNamed(const QString& name);
QStringList blendModeNames();
/// Nearest, Smooth or High quality, by any case or prefix ("high", "near").
std::optional<compositor::Sampling> samplingNamed(const QString& name);

// ---- JSON views of the model --------------------------------------------------------------------

QJsonObject rectJson(const compositor::Rect& r);
QJsonObject transformJson(const compositor::LayerTransform& t);
QJsonObject layerJson(const compositor::Layer& layer, int depth);
/// A text style from request parameters over `base`: text, font, size, bold, italic, color (CSS), align
/// (left, center, right), lineSpacing, letterSpacing.
compositor::LayerText textFromParams(const QJsonObject& p, compositor::LayerText base);
QJsonArray layersJson(const compositor::Document& doc);
QString base64Png(const compositor::Image& image);
/// `image` as PNG: written to params.path when given (result carries the path), else base64 in "png".
QJsonObject deliverPng(const compositor::Image& image, const QJsonObject& p, QJsonObject result);
std::shared_ptr<compositor::Image> scaledCopy(const compositor::Image& image, double maxSize);

// ---- Method descriptions (AutomationDescriptions.cpp) --------------------------------------------

/// Bumped when a method changes in a way a client could notice; app.info reports it.
constexpr int protocolVersion = 2;
/// The names tool.select takes.
const QStringList& toolNames();
bool isDescribed(const QString& method);
/// A method's summary and parameters with types, defaults and valid values; an error for an unknown method.
QJsonObject describeMethod(const QString& method);
/// Every described method's one-line summary.
QJsonObject describeAll();
/// Empty when every key of `params` is one the method takes, else a message naming the ones it does.
QString unknownParameter(const QString& method, const QJsonObject& params);
/// Empty when every required parameter is there, else a message naming the first one missing.
QString missingParameter(const QString& method, const QJsonObject& params);

// ---- What a handler works on --------------------------------------------------------------------
// Small callables rather than functions so a handler can capture them by value, as `[document, layer]`.

/// The current tab's session.
struct SessionOf {
    MainWindow* w;
    EditorSession* operator()() const { return w->session(); }
};

/// The current tab's document; an error when none is open.
struct DocumentOf {
    MainWindow* w;
    const compositor::Document& operator()() const;
};

/// The layer whose id is in `key`; an error when there is none.
struct LayerOf {
    DocumentOf document;
    const compositor::Layer& operator()(const QJsonObject& p, const char* key = "id") const;
};

/// The layer in "id", or the active layer when there is no id.
struct LayerOrActive {
    MainWindow* w;
    const compositor::Layer& operator()(const QJsonObject& p) const;
};

/// Runs `f` with `id` as the active layer, restoring the previous selection afterwards.
struct WithActive {
    MainWindow* w;
    template <class F> void operator()(const compositor::Uuid& id, F f) const {
        EditorSession* s = w->session();
        auto previous = s->activeLayerId();
        bool previousMask = s->isMaskSelected();
        if (previous != id) s->selectLayer(id);
        f();
        if (previous && previous != id && s->document() && s->document()->find(*previous)) s->selectLayer(previous, previousMask);
    }
};

} // namespace app::rpc
