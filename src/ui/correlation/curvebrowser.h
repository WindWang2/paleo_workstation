#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "../../io/lasparser.h"

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

// ui/correlation/ — CurveBrowser: LAS curve browser panel.
//
// Lists every mnemonic of the current well's LAS file (from io/LasParser)
// with unit + description, one checkable row per curve. Checking a row
// asks the section to ADD a track for that mnemonic in the well's column;
// unchecking asks to REMOVE it. Multi-select is inherent (any number of
// rows checked). Pure Qt Widgets per DESIGN.md (Qgs* stays out of the
// panel chrome); the browser never touches the scene itself — the panel
// owns composition and honors the requests.
class CurveBrowser : public QWidget
{
  Q_OBJECT
  public:
    explicit CurveBrowser(QWidget *parent = nullptr);

    // Replaces the listing for `wellId` (DEPT included — the depth channel
    // is a legitimate track choice on sections with mixed step files).
    // Check states of mnemonics that survive the replacement are kept.
    void setCurves(const QString &wellId, const QList<LasCurve> &curves);
    QString wellId() const { return m_wellId; }
    bool hasCurves() const { return m_rows > 0; }

    QStringList mnemonics() const;             // listing order
    QStringList checkedMnemonics() const;
    bool isChecked(const QString &mnemonic) const;

    // Programmatic check (tests / restore-after-relist). Emits
    // mnemonicToggled exactly like a user click. Re-asserting the state a
    // row already has is a silent no-op; an unknown mnemonic likewise.
    void setChecked(const QString &mnemonic, bool on);

  signals:
    // `on` = add the track, off = remove it. wellId is the browser's
    // current well — repeated for convenience when one slot serves
    // several browsers.
    void mnemonicToggled(const QString &wellId, const QString &mnemonic, bool on);

  private:
    void onItemChanged(QTreeWidgetItem *item, int column);
    QTreeWidgetItem *itemFor(const QString &mnemonic) const;

    QString m_wellId;
    QTreeWidget *m_list = nullptr;
    QLabel *m_wellLabel = nullptr;    // "井：W1" — checks act on this well
    QLabel *m_emptyLabel = nullptr;   // §42.4 guidance when no curves
    int m_rows = 0;
};
