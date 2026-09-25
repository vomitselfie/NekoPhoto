#include "WelcomeDialog.h"
#include "Style.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace app {

namespace {

const QColor accent(255, 61, 154);   // the splash's pink

/// The splash scaled to `width` logical pixels, or the slice of it a page shows as its banner: pages pan
/// across the picture from left to right.
QPixmap splashPixmap(int width, double dpr, int page = -1, int pages = 1, int bannerHeight = 0) {
    static const QPixmap source(QStringLiteral(":/images/welcome.jpg"));
    if (source.isNull()) return {};
    if (page < 0) {
        QPixmap scaled = source.scaledToWidth(int(width * dpr), Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        return scaled;
    }
    // Wider than the banner so there is room to pan, cut at the band just under the logo.
    const int wide = int(width * 1.5 * dpr);
    QPixmap scaled = source.scaledToWidth(wide, Qt::SmoothTransformation);
    const int h = int(bannerHeight * dpr), w = int(width * dpr);
    const int x = pages > 1 ? (wide - w) * page / (pages - 1) : 0;
    const int y = std::clamp(int(scaled.height() * 0.42) - h / 2, 0, std::max(0, scaled.height() - h));
    QPixmap slice = scaled.copy(x, y, w, h);
    slice.setDevicePixelRatio(dpr);
    return slice;
}

QLabel* heading(const QString& text, int size) {
    auto* label = new QLabel(text);
    QFont f = label->font();
    f.setPointSizeF(size);
    f.setBold(true);
    label->setFont(f);
    label->setWordWrap(true);
    return label;
}

QLabel* body(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    return label;
}

/// A feature row: an accent tick and the text.
QWidget* item(const QString& text) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(10);
    auto* tick = new QLabel(QStringLiteral("✓"));
    tick->setStyleSheet(QStringLiteral("color: %1; font-weight: bold;").arg(accent.name()));
    tick->setAlignment(Qt::AlignTop);
    layout->addWidget(tick);
    layout->addWidget(body(text), 1);
    return row;
}

} // namespace

bool WelcomeDialog::shown() { return QSettings().value("welcome/shown", false).toBool(); }
void WelcomeDialog::markShown() { QSettings().setValue("welcome/shown", true); }

WelcomeDialog::WelcomeDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Welcome to NekoPhoto"));
    // However it closes (Skip, Start, Esc, the window's close button), it has been seen.
    connect(this, &QDialog::finished, this, [] { markShown(); });
    QSize size(860, 640);
    if (QScreen* screen = parent ? parent->screen() : nullptr) size = size.boundedTo(screen->availableGeometry().size() * 0.9);
    resize(size);
    const int inner = size.width() - 48;
    const double dpr = devicePixelRatioF();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 16);
    pages_ = new QStackedWidget;
    layout->addWidget(pages_, 1);

    // The splash.
    {
        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(12);
        auto* picture = new QLabel;
        picture->setPixmap(splashPixmap(inner, dpr));
        picture->setAlignment(Qt::AlignCenter);
        v->addWidget(picture);
        v->addWidget(heading(tr("Bring your work with you."), 20));
        v->addWidget(body(tr("NekoPhoto opens the files and brushes you already have, from Photoshop, Clip Studio Paint, Procreate "
                             "and more. A few pages on what comes along, or skip ahead and start.")));
        v->addStretch(1);
        pages_->addWidget(page);
    }

    struct Platform { QString title, subtitle; QStringList items; };
    const QVector<Platform> platforms{
        {tr("Coming from Photoshop"), tr("Your files, your brushes, your muscle memory."),
         {tr("Open <b>.psd</b> and <b>.psb</b> files with their layers, folders, masks, clipping masks and blend modes, and export back to layered <b>.psd</b>"),
          tr("Import your <b>.abr</b> brushes"),
          tr("The shortcuts you know: V, M, L, W, B, E, [ and ], Ctrl+T, Ctrl+J, Ctrl+G, Ctrl+Alt+G"),
          tr("Adjustment layers (Levels, Curves, Hue/Saturation, Exposure, Gradient Map), Content-Aware Fill and Spot Healing")}},
        {tr("Coming from Clip Studio Paint"), tr("Your projects and your brushes."),
         {tr("Open <b>.clip</b> projects with their layers, folders, masks, clipping and blend modes"),
          tr("Import your <b>.sut</b> brushes: their tips come along as brushes that follow your pen"),
          tr("Clipping masks, folders and layer masks work the way you expect"),
          tr("Quick Select by a scribble or a single click, for masking off flats and fills")}},
        {tr("Coming from Procreate"), tr("Bring your brush sets."),
         {tr("Import <b>.brushset</b> and <b>.brush</b> files"),
          tr("196 MyPaint brushes (pencils, inks, charcoal, paint, smudging) that follow pen pressure and tilt"),
          tr("Canvases of up to a gigapixel of layers, with crash recovery if anything goes wrong"),
          tr("Export to PNG, JPEG, WebP and TIFF")}},
        {tr("Coming from Krita or GIMP"), tr("Familiar engines in a lighter app."),
         {tr("The MyPaint brush engine that Krita and GIMP offer too"),
          tr("Over 850 G'MIC filters with a live preview, when <b>gmic</b> is installed"),
          tr("Photoshop and Clip Studio files open with their layers, and layered PSD goes back out, for working with collaborators"),
          tr("Runs natively on Wayland and X11")}},
    };
    const int bannerHeight = 150, pageCount = int(platforms.size()) + 2;
    auto addPlatformPage = [&](const QString& title, const QString& subtitle, const QStringList& items, int index) {
        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(6);
        auto* banner = new QLabel;
        banner->setPixmap(splashPixmap(inner, dpr, index - 1, pageCount - 1, bannerHeight));
        banner->setFixedHeight(bannerHeight);
        v->addWidget(banner);
        v->addSpacing(10);
        v->addWidget(heading(title, 18));
        auto* sub = body(subtitle);
        sub->setStyleSheet(hintStyle());
        v->addWidget(sub);
        v->addSpacing(8);
        for (const QString& text : items) v->addWidget(item(text));
        v->addStretch(1);
        pages_->addWidget(page);
        return v;
    };
    for (int i = 0; i < platforms.size(); i++) addPlatformPage(platforms[i].title, platforms[i].subtitle, platforms[i].items, i + 1);

    // Ways to start.
    {
        QVBoxLayout* v = addPlatformPage(tr("Ready when you are"), tr("A few more things you might like:"),
            {tr("<b>Remove Background</b> with an AI model that runs on your machine; nothing is uploaded (turn it on in Edit > Preferences)"),
             tr("Several projects in tabs, and your work recovered if the app ever closes unexpectedly"),
             tr("AI agents such as Claude Code can drive the editor for you")},
            pageCount - 1);
        delete v->takeAt(v->count() - 1);   // the stretch; the buttons go under the list
        v->addSpacing(14);
        auto* actions = new QHBoxLayout;
        auto addAction = [&](const QString& label, auto signal) {
            auto* button = new QPushButton(label);
            connect(button, &QPushButton::clicked, this, [this, signal] { accept(); emit (this->*signal)(); });
            actions->addWidget(button);
        };
        addAction(tr("Open a File…"), &WelcomeDialog::openRequested);
        addAction(tr("New Canvas…"), &WelcomeDialog::newCanvasRequested);
        addAction(tr("Import Brushes…"), &WelcomeDialog::importBrushesRequested);
        actions->addStretch(1);
        v->addLayout(actions);
        v->addStretch(1);
    }

    // Skip, the page dots, Back and Next.
    auto* bar = new QHBoxLayout;
    skip_ = new QPushButton(tr("Skip Intro"));
    skip_->setFlat(true);
    connect(skip_, &QPushButton::clicked, this, &QDialog::reject);
    QSizePolicy keep = skip_->sizePolicy();
    keep.setRetainSizeWhenHidden(true);   // the dots stay put when it hides on the last page
    skip_->setSizePolicy(keep);
    bar->addWidget(skip_);
    bar->addStretch(1);
    for (int i = 0; i < pages_->count(); i++) {
        auto* dot = new QLabel(QStringLiteral("●"));
        dots_.push_back(dot);
        bar->addWidget(dot);
    }
    bar->addStretch(1);
    back_ = new QPushButton(tr("Back"));
    next_ = new QPushButton;
    next_->setDefault(true);
    connect(back_, &QPushButton::clicked, this, [this] { go(pages_->currentIndex() - 1); });
    connect(next_, &QPushButton::clicked, this, [this] {
        if (pages_->currentIndex() + 1 < pages_->count()) go(pages_->currentIndex() + 1);
        else accept();
    });
    bar->addWidget(back_);
    bar->addWidget(next_);
    layout->addLayout(bar);
    go(0);
}

void WelcomeDialog::go(int page) {
    page = std::clamp(page, 0, pages_->count() - 1);
    pages_->setCurrentIndex(page);
    for (int i = 0; i < dots_.size(); i++)
        dots_[i]->setStyleSheet(i == page ? QStringLiteral("color: %1;").arg(accent.name()) : hintStyle());
    back_->setEnabled(page > 0);
    const bool last = page == pages_->count() - 1;
    next_->setText(last ? tr("Start") : tr("Next"));
    skip_->setVisible(!last);
}

} // namespace app
