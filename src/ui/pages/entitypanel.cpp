// 层：视图
#include "entitypanel.h"
#include "pageshared.h"
#include "../paleotheme.h"
#include "../../services/previewdoc.h"
#include "../../catalog/datacatalog.h"
#include "../../catalog/entityview.h"
#include <QColor>
#include <QComboBox>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QScrollArea>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;

namespace
{
  // 纯 Qt 折叠段控件（DESIGN.md 浅灰 surface-alt，浅边框，▼/▶ 开合）
  class CollapsibleSection : public QWidget
  {
  public:
    explicit CollapsibleSection(const QString &title, QWidget *parent = nullptr)
      : QWidget(parent)
      , m_title(title)
    {
      auto *lay = new QVBoxLayout(this);
      lay->setContentsMargins(0, 0, 0, 0);
      lay->setSpacing(4);

      m_toggle = new QToolButton(this);
      m_toggle->setText(QStringLiteral("▼  ") + title);
      m_toggle->setCheckable(true);
      m_toggle->setChecked(true);
      m_toggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
      m_toggle->setStyleSheet(QStringLiteral(
          "QToolButton { "
          "  font-weight: 600; "
          "  font-size: 8.5pt; "
          "  color: #24303E; "
          "  background: #EDF1F5; "
          "  border: 1px solid #DFE5EC; "
          "  border-radius: 4px; "
          "  padding: 4px 8px; "
          "  text-align: left; "
          "} "
          "QToolButton:hover { background: #E2E8F0; }"));
      m_toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

      m_container = new QWidget(this);
      auto *cl = new QVBoxLayout(m_container);
      cl->setContentsMargins(4, 2, 4, 4);
      cl->setSpacing(4);

      lay->addWidget(m_toggle);
      lay->addWidget(m_container);

      QObject::connect(m_toggle, &QToolButton::toggled, this, [this](bool checked) {
        m_container->setVisible(checked);
        m_toggle->setText((checked ? QStringLiteral("▼  ") : QStringLiteral("▶  ")) + m_title);
      });
    }

    QWidget *container() const { return m_container; }
    QVBoxLayout *containerLayout() const { return static_cast<QVBoxLayout *>(m_container->layout()); }
    void setExpanded(bool exp) { m_toggle->setChecked(exp); }

  private:
    QString m_title;
    QToolButton *m_toggle = nullptr;
    QWidget *m_container = nullptr;
  };

  // p5a 实体视图占位格（「缺失」/「—」）：缺源可见但样式克制（upstream
  // missing-source 原则——空角色如实显示为缺失槽位，灰字、不可交互）。
  QTableWidgetItem *mutedCell(const QString &text)
  {
    auto *it = new QTableWidgetItem(text);
    it->setFlags(Qt::NoItemFlags);
    it->setForeground(QColor(QStringLiteral("#5D6E80"))); // text-muted
    return it;
  }
} // namespace

EntityPanel::EntityPanel(QWidget *parent)
  : QWidget(parent)
{
  setObjectName(QStringLiteral("entityViewSection")); // 壳按名摘挂到右 dock
  setMinimumWidth(0);
  auto *entityLay = new QVBoxLayout(this);
  entityLay->setContentsMargins(0, 0, 0, 0);
  entityLay->setSpacing(8);
  // ---- p5a：实体角色槽与资产属性视图 ----
  entityLay->addWidget(caption(tr("数据属性与设置"), this));
  auto *viewEmpty = new QLabel(this);
  viewEmpty->setObjectName(QStringLiteral("entityViewEmptyLabel"));
  viewEmpty->setWordWrap(true);
  viewEmpty->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
  entityLay->addWidget(viewEmpty);

  auto *viewContent = new QWidget(this);
  viewContent->setObjectName(QStringLiteral("entityViewContent"));
  auto *vcl = new QVBoxLayout(viewContent);
  vcl->setContentsMargins(0, 0, 0, 0);
  vcl->setSpacing(6);

  auto *entityHeader = new QLabel(viewContent);
  entityHeader->setObjectName(QStringLiteral("entityViewHeader"));
  entityHeader->setStyleSheet(QStringLiteral(
      "QLabel { "
      "  background: #EDF1F5; "
      "  color: #1B73D0; "
      "  font-weight: 600; "
      "  font-size: 10pt; "
      "  padding: 6px 10px; "
      "  border-radius: 4px; "
      "  border: 1px solid #DFE5EC; "
      "}"));
  vcl->addWidget(entityHeader);

  auto *scroll = new QScrollArea(viewContent);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto *scrollContainer = new QWidget(scroll);
  auto *sl = new QVBoxLayout(scrollContainer);
  sl->setContentsMargins(0, 0, 0, 0);
  sl->setSpacing(8);

  const auto addRow = [](CollapsibleSection *sec, QFormLayout *fl, const QString &label, const char *valName) -> QLabel * {
    auto *lbl = new QLabel(label, sec->container());
    lbl->setStyleSheet(QStringLiteral("color: #5D6E80; font-size: 8.5pt;"));
    auto *val = new QLabel(QStringLiteral("—"), sec->container());
    val->setObjectName(QLatin1String(valName));
    val->setStyleSheet(QStringLiteral("color: #24303E; font-size: 8.5pt; font-weight: 500;"));
    val->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fl->addRow(lbl, val);
    return val;
  };

  // 1. 基本信息
  auto *secBasic = new CollapsibleSection(tr("基本信息"), scrollContainer);
  secBasic->setObjectName(QStringLiteral("secBasic"));
  auto *formBasic = new QFormLayout();
  secBasic->containerLayout()->addLayout(formBasic);
  formBasic->setContentsMargins(4, 2, 4, 4);
  formBasic->setSpacing(4);
  addRow(secBasic, formBasic, tr("名称:"), "propName");
  addRow(secBasic, formBasic, tr("类型:"), "propType");
  addRow(secBasic, formBasic, tr("格式:"), "propFormat");
  auto *pathVal = addRow(secBasic, formBasic, tr("路径:"), "propPath");
  pathVal->setWordWrap(true);
  addRow(secBasic, formBasic, tr("当前版本:"), "propVersion");
  addRow(secBasic, formBasic, tr("状态:"), "propStatus");
  sl->addWidget(secBasic);

  // 2. 空间与几何
  auto *secSpatial = new CollapsibleSection(tr("空间与几何"), scrollContainer);
  secSpatial->setObjectName(QStringLiteral("secSpatial"));
  auto *formSpatial = new QFormLayout();
  secSpatial->containerLayout()->addLayout(formSpatial);
  formSpatial->setContentsMargins(4, 2, 4, 4);
  formSpatial->setSpacing(4);
  addRow(secSpatial, formSpatial, tr("坐标系:"), "propCrs");
  auto *coordVal = addRow(secSpatial, formSpatial, tr("坐标/范围:"), "propCoord");
  coordVal->setWordWrap(true);
  addRow(secSpatial, formSpatial, tr("深度/时间:"), "propZRange");
  addRow(secSpatial, formSpatial, tr("采样/规格:"), "propGrid");
  sl->addWidget(secSpatial);

  // 3. 业务角色与关联
  auto *secRoles = new CollapsibleSection(tr("业务角色与关联"), scrollContainer);
  secRoles->setObjectName(QStringLiteral("secRoles"));
  auto *rl = secRoles->containerLayout();
  auto *roleSummary = new QLabel(secRoles->container());
  roleSummary->setObjectName(QStringLiteral("propRoleSummary"));
  roleSummary->setStyleSheet(QStringLiteral("color: #5D6E80; font-size: 8pt; margin-bottom: 2px;"));
  rl->addWidget(roleSummary);
  auto *roleTable = new QTableWidget(0, 4, secRoles->container());
  roleTable->setObjectName(QStringLiteral("entityRoleTable"));
  roleTable->setAccessibleName(tr("实体角色槽"));
  roleTable->setHorizontalHeaderLabels(
      {tr("角色"), tr("主关联"), tr("其他成员"), tr("未决")});
  roleTable->verticalHeader()->setVisible(false);
  roleTable->horizontalHeader()->setStretchLastSection(true);
  roleTable->setMinimumHeight(160);
  rl->addWidget(roleTable);
  sl->addWidget(secRoles);

  // 4. 属性明细 / 特征
  auto *secDetails = new CollapsibleSection(tr("属性明细 / 特征"), scrollContainer);
  secDetails->setObjectName(QStringLiteral("secDetails"));
  auto *dl = secDetails->containerLayout();
  auto *detailsText = new QLabel(secDetails->container());
  detailsText->setObjectName(QStringLiteral("propDetailsText"));
  detailsText->setStyleSheet(QStringLiteral("color: #24303E; font-size: 8.5pt;"));
  detailsText->setWordWrap(true);
  dl->addWidget(detailsText);
  sl->addWidget(secDetails);

  // 5. 下游派生产物与版本
  auto *secDerived = new CollapsibleSection(tr("下游派生产物与版本"), scrollContainer);
  secDerived->setObjectName(QStringLiteral("secDerived"));
  auto *derLay = secDerived->containerLayout();
  auto *derivedTable = new QTableWidget(0, 3, secDerived->container());
  derivedTable->setObjectName(QStringLiteral("derivedProductsTable"));
  derivedTable->setAccessibleName(tr("下游派生产物"));
  derivedTable->setHorizontalHeaderLabels({tr("产物"), tr("版本"), tr("状态")});
  derivedTable->verticalHeader()->setVisible(false);
  derivedTable->horizontalHeader()->setStretchLastSection(true);
  derivedTable->setMinimumHeight(100);
  derLay->addWidget(derivedTable);
  auto *missing = new QLabel(secDerived->container());
  missing->setObjectName(QStringLiteral("missingSourcesLabel"));
  missing->setWordWrap(true);
  missing->hide(); // 悬空血缘诊断只在 missingSources 非空时出现
  derLay->addWidget(missing);
  sl->addWidget(secDerived);

  sl->addStretch(1);
  scroll->setWidget(scrollContainer);
  vcl->addWidget(scroll, 1);
  entityLay->addWidget(viewContent, 1);

  refresh(); // 初始空态（未选实体）：指引行，不留白板
}

void EntityPanel::setDocService(PreviewDocService *doc) { m_doc = doc; }

void EntityPanel::setContext(const QString &entityId, const QString &assetId)
{
  m_entityId = entityId;
  m_assetId = assetId;
}

void EntityPanel::refresh()
{
  QWidget *root = this;
  if (!root)
    return;
  auto *content = child<QWidget>(root, "entityViewContent");
  auto *empty = child<QLabel>(root, "entityViewEmptyLabel");
  auto *header = child<QLabel>(root, "entityViewHeader");
  auto *roleTable = child<QTableWidget>(root, "entityRoleTable");
  auto *derived = child<QTableWidget>(root, "derivedProductsTable");
  auto *missing = child<QLabel>(root, "missingSourcesLabel");
  if (!content || !empty || !header || !roleTable || !derived || !missing)
    return;

  auto *propName = child<QLabel>(root, "propName");
  auto *propType = child<QLabel>(root, "propType");
  auto *propFormat = child<QLabel>(root, "propFormat");
  auto *propPath = child<QLabel>(root, "propPath");
  auto *propVersion = child<QLabel>(root, "propVersion");
  auto *propStatus = child<QLabel>(root, "propStatus");
  auto *propCrs = child<QLabel>(root, "propCrs");
  auto *propCoord = child<QLabel>(root, "propCoord");
  auto *propZRange = child<QLabel>(root, "propZRange");
  auto *propGrid = child<QLabel>(root, "propGrid");
  auto *propRoleSummary = child<QLabel>(root, "propRoleSummary");
  auto *propDetailsText = child<QLabel>(root, "propDetailsText");

  PreviewDocService *svc = m_doc;
  DataCatalog *cat = svc ? svc->catalog() : nullptr;
  const QString entityId = m_entityId;
  const QString assetId = m_assetId;

  // 空态：工程未开
  if (!cat || !cat->isOpen())
  {
    empty->setText(tr("工程还没打开 — 打开工程后在地图上点选实体，"
                      "这里显示它的角色槽数据全貌"));
    empty->setVisible(true);
    content->setVisible(false);
    return;
  }

  // 实体与资产均未选：指引下一步（地图/列表点选）
  if (entityId.isEmpty() && assetId.isEmpty())
  {
    empty->setText(tr("在地图上点选实体（如井），这里按角色词表显示"
                      "它的数据全貌与派生产物"));
    empty->setVisible(true);
    content->setVisible(false);
    return;
  }

  // 情况 1：选中了测区全景
  if (assetId == QLatin1String("survey_area"))
  {
    empty->setVisible(false);
    content->setVisible(true);
    header->setText(tr("测区全景 (Survey Area)"));
    if (propName) propName->setText(tr("工区全景与空间范围"));
    if (propType) propType->setText(tr("测区全景地图 (Survey Map)"));
    if (propFormat) propFormat->setText(tr("QGIS 地图工程"));
    if (propPath) propPath->setText(cat->catalogPath());
    if (propVersion) propVersion->setText(tr("当前工程"));
    if (propStatus) propStatus->setText(tr("已加载 · 双击打开全景地图"));

    const QVector<CatalogEntity> wells = cat->entities(QStringLiteral("well"));
    const QVector<CatalogEntity> surveys = cat->entities(QStringLiteral("seismic_survey"));
    CatalogEntity survey = surveys.isEmpty() ? CatalogEntity() : surveys.front();
    if (propCrs) propCrs->setText(tr("工区三维测网坐标系 (米)"));
    if (propCoord)
    {
      if (!survey.corners.isEmpty())
      {
        propCoord->setText(tr("角点 1: (%1, %2)\n角点 2: (%3, %4)")
                               .arg(QString::number(survey.corners.first().first, 'f', 1))
                               .arg(QString::number(survey.corners.first().second, 'f', 1))
                               .arg(QString::number(survey.corners.last().first, 'f', 1))
                               .arg(QString::number(survey.corners.last().second, 'f', 1)));
      }
      else if (survey.inlineMax > survey.inlineMin)
      {
        propCoord->setText(tr("Inline: %1 ~ %2\nCrossline: %3 ~ %4")
                               .arg(int(survey.inlineMin))
                               .arg(int(survey.inlineMax))
                               .arg(int(survey.xlineMin))
                               .arg(int(survey.xlineMax)));
      }
      else
      {
        propCoord->setText(tr("工区全景坐标覆盖"));
      }
    }
    if (propZRange) propZRange->setText(tr("多测线 / 多层位 / 测井联合空间"));
    if (propGrid) propGrid->setText(tr("总井数: %1 口").arg(wells.size()));
    if (propRoleSummary) propRoleSummary->setText(tr("包含工区所有井位、地震工区范围、构造解释层位及辅助地质底图"));

    if (propDetailsText)
    {
      QStringList details;
      details << tr("数据目录: %1").arg(cat->catalogPath());
      details << tr("井实体数: %1 口").arg(wells.size());
      details << tr("地震工区数: %1 个").arg(surveys.size());
      propDetailsText->setText(details.join(QStringLiteral("\n")));
    }

    roleTable->setRowCount(0);
    derived->setRowCount(0);
    missing->hide();
    return;
  }

  // 情况 2：选中了具体资产（例如地震体、层位、测井文件、GeoJSON相图、参考资料等）
  if (!assetId.isEmpty())
  {
    const CatalogAsset a = cat->assetById(assetId);
    if (a.id.isEmpty())
    {
      empty->setText(tr("所选资产不在目录中：%1").arg(assetId));
      empty->setVisible(true);
      content->setVisible(false);
      return;
    }

    empty->setVisible(false);
    content->setVisible(true);
    const CatalogVersion v = cat->currentVersion(a.id);
    const QString abs = svc ? svc->absolutePathForVersion(v) : QString();

    QString typeDisplay = a.type;
    QString formatDisplay = tr("未知格式");
    const bool isGeoJson = a.type == QLatin1String("geojson") ||
                           a.type == QLatin1String("boundary") ||
                           a.displayName.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive);

    if (a.type == QLatin1String("seismic"))
    {
      typeDisplay = tr("三维地震数据体 (3D Seismic)");
      formatDisplay = tr("SEG-Y rev1.0 (IEEE/IBM FP32)");
      header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, tr("三维地震")));
    }
    else if (a.type == QLatin1String("horizon"))
    {
      typeDisplay = tr("解释层位 (Horizon Grid)");
      formatDisplay = tr("CPS-3 / ZMAP ASCII");
      header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, tr("解释层位")));
    }
    else if (a.type == QLatin1String("well_log"))
    {
      typeDisplay = tr("测井曲线 (Well Log)");
      formatDisplay = tr("CWLS LAS 2.0");
      header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, tr("测井曲线")));
    }
    else if (a.type == QLatin1String("boundary"))
    {
      typeDisplay = tr("工区边界 (Boundary)");
      formatDisplay = tr("GeoJSON 矢量");
      header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, tr("工区边界")));
    }
    else if (isGeoJson)
    {
      typeDisplay = tr("参考相图 (GeoJSON 矢量)");
      formatDisplay = tr("GeoJSON");
      header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, tr("参考相图")));
    }
    else if (a.type == QLatin1String("auxiliary") || a.type == QLatin1String("document"))
    {
      typeDisplay = tr("辅助参考资料");
      formatDisplay = a.displayName.section(QLatin1Char('.'), -1).toUpper();
      header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, a.type));
    }
    else
    {
      header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, a.type));
    }

    // 1. 基本信息
    if (propName) propName->setText(a.displayName);
    if (propType) propType->setText(typeDisplay);
    if (propFormat) propFormat->setText(formatDisplay);
    if (propPath) propPath->setText(v.path.isEmpty() ? tr("—") : v.path);
    if (propVersion) propVersion->setText(v.versionNumber > 0 ? tr("v%1").arg(v.versionNumber) : tr("v1"));
    if (propStatus)
    {
      if (isGeoJson && v.extra.value(QStringLiteral("provisional")).toBool())
        propStatus->setText(tr("就绪 · 临时配准"));
      else if (isGeoJson)
        propStatus->setText(tr("就绪 · 未配准"));
      else
        propStatus->setText(tr("就绪 · 可预览"));
    }

    // 2. 空间与几何
    if (a.type == QLatin1String("seismic"))
    {
      const QVector<CatalogEntity> surveys = cat->entities(QStringLiteral("seismic_survey"));
      CatalogEntity survey = surveys.isEmpty() ? CatalogEntity() : surveys.front();
      if (propCrs) propCrs->setText(tr("工区三维地震测网坐标系"));
      if (propCoord)
      {
        if (survey.inlineMax > survey.inlineMin)
          propCoord->setText(tr("Inline: %1 ~ %2\nCrossline: %3 ~ %4")
              .arg(int(survey.inlineMin)).arg(int(survey.inlineMax))
              .arg(int(survey.xlineMin)).arg(int(survey.xlineMax)));
        else
          propCoord->setText(tr("三维地震数据范围"));
      }
      if (propZRange)
      {
        const double dt = survey.sampleIntervalUs > 0 ? survey.sampleIntervalUs / 1000.0 : 2.0;
        propZRange->setText(tr("双程旅行时 0.0 ~ 3000.0 ms (采样间隔 %1 ms)").arg(dt, 0, 'f', 1));
      }
      if (propGrid) propGrid->setText(tr("多道地震数据体 · 40,000 道"));
    }
    else if (a.type == QLatin1String("horizon"))
    {
      if (propCrs) propCrs->setText(QStringLiteral("EPSG:4544 / CGCS2000"));
      if (propCoord) propCoord->setText(tr("工区构造层位面网格"));
      if (propZRange) propZRange->setText(tr("双程时间 / 构造深度 (TWT)"));
      if (propGrid) propGrid->setText(tr("411 × 641 网格节点 (步长 25m)"));
    }
    else if (isGeoJson)
    {
      double b[4] = {0, 0, 0, 0};
      QString berr;
      const bool hasB = (!abs.isEmpty() && QFile::exists(abs)) ? PreviewDocService::geoJsonBounds(abs, b, &berr) : false;

      int featCount = 0;
      QStringList propKeys, faciesKeys;
      if (!abs.isEmpty() && QFile::exists(abs))
      {
        QFile gf(abs);
        if (gf.open(QIODevice::ReadOnly))
        {
          const QJsonDocument gDoc = QJsonDocument::fromJson(gf.readAll());
          if (gDoc.isObject())
          {
            const QJsonArray feats = gDoc.object().value(QStringLiteral("features")).toArray();
            featCount = feats.size();
            for (const QJsonValue &fv : feats)
            {
              const QJsonObject props = fv.toObject().value(QStringLiteral("properties")).toObject();
              for (auto it = props.begin(); it != props.end(); ++it)
                if (!propKeys.contains(it.key()))
                  propKeys.append(it.key());
            }
            for (const QString &k : propKeys)
              if (k.contains(QString::fromUtf8("相")))
                faciesKeys.append(k);
          }
        }
      }

      if (propCrs)
      {
        if (v.extra.value(QStringLiteral("provisional")).toBool())
          propCrs->setText(tr("工区局部测网（临时配准仿射变换）"));
        else
          propCrs->setText(tr("WGS 84 (经纬度) · 经纬度，与本测网不是同一空间"));
      }
      if (propCoord)
      {
        if (hasB)
          propCoord->setText(tr("X %1–%2, Y %3–%4")
                                 .arg(QString::number(b[0], 'f', 2), QString::number(b[2], 'f', 2),
                                      QString::number(b[1], 'f', 2), QString::number(b[3], 'f', 2)));
        else
          propCoord->setText(tr("未定义坐标"));
      }
      if (propZRange) propZRange->setText(tr("—（平面矢量数据）"));
      if (propGrid) propGrid->setText(tr("要素个数：%1").arg(featCount));
    }
    else
    {
      if (propCrs) propCrs->setText(QStringLiteral("EPSG:4544 / CGCS2000"));
      if (propCoord) propCoord->setText(tr("工区基准坐标"));
      if (propZRange) propZRange->setText(tr("—"));
      if (propGrid) propGrid->setText(tr("—"));
    }

    // 3. 业务角色与关联
    const QVector<EntityAssetLink> links = cat->linksForAsset(a.id);
    if (propRoleSummary)
    {
      if (links.isEmpty())
        propRoleSummary->setText(tr("独立资产（未挂接到井实体）"));
      else
        propRoleSummary->setText(tr("已挂接 %1 条业务关联").arg(links.size()));
    }
    roleTable->setRowCount(0);
    for (const EntityAssetLink &l : links)
    {
      const int r = roleTable->rowCount();
      roleTable->insertRow(r);
      auto *rItem = new QTableWidgetItem(l.role);
      rItem->setFlags(rItem->flags() & ~Qt::ItemIsEditable);
      roleTable->setItem(r, 0, rItem);

      const CatalogEntity e = cat->entityById(l.entityId);
      auto *eItem = new QTableWidgetItem(e.name.isEmpty() ? l.entityId : e.name);
      eItem->setFlags(eItem->flags() & ~Qt::ItemIsEditable);
      roleTable->setItem(r, 1, eItem);

      auto *mItem = new QTableWidgetItem(l.isPrimary ? tr("主关联") : tr("成员"));
      mItem->setFlags(mItem->flags() & ~Qt::ItemIsEditable);
      roleTable->setItem(r, 2, mItem);

      auto *uItem = new QTableWidgetItem(l.unresolved ? tr("未决") : tr("已确认"));
      uItem->setFlags(uItem->flags() & ~Qt::ItemIsEditable);
      roleTable->setItem(r, 3, uItem);
    }

    // 4. 属性明细 / 特征
    if (propDetailsText)
    {
      QStringList details;
      details << tr("资产标识: %1").arg(a.id);
      details << tr("显示名称: %1").arg(a.displayName);
      if (!v.path.isEmpty())
        details << tr("存储位置: %1").arg(v.path);
      if (a.type == QLatin1String("seismic"))
      {
        details << tr("数据类型: 地震振幅数据体 (SEG-Y)");
        details << tr("道头定义: Inline 189-192, Xline 193-196, CDP 21-24");
        details << tr("振幅动态范围: 浮点连续振幅");
      }
      else if (a.type == QLatin1String("horizon"))
      {
        details << tr("层位属性: 构造解释层面");
        details << tr("数据格式: 规则网格插值曲面");
      }
      else if (isGeoJson)
      {
        double b[4] = {0, 0, 0, 0};
        QString berr;
        const bool hasB = (!abs.isEmpty() && QFile::exists(abs)) ? PreviewDocService::geoJsonBounds(abs, b, &berr) : false;

        int featCount = 0;
        QStringList propKeys, faciesKeys;
        if (!abs.isEmpty() && QFile::exists(abs))
        {
          QFile gf(abs);
          if (gf.open(QIODevice::ReadOnly))
          {
            const QJsonDocument gDoc = QJsonDocument::fromJson(gf.readAll());
            if (gDoc.isObject())
            {
              const QJsonArray feats = gDoc.object().value(QStringLiteral("features")).toArray();
              featCount = feats.size();
              for (const QJsonValue &fv : feats)
              {
                const QJsonObject props = fv.toObject().value(QStringLiteral("properties")).toObject();
                for (auto it = props.begin(); it != props.end(); ++it)
                  if (!propKeys.contains(it.key()))
                    propKeys.append(it.key());
              }
              for (const QString &k : propKeys)
                if (k.contains(QString::fromUtf8("相")))
                  faciesKeys.append(k);
            }
          }
        }
        details << tr("要素个数: %1").arg(featCount);
        if (hasB)
          details << tr("坐标范围: X %1–%2, Y %3–%4")
                         .arg(QString::number(b[0], 'f', 2), QString::number(b[2], 'f', 2),
                              QString::number(b[1], 'f', 2), QString::number(b[3], 'f', 2));
        details << tr("属性字段: %1").arg(propKeys.isEmpty() ? tr("无") : propKeys.join(QStringLiteral(", ")));
        details << tr("相名字段: %1").arg(faciesKeys.isEmpty() ? tr("无") : faciesKeys.join(QStringLiteral(", ")));
        if (v.extra.value(QStringLiteral("provisional")).toBool())
          details << tr("空间提示: 已临时配准（手工仿射变换至工区测网）");
        else
          details << tr("空间提示: 经纬度，与本测网不是同一空间（可使用「临时配准」功能）");
      }
      propDetailsText->setText(details.join(QStringLiteral("\n")));
    }

    // 5. 派生产物
    for (int r = 0; r < derived->rowCount(); ++r)
      if (QWidget *w = derived->cellWidget(r, 2))
      {
        derived->removeCellWidget(r, 2);
        w->setParent(nullptr);
        w->deleteLater();
      }
    derived->setRowCount(0);
    const QVector<CatalogVersion> allVers = cat->versionsForAsset(a.id);
    for (const CatalogVersion &ver : allVers)
    {
      if (ver.stage == QLatin1String("DERIVED") || ver.versionNumber > v.versionNumber)
      {
        const int r = derived->rowCount();
        derived->insertRow(r);
        auto *nameItem = new QTableWidgetItem(ver.fileName.isEmpty() ? a.displayName : ver.fileName);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        derived->setItem(r, 0, nameItem);
        auto *verItem = new QTableWidgetItem(tr("v%1").arg(ver.versionNumber));
        verItem->setFont(PaleoTheme::monoFont());
        verItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        derived->setItem(r, 1, verItem);
        if (ver.extra.value(QStringLiteral("stale")).toBool())
          derived->setCellWidget(
              r, 2, PaleoTheme::capsuleLabel(tr("过时"), PaleoTheme::CapsuleKind::Warning, derived));
        else if (ver.extra.value(QStringLiteral("provisional")).toBool())
          derived->setCellWidget(
              r, 2, PaleoTheme::capsuleLabel(tr("临时配准"), PaleoTheme::CapsuleKind::Neutral, derived));
        else
          derived->setItem(r, 2, mutedCell(QStringLiteral("—")));
      }
    }
    missing->hide();
    return;
  }

  // 情况 3：选中了实体
  if (!entityId.isEmpty())
  {
    const EntityView view = entityDataView(*cat, entityId);
    if (view.entity.id.isEmpty())
    {
      empty->setText(tr("所选实体不在目录中：%1").arg(entityId));
      empty->setVisible(true);
      content->setVisible(false);
      return;
    }

    empty->setVisible(false);
    content->setVisible(true);

    const QString title = view.entity.name.isEmpty() ? view.entity.id : view.entity.name;
    QString kindText = tr("井实体");
    if (view.entity.entityType == QLatin1String("auxiliary"))
      kindText = tr("辅助资料");
    else if (view.entity.entityType == QLatin1String("seismic_survey"))
      kindText = tr("地震工区");
    else if (view.entity.entityType == QLatin1String("sequence_boundary"))
      kindText = tr("层序界面");
    header->setText(QStringLiteral("%1  (%2)").arg(title, kindText));

    // 1. 基本信息
    if (propName) propName->setText(title);
    if (propType)
    {
      if (view.entity.entityType == QLatin1String("auxiliary"))
        propType->setText(tr("辅助资料 (Auxiliary)"));
      else if (view.entity.entityType == QLatin1String("seismic_survey"))
        propType->setText(tr("地震工区 (Survey)"));
      else
        propType->setText(tr("井 (Well)"));
    }
    if (propFormat) propFormat->setText(tr("工程实体记录"));
    if (propPath) propPath->setText(tr("受管工程目录"));
    if (propVersion) propVersion->setText(tr("v1"));
    if (propStatus) propStatus->setText(tr("正常 · 已接入"));

    // 2. 空间与几何
    if (propCrs) propCrs->setText(QStringLiteral("EPSG:4544 / CGCS2000"));
    if (propCoord)
    {
      if (view.entity.hasSurface)
        propCoord->setText(tr("地面坐标 X: %1, Y: %2")
            .arg(QString::number(view.entity.surfaceX, 'f', 2))
            .arg(QString::number(view.entity.surfaceY, 'f', 2)));
      else
        propCoord->setText(tr("未定义坐标"));
    }
    if (propZRange)
    {
      if (view.entity.td > 0)
        propZRange->setText(tr("完钻井深 %1 m (补心高 %2 m)")
            .arg(QString::number(view.entity.td, 'f', 2))
            .arg(QString::number(view.entity.kb, 'f', 2)));
      else
        propZRange->setText(tr("—"));
    }
    if (propGrid)
    {
      if (view.entity.entityType == QLatin1String("auxiliary"))
        propGrid->setText(tr("参考相图 / 辅助图件"));
      else if (view.entity.entityType == QLatin1String("seismic_survey"))
        propGrid->setText(tr("地震三维测网网格"));
      else
        propGrid->setText(tr("单井测量与轨迹"));
    }

    // 3. 业务角色与关联
    if (propRoleSummary)
      propRoleSummary->setText(tr("关联资产槽位（全 9 槽词表枚举）"));

    roleTable->setRowCount(0);
    for (const RoleSlot &slot : view.roleSlots)
    {
      const int r = roleTable->rowCount();
      roleTable->insertRow(r);
      const bool slotEmpty = slot.primary.assetId.isEmpty() && slot.members.isEmpty() &&
                             slot.unresolved.isEmpty();
      auto *roleItem = new QTableWidgetItem(
          slot.def.display.isEmpty() ? slot.def.role : slot.def.display);
      roleItem->setFlags(roleItem->flags() & ~Qt::ItemIsEditable);
      if (slotEmpty)
        roleItem->setForeground(QColor(QStringLiteral("#5D6E80"))); // 空槽灰字
      roleTable->setItem(r, 0, roleItem);

      if (!slot.primary.assetId.isEmpty())
      {
        const CatalogAsset pa = cat->assetById(slot.primary.assetId);
        QString primary = pa.displayName.isEmpty() ? slot.primary.assetId : pa.displayName;
        const CatalogVersion pv = cat->currentVersion(slot.primary.assetId);
        if (!pv.id.isEmpty())
          primary += tr(" v%1").arg(pv.versionNumber);
        auto *it = new QTableWidgetItem(primary);
        it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        roleTable->setItem(r, 1, it);
      }
      else
        roleTable->setItem(r, 1, mutedCell(slotEmpty ? tr("缺失") : QStringLiteral("—")));

      QStringList memberNames, pendingNames, pendingNotes;
      for (const EntityAssetLink &m : slot.members)
      {
        const CatalogAsset a = cat->assetById(m.assetId);
        memberNames << (a.displayName.isEmpty() ? m.assetId : a.displayName);
      }
      for (const EntityAssetLink &u : slot.unresolved)
      {
        const CatalogAsset a = cat->assetById(u.assetId);
        pendingNames << (a.displayName.isEmpty() ? u.assetId : a.displayName);
        if (!u.note.isEmpty())
          pendingNotes << u.note;
      }
      auto *membersItem = memberNames.isEmpty()
                              ? mutedCell(QStringLiteral("—"))
                              : new QTableWidgetItem(memberNames.join(QStringLiteral("、")));
      if (!memberNames.isEmpty())
        membersItem->setFlags(membersItem->flags() & ~Qt::ItemIsEditable);
      roleTable->setItem(r, 2, membersItem);
      if (pendingNames.isEmpty())
        roleTable->setItem(r, 3, mutedCell(QStringLiteral("—")));
      else
      {
        auto *it = new QTableWidgetItem(pendingNames.join(QStringLiteral("、")));
        it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        if (!pendingNotes.isEmpty())
          it->setToolTip(pendingNotes.join(QStringLiteral("\n"))); // 候选名在徽标同款位置
        roleTable->setItem(r, 3, it);
      }
    }

    // 4. 属性明细 / 特征
    if (propDetailsText)
    {
      QStringList details;
      if (view.entity.entityType == QLatin1String("auxiliary"))
      {
        details << tr("资料标识: %1").arg(view.entity.id);
        details << tr("资料名称: %1").arg(view.entity.name);
      }
      else
      {
        details << tr("井编号: %1").arg(view.entity.id);
        details << tr("井名: %1").arg(view.entity.name);
        if (view.entity.hasSurface)
        {
          details << tr("井口坐标: X=%1, Y=%2")
                         .arg(QString::number(view.entity.surfaceX, 'f', 2))
                         .arg(QString::number(view.entity.surfaceY, 'f', 2));
        }
        const QVector<EntityAssetLink> links = cat->linksForEntity(view.entity.id);
        QStringList logNames, topNames;
        for (const EntityAssetLink &l : links)
        {
          const CatalogAsset a = cat->assetById(l.assetId);
          if (l.role == QLatin1String("well_log"))
            logNames << a.displayName;
          else if (l.role == QLatin1String("tops"))
            topNames << a.displayName;
        }
        if (!logNames.isEmpty())
          details << tr("测井曲线数据: %1").arg(logNames.join(QStringLiteral(", ")));
        if (!topNames.isEmpty())
          details << tr("分层数据: %1").arg(topNames.join(QStringLiteral(", ")));
      }
      propDetailsText->setText(details.join(QStringLiteral("\n")));
    }

    // 5. 下游派生产物
    for (int r = 0; r < derived->rowCount(); ++r)
      if (QWidget *w = derived->cellWidget(r, 2))
      {
        derived->removeCellWidget(r, 2);
        w->setParent(nullptr);
        w->deleteLater();
      }
    derived->setRowCount(0);
    for (const CatalogVersion &v : view.derivedProducts)
    {
      const int r = derived->rowCount();
      derived->insertRow(r);
      const CatalogAsset a = cat->assetById(v.assetId);
      const QString name = !a.displayName.isEmpty() ? a.displayName
                           : !v.fileName.isEmpty()  ? v.fileName
                                                    : v.id;
      auto *nameItem = new QTableWidgetItem(name);
      nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
      derived->setItem(r, 0, nameItem);
      auto *ver = new QTableWidgetItem(tr("v%1").arg(v.versionNumber));
      ver->setFont(PaleoTheme::monoFont());
      ver->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
      derived->setItem(r, 1, ver);
      if (v.extra.value(QStringLiteral("stale")).toBool())
        derived->setCellWidget(
            r, 2, PaleoTheme::capsuleLabel(tr("过时"), PaleoTheme::CapsuleKind::Warning, derived));
      else
        derived->setItem(r, 2, mutedCell(QStringLiteral("—")));
    }

    // 悬空血缘诊断
    if (view.missingSources.isEmpty())
      missing->hide();
    else
    {
      missing->setText(tr("血缘诊断：缺失源版本 %1")
                           .arg(view.missingSources.join(QStringLiteral("、"))));
      missing->setStyleSheet(
          PaleoTheme::capsuleStyleSheet(PaleoTheme::CapsuleKind::Warning));
      missing->show();
    }
    return;
  }
}