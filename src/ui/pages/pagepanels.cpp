#include "pagepanels.h"

#include "../../ai/onnxpredictionservice.h" // ORT-free header; runtimeAvailable 调用受 PALEO_HAVE_ORT 保护
#include "../../catalog/datacatalog.h"
#include "../../io/dataimportservice.h"
#include "../../workflow/workflows.h"    // signal names + ValidationWorkflow::validate
#include "../../qgis/qgislayerservice.h" // declared() — forward-declares Qgs*, none included
#include "../../metadata/layermanifest.h" // LayerDeclaration fields
#include "../../domain/types.h"          // ValidationIssue fields

#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QShowEvent>
#include <QSpinBox>
#include <QStackedLayout>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

// ---------------------------------------------------------------------------
// pagepanels.h fixes the public shape of these classes and declares no data
// members, so service bindings ride on dynamic QObject properties (same idiom
// as workflows.cpp) and child widgets are located by objectName.
// ---------------------------------------------------------------------------
namespace
{
  const char kLayersProp[] = "paleo.page.layers"; // QObject* (QgisLayerService)
  const char kWfProp[]     = "paleo.page.wf";     // QObject* (page workflow)

  // DESIGN.md `label` token: 8pt muted captions mark panel groups.
  QLabel *caption(const QString &text, QWidget *parent)
  {
    auto *l = new QLabel(text, parent);
    QFont f = l->font();
    f.setPointSize(8);
    l->setFont(f);
    l->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
    return l;
  }

  QVBoxLayout *panelLayout(QWidget *page)
  {
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(8, 8, 8, 8); // spacing.sm
    lay->setSpacing(8);
    return lay;
  }

  template <typename T>
  T *child(QObject *root, const char *name)
  {
    return root->findChild<T *>(QLatin1String(name));
  }

  // §42.4: an empty asset table shows a guidance row — never a blank panel.
  void refreshAssetEmptyState(QTableWidget *t)
  {
    if (t->rowCount() > 0)
      return;
    t->insertRow(0);
    auto *it = new QTableWidgetItem(
        DataPage::tr("还没有数据资产 — 通过上方「数据导入」添加井、地震或边界数据"));
    it->setFlags(Qt::NoItemFlags);
    it->setForeground(QColor(QStringLiteral("#5D6E80"))); // text-muted
    t->setItem(0, 0, it);
    t->setSpan(0, 0, 1, t->columnCount());
  }

  QString severityText(ValidationIssue::Severity s)
  {
    switch (s)
    {
      case ValidationIssue::Info:    return ValidatePage::tr("信息");
      case ValidationIssue::Error:   return ValidatePage::tr("错误");
      case ValidationIssue::Warning: // fall through
      default:                       return ValidatePage::tr("警告");
    }
  }

  // Semantic colors pair with the severity text, never stand alone (§42.16).
  QColor severityColor(ValidationIssue::Severity s)
  {
    switch (s)
    {
      case ValidationIssue::Info:  return QColor(QStringLiteral("#1B73D0")); // primary
      case ValidationIssue::Error: return QColor(QStringLiteral("#E53935")); // error
      default:                     return QColor(QStringLiteral("#F29900")); // warning
    }
  }
} // namespace

// ---------------------------------------------------------------------------
// DataPage — 数据管理
// ---------------------------------------------------------------------------
DataPage::DataPage(QWidget *parent)
  : QWidget(parent)
{
  auto *lay = panelLayout(this);

  lay->addWidget(caption(tr("数据导入"), this));
  const struct { const char *name; const char *text; const char *kind; } kImports[] = {
    {"importWells", QT_TR_NOOP("导入井数据"), "wells"},
    {"importSeismic", QT_TR_NOOP("导入地震数据"), "seismic"},
    {"importBoundary", QT_TR_NOOP("导入边界数据"), "boundary"},
    {"importFolder", QT_TR_NOOP("导入工区文件夹"), "folder"},
  };
  for (const auto &spec : kImports)
  {
    auto *btn = new QPushButton(tr(spec.text), this);
    btn->setObjectName(QLatin1String(spec.name));
    connect(btn, &QPushButton::clicked, this,
            [this, kind = QLatin1String(spec.kind)] { emit importRequested(kind); });
    lay->addWidget(btn);
  }

  lay->addSpacing(16); // spacing.md between groups
  lay->addWidget(caption(tr("资产"), this));
  auto *table = new QTableWidget(0, 3, this);
  table->setObjectName(QStringLiteral("assetTable"));
  table->setAccessibleName(tr("资产列表"));
  // §4 预览壳重排：预览移到共享地图下方（不再挂右栏）；第三列由「来源」改为
  // 「关联」——已决链接写实体名、未决给「未决」徽标+挂接控件、参考资产写「参考」。
  table->setHorizontalHeaderLabels({tr("名称"), tr("类型"), tr("关联")});
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  refreshAssetEmptyState(table);
  lay->addWidget(table, 1);

  // 列表选中一条资产 → 中央预览标签（预览部件由 shell 持有，重选聚焦语义
  // 由 DataPreviewTabs 实现）。
  connect(table, &QTableWidget::itemSelectionChanged, this, [this, table]() {
    const QList<QTableWidgetItem *> sel = table->selectedItems();
    if (sel.isEmpty())
      return;
    const QString assetId = sel.front()->data(Qt::UserRole).toString();
    if (!assetId.isEmpty())
      emit assetActivated(assetId);
  });
}

void DataPage::refreshAssetTable()
{
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (!table)
    return;
  auto *svc = qobject_cast<DataImportService *>(property("paleo.page.importsvc").value<QObject *>());
  if (!svc)
    return;
  DataCatalog *cat = svc->catalog();
  const QVector<EntityAssetLink> allLinks = cat->links();
  // 本会话从本页手动挂上的链接 {asset_id, entity_id, role} —— 撤销入口只认
  // 这些（不碰导入期就已决的链接）。
  const QVariantList sessionAttach =
      property("paleo.page.sessionAttach").toList();

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
  for (const CatalogAsset &a : cat->assets())
  {
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
    table->setItem(r, 1, new QTableWidgetItem(a.type));

    // ---- 关联列 ----------------------------------------------------------
    QStringList parts;    // item 文本（控件行也保留，便于检索与断言）
    QStringList resolved; // 控件行回显的实体名/「参考」
    QStringList notes;    // 未决备注（徽标 tooltip：候选名在这里）
    int unresolvedIdx = -1; // links() 序：本资产第一条未决链接
    int undoableIdx = -1;   // links() 序：本会话挂上、可撤销的那条
    QString undoableTarget; // 撤销按钮 tooltip 的实体名
    int promotableIdx = -1; // links() 序：已决非主链接（「设为主版本」）
    for (int i = 0; i < allLinks.size(); ++i)
    {
      const EntityAssetLink &l = allLinks.at(i);
      if (l.assetId != a.id)
        continue;
      if (l.unresolved)
      {
        if (unresolvedIdx < 0)
          unresolvedIdx = i;
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
      if (!l.isPrimary && promotableIdx < 0)
        promotableIdx = i;
      if (undoableIdx < 0)
        for (const QVariant &pv : sessionAttach)
        {
          const QVariantMap m = pv.toMap();
          if (m.value(QStringLiteral("asset_id")).toString() == l.assetId &&
              m.value(QStringLiteral("entity_id")).toString() == l.entityId &&
              m.value(QStringLiteral("role")).toString() == l.role)
          {
            undoableIdx = i;
            undoableTarget = nm;
            break;
          }
        }
    }

    auto *assoc = new QTableWidgetItem(parts.join(QStringLiteral("、")));
    if (!notes.isEmpty())
      assoc->setToolTip(notes.join(QStringLiteral("\n")));
    table->setItem(r, 2, assoc);

    if (unresolvedIdx < 0 && undoableIdx < 0 && promotableIdx < 0)
      continue; // 纯已决文本行——不需要单元格控件

    // 控件单元格：[已决名…] [未决徽标+实体下拉+挂接] [撤销] [设为主版本]。
    // 「挂到这口井」走页内确认条（同时写出资产名和实体名），不弹模态框。
    auto *cell = new QWidget(table);
    auto *stack = new QStackedLayout(cell);
    stack->setContentsMargins(0, 0, 0, 0);
    auto *browse = new QWidget(cell);
    auto *bl = new QHBoxLayout(browse);
    bl->setContentsMargins(4, 1, 4, 1);
    bl->setSpacing(4);
    if (!resolved.isEmpty())
    {
      auto *names = new QLabel(resolved.join(QStringLiteral("、")), browse);
      names->setStyleSheet(QStringLiteral("color: #24303E;"));
      bl->addWidget(names);
    }

    if (unresolvedIdx >= 0)
    {
      const EntityAssetLink link = allLinks.at(unresolvedIdx);
      auto *badge = new QLabel(tr("未决"), browse);
      badge->setObjectName(QStringLiteral("unresolvedBadge"));
      // §4 未决徽标：bg #FFF4E0 / text #24303E / border #F29900（DESIGN.md）。
      badge->setStyleSheet(QStringLiteral(
          "background: #FFF4E0; color: #24303E; border: 1px solid #F29900;"
          "border-radius: 3px; padding: 0 6px;"));
      badge->setToolTip(link.note.isEmpty() ? tr("未决关联") : link.note);
      bl->addWidget(badge);

      // 下拉框：全量同类实体（井→全部井实体），默认空（哨兵项）。
      auto *combo = new QComboBox(browse);
      combo->setObjectName(QStringLiteral("resolveEntityCombo"));
      combo->setAccessibleName(tr("挂到实体"));
      const bool isWell = link.entityType == QLatin1String("well");
      combo->addItem(isWell ? tr("（选择井）") : tr("（选择实体）"), QString());
      for (const CatalogEntity &e : cat->entities(link.entityType))
        combo->addItem(e.name.isEmpty() ? e.id : e.name, e.id);
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
      cf->setContentsMargins(4, 1, 4, 1);
      cf->setSpacing(4);
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

      connect(attach, &QPushButton::clicked, this,
              [stack, confirmStrip, confirmText, combo, displayName = a.displayName]() {
                if (combo->currentData().toString().isEmpty())
                  return;
                confirmText->setText(
                    tr("把「%1」挂到「%2」？").arg(displayName, combo->currentText()));
                stack->setCurrentWidget(confirmStrip);
              });
      connect(cancel, &QPushButton::clicked, this,
              [stack, browse]() { stack->setCurrentWidget(browse); });
      connect(ok, &QPushButton::clicked, this,
              [this, cat, assetId = a.id, linkIndex = unresolvedIdx, combo]() {
                const QString eid = combo->currentData().toString();
                const QVector<EntityAssetLink> ls = cat->links();
                if (eid.isEmpty() || linkIndex < 0 || linkIndex >= ls.size())
                  return;
                const QString role = ls.at(linkIndex).role;
                QString err;
                if (!cat->attachLink(linkIndex, eid, &err))
                {
                  qWarning() << "DataPage attachLink failed:" << err;
                  refreshAssetTable();
                  return;
                }
                // 记入会话挂接 —— 刷新后这行给「撤销」。
                QVariantList sa =
                    property("paleo.page.sessionAttach").toList();
                QVariantMap rec;
                rec.insert(QStringLiteral("asset_id"), assetId);
                rec.insert(QStringLiteral("entity_id"), eid);
                rec.insert(QStringLiteral("role"), role);
                sa.append(rec);
                setProperty("paleo.page.sessionAttach", sa);
                refreshAssetTable();
              });
    }
    else
      stack->addWidget(browse);

    if (undoableIdx >= 0)
    {
      auto *undo = new QPushButton(tr("撤销"), browse);
      undo->setObjectName(QStringLiteral("undoAttachButton"));
      if (!undoableTarget.isEmpty())
        undo->setToolTip(tr("撤回对「%1」的挂接（回到未决）").arg(undoableTarget));
      connect(undo, &QPushButton::clicked, this, [this, cat, linkIndex = undoableIdx]() {
        const QVector<EntityAssetLink> ls = cat->links();
        if (linkIndex < 0 || linkIndex >= ls.size())
          return;
        const EntityAssetLink target = ls.at(linkIndex); // 改回未决前先留底
        QString err;
        if (!cat->setLinkUnresolved(linkIndex, &err))
        {
          qWarning() << "DataPage setLinkUnresolved failed:" << err;
          refreshAssetTable();
          return;
        }
        QVariantList sa =
            property("paleo.page.sessionAttach").toList();
        for (int i = sa.size() - 1; i >= 0; --i)
        {
          const QVariantMap m = sa.at(i).toMap();
          if (m.value(QStringLiteral("asset_id")).toString() == target.assetId &&
              m.value(QStringLiteral("entity_id")).toString() == target.entityId &&
              m.value(QStringLiteral("role")).toString() == target.role)
            sa.removeAt(i);
        }
        setProperty("paleo.page.sessionAttach", sa);
        refreshAssetTable();
      });
      bl->addWidget(undo);
    }

    if (promotableIdx >= 0)
    {
      // 「将此版本设为主版本」：同（实体,角色）的旧版本资产可拿回主关联——
      // 只动链接的 isPrimary 标志，不复制版本字节（§4）。
      auto *primary = new QPushButton(tr("设为主版本"), browse);
      primary->setObjectName(QStringLiteral("setPrimaryButton"));
      primary->setToolTip(tr("同角色旧版本 — 把这条关联设为主关联"));
      connect(primary, &QPushButton::clicked, this,
              [this, cat, linkIndex = promotableIdx]() {
                QString err;
                if (!cat->setLinkPrimary(linkIndex, &err))
                  qWarning() << "DataPage setLinkPrimary failed:" << err;
                refreshAssetTable();
              });
      bl->addWidget(primary);
    }

    bl->addStretch(1);
    table->setCellWidget(r, 2, cell);
  }
  refreshAssetEmptyState(table);
}

void DataPage::selectAssetsForEntities(const QStringList &entityIds)
{
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  auto *svc = qobject_cast<DataImportService *>(
      property("paleo.page.importsvc").value<QObject *>());
  if (!table || !svc || entityIds.isEmpty())
    return;
  // 实体 → 已决关联资产集合（未决链接实体 id 为空，天然不命中）。
  QSet<QString> wanted;
  for (const QString &eid : entityIds)
    for (const EntityAssetLink &l : svc->catalog()->linksForEntity(eid))
      if (!l.unresolved)
        wanted.insert(l.assetId);
  if (wanted.isEmpty())
    return;
  // 一次应用整份选中（QTableView::selectRow 是单点替换语义，逐行调会互相
  // 顶掉）；选中变化照发 itemSelectionChanged → assetActivated 首个命中行。
  QItemSelection sel;
  QTableWidgetItem *firstHit = nullptr;
  for (int r = 0; r < table->rowCount(); ++r)
  {
    QTableWidgetItem *it = table->item(r, 0);
    if (!it || !wanted.contains(it->data(Qt::UserRole).toString()))
      continue;
    sel.select(table->model()->index(r, 0),
               table->model()->index(r, table->columnCount() - 1));
    if (!firstHit)
      firstHit = it;
  }
  if (sel.isEmpty())
    return;
  table->selectionModel()->select(
      sel, QItemSelectionModel::Select | QItemSelectionModel::Rows);
  if (firstHit)
    table->scrollToItem(firstHit);
}

// ---------------------------------------------------------------------------
// PredictPage — ①智能预测
// ---------------------------------------------------------------------------
PredictPage::PredictPage(PredictionWorkflow *wf, QgisLayerService *layers, QWidget *parent)
  : QWidget(parent)
{
  auto *lay = panelLayout(this);

  lay->addWidget(caption(tr("层位"), this));
  auto *horizons = new QComboBox(this);
  horizons->setObjectName(QStringLiteral("horizonCombo"));
  lay->addWidget(horizons);

  lay->addWidget(caption(tr("算法"), this));
  auto *algos = new QComboBox(this);
  algos->setObjectName(QStringLiteral("algoCombo"));
  lay->addWidget(algos);

  // ONNX params area — visible only for onnx:* algorithms
  auto *paramsArea = new QWidget(this);
  paramsArea->setObjectName(QStringLiteral("onnxParamsArea"));
  auto *paramsLay = new QVBoxLayout(paramsArea);
  paramsLay->setContentsMargins(0, 0, 0, 0);
  paramsLay->setSpacing(8);

  paramsLay->addWidget(caption(tr("输入数据 (逗号分隔浮点数)"), paramsArea));
  auto *inputEdit = new QLineEdit(paramsArea);
  inputEdit->setObjectName(QStringLiteral("onnxInputEdit"));
  inputEdit->setPlaceholderText(QStringLiteral("例如: 0.0, 1.0"));
  paramsLay->addWidget(inputEdit);

  paramsLay->addWidget(caption(tr("输入形状 (逗号分隔整数)"), paramsArea));
  auto *shapeEdit = new QLineEdit(paramsArea);
  shapeEdit->setObjectName(QStringLiteral("onnxShapeEdit"));
  shapeEdit->setPlaceholderText(QStringLiteral("例如: 1, 1"));
  paramsLay->addWidget(shapeEdit);

  paramsLay->addWidget(caption(tr("输入名称"), paramsArea));
  auto *nameEdit = new QLineEdit(paramsArea);
  nameEdit->setObjectName(QStringLiteral("onnxInputNameEdit"));
  nameEdit->setText(QStringLiteral("x"));
  paramsLay->addWidget(nameEdit);

  // 输出网格是工区合同（PROJECT_AREA_PLAN §3）：D61 栅格 411×641 与同一套
  // geotransform，行/列/像元不可配——结果不是 411×641 时 workflow 拒绝写盘。
  paramsLay->addWidget(caption(tr("输出固定为 D61 工区网格 411×641"), paramsArea));

  paramsArea->hide();
  lay->addWidget(paramsArea);

  const auto updateVisibility = [algos, paramsArea] {
    const QString alg = algos->currentData().toString();
    paramsArea->setVisible(alg.startsWith(QLatin1String("onnx:")));
  };
  connect(algos, &QComboBox::activated, this, updateVisibility);
  connect(algos, &QComboBox::currentIndexChanged, this, updateVisibility);

  auto *run = new QPushButton(tr("运行预测"), this);
  run->setObjectName(QStringLiteral("runButton"));
  lay->addWidget(run);
  connect(run, &QPushButton::clicked, this, [this, horizons, algos] {
    const QString horizon = horizons->currentText();
    const QString algId = algos->currentData().toString();
    QVariantMap params;
    if (algId.startsWith(QLatin1String("onnx:")))
    {
      params = parseInputParams();
      if (params.isEmpty())
        return;
    }
    emit runRequested(horizon, algId, params);
  });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  status->setWordWrap(true);
  lay->addWidget(status);
  lay->addStretch(1);

  if (wf) // workflow feedback lands on the status label
  {
    setAlgorithms(wf->availableAlgorithms());
    connect(wf, &PredictionWorkflow::predictionDone, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("预测完成：%1 → %2").arg(h, layerId));
            });
    connect(wf, &PredictionWorkflow::predictionFailed, status,
            [status](const QString &, const QString &error) { status->setText(error); });
#if PALEO_HAVE_ORT
    // 诚实可用性：服务恒绑定，但运行库缺失时 onnx:* 不会出现在算法列表里；
    // 在这里写明原因，而不是静默少列。
    if (!PaleoOnnxService::runtimeAvailable())
      status->setText(tr("ONNX 运行时不可用（vendor/onnxruntime 缺少运行库）"));
#endif
  }
  Q_UNUSED(layers); // reserved: shell lists horizons from the layer service
}

void PredictPage::setHorizons(const QStringList &horizons)
{
  if (auto *combo = child<QComboBox>(this, "horizonCombo"))
  {
    combo->clear();
    combo->addItems(horizons);
  }
}

void PredictPage::setAlgorithms(const QStringList &algIds)
{
  if (auto *combo = child<QComboBox>(this, "algoCombo"))
  {
    combo->clear();
    for (const QString &id : algIds)
    {
      QString display = id;
      if (id.startsWith(QLatin1String("onnx:")))
        display = tr("%1 (ONNX)").arg(id.mid(5));
      combo->addItem(display, id);
    }
    if (auto *paramsArea = child<QWidget>(this, "onnxParamsArea"))
    {
      const QString cur = combo->currentData().toString();
      paramsArea->setVisible(cur.startsWith(QLatin1String("onnx:")));
    }
  }
}

QVariantMap PredictPage::parseInputParams()
{
  auto *status = child<QLabel>(this, "statusLabel");
  auto *inputEdit = child<QLineEdit>(this, "onnxInputEdit");
  auto *shapeEdit = child<QLineEdit>(this, "onnxShapeEdit");
  auto *nameEdit = child<QLineEdit>(this, "onnxInputNameEdit");

  if (!inputEdit || !shapeEdit || !nameEdit)
    return {};

  const QString inStr = inputEdit->text().trimmed();
  if (inStr.isEmpty())
  {
    if (status)
      status->setText(tr("输入数据不能为空"));
    return {};
  }
  const QStringList inParts = inStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
  if (inParts.isEmpty())
  {
    if (status)
      status->setText(tr("输入数据不能为空"));
    return {};
  }
  QVariantList inList;
  for (const QString &p : inParts)
  {
    bool ok = false;
    const float val = p.trimmed().toFloat(&ok);
    if (!ok)
    {
      if (status)
        status->setText(tr("输入数据包含非法浮点数: %1").arg(p.trimmed()));
      return {};
    }
    inList.append(val);
  }

  const QString shapeStr = shapeEdit->text().trimmed();
  if (shapeStr.isEmpty())
  {
    if (status)
      status->setText(tr("输入形状不能为空"));
    return {};
  }
  const QStringList shapeParts = shapeStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
  if (shapeParts.isEmpty())
  {
    if (status)
      status->setText(tr("输入形状不能为空"));
    return {};
  }
  QVariantList shapeList;
  for (const QString &p : shapeParts)
  {
    bool ok = false;
    const qint64 val = p.trimmed().toLongLong(&ok);
    if (!ok)
    {
      if (status)
        status->setText(tr("输入形状包含非法整数: %1").arg(p.trimmed()));
      return {};
    }
    shapeList.append(val);
  }

  QString nameStr = nameEdit->text().trimmed();
  if (nameStr.isEmpty())
    nameStr = QStringLiteral("x");

  QVariantMap params;
  params.insert(QStringLiteral("input"), inList);
  params.insert(QStringLiteral("shape"), shapeList);
  params.insert(QStringLiteral("inputName"), nameStr);
  return params;
}

// ---------------------------------------------------------------------------
// ConstraintPage — ②约束与单因素
// ---------------------------------------------------------------------------
ConstraintPage::ConstraintPage(ConstraintWorkflow *wf, QWidget *parent)
  : QWidget(parent)
{
  auto *lay = panelLayout(this);

  lay->addWidget(caption(tr("层位"), this));
  auto *horizons = new QComboBox(this);
  horizons->setObjectName(QStringLiteral("horizonCombo"));
  lay->addWidget(horizons);

  lay->addWidget(caption(tr("约束"), this));
  auto *list = new QListWidget(this);
  list->setObjectName(QStringLiteral("constraintList"));
  list->setAccessibleName(tr("约束列表"));
  lay->addWidget(list, 1);

  auto *spin = new QSpinBox(this);
  spin->setObjectName(QStringLiteral("faciesCodeSpin"));
  spin->setRange(0, 9999);
  spin->setAccessibleName(tr("相代码"));
  lay->addWidget(spin);

  // Shape picker feeds ConstraintDrawController::startCapture's tool choice.
  auto *shape = new QComboBox(this);
  shape->setObjectName(QStringLiteral("shapeCombo"));
  shape->addItem(tr("约束线"), QStringLiteral("line"));
  shape->addItem(tr("约束多边形"), QStringLiteral("polygon"));
  shape->addItem(tr("约束矩形"), QStringLiteral("rect"));
  shape->addItem(tr("约束点"), QStringLiteral("point"));
  shape->addItem(tr("约束圆"), QStringLiteral("circle"));
  shape->addItem(tr("约束椭圆"), QStringLiteral("ellipse"));
  lay->addWidget(shape);

  auto *draw = new QPushButton(tr("绘制约束"), this);
  draw->setObjectName(QStringLiteral("drawButton"));
  lay->addWidget(draw);
  connect(draw, &QPushButton::clicked, this, [this, horizons, shape, spin] {
    emit drawConstraintRequested(horizons->currentText(),
                                 shape->currentData().toString(), spin->value());
  });

  auto *field = new QLineEdit(QStringLiteral("z"), this);
  field->setObjectName(QStringLiteral("idwField"));
  field->setPlaceholderText(tr("井属性字段"));
  field->setAccessibleName(tr("插值字段"));
  lay->addWidget(field);

  auto *cell = new QDoubleSpinBox(this);
  cell->setObjectName(QStringLiteral("idwCellSize"));
  cell->setRange(0.0001, 1.0e9);
  cell->setDecimals(4);
  cell->setValue(1.0);
  cell->setAccessibleName(tr("像元大小"));
  lay->addWidget(cell);

  auto *idw = new QPushButton(tr("插值"), this);
  idw->setObjectName(QStringLiteral("runIdwButton"));
  lay->addWidget(idw);
  connect(idw, &QPushButton::clicked, this, [this, horizons] {
    emit runIdwRequested(horizons->currentText());
  });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  status->setWordWrap(true);
  lay->addWidget(status);

  // ---- 阶段C 厚度样本表（autoplan §5C）-------------------------------------
  // 逐井：井名 / D61 TVD / D62 TVD / 层间速度或原因。行表由 MappingWorkflow
  // 镜像到 ConstraintWorkflow 的 paleo.thickness.* 动态属性；不足样本的两句
  // （「厚度样本不足以成面」/「没有厚度样本」）渲染在 thicknessHint，不弹框。
  lay->addSpacing(16); // spacing.md
  lay->addWidget(caption(tr("D61→D62 厚度样本"), this));
  auto *thTable = new QTableWidget(0, 4, this);
  thTable->setObjectName(QStringLiteral("thicknessTable"));
  thTable->setAccessibleName(tr("厚度样本表"));
  thTable->setHorizontalHeaderLabels(
      {tr("井名"), tr("D61 TVD"), tr("D62 TVD"), tr("层间速度或原因")});
  thTable->verticalHeader()->setVisible(false);
  thTable->horizontalHeader()->setStretchLastSection(true);
  lay->addWidget(thTable, 1);
  auto *thHint = new QLabel(this);
  thHint->setObjectName(QStringLiteral("thicknessHint"));
  thHint->setWordWrap(true);
  thHint->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
  lay->addWidget(thHint);

  if (wf) // workflow feedback lands on the status label
  {
    setProperty(kWfProp, QVariant::fromValue(static_cast<QObject *>(wf)));
    connect(wf, &ConstraintWorkflow::constraintAdded, status,
            [status](const QString &id) { status->setText(tr("已添加约束 %1").arg(id)); });
    connect(wf, &ConstraintWorkflow::factorDone, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("单因素完成：%1 → %2").arg(h, layerId));
            });
  }
}

void ConstraintPage::showEvent(QShowEvent *event)
{
  QWidget::showEvent(event);
  refreshThicknessSamples();
}

void ConstraintPage::refreshThicknessSamples()
{
  auto *table = child<QTableWidget>(this, "thicknessTable");
  auto *hint = child<QLabel>(this, "thicknessHint");
  if (!table)
    return;
  auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
  const QVariantList rows =
      wf ? wf->property("paleo.thickness.samples").toList() : QVariantList();
  const QString message =
      wf ? wf->property("paleo.thickness.message").toString() : QString();

  table->setRowCount(0);
  for (const QVariant &v : rows)
  {
    const QVariantMap m = v.toMap();
    const int r = table->rowCount();
    table->insertRow(r);
    auto *name = new QTableWidgetItem(m.value(QStringLiteral("well_name")).toString());
    const QString tvdTop = m.contains(QStringLiteral("tvd_top"))
                               ? QString::number(m.value(QStringLiteral("tvd_top")).toDouble(), 'f', 1)
                               : QStringLiteral("—");
    const QString tvdBase = m.contains(QStringLiteral("tvd_base"))
                                ? QString::number(m.value(QStringLiteral("tvd_base")).toDouble(), 'f', 1)
                                : QStringLiteral("—");
    // 贡献井 → 层间速度（m/s）；否则 → 原因文案。
    const QString last = m.value(QStringLiteral("contributing")).toBool()
                             ? tr("%1 m/s").arg(m.value(QStringLiteral("vint")).toDouble(), 0, 'f', 0)
                             : m.value(QStringLiteral("reason")).toString();
    auto *itTop = new QTableWidgetItem(tvdTop);
    auto *itBase = new QTableWidgetItem(tvdBase);
    auto *itV = new QTableWidgetItem(last);
    for (auto *it : {name, itTop, itBase, itV})
      it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    table->setItem(r, 0, name);
    table->setItem(r, 1, itTop);
    table->setItem(r, 2, itBase);
    table->setItem(r, 3, itV);
  }
  if (hint)
    hint->setText(message);
}

// ---------------------------------------------------------------------------
// ComposePage — ③综合编图
// ---------------------------------------------------------------------------
ComposePage::ComposePage(CompositionWorkflow *wf, QgisLayerService *layers, QWidget *parent)
  : QWidget(parent)
{
  setProperty(kLayersProp, QVariant::fromValue(static_cast<QObject *>(layers)));

  auto *lay = panelLayout(this);

  // ---- wave/mapping-pipeline 阶段C+E：D61 编图链 / 导出 / 版本按钮块 ----
  lay->addWidget(caption(tr("编图链（等时差 × 层间速度 → 等厚图）"), this));
  // D8 厚度触发：命名「生成 <层位> 等厚图」，未选层位禁用并写原因；使能态
  // 由壳经 setThicknessHorizon 跟 activeHorizon 联动。
  auto *chain = new QPushButton(tr("生成等厚图"), this);
  chain->setObjectName(QStringLiteral("thicknessChainButton"));
  chain->setEnabled(false);
  chain->setToolTip(tr("先在顶部层位 chip 选择层位"));
  chain->setAccessibleName(tr("生成等厚图"));
  connect(chain, &QPushButton::clicked, this, [this] { emit thicknessChainRequested(); });
  lay->addWidget(chain);

  auto *exportPdf = new QPushButton(tr("导出层位图 PDF"), this);
  exportPdf->setObjectName(QStringLiteral("exportPdfButton"));
  exportPdf->setAccessibleName(tr("导出层位图 PDF"));
  connect(exportPdf, &QPushButton::clicked, this, [this] { emit exportPdfRequested(); });
  lay->addWidget(exportPdf);

  auto *saveVersion = new QPushButton(tr("保存版本"), this);
  saveVersion->setObjectName(QStringLiteral("saveVersionButton"));
  saveVersion->setAccessibleName(tr("保存版本"));
  connect(saveVersion, &QPushButton::clicked, this, [this] { emit saveVersionRequested(); });
  lay->addWidget(saveVersion);

  auto *publish = new QPushButton(tr("发布"), this);
  publish->setObjectName(QStringLiteral("publishButton"));
  publish->setAccessibleName(tr("发布"));
  publish->setEnabled(false); // 发布门：PDF 能导出之后再暴露（shell 开闸）
  publish->setToolTip(tr("导出 PDF 后再保存")); // 门控原因（阶段E：PDF 先行）
  connect(publish, &QPushButton::clicked, this, [this] { emit publishRequested(); });
  lay->addWidget(publish);

  // 版本状态标注（已发布 / 编辑中 / 无版本），跟着 shell 的发布门刷新走。
  auto *publishState = new QLabel(this);
  publishState->setObjectName(QStringLiteral("publishStateLabel"));
  publishState->setAccessibleName(tr("版本发布状态"));
  publishState->setWordWrap(true);
  lay->addWidget(publishState);
  lay->addSpacing(16); // spacing.md between groups

  lay->addWidget(caption(tr("单因素图层"), this));
  auto *list = new QListWidget(this);
  list->setObjectName(QStringLiteral("factorList"));
  list->setAccessibleName(tr("单因素图层列表"));
  lay->addWidget(list, 1);

  auto *fuse = new QPushButton(tr("合成编图"), this);
  fuse->setObjectName(QStringLiteral("fuseButton"));
  lay->addWidget(fuse);
  connect(fuse, &QPushButton::clicked, this, [this, list] {
    QStringList ids;
    for (int i = 0; i < list->count(); ++i)
      if (list->item(i)->checkState() == Qt::Checked)
        ids << list->item(i)->data(Qt::UserRole).toString();
    emit fuseRequested(ids);
  });

  lay->addWidget(caption(tr("沉积相面"), this));
  auto *rasterCombo = new QComboBox(this);
  rasterCombo->setObjectName(QStringLiteral("faciesRasterCombo"));
  rasterCombo->setAccessibleName(tr("待转面的栅格"));
  lay->addWidget(rasterCombo);

  auto *minArea = new QDoubleSpinBox(this);
  minArea->setObjectName(QStringLiteral("minAreaSpin"));
  minArea->setAccessibleName(tr("碎屑面积阈值"));
  minArea->setDecimals(4);
  minArea->setRange(0.0, 1.0e9);
  minArea->setSingleStep(1.0);
  minArea->setPrefix(tr("最小面积 "));
  lay->addWidget(minArea);

  auto *simplify = new QDoubleSpinBox(this);
  simplify->setObjectName(QStringLiteral("simplifySpin"));
  simplify->setAccessibleName(tr("边界简化容差"));
  simplify->setDecimals(4);
  simplify->setRange(0.0, 1.0e9);
  simplify->setSingleStep(1.0);
  simplify->setPrefix(tr("简化容差 "));
  lay->addWidget(simplify);

  auto *polygonize = new QPushButton(tr("转为相多边形"), this);
  polygonize->setObjectName(QStringLiteral("polygonizeButton"));
  polygonize->setAccessibleName(tr("转为相多边形"));
  lay->addWidget(polygonize);
  connect(polygonize, &QPushButton::clicked, this, [this, rasterCombo, minArea, simplify] {
    const QString layerId = rasterCombo->currentData().toString();
    if (layerId.isEmpty())
    {
      if (auto *status = child<QLabel>(this, "statusLabel"))
        status->setText(tr("还没有可转面的栅格 — 先运行预测或合成编图"));
      return;
    }
    emit polygonizeRequested(layerId, minArea->value(), simplify->value());
  });
  // autoplan §5C：厚度栅格不是相编码；工程里没有相编码栅格时明确说明，
  // 而不是笼统的「还没有可转面的栅格」。
  connect(polygonize, &QPushButton::clicked, this, [this, rasterCombo] {
    if (!rasterCombo->currentData().toString().isEmpty())
      return;
    auto *status = child<QLabel>(this, "statusLabel");
    auto *layers = qobject_cast<QgisLayerService *>(
        property(kLayersProp).value<QObject *>());
    QVector<LayerDeclaration> declared;
    if (status && layers && layers->tryDeclared(&declared))
    {
      const bool anyRaster = std::any_of(
          declared.cbegin(), declared.cend(), [](const LayerDeclaration &d) {
            return d.type.compare(QLatin1String("raster"), Qt::CaseInsensitive) == 0;
          });
      if (anyRaster)
        status->setText(tr("没有相编码栅格，这一工区不从厚度生成相"));
    }
  });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  status->setWordWrap(true);
  lay->addWidget(status);
  if (wf)
  {
    connect(wf, &CompositionWorkflow::compositionDone, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("合成完成：%1 → %2").arg(h, layerId));
            });
    connect(wf, &CompositionWorkflow::faciesPolygonsReady, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("相多边形完成：%1 → %2").arg(h, layerId));
            });
    connect(wf, &CompositionWorkflow::faciesPolygonsFailed, status,
            [status](const QString &, const QString &error) { status->setText(error); });
  }
  // A raster declared after this page exists (e.g. a fresh ONNX prediction)
  // still lands in the combo; the declaration set is the source of truth.
  if (layers)
    connect(layers, &QgisLayerService::layerDeclared, this,
            [this](const QString &) { refreshFactors(); });

  refreshFactors();
}

void ComposePage::setPublishEnabled(bool enabled)
{
  setPublishState(enabled, -1, -1); // 旧调用形态：不带残差评估
}

void ComposePage::setPublishState(bool hasPdf, int covered, int total)
{
  auto *btn = child<QPushButton>(this, "publishButton");
  if (!btn)
    return;
  // covered<0 = 调用方没评估残差 → 不拿残差卡门；其余情形须 total>0 且全
  // 覆盖（每口井都有残差或原因，§177）。
  const bool residualsOk = covered < 0 || (total > 0 && covered == total);
  btn->setEnabled(hasPdf && residualsOk);
  QString tip;
  if (!hasPdf)
    tip = tr("导出 PDF 后再保存");
  else if (!residualsOk)
    tip = total > 0 ? tr("还有 %1/%2 口井没有残差或原因 — 先在验证页运行验证")
                        .arg(total - covered)
                        .arg(total)
                    : tr("先在验证页运行验证");
  btn->setToolTip(tip);
}

void ComposePage::setVersionState(int version, bool published)
{
  if (auto *label = child<QLabel>(this, "publishStateLabel"))
    label->setText(version <= 0 ? tr("还没有保存的版本")
                   : published  ? tr("已发布 · v%1").arg(version)
                                : tr("编辑中 · v%1").arg(version));
  if (auto *save = child<QPushButton>(this, "saveVersionButton"))
    save->setText(published ? tr("保存新版本") : tr("保存版本"));
}

void ComposePage::setThicknessHorizon(const QString &horizon)
{
  auto *chain = child<QPushButton>(this, "thicknessChainButton");
  if (!chain)
    return;
  chain->setEnabled(!horizon.isEmpty());
  chain->setText(horizon.isEmpty() ? tr("生成等厚图")
                                  : tr("生成 %1 等厚图").arg(horizon));
  chain->setAccessibleName(chain->text());
  chain->setToolTip(horizon.isEmpty()
                        ? tr("先在顶部层位 chip 选择层位")
                        : tr("等时差（双向 ms）× 层间速度 IDW → 厚度栅格"));
}

void ComposePage::refreshFactors()
{
  auto *list = child<QListWidget>(this, "factorList");
  if (!list)
    return;
  auto *layers = qobject_cast<QgisLayerService *>(
      property(kLayersProp).value<QObject *>());
  // Manifest read failure keeps current content — an empty declaration set
  // here would silently blank the page.
  QVector<LayerDeclaration> declared;
  if (!layers || !layers->tryDeclared(&declared))
    return;
  list->clear();
  for (const LayerDeclaration &d : declared)
  {
    if (d.group != QLatin1String("04_SingleFactor"))
      continue;
    auto *it = new QListWidgetItem(d.layerId, list);
    it->setData(Qt::UserRole, d.layerId);
    it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
    it->setCheckState(Qt::Unchecked);
  }

  auto *combo = child<QComboBox>(this, "faciesRasterCombo");
  if (!combo)
    return;
  const QString previous = combo->currentData().toString();
  combo->clear();
  for (const LayerDeclaration &d : declared)
  {
    const bool raster = d.type.compare(QLatin1String("raster"), Qt::CaseInsensitive) == 0;
    const bool groupOk = d.group == QLatin1String("03_Composite") ||
                         d.group == QLatin1String("01_Prediction") ||
                         d.group == QLatin1String("02_Prediction") ||
                         d.group == QLatin1String("03_Predict");
    const bool idOk = d.layerId.startsWith(QLatin1String("composite.")) ||
                      d.layerId.startsWith(QLatin1String("pred.")) ||
                      d.layerId.startsWith(QLatin1String("predict."));
    if (!raster || (!groupOk && !idOk))
      continue;
    combo->addItem(d.layerId, d.layerId);
  }
  const int keep = combo->findData(previous);
  if (keep >= 0)
    combo->setCurrentIndex(keep);
}

// ---------------------------------------------------------------------------
// ValidatePage — ④验证
// ---------------------------------------------------------------------------
ValidatePage::ValidatePage(ValidationWorkflow *wf, QWidget *parent)
  : QWidget(parent)
{
  setProperty(kWfProp, QVariant::fromValue(static_cast<QObject *>(wf)));

  auto *lay = panelLayout(this);
  auto *run = new QPushButton(tr("运行验证"), this);
  run->setObjectName(QStringLiteral("runButton"));
  lay->addWidget(run);
  connect(run, &QPushButton::clicked, this, [this] { populate(); });

  // autoplan §5C：D61 残差表 —— 每口井一行（井名/残差或原因/阈值），
  // 状态字+颜色（通过 #43A047 / 超过阈值 #F29900 / 未计算 #5D6E80）。
  // 计数行在表头；还没跑时面板写「还没有计算 D61 残差」。
  auto *resSummary = new QLabel(tr("还没有计算 D61 残差"), this);
  resSummary->setObjectName(QStringLiteral("residualSummaryLabel"));
  lay->addWidget(resSummary);
  lay->addWidget(caption(tr("D61 时间残差"), this));
  auto *resTable = new QTableWidget(0, 3, this);
  resTable->setObjectName(QStringLiteral("residualTable"));
  resTable->setAccessibleName(tr("D61 残差表"));
  resTable->setHorizontalHeaderLabels({tr("井名"), tr("残差或原因"), tr("阈值")});
  resTable->verticalHeader()->setVisible(false);
  resTable->horizontalHeader()->setStretchLastSection(true);
  lay->addWidget(resTable, 1);
  // T24：残差行双击与问题行同一条 locateRequested——列 0 上挂
  // layerId/WKT/payload（populate 写入），三视图按同一载荷联动。
  connect(resTable, &QTableWidget::itemDoubleClicked, this, [this, resTable](QTableWidgetItem *it) {
    if (!it)
      return;
    auto *first = resTable->item(it->row(), 0);
    if (first)
      emit locateRequested(first->data(Qt::UserRole).toString(),
                           first->data(Qt::UserRole + 1).toString(),
                           first->data(Qt::UserRole + 2).toMap());
  });

  auto *table = new QTableWidget(0, 4, this);
  table->setObjectName(QStringLiteral("issueTable"));
  table->setAccessibleName(tr("验证问题列表"));
  table->setHorizontalHeaderLabels({tr("级别"), tr("代码"), tr("信息"), tr("图层")});
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  lay->addWidget(table, 1);
  connect(table, &QTableWidget::itemDoubleClicked, this, [this, table](QTableWidgetItem *it) {
    if (!it)
      return;
    auto *first = table->item(it->row(), 0); // issue data lives on column 0
    if (first)
      emit locateRequested(first->data(Qt::UserRole).toString(),
                           first->data(Qt::UserRole + 1).toString(),
                           first->data(Qt::UserRole + 2).toMap());
  });

  // 「在数据页看这条剖面」（预览壳重排）：问题行/残差行的载荷里带 inline
  // 测线号才可用；点击把整份 payload 原样发给 shell（切数据页+聚焦该测线）。
  // 两张表共用一颗钮——armed 载荷记在按钮属性上，最近一次选中的表生效。
  auto *openSection = new QPushButton(tr("在数据页看这条剖面"), this);
  openSection->setObjectName(QStringLiteral("openSeismicSectionButton"));
  openSection->setAccessibleName(tr("在数据页看这条剖面"));
  openSection->setEnabled(false);
  lay->addWidget(openSection);
  const auto hasSection = [](const QVariantMap &p) {
    return p.value(QStringLiteral("inline"), -1).toInt() >= 0;
  };
  const auto armSectionFrom = [openSection, hasSection](QTableWidget *src) {
    int row = src ? src->currentRow() : -1;
    if (row < 0 && src)
    {
      const QList<QTableWidgetItem *> sel = src->selectedItems();
      if (!sel.isEmpty())
        row = sel.front()->row();
    }
    auto *first = (src && row >= 0) ? src->item(row, 0) : nullptr;
    const QVariantMap p = first ? first->data(Qt::UserRole + 2).toMap() : QVariantMap();
    openSection->setProperty("armedPayload", p);
    openSection->setEnabled(hasSection(p));
  };
  connect(table, &QTableWidget::itemSelectionChanged, openSection,
          [armSectionFrom, table]() { armSectionFrom(table); });
  connect(resTable, &QTableWidget::itemSelectionChanged, openSection,
          [armSectionFrom, resTable]() { armSectionFrom(resTable); });
  connect(openSection, &QPushButton::clicked, this,
          [this, openSection, hasSection]() {
            const QVariantMap p = openSection->property("armedPayload").toMap();
            if (hasSection(p))
              emit seismicSectionRequested(p);
          });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  lay->addWidget(status);
  if (wf)
    connect(wf, &ValidationWorkflow::validationDone, status,
            [status](int n) { status->setText(tr("发现 %1 个问题").arg(n)); });
}

void ValidatePage::populate()
{
  auto *table = child<QTableWidget>(this, "issueTable");
  if (!table)
    return;
  table->setRowCount(0);
  auto *wf = qobject_cast<ValidationWorkflow *>(
      property(kWfProp).value<QObject *>());
  if (!wf)
    return;
  const QList<ValidationIssue> issues = wf->validate();
  for (const ValidationIssue &v : issues)
  {
    const int row = table->rowCount();
    table->insertRow(row);
    auto *sev = new QTableWidgetItem(severityText(v.severity));
    sev->setForeground(severityColor(v.severity));
    sev->setData(Qt::UserRole, v.layerId);       // locate intent reads these
    sev->setData(Qt::UserRole + 1, v.wktLocation);
    // 三视图联动载荷：wellId/horizon/inline/time_ms（非残差问题不含井字段）。
    QVariantMap payload = v.details;
    if (!v.wellId.isEmpty())
      payload.insert(QStringLiteral("wellId"), v.wellId);
    if (!v.horizon.isEmpty())
      payload.insert(QStringLiteral("horizon"), v.horizon);
    sev->setData(Qt::UserRole + 2, payload);
    sev->setFlags(sev->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, 0, sev);
    auto *code = new QTableWidgetItem(v.code);
    auto *msg = new QTableWidgetItem(v.message);
    auto *layer = new QTableWidgetItem(v.layerId);
    for (auto *it : {code, msg, layer})
      it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, 1, code);
    table->setItem(row, 2, msg);
    table->setItem(row, 3, layer);
  }

  // ---- D61 逐井残差表（autoplan §5C）--------------------------------------
  auto *resTable = child<QTableWidget>(this, "residualTable");
  auto *resSummary = child<QLabel>(this, "residualSummaryLabel");
  if (!resTable)
    return;
  resTable->setRowCount(0);
  const QVariantList rows = wf->lastResidualRows();
  if (rows.isEmpty())
  {
    // 空残差表两种含义要分清（T25）：D61 栅格缺失/打不开时工作流会
    // 发 RASTER_MISSING 问题——摘要行复述它的原因，不冒充「没跑过」。
    QString rasterReason;
    for (const ValidationIssue &v : issues)
      if (v.code == QLatin1String("RASTER_MISSING"))
        rasterReason = v.message;
    if (resSummary)
      resSummary->setText(rasterReason.isEmpty()
                              ? tr("还没有计算 D61 残差")
                              : rasterReason);
    return;
  }
  const double thr = rows.first().toMap()
                         .value(QStringLiteral("threshold_ms"), 10.0)
                         .toDouble();
  int nExceed = 0;
  for (const QVariant &v : rows)
  {
    const QVariantMap m = v.toMap();
    const QString status = m.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("exceed"))
      ++nExceed;
    const int r = resTable->rowCount();
    resTable->insertRow(r);
    QString word;
    QColor color;
    if (status == QLatin1String("pass"))
    {
      word = tr("通过");
      color = QColor(QStringLiteral("#43A047")); // success
    }
    else if (status == QLatin1String("exceed"))
    {
      word = tr("超过阈值");
      color = QColor(QStringLiteral("#F29900")); // warning
    }
    else if (status == QLatin1String("warn"))
    {
      word = tr("警告");
      color = QColor(QStringLiteral("#F29900")); // 警告行，不算数值残差
    }
    else
    {
      word = tr("未计算");
      color = QColor(QStringLiteral("#5D6E80")); // text-muted
    }
    const QString value = m.contains(QStringLiteral("residual_ms"))
                              ? tr("%1 %2 ms").arg(word).arg(
                                    m.value(QStringLiteral("residual_ms")).toDouble(), 0, 'f', 1)
                              : tr("%1 · %2").arg(word, m.value(QStringLiteral("reason")).toString());
    auto *name = new QTableWidgetItem(m.value(QStringLiteral("well_name")).toString());
    // T24：残差行与问题行共用三视图联动载荷——列 0 挂 layerId/POINT WKT/
    // payload（wellId/horizon/well_x/well_y/inline/time_ms）。采样点是分层
    // X/Y（缺省井口），缺坐标的行不填 (0,0)（threewaylocator 只认成对字段）。
    name->setData(Qt::UserRole, m.value(QStringLiteral("layer_id")).toString());
    const bool hasXY = m.contains(QStringLiteral("x")) && m.contains(QStringLiteral("y"));
    if (hasXY)
      name->setData(Qt::UserRole + 1,
                    QStringLiteral("POINT(%1 %2)")
                        .arg(m.value(QStringLiteral("x")).toDouble())
                        .arg(m.value(QStringLiteral("y")).toDouble()));
    QVariantMap payload;
    payload.insert(QStringLiteral("wellId"), m.value(QStringLiteral("well_id")).toString());
    payload.insert(QStringLiteral("horizon"), m.value(QStringLiteral("horizon")).toString());
    payload.insert(QStringLiteral("well_name"), m.value(QStringLiteral("well_name")).toString());
    payload.insert(QStringLiteral("inline"), m.value(QStringLiteral("inline"), -1).toInt());
    if (hasXY)
    {
      payload.insert(QStringLiteral("well_x"), m.value(QStringLiteral("x")));
      payload.insert(QStringLiteral("well_y"), m.value(QStringLiteral("y")));
    }
    if (m.contains(QStringLiteral("time_ms")))
      payload.insert(QStringLiteral("time_ms"), m.value(QStringLiteral("time_ms")));
    if (m.contains(QStringLiteral("residual_ms")))
      payload.insert(QStringLiteral("residual_ms"), m.value(QStringLiteral("residual_ms")));
    name->setData(Qt::UserRole + 2, payload);
    auto *val = new QTableWidgetItem(value);
    val->setForeground(color);
    auto *thrItem = new QTableWidgetItem(tr("%1 ms").arg(thr, 0, 'f', 0));
    for (auto *it : {name, val, thrItem})
      it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    resTable->setItem(r, 0, name);
    resTable->setItem(r, 1, val);
    resTable->setItem(r, 2, thrItem);
  }
  if (resSummary)
    resSummary->setText(tr("%1 口超过 %2 ms").arg(nExceed).arg(thr, 0, 'f', 0));
}
