// 层：视图
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"

#include "../../domain/wellrecords.h"     // WellTopRecord/WellHeadRecord/TimeDepthTable
#include "../../qgis/projectmapreference.h" // 底图克隆（well_head 井位预览）
#include <qgsmapcanvas.h>
#include <qgslayertree.h>
#include <qgsproject.h>
#include <QStackedWidget>
#include <QTimer>

// 共享辅助来自内部头——与 datapreviewtabs.cpp 用同一 using 引入（族内先例：
// datapreviewtabworkbook/tabxml 同款）。
using namespace paleo::datapreview_detail;

// buildContent 的 wellFilterable 分支（well_head/well_stratification/time_depth）
// 析出（方向97）：单井直滤/死胡同文案/多井下拉框语义逐字节不变。
QWidget *DataPreviewTabs::buildWellFilteredContent(
    DataCatalog *cat, const CatalogAsset &asset, const QString &abs,
    const QString &assetId, const QVector<QPair<QString, QString>> &wells,
    QWidget *host, QVBoxLayout *lay)
{
  if (wells.size() == 1)
  {
    // 恰好一口已决井：直接按它过滤（§4 autoplan：唯一解析时过滤即它）。
    const QString wellId = wells.front().first;
    m_wellEntityOfAsset[assetId] = wellId;
    m_titleSuffixOfAsset[assetId] = wells.front().second;
    lay->addWidget(buildWellBody(asset, abs, wellId, wells.front().second, host), 1);
    return host;
  }
  // T31 死胡同文案：多井 tab 无井可挂（下拉会是空的）时不留空白页——
  // 工程没井指向导入；资产未决指向数据页「挂到这口井」入口。
  if (wells.isEmpty())
  {
    auto *deadEnd = stateLabel(
        cat->entities(QStringLiteral("well")).isEmpty()
            ? tr("工程里还没有井 — 先导入工区文件夹（井位表会建立井）")
            : tr("该资产尚未关联到任何井。请在数据页资产表的「未决」行，"
                 "用「挂到这口井」完成挂接"),
        host);
    deadEnd->setObjectName(QStringLiteral("deadEndText"));
    lay->addWidget(deadEnd, 1);
    return host;
  }

  // 多井文件（井口表、DC.dat、多井 TD）或未决资产：每标签自带「井」下拉框，
  // 只列已决链接的井；默认未选 → 正文「先选择一口井」。
  auto *bar = new QWidget(host);
  auto *barLay = new QHBoxLayout(bar);
  barLay->setContentsMargins(0, 0, 0, 0);
  barLay->addWidget(caption8(tr("井"), bar));
  auto *combo = new QComboBox(bar);
  combo->setObjectName(QStringLiteral("wellCombo"));
  combo->setAccessibleName(tr("井"));
  for (const auto &w : wells)
    combo->addItem(w.second, w.first);
  combo->setCurrentIndex(-1); // 默认未选（§4）
  barLay->addWidget(combo);
  barLay->addStretch(1);
  lay->addWidget(bar);

  auto *bodyHost = new QWidget(host);
  auto *bodyLay = new QVBoxLayout(bodyHost);
  bodyLay->setContentsMargins(0, 0, 0, 0);
  lay->addWidget(bodyHost, 1);

  const auto applyWell = [this, assetId, asset, abs, cat, combo, bodyLay, bodyHost,
                          host](const QString &wellId) {
    // 本标签自己的选择：不动其他标签（§4）。
    if (wellId.isEmpty())
      m_wellEntityOfAsset.remove(assetId);
    else
      m_wellEntityOfAsset[assetId] = wellId;
    const QString wname =
        wellId.isEmpty() ? QString() : cat->entityById(wellId).name;
    m_titleSuffixOfAsset[assetId] = wname;
    updateTabTitle(assetId);
    while (QLayoutItem *it = bodyLay->takeAt(0))
    {
      if (QWidget *w = it->widget())
        delete w; // 直接删：发送者（下拉框）不在正文子树里，陈旧控件立刻出树
      delete it;
    }
    if (wellId.isEmpty())
      bodyLay->addWidget(stateLabel(tr("先选择一口井"), bodyHost), 1);
    else
    {
      bodyLay->addWidget(buildWellBody(asset, abs, wellId, wname, bodyHost), 1);
      if (asset.type == QLatin1String("well_head"))
        emit wellSelected(wellId); // §4：选中时地图同时高亮该井
    }
  };
  connect(combo, &QComboBox::currentIndexChanged, host,
          [applyWell, combo](int idx) {
            applyWell(idx >= 0 ? combo->itemData(idx).toString() : QString());
          });
  // 重建时恢复本标签之前选中的井；否则保持未选。
  const QString prev = m_wellEntityOfAsset.value(assetId);
  const int prevIdx = prev.isEmpty() ? -1 : combo->findData(prev);
  if (prevIdx >= 0)
    combo->setCurrentIndex(prevIdx); // 触发 applyWell → 正文按该井渲染
  else
    applyWell(QString());
  return host;
}

QWidget *DataPreviewTabs::buildWellBody(const CatalogAsset &asset, const QString &absPath,
                                        const QString &wellEntityId,
                                        const QString &wellName, QWidget *parent)
{
  const QString normWell = DataCatalog::normalizeWellName(wellName);
  const auto matchWell = [&normWell](const QString &rowName) {
    return normWell.isEmpty() ||
           DataCatalog::normalizeWellName(rowName) == normWell;
  };

  if (asset.type == QLatin1String("well_stratification"))
  {
    QVector<WellTopRecord> tops;
    QString werr;
    if (!m_doc->wellTopsAt(absPath, &tops, &werr))
      return failureState(asset.id, werr, parent);
    auto *holder = new QWidget(parent);
    auto *hl = new QVBoxLayout(holder);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(PaleoTheme::tokens().spacingSm);
    // §4：层名、MD、TVD、X、Y；Time 列为空就显示空，不填 -99999，也不填假时间。
    auto *table = new QTableWidget(0, 6, holder);
    table->setObjectName(QStringLiteral("topsTable"));
    table->setHorizontalHeaderLabels(
        {tr("层名"), tr("MD"), tr("TVD"), tr("X"), tr("Y"), tr("Time(ms)")});
    table->verticalHeader()->setVisible(false);
    for (const WellTopRecord &t : tops)
    {
      if (!matchWell(t.wellName))
        continue; // 多井文件按当前井过滤，不拆文件（§3）
      const int r = table->rowCount();
      table->insertRow(r);
      table->setItem(r, 0, new QTableWidgetItem(t.topName));
      auto *md = new QTableWidgetItem(t.hasMd ? QString::number(t.md, 'f', 1) : QString());
      auto *tvd = new QTableWidgetItem(t.hasTvd ? QString::number(t.tvd, 'f', 1) : QString());
      auto *x = new QTableWidgetItem(t.hasX ? QString::number(t.x, 'f', 2) : QString());
      auto *y = new QTableWidgetItem(t.hasY ? QString::number(t.y, 'f', 2) : QString());
      auto *tm = new QTableWidgetItem(t.hasTime ? QString::number(t.timeMs, 'f', 1) : QString());
      for (QTableWidgetItem *it : {md, tvd, x, y, tm})
        setNumericItem(it); // JetBrains Mono 9pt 右对齐（§4/DESIGN.md）
      table->setItem(r, 1, md);
      table->setItem(r, 2, tvd);
      table->setItem(r, 3, x);
      table->setItem(r, 4, y);
      table->setItem(r, 5, tm); // Time 空（-99999）就显示空，不填假时间
    }
    table->horizontalHeader()->setStretchLastSection(true);
    hl->addWidget(caption8(wellName.isEmpty() ? tr("分层表")
                                              : tr("%1 的分层表").arg(wellName),
                           holder));
    hl->addWidget(table, 1);

    // P2 D2.8 井位落图：分层行带坐标（X/Y）时把层位顶点打上图。
    bool anyCoords = false;
    for (const WellTopRecord &t : tops)
      if (matchWell(t.wellName) && t.hasX && t.hasY)
      {
        anyCoords = true;
        break;
      }
    if (anyCoords)
    {
      auto *topsVl = makeMemoryPointLayer(
          wellName.isEmpty() ? tr("分层顶点") : tr("%1 分层顶点").arg(wellName), holder);
      for (const WellTopRecord &t : tops)
        if (matchWell(t.wellName) && t.hasX && t.hasY)
          addMemoryPoint(topsVl, t.x, t.y, t.topName, QStringLiteral("well"));
      stylePointLayer(topsVl, true);
      auto *mapPage = new PreviewMapPage(holder);
      mapPage->setObjectName(QStringLiteral("topsPreviewPage"));
      mapPage->mapCanvas()->canvas()->setObjectName(QStringLiteral("topsMapCanvas"));
      mapPage->setProfileEnabled(false);
      mapPage->addMapLayer(topsVl, tr("分层顶点"), absPath);
      auto *labelRow = new QWidget(holder);
      auto *labelLay = new QHBoxLayout(labelRow);
      labelLay->setContentsMargins(0, 0, 0, 0);
      auto *labelToggle = new QCheckBox(tr("名称标注"), labelRow);
      labelToggle->setObjectName(QStringLiteral("topsLabelToggle"));
      labelToggle->setChecked(true);
      labelLay->addWidget(labelToggle);
      labelLay->addStretch(1);
      QObject::connect(labelToggle, &QCheckBox::toggled, mapPage,
                       [topsVl, mapPage](bool on) {
                         topsVl->setLabelsEnabled(on);
                         mapPage->mapCanvas()->canvas()->refresh();
                       });
      hl->addWidget(labelRow);
      hl->addWidget(mapPage, 1);
      QTimer::singleShot(0, holder, [mapPage, topsVl]() {
        mapPage->mapCanvas()->zoomToLayer(topsVl);
      });
    }
    return holder;
  }

  if (asset.type == QLatin1String("time_depth"))
  {
    TimeDepthTable td;
    QString terr;
    if (!m_doc->timeDepthAt(absPath, &td, &terr))
      return failureState(asset.id, terr, parent);
    QVector<double> tvds, times;
    for (const TdRow &r : td.rows)
    {
      if (!r.hasTvd)
        continue;
      tvds.append(r.tvd);
      times.append(r.timeMs);
    }
    // §4：time_depth 没有可用样点时写「无时深表」，不画假线。
    if (tvds.isEmpty())
      return stateLabel(tr("无时深表"), parent);
    auto *panel = new CurvePanel(parent);
    panel->setEmptyText(tr("无时深表")); // 双保险：NaN 过滤后仍空的兜底文案
    panel->setCurve(tr("TIME–TVD"), QStringLiteral("ms"), times, tvds);
    return panel;
  }

  if (asset.type == QLatin1String("well_head"))
  {
    QVector<WellHeadRecord> rows;
    QString herr;
    if (!m_doc->wellHeadsAt(absPath, &rows, &herr))
      return failureState(asset.id, herr, parent);
    auto *holder = new QWidget(parent);
    auto *hl = new QVBoxLayout(holder);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(PaleoTheme::tokens().spacingSm);
    // 预览按当前井过滤（多井井位文件；井名规范化后比较）
    const WellHeadRecord *rec = nullptr;
    for (const WellHeadRecord &r : rows)
      if (matchWell(r.name))
        rec = &r;
    if (!rec && !wellName.isEmpty())
    {
      hl->addWidget(stateLabel(tr("井 %1 不在该井位文件中").arg(wellName), holder), 1);
      return holder;
    }
    // §4 字段不变（井名/X/Y/KB/TD/BottomX/BottomY/WellType/坐标状态），
    // 收为单行字段条——纵列信息卡占高约十行，压缩后纵向让给地图；
    // 数字仍 JetBrains Mono（DESIGN mono 约定）。
    auto *info = new QWidget(holder);
    auto *grid = new QHBoxLayout(info);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(PaleoTheme::tokens().spacingSm * 2);
    const auto addField = [&](const QString &k, const QString &val, bool mono = false,
                              bool muted = false) {
      grid->addWidget(caption8(k, info));
      auto *v = valueLabel(val, info, mono);
      if (muted)
        PaleoTheme::applyThemedStyleSheet(
            v, [] { return PaleoTheme::mutedCaptionStyleSheet(); }); // text-muted
      grid->addWidget(v);
    };
    if (rec)
    {
      addField(tr("井名"), rec->name);
      addField(tr("X"), QString::number(rec->x, 'f', 2), true);
      addField(tr("Y"), QString::number(rec->y, 'f', 2), true);
      addField(tr("KB"), QString::number(rec->kb, 'f', 2), true);
      addField(tr("TD"), QString::number(rec->td, 'f', 2), true);
      addField(tr("BottomX"),
               rec->hasBottomX ? QString::number(rec->bottomX, 'f', 2) : QString(), true);
      addField(tr("BottomY"),
               rec->hasBottomY ? QString::number(rec->bottomY, 'f', 2) : QString(), true);
      addField(tr("WellType"), rec->wellType);
    }
    const QString status =
        wellEntityId.isEmpty()
            ? QString()
            : m_doc->catalog()->entityById(wellEntityId).coordinateStatus;
    // T27：坐标状态中文化 + text-muted（计划 §4：这些状态仍用 #5D6E80）。
    addField(tr("坐标状态"), coordinateStatusText(status), false, true);
    grid->addStretch(1);
    hl->addWidget(info);

    // P2 D2.6 井位地图预览：全部井位打点 + 当前井高亮 + 名称标注开关。
    auto *wellsVl = makeMemoryPointLayer(tr("井位"), holder);
    for (const WellHeadRecord &r : rows)
      if (r.x > -99990.0 && r.y > -99990.0) // 坐标哨兵过滤
        addMemoryPoint(wellsVl, r.x, r.y, r.name,
                       (rec && matchWell(r.name)) ? QStringLiteral("highlight")
                                                   : QStringLiteral("well"));
    stylePointLayer(wellsVl, true);
    auto *mapPage = new PreviewMapPage(holder);
    mapPage->setObjectName(QStringLiteral("wellHeadPreviewPage"));
    mapPage->mapCanvas()->canvas()->setObjectName(QStringLiteral("wellHeadMapCanvas"));
    mapPage->setProfileEnabled(false);
    QgsMapCanvas *canvas = mapPage->mapCanvas()->canvas();
    // 与测区全景同一口径：有效配准的工程把离线底图克隆进预览画布（独立实例
    // 随标签释放），overrideCrs 让画布跟随工程地图坐标系。
    if (m_project)
    {
      canvas->setProject(m_project);
      canvas->mapSettings().setTransformContext(m_project->transformContext());
      mapPage->mapCanvas()->setOverrideCrs(m_project->crs());
      const auto baseLayers = m_project->layerTreeRoot()->findLayers();
      for (auto it = baseLayers.crbegin(); it != baseLayers.crend(); ++it)
      {
        auto *layer = (*it)->layer();
        if (!layer || !layer->customProperty("paleoBasemap").toBool())
          continue;
        if (auto *base = paleo::mapreference::offlineBasemap(
                layer->customProperty("paleoBasemapPath").toString(), layer->name(),
                canvas))
          mapPage->addMapLayer(base, base->name(), QString());
      }
    }
    // 井位预览是跟随内容缩放的交互画布，不出鹰眼（同测区全景页口径）。
    if (auto *ovAction =
            mapPage->findChild<QAction *>(QStringLiteral("previewOverviewAction")))
    {
      ovAction->setChecked(false); // toggled → setOverviewVisible(false)
      ovAction->setVisible(false);
    }
    else
    {
      mapPage->setOverviewVisible(false);
    }
    mapPage->addMapLayer(wellsVl, tr("井位"), absPath);
    auto *labelRow = new QWidget(holder);
    auto *labelLay = new QHBoxLayout(labelRow);
    labelLay->setContentsMargins(0, 0, 0, 0);
    auto *labelToggle = new QCheckBox(tr("名称标注"), labelRow);
    labelToggle->setObjectName(QStringLiteral("wellLabelToggle"));
    labelToggle->setChecked(true);
    labelLay->addWidget(labelToggle);
    labelLay->addStretch(1);
    // 底图版权方标注 + 高亮提示收进同一行（不占独立行）。
    if (m_project)
    {
      const QString attr = paleo::mapreference::attribution(canvas);
      if (!attr.isEmpty())
      {
        auto *src = caption8(attr, labelRow);
        src->setObjectName(QStringLiteral("wellHeadBasemapAttribution"));
        labelLay->addWidget(src);
      }
    }
    labelLay->addWidget(caption8(tr("选中时地图同时高亮该井"), labelRow));
    QObject::connect(labelToggle, &QCheckBox::toggled, mapPage,
                     [wellsVl, mapPage](bool on) {
                       wellsVl->setLabelsEnabled(on);
                       mapPage->mapCanvas()->canvas()->refresh();
                     });
    hl->addWidget(labelRow);
    hl->addWidget(mapPage, 1);
    QTimer::singleShot(0, holder, [mapPage, wellsVl]() {
      mapPage->mapCanvas()->zoomToLayer(wellsVl);
    });
    return holder;
  }

  return stateLabel(tr("先选择一口井"), parent); // 兜底（不可达）
}

void DataPreviewTabs::onLasReady(const QString &key, const QStringList &, const QList<LasCurve> &curves)
{
  // F1（goal/perf-systematize 簇2）：数据行到达——fill 闭包内自带 QPointer
  // 护栏（页没了就不装）；服务侧世代号已保证这是最新一代。兄弟文件文档
  // 在 lasReady 之前已写入门面，失败的不在表里。
  if (!m_pendingLas.contains(key))
    return;
  const LasPending pend = m_pendingLas.take(key);
  const QHash<QString, LasDoc> siblings =
      m_doc ? m_doc->lasSiblingDocs(key) : QHash<QString, LasDoc>();
  if (pend.fill)
    pend.fill(curves, siblings);
}

void DataPreviewTabs::onLasFailed(const QString &key, const QString &reason)
{
  // 头部能解但整份解析失败（截断/坏行）：页面骨架已建好——把视图栈内容
  // 换成「读取失败」面（带重试=重建标签重走两段式），与其它失败态同一
  // 形态（§4）。呈现切换条保留（重试成功后仍有用）。
  if (!m_pendingLas.contains(key))
    return;
  const LasPending pend = m_pendingLas.take(key);
  if (auto *stack = qobject_cast<QStackedWidget *>(pend.page.data()))
  {
    while (stack->count() > 0)
    {
      QWidget *w = stack->widget(0);
      stack->removeWidget(w);
      w->deleteLater();
    }
    stack->addWidget(failureState(key, reason.isEmpty() ? tr("无法解析 LAS 文件") : reason, stack));
  }
}
