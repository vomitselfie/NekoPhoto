// EditorSession: Quick Select: scribbles (GrabCut) and click prompts (EfficientSAM), run off the UI thread.
#include "EditorSession.h"
#include "ModelStore.h"
#include "compositor/scribble.h"
#include "compositor/subject.h"
#include <algorithm>
#include <thread>

using namespace compositor;

namespace app {

namespace {

/// Stamps discs of the stroke's width along its polyline into the label map.
void stampScribble(GrayImage& labels, const EditorSession::Scribble& stroke, uint8_t value) {
    const int w = labels.width(), h = labels.height();
    const double r = std::max(0.5, stroke.size / 2);
    auto disc = [&](QPointF c) {
        const int x0 = std::max(0, int(std::floor(c.x() - r))), x1 = std::min(w - 1, int(std::ceil(c.x() + r)));
        const int y0 = std::max(0, int(std::floor(c.y() - r))), y1 = std::min(h - 1, int(std::ceil(c.y() + r)));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                const double dx = x + 0.5 - c.x(), dy = y + 0.5 - c.y();
                if (dx * dx + dy * dy <= r * r) labels.at(x, y) = value;
            }
    };
    if (stroke.points.empty()) return;
    disc(stroke.points.front());
    for (size_t i = 1; i < stroke.points.size(); i++) {
        const QPointF a = stroke.points[i - 1], b = stroke.points[i];
        const double length = std::hypot(b.x() - a.x(), b.y() - a.y());
        const int steps = std::max(1, int(std::ceil(length / std::max(1.0, r / 2))));
        for (int k = 1; k <= steps; k++) disc(a + (b - a) * (double(k) / steps));
    }
}

/// The panel's refinement for a Quick Select result: pulled onto the image's edges, hazed values pushed apart,
/// specks dropped; no matting and no colour change (the selection is what comes out, not pixels).
std::shared_ptr<GrayImage> refinedQuickSelect(const GrayImage& coverage, const Image& composite, int refine) {
    if (refine <= 0) return std::make_shared<GrayImage>(coverage);
    MatteSettings settings;
    settings.refineEdges = refine;
    settings.contrast = 25;
    settings.matting = 0;
    settings.cleanup = true;
    settings.decontaminate = false;
    return refineMatte(coverage, composite, settings, 0);
}

} // namespace

std::shared_ptr<const Image> EditorSession::flattenedForSampling() {
    if (!(wandSample_ && wandSampleAll_ && wandSampleRevision_ == documentRevision_)) {
        wandSample_ = renderFlattened(*document_);
        wandSampleAll_ = true; wandSampleLayer_ = Uuid{}; wandSampleRevision_ = documentRevision_;
    }
    return wandSample_;
}

void EditorSession::addScribble(const std::vector<QPointF>& points, bool background, bool run) {
    if (!document_ || points.empty()) return;
    scribbles_.push_back({points, double(std::max(1, scribbleSize)), background});
    emit scribblesChanged();
    if (run) startQuickSelectJob();
}

void EditorSession::removeLastScribble() {
    if (scribbles_.empty()) return;
    scribbles_.pop_back();
    emit scribblesChanged();
    if (!scribbles_.empty()) startQuickSelectJob();
}

void EditorSession::setQuickSelectClicks(bool clicks) {
    if (quickSelectClicks == clicks) return;
    quickSelectClicks = clicks;
    emit scribblesChanged();
}

void EditorSession::addClickPrompt(QPointF at, bool background, bool run) {
    if (!document_) return;
    clickPrompts_.push_back({at, background ? 0 : 1});
    emit scribblesChanged();
    if (run) startQuickSelectJob();
}

void EditorSession::setClickBox(QPointF a, QPointF b, bool run) {
    if (!document_) return;
    // One box at a time: an earlier one goes.
    clickPrompts_.erase(std::remove_if(clickPrompts_.begin(), clickPrompts_.end(), [](const ClickPrompt& p) { return p.label >= 2; }), clickPrompts_.end());
    clickPrompts_.push_back({QPointF(std::min(a.x(), b.x()), std::min(a.y(), b.y())), 2});
    clickPrompts_.push_back({QPointF(std::max(a.x(), b.x()), std::max(a.y(), b.y())), 3});
    emit scribblesChanged();
    if (run) startQuickSelectJob();
}

void EditorSession::removeLastClickPrompt() {
    if (clickPrompts_.empty()) return;
    // A box is two prompts.
    const bool box = clickPrompts_.back().label >= 2;
    clickPrompts_.pop_back();
    if (box && !clickPrompts_.empty() && clickPrompts_.back().label >= 2) clickPrompts_.pop_back();
    emit scribblesChanged();
    if (!clickPrompts_.empty()) startQuickSelectJob();
}

void EditorSession::clearClickPrompts() {
    if (clickPrompts_.empty()) return;
    clickPrompts_.clear();
    emit scribblesChanged();
}

namespace {

std::vector<PointPrompt> promptsOf(const std::vector<EditorSession::ClickPrompt>& clicks) {
    std::vector<PointPrompt> out;
    for (const EditorSession::ClickPrompt& c : clicks) out.push_back({c.at.x(), c.at.y(), c.label});
    return out;
}

} // namespace

bool EditorSession::runClickSelection(SelectionMode mode, QString* error) {
    if (!document_) return false;
    if (!ModelStore::promptReady()) { if (error) *error = tr("The click-to-select model is not downloaded: choose the Click engine in the Quick Select options and download it, or scribble instead."); return false; }
    std::shared_ptr<const Image> composite = flattenedForSampling();
    std::string why;
    auto coverage = subjectFromPrompts(*composite, ModelStore::pathFor(ModelStore::promptModel()).toStdString(), promptsOf(clickPrompts_), &why);
    if (!coverage) { if (error) *error = QString::fromStdString(why); return false; }
    coverage = refinedQuickSelect(*coverage, *composite, scribbleRefine);
    applySelectionShape(*coverage, mode, "Select Subject");
    return true;
}

void EditorSession::startQuickSelectJob() {
    if (!document_) return;
    if (quickSelectBusy_) { quickSelectAgain_ = true; return; }
    struct Inputs {
        std::shared_ptr<const Image> composite;
        GrayImage labels;
        std::vector<PointPrompt> prompts;
        bool clicks = false;
        std::string model;
        int refine = 0;
        uint64_t revision = 0;
        int width = 0, height = 0;
    };
    auto in = std::make_shared<Inputs>();
    in->clicks = quickSelectClicks;
    in->refine = scribbleRefine;
    if (quickSelectClicks) {
        if (clickPrompts_.empty()) return;
        if (!ModelStore::promptReady()) { emit quickSelectFailed(tr("The click-to-select model is not downloaded: use the Download button in the options bar, or the Scribble engine.")); return; }
        in->prompts = promptsOf(clickPrompts_);
        in->model = ModelStore::pathFor(ModelStore::promptModel()).toStdString();
    } else {
        if (scribbles_.empty()) return;
        in->labels = GrayImage(document_->width, document_->height, 0);
        for (const Scribble& stroke : scribbles_) stampScribble(in->labels, stroke, stroke.background ? 2 : 1);
    }
    in->composite = flattenedForSampling();
    in->revision = documentRevision_;
    in->width = document_->width;
    in->height = document_->height;
    quickSelectBusy_ = true;
    emit quickSelectBusyChanged(true);
    if (quickSelectThread_.joinable()) quickSelectThread_.join();
    quickSelectThread_ = std::thread([this, in] {
        std::string why;
        std::shared_ptr<GrayImage> coverage;
        if (in->clicks) coverage = subjectFromPrompts(*in->composite, in->model, in->prompts, &why);
        else coverage = scribbleSelection(*in->composite, in->labels, 450, 2, &why);
        if (coverage) coverage = refinedQuickSelect(*coverage, *in->composite, in->refine);
        QMetaObject::invokeMethod(this, [this, coverage, why, in] {
            quickSelectBusy_ = false;
            emit quickSelectBusyChanged(false);
            // The document moved on while the job ran (an edit, a crop, another document): the result
            // describes pixels that are gone and may not even fit the canvas, so compute it again.
            if (!document_ || documentRevision_ != in->revision || document_->width != in->width || document_->height != in->height) {
                quickSelectAgain_ = false;
                startQuickSelectJob();
                return;
            }
            if (coverage) applySelectionShape(*coverage, SelectionMode::Replace, in->clicks ? "Select Subject" : "Quick Select");
            else emit quickSelectFailed(QString::fromStdString(why));
            if (quickSelectAgain_) { quickSelectAgain_ = false; startQuickSelectJob(); }
        }, Qt::QueuedConnection);
    });
}

void EditorSession::clearScribbles() {
    if (scribbles_.empty()) return;
    scribbles_.clear();
    emit scribblesChanged();
}

bool EditorSession::runScribbleSelection(SelectionMode mode, QString* error) {
    if (!document_) return false;
    GrayImage labels(document_->width, document_->height, 0);
    for (const Scribble& stroke : scribbles_) stampScribble(labels, stroke, stroke.background ? 2 : 1);
    std::shared_ptr<const Image> composite = flattenedForSampling();
    std::string why;
    // GrabCut's cost grows fast with size (a 12 MP test image: 4 s at 400 px, 22 s at 700); the guided refine
    // at full size restores the edge, so the segmentation runs small.
    auto coverage = scribbleSelection(*composite, labels, 450, 2, &why);
    if (!coverage) { if (error) *error = QString::fromStdString(why); return false; }
    coverage = refinedQuickSelect(*coverage, *composite, scribbleRefine);
    applySelectionShape(*coverage, mode, "Quick Select");
    return true;
}

} // namespace app
