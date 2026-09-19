// A font family picker that scales to a Linux font list: families sharing a leading name (the two
// hundred Noto variants, the DejaVu and Liberation sets) fold into one row showing a representative,
// expandable to the full set; a filter box narrows the list as you type; recently chosen families sit
// on top. A button shows the current family and drops the picker below it.
#pragma once
#include "FontGroups.h"
#include <QToolButton>
#include <QString>
#include <QStringList>

class QLineEdit;
class QTreeView;
class QStandardItemModel;
class QStandardItem;
class QFrame;

namespace app {

class FontPicker : public QToolButton {
    Q_OBJECT
public:
    explicit FontPicker(QWidget* parent = nullptr);
    QString family() const { return family_; }
    void setFamily(const QString& family);

    /// Drops the picker (what a click does), for scripts and screenshots.
    void showPicker();

signals:
    void familyChanged(const QString& family);

private:
    void openPopup();
    void closePopup();
    void rebuild(const QString& filter);
    void choose(const QString& family);
    QStandardItem* itemFor(const FontGroup& node, const QString& filter, bool& anyMatch);
    bool eventFilter(QObject* watched, QEvent* event) override;

    QString family_;
    QFrame* popup_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QTreeView* tree_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    std::vector<FontGroup> nodes_;
    QStringList recent_;
};

} // namespace app
