// 层：视图
// paleomainwindow_wellsection — 连井剖面 dock/面板的壳层编排：workflow
// 取数（连井剖面）+ SeismicTaskService 井间缝 + catalog 变更去抖刷新 +
// 逐工区井集持久化。
#include "paleomainwindow.h"

#include "catalog/datacatalog.h"
#include "domain/wellsection.h"
#include "linkage/seismicmaplink.h"
#include "metadata/paleoprojectstore.h"
#include "metadata/wellsectionstore.h"
#include "seismicsection/seismicsectiondockwidget.h"
#include "services/previewdoc.h"
#include "services/seismictaskservice.h"
#include "wellsection/wellsectionpanel.h"
#include "workflow/sectionworkbench.h"
#include "workflow/wellsectionworkflow.h"

#include <QTimer>
#include <qgsmessagelog.h>

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
  if (store) {
    m_wellSectionStore =
        new metadata::WellSectionStore(store->metaDbPath(), store);
    QString storeErr;
    if (!m_wellSectionStore->open(&storeErr))
      QgsMessageLog::logMessage(
          tr("连井剖面编辑库打开失败：%1").arg(storeErr),
          QStringLiteral("Paleo"));
  }

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
  const auto restoreWells = [this, wf, panel, store]() {
    QStringList saved;
    QVector<wellsection::LinkOverride> overrides;
    if (m_wellSectionStore && store)
    {
      m_wellSectionStore->rebind(store->metaDbPath(), store);
      QString err;
      m_wellSectionStore->open(&err);
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
      return; // 空恢复不触发空请求/忙碌闪动
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

  // 初始装配同恢复路径。
  panel->setWellChoices(toChoices(wf->wellChoices()));
  restoreWells();
}
