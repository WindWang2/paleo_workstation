// 层：视图
#include "wellattachmentpanel.h"

#include "../../catalog/datacatalog.h"
#include "../../services/imagelod.h"
#include "../dialogs/depthanchordialog.h"
#include "../notifications/paleonotify.h"
#include "../paleotheme.h"
#include "dataops/dataopsmodel.h"

#include <QComboBox>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{

enum
{
  kColThumb,
  kColFile,
  kColDepth,
  kColSource,
  kColRole,
  kColVersion,
  kColStatus,
  kColCount
};

QString roleText(const QString &role)
{
  if (role == QLatin1String("core"))
    return WellAttachmentPanel::tr("岩心照片");
  if (role == QLatin1String("lab_analysis"))
    return WellAttachmentPanel::tr("薄片照片");
  return role;
}

QString sourceText(const QString &source)
{
  if (source == QLatin1String("filename"))
    return WellAttachmentPanel::tr("文件名");
  if (source == QLatin1String("manual"))
    return WellAttachmentPanel::tr("手工");
  return QString();
}

} // namespace

WellAttachmentPanel::WellAttachmentPanel(
    DataCatalog *cat, const paleo::dataops::RecycleBin *recycle, QWidget *parent)
  : QDialog(parent), m_recycle(recycle)
{
  setObjectName(QStringLiteral("wellAttachmentPanel"));
  setModal(false);
  setWindowTitle(tr("井附件管理"));
  setMinimumSize(QSize(720, 420));
  buildUi();
  // 锚深编辑走 catalog mutator（changed() 触发 refresh）；软删不落
  // catalog（sidecar），由属主在命令完成后显式调 refresh()。
  setCatalog(cat);
}

WellAttachmentPanel::~WellAttachmentPanel() = default;

void WellAttachmentPanel::buildUi()
{
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(PaleoTheme::tokens().spacingMd,
                          PaleoTheme::tokens().spacingMd,
                          PaleoTheme::tokens().spacingMd,
                          PaleoTheme::tokens().spacingMd);
  lay->setSpacing(PaleoTheme::tokens().spacingSm);

  m_wellBox = new QComboBox(this);
  m_wellBox->setObjectName(QStringLiteral("wellAttachmentWellBox"));
  connect(m_wellBox, &QComboBox::currentIndexChanged, this,
          [this] { refresh(); });
  lay->addWidget(m_wellBox);

  m_table = new QTableWidget(this);
  m_table->setObjectName(QStringLiteral("wellAttachmentTable"));
  m_table->setColumnCount(kColCount);
  m_table->setHorizontalHeaderLabels({tr("缩略"), tr("文件名"), tr("锚深 (m)"),
                                      tr("来源"), tr("角色"), tr("版本"),
                                      tr("状态")});
  m_table->horizontalHeader()->setStretchLastSection(false);
  m_table->horizontalHeader()->setSectionResizeMode(kColThumb,
                                                    QHeaderView::ResizeToContents);
  m_table->horizontalHeader()->setSectionResizeMode(kColFile,
                                                    QHeaderView::Stretch);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_table->setEditTriggers(QAbstractItemView::DoubleClicked |
                           QAbstractItemView::EditKeyPressed);
  connect(m_table, &QTableWidget::itemChanged, this,
          [this](QTableWidgetItem *item) {
            if (m_updating || !item || item->column() != kColDepth)
              return;
            // 推迟到事件循环：编辑落库会经 catalog changed() 同步触发
            // refresh 重建表（setRowCount(0) 删除全部 item）——在
            // itemChanged 发射栈内做会让 QTableWidget 内部摸悬垂 item。
            const int row = item->row();
            const QString text = item->text();
            QTimer::singleShot(0, this, [this, row, text] {
              if (row < m_table->rowCount())
                applyCellEdit(row, m_table->item(row, kColDepth), text);
            });
          });
  connect(m_table, &QTableWidget::cellDoubleClicked, this,
          [this](int row, int col) {
            // 锚深列双击 = 行内编辑（EditTrigger 已接）；其余列双击开
            // 预览 + 深度输入对话框（与剖面图片道双击同入口）。
            if (col != kColDepth)
              openEditorForRow(row);
          });
  connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
    const int row = m_table->currentRow();
    const bool has = row >= 0 && row < m_rows.size();
    m_editBtn->setEnabled(has);
    m_clearBtn->setEnabled(has && m_rows.at(row).hasAnchor);
  });
  lay->addWidget(m_table, 1);

  auto *btnRow = new QWidget(this);
  auto *btnLay = new QHBoxLayout(btnRow);
  btnLay->setContentsMargins(0, 0, 0, 0);
  btnLay->setSpacing(PaleoTheme::tokens().spacingXs);
  m_editBtn = new QPushButton(tr("编辑锚深…"), btnRow);
  m_editBtn->setObjectName(QStringLiteral("wellAttachmentEditBtn"));
  m_editBtn->setToolTip(tr("预览照片并输入新深度（单位米）"));
  connect(m_editBtn, &QPushButton::clicked, this, [this] {
    const int row = m_table->currentRow();
    if (row >= 0)
      openEditorForRow(row);
  });
  m_clearBtn = new QPushButton(tr("清除锚定"), btnRow);
  m_clearBtn->setObjectName(QStringLiteral("wellAttachmentClearBtn"));
  m_clearBtn->setToolTip(tr("移除锚深（版本元数据操作，不删文件）"));
  connect(m_clearBtn, &QPushButton::clicked, this, [this] {
    const int row = m_table->currentRow();
    if (row >= 0)
      clearAnchorForRow(row);
  });
  m_removeBtn = new QPushButton(tr("移除附件…"), btnRow);
  m_removeBtn->setObjectName(QStringLiteral("wellAttachmentRemoveBtn"));
  m_removeBtn->setToolTip(tr("移入可回收清单（软删，可撤销；磁盘文件保留）"));
  connect(m_removeBtn, &QPushButton::clicked, this,
          &WellAttachmentPanel::removeSelected);
  auto *closeBtn = new QPushButton(tr("关闭"), btnRow);
  connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
  for (auto *b : {m_editBtn, m_clearBtn, m_removeBtn, closeBtn})
    btnLay->addWidget(b);
  btnLay->addStretch(1);
  lay->addWidget(btnRow);

  m_status = new QLabel(this);
  m_status->setObjectName(QStringLiteral("wellAttachmentStatus"));
  m_status->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  m_status->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(m_status, [this] { return statusStyleSheet(); });
  lay->addWidget(m_status);
}

void WellAttachmentPanel::setCatalog(DataCatalog *cat)
{
  if (m_cat == cat)
    return;
  if (m_cat)
    disconnect(m_cat, &DataCatalog::changed, this, &WellAttachmentPanel::refresh);
  m_cat = cat;
  if (m_cat)
    connect(m_cat, &DataCatalog::changed, this, &WellAttachmentPanel::refresh);
  reloadWells();
  refresh();
}

void WellAttachmentPanel::reloadWells()
{
  if (!m_cat)
    return;
  const QString keep = m_wellBox->currentData().toString();
  m_updating = true;
  m_wellBox->clear();
  // 有 core/lab_analysis 链接的井（面板价值面）；按名排序稳定。
  QVector<QPair<QString, QString>> rows; // (id, name)
  for (const CatalogEntity &e : m_cat->entities(QStringLiteral("well")))
  {
    bool has = false;
    for (const EntityAssetLink &l : m_cat->linksForEntity(e.id))
      if (l.role == QLatin1String("core") ||
          l.role == QLatin1String("lab_analysis"))
      {
        has = true;
        break;
      }
    if (has)
      rows.append({e.id, e.name});
  }
  std::sort(rows.begin(), rows.end(),
            [](const auto &a, const auto &b) { return a.second < b.second; });
  for (const auto &r : rows)
    m_wellBox->addItem(r.second, r.first);
  const int idx = m_wellBox->findData(keep);
  if (idx >= 0)
    m_wellBox->setCurrentIndex(idx);
  m_updating = false;
}

void WellAttachmentPanel::setWell(const QString &wellId)
{
  reloadWells();
  const int idx = m_wellBox->findData(wellId);
  if (idx >= 0)
    m_wellBox->setCurrentIndex(idx);
  refresh();
  show();
  raise();
  activateWindow();
}

void WellAttachmentPanel::refresh()
{
  if (!m_cat)
    return;
  const QString wellId = m_wellBox->currentData().toString();
  // 不缓存工程目录：catalog 原地 open 到另一工程后，版本来自新库，路径必须
  // 用当前 projectDir()，否则同相对路径会显示/写到上一工程。
  m_rows = paleo::WellAttachmentOps(m_cat).rowsForWell(wellId, m_cat->projectDir());
  m_updating = true;
  m_table->setRowCount(0);
  // 实际显示行（软删过滤后）——表行号 ↔ 数据索引靠它对齐。
  QVector<paleo::WellAttachmentRow> shown;
  for (const paleo::WellAttachmentRow &r : std::as_const(m_rows))
  {
    if (m_recycle && m_recycle->isRemoved(r.assetId))
      continue; // 软删行不显示（可回收清单是恢复面）
    shown.append(r);
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    auto *thumb = new QTableWidgetItem;
    if (!r.path.isEmpty() && QFile::exists(r.path))
    {
      // 面板缩略 64px（imagelod 解码期降采样——面板刷新不引全图解码）。
      const paleo::imagelod::TrackImage ti =
          paleo::imagelod::loadThumbnail(r.path, 64);
      if (!ti.isNull())
        thumb->setIcon(QIcon(QPixmap::fromImage(ti.thumbnail)));
    }
    thumb->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_table->setItem(row, kColThumb, thumb);

    auto *file = new QTableWidgetItem(r.fileName);
    file->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    file->setToolTip(r.displayName);
    m_table->setItem(row, kColFile, file);

    auto *depth = new QTableWidgetItem(
        r.hasAnchor ? QString::number(r.depthMd, 'f', 2) : QString());
    depth->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                    Qt::ItemIsEditable);
    depth->setFont(PaleoTheme::monoFont());
    depth->setData(Qt::UserRole, r.versionId);
    depth->setData(Qt::UserRole + 1, r.hasAnchor);
    depth->setData(Qt::UserRole + 2, r.path);
    m_table->setItem(row, kColDepth, depth);
    if (!r.hasAnchor)
      PaleoTheme::setItemTextColor(depth, PaleoTheme::ItemTextColor::Warning);

    auto *src = new QTableWidgetItem(sourceText(r.anchorSource));
    src->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_table->setItem(row, kColSource, src);

    auto *role = new QTableWidgetItem(roleText(r.role));
    role->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_table->setItem(row, kColRole, role);

    auto *ver = new QTableWidgetItem(
        QStringLiteral("%1 #%2").arg(r.versionId).arg(r.versionNumber));
    ver->setFont(PaleoTheme::monoFont(PaleoTheme::kLabelPt));
    ver->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    ver->setToolTip(tr("%1 · %2")
                        .arg(r.stage,
                             r.managed ? tr("受管副本") : tr("外链")));
    m_table->setItem(row, kColVersion, ver);

    auto *status = new QTableWidgetItem(
        r.unresolved ? tr("未决关联") : (r.hasAnchor ? tr("已锚定") : tr("未锚定")));
    status->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    PaleoTheme::setItemTextColor(
        status, r.unresolved || !r.hasAnchor
                    ? PaleoTheme::ItemTextColor::Warning
                    : PaleoTheme::ItemTextColor::Success);
    m_table->setItem(row, kColStatus, status);
  }
  m_rows = shown; // 表行号 ↔ m_rows 从此一一对齐
  m_updating = false;
  m_clearBtn->setEnabled(false);
  m_editBtn->setEnabled(false);
  setWindowTitle(m_wellBox->currentText().isEmpty()
                     ? tr("井附件管理")
                     : tr("井附件管理 — %1").arg(m_wellBox->currentText()));
}

void WellAttachmentPanel::applyCellEdit(int row, QTableWidgetItem *item,
                                        const QString &text)
{
  if (row < 0 || row >= m_rows.size() || !item)
    return;
  const paleo::WellAttachmentRow r = m_rows.at(row);
  // 空输入不是清锚（误触风险）——清锚走显式按钮；这里拒收并提示。
  paleo::DepthInputStatus pre =
      paleo::WellAttachmentOps::parseDepthInput(text, nullptr);
  if (pre == paleo::DepthInputStatus::Clear)
  {
    setStatus(tr("输入为空：填新深度，或用「清除锚定」按钮"), false);
    m_updating = true;
    item->setText(r.hasAnchor ? QString::number(r.depthMd, 'f', 2) : QString());
    m_updating = false;
    return;
  }
  paleo::DepthInputStatus st = paleo::DepthInputStatus::Ok;
  QString err;
  double applied = 0.0;
  paleo::WellAttachmentOps::parseDepthInput(text, &applied);
  const bool ok =
      paleo::WellAttachmentOps(m_cat).setDepthAnchor(r.versionId, text, &st, &err);
  if (ok)
  {
    // 成功：catalog changed() 同步触发 refresh 已重建表（item 换新）——
    // 状态行出结果，不碰旧 item。
    setStatus(tr("已更新锚深：%1 → %2 m").arg(r.fileName).arg(applied, 0, 'f', 2), true);
    return;
  }
  const QString reason = paleo::WellAttachmentOps::reasonText(st);
  setStatus(reason.isEmpty() ? err : reason, false);
  // 拒收无 catalog 变更、无 refresh——item 仍存活，回滚到改前值。
  m_updating = true;
  item->setText(r.hasAnchor ? QString::number(r.depthMd, 'f', 2) : QString());
  m_updating = false;
}

void WellAttachmentPanel::openEditorForRow(int row)
{
  if (row < 0 || row >= m_rows.size() || !m_cat)
    return;
  const paleo::WellAttachmentRow r = m_rows.at(row);
  PaleoDepthAnchorDialog::Context ctx;
  ctx.wellName = m_wellBox->currentText();
  ctx.fileName = r.fileName;
  ctx.hasAnchor = r.hasAnchor;
  ctx.currentDepth = r.depthMd;
  ctx.anchorSource = r.anchorSource;
  ctx.imagePath = r.path;
  PaleoDepthAnchorDialog::Result res;
  if (!PaleoDepthAnchorDialog::prompt(this, ctx, &res))
    return;
  paleo::WellAttachmentOps ops(m_cat);
  paleo::DepthInputStatus st = paleo::DepthInputStatus::Ok;
  QString err;
  if (!ops.setDepthAnchor(r.versionId,
                          res.clear ? QString() : QString::number(res.depth),
                          &st, &err))
  {
    const QString reason = paleo::WellAttachmentOps::reasonText(st);
    setStatus(tr("锚深更新失败：%1").arg(reason.isEmpty() ? err : reason), false);
    return;
  }
  setStatus(res.clear ? tr("已清除锚深（%1 回到未锚定）").arg(r.fileName)
                      : tr("已更新锚深：%1 → %2 m").arg(r.fileName).arg(res.depth),
            true);
}

void WellAttachmentPanel::clearAnchorForRow(int row)
{
  if (row < 0 || row >= m_rows.size() || !m_cat)
    return;
  const paleo::WellAttachmentRow r = m_rows.at(row);
  if (!r.hasAnchor)
    return;
  if (!PaleoNotify::ask(this, tr("清除锚定"),
                        tr("移除「%1」的锚深？\n（版本元数据操作，不删文件；"
                           "图片回到「未锚定」，可随时再补）")
                            .arg(r.fileName)))
    return;
  QString err;
  if (!m_cat->updateVersionExtra(r.versionId, QStringLiteral("depthMd"), {},
                                 &err))
  {
    setStatus(tr("清除锚深失败：%1").arg(err), false);
    return;
  }
  setStatus(tr("已清除锚深（%1 回到未锚定）").arg(r.fileName), true);
}

void WellAttachmentPanel::removeSelected()
{
  QStringList ids;
  const QList<QTableWidgetItem *> sel = m_table->selectedItems();
  QSet<int> rows;
  for (const QTableWidgetItem *it : sel)
    rows.insert(it->row());
  for (const int row : rows)
    if (row >= 0 && row < m_rows.size())
      ids << m_rows.at(row).assetId;
  if (ids.isEmpty())
    return;
  // 移除 = 软删（可回收清单，可撤销；磁盘文件不动）——命令与清单归属主
  //（DataListPanel 的撤销栈/RecycleBin 实例），这里只发意图。
  emit removeRequested(ids);
}

QString WellAttachmentPanel::statusStyleSheet() const
{
  if (m_statusTone == StatusTone::Success)
    return QStringLiteral("color: %1;").arg(PaleoTheme::tokens().successText.name());
  if (m_statusTone == StatusTone::Error)
    return QStringLiteral("color: %1;").arg(PaleoTheme::tokens().errorText.name());
  return QString();
}

void WellAttachmentPanel::setStatus(const QString &text, bool success)
{
  m_status->setText(text);
  m_statusTone = text.isEmpty() ? StatusTone::Clear
                                : (success ? StatusTone::Success : StatusTone::Error);
  m_status->setStyleSheet(statusStyleSheet());
}
