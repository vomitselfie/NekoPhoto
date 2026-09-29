// Scrubby labels: which label a field finds, and what a drag on it does (offscreen Qt Widgets).
#include "check.h"
#include "Scrub.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

using namespace app;

namespace {

void application() {
    static int argc = 1;
    static char name[] = "scrub_tests";
    static char* argv[] = {name, nullptr};
    static QApplication* instance = nullptr;
    if (instance) return;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    instance = new QApplication(argc, argv);
    scrub::install(*instance);
}

void mouse(QWidget* w, QEvent::Type type, double x, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent e(type, QPointF(x, 5), QPointF(x, 5), button, buttons, modifiers);
    QApplication::sendEvent(w, &e);
}

bool scrubs(QLabel* label) { return label && label->property("scrubField").isValid(); }

} // namespace

TEST_CASE(values_follow_step_range_and_modifiers) {
    CHECK_NEAR(scrub::scrubbedValue(10, 5, 1, 0, 100, 0, Qt::NoModifier), 15, 1e-9);
    CHECK_NEAR(scrub::scrubbedValue(10, 5, 1, 0, 100, 1, Qt::ShiftModifier), 10.5, 1e-9);
    CHECK_NEAR(scrub::scrubbedValue(10, 5, 1, 0, 100, 0, Qt::ControlModifier), 60, 1e-9);
    CHECK_NEAR(scrub::scrubbedValue(10, 5, 1, 0, 100, 0, Qt::AltModifier), 60, 1e-9);
    CHECK_NEAR(scrub::scrubbedValue(10, 500, 1, 0, 100, 0, Qt::NoModifier), 100, 1e-9);
    CHECK_NEAR(scrub::scrubbedValue(10, -500, 1, 0, 100, 0, Qt::NoModifier), 0, 1e-9);
    CHECK_NEAR(scrub::scrubbedValue(0.5, 3, 0.01, 0, 1, 2, Qt::NoModifier), 0.53, 1e-9);
    // An integer field under Shift moves one unit every ten pixels.
    CHECK_NEAR(scrub::scrubbedValue(10, 4, 1, 0, 100, 0, Qt::ShiftModifier), 10, 1e-9);
    CHECK_NEAR(scrub::scrubbedValue(10, 16, 1, 0, 100, 0, Qt::ShiftModifier), 12, 1e-9);
}

TEST_CASE(labels_are_found_in_rows_forms_and_grids) {
    application();
    QWidget window;
    auto* column = new QVBoxLayout(&window);
    auto* row = new QHBoxLayout;
    auto* sizeLabel = new QLabel("Size");
    row->addWidget(sizeLabel);
    row->addWidget(new QSpinBox);
    column->addLayout(row);
    auto* form = new QFormLayout;
    auto* sliderRow = new QHBoxLayout;
    sliderRow->addWidget(new QSlider(Qt::Horizontal));
    sliderRow->addWidget(new QDoubleSpinBox);
    form->addRow("Amount", sliderRow);
    column->addLayout(form);
    auto* grid = new QGridLayout;
    auto* radiusLabel = new QLabel("Radius");
    grid->addWidget(radiusLabel, 0, 0);
    grid->addWidget(new QSlider(Qt::Horizontal), 0, 1);
    grid->addWidget(new QDoubleSpinBox, 0, 2);
    auto* lonely = new QSpinBox;
    grid->addWidget(lonely, 1, 2);
    column->addLayout(grid);
    auto* note = new QLabel("A note");
    column->addWidget(note);
    window.show();
    CHECK(scrubs(sizeLabel));
    CHECK(scrubs(qobject_cast<QLabel*>(form->itemAt(0, QFormLayout::LabelRole)->widget())));
    CHECK(scrubs(radiusLabel));
    CHECK(!scrubs(note));
    CHECK(!lonely->property("scrubLabel").isValid());
}

TEST_CASE(a_drag_changes_the_value_in_one_group_and_a_click_edits) {
    application();
    int begun = 0, ended = 0;
    QString groupName;
    scrub::setUndoGroupHooks([&](const QString& name) { begun++; groupName = name; return true; }, [&] { ended++; });
    QWidget window;
    auto* row = new QHBoxLayout(&window);
    auto* label = new QLabel("Opacity:");
    auto* field = new QSpinBox;
    field->setRange(0, 100);
    field->setValue(50);
    row->addWidget(label);
    row->addWidget(field);
    window.show();
    REQUIRE(scrubs(label));
    int changes = 0;
    QObject::connect(field, &QSpinBox::valueChanged, [&](int) { changes++; });

    mouse(label, QEvent::MouseButtonPress, 100);
    mouse(label, QEvent::MouseMove, 110);   // past the threshold: the drag starts here
    mouse(label, QEvent::MouseMove, 130);
    CHECK_EQ(field->value(), 70);
    mouse(label, QEvent::MouseMove, 140, Qt::ShiftModifier);   // Shift: a tenth from here on
    mouse(label, QEvent::MouseMove, 160, Qt::ShiftModifier);
    CHECK_EQ(field->value(), 72);
    mouse(label, QEvent::MouseMove, 161, Qt::ControlModifier);
    mouse(label, QEvent::MouseMove, 171, Qt::ControlModifier);
    CHECK_EQ(field->value(), 100);   // clamped to the range
    mouse(label, QEvent::MouseButtonRelease, 171);
    CHECK_EQ(begun, 1);
    CHECK_EQ(ended, 1);
    CHECK(groupName == "Opacity");
    CHECK(changes > 1);

    // A click without a drag leaves the value and opens no group.
    mouse(label, QEvent::MouseButtonPress, 50);
    mouse(label, QEvent::MouseButtonRelease, 50);
    CHECK_EQ(field->value(), 100);
    CHECK_EQ(begun, 1);

    // A disabled field ignores its label.
    field->setEnabled(false);
    mouse(label, QEvent::MouseButtonPress, 50);
    mouse(label, QEvent::MouseMove, 20);
    mouse(label, QEvent::MouseButtonRelease, 20);
    CHECK_EQ(field->value(), 100);
    scrub::setUndoGroupHooks({}, {});
}

TEST_MAIN()
