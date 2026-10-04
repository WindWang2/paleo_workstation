// 层：视图
#include "cataloghealthdialog.h"

#include "../paleotheme.h"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace
{

// 分类展示序（回收站积压行殿后，由壳传入计数）。
struct CategoryRow
{
  paleo::health::IssueKind kind;
  const char *label;
};

const CategoryRow kCategoryRows[] = {
    {paleo::health::IssueKind::MissingFile, QT_TRANSLATE_NOOP("CatalogHealthDialog", "缺失文件")},
    {paleo::health::IssueKind::ShaMismatch, QT_TRANSLATE_NOOP("CatalogHealthDialog", "SHA 不一致")},
    {paleo::health::IssueKind::PendingLink, QT_TRANSLATE_NOOP("CatalogHealthDialog", "未决链接")},
    {paleo::health::IssueKind::OrphanEntity, QT_TRANSLATE_NOOP("CatalogHealthDialog", "孤立实体")},
    {paleo::health::IssueKind::NoVersionAsset, QT_TRANSLATE_NOOP("CatalogHealthDialog", "无版本资产")},
    {paleo::health::IssueKind::InvalidRoleLink, QT_TRANSLATE_NOOP("CatalogHealthDialog", "角色词表违例")},
    {paleo::health::IssueKind::StaleVersion, QT_TRANSLATE_NOOP("CatalogHealthDialog", "过时版本")},
};
constexpr int kCategoryCount = sizeof(kCategoryRows) / sizeof(kCategoryRows[0]);
constexpr int kRecycleRow = kCategoryCount; // 列表第 kCategoryCount 行 = 回收站积压

QString formatBytes(qint64 bytes)
{
  if (bytes < 1024)
    return QObject::tr("%1 B").arg(bytes);
  if (bytes < 1024 * 1024)
    return QObject::tr("%1 KB").arg(QString::number(bytes / 1024.0, 'f', 1));
  if (bytes < 1024LL * 1024 * 1024)
    return QObject::tr("%1 MB").arg(QString::number(bytes / (1024.0 * 1024), 'f', 1));
  return QObject::tr("%1 GB").arg(QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 2));
}

} // namespace

CatalogHealthDialog::CatalogHealthDialog(QWidget *parent)
  : QDialog(parent)
{
  setObjectName(QStringLiteral("catalogHealthDialog"));
  setWindowTitle(tr("工区资产体检"));
  setModal(true);
  resize(760, 480);

  auto *lay = new QVBoxLayout(this);
  m_summary = new QLabel(this);
  m_summary->setObjectName(QStringLiteral("healthSummary"));
  m_summary->setWordWrap(true);
  lay->addWidget(m_summary);

  auto *split = new QWidget(this);
  auto *hl = new QHBoxLayout(split);
  hl->setContentsMargins(0, 0, 0, 0);
  m_categories = new QListWidget(split);
  m_categories->setObjectName(QStringLiteral("healthCategories"));
  m_categories->setFixedWidth(180);
  m_issues = new QTableWidget(0, 2, split);
  m_issues->setObjectName(QStringLiteral("healthIssues"));
  m_issues->setHorizontalHeaderLabels({tr("对象"), tr("明细")});
  m_issues->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_issues->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_issues->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_issues->verticalHeader()->setVisible(false);
  m_issues->horizontalHeader()->setStretchLastSection(true);
  hl->addWidget(m_categories);
  hl->addWidget(m_issues, 1);
  lay->addWidget(split, 1);

  m_shaState = new QLabel(this);
  m_shaState->setObjectName(QStringLiteral("healthShaState"));
  m_shaState->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(
      m_shaState, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  lay->addWidget(m_shaState);

  auto *row = new QWidget(this);
  auto *rl = new QHBoxLayout(row);
  rl->setContentsMargins(0, 0, 0, 0);
  m_refreshBtn = new QPushButton(tr("重新体检"), row);
  m_refreshBtn->setObjectName(QStringLiteral("healthRefreshButton"));
  m_verifyBtn = new QPushButton(tr("校验外链 SHA-256"), row);
  m_verifyBtn->setObjectName(QStringLiteral("healthVerifyButton"));
  m_closeBtn = new QPushButton(tr("关闭"), row);
  rl->addWidget(m_refreshBtn);
  rl->addWidget(m_verifyBtn);
  rl->addStretch(1);
  rl->addWidget(m_closeBtn);
  lay->addWidget(row);

  connect(m_refreshBtn, &QPushButton::clicked, this, [this] { emit refreshRequested(); });
  // 校验按钮双态：空闲发 verifyShaRequested；运行中壳调 setVerifyRunning(true)
  // 切文案，点击改发取消——壳自己停调度循环。
  connect(m_verifyBtn, &QPushButton::clicked, this, [this] {
    if (m_verifyBtn->property("paleo.running").toBool())
      emit cancelVerifyRequested();
    else
      emit verifyShaRequested();
  });
  connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
  connect(m_categories, &QListWidget::currentRowChanged, this, [this](int row) {
    m_selectedKind = row;
    fillIssueTable();
  });
  connect(m_issues, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
    const QString assetId = m_issues->item(row, 0) ? m_issues->item(row, 0)
                                                         ->data(Qt::UserRole + 1)
                                                         .toString()
                                                   : QString();
    const QString entityId = m_issues->item(row, 0) ? m_issues->item(row, 0)
                                                          ->data(Qt::UserRole + 2)
                                                          .toString()
                                                    : QString();
    const QString versionId = m_issues->item(row, 0) ? m_issues->item(row, 0)->data(Qt::UserRole + 3).toString() : QString();
    if (!versionId.isEmpty()) emit jumpToVersion(versionId);
    else if (!entityId.isEmpty())
      emit jumpToEntity(entityId);
    else if (!assetId.isEmpty())
      emit jumpToAsset(assetId);
  });

  m_selectedKind = 0;
  rebuildCategoryList();
}

void CatalogHealthDialog::setReport(const paleo::health::HealthReport &report,
                                    int recycleCount, qint64 recycleBytes)
{
  m_report = report;
  m_recycleCount = recycleCount;
  m_recycleBytes = recycleBytes;
  rebuildCategoryList();
  if (m_selectedKind < 0 || m_selectedKind > kRecycleRow)
    m_selectedKind = 0;
  m_categories->setCurrentRow(m_selectedKind);
  fillIssueTable();
}

void CatalogHealthDialog::setShaState(const QString &text)
{
  m_shaState->setText(text);
}

void CatalogHealthDialog::setVerifyRunning(bool running)
{
  m_verifyBtn->setProperty("paleo.running", running);
  m_verifyBtn->setText(running ? tr("取消校验") : tr("校验外链 SHA-256"));
}

void CatalogHealthDialog::rebuildCategoryList()
{
  m_categories->blockSignals(true);
  m_categories->clear();
  for (int i = 0; i < kCategoryCount; ++i)
  {
    const int n = m_report.count(kCategoryRows[i].kind);
    auto *it = new QListWidgetItem(
        tr(kCategoryRows[i].label) + QStringLiteral("  %1").arg(n));
    it->setData(Qt::UserRole, i);
    m_categories->addItem(it);
  }
  if (m_recycleCount >= 0)
  {
    auto *it = new QListWidgetItem(tr("回收站积压  %1").arg(m_recycleCount));
    if (m_recycleBytes > 0)
      it->setText(it->text() + QStringLiteral(" · ") + formatBytes(m_recycleBytes));
    it->setData(Qt::UserRole, kRecycleRow);
    m_categories->addItem(it);
  }
  m_categories->blockSignals(false);
  if (m_categories->currentRow() < 0)
    m_categories->setCurrentRow(qMax(0, m_selectedKind));
}

void CatalogHealthDialog::fillIssueTable()
{
  m_issues->setRowCount(0);
  const int row = m_selectedKind;
  if (row < 0 || row > kRecycleRow)
    return;

  const auto addRow = [this](const QString &subject, const QString &detail,
                             const QString &assetId, const QString &entityId) {
    const int r = m_issues->rowCount();
    m_issues->insertRow(r);
    auto *s = new QTableWidgetItem(subject);
    s->setFlags(s->flags() & ~Qt::ItemIsEditable);
    s->setData(Qt::UserRole + 1, assetId); // 双击跳转目标
    s->setData(Qt::UserRole + 2, entityId);
    m_issues->setItem(r, 0, s);
    auto *d = new QTableWidgetItem(detail);
    d->setFlags(d->flags() & ~Qt::ItemIsEditable);
    m_issues->setItem(r, 1, d);
  };

  if (row == kRecycleRow)
  {
    if (m_recycleCount <= 0)
      m_summary->setText(tr("回收站无积压。"));
    else
      m_summary->setText(tr("可回收清单中有 %1 项软删资产（%2）。双击无法跳转——"
                            "请在数据页「可回收清单」处理。")
                             .arg(m_recycleCount)
                             .arg(formatBytes(m_recycleBytes)));
    return;
  }

  const paleo::health::IssueKind kind = kCategoryRows[row].kind;
  int n = 0;
  for (const paleo::health::HealthIssue &i : m_report.issues)
  {
    if (i.kind != kind)
      continue;
    addRow(i.subject, i.detail, i.assetId, i.entityId);
    if (i.kind == paleo::health::IssueKind::StaleVersion)
      m_issues->item(m_issues->rowCount() - 1, 0)->setData(Qt::UserRole + 3, i.versionId);
    ++n;
  }
  const int total = m_report.issues.size();
  m_summary->setText(
      total == 0
          ? tr("体检通过：未发现问题（共 %1 类检查）。").arg(kCategoryCount)
          : tr("共 %1 条问题；当前分类 %2 条。双击问题行可跳到对应资产。")
                .arg(total)
                .arg(n));
  if (!m_report.shaVerifyComplete)
    m_summary->setText(m_summary->text() + tr("（外链 SHA 校验未扫完，结果只是已扫部分）"));
}
