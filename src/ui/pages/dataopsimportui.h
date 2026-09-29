// 层：视图
// ui/pages/dataopsimportui — D8 导入流增强的 UI 面。
//   · ImportQueuePanel：逐文件进度 + 单项取消/重试（D8.1/D8.2）
//   · ImportReportDialog：导入摘要报告（可复制）（D8.5）
//   · ImportPresetDialog：常用目录/类型映射存预设（D8.3）
//   · DuplicateDialog：SHA/同名提示与处置（跳过/重命名/覆盖→派生版）（D8.4）
//   · ImportEstimateDialog：拖入大目录的预估与分批确认（D8.6）
// 执行钩子注入：面板不直接调 workflow——start 时对每个 Queued 条目调用
// 注入的 runner（测试注入假钩子；生产由壳接 FolderImportWorkflow）。
#pragma once

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "../paleotheme.h"
#include "dataops/dataopsimportlogic.h"

namespace paleo::dataops
{

// ---- D8.5 导入摘要报告 ---------------------------------------------------------
class ImportReportDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ImportReportDialog(const QString &report, QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("importReportDialog"));
    setWindowTitle(tr("导入摘要"));
    setModal(true);
    resize(520, 320);
    auto *lay = new QVBoxLayout(this);
    auto *text = new QPlainTextEdit(report, this);
    text->setObjectName(QStringLiteral("importReportText"));
    text->setReadOnly(true);
    lay->addWidget(text, 1);
    auto *row = new QWidget(this);
    auto *rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    auto *copy = new QPushButton(tr("复制"), row);
    copy->setObjectName(QStringLiteral("importReportCopy"));
    connect(copy, &QPushButton::clicked, this, [text] {
      QGuiApplication::clipboard()->setText(text->toPlainText());
    });
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close, row);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    rl->addWidget(copy);
    rl->addStretch(1);
    rl->addWidget(box);
    lay->addWidget(row);
  }
};

// ---- D8.1/D8.2 导入队列面板 ----------------------------------------------------
class ImportQueuePanel : public QWidget
{
  Q_OBJECT

public:
  // runner：执行一条导入（应驱动队列 markRunning/markProgress/markDone/
  // markFailed；同步执行直接落状态，异步由进度回调驱动）。
  using Runner = std::function<void(int index, ImportQueueItem &item, ImportRetryQueue *queue)>;

  explicit ImportQueuePanel(QWidget *parent = nullptr)
    : QWidget(parent)
  {
    setObjectName(QStringLiteral("importQueuePanel"));
    hide(); // 空队列不占位
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);
    auto *head = new QWidget(this);
    auto *hl = new QHBoxLayout(head);
    hl->setContentsMargins(0, 0, 0, 0);
    auto *cap = new QLabel(tr("导入队列"), head);
    PaleoTheme::applyThemedStyleSheet(cap,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    hl->addWidget(cap);
    m_count = new QLabel(head);
    m_count->setObjectName(QStringLiteral("importQueueCount"));
    PaleoTheme::applyThemedStyleSheet(m_count,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    hl->addWidget(m_count);
    hl->addStretch(1);
    m_report = new QPushButton(tr("摘要"), head);
    m_report->setObjectName(QStringLiteral("importReportButton"));
    m_clear = new QPushButton(tr("清已完成"), head);
    m_clear->setObjectName(QStringLiteral("importClearDoneButton"));
    m_close = new QToolButton(head);
    m_close->setText(QStringLiteral("×"));
    m_close->setObjectName(QStringLiteral("importQueueClose"));
    m_close->setToolTip(tr("收起队列面板"));
    hl->addWidget(m_report);
    hl->addWidget(m_clear);
    hl->addWidget(m_close);
    lay->addWidget(head);

    m_table = new QTableWidget(0, 4, this);
    m_table->setObjectName(QStringLiteral("importQueueTable"));
    m_table->setHorizontalHeaderLabels({tr("文件"), tr("状态"), tr("进度"), tr("操作")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->setColumnWidth(0, 260);
    m_table->setColumnWidth(1, 70);
    m_table->setColumnWidth(2, 90);
    m_table->setMaximumHeight(180);
    lay->addWidget(m_table);

    connect(m_report, &QPushButton::clicked, this, [this] {
      ImportReportDialog dlg(m_queue.summaryText(), this);
      dlg.exec();
    });
    connect(m_clear, &QPushButton::clicked, this, [this] {
      m_queue.clearFinished();
      refresh();
    });
    connect(m_close, &QToolButton::clicked, this, [this] {
      if (!m_queue.hasPending())
        hide();
      else if (QMessageBox::question(this, tr("导入进行中"),
                                     tr("还有未完成项，仍要收起面板？")) == QMessageBox::Yes)
        hide();
    });
    // RetryWait → Queued 的自动驱动拍（D8.2 自动重试）。
    m_timer = new QTimer(this);
    m_timer->setInterval(1500);
    connect(m_timer, &QTimer::timeout, this, [this] {
      if (m_queue.promoteRetryWaiters() > 0)
      {
        refresh();
        runPending();
      }
    });
  }

  void setRunner(Runner r) { m_runner = std::move(r); }

  // D3.2/D8.6 入口：外部拖入或导入按钮路径进队列（paths 全量，预估与
  // 批确认由装配方先行）。
  void enqueuePaths(const QStringList &paths, const QHash<QString, QString> &typeByExt)
  {
    for (const QString &p : paths)
    {
      ImportQueueItem it;
      it.path = p;
      it.type = typeByExt.value(QFileInfo(p).suffix().toLower());
      m_queue.enqueue(it);
    }
    show();
    refresh();
    runPending();
    if (!m_timer->isActive())
      m_timer->start();
  }

  const ImportRetryQueue &queue() const { return m_queue; }

public slots:
  // 驱动所有 Queued 条目（同步 runner 一次跑完；异步 runner 自行回状态）。
  void runPending()
  {
    if (!m_runner)
      return;
    const QVector<ImportQueueItem> snapshot = m_queue.items();
    for (int i = 0; i < snapshot.size(); ++i)
      if (snapshot.at(i).state == ImportItemState::Queued)
      {
        ImportQueueItem *it = m_queue.at(i);
        if (it && it->state == ImportItemState::Queued)
          m_runner(i, *it, &m_queue);
      }
    refresh();
    if (!m_queue.hasPending())
      m_timer->stop();
  }

  void refresh()
  {
    const QVector<ImportQueueItem> items = m_queue.items();
    m_table->setRowCount(0);
    // 清理旧行控件（进度条在 cellWidget）。
    for (int r = 0; r < m_table->rowCount(); ++r)
      if (QWidget *w = m_table->cellWidget(r, 2))
        w->deleteLater();
    int pending = 0;
    for (int i = 0; i < items.size(); ++i)
    {
      const ImportQueueItem &it = items.at(i);
      if (it.state == ImportItemState::Queued || it.state == ImportItemState::Running ||
          it.state == ImportItemState::RetryWait)
        ++pending;
      const int r = m_table->rowCount();
      m_table->insertRow(r);
      auto *name = new QTableWidgetItem(QFileInfo(it.path).fileName());
      name->setFlags(name->flags() & ~Qt::ItemIsEditable);
      name->setToolTip(it.path);
      m_table->setItem(r, 0, name);
      auto *st = new QTableWidgetItem(ImportQueueItem::stateText(it.state));
      st->setFlags(st->flags() & ~Qt::ItemIsEditable);
      if (it.state == ImportItemState::Failed || it.state == ImportItemState::Canceled)
        st->setForeground(PaleoTheme::tokens().error);
      else if (it.state == ImportItemState::Done)
        st->setForeground(PaleoTheme::tokens().success);
      m_table->setItem(r, 1, st);
      auto *bar = new QProgressBar(m_table);
      bar->setObjectName(QStringLiteral("importItemProgress"));
      bar->setRange(0, 100);
      bar->setValue(it.progressPercent);
      bar->setStyleSheet(
          QStringLiteral("QProgressBar { max-height: 12px; }"));
      m_table->setCellWidget(r, 2, bar);
      // 操作列：取消（进行/排队）/ 重试+跳过（失败/取消）。
      auto *ops = new QWidget(m_table);
      auto *ol = new QHBoxLayout(ops);
      ol->setContentsMargins(2, 0, 2, 0);
      ol->setSpacing(2);
      if (it.state == ImportItemState::Queued || it.state == ImportItemState::Running ||
          it.state == ImportItemState::RetryWait)
      {
        auto *cancel = new QPushButton(tr("取消"), ops);
        cancel->setObjectName(QStringLiteral("importItemCancel"));
        const int idx = i;
        connect(cancel, &QPushButton::clicked, this, [this, idx] {
          m_queue.cancelItem(idx);
          refresh();
        });
        ol->addWidget(cancel);
      }
      else if (it.state == ImportItemState::Failed || it.state == ImportItemState::Canceled)
      {
        auto *retry = new QPushButton(tr("重试"), ops);
        retry->setObjectName(QStringLiteral("importItemRetry"));
        auto *skip = new QPushButton(tr("跳过"), ops);
        skip->setObjectName(QStringLiteral("importItemSkip"));
        const int idx = i;
        connect(retry, &QPushButton::clicked, this, [this, idx] {
          m_queue.retryItem(idx);
          refresh();
          runPending();
        });
        connect(skip, &QPushButton::clicked, this, [this, idx] {
          if (ImportQueueItem *it2 = m_queue.at(idx))
            it2->state = ImportItemState::Skipped;
          refresh();
        });
        ol->addWidget(retry);
        ol->addWidget(skip);
      }
      ol->addStretch(1);
      m_table->setCellWidget(r, 3, ops);
    }
    m_count->setText(tr("%1 项 · 待处理 %2").arg(items.size()).arg(pending));
    setVisible(items.size() > 0);
  }

private:
  Runner m_runner;
  ImportRetryQueue m_queue;
  QTimer *m_timer = nullptr;
  QTableWidget *m_table = nullptr;
  QLabel *m_count = nullptr;
  QPushButton *m_report = nullptr;
  QPushButton *m_clear = nullptr;
  QToolButton *m_close = nullptr;
};

// ---- D8.3 导入预设对话框 --------------------------------------------------------
class ImportPresetDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ImportPresetDialog(QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("importPresetDialog"));
    setWindowTitle(tr("导入预设"));
    setModal(true);
    resize(520, 380);
    auto *lay = new QVBoxLayout(this);
    m_list = new QTreeWidget(this);
    m_list->setObjectName(QStringLiteral("importPresetList"));
    m_list->setHeaderLabels({tr("预设"), tr("目录数"), tr("类型映射")});
    m_list->setRootIsDecorated(false);
    lay->addWidget(m_list, 1);
    auto *row = new QWidget(this);
    auto *rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    auto *save = new QPushButton(tr("把当前设置存为预设"), row);
    save->setObjectName(QStringLiteral("importPresetSave"));
    auto *del = new QPushButton(tr("删除选中预设"), row);
    del->setObjectName(QStringLiteral("importPresetDelete"));
    auto *close = new QPushButton(tr("关闭"), row);
    rl->addWidget(save);
    rl->addWidget(del);
    rl->addStretch(1);
    rl->addWidget(close);
    lay->addWidget(row);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, [this] { emit saveRequested(); });
    connect(del, &QPushButton::clicked, this, [this] {
      QTreeWidgetItem *it = m_list->currentItem();
      if (it)
        emit deleteRequested(it->text(0));
    });
  }
  void reload(const QList<ImportPreset> &presets)
  {
    m_list->clear();
    for (const ImportPreset &p : presets)
    {
      auto *it = new QTreeWidgetItem(m_list);
      it->setText(0, p.name);
      it->setText(1, QString::number(p.dirs.size()));
      QStringList tm;
      for (auto k = p.typeMap.constBegin(); k != p.typeMap.constEnd(); ++k)
        tm << QStringLiteral("%1→%2").arg(k.key(), k.value());
      it->setText(2, tm.join(QStringLiteral(", ")));
    }
  }

signals:
  void saveRequested();
  void deleteRequested(const QString &name);

private:
  QTreeWidget *m_list = nullptr;
};

// ---- D8.4 重复检测处置对话框 ----------------------------------------------------
// 命中列表 + 逐条处置（跳过/重命名/覆盖→派生版）+ 「全部按此处置」。
class DuplicateDialog : public QDialog
{
  Q_OBJECT
public:
  explicit DuplicateDialog(QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("duplicateDialog"));
    setWindowTitle(tr("重复文件检测"));
    setModal(true);
    resize(560, 360);
    auto *lay = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("以下文件与库内资产重复（SHA-256 或同名）。"
                               "逐条选择处置方式："), this);
    hint->setWordWrap(true);
    lay->addWidget(hint);
    m_table = new QTableWidget(0, 3, this);
    m_table->setObjectName(QStringLiteral("duplicateTable"));
    m_table->setHorizontalHeaderLabels({tr("新文件"), tr("命中"), tr("处置")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->setColumnWidth(0, 210);
    m_table->setColumnWidth(1, 110);
    lay->addWidget(m_table, 1);
    auto *row = new QWidget(this);
    auto *rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    auto *allSkip = new QPushButton(tr("全部跳过"), row);
    auto *allRename = new QPushButton(tr("全部重命名"), row);
    auto *allDerive = new QPushButton(tr("全部作新版本"), row);
    rl->addWidget(allSkip);
    rl->addWidget(allRename);
    rl->addWidget(allDerive);
    rl->addStretch(1);
    lay->addWidget(row);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
    const auto applyAll = [this](DuplicateResolution r) {
      for (int i = 0; i < m_table->rowCount(); ++i)
        setRowResolution(i, r);
    };
    connect(allSkip, &QPushButton::clicked, this, [applyAll] { applyAll(DuplicateResolution::Skip); });
    connect(allRename, &QPushButton::clicked, this, [applyAll] { applyAll(DuplicateResolution::Rename); });
    connect(allDerive, &QPushButton::clicked, this, [applyAll] { applyAll(DuplicateResolution::Derive); });
  }

  void loadHits(const QVector<DuplicateHit> &hits)
  {
    m_hits = hits;
    m_table->setRowCount(0);
    for (const DuplicateHit &h : hits)
    {
      const int r = m_table->rowCount();
      m_table->insertRow(r);
      auto *n = new QTableWidgetItem(QFileInfo(h.incomingPath).fileName());
      n->setToolTip(h.incomingPath);
      n->setFlags(n->flags() & ~Qt::ItemIsEditable);
      m_table->setItem(r, 0, n);
      auto *hit = new QTableWidgetItem(h.reason == QLatin1String("sha")
                                           ? tr("SHA 相同")
                                           : tr("同名"));
      hit->setToolTip(h.existingPath);
      hit->setFlags(hit->flags() & ~Qt::ItemIsEditable);
      m_table->setItem(r, 1, hit);
      auto *combo = new QComboBox(m_table);
      combo->setObjectName(QStringLiteral("duplicateResolutionCombo"));
      combo->addItem(tr("跳过"), int(DuplicateResolution::Skip));
      combo->addItem(tr("重命名导入"), int(DuplicateResolution::Rename));
      combo->addItem(tr("覆盖 → 派生新版本"), int(DuplicateResolution::Derive));
      m_table->setCellWidget(r, 2, combo);
    }
  }
  void setRowResolution(int r, DuplicateResolution res)
  {
    if (auto *combo = qobject_cast<QComboBox *>(m_table->cellWidget(r, 2)))
    {
      const int idx = combo->findData(int(res));
      if (idx >= 0)
        combo->setCurrentIndex(idx);
    }
  }
  // 结果：path → 处置。
  QHash<QString, DuplicateResolution> resolutions() const
  {
    QHash<QString, DuplicateResolution> out;
    for (int r = 0; r < m_table->rowCount(); ++r)
      if (auto *combo = qobject_cast<QComboBox *>(m_table->cellWidget(r, 2)))
        out.insert(m_hits.at(r).incomingPath,
                   DuplicateResolution(combo->currentData().toInt()));
    return out;
  }

private:
  QVector<DuplicateHit> m_hits;
  QTableWidget *m_table = nullptr;
};

// ---- D8.6 大目录预估 + 分批确认 -------------------------------------------------
class ImportEstimateDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ImportEstimateDialog(QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("importEstimateDialog"));
    setWindowTitle(tr("导入预估"));
    setModal(true);
    auto *lay = new QVBoxLayout(this);
    m_text = new QLabel(this);
    m_text->setWordWrap(true);
    lay->addWidget(m_text);
    m_all = new QRadioButton(tr("一次全部导入"), this);
    m_batch = new QRadioButton(tr("分批导入（每批 200 个文件）"), this);
    m_all->setChecked(true);
    lay->addWidget(m_all);
    lay->addWidget(m_batch);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
  }
  void setEstimate(const ImportEstimate &est)
  {
    m_text->setText(tr("目录内共 %1 个文件 · 约 %2 MB%3\n扩展名分布：%4")
                        .arg(est.fileCount)
                        .arg(est.totalBytes / (1024 * 1024))
                        .arg(est.largeFileCount > 0
                                 ? tr("（其中 %1 个 >100MB）").arg(est.largeFileCount)
                                 : QString())
                        .arg(est.byExtension.join(QStringLiteral(", "))));
  }
  bool batchChosen() const { return m_batch->isChecked(); }

private:
  QLabel *m_text = nullptr;
  QRadioButton *m_all = nullptr;
  QRadioButton *m_batch = nullptr;
};

} // namespace paleo::dataops
