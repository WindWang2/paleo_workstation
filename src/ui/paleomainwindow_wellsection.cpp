// 层：视图
// paleomainwindow_wellsection — 连井剖面 dock/面板的壳层编排：workflow
// 取数（连井剖面）+ SeismicTaskService 井间缝 + catalog 变更去抖刷新 +
// 逐工区井集持久化。
#include "paleomainwindow.h"

#include "catalog/datacatalog.h"
#include "domain/faultset.h"
#include "domain/wellsection.h"
#include "linkage/seismicmaplink.h"
#include "qgis/qgiscanvascontroller.h"
#include "qgis/wellsectionmapband.h"
#include "metadata/faultsetstore.h"
#include "metadata/paleoprojectstore.h"
#include "metadata/wellsectionstore.h"
#include "seismicsection/seismicsectiondockwidget.h"
#include "services/previewdoc.h"
#include "services/seismictaskservice.h"
#include "wellsection/fencewidget.h"
#include "wellsection/wellsectionpanel.h"
#include "workflow/sectionworkbench.h"
#include "workflow/wellsectionworkflow.h"

#include <QTimer>
#include <qgsmessagelog.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

#include <memory>

namespace {
// 连井剖面编辑产物在 project.sqlite 的节 id（单剖面 dock）。
constexpr char kSectionId[] = "default";

QVector<metadata::WellSectionLinkOverride>
toStoreOverrides(const QVector<wellsection::LinkOverride> &in) {
  QVector<metadata::WellSectionLinkOverride> out;
  out.reserve(in.size());
  for (const wellsection::LinkOverride &o : in)
    out.push_back({o.leftWellId, o.rightWellId, o.topName, o.connected});
  return out;
}

QVector<wellsection::LinkOverride>
fromStoreOverrides(const QVector<metadata::WellSectionLinkOverride> &in) {
  QVector<wellsection::LinkOverride> out;
  out.reserve(in.size());
  for (const metadata::WellSectionLinkOverride &o : in)
    out.push_back({o.leftWellId, o.rightWellId, o.topName, o.connected});
  return out;
}
} // namespace

void PaleoMainWindow::attachWellSection(PaleoTaskService *taskSvc,
                                        PaleoProjectStore *store)
{
  if (!m_wellSectionPanel || !m_previewDoc || !m_previewDoc->catalog())
    return;
  auto *panel = m_wellSectionPanel;
  auto *wf = new WellSectionWorkflow(m_previewDoc->catalog(), this);
  m_wellSectionWf = wf;
  wf->setTaskService(taskSvc);
  if (m_seismicTaskSvc)
    wf->setSeismicTaskService(m_seismicTaskSvc.get());
  wf->setSectionWorkbench(m_sectionWorkbench);

  // 剖面编辑产物落库（井序 + 连线改接；每次落盘版本 +1——Oracle #2）。
  // 无工程库（store 空/未开工程）时编辑仅驻内存。
  WellSectionMapBand *band = nullptr; // 前置声明给 bindStores 用（见下）
  const auto rebindFenceStore = [this]() {
    // 换工程：打开中的栅状图换到新 store（否则继续写旧工程的库）。
    if (m_wellSectionFence)
      m_wellSectionFence->setStore(m_wellSectionStore);
  };
  const auto bindStores = [this, wf, panel, store, &band, rebindFenceStore] {
    if (!store)
      return;
    delete m_wellSectionStore; // 换工程释放旧实例（非 QObject，无父子回收）
    m_wellSectionStore =
        new metadata::WellSectionStore(store->metaDbPath(), store);
    QString storeErr;
    if (!m_wellSectionStore->open(&storeErr))
      QgsMessageLog::logMessage(
          tr("连井剖面编辑库打开失败：%1").arg(storeErr),
          QStringLiteral("Paleo"));
    // 断层投绘供数：断面 mesh ∩ 井径 curtain（换工程重绑）。
    delete m_wellSectionFaultStore;
    m_wellSectionFaultStore =
        new FaultSetStore(store->metaDbPath(), store);
    if (!m_wellSectionFaultStore->open(&storeErr))
      QgsMessageLog::logMessage(
          tr("断层解释库打开失败：%1").arg(storeErr),
          QStringLiteral("Paleo"));
    wf->setFaultSetStore(m_wellSectionFaultStore);
    paleo::fault::FaultSet probe;
    bool has = m_wellSectionFaultStore->load(probe, &storeErr) &&
               probe.faultCount() > 0;
    panel->setFaultsAvailable(has,
                              has ? QString() : tr("工程内无断层解释"));
    // 井位层随工程重建——重解析（首启/换工程后联动不失效）。
    if (band)
      band->setWellLayer(
          qobject_cast<QgsVectorLayer *>(
              QgsProject::instance()->mapLayer(QStringLiteral("wells"))),
          QStringLiteral("id"));
    rebindFenceStore();
  };
  bindStores();
  connect(panel, &WellSectionPanel::faultsRequested, this,
          [wf, panel, this] {
            // 每次请求现读 store（最近落盘的断层解释即时可见）。
            const WellSectionWorkflow::FaultProjection fp =
                wf->faultProjection(panel->wells());
            panel->setFaultTraces(fp.traces, fp.status);
          });

  auto lastGen = std::make_shared<int>(0);
  auto lastSeismicGen = std::make_shared<int>(0);
  auto catalogPath =
      std::make_shared<QString>(m_previewDoc->catalog()->catalogPath());

  // 井集/连线改接持久化：用户改动才写（程序化恢复不发这两个信号）。
  const auto saveSectionEdits = [this, panel] {
    if (!m_wellSectionStore)
      return;
    QString err;
    const metadata::WellSectionRecord rec =
        m_wellSectionStore->save(QString::fromLatin1(kSectionId), panel->wellIds(),
                                 toStoreOverrides(panel->linkOverrides()),
                                 &err);
    if (!rec.valid())
      QgsMessageLog::logMessage(tr("连井剖面编辑保存失败：%1").arg(err),
                                QStringLiteral("Paleo"));
  };
  connect(panel, &WellSectionPanel::wellIdsChanged, this,
          [saveSectionEdits](const QStringList &) { saveSectionEdits(); });
  connect(panel, &WellSectionPanel::linkOverridesChanged, this,
          [saveSectionEdits](const QVector<wellsection::LinkOverride> &) {
            saveSectionEdits();
          });

  connect(panel, &WellSectionPanel::dataRequested, this,
          [this, wf, panel, lastGen](const QStringList &ids,
                                     const QStringList &mnemonics) {
            panel->setBusy(true);
            *lastGen = wf->request(ids, mnemonics);
          });
  connect(wf, &WellSectionWorkflow::sectionReady, this,
          [this, wf, panel, lastGen](int gen,
                                     const QVector<wellsection::Well> &wells,
                                     const QStringList &warnings) {
            if (gen != *lastGen)
              return; // 陈旧世代丢弃
            panel->setBusy(false);
            panel->setSection(wells);
            panel->setWarnings(warnings);
            panel->setMnemonicChoices(wf->availableMnemonics(panel->wellIds()));
            for (const QString &w : warnings)
              QgsMessageLog::logMessage(w, QStringLiteral("Paleo"));
          });

  connect(panel, &WellSectionPanel::seismicRequested, this,
          [this, wf, panel, lastSeismicGen] {
            WellSectionWorkflow::SeismicSource src;
            if (m_sectionLink)
            {
              src.volume = m_sectionLink->activeVolume();
              src.grid = m_sectionLink->gridGeometry();
            }
            src.timeOriginMs = m_seismicSectionDock
                                   ? m_seismicSectionDock->timeOriginMs()
                                   : 0.0;
            *lastSeismicGen = wf->requestSeismic(panel->wells(), src);
          });
  connect(wf, &WellSectionWorkflow::seismicReady, this,
          [panel, lastSeismicGen](int gen,
                                  const wellsection::SeismicStrip &strip) {
            if (gen != *lastSeismicGen)
              return;
            panel->setSeismicStrip(strip);
          });

  // 地震可用性：初始一次 + 剖面体变更跟随（禁用按钮带原因，§35）。
  const auto syncSeismic = [this, panel] {
    const bool ok = m_sectionLink && m_sectionLink->activeVolume() &&
                    m_sectionLink->gridGeometry().valid;
    panel->setSeismicAvailable(
        ok, tr("先在「数据管理」导入带坐标的地震体"));
  };
  syncSeismic();
  if (m_sectionLink)
    connect(m_sectionLink, &SeismicMapLink::sectionVolumeChanged, panel,
            [syncSeismic](std::shared_ptr<const seismic::SgyVolume>) {
              syncSeismic();
            });

  const auto toChoices = [](const QVector<WellSectionWorkflow::WellChoice> &in) {
    QVector<WellSectionPanel::WellChoice> out;
    out.reserve(in.size());
    for (const auto &c : in)
      out.push_back({c.id, c.name, c.hasCoordinates, c.x, c.y});
    return out;
  };

  // catalog 变更：300ms 去抖 → 刷新井选项；换工区 → 重绑编辑库并按
  // 落库行恢复井序/连线改接；同工区数据变化 → 有井即重取。
  auto *debounce = new QTimer(this);
  debounce->setSingleShot(true);
  debounce->setInterval(300);
  const auto restoreWells = [this, wf, panel, store, bindStores]() {
    QStringList saved;
    QVector<wellsection::LinkOverride> overrides;
    if (store)
    {
      bindStores(); // 换工程：编辑库 + 断层库一并重绑（旧实例由 bind 释放）
      QString err;
      const metadata::WellSectionRecord rec =
          m_wellSectionStore->load(QString::fromLatin1(kSectionId), &err);
      saved = rec.wellIds;
      overrides = fromStoreOverrides(rec.linkOverrides);
    }
    QSet<QString> existing;
    for (const auto &c : wf->wellChoices())
      existing.insert(c.id);
    QStringList ids;
    for (const QString &id : saved)
      if (existing.contains(id))
        ids << id;
    panel->setLinkOverrides(overrides);
    if (ids.isEmpty())
    {
      // 新工程无存档：旧工程井集滞留显示会与空改接集混合——显式清空
      //（不发数据请求/忙碌闪动）。
      if (!panel->wellIds().isEmpty())
        panel->setSection({});
      return;
    }
    panel->setWellIds(ids);
  };
  connect(debounce, &QTimer::timeout, this,
          [this, wf, panel, catalogPath, lastGen, restoreWells, toChoices] {
            panel->setWellChoices(toChoices(wf->wellChoices()));
            const QString path = m_previewDoc->catalog()->catalogPath();
            if (path != *catalogPath)
            {
              *catalogPath = path;
              restoreWells();
            }
            else if (!panel->wellIds().isEmpty())
            {
              panel->setBusy(true);
              *lastGen = wf->request(panel->wellIds(),
                                     panel->sectionTemplate().mnemonics());
            }
          });
  connect(m_previewDoc->catalog(), &DataCatalog::changed, debounce,
          qOverload<>(&QTimer::start));

  // 剖面-平面联动：剖面线位高亮（井序连线）+ 点名反向闪烁。线位随
  // 井集/井序/取数回填刷新；闪烁经 well 层 fid 解析。
  if (m_canvasCtl)
  {
    band = new WellSectionMapBand(m_canvasCtl->canvas(), this);
    // well 层解析：QgsProject 里 id/name 双查（refreshWellsLayer 声明的
    // layerId = "wells"）。
    band->setWellLayer(
        qobject_cast<QgsVectorLayer *>(
            QgsProject::instance()->mapLayer(QStringLiteral("wells"))),
        QStringLiteral("id"));
  }
  const auto syncBand = [panel, band] {
    if (!band)
      return;
    QVector<QPair<double, double>> path;
    for (const wellsection::Well &w : panel->wells())
      if (w.hasCoordinates())
        path.push_back({w.x, w.y});
    band->setSectionPath(path);
  };
  connect(panel, &WellSectionPanel::wellIdsChanged, this,
          [syncBand](const QStringList &) { syncBand(); });
  connect(panel, &WellSectionPanel::wellClicked, this,
          [band](const QString &id) {
            if (band)
              band->flashWell(id);
          });
  // sectionReady 回填后井位才可读——同步进现有回填槽。
  connect(wf, &WellSectionWorkflow::sectionReady, this,
          [syncBand](int, const QVector<wellsection::Well> &,
                     const QStringList &) { syncBand(); });

  // 栅状图（fence）：面板入口 → 单实例窗（交点井联动在部件内部接线）。
  connect(panel, &WellSectionPanel::fenceRequested, this,
          [this, wf, taskSvc, toChoices] {
            if (m_wellSectionFence)
            {
              m_wellSectionFence->raise();
              m_wellSectionFence->activateWindow();
              return;
            }
            WellSectionFenceWidget::Params fp;
            fp.catalog = m_previewDoc ? m_previewDoc->catalog() : nullptr;
            fp.selection = m_selection;
            fp.tasks = taskSvc;
            fp.store = m_wellSectionStore;
            fp.choices = toChoices(wf->wellChoices());
            auto *fence = new WellSectionFenceWidget(fp, this);
            fence->setAttribute(Qt::WA_DeleteOnClose);
            fence->setWindowFlags(Qt::Window);
            fence->resize(1100, 700);
            m_wellSectionFence = fence;
            connect(fence, &QObject::destroyed, this,
                    [this] { m_wellSectionFence = nullptr; });
            fence->show();
          });

  // 初始装配同恢复路径。
  panel->setWellChoices(toChoices(wf->wellChoices()));
  restoreWells();
}
