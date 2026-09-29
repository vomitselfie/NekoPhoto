#include "Scrub.h"

#include <QAbstractSlider>
#include <QApplication>
#include <QBoxLayout>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QSpinBox>

#include <algorithm>
#include <cmath>

namespace app::scrub {

double scrubbedValue(double start, double pixels, double step, double min, double max, int decimals, Qt::KeyboardModifiers modifiers) {
    double factor = 1;
    if (modifiers & Qt::ShiftModifier) factor = 0.1;
    else if (modifiers & (Qt::AltModifier | Qt::ControlModifier)) factor = 10;
    double value = start + pixels * step * factor;
    const double scale = std::pow(10.0, std::clamp(decimals, 0, 9));
    value = std::round(value * scale) / scale;
    return std::clamp(value, min, max);
}

namespace {

std::function<bool(const QString&)> beginGroup;
std::function<void()> endGroup;

/// The layout that holds `w` directly, searched from `root` down.
QLayout* layoutHolding(QLayout* root, QWidget* w) {
    if (!root) return nullptr;
    if (root->indexOf(w) >= 0) return root;
    for (int i = 0; i < root->count(); i++)
        if (QLayout* child = root->itemAt(i)->layout())
            if (QLayout* found = layoutHolding(child, w)) return found;
    return nullptr;
}

/// The layout that holds the layout `inner` directly.
QLayout* parentLayoutOf(QLayout* root, QLayout* inner, int* index) {
    if (!root) return nullptr;
    for (int i = 0; i < root->count(); i++) {
        QLayout* child = root->itemAt(i)->layout();
        if (!child) continue;
        if (child == inner) { *index = i; return root; }
        if (QLayout* found = parentLayoutOf(child, inner, index)) return found;
    }
    return nullptr;
}

bool usableLabel(QLabel* label) {
    if (!label || label->text().trimmed().isEmpty()) return false;
    if ((label->textInteractionFlags() & Qt::TextSelectableByMouse) || label->text().contains(QLatin1String("<a "))) return false;
    return !label->property("scrubField").isValid();
}

/// Walks left from item `index` of a box layout: the first label, past at most one slider.
QLabel* leftOf(QBoxLayout* box, int index) {
    if (box->direction() != QBoxLayout::LeftToRight && box->direction() != QBoxLayout::RightToLeft) return nullptr;
    bool passedSlider = false;
    for (int i = index - 1; i >= 0; i--) {
        QLayoutItem* item = box->itemAt(i);
        if (item->spacerItem()) continue;
        QWidget* w = item->widget();
        if (!w) return nullptr;
        if (auto* label = qobject_cast<QLabel*>(w)) return label;
        if (qobject_cast<QAbstractSlider*>(w) && !passedSlider) { passedSlider = true; continue; }
        return nullptr;
    }
    return nullptr;
}

QLabel* leftInGrid(QGridLayout* grid, int index) {
    int row = 0, column = 0, rowSpan = 0, columnSpan = 0;
    grid->getItemPosition(index, &row, &column, &rowSpan, &columnSpan);
    bool passedSlider = false;
    for (int c = column - 1; c >= 0; c--) {
        QLayoutItem* item = grid->itemAtPosition(row, c);
        if (!item || item->spacerItem()) continue;
        QWidget* w = item->widget();
        if (!w) return nullptr;
        if (auto* label = qobject_cast<QLabel*>(w)) return label;
        if (qobject_cast<QAbstractSlider*>(w) && !passedSlider) { passedSlider = true; continue; }
        return nullptr;
    }
    return nullptr;
}

/// The label of the row a layout item sits in: left of it in a box or grid, or the form row's label.
QLabel* labelInLayout(QLayout* layout, int index, QLayoutItem* item) {
    if (auto* box = qobject_cast<QBoxLayout*>(layout)) return leftOf(box, index);
    if (auto* grid = qobject_cast<QGridLayout*>(layout)) return leftInGrid(grid, index);
    if (auto* form = qobject_cast<QFormLayout*>(layout)) {
        int row = 0;
        QFormLayout::ItemRole role{};
        form->getItemPosition(index, &row, &role);
        if (role != QFormLayout::FieldRole) return nullptr;
        QLayoutItem* labelItem = form->itemAt(row, QFormLayout::LabelRole);
        (void)item;
        return labelItem ? qobject_cast<QLabel*>(labelItem->widget()) : nullptr;
    }
    return nullptr;
}

/// True when only spacers and at most one slider stand before item `index` of a box layout (so its row's
/// label is outside the box).
bool firstInBox(QLayout* layout, int index) {
    auto* box = qobject_cast<QBoxLayout*>(layout);
    if (!box) return false;
    int sliders = 0;
    for (int i = 0; i < index; i++) {
        QLayoutItem* item = box->itemAt(i);
        if (item->spacerItem()) continue;
        if (qobject_cast<QAbstractSlider*>(item->widget()) && sliders++ == 0) continue;
        return false;
    }
    return true;
}

QLabel* findLabel(QAbstractSpinBox* field) {
    QWidget* parent = field->parentWidget();
    if (!parent) return nullptr;
    for (QLabel* label : parent->findChildren<QLabel*>(Qt::FindDirectChildrenOnly))
        if (label->buddy() == field && usableLabel(label)) return label;
    QLayout* root = parent->layout();
    QLayout* layout = layoutHolding(root, field);
    if (!layout) return nullptr;
    int index = layout->indexOf(field);
    if (QLabel* label = labelInLayout(layout, index, layout->itemAt(index)); usableLabel(label)) return label;
    // A field that opens its own row box (a form row's field, a grid cell) takes the label of that row.
    if (firstInBox(layout, index)) {
        int outerIndex = -1;
        if (QLayout* outer = parentLayoutOf(root, layout, &outerIndex))
            if (QLabel* label = labelInLayout(outer, outerIndex, outer->itemAt(outerIndex)); usableLabel(label)) return label;
    }
    return nullptr;
}

class Scrubber : public QObject {
public:
    using QObject::QObject;
    QHash<const QObject*, QPointer<QAbstractSpinBox>> fields;   // keyed by label

    void attach(QLabel* label, QAbstractSpinBox* field) {
        if (!label || !field || fields.contains(label)) return;
        if (!qobject_cast<QSpinBox*>(field) && !qobject_cast<QDoubleSpinBox*>(field)) return;
        fields.insert(label, field);
        label->setProperty("scrubField", true);
        field->setProperty("scrubLabel", true);
        label->setCursor(Qt::SizeHorCursor);
        connect(label, &QObject::destroyed, this, [this, label] {
            if (dragLabel_ == label) finishDrag();
            fields.remove(label);
        });
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        const QEvent::Type type = event->type();
        if (type == QEvent::Show) {
            if (auto* field = qobject_cast<QAbstractSpinBox*>(watched); field && !field->property("scrubScanned").isValid()) {
                field->setProperty("scrubScanned", true);
                if (!field->property("scrubLabel").isValid()) attach(findLabel(field), field);
            }
            return false;
        }
        if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease
            && type != QEvent::MouseButtonDblClick)
            return false;
        if (!fields.contains(watched)) return false;   // before any cast: every mouse event passes here
        auto* label = static_cast<QLabel*>(watched);
        QAbstractSpinBox* field = fields.value(label);
        if (!field || !field->isEnabled() || field->isReadOnly()) return false;
        auto* mouse = static_cast<QMouseEvent*>(event);
        const double x = mouse->globalPosition().x();
        switch (type) {
        case QEvent::MouseButtonPress:
            if (mouse->button() != Qt::LeftButton) return false;
            dragLabel_ = label;
            pressX_ = originX_ = x;
            startValue_ = valueOf(field);
            modifiers_ = mouse->modifiers();
            dragging_ = false;
            return true;
        case QEvent::MouseMove: {
            if (dragLabel_ != label || !(mouse->buttons() & Qt::LeftButton)) return false;
            if (!dragging_) {
                if (std::abs(x - pressX_) < QApplication::startDragDistance() / 2.0 + 1) return true;
                dragging_ = true;
                originX_ = x;
                QString name = label->text();
                name.remove('&');
                while (name.endsWith(':') || name.endsWith(QChar(0xFF1A))) name.chop(1);
                groupOpen_ = beginGroup && beginGroup(name.trimmed());
            }
            // A change of modifier mid-drag continues from where the value is, at the new speed.
            if (mouse->modifiers() != modifiers_) { startValue_ = valueOf(field); originX_ = x; modifiers_ = mouse->modifiers(); }
            setValueOf(field, x - originX_, modifiers_);
            return true;
        }
        case QEvent::MouseButtonRelease:
            if (dragLabel_ != label || mouse->button() != Qt::LeftButton) return false;
            if (dragging_) {
                finishDrag();
                QMetaObject::invokeMethod(field, "editingFinished");
            } else {
                dragLabel_ = nullptr;
                focusField(field);
            }
            return true;
        case QEvent::MouseButtonDblClick:
            focusField(field);
            return true;
        default: return false;
        }
    }

private:
    QLabel* dragLabel_ = nullptr;
    double pressX_ = 0, originX_ = 0, startValue_ = 0;
    Qt::KeyboardModifiers modifiers_;
    bool dragging_ = false, groupOpen_ = false;

    void finishDrag() {
        dragLabel_ = nullptr;
        dragging_ = false;
        if (groupOpen_) { groupOpen_ = false; if (endGroup) endGroup(); }
    }
    static void focusField(QAbstractSpinBox* field) {
        field->setFocus(Qt::MouseFocusReason);
        field->selectAll();
    }
    static double valueOf(QAbstractSpinBox* field) {
        if (auto* d = qobject_cast<QDoubleSpinBox*>(field)) return d->value();
        if (auto* i = qobject_cast<QSpinBox*>(field)) return i->value();
        return 0;
    }
    void setValueOf(QAbstractSpinBox* field, double pixels, Qt::KeyboardModifiers modifiers) const {
        if (auto* d = qobject_cast<QDoubleSpinBox*>(field))
            d->setValue(scrubbedValue(startValue_, pixels, d->singleStep(), d->minimum(), d->maximum(), d->decimals(), modifiers));
        else if (auto* i = qobject_cast<QSpinBox*>(field))
            i->setValue(int(scrubbedValue(startValue_, pixels, i->singleStep(), i->minimum(), i->maximum(), 0, modifiers)));
    }
};

Scrubber* scrubber = nullptr;

} // namespace

void install(QApplication& app) {
    if (scrubber) return;
    scrubber = new Scrubber(&app);
    app.installEventFilter(scrubber);
}

void attach(QLabel* label, QAbstractSpinBox* field) {
    if (scrubber) scrubber->attach(label, field);
}

void setUndoGroupHooks(std::function<bool(const QString&)> begin, std::function<void()> end) {
    beginGroup = std::move(begin);
    endGroup = std::move(end);
}

} // namespace app::scrub
