// 层：视图
#include "datapreviewtabs.h"

#include "../paleotheme.h" // DESIGN.md token 出口（颜色/字阶/活体样式共用）
#include "../paleoicons.h" // 角落最大化/还原自绘图标

#include "../../catalog/datacatalog.h"
#include "../../domain/seismic/nicestep.h"
#include "../../domain/wellrecords.h"     // WellTopRecord/TimeDepthTable（domain 纯数据）
#include "../../domain/sectiontrace.h"    // SegyTrace/SegySectionGrid（domain 纯数据）
#include "../../io/lasdoc.h"              // LasCurve（白名单：数据模型）
#include "../../services/previewdoc.h"    // 唯一数据门面——解析/解码/SHA/PDF 编排全经它（W1）
#include "../../services/sectiondoc.h"   // SectionDoc 完整定义（方向 59 拆细头；onSectionReady 触碰成员）
#include "../../services/welllogset.h"    // 井曲线并集（综合柱状图；只读 ~C 头）
#include "../../services/paleotaskservice.h" // PaleoTask 进度/取消（地震转码区）
#include "../seismic3d/seismic3dviewpanel.h"
#include "../seismicsection/seismicsectioncanvas.h"
#include "../wellcomposite/wellcompositepanel.h"

#include "../decorations/paleodecorations.h"
#include "previewhistogramwidget.h"
#include "previewmappage.h"
#include "previewmapstates.h"
#include "previewprofilepanel.h"
#include "previewtocpanel.h"
#include "../../qgis/factorcontour.h"
#include "../../qgis/previewrasteranalysis.h"
#include "../../qgis/projectmapreference.h"
#include <qgsmapcanvas.h>
#include <qgslayertreemapcanvasbridge.h>
#include <qgslayertree.h>
#include <qgsmaptoolpan.h>
#include <qgsproject.h>
#include <qgsrasterbandstats.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterlayer.h>
#include <qgsrastershader.h>
#include <qgscolorrampshader.h>
#include <qgscolorrampimpl.h>
#include <qgssinglebandpseudocolorrenderer.h>
#include <qgsrubberband.h>
#include <qgsexpression.h>
#include <qgsgeometry.h>
#include <qgsvectorlayer.h>
#include <qgsfields.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgssinglesymbolrenderer.h>
#include <qgssymbol.h>
#include <qgsfillsymbol.h>
#include <qgsmarkersymbol.h>
#include <qgsmarkersymbollayer.h>
#include <qgslinesymbol.h>
#include <qgspallabeling.h>
#include <qgsvectorlayerlabeling.h>
#include <qgstextbuffersettings.h>
#include <QButtonGroup>
#include <QTimer>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFileInfo>
#include <QHeaderView>
#include <QDesktopServices>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QToolButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// §4 状态文案与九类资产面板。DESIGN.md dock 面板 tokens：
// surface #FFFFFF、border #DFE5EC、text #24303E、text-muted #5D6E80、8pt captions；
// 数值列 JetBrains Mono 9pt 右对齐；语义色 #F29900(警告)/#E53935(失败)。
// ---------------------------------------------------------------------------
#include "datapreviewtabs_internal.h"
// 匿名命名空间已抽成内部头（方向20 轮4）——用 using 引入，避免给 300+ 处
// 调用点逐个加限定（那会把纯机械改动铺满 diff，掩盖真正的结构变更）。
using namespace paleo::datapreview_detail;

// T27 中文化：coordinate_status 枚举 → §4 计划文案。untransformed 用与
// 状态栏/PDF 页脚同一句「工程坐标 · 米 · 未投影」；invalid/missing 用
// 「坐标无效」「没有坐标」，仍 text-muted（#5D6E80）。
QString DataPreviewTabs::coordinateStatusText(const QString &status)
{
  if (status == QLatin1String("ok"))
    return tr("坐标有效");
  if (status == QLatin1String("untransformed"))
    return tr("工程坐标 · 米 · 未投影");
  if (status == QLatin1String("invalid"))
    return tr("坐标无效");
  return tr("没有坐标"); // missing / 空 / 未知
}

void DataPreviewTabs::setHorizonOnMap(const QString &layerId, bool on)
{
  // T29 双向同步：所有绑到该 layerId 的「在地图上显示」按钮跟随图层可见性。
  auto buttons = findChildren<QPushButton *>(QStringLiteral("showOnMapBtn"));
  if (m_detailsHost)
    buttons.append(m_detailsHost->findChildren<QPushButton *>(QStringLiteral("showOnMapBtn")));
  for (QPushButton *btn : buttons)
    if (btn->property("layerId").toString() == layerId)
    {
      btn->setProperty("onMap", on);
      btn->setText(on ? tr("已在地图上") : tr("在地图上显示"));
    }
}

DataPreviewTabs::DataPreviewTabs(QWidget *parent)
  : QWidget(parent)
{
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(PaleoTheme::tokens().spacingXs);

  m_tabs = new QTabWidget(this);
  m_tabs->setObjectName(QStringLiteral("dataPreviewTabs"));
  m_tabs->setTabsClosable(true);
  m_tabs->setUsesScrollButtons(true); // T32：标签超宽滚动，不挤压
  m_tabs->setAccessibleName(tr("预览"));
  // dock 面板样式（DESIGN.md）：无工作流蓝下划线，安静边框；活体跟随主题。
  PaleoTheme::applyThemedStyleSheet(m_tabs, [] {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
        "QTabWidget::pane { border: 1px solid %1; background: %2; top: -1px; }"
        "QTabBar::tab { padding: {spacing.xs}px {spacing.md}px; color: %3; border: 1px solid %1;"
        " border-bottom: none; background: %2; }"
        "QTabBar::tab:selected { color: %4; font-weight: 600; }"))
        .arg(qssHex(t.border), qssHex(t.surface), qssHex(t.textMuted), qssHex(t.text));
  });
  connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
    const QString assetId = assetIdAt(index);
    if (!assetId.isEmpty())
      closeAssetTab(assetId);
  });
  connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
    if (index >= 0)
      focusWellIfNeeded(assetIdAt(index), m_tabs->widget(index));
    syncDetails();
  });
  // D7 最大化 affordance：右上角 checkable 钮，切换时只发意图信号——实际
  // 分栏尺寸由 shell 决定。空态时 tabs 隐藏，按钮随之隐藏。
  auto *maxBtn = new QToolButton(m_tabs);
  maxBtn->setObjectName(QStringLiteral("previewMaxButton"));
  maxBtn->setCheckable(true);
  maxBtn->setText(tr("最大化预览"));
  maxBtn->setAccessibleName(tr("最大化预览"));
  maxBtn->setToolTip(tr("暂时收起数据列表和属性面板，让预览占满工作区"));
  // QGIS 主题没有最大化/还原语义——PaleoIcons 自绘，随勾选态切换。
  maxBtn->setIcon(PaleoIcons::maximize());
  maxBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  connect(maxBtn, &QToolButton::toggled, this, [this, maxBtn](bool on) {
    maxBtn->setText(on ? tr("还原预览") : tr("最大化预览"));
    maxBtn->setIcon(on ? PaleoIcons::restore() : PaleoIcons::maximize());
    maxBtn->setToolTip(on ? tr("恢复最大化前的面板布局") : tr("暂时收起数据列表和属性面板，让预览占满工作区"));
    emit previewMaximizeToggled(on);
  });
  m_tabs->setCornerWidget(maxBtn, Qt::TopRightCorner);
  lay->addWidget(m_tabs);

  m_emptyLabel = stateLabel(tr("还没有打开的预览 — 从顶部导入数据，再在左侧列表选择一条数据"), this);
  m_emptyLabel->setObjectName(QStringLiteral("previewEmptyLabel"));
  lay->addWidget(m_emptyLabel);
  m_tabs->setVisible(false);
}

DataPreviewTabs::~DataPreviewTabs() = default;

void DataPreviewTabs::setDetailsHost(QWidget *host)
{
  m_detailsHost = host;
  syncDetails();
}

void DataPreviewTabs::clearDetails(const QString &assetId)
{
  if (auto old = m_detailsOfAsset.take(assetId)) {
    old->hide();
    old->setParent(nullptr);
    old->deleteLater();
  }
  syncDetails();
}

void DataPreviewTabs::syncDetails()
{
  const QString active = assetIdAt(m_tabs->currentIndex());
  bool any = false;
  for (auto it = m_detailsOfAsset.cbegin(); it != m_detailsOfAsset.cend(); ++it)
    if (it.value()) {
      const bool show = it.key() == active;
      it.value()->setVisible(show);
      any |= show;
    }
  if (m_detailsHost)
    m_detailsHost->setVisible(any);
}


void DataPreviewTabs::setImportService(DataImportService *svc)
{
  // 自建门面（测试/小环境）；壳共享实例经 setDocService。
  m_docOwned.reset(svc ? new PreviewDocService(svc) : nullptr);
  attachDoc(m_docOwned.get());
}

void DataPreviewTabs::setDocService(PreviewDocService *doc)
{
  m_docOwned.reset();
  attachDoc(doc);
}

void DataPreviewTabs::attachDoc(PreviewDocService *doc)
{
  if (m_doc)
    disconnect(m_doc, nullptr, this, nullptr);
  if (m_catalogForTitles)
    disconnect(m_catalogForTitles, nullptr, this, nullptr);
  m_catalogForTitles = nullptr;
  m_doc = doc;
  if (!m_doc)
    return;
  if (m_taskSvc)
    m_doc->setTaskService(m_taskSvc); // 接线顺序无关：后到的服务补进门面
  // 测线解码结果（D1/T23）：陈旧结果已在服务内按世代号丢弃。
  connect(m_doc, &PreviewDocService::seismicSectionReady, this,
          &DataPreviewTabs::onSectionReady);
  connect(m_doc, &PreviewDocService::seismicSectionFailed, this,
          &DataPreviewTabs::onSectionFailed);
  connect(m_doc, &PreviewDocService::seismicSectionCancelled, this,
          [this](const QString &assetId) {
            onSectionFailed(assetId, tr("已取消"));
          });
  // F1（goal/perf-systematize 簇2）：LAS 数据行异步填充结果（key=assetId；
  // 陈旧结果已在服务内按世代号丢弃）。
  connect(m_doc, &PreviewDocService::lasReady, this,
          &DataPreviewTabs::onLasReady);
  connect(m_doc, &PreviewDocService::lasFailed, this,
          &DataPreviewTabs::onLasFailed);
  connect(m_doc, &PreviewDocService::lasCancelled, this,
          [this](const QString &key) { onLasFailed(key, tr("已取消")); });
  // B 包 staleness-lite：stale 标记可能来自其它标签的 sha 复验或上游版本
  // 取代——catalog 任一变更后重算已开标签的「过时」徽标（GUI 线程直连，
  // 不必重建标签）。换绑服务时先断旧 catalog（上面已断）。
  m_catalogForTitles = m_doc->catalog();
  if (m_catalogForTitles)
    connect(m_catalogForTitles, &DataCatalog::changed, this, [this]() {
      for (auto it = m_pageOfAsset.constBegin(); it != m_pageOfAsset.constEnd(); ++it)
        updateTabTitle(it.key());
    });
}

void DataPreviewTabs::setTaskService(PaleoTaskService *svc)
{
  m_taskSvc = svc;
  if (m_doc)
    m_doc->setTaskService(svc);
}

void DataPreviewTabs::setProject(QgsProject *project)
{
  m_project = project;
}

int DataPreviewTabs::tabCount() const
{
  return m_tabs->count();
}

QString DataPreviewTabs::assetIdAt(int index) const
{
  QWidget *w = m_tabs->widget(index);
  if (!w)
    return QString();
  for (auto it = m_pageOfAsset.constBegin(); it != m_pageOfAsset.constEnd(); ++it)
    if (it.value() == w)
      return it.key();
  return QString();
}

void DataPreviewTabs::closeAssetTab(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return;
  const int idx = m_tabs->indexOf(page);
  if (idx >= 0)
    m_tabs->removeTab(idx);
  m_pageOfAsset.remove(assetId);
  clearDetails(assetId);
  m_wellEntityOfAsset.remove(assetId);
  m_titleSuffixOfAsset.remove(assetId);
  m_chosenVersionOfAsset.remove(assetId);
  // D1：标签关掉即释放该资产的索引缓存（持有文件句柄级状态）与世代号；
  // 进行中的解码任务请求取消——结果没人等了。
  if (m_doc)
  {
    m_doc->releaseSection(assetId);
    m_doc->releaseLas(assetId); // F1：同口径释放 LAS 解析世代号/取消在途
  }
  m_pendingSection.remove(assetId);
  m_pendingLas.remove(assetId);
  page->setParent(nullptr); // 摘出子树再推迟删除，关闭后 findChild 不再命中
  page->deleteLater();
  if (m_tabs->count() == 0)
  {
    m_tabs->setVisible(false);
    m_emptyLabel->setVisible(true);
  }
}

void DataPreviewTabs::closeAllTabs()
{
  const QStringList ids = m_pageOfAsset.keys();
  for (const QString &id : ids)
    closeAssetTab(id);
  // 不在 m_pageOfAsset 里登记的页（理论上没有）一并摘掉，确保空态。
  while (m_tabs->count() > 0)
  {
    QWidget *w = m_tabs->widget(0);
    m_tabs->removeTab(0);
    if (w)
    {
      w->setParent(nullptr);
      w->deleteLater();
    }
  }
  m_pendingSection.clear();
  m_pendingLas.clear();
  m_tiledCanvas.clear();
  m_tiledSample = -1;
  m_tabs->setVisible(false);
  m_emptyLabel->setVisible(true);
}

bool DataPreviewTabs::isMissingSourceState(const QString &assetId) const
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return false;
  auto *lbl = page->findChild<QLabel *>(QStringLiteral("stateText"));
  return lbl && lbl->text().contains(tr("找不到源文件"));
}

bool DataPreviewTabs::relocateMissingSourceWith(const QString &assetId,
                                                const QString &versionId,
                                                const QString &pickedPath)
{
  // wave4：把死胡同接到 relocateVersionSource——内容一致才重接（服务层拒解
  // SHA 不一致的候选文件，不静默换源）。失败保留「找不到源文件」状态与按钮，
  // 错误就地可见，可换文件再试；成功清掉本会话的 SHA 已验缓存（新路径要在
  // 重建时重新过 §3 校验门）并重建标签加载真预览。
  if (!m_doc || assetId.isEmpty())
    return false;
  QString err;
  const QString newVer = m_doc->relocateVersionSource(versionId, pickedPath, &err);
  if (newVer.isEmpty())
  {
    QWidget *page = m_pageOfAsset.value(assetId);
    if (auto *lbl = page ? page->findChild<QLabel *>(QStringLiteral("stateText")) : nullptr)
      lbl->setText(tr("找不到源文件\n重新定位失败：%1").arg(err));
    return false;
  }
  if (m_doc)
    m_doc->resetSha(assetId);
  rebuildAssetTab(assetId);
  return true;
}

QLabel *DataPreviewTabs::loadingLabel(const QString &fileName, QWidget *parent)
{
  // §4 读取中态：「正在读取」+文件名。读取仍是同步的——标签先就位并立即
  // 重绘，文件读完后隐藏（钩子存在，但不引入线程）。
  auto *l = stateLabel(tr("正在读取\n%1").arg(fileName), parent);
  l->setObjectName(QStringLiteral("loadingText"));
  return l;
}

QWidget *DataPreviewTabs::failureState(const QString &assetId, const QString &reason,
                                       QWidget *parent)
{
  // §4 失败态：「读取失败」+原因+文件名+「重试」。重试 = 重建该标签。
  const QString name =
      m_doc ? m_doc->catalog()->assetById(assetId).displayName : assetId;
  auto *box = new QWidget(parent);
  auto *l = new QVBoxLayout(box);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(PaleoTheme::tokens().spacingXs);
  l->addStretch(1);
  l->addWidget(stateLabel(tr("读取失败\n%1\n%2").arg(reason, name), box, true));
  auto *btn = new QPushButton(tr("重试"), box);
  btn->setObjectName(QStringLiteral("retryBtn"));
  connect(btn, &QPushButton::clicked, box,
          [this, assetId] { rebuildAssetTab(assetId); });
  l->addWidget(btn, 0, Qt::AlignHCenter);
  l->addStretch(1);
  return box;
}

void DataPreviewTabs::focusWellIfNeeded(const QString &assetId, QWidget *page)
{
  Q_UNUSED(page);
  if (!m_doc || assetId.isEmpty())
    return;
  // §4：well_head 标签的选中井在地图上高亮。多井标签只报该标签已选中的井
  // ——没有选中就不报，绝不拿第一条链接糊弄（m_wellEntityOfAsset 在
  // 唯一已决井/下拉框选择时写入）。
  const CatalogAsset asset = m_doc->catalog()->assetById(assetId);
  if (asset.type != QLatin1String("well_head"))
    return;
  const QString wellId = m_wellEntityOfAsset.value(assetId);
  if (!wellId.isEmpty())
    emit wellSelected(wellId);
}

void DataPreviewTabs::updateTabTitle(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return;
  const int idx = m_tabs->indexOf(page);
  if (idx < 0)
    return;
  // §4：标题是「文件名 · 井名」/「文件名 · 测线」；无过滤时只有文件名。
  QString title =
      m_doc ? m_doc->catalog()->assetById(assetId).displayName : assetId;
  if (title.isEmpty())
    title = assetId;
  const QString suffix = m_titleSuffixOfAsset.value(assetId);
  if (!suffix.isEmpty())
    title += QStringLiteral(" · ") + suffix;
  // B 包 staleness-lite：实际预览版本被标 stale（上游 sha 失配/被取代）→
  // 标题带「过时」徽标——下游产物过期在数据页如实可见。
  const CatalogVersion shown = !m_doc ? CatalogVersion()
      : m_chosenVersionOfAsset.value(assetId).isEmpty() ? m_doc->catalog()->currentVersion(assetId)
      : m_doc->versionForPreview(m_chosenVersionOfAsset.value(assetId));
  if (shown.extra.value(QStringLiteral("stale")).toBool())
    title += QStringLiteral(" · ") + tr("过时");
  m_tabs->setTabText(idx, title);
}

void DataPreviewTabs::drainDeferredRebuilds()
{
  while (!m_rebuildDeferred.isEmpty())
  {
    const QList<QString> ids = m_rebuildDeferred.values();
    m_rebuildDeferred.clear();
    for (const QString &id : ids)
      rebuildAssetTab(id); // m_buildingContent 已复位——正常重建
  }
}

void DataPreviewTabs::rebuildAssetTab(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page || !m_doc)
    return;
  if (m_buildingContent)
  {
    // 嵌套重建（转换失败信号在 ensure* 内同步发射所致）：外层
    // buildContent 继续走完会看到服务层已写就的最终态，这里只记顺延。
    m_rebuildDeferred.insert(assetId);
    return;
  }
  auto *pageLay = qobject_cast<QVBoxLayout *>(page->layout());
  if (!pageLay)
    return;
  clearDetails(assetId);
  while (QLayoutItem *it = pageLay->takeAt(0))
  {
    if (QWidget *w = it->widget())
    {
      // 信号发送者（如「重试」钮）可能就在被清的子树里——不能就地 delete，
      // 但先摘出父子树，deleteLater 后 findChild 不再碰到陈旧控件。
      w->setParent(nullptr);
      w->deleteLater();
    }
    delete it;
  }
  const QString name = m_doc->catalog()->assetById(assetId).displayName;
  QLabel *loading = loadingLabel(name.isEmpty() ? assetId : name, page);
  pageLay->addWidget(loading, 1);
  loading->repaint(); // 「正在读取」先可见，随后同步读
  m_buildingContent = true;
  QWidget *content = buildContent(assetId, page);
  m_buildingContent = false;
  loading->setVisible(false); // 保留在树里，便于测试/诊断读取中态
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成预览"), page, true), 1);
  updateTabTitle(assetId);
  drainDeferredRebuilds();
}

void DataPreviewTabs::openAsset(const QString &assetId)
{
  if (!m_doc || assetId.isEmpty())
    return;
  if (QWidget *existing = m_pageOfAsset.value(assetId))
  {
    m_tabs->setCurrentIndex(m_tabs->indexOf(existing)); // 重选聚焦（§4）
    focusWellIfNeeded(assetId, existing);
    return;
  }

  const QString displayName = m_doc->catalog()->assetById(assetId).displayName;
  QWidget *page = new QWidget(this);
  auto *pageLay = new QVBoxLayout(page);
  pageLay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);

  QLabel *loading = loadingLabel(displayName.isEmpty() ? assetId : displayName, page);
  pageLay->addWidget(loading, 1);

  const int idx = m_tabs->addTab(page, displayName.isEmpty() ? assetId : displayName);
  m_pageOfAsset.insert(assetId, page);
  m_tabs->setVisible(true);
  m_emptyLabel->setVisible(false);
  m_tabs->setCurrentIndex(idx);
  loading->repaint(); // 「正在读取」+文件名在同步读取前先可见（§4）

  m_buildingContent = true;
  QWidget *content = buildContent(assetId, page);
  m_buildingContent = false;
  loading->setVisible(false); // 读取完成；隐藏但保留节点便于测试断言该状态
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成预览"), page, true), 1);
  updateTabTitle(assetId);
  focusWellIfNeeded(assetId, page);
  drainDeferredRebuilds();
}

QString DataPreviewTabs::versionIdAt(int index) const
{
  QWidget *page = index >= 0 && index < m_tabs->count() ? m_tabs->widget(index) : nullptr;
  return page ? page->property("previewVersionId").toString() : QString();
}

void DataPreviewTabs::openVersion(const QString &versionId)
{
  if (!m_doc) return;
  const CatalogVersion v = m_doc->versionForPreview(versionId);
  if (v.id.isEmpty()) return;
  const bool existing = m_pageOfAsset.contains(v.assetId);
  // 解码与 SHA 留底缓存按资产键控；换版本必须取消旧世代并释放旧句柄。
  if (m_chosenVersionOfAsset.value(v.assetId) != v.id)
  {
    m_doc->releaseSection(v.assetId);
    m_doc->releaseLas(v.assetId);
    m_pendingSection.remove(v.assetId); m_pendingLas.remove(v.assetId);
  }
  m_chosenVersionOfAsset[v.assetId] = v.id;
  // 页签切换时壳不先覆盖图内选中态，最终 versionContextChanged 统一定位。
  setProperty("paleo.versionNavigation", true);
  openAsset(v.assetId);
  if (existing) rebuildAssetTab(v.assetId);
  setProperty("paleo.versionNavigation", false);
  emit versionContextChanged(v.assetId, v.id);
}

void DataPreviewTabs::openAssetForWell(const QString &assetId, const QString &wellId)
{
  if (!m_doc || assetId.isEmpty())
    return;
  if (!wellId.isEmpty())
  {
    m_wellEntityOfAsset[assetId] = wellId;
    if (m_doc->catalog())
      m_titleSuffixOfAsset[assetId] = m_doc->catalog()->entityById(wellId).name;
  }
  openAsset(assetId);
  QWidget *page = m_pageOfAsset.value(assetId);
  if (page)
  {
    if (auto *combo = page->findChild<QComboBox *>(QStringLiteral("wellCombo")))
    {
      const int idx = combo->findData(wellId);
      if (idx >= 0 && combo->currentIndex() != idx)
        combo->setCurrentIndex(idx);
    }
    updateTabTitle(assetId);
    focusWellIfNeeded(assetId, page);
  }
}

void DataPreviewTabs::openSeismicLine(const QString &assetId, const QString &kind,
                                      int line, double timeMs)
{
  openAsset(assetId); // §4 重选语义：已有标签聚焦，否则新开
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return;
  auto *mode = page->findChild<QComboBox *>(QStringLiteral("lineMode"));
  auto *no = page->findChild<QSpinBox *>(QStringLiteral("lineSpin"));
  if (!mode || !no)
    return; // 非地震标签（或地震正文未建出来）——不造假测线控件
  const int want = mode->findData(
      kind == QLatin1String("crossline") ? QStringLiteral("crossline")
                                         : QStringLiteral("inline"));
  if (want >= 0 && mode->currentIndex() != want)
    mode->setCurrentIndex(want); // currentIndexChanged → 该控件链路上的 decode
  if (no->value() != line)
    no->setValue(line); // valueChanged → decode 目标测线
  if (auto *modeTabs = page->findChild<QTabWidget *>(QStringLiteral("seismicSubTabs")))
    modeTabs->setCurrentIndex(0); // 聚焦到二维测线剖面页签
  Q_UNUSED(timeMs); // 目标时间的标注由剖面自身的 D61 标定线承担（§4/阶段B）
}

// ---- 测线解码结果应用（PreviewDocService 信号 → 挂起控件组）----
// 陈旧结果与 SHA 标过时都在服务内做完；这里只把最新一代贴上控件。
void DataPreviewTabs::onSectionReady(const QString &assetId,
                                     const PreviewDocService::SectionDoc &doc)
{
  const SectionPending pend = m_pendingSection.value(assetId);
  if (!pend.panel)
    return;
  // SectionPanel 是本 cpp 内聚的预览控件——挂起时存的是它。
  auto *sp = static_cast<SectionPanel *>(pend.panel.data());
  sp->setTraces(doc.traces, doc.sampleIntervalUs, doc.startTimeMs, doc.readReport.message);
  if (pend.hasTie)
    sp->setTieMarker(pend.tieText, pend.tieMs);
  // 标题后缀：「文件名 · IL1315」/「文件名 · XL4165」（§4）。
  m_titleSuffixOfAsset[assetId] =
      (doc.isInline ? QStringLiteral("IL") : QStringLiteral("XL")) +
      QString::number(doc.lineNo);
  updateTabTitle(assetId);
  if (pend.mode)
  {
    pend.mode->setEnabled(true);
    pend.mode->setToolTip(doc.readReport.message);
  }
  if (pend.spin)
  {
    pend.spin->setEnabled(true);
    pend.spin->setToolTip(doc.readReport.message);
  }
}

void DataPreviewTabs::onSectionFailed(const QString &assetId,
                                      const QString &reason)
{
  const SectionPending pend = m_pendingSection.value(assetId);
  if (auto *sp = static_cast<SectionPanel *>(
          pend.panel ? pend.panel.data() : nullptr))
    sp->setError(reason.isEmpty() ? tr("无法解码测线") : reason); // §4：如实写，不装灰图
  if (pend.mode)
  {
    pend.mode->setEnabled(true);
    pend.mode->setToolTip(QString());
  }
  if (pend.spin)
  {
    pend.spin->setEnabled(true);
    pend.spin->setToolTip(QString());
  }
}
