// swatches: the tool rail's foreground and background swatches show the colours through the monitor profile from the
// space they are values in: the document's profile in RGB, sRGB in CMYK and Lab (as the core keeps stored colours).
// A CMYK document's colours went through its CMYK profile as if they were RGB values. The monitor here is Adobe RGB
// (a profile file, as Color Settings takes one), so every conversion shows.
#include "SelfTestSupport.h"
#include "ColorManagement.h"
#include "ColorSwatches.h"
#include "EditorSession.h"
#include "MainWindow.h"
#include "nekophoto/colormgmt.h"
#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

using namespace nekophoto;

namespace app {

int swatches(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    w.resize(1400, 900);
    w.show();
    QApplication::processEvents();
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "swatches: %s\n", what); failures++; } };
    auto* swatch = w.findChild<ColorSwatches*>();
    QTemporaryDir dir;
    QFile icc(dir.filePath(QStringLiteral("monitor.icc")));
    const ColorProfile monitor = builtinProfile(WorkingSpace::AdobeRGB);
    if (!swatch || !dir.isValid() || !icc.open(QIODevice::WriteOnly) || icc.write(reinterpret_cast<const char*>(monitor.icc.data()), qint64(monitor.icc.size())) != qint64(monitor.icc.size())) {
        std::fprintf(stderr, "swatches: no swatches, or the monitor profile could not be written\n");
        return 1;
    }
    icc.close();
    const color::Settings before = color::settings();
    color::Settings settings = before;
    settings.monitorFile = icc.fileName();
    color::setSettings(settings);
    expect(color::monitorProfile() == monitor, "the monitor profile file was not taken");

    // Each channel within one level of the colour converted from `from` to the monitor.
    auto shows = [&](const QColor& shown, const QColor& color, const ColorProfile& from) {
        double rgb[3] = {color.redF(), color.greenF(), color.blueF()};
        convertColor(from, monitor, rgb);
        const QColor want = QColor::fromRgbF(float(rgb[0]), float(rgb[1]), float(rgb[2]));
        const bool ok = std::abs(shown.red() - want.red()) <= 1 && std::abs(shown.green() - want.green()) <= 1 && std::abs(shown.blue() - want.blue()) <= 1;
        if (!ok) std::fprintf(stderr, "swatches: shows %s for %s, wants %s\n", qPrintable(shown.name()), qPrintable(color.name()), qPrintable(want.name()));
        return ok;
    };
    auto setColours = [&](const QColor& fg, const QColor& bg) {
        s.foregroundColor = fg;
        s.backgroundColor = bg;
        emit s.toolChanged();
        emit s.layersChanged();   // the window repaints the swatches
        QApplication::processEvents();
    };

    // RGB: the document's profile (the demo is untagged: sRGB) to the monitor.
    setColours(QColor(200, 40, 60), QColor(30, 160, 90));
    expect(shows(swatch->foreground(), s.foregroundColor, s.document()->profile) && shows(swatch->background(), s.backgroundColor, s.document()->profile),
           "in RGB the swatches do not show the colours from the document's profile");
    // CMYK: the colours are sRGB values (converted through the press's gamut with the document), not CMYK ones.
    QString error;
    expect(s.convertColorMode(ColorMode::CMYK, &error) && s.colorMode() == ColorMode::CMYK, "the demo did not convert to CMYK");
    setColours(QColor(200, 40, 60), QColor(30, 160, 90));
    expect(shows(swatch->foreground(), s.foregroundColor, ColorProfile()) && shows(swatch->background(), s.backgroundColor, ColorProfile()),
           "in CMYK the swatches do not show the colours as sRGB");
    // Lab, likewise.
    expect(s.convertColorMode(ColorMode::Lab, &error) && s.colorMode() == ColorMode::Lab, "the demo did not convert to Lab");
    setColours(QColor(200, 40, 60), QColor(30, 160, 90));
    expect(shows(swatch->foreground(), s.foregroundColor, ColorProfile()) && shows(swatch->background(), s.backgroundColor, ColorProfile()),
           "in Lab the swatches do not show the colours as sRGB");

    color::setSettings(before);
    std::printf("swatches: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

} // namespace app
