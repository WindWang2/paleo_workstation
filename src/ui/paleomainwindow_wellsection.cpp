// 层：视图
// paleomainwindow_wellsection — 连井剖面 dock/面板的壳层编排：workflow
// 取数（连井剖面）+ SeismicTaskService 井间缝 + catalog 变更去抖刷新 +
// 逐工区井集持久化。
#include "paleomainwindow.h"

#include "catalog/datacatalog.h"
#include "linkage/seismicmaplink.h"
#include "seismicsection/seismicsectiondockwidget.h"
#include "qgis/qgisprojectservice.h"
#include "services/previewdoc.h"
#include "services/seismictaskservice.h"
#include "wellsection/wellsectionpanel.h"
#include "workflow/sectionworkbench.h"
#include "workflow/wellsectionworkflow.h"

#include <QSettings>
#include <QTimer>
#include <qgsmessagelog.h>

#include <memory>

namespace {
// 井集按工区持久化：key = wellSection/wells/<catalogPath 清洗>。
QString wellsKey(const QString &catalogPath)
{
  QString k = catalogPath;
  k.replace(QLatin1Char('/'), QLatin1Char('_'));
  k.replace(QLatin1Char('\\'), QLatin1Char('_'));
  return QStringLiteral("wellSection/wells/%1").arg(k);
}
} // namespace

void PaleoMainWindow::attachWellSection(PaleoTaskService *taskSvc)
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

  // #128：结果按 workflow 的当前世代过滤（wf->currentGeneration()），不记
  // request() 的返回值——同步/早退路径在返回前就已 emit，旧实现把那一次
  // 结果当陈旧丢掉（面板卡在忙碌、地震缝状态不显示）。
  auto catalogPath =
      std::make_shared<QString>(m_previewDoc->catalog()->catalogPath());

  // 井集持久化：用户改动（选井/拖排/移除）才写。
  connect(panel, &WellSectionPanel::wellIdsChanged, this,
          [catalogPath](const QStringList &ids) {
            QSettings(QStringLiteral("paleo"), QStringLiteral("paleo"))
                .setValue(wellsKey(*catalogPath), ids);
          });

  connect(panel, &WellSectionPanel::dataRequested, this,
          [wf, panel](const QStringList &ids, const QStringList &mnemonics) {
            panel->setBusy(true);
            wf->request(ids, mnemonics);
          });
  connect(wf, &WellSectionWorkflow::sectionReady, this,
          [wf, panel](int gen, const QVector<wellsection::Well> &wells,
                      const QStringList &warnings) {
            if (gen != wf->currentGeneration())
              return; // 陈旧世代丢弃
            panel->setBusy(false);
            panel->setSection(wells);
            panel->setWarnings(warnings);
            panel->setMnemonicChoices(wf->availableMnemonics(panel->wellIds()));
            for (const QString &w : warnings)
              QgsMessageLog::logMessage(w, QStringLiteral("Paleo"));
          });

  connect(panel, &WellSectionPanel::seismicRequested, this,
          [this, wf, panel] {
            WellSectionWorkflow::SeismicSource src;
            if (m_sectionLink)
            {
              src.volume = m_sectionLink->activeVolume();
              src.grid = m_sectionLink->gridGeometry();
            }
            src.timeOriginMs = m_seismicSectionDock
                                   ? m_seismicSectionDock->timeOriginMs()
                                   : 0.0;
            wf->requestSeismic(panel->wells(), src);
          });
  connect(wf, &WellSectionWorkflow::seismicReady, this,
          [wf, panel](int gen, const wellsection::SeismicStrip &strip) {
            if (gen != wf->currentSeismicGeneration())
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

  // catalog 变更：300ms 去抖 → 刷新井选项；换工区 → 按 catalogPath 恢复
  // 井集；同工区数据变化 → 有井即重取。
  auto *debounce = new QTimer(this);
  debounce->setSingleShot(true);
  debounce->setInterval(300);
  const auto restoreWells = [wf, panel, catalogPath]() {
    QStringList saved =
        QSettings(QStringLiteral("paleo"), QStringLiteral("paleo"))
            .value(wellsKey(*catalogPath))
            .toStringList();
    QSet<QString> existing;
    for (const auto &c : wf->wellChoices())
      existing.insert(c.id);
    QStringList ids;
    for (const QString &id : saved)
      if (existing.contains(id))
        ids << id;
    if (ids.isEmpty())
      return; // 空恢复不触发空请求/忙碌闪动
    panel->setWellIds(ids);
  };
  connect(debounce, &QTimer::timeout, this,
          [this, wf, panel, catalogPath, restoreWells, toChoices] {
            panel->setWellChoices(toChoices(wf->wellChoices()));
            const QString path = m_previewDoc->catalog()->catalogPath();
            if (path != *catalogPath)
            {
              // #124：工区身份变了——先作废在途代并清空面板，再按新工区
              // 恢复；空恢复即空面板（兜底 aboutToClose 之外的 catalog 换绑）。
              *catalogPath = path;
              wf->cancel();
              panel->setBusy(false);
              panel->setWarnings({});
              panel->setSection({});
              restoreWells();
            }
            else if (!panel->wellIds().isEmpty())
            {
              panel->setBusy(true);
              wf->request(panel->wellIds(), panel->sectionTemplate().mnemonics());
            }
          });
  connect(m_previewDoc->catalog(), &DataCatalog::changed, debounce,
          qOverload<>(&QTimer::start));

  // #124：工程即将切换——作废在途取数/井间缝（迟到结果按世代丢弃）并清空
  // 面板。新工程的井集由 catalog changed 去抖路径按新 catalogPath 恢复；
  // 恢复为空时面板保持空，不再残留上一工程的井。
  if (m_projectSvc)
    connect(m_projectSvc, &QgisProjectService::projectAboutToClose, panel, [wf, panel] {
      wf->cancel();
      panel->setBusy(false);
      panel->setWarnings({});
      panel->setSection({});
    });

  // 初始装配同恢复路径。
  panel->setWellChoices(toChoices(wf->wellChoices()));
  restoreWells();
}
