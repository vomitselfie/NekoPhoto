// What the options bar's pages share (ToolOptionsBar.cpp and ToolOptions<Group>.cpp, one per group of tools): a
// number field, a thin rule between groups of controls, and a row to lay a page out in.
#pragma once
#include "Style.h"
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QString>
#include <QWidget>

namespace app::tooloptions {

inline QDoubleSpinBox* numberField(double min, double max, int decimals, const QString& suffix, const QString& tip) {
    auto* f = new QDoubleSpinBox;
    f->setRange(min, max);
    f->setDecimals(decimals);
    f->setSuffix(suffix);
    f->setToolTip(tip);
    f->setKeyboardTracking(false);
    f->setButtonSymbols(QAbstractSpinBox::NoButtons);
    f->setAlignment(Qt::AlignRight);
    f->setFixedWidth(suffix.isEmpty() ? 64 : 82);
    return f;
}

/// A thin vertical rule between groups of controls.
inline QWidget* separator() {
    auto* line = new QWidget;
    line->setFixedSize(1, 18);
    line->setStyleSheet(QStringLiteral("background: %1;").arg(hintColor(3).name()));
    return line;
}

inline QWidget* row() {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(6, 2, 6, 2);
    h->setSpacing(8);
    return w;
}

inline QHBoxLayout* layoutOf(QWidget* w) { return static_cast<QHBoxLayout*>(w->layout()); }

} // namespace app::tooloptions
