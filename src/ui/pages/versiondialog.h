// 层：视图
#pragma once
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include <functional>
#include <algorithm>
#include <QString>
#include <QVector>

#include "../../workflow/assetops.h" // VersionCompare（对比渲染类型；计算由壳注入）

namespace paleo::dataops
{

// ui/pages/dataops/versiondialog — 资产版本面对话框（方向 30：版本对比与
// 回滚入口）。纯渲染 + 意图出口：版本行数据与对比计算由壳（DataListPanel）
// 灌入/注入（compareHook 走 workflow/assetops::compareVersions），回滚经
// rollbackRequested 信号出壳。选两行 → 面板显示元数据差异 + 文本行级首差异
// 块；单选非当前行 → 「回滚到此版本」可用（当前版本行禁用——回滚到自身
// 无意义）。全部控件直用原生 Qt（DESIGN.md：不自绘 chrome）。
struct VersionRow
{
  QString versionId;
  int versionNumber = 0;
  QString stage;        // RAW | DERIVED | …
  bool managed = true;  // false = 外链
  QString fileName;
  qint64 sizeBytes = -1;
  QString sha256;       // 完整留底；表里缩写显示
  QString sourceUri;
  bool isCurrent = false; // 最高 versionNumber
  // 方向 47：realization 成员/统计面标签（壳侧自 extra 契约键填；
  // 「成员 #3」「成员均值」等——空 = 普通版本行）。
  QString memberNote;
};

inline QString versionSizeText(qint64 bytes)
{
  if (bytes < 0)
    return QObject::tr("未知");
  if (bytes < 1024)
    return QObject::tr("%1 B").arg(bytes);
  if (bytes < 1024 * 1024)
    return QObject::tr("%1 KB").arg(QString::number(bytes / 1024.0, 'f', 1));
  return QObject::tr("%1 MB").arg(QString::number(bytes / (1024.0 * 1024), 'f', 2));
}

class VersionTableDialog : public QDialog
{
  Q_OBJECT
  public:
    explicit VersionTableDialog(QWidget *parent = nullptr)
      : QDialog(parent)
    {
      setObjectName(QStringLiteral("versionTableDialog"));
      setWindowTitle(tr("资产版本"));
      setModal(true);
      resize(780, 520);
      auto *lay = new QVBoxLayout(this);
      m_title = new QLabel(this);
      m_title->setObjectName(QStringLiteral("versionDialogTitle"));
      lay->addWidget(m_title);
      m_table = new QTableWidget(0, 5, this);
      m_table->setObjectName(QStringLiteral("versionTable"));
      m_table->setHorizontalHeaderLabels({tr("版本"), tr("阶段"), tr("受管"), tr("大小"), tr("文件名")});
      m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
      m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
      m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
      m_table->verticalHeader()->setVisible(false);
      m_table->horizontalHeader()->setStretchLastSection(true);
      lay->addWidget(m_table, 2);
      m_compareSummary = new QLabel(this);
      m_compareSummary->setObjectName(QStringLiteral("versionCompareSummary"));
      m_compareSummary->setWordWrap(true);
      lay->addWidget(m_compareSummary);
      m_compareDetail = new QTextEdit(this);
      m_compareDetail->setObjectName(QStringLiteral("versionCompareDetail"));
      m_compareDetail->setReadOnly(true);
      m_compareDetail->setMaximumHeight(160);
      lay->addWidget(m_compareDetail, 1);
      auto *row = new QWidget(this);
      auto *rl = new QHBoxLayout(row);
      rl->setContentsMargins(0, 0, 0, 0);
      m_rollbackBtn = new QPushButton(tr("回滚到此版本"), row);
      m_rollbackBtn->setObjectName(QStringLiteral("versionRollbackButton"));
      m_closeBtn = new QPushButton(tr("关闭"), row);
      rl->addWidget(m_rollbackBtn);
      rl->addStretch(1);
      rl->addWidget(m_closeBtn);
      lay->addWidget(row);
      connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
      connect(m_rollbackBtn, &QPushButton::clicked, this, [this] {
        const QString id = selectedVersionId();
        if (!id.isEmpty())
          emit rollbackRequested(id);
      });
      connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        updateComparePanel();
        updateRollbackButton();
      });
      m_compareSummary->setText(tr("选中两行可对比；选中单行（非当前版本）可回滚。"));
    }

    void setAssetTitle(const QString &displayName)
    {
      m_title->setText(tr("「%1」的版本历史（回滚 = 产生指向旧内容的新版本，不删历史）")
                           .arg(displayName));
    }

    void setRows(const QVector<VersionRow> &rows)
    {
      m_rows = rows;
      std::sort(m_rows.begin(), m_rows.end(),
                [](const VersionRow &a, const VersionRow &b) {
                  return a.versionNumber > b.versionNumber; // 新版本在上
                });
      m_table->setRowCount(0);
      for (const VersionRow &r : m_rows)
      {
        const int i = m_table->rowCount();
        m_table->insertRow(i);
        auto *n = new QTableWidgetItem(
            tr("v%1%2%3").arg(r.versionNumber)
                .arg(r.isCurrent ? tr("（当前）") : QString())
                .arg(r.memberNote.isEmpty() ? QString()
                                            : tr("（%1）").arg(r.memberNote)));
        n->setData(Qt::UserRole, r.versionId);
        n->setFlags(n->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(i, 0, n);
        auto *st = new QTableWidgetItem(r.stage);
        st->setFlags(st->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(i, 1, st);
        auto *mg = new QTableWidgetItem(r.managed ? tr("受管") : tr("外链"));
        mg->setFlags(mg->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(i, 2, mg);
        auto *sz = new QTableWidgetItem(versionSizeText(r.sizeBytes));
        sz->setFlags(sz->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(i, 3, sz);
        auto *fn = new QTableWidgetItem(r.fileName + QStringLiteral("\n") + shortSha(r.sha256));
        fn->setFlags(fn->flags() & ~Qt::ItemIsEditable);
        fn->setToolTip(r.sha256.isEmpty() ? tr("无留底 SHA") : r.sha256);
        m_table->setItem(i, 4, fn);
      }
      updateRollbackButton();
    }

    void setCompareHook(const std::function<paleo::assetops::VersionCompare(
                            const QString &, const QString &)> &hook)
    {
      m_compareHook = hook;
    }

    // 回滚成功后的行刷新 + 成功回执文案。
    void setRowsRefreshed(const QVector<VersionRow> &rows, const QString &note)
    {
      setRows(rows);
      m_compareSummary->setText(note);
    }

    QString selectedVersionId() const
    {
      const QList<QTableWidgetItem *> sel = m_table->selectedItems();
      if (sel.size() != m_table->columnCount() || sel.isEmpty())
        return QString();
      return sel.first()->data(Qt::UserRole).toString();
    }

  signals:
    void rollbackRequested(const QString &versionId); // NOLINT(readability-inconsistent-declaration-parameter-name)

  private:
    static QString shortSha(const QString &sha)
    {
      return sha.isEmpty() ? QObject::tr("SHA: —") : QObject::tr("SHA: %1…").arg(sha.left(12));
    }

    QList<QString> selectedVersionIds() const
    {
      QList<QString> ids;
      for (QTableWidgetItem *it : m_table->selectedItems())
      {
        const QString id = it->data(Qt::UserRole).toString();
        if (!id.isEmpty() && !ids.contains(id))
          ids << id;
      }
      return ids;
    }

    void updateComparePanel()
    {
      m_compareDetail->clear();
      const QList<QString> ids = selectedVersionIds();
      if (ids.size() != 2 || !m_compareHook)
      {
        m_compareSummary->setText(
            ids.size() >= 2 ? tr("版本对比尚未接入")
                            : tr("选中两行可对比；选中单行（非当前版本）可回滚。"));
        return;
      }
      const paleo::assetops::VersionCompare c = m_compareHook(ids.at(0), ids.at(1));
      QString summary;
      if (!c.shaKnown)
        summary = tr("至少一侧无留底 SHA——内容一致性无法判定；");
      else if (c.sameSha)
        summary = tr("两版本字节内容一致（SHA 相同）；");
      else
        summary = tr("两版本内容不同；");
      summary += tr("大小 %1 ↔ %2。").arg(versionSizeText(c.sizeA), versionSizeText(c.sizeB));
      if (c.textCompared)
        summary += tr(" 文本首差异块：%1 行 ↔ %2 行。").arg(c.linesA).arg(c.linesB);
      else
        summary += tr(" （非文本格式或超限，不做行级对比）");
      m_compareSummary->setText(summary);

      QString detail;
      for (const auto &d : c.fieldDiffs)
        detail += tr("【%1】%2 → %3\n").arg(d.field, d.a, d.b);
      if (c.textCompared && !c.differingLines.isEmpty())
      {
        detail += QLatin1Char('\n') + tr("—— 首差异块（“-” 旧 / “+” 新）——") + QLatin1Char('\n');
        detail += c.differingLines.join(QLatin1Char('\n'));
      }
      m_compareDetail->setPlainText(detail);
    }

    void updateRollbackButton()
    {
      const QList<QString> ids = selectedVersionIds();
      bool enabled = ids.size() == 1;
      if (enabled)
      {
        for (const VersionRow &r : m_rows)
          if (r.versionId == ids.first() && r.isCurrent)
            enabled = false; // 回滚到当前版本无意义（catalog 亦拒绝）
      }
      m_rollbackBtn->setEnabled(enabled);
      m_rollbackBtn->setToolTip(enabled
                                    ? tr("新增一条内容与目标版本一致的新版本；历史全保留")
                                    : tr("请单选一个非当前版本"));
    }

    QVector<VersionRow> m_rows;
    std::function<paleo::assetops::VersionCompare(const QString &, const QString &)>
        m_compareHook;
    QLabel *m_title = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_compareSummary = nullptr;
    QTextEdit *m_compareDetail = nullptr;
    QPushButton *m_rollbackBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
};

} // namespace paleo::dataops
