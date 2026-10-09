// 层：视图
#include "datalist.h"
#include "datalistops.h"
#include "assetentitychoicemodel.h"
#include "pageshared.h"
#include "../paleotheme.h"
#include "../paleoicons.h"
#include "../../catalog/datacatalog.h"
#include "../../services/previewdoc.h"
#include "dataops/dataopscommands.h"
#include "dataops/dataopsmodel.h"
#include "dataops/dataopsselection.h"
#include "dataopspanelops.h"
#include "dataopsviews.h"
#include "dataopswidgets.h"

#include <QBoxLayout>
#include <QComboBox>
#include <QDebug>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QVariantMap>

using namespace paleo::pagesinternal;

void DataListPanel::refreshAssetTable()
{
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (!table || !m_doc)
    return;
  loadStoresForCatalog();
  m_selKeep.snapshot(m_tree, table);
  m_pageSelection.unite(m_selKeep.ids());
  DataCatalog *cat = m_doc->catalog();
  for (auto *model : m_entityChoiceModels) model->deleteLater();
  m_entityChoiceModels.clear();
  rebuildRowSnapshot();
  m_tableDirty = true;
  QSet<QString> live;
  for (const auto &row : m_rows)
    live.insert(row.assetId);
  m_pageSelection.intersect(live);
  // 类型下拉按当前资产类型集重建（保留原选择）。
  if (auto *typeFilter = findChild<QComboBox *>(QStringLiteral("assetTypeFilter")))
  {
    const QString keep = typeFilter->currentData().toString();
    QStringList types;
    for (const CatalogAsset &a : cat->assets())
      if (!a.type.isEmpty() && !types.contains(a.type))
        types << a.type;
    types.sort();
    const QSignalBlocker block(typeFilter);
    typeFilter->clear();
    typeFilter->addItem(tr("所有类型"), QString());
    for (const QString &t : types)
    {
      QString label = t;
      if (t == QLatin1String("well_log")) label = tr("测井曲线 (well_log)");
      else if (t == QLatin1String("well_head")) label = tr("井位/井身 (well_head)");
      else if (t == QLatin1String("tops")) label = tr("井分层 (tops)");
      else if (t == QLatin1String("time_depth")) label = tr("时深关系 (time_depth)");
      else if (t == QLatin1String("seismic")) label = tr("地震数据 (seismic)");
      else if (t == QLatin1String("horizon")) label = tr("层位解释 (horizon)");
      else if (t == QLatin1String("boundary")) label = tr("边界/相图 (boundary)");
      else if (t == QLatin1String("auxiliary") || t == QLatin1String("reference")) label = tr("辅助/综合图 (auxiliary)");
      typeFilter->addItem(label, t);
    }
    typeFilter->setCurrentIndex(qMax(0, typeFilter->findData(keep)));
  }
  refreshTagCloud();
  updatePendingCounts();
  refreshChipBar();
  refreshUndoButtons();
  applyListFilter();
  refreshSelectionBadge();
}

void DataListPanel::renderAssetPage()
{
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (!table || !m_doc)
    return;
  const QSignalBlocker block(table);
  DataCatalog *cat = m_doc->catalog();
  // T28 undo 库：attach 落档（含被降级的前主关联 + 挂前 note），撤销消费；
  // note 记忆独立长存（撤销后未决徽标 tooltip 从这里恢复——catalog 无 note
  // 写回 API，A 包接缝，UI 层保管）。随工程 sidecar 持久，跨 open() 存活。
  QHash<QString, QString> noteMemory;
  QVector<UndoRecord> undoRecords = loadUndoVault(cat, &noteMemory);
  // 确认条存活（T28）：changed() 刷新重建表时，未决的确认状态从这恢复。
  const QVariantMap pendingConfirm =
      property("paleo.page.pendingConfirm").toMap();

  // Qt 清行不删单元格控件（setRowCount(0)/removeCellWidget 都只摘不删，
  // 控件会活成表内孤儿）——摘出父子树再 deleteLater：刷新可能正被这行
  // 自己的按钮 clicked 触发，同步 delete 会在发件者栈上自杀。
  for (int r = 0; r < table->rowCount(); ++r)
    if (QWidget *w = table->cellWidget(r, 2))
    {
      table->removeCellWidget(r, 2);
      w->setParent(nullptr);
      w->deleteLater();
    }
  table->setRowCount(0);

  const bool unresolvedOnly = property("paleo.page.filterUnresolved").toBool();
  for (const auto &row : m_pageRows)
  {
    const CatalogAsset a = cat->assetById(row.assetId);
    const int r = table->rowCount();
    table->insertRow(r);
    auto *nameItem = new QTableWidgetItem(a.displayName);
    nameItem->setData(Qt::UserRole, a.id);
    // 「来源」并入名称 tooltip：受管相对路径 / 外链绝对路径（§4）。
    const CatalogVersion v = cat->currentVersion(a.id);
    if (!v.id.isEmpty())
      nameItem->setToolTip(v.managed ? tr("受管 %1").arg(v.path)
                                     : tr("外部链接 %1").arg(v.path));
    table->setItem(r, 0, nameItem);
    const QString shownType = m_typeOv.overriddenType(a.id).isEmpty()
                                  ? a.type
                                  : m_typeOv.overriddenType(a.id);
    table->setItem(r, 1, new QTableWidgetItem(shownType));

    // ---- 关联列 ----------------------------------------------------------
    QStringList parts;    // item 文本（控件行也保留，便于检索与断言）
    QStringList resolved; // 控件行回显的实体名/「参考」
    QStringList notes;    // 未决备注（徽标 tooltip：候选名在这里）
    // 身份而非下标（T28）：动作在点击时刻按这些键重扫 links()。
    QString unresolvedRole, unresolvedEntityType; // 本资产第一条未决链接的身份
    const UndoRecord *undoRecord = nullptr;       // 本资产可撤销的挂接记录
    QString promotableRole, promotableEntityId;   // 已决非主链接（「设为主版本」）
    for (const EntityAssetLink &l : cat->linksForAsset(a.id))
    {
      if (l.unresolved)
      {
        if (unresolvedRole.isEmpty())
        {
          unresolvedRole = l.role;
          unresolvedEntityType = l.entityType;
        }
        if (!parts.contains(QStringLiteral("未决")))
          parts << tr("未决");
        if (!l.note.isEmpty())
          notes << l.note;
        continue;
      }
      if (l.role == QLatin1String("reference"))
      {
        if (!parts.contains(QStringLiteral("参考")))
        {
          parts << tr("参考");
          resolved << tr("参考");
        }
        continue;
      }
      const CatalogEntity e = cat->entityById(l.entityId);
      const QString nm = e.name.isEmpty() ? l.entityId : e.name;
      if (!nm.isEmpty() && !parts.contains(nm))
      {
        parts << nm;
        resolved << nm;
      }
      if (!l.isPrimary && promotableRole.isEmpty())
      {
        promotableRole = l.role;
        promotableEntityId = l.entityId;
      }
    }
    for (const UndoRecord &rec : undoRecords)
      if (rec.assetId == a.id)
      {
        undoRecord = &rec;
        break;
      }

    auto *assoc = new QTableWidgetItem(parts.join(QStringLiteral("、")));
    if (!notes.isEmpty())
      assoc->setToolTip(notes.join(QStringLiteral("\n")));
    table->setItem(r, 2, assoc);

    if (unresolvedRole.isEmpty() && !undoRecord && promotableRole.isEmpty())
      continue; // 纯已决文本行——不需要单元格控件

    // 控件单元格：[已决名…] [未决徽标+实体下拉+挂接] [撤销] [设为主版本]。
    // 「挂到这口井」走页内确认条（同时写出资产名和实体名），不弹模态框。
    auto *cell = new QWidget(table);
    auto *stack = new QStackedLayout(cell);
    stack->setContentsMargins(0, 0, 0, 0);
    auto *browse = new QWidget(cell);
    auto *bl = new QHBoxLayout(browse);
    // 保持原生表格既定行高；纵向留白由行控制，单元格不重复叠加。
    bl->setContentsMargins(PaleoTheme::tokens().spacingXs, 0, PaleoTheme::tokens().spacingXs, 0);
    bl->setSpacing(PaleoTheme::tokens().spacingXs);
    if (!resolved.isEmpty())
    {
      auto *names = new QLabel(resolved.join(QStringLiteral("、")), browse);
      PaleoTheme::applyThemedStyleSheet(names, [] { return QStringLiteral("color: %1;").arg(PaleoTheme::tokens().text.name().toUpper()); });
      bl->addWidget(names);
    }

    if (!unresolvedRole.isEmpty())
    {
      const QString noteKey = a.id + QLatin1Char('|') + unresolvedRole;
      const QString badgeNote = [cat, &noteMemory, &noteKey]() {
        for (const EntityAssetLink &l : cat->linksForAsset(
                 noteKey.section(QLatin1Char('|'), 0, 0)))
          if (l.role == noteKey.section(QLatin1Char('|'), 1) && l.unresolved &&
              !l.note.isEmpty())
            return l.note; // catalog 里的 note 优先
        return noteMemory.value(noteKey, QString()); // T28：撤销后从记忆恢复
      }();
      auto *badge = new QLabel(tr("未决"), browse);
      badge->setObjectName(QStringLiteral("unresolvedBadge"));
      // §4 未决徽标：warningBg 底 / text 字 / warning 边（DESIGN.md 浅值
      // #FFF4E0/#24303E/#F29900——token 活体，随主题出暗色变体）。
      PaleoTheme::applyThemedStyleSheet(badge, [] {
        const auto &t = PaleoTheme::tokens();
        return PaleoTheme::metricStyleSheet(QStringLiteral(
                   "background: %1; color: %2; border: 1px solid %3;"
                   "border-radius: {rounded.sm}px; padding: 0 {spacing.sm}px;"))
            .arg(t.warningBg.name().toUpper(), t.text.name().toUpper(),
                 t.warning.name().toUpper());
      });
      badge->setToolTip(badgeNote.isEmpty() ? tr("未决关联") : badgeNote);
      bl->addWidget(badge);

      // 下拉框：全量同类实体（井→全部井实体），默认空（哨兵项）。
      auto *combo = new QComboBox(browse);
      combo->setObjectName(QStringLiteral("resolveEntityCombo"));
      combo->setAccessibleName(tr("挂接实体"));
      const bool isWell = unresolvedEntityType == QLatin1String("well");
      auto *choices = m_entityChoiceModels.value(unresolvedEntityType);
      if (!choices) {
        choices = new AssetEntityChoiceModel(cat->entities(unresolvedEntityType),
          isWell ? tr("（选择井）") : tr("（选择实体）"), this);
        m_entityChoiceModels.insert(unresolvedEntityType, choices);
      }
      combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
      combo->setMinimumContentsLength(8);
      combo->setMaxVisibleItems(20);
      combo->setModel(choices);
      combo->setCurrentIndex(0);
      combo->setMinimumWidth(72);
      bl->addWidget(combo, 1);

      auto *attach = new QPushButton(isWell ? tr("挂到这口井") : tr("挂接关联"), browse);
      attach->setObjectName(QStringLiteral("attachLinkButton"));
      attach->setEnabled(false); // 选到实体才放闸
      if (combo->count() <= 1)
        attach->setToolTip(tr("工程里还没有可挂的实体"));
      connect(combo, &QComboBox::currentIndexChanged, attach, [combo, attach](int) {
        attach->setEnabled(!combo->currentData().toString().isEmpty());
      });
      bl->addWidget(attach);

      // 确认条：资产名 + 选中的实体名同时写出（§4），不弹模态对话框。
      auto *confirmStrip = new QWidget(cell);
      auto *cf = new QHBoxLayout(confirmStrip);
      cf->setContentsMargins(PaleoTheme::tokens().spacingXs, 0, PaleoTheme::tokens().spacingXs, 0);
      cf->setSpacing(PaleoTheme::tokens().spacingXs);
      auto *confirmText = new QLabel(confirmStrip);
      confirmText->setObjectName(QStringLiteral("attachConfirmText"));
      confirmText->setWordWrap(true);
      cf->addWidget(confirmText, 1);
      auto *ok = new QPushButton(tr("确认"), confirmStrip);
      ok->setObjectName(QStringLiteral("attachConfirmButton"));
      auto *cancel = new QPushButton(tr("取消"), confirmStrip);
      cancel->setObjectName(QStringLiteral("attachCancelButton"));
      cf->addWidget(ok);
      cf->addWidget(cancel);
      stack->addWidget(browse);
      stack->addWidget(confirmStrip);
      stack->setCurrentWidget(browse);

      // 确认状态落在页面属性上（T28）：changed() 触发的整表重建会从这里
      // 恢复确认条——用户不会被一次后台刷新打断。
      const auto armConfirm = [this, confirmText, stack, confirmStrip, combo,
                               displayName = a.displayName, assetId = a.id]() {
        const QString eid = combo->currentData().toString();
        if (eid.isEmpty())
          return;
        const QString ename = combo->currentText();
        QVariantMap pc;
        pc.insert(QStringLiteral("asset_id"), assetId);
        pc.insert(QStringLiteral("entity_id"), eid);
        pc.insert(QStringLiteral("entity_name"), ename);
        setProperty("paleo.page.pendingConfirm", pc);
        confirmText->setText(
            tr("将「%1」挂接到「%2」？").arg(displayName, ename));
        stack->setCurrentWidget(confirmStrip);
      };
      connect(attach, &QPushButton::clicked, this, armConfirm);
      const auto clearPending = [this]() {
        setProperty("paleo.page.pendingConfirm", QVariantMap());
      };
      connect(cancel, &QPushButton::clicked, this, [this, stack, browse, clearPending]() {
        clearPending();
        stack->setCurrentWidget(browse);
      });
      connect(ok, &QPushButton::clicked, this,
              [this, cat, assetId = a.id, role = unresolvedRole, combo, clearPending]() {
                clearPending();
                const QString eid = combo->currentData().toString();
                // 身份寻址（T28）：点击时刻重扫——刷新与点击之间 links()
                // 变过也命中本资产的这条未决链接。
                const int idx = indexOfLink(cat, assetId, role, QString());
                if (eid.isEmpty() || idx < 0)
                  return;
                const QVector<EntityAssetLink> ls = cat->links();
                const EntityAssetLink &link = ls.at(idx);
                // 被降级的前主关联（撤销时恢复，D4）：同（实体,角色）当前主链。
                UndoRecord rec;
                rec.assetId = assetId;
                rec.entityId = eid;
                rec.role = role;
                rec.entityType = link.entityType;
                rec.note = link.note; // 挂前 note（attachLink 会清）
                for (int i = 0; i < ls.size(); ++i)
                  if (i != idx && ls.at(i).isPrimary && !ls.at(i).unresolved &&
                      ls.at(i).entityType == link.entityType &&
                      ls.at(i).entityId == eid && ls.at(i).role == role)
                  {
                    rec.demotedAssetId = ls.at(i).assetId;
                    rec.demotedEntityId = ls.at(i).entityId;
                    rec.demotedRole = ls.at(i).role;
                    rec.demotedEntityType = ls.at(i).entityType;
                    break;
                  }
                QString err;
                if (!cat->attachLink(idx, eid, &err))
                {
                  qWarning() << "DataPage attachLink failed:" << err;
                  refreshAssetTable();
                  emit entityRefreshRequested();
                  return;
                }
                // undo 落档 + note 记忆（跨 open() 持久；从盘上状态续写——
                // 处理器运行时刷新局部早没了）。
                QHash<QString, QString> notes;
                QVector<UndoRecord> records = loadUndoVault(cat, &notes);
                records.append(rec);
                if (!rec.note.isEmpty())
                  notes.insert(assetId + QLatin1Char('|') + role, rec.note);
                saveUndoVault(cat, records, notes);
                refreshAssetTable();
                emit entityRefreshRequested();
              });

      // 刷新后恢复确认条（pendingConfirm 仍指向本资产的未决链接）。
      if (pendingConfirm.value(QStringLiteral("asset_id")).toString() == a.id &&
          pendingConfirm.value(QStringLiteral("entity_id")).toString().isEmpty() == false)
      {
        const QString pendEid =
            pendingConfirm.value(QStringLiteral("entity_id")).toString();
        const int comboIdx = combo->findData(pendEid);
        if (comboIdx >= 0 && indexOfLink(cat, a.id, unresolvedRole, QString()) >= 0)
        {
          combo->setCurrentIndex(comboIdx);
          confirmText->setText(tr("将「%1」挂接到「%2」？")
                                   .arg(a.displayName,
                                        pendingConfirm.value(QStringLiteral("entity_name"))
                                            .toString()));
          stack->setCurrentWidget(confirmStrip);
        }
        else
          clearPending();
      }
    }
    else
      stack->addWidget(browse);

    if (undoRecord)
    {
      auto *undo = new QPushButton(tr("撤销"), browse);
      undo->setObjectName(QStringLiteral("undoAttachButton"));
      const CatalogEntity e = cat->entityById(undoRecord->entityId);
      const QString target = e.name.isEmpty() ? undoRecord->entityId : e.name;
      undo->setToolTip(tr("撤回对「%1」的挂接（回到未决）").arg(target));
      connect(undo, &QPushButton::clicked, this,
              [this, cat, assetId = undoRecord->assetId, entityId = undoRecord->entityId,
               role = undoRecord->role, demotedAssetId = undoRecord->demotedAssetId,
               demotedEntityId = undoRecord->demotedEntityId,
               demotedRole = undoRecord->demotedRole]() {
                // 身份寻址（T28）：撤销的是这条挂接，不是某个行号。
                const int idx = indexOfLink(cat, assetId, role, entityId);
                QString err;
                if (idx < 0 || !cat->setLinkUnresolved(idx, &err))
                {
                  qWarning() << "DataPage setLinkUnresolved failed:" << err;
                  refreshAssetTable();
                  emit entityRefreshRequested();
                  return;
                }
                // D4：恢复被降级的前主关联（同井同角色换回旧主链）。
                if (!demotedAssetId.isEmpty())
                {
                  const int pIdx = indexOfLink(cat, demotedAssetId, demotedRole,
                                               demotedEntityId);
                  if (pIdx >= 0 && !cat->setLinkPrimary(pIdx, &err))
                    qWarning() << "DataPage restore demoted primary failed:" << err;
                }
                // note 记忆保留（撤销后的未决徽标 tooltip 用），undo 记录消费
                // （vault 从盘上续读——刷新局部不进处理器）。
                QHash<QString, QString> notes;
                QVector<UndoRecord> records = loadUndoVault(cat, &notes);
                for (int i = 0; i < records.size(); ++i)
                  if (records.at(i).assetId == assetId &&
                      records.at(i).entityId == entityId &&
                      records.at(i).role == role)
                    records.removeAt(i--);
                saveUndoVault(cat, records, notes);
                refreshAssetTable();
                emit entityRefreshRequested();
              });
      bl->addWidget(undo);
    }

    if (!promotableRole.isEmpty())
    {
      // 「将此版本设为主版本」：同（实体,角色）的旧版本资产可拿回主关联——
      // 只动链接的 isPrimary 标志，不复制版本字节（§4）。身份寻址（T28）。
      // well_log 文案是「设为主文件」，点击仍按 assetId+role+entityId 重查。
      const bool wellLogPrimary = promotableRole == QLatin1String("well_log");
      auto *primary = new QPushButton(wellLogPrimary ? tr("设为主文件") : tr("设为主版本"),
                                      browse);
      primary->setObjectName(wellLogPrimary ? QStringLiteral("setWellLogPrimaryButton")
                                            : QStringLiteral("setPrimaryButton"));
      primary->setToolTip(wellLogPrimary ? tr("同井测井 — 把这份文件设为主文件")
                                         : tr("同角色旧版本 — 把这条关联设为主关联"));
      connect(primary, &QPushButton::clicked, this,
              [this, cat, assetId = a.id, role = promotableRole,
               eid = promotableEntityId]() {
                const int idx = indexOfLink(cat, assetId, role, eid);
                QString err;
                if (idx < 0 || !cat->setLinkPrimary(idx, &err))
                {
                  qWarning() << "DataPage setLinkPrimary failed:" << err;
                }
                refreshAssetTable();
                emit entityRefreshRequested();
              });
      bl->addWidget(primary);
    }

    bl->addStretch(1);
    table->setCellWidget(r, 2, cell);
  }
  refreshAssetEmptyState(table, unresolvedOnly ? tr("没有未决资产 — 全部资产都已挂接") : QString());
  m_selKeep.snapshotIds(m_pageSelection);
  m_selKeep.restoreTable(table);
  refreshAssetTree();
  m_selKeep.restoreTree(m_tree);
}

// ---- D7 视图形态 ------------------------------------------------------------------
void DataListPanel::setViewMode(int mode)
{
  if (m_viewStack && mode >= 0 && mode < m_viewStack->count())
    m_viewStack->setCurrentIndex(mode);
}

void DataListPanel::openColumnConfig()
{
  using namespace paleo::dataops;
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (!table)
    return;
  QStringList labels;
  QList<bool> visible;
  for (int c = 0; c < table->columnCount(); ++c)
  {
    const int logical = table->horizontalHeader()->logicalIndex(c);
    labels << (table->horizontalHeaderItem(logical)
                   ? table->horizontalHeaderItem(logical)->text()
                   : QString::number(logical));
    visible << !table->isColumnHidden(logical);
  }
  ColumnConfigDialog dlg(labels, visible, this);
  if (dlg.exec() != QDialog::Accepted)
    return;
  const QList<QPair<int, bool>> result = dlg.result();
  for (int visual = 0; visual < result.size(); ++visual)
  {
    const int logical = result.at(visual).first;
    table->setColumnHidden(logical, !result.at(visual).second);
    const int curVisual = table->horizontalHeader()->visualIndex(logical);
    if (curVisual != visual)
      table->horizontalHeader()->moveSection(curVisual, visual);
  }
  saveColumnState(QStringLiteral("assetTable"), table->horizontalHeader());
}
