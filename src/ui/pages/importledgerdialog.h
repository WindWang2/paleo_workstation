// 层：视图
#pragma once
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "../../workflow/importledger.h" // LedgerBatch（数据面；加载由壳完成）

namespace paleo::dataops
{

// ui/pages/dataops/importledgerdialog — 导入台账查看器（方向 30：批次台账
// 可查）。纯渲染：批次列表左（时间/目录/四计数）、行明细右（路径/类型/实体/
// 结局/原因）；「复制明细」把当前批次的行级报告拷进剪贴板。台账数据由壳从
// workflow/importledger 读好灌入——本对话框不碰文件。
class ImportLedgerDialog : public QDialog
{
  Q_OBJECT
  public:
    explicit ImportLedgerDialog(QWidget *parent = nullptr)
      : QDialog(parent)
    {
      setObjectName(QStringLiteral("importLedgerDialog"));
      setWindowTitle(tr("导入台账"));
      setModal(true);
      resize(860, 500);
      auto *lay = new QVBoxLayout(this);
      m_summary = new QLabel(this);
      m_summary->setObjectName(QStringLiteral("ledgerSummary"));
      lay->addWidget(m_summary);
      auto *split = new QWidget(this);
      auto *hl = new QHBoxLayout(split);
      hl->setContentsMargins(0, 0, 0, 0);
      m_batches = new QListWidget(split);
      m_batches->setObjectName(QStringLiteral("ledgerBatches"));
      m_batches->setFixedWidth(260);
      m_rows = new QTableWidget(0, 5, split);
      m_rows->setObjectName(QStringLiteral("ledgerRows"));
      m_rows->setHorizontalHeaderLabels({tr("路径"), tr("类型"), tr("实体"), tr("结局"), tr("原因")});
      m_rows->setSelectionBehavior(QAbstractItemView::SelectRows);
      m_rows->setEditTriggers(QAbstractItemView::NoEditTriggers);
      m_rows->verticalHeader()->setVisible(false);
      m_rows->horizontalHeader()->setStretchLastSection(true);
      hl->addWidget(m_batches);
      hl->addWidget(m_rows, 1);
      lay->addWidget(split, 1);
      auto *row = new QWidget(this);
      auto *rl = new QHBoxLayout(row);
      rl->setContentsMargins(0, 0, 0, 0);
      m_copyBtn = new QPushButton(tr("复制明细"), row);
      m_copyBtn->setObjectName(QStringLiteral("ledgerCopyButton"));
      m_closeBtn = new QPushButton(tr("关闭"), row);
      rl->addWidget(m_copyBtn);
      rl->addStretch(1);
      rl->addWidget(m_closeBtn);
      lay->addWidget(row);
      connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
      connect(m_copyBtn, &QPushButton::clicked, this, [this] {
        const QString text = currentBatchText();
        if (!text.isEmpty())
          QApplication::clipboard()->setText(text);
      });
      connect(m_batches, &QListWidget::currentRowChanged, this,
              [this](int row) { fillRows(row); });
      m_summary->setText(tr("最近 %1 批（窗口滚动保留）——新批次在上。").arg(0));
    }

    void setBatches(const QVector<paleo::imports::LedgerBatch> &batchesAscending)
    {
      m_batchesAsc = batchesAscending;
      m_batches->clear();
      // 倒序展示：最新在上。
      for (int i = m_batchesAsc.size() - 1; i >= 0; --i)
      {
        const paleo::imports::LedgerBatch &b = m_batchesAsc.at(i);
        const QString label = tr("%1 · %2\n入库 %3 / 未决 %4 / 失败 %5 / 跳过 %6")
                                  .arg(b.finishedAt.toString(QStringLiteral("MM-dd hh:mm")),
                                       QDir(b.dir).dirName())
                                  .arg(b.imported)
                                  .arg(b.unresolved)
                                  .arg(b.failed)
                                  .arg(b.skipped);
        auto *it = new QListWidgetItem(label);
        it->setToolTip(b.dir);
        m_batches->addItem(it);
      }
      m_summary->setText(batchesAscending.isEmpty()
                             ? tr("暂无导入批次记录（首批导入后将在此留痕）。")
                             : tr("共 %1 批在窗口内。选中批次查看逐行结局；"
                                  "「复制明细」可把行级报告拷出。")
                                   .arg(batchesAscending.size()));
      if (m_batches->count() > 0)
        m_batches->setCurrentRow(0);
    }

    // 测试/取数出口：当前选中批次。
    paleo::imports::LedgerBatch currentBatch() const
    {
      const int idx = m_batches->currentRow();
      if (idx < 0 || idx >= m_batchesAsc.size())
        return paleo::imports::LedgerBatch();
      // 列表是倒序的：第 0 行 = 最新 = m_batchesAsc 最后一个。
      return m_batchesAsc.at(m_batchesAsc.size() - 1 - idx);
    }

  private:
    static QString outcomeLabel(const QString &outcome)
    {
      if (outcome == QLatin1String("imported"))
        return QObject::tr("入库");
      if (outcome == QLatin1String("unresolved"))
        return QObject::tr("未决");
      if (outcome == QLatin1String("failed"))
        return QObject::tr("失败");
      if (outcome == QLatin1String("skipped"))
        return QObject::tr("跳过");
      return outcome;
    }

    void fillRows(int listRow)
    {
      m_rows->setRowCount(0);
      if (listRow < 0 || listRow >= m_batches->count())
        return;
      const paleo::imports::LedgerBatch b = currentBatch();
      for (const paleo::imports::LedgerRow &r : b.rows)
      {
        const int i = m_rows->rowCount();
        m_rows->insertRow(i);
        const auto mk = [](const QString &t) {
          auto *it = new QTableWidgetItem(t);
          it->setFlags(it->flags() & ~Qt::ItemIsEditable);
          return it;
        };
        m_rows->setItem(i, 0, mk(r.path));
        m_rows->setItem(i, 1, mk(r.type));
        m_rows->setItem(i, 2, mk(r.entity));
        m_rows->setItem(i, 3, mk(outcomeLabel(r.outcome)));
        m_rows->setItem(i, 4, mk(r.message));
      }
    }

    QString currentBatchText() const
    {
      const paleo::imports::LedgerBatch b = currentBatch();
      if (b.id.isEmpty())
        return QString();
      QStringList lines;
      lines << tr("批次 %1").arg(b.id)
            << tr("目录：%1").arg(b.dir)
            << tr("时间：%1 → %2")
                   .arg(b.startedAt.toString(Qt::ISODate),
                        b.finishedAt.toString(Qt::ISODate))
            << tr("入库 %1 / 未决 %2 / 失败 %3 / 跳过 %4")
                   .arg(b.imported)
                   .arg(b.unresolved)
                   .arg(b.failed)
                   .arg(b.skipped);
      for (const paleo::imports::LedgerRow &r : b.rows)
        lines << QStringLiteral("[%1] %2 | %3 | %4 | %5")
                     .arg(outcomeLabel(r.outcome), r.path, r.type, r.entity, r.message);
      return lines.join(QLatin1Char('\n'));
    }

    QVector<paleo::imports::LedgerBatch> m_batchesAsc;
    QListWidget *m_batches = nullptr;
    QTableWidget *m_rows = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_copyBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
};

} // namespace paleo::dataops
