#include "pagepanels.h"

#include "../paleotheme.h" // DESIGN.md token 出口：胶囊/mono 数字面

#include "../../ai/onnxpredictionservice.h" // ORT-free header; runtimeAvailable 调用受 PALEO_HAVE_ORT 保护
#include "../../catalog/datacatalog.h"
#include "../../io/dataimportservice.h"
#include "../../workflow/workflows.h"    // signal names + ValidationWorkflow::validate
#include "../../qgis/qgislayerservice.h" // declared() — forward-declares Qgs*, none included
#include "../../metadata/layermanifest.h" // LayerDeclaration fields
#include "../../domain/types.h"          // ValidationIssue fields

#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

  // §42.4/T31: an empty asset table shows a centered guidance row — never a
  // blank panel. Copy names the next concrete step (导入工区文件夹 first).
  void refreshAssetEmptyState(QTableWidget *t, const QString &text)
  {
    if (t->rowCount() > 0)
      return;
    t->insertRow(0);
    auto *it = new QTableWidgetItem(text.isEmpty()
                                        ? DataPage::tr("还没有数据资产 — 先导入工区文件夹，"
                                                       "或用上方按钮导入单个文件")
                                        : text);
    it->setFlags(Qt::NoItemFlags);
    it->setForeground(QColor(QStringLiteral("#5D6E80"))); // text-muted
    it->setTextAlignment(Qt::AlignCenter);                // T31 居中提示
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

  // T27 胶囊化：级别/状态文字走 DESIGN.md status-tag（浅底深字胶囊），
  // 不再用 setForeground 彩色裸文字（对比度 2.3–3.3:1 不达标）。
  // 信息是中性事实（不占语义色）；错误/警告走各自语义 token。
  PaleoTheme::CapsuleKind severityCapsule(ValidationIssue::Severity s)
  {
    switch (s)
    {
      case ValidationIssue::Error: return PaleoTheme::CapsuleKind::Error;
      case ValidationIssue::Warning: return PaleoTheme::CapsuleKind::Warning;
      case ValidationIssue::Info: // fall through
      default: return PaleoTheme::CapsuleKind::Neutral;
    }
  }

  PaleoTheme::CapsuleKind residualCapsule(const QString &status)
  {
    if (status == QLatin1String("pass"))
      return PaleoTheme::CapsuleKind::Success;
    if (status == QLatin1String("exceed") || status == QLatin1String("warn"))
      return PaleoTheme::CapsuleKind::Warning;
    return PaleoTheme::CapsuleKind::Neutral; // 未计算
  }

  // ---- T28：链接身份寻址 + undo 库 -----------------------------------------
  // 动作（挂接/撤销/设为主版本）按 (assetId, role[, entityId]) 链接身份在
  // 点击时刻重扫 links()，不用刷新时捕获的行下标——open() 重建或任何
  // 中途变更后仍命中同一条链接。
  int indexOfLink(DataCatalog *cat, const QString &assetId, const QString &role,
                  const QString &entityId)
  {
    const QVector<EntityAssetLink> ls = cat->links();
    for (int i = 0; i < ls.size(); ++i)
    {
      const EntityAssetLink &l = ls.at(i);
      if (l.assetId != assetId || l.role != role)
        continue;
      if (entityId.isEmpty())
      {
        if (l.unresolved)
          return i; // 未决链接：entityId 空
      }
      else if (!l.unresolved && l.entityId == entityId)
        return i;
    }
    return -1;
  }

  // undo 记录：attach 时落档，撤销时消费。跨 open() 持久（随工程的
  // sidecar .paleo/undo_stack.json——catalog 无 note 写回 API（A 包接缝），
  // note 在 UI 层保管：撤销后未决徽标 tooltip 从 note 记忆恢复显示）。
  struct UndoRecord
  {
    QString assetId, entityId, role, entityType, note;
    // 被 attach 降级的前主关联（D4：撤销恢复降级 primary）；空 = 没有。
    QString demotedAssetId, demotedEntityId, demotedRole, demotedEntityType;
    bool isValid() const
    {
      return !assetId.isEmpty() && !entityId.isEmpty() && !role.isEmpty();
    }
  };

  QString undoVaultPath(DataCatalog *cat)
  {
    const QString cp = cat ? cat->catalogPath() : QString();
    if (cp.isEmpty() || !cat->isOpen())
      return QString();
    // catalog 在 <projectDir>/artifacts/metadata/ → 退两级到工程目录。
    const QString projectDir = QFileInfo(QFileInfo(cp).dir().absolutePath())
                                   .dir()
                                   .absolutePath();
    return QDir(projectDir).filePath(QStringLiteral(".paleo/undo_stack.json"));
  }

  // 副作用清洗：attach 的目标链接已不在（被外部改掉/新会话没有这条挂接）
  // → 记录作废剔除；note 记忆只留仍未决链接能用的。
  QVector<UndoRecord> loadUndoVault(DataCatalog *cat, QHash<QString, QString> *notesOut)
  {
    QVector<UndoRecord> records;
    const QString path = undoVaultPath(cat);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      return records;
    const QJsonObject root =
        QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue &v : root.value(QStringLiteral("undo")).toArray())
    {
      const QJsonObject o = v.toObject();
      UndoRecord r;
      r.assetId = o.value(QStringLiteral("asset_id")).toString();
      r.entityId = o.value(QStringLiteral("entity_id")).toString();
      r.role = o.value(QStringLiteral("role")).toString();
      r.entityType = o.value(QStringLiteral("entity_type")).toString();
      r.note = o.value(QStringLiteral("note")).toString();
      const QJsonObject d = o.value(QStringLiteral("demoted")).toObject();
      r.demotedAssetId = d.value(QStringLiteral("asset_id")).toString();
      r.demotedEntityId = d.value(QStringLiteral("entity_id")).toString();
      r.demotedRole = d.value(QStringLiteral("role")).toString();
      r.demotedEntityType = d.value(QStringLiteral("entity_type")).toString();
      if (r.isValid() && indexOfLink(cat, r.assetId, r.role, r.entityId) >= 0)
        records.append(r); // 链接仍在（已决到同一实体）→ 撤销入口有效
    }
    if (notesOut)
      for (const QJsonValue &v : root.value(QStringLiteral("notes")).toArray())
      {
        const QJsonObject o = v.toObject();
        const QString key = o.value(QStringLiteral("asset_id")).toString() +
                            QLatin1Char('|') + o.value(QStringLiteral("role")).toString();
        notesOut->insert(key, o.value(QStringLiteral("note")).toString());
      }
    return records;
  }

  void saveUndoVault(DataCatalog *cat, const QVector<UndoRecord> &records,
                     const QHash<QString, QString> &notes)
  {
    const QString path = undoVaultPath(cat);
    if (path.isEmpty())
      return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonObject root;
    QJsonArray arr;
    for (const UndoRecord &r : records)
    {
      QJsonObject o;
      o.insert(QStringLiteral("asset_id"), r.assetId);
      o.insert(QStringLiteral("entity_id"), r.entityId);
      o.insert(QStringLiteral("role"), r.role);
      o.insert(QStringLiteral("entity_type"), r.entityType);
      o.insert(QStringLiteral("note"), r.note);
      if (!r.demotedAssetId.isEmpty())
      {
        QJsonObject d;
        d.insert(QStringLiteral("asset_id"), r.demotedAssetId);
        d.insert(QStringLiteral("entity_id"), r.demotedEntityId);
        d.insert(QStringLiteral("role"), r.demotedRole);
        d.insert(QStringLiteral("entity_type"), r.demotedEntityType);
        o.insert(QStringLiteral("demoted"), d);
      }
      arr.append(o);
    }
    root.insert(QStringLiteral("undo"), arr);
    QJsonArray noteArr;
    for (auto it = notes.constBegin(); it != notes.constEnd(); ++it)
    {
      const int sep = it.key().indexOf(QLatin1Char('|'));
      QJsonObject o;
      o.insert(QStringLiteral("asset_id"), it.key().left(sep));
      o.insert(QStringLiteral("role"), it.key().mid(sep + 1));
      o.insert(QStringLiteral("note"), it.value());
      noteArr.append(o);
    }
    root.insert(QStringLiteral("notes"), noteArr);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
      f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    // 写失败只在日志说明——undo 是增强面，不阻塞挂接本身。
    else
      qWarning() << "DataPage undo vault write failed:" << path;
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
  const struct { const char *name; const char *text; const char *kind; const char *desc; }
      kImports[] = {
    {"importWells", QT_TR_NOOP("导入井数据"), "wells",
     QT_TR_NOOP("选择单个井位/测井/分层文件入库")},
    {"importSeismic", QT_TR_NOOP("导入地震数据"), "seismic",
     QT_TR_NOOP("选择 SEG-Y 等地震数据文件入库")},
    {"importBoundary", QT_TR_NOOP("导入边界数据"), "boundary",
     QT_TR_NOOP("选择边界矢量文件入库")},
    {"importFolder", QT_TR_NOOP("导入工区文件夹"), "folder",
     QT_TR_NOOP("选择工区目录：确认每个文件的类型后整目录入库")},
  };
  for (const auto &spec : kImports)
  {
    auto *btn = new QPushButton(tr(spec.text), this);
    btn->setObjectName(QLatin1String(spec.name));
    // T32 a11y：导入入口各自报名（屏幕阅读器不读图标猜测）。
    btn->setAccessibleName(tr(spec.text));
    btn->setAccessibleDescription(tr(spec.desc));
    connect(btn, &QPushButton::clicked, this,
            [this, kind = QLatin1String(spec.kind)] { emit importRequested(kind); });
    lay->addWidget(btn);
  }

  lay->addSpacing(16); // spacing.md between groups
  lay->addWidget(caption(tr("资产"), this));
  // T31「查看未决」过滤条：过滤开启时露出一行，带「清除过滤」。
  auto *filterBar = new QWidget(this);
  filterBar->setObjectName(QStringLiteral("unresolvedFilterBar"));
  filterBar->hide();
  auto *fl = new QHBoxLayout(filterBar);
  fl->setContentsMargins(0, 0, 0, 0);
  fl->setSpacing(4);
  auto *filterText = new QLabel(tr("只显示未决资产"), filterBar);
  filterText->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
  fl->addWidget(filterText);
  auto *clearBtn = new QPushButton(tr("清除过滤"), filterBar);
  clearBtn->setObjectName(QStringLiteral("clearUnresolvedFilterButton"));
  clearBtn->setFlat(true);
  fl->addWidget(clearBtn);
  fl->addStretch(1);
  connect(clearBtn, &QPushButton::clicked, this, [this]() { setUnresolvedFilter(false); });
  lay->addWidget(filterBar);
  auto *table = new QTableWidget(0, 3, this);
  table->setObjectName(QStringLiteral("assetTable"));
  table->setAccessibleName(tr("资产列表"));
  // §4 预览壳重排：预览移到共享地图下方（不再挂右栏）；第三列由「来源」改为
  // 「关联」——已决链接写实体名、未决给「未决」徽标+挂接控件、参考资产写「参考」。
  table->setHorizontalHeaderLabels({tr("名称"), tr("类型"), tr("关联")});
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  refreshAssetEmptyState(table, QString());
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
  // T31「查看未决」：过滤开启时只留仍有未决链接的资产行。
  const bool unresolvedOnly =
      property("paleo.page.filterUnresolved").toBool();
  for (const CatalogAsset &a : cat->assets())
  {
    if (unresolvedOnly)
    {
      bool anyUnresolved = false;
      for (const EntityAssetLink &l : allLinks)
        if (l.assetId == a.id && l.unresolved)
        {
          anyUnresolved = true;
          break;
        }
      if (!anyUnresolved)
        continue;
    }
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
    // 身份而非下标（T28）：动作在点击时刻按这些键重扫 links()。
    QString unresolvedRole, unresolvedEntityType; // 本资产第一条未决链接的身份
    const UndoRecord *undoRecord = nullptr;       // 本资产可撤销的挂接记录
    QString promotableRole, promotableEntityId;   // 已决非主链接（「设为主版本」）
    for (int i = 0; i < allLinks.size(); ++i)
    {
      const EntityAssetLink &l = allLinks.at(i);
      if (l.assetId != a.id)
        continue;
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
    bl->setContentsMargins(4, 1, 4, 1);
    bl->setSpacing(4);
    if (!resolved.isEmpty())
    {
      auto *names = new QLabel(resolved.join(QStringLiteral("、")), browse);
      names->setStyleSheet(QStringLiteral("color: #24303E;"));
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
      // §4 未决徽标：bg #FFF4E0 / text #24303E / border #F29900（DESIGN.md）。
      badge->setStyleSheet(QStringLiteral(
          "background: #FFF4E0; color: #24303E; border: 1px solid #F29900;"
          "border-radius: 3px; padding: 0 6px;"));
      badge->setToolTip(badgeNote.isEmpty() ? tr("未决关联") : badgeNote);
      bl->addWidget(badge);

      // 下拉框：全量同类实体（井→全部井实体），默认空（哨兵项）。
      auto *combo = new QComboBox(browse);
      combo->setObjectName(QStringLiteral("resolveEntityCombo"));
      combo->setAccessibleName(tr("挂到实体"));
      const bool isWell = unresolvedEntityType == QLatin1String("well");
      combo->addItem(isWell ? tr("（选择井）") : tr("（选择实体）"), QString());
      for (const CatalogEntity &e : cat->entities(unresolvedEntityType))
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
            tr("把「%1」挂到「%2」？").arg(displayName, ename));
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
                const EntityAssetLink link = ls.at(idx);
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
          confirmText->setText(tr("把「%1」挂到「%2」？")
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
              });
      bl->addWidget(undo);
    }

    if (!promotableRole.isEmpty())
    {
      // 「将此版本设为主版本」：同（实体,角色）的旧版本资产可拿回主关联——
      // 只动链接的 isPrimary 标志，不复制版本字节（§4）。身份寻址（T28）。
      auto *primary = new QPushButton(tr("设为主版本"), browse);
      primary->setObjectName(QStringLiteral("setPrimaryButton"));
      primary->setToolTip(tr("同角色旧版本 — 把这条关联设为主关联"));
      connect(primary, &QPushButton::clicked, this,
              [this, cat, assetId = a.id, role = promotableRole,
               eid = promotableEntityId]() {
                const int idx = indexOfLink(cat, assetId, role, eid);
                QString err;
                if (idx < 0 || !cat->setLinkPrimary(idx, &err))
                  qWarning() << "DataPage setLinkPrimary failed:" << err;
                refreshAssetTable();
              });
      bl->addWidget(primary);
    }

    bl->addStretch(1);
    table->setCellWidget(r, 2, cell);
  }
  refreshAssetEmptyState(
      table, unresolvedOnly ? tr("没有未决资产 — 全部资产都已挂接") : QString());
}

void DataPage::setUnresolvedFilter(bool on)
{
  setProperty("paleo.page.filterUnresolved", on);
  if (auto *bar = findChild<QWidget *>(QStringLiteral("unresolvedFilterBar")))
    bar->setVisible(on);
  refreshAssetTable();
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
  {
    // T27：版本状态胶囊——已发布=绿、编辑中=橙、无版本=中性「未计算」类。
    label->setText(version <= 0 ? tr("还没有保存的版本")
                   : published  ? tr("已发布 · v%1").arg(version)
                                : tr("编辑中 · v%1").arg(version));
    label->setStyleSheet(PaleoTheme::capsuleStyleSheet(
        version <= 0 ? PaleoTheme::CapsuleKind::Neutral
                     : (published ? PaleoTheme::CapsuleKind::Success
                                  : PaleoTheme::CapsuleKind::Warning)));
  }
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
    auto *sev = new QTableWidgetItem(); // 级别文字进胶囊控件（T27）
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
    table->setCellWidget(row, 0,
                         PaleoTheme::capsuleLabel(severityText(v.severity),
                                                  severityCapsule(v.severity), table));
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
  fillResidualTable(resTable, rows);
  if (resSummary)
  {
    int nExceed = 0;
    for (const QVariant &v : rows)
      if (v.toMap().value(QStringLiteral("status")).toString() == QLatin1String("exceed"))
        ++nExceed;
    const double thr = rows.first().toMap()
                           .value(QStringLiteral("threshold_ms"), 10.0)
                           .toDouble();
    resSummary->setText(tr("%1 口超过 %2 ms").arg(nExceed).arg(thr, 0, 'f', 0));
  }
}

void ValidatePage::fillResidualTable(QTableWidget *resTable, const QVariantList &rows)
{
  if (!resTable || rows.isEmpty())
    return;
  const double thr = rows.first().toMap()
                         .value(QStringLiteral("threshold_ms"), 10.0)
                         .toDouble();
  for (const QVariant &v : rows)
  {
    const QVariantMap m = v.toMap();
    const QString status = m.value(QStringLiteral("status")).toString();
    const int r = resTable->rowCount();
    resTable->insertRow(r);
    QString word;
    if (status == QLatin1String("pass"))
      word = tr("通过");
    else if (status == QLatin1String("exceed"))
      word = tr("超过阈值");
    else if (status == QLatin1String("warn"))
      word = tr("警告");
    else
      word = tr("未计算"); // 中性胶囊：无栅格/未跑，不占语义色
    const double residualMs = m.value(QStringLiteral("residual_ms")).toDouble();
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
    auto *val = new QTableWidgetItem(); // 文本进胶囊+mono 值控件（T27/T32）
    // 状态胶囊 + 数值 mono 面：残差数字右对齐等宽（DESIGN.md mono token）。
    auto *cell = new QWidget(resTable);
    auto *hl = new QHBoxLayout(cell);
    hl->setContentsMargins(4, 1, 4, 1);
    hl->setSpacing(4);
    hl->addWidget(PaleoTheme::capsuleLabel(word, residualCapsule(status), cell));
    if (m.contains(QStringLiteral("residual_ms")))
    {
      auto *num = new QLabel(tr("%1 ms").arg(residualMs, 0, 'f', 1), cell);
      num->setFont(PaleoTheme::monoFont());
      num->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      num->setStyleSheet(QStringLiteral("color: #24303E;"));
      hl->addWidget(num);
    }
    else if (!m.value(QStringLiteral("reason")).toString().isEmpty())
    {
      auto *reason = new QLabel(m.value(QStringLiteral("reason")).toString(), cell);
      reason->setWordWrap(true);
      reason->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
      hl->addWidget(reason, 1);
    }
    hl->addStretch(1);
    auto *thrItem = new QTableWidgetItem(tr("%1 ms").arg(thr, 0, 'f', 0));
    thrItem->setFont(PaleoTheme::monoFont()); // 阈值列也是数字面
    thrItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    for (auto *it : {name, val, thrItem})
      it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    resTable->setItem(r, 0, name);
    resTable->setItem(r, 1, val);
    resTable->setItem(r, 2, thrItem);
    resTable->setCellWidget(r, 1, cell);
  }
}
