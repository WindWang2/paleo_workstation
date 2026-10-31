// 层：视图
// paleomainwindow_wellsection — 连井剖面 dock/面板的壳层编排：workflow
// 取数（连井剖面）+ SeismicTaskService 井间缝 + catalog 变更去抖刷新 +
// 逐工区井集持久化。
#include "paleomainwindow.h"
#include "windowsizing_internal.h"

#include "catalog/datacatalog.h"
#include "dialogs/depthanchordialog.h"
#include "domain/faultset.h"
#include "domain/wellsection.h"
#include "linkage/seismicmaplink.h"
#include "qgis/qgiscanvascontroller.h"
#include "qgis/wellsectionmapband.h"
#include "metadata/faultsetstore.h"
#include "metadata/paleoprojectstore.h"
#include "metadata/wellsectionstore.h"
#include "seismicsection/seismicsectiondockwidget.h"
#include "qgis/qgisprojectservice.h"
#include "services/previewdoc.h"
#include "services/seismictaskservice.h"
#include "services/fspathutils.h"
#include "wellsection/fencewidget.h"
#include "wellsection/wellsectionpanel.h"
#include "workflow/sectionworkbench.h"
#include "workflow/wellattachmentops.h"
#include "workflow/wellsectionworkflow.h"
#include "../workflow/mappingworkbench.h" // #213：推岩性选择表给测井预测服务

#include <QInputDialog>
#include <QTimer>
#include <qgsmessagelog.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

#include <memory>

#include <cmath>

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

void PaleoMainWindow::pushLithoSelectionsToPrediction()
{
  // #213：预测服务按这张表取岩性；无 workbench/服务（未接编图链）时静默跳过。
  auto *service = m_workbench ? m_workbench->wellPredictionService() : nullptr;
  if (!service)
    return;
  service->setLithoAssetSelections(m_wellSectionWf ? m_wellSectionWf->lithoAssetSelections()
                                                   : QHash<QString, QString>{});
}

void PaleoMainWindow::clearWellSectionStores()
{
  if (m_wellSectionWf) {
    m_wellSectionWf->setFaultSetStore(nullptr);
    m_wellSectionWf->setLithoAssetSelections({});
  }
  pushLithoSelectionsToPrediction(); // 选择表已清空：预测回落默认仲裁
  if (m_wellSectionFence) m_wellSectionFence->setStore(nullptr, nullptr);
  delete m_wellSectionStore; m_wellSectionStore = nullptr;
  delete m_wellSectionFaultStore; m_wellSectionFaultStore = nullptr;
  if (m_wellSectionPanel)
    m_wellSectionPanel->setFaultsAvailable(false, tr("工程内无断层解释"));
}

bool PaleoMainWindow::bindWellSectionStores(PaleoProjectStore *store, QString *boundPath)
{
  if (!store || store->metaDbPath().isEmpty()) {
    clearWellSectionStores(); boundPath->clear(); return false;
  }
  const bool sameProject = m_wellSectionStore && m_wellSectionFaultStore &&
      *boundPath == store->metaDbPath() && m_wellSectionStore->isReadOnly() == store->isReadOnly();
  QString error;
  if (!sameProject) {
    clearWellSectionStores();
    boundPath->clear();
    auto sections = std::make_unique<metadata::WellSectionStore>(store->metaDbPath(), store);
    auto faults = std::make_unique<FaultSetStore>(store->metaDbPath(), store);
    sections->setReadOnly(store->isReadOnly()); faults->setReadOnly(store->isReadOnly());
    if (!sections->open(&error) || !faults->open(&error)) {
      QgsMessageLog::logMessage(tr("连井剖面编辑库打开失败：%1").arg(error), QStringLiteral("Paleo"));
      return false;
    }
    m_wellSectionStore = sections.release();
    m_wellSectionFaultStore = faults.release();
    *boundPath = store->metaDbPath();
  }
  m_wellSectionWf->setFaultSetStore(m_wellSectionFaultStore);
  paleo::fault::FaultSet probe;
  const bool has = m_wellSectionFaultStore->load(probe, &error) && probe.faultCount() > 0;
  m_wellSectionPanel->setFaultsAvailable(has, has ? QString() : tr("工程内无断层解释"));
  if (m_wellSectionBand) {
    QgsVectorLayer *wells = nullptr;
    const auto layers = QgsProject::instance()->mapLayers();
    for (auto it = layers.cbegin(); it != layers.cend(); ++it)
      if (auto *layer = qobject_cast<QgsVectorLayer *>(it.value()))
        if (layer->customProperty(QStringLiteral("paleoLayerId")).toString() == QStringLiteral("wells")) {
          wells = layer; break;
        }
    m_wellSectionBand->setWellLayer(wells, QStringLiteral("id"));
  }
  if (m_wellSectionFence) m_wellSectionFence->setStore(m_wellSectionStore, m_wellSectionFaultStore);
  // #213：剖面页的解释岩性资产选择（含整表恢复）→ 推给测井预测服务，预测输入
  // 的岩性与界面展示同源。连接只需建一次；恢复路径由
  // setLithoAssetSelections 的逐条广播触发（本函数先于恢复调用）。
  if (m_wellSectionWf && !m_lithoSyncConnected) {
    m_lithoSyncConnected = true;
    connect(m_wellSectionWf, &WellSectionWorkflow::lithoSelectionChanged, this,
            [this] { pushLithoSelectionsToPrediction(); });
  }
  return true;
}

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

  // 剖面编辑产物落库（井序 + 连线改接 + 深度域 + 井间距；每次落盘版本 +1
  // ——Oracle #2）。深度域/间距进剖面状态（方向 69/98 工程级 round-trip）：
  // 用户域/井距动作与井序/改接同一保存钩子。无工程库（store 空/未开工程）
  // 时编辑仅驻内存。

  const auto boundPath = std::make_shared<QString>();
  const auto sessionClosed = std::make_shared<bool>(false);
  const auto bindStores = [this, store, boundPath, sessionClosed] {
    if (*sessionClosed) { clearWellSectionStores(); boundPath->clear(); return false; }
    return bindWellSectionStores(store, boundPath.get());
  };
  bindStores();
  connect(panel, &WellSectionPanel::faultsRequested, this,
          [wf, panel, this] {
            // 每次请求现读 store（最近落盘的断层解释即时可见）。
            const WellSectionWorkflow::FaultProjection fp =
                wf->faultProjection(panel->wells());
            panel->setFaultTraces(fp.traces, fp.status);
          });

  // 三处一致性：主面板的用户域/井距动作 → 打开中的栅状图各剖面同步；
  // 反向（fence 内动作 → 主面板）在 fence 创建处接线。
  connect(panel, &WellSectionPanel::depthDomainChanged, this,
          [this](wellsection::DepthDomain domain) {
            if (m_wellSectionFence)
              m_wellSectionFence->setDepthDomain(domain);
          });
  connect(panel, &WellSectionPanel::spacingModeChanged, this,
          [this](wellsection::SpacingMode mode) {
            if (m_wellSectionFence)
              m_wellSectionFence->setSpacingMode(mode);
          });

  // #128：结果按 workflow 的当前世代过滤（wf->currentGeneration()），不记
  // request() 的返回值——同步/早退路径在返回前就已 emit，旧实现把那一次
  // 结果当陈旧丢掉（面板卡在忙碌、地震缝状态不显示）。
  auto catalogPath =
      std::make_shared<QString>(m_previewDoc->catalog()->catalogPath());

  // 井集/连线改接/深度域持久化：用户改动才写（程序化恢复不发这些信号）。
  const auto saveSectionEdits = [this, panel] {
    if (!m_wellSectionStore)
      return;
    QString err;
    const metadata::WellSectionRecord rec =
        m_wellSectionStore->save(QString::fromLatin1(kSectionId), panel->wellIds(),
                                 toStoreOverrides(panel->linkOverrides()),
                                 panel->depthDomain(), panel->spacingMode(),
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
  connect(panel, &WellSectionPanel::depthDomainChanged, this,
          [saveSectionEdits](wellsection::DepthDomain) {
            saveSectionEdits();
          });
  // 井间距同为剖面状态（方向 98）：用户动作与域/井序同一保存钩子。
  connect(panel, &WellSectionPanel::spacingModeChanged, this,
          [saveSectionEdits](wellsection::SpacingMode) {
            saveSectionEdits();
          });

  connect(panel, &WellSectionPanel::dataRequested, this,
          [wf, panel](const QStringList &ids, const QStringList &mnemonics) {
            panel->setBusy(true);
            wf->request(ids, mnemonics);
          });

  // 分层来源切换：面板菜单点选（已写 QSettings）→ workflow 选择 + 重取。
  // 井集空时只记选择——下一场景恢复/选井请求自然带上。
  const auto toTopsChoices =
      [](const QVector<WellSectionWorkflow::TopsSourceChoice> &in) {
        QVector<WellSectionPanel::TopsSourceChoice> out;
        out.reserve(in.size());
        for (const auto &c : in)
          out.push_back({c.assetId, c.label});
        return out;
      };
  connect(panel, &WellSectionPanel::topsSourceChanged, this,
          [wf, panel](const QString &assetId) {
            wf->setTopsSource(assetId);
            if (!panel->wellIds().isEmpty())
            {
              panel->setBusy(true);
              wf->request(panel->wellIds(),
                          panel->sectionTemplate().mnemonics());
            }
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
            const auto volume=m_sectionLink?m_sectionLink->activeVolume():nullptr;
            const auto src=wf->seismicSource(volume?paleo::fromFsPath(volume->Path()):QString());
            wf->requestSeismic(panel->wells(), src);
          });
  connect(wf, &WellSectionWorkflow::seismicReady, this,
          [wf, panel](int gen, const wellsection::SeismicStrip &strip) {
            if (gen != wf->currentSeismicGeneration())
              return;
            panel->setSeismicStrip(strip);
          });

  // 地震可用性：初始一次 + 剖面体变更跟随（禁用按钮带原因，§35）。
  const auto syncSeismic = [this, wf, panel] {
    const auto volume=m_sectionLink?m_sectionLink->activeVolume():nullptr;
    const auto source=wf->seismicSource(volume?paleo::fromFsPath(volume->Path()):QString());
    panel->setSeismicAvailable(!source.path.isEmpty(),source.status);
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
  const auto restoreWells = [this, wf, panel, bindStores]() {
    QStringList saved;
    QVector<wellsection::LinkOverride> overrides;
    if (bindStores() && m_wellSectionStore)
    {
      QString err;
      const metadata::WellSectionRecord rec =
          m_wellSectionStore->load(QString::fromLatin1(kSectionId), &err);
      saved = rec.wellIds;
      overrides = fromStoreOverrides(rec.linkOverrides);
      // 深度域是剖面状态（方向 69）：仅库中有记录（version>0）才以工程级
      // 覆盖面板；无记录时保留面板现状——应用级 QSettings 偏好就是缺省，
      // 不得被 store 缺省 MD 静默盖回（R1-3 M1）。程序化 setter 不发信号、
      // 不重发数据请求之外的持久化钩子；此时面板井集已清（setDepthDomain
      // 不触发在途请求）。井间距同口径（方向 98）：行里存过才回读，缺档
      //（旧库行）保持面板现状。
      if (rec.valid())
      {
        panel->setDepthDomain(rec.depthDomain);
        if (rec.spacing.has_value())
          panel->setSpacingMode(*rec.spacing);
      }
      // 解释来源选择整表灌入（方向 98）：无行 = 全默认取最新；选择失效
      //（资产删链）由消费侧回落 + 告警。
      QString selErr;
      wf->setLithoAssetSelections(
          m_wellSectionStore->loadInterpretationSelections(&selErr));
      if (!selErr.isEmpty())
        QgsMessageLog::logMessage(
            tr("解释岩性来源读回失败：%1").arg(selErr),
            QStringLiteral("Paleo"));
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
          [this, wf, panel, catalogPath, restoreWells, toChoices,
           toTopsChoices, syncSeismic] {
            syncSeismic();
            const auto freshChoices = toChoices(wf->wellChoices());
            panel->setWellChoices(freshChoices);
            // 分层来源候选随 catalog 链接刷新；面板侧对失效选择自动回落
            // 默认——同步 workflow 保持一致。
            panel->setTopsSourceChoices(
                toTopsChoices(wf->topsSourceChoices({})));
            wf->setTopsSource(panel->topsSource());
            if (m_wellSectionFence) // 栅格井选项随 catalog 刷新
              m_wellSectionFence->setChoices(freshChoices);
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
  if (m_sectionWorkbench)
    connect(m_sectionWorkbench, &SectionWorkbench::alignmentsChanged, debounce,
            qOverload<>(&QTimer::start));

  // 剖面-平面联动：剖面线位高亮（井序连线）+ 点名反向闪烁。线位随
  // 井集/井序/取数回填刷新；闪烁经 well 层 fid 解析。
  if (m_canvasCtl && !m_wellSectionBand)
  {
    m_wellSectionBand = new WellSectionMapBand(m_canvasCtl->canvas(), this);
    bindStores(); // 首次解析井位层（band 就位后）
  }
  WellSectionMapBand *const band = m_wellSectionBand;
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
  // 图片锚双击（方向 79）：预览 + 深度输入 → catalog 就地更新（updateVersionExtra
  // 通道，审计进 #history）；changed() 的防抖重载链让剖面随新锚深重摆。
  connect(panel, &WellSectionPanel::imageAnchorEditRequested, this,
          [this, panel](const QString &wellId, const QString &assetId, double curDepth) {
            auto *cat = m_previewDoc ? m_previewDoc->catalog() : nullptr;
            if (!cat)
              return;
            const CatalogVersion v = cat->currentVersion(assetId);
            if (v.id.isEmpty())
              return;
            PaleoDepthAnchorDialog::Context ctx;
            ctx.wellName = cat->entityById(wellId).name;
            ctx.fileName = v.fileName.isEmpty()
                               ? cat->assetById(assetId).displayName
                               : v.fileName;
            ctx.hasAnchor = curDepth > 0.0 && std::isfinite(curDepth);
            ctx.currentDepth = curDepth;
            ctx.anchorSource =
                v.extra.value(QLatin1String("depthMd#source")).toString();
            ctx.imagePath = m_previewDoc->absolutePathForVersion(v);
            PaleoDepthAnchorDialog::Result res;
            if (!PaleoDepthAnchorDialog::prompt(panel, ctx, &res))
              return;
            paleo::WellAttachmentOps ops(cat);
            paleo::DepthInputStatus st = paleo::DepthInputStatus::Ok;
            QString err;
            if (!ops.setDepthAnchor(
                    v.id, res.clear ? QString() : QString::number(res.depth),
                    &st, &err))
              qWarning("wellsection: anchor edit failed for %s: %s",
                       qPrintable(v.id), qPrintable(err));
          });
  // 解释岩性来源（方向 98 多解释显式选择）：枚举该井解释资产（版本降序）
  // → 选择对话框（首项 = 默认取最新，当前选择预选）→ workflow 选择 + 工程
  // 库落表 + 重取剖面（多份时题注/告警点名消费与落选来源）。无解释资产
  // 如实提示（岩屑/GR 推断兜底是正常态），不开对话框。
  connect(panel, &WellSectionPanel::lithoSourceEditRequested, this,
          [this, wf, panel](const QString &wellId) {
            if (wellId.isEmpty())
              return;
            const auto choices = wf->lithoSourceChoices(wellId);
            if (choices.isEmpty())
            {
              QgsMessageLog::logMessage(
                  tr("井 %1 无解释岩性资产（岩屑/GR 推断兜底是正常态）")
                      .arg(wellId),
                  QStringLiteral("Paleo"));
              return;
            }
            const QString def = tr("默认（最新版本）");
            QStringList items;
            items << def;
            const QString current = wf->lithoAssetSelections().value(wellId);
            int selected = 0;
            for (int i = 0; i < choices.size(); ++i)
            {
              items << choices.at(i).label;
              if (choices.at(i).assetId == current)
                selected = i + 1;
            }
            bool ok = false;
            const QString picked = QInputDialog::getItem(
                panel, tr("解释岩性来源"), tr("消费的解释资产："), items,
                selected, /*editable=*/false, &ok);
            if (!ok)
              return;
            const int idx = items.indexOf(picked);
            const QString assetId =
                idx <= 0 ? QString() : choices.at(idx - 1).assetId;
            wf->setLithoAssetSelection(wellId, assetId);
            if (m_wellSectionStore)
            {
              QString storeErr;
              if (!m_wellSectionStore->saveInterpretationSelection(
                      wellId, assetId, &storeErr))
                QgsMessageLog::logMessage(
                    tr("解释岩性来源保存失败：%1").arg(storeErr),
                    QStringLiteral("Paleo"));
            }
            panel->setBusy(true);
            wf->request(panel->wellIds(),
                        panel->sectionTemplate().mnemonics());
          });

  // sectionReady 回填后井位才可读——同步进现有回填槽。
  connect(wf, &WellSectionWorkflow::sectionReady, this,
          [syncBand](int, const QVector<wellsection::Well> &,
                     const QStringList &) { syncBand(); });

  // 栅状图（fence）：面板入口 → 单实例窗（交点井联动在部件内部接线）。
  connect(panel, &WellSectionPanel::fenceRequested, this,
          [this, wf, taskSvc, toChoices, panel, saveSectionEdits] {
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
            fp.faultStore = m_wellSectionFaultStore;
            fp.choices = toChoices(wf->wellChoices());
            auto *fence = new WellSectionFenceWidget(fp, this);
            fence->setAttribute(Qt::WA_DeleteOnClose);
            fence->setWindowFlags(Qt::Window);
            paleo::ui_detail::resizeWithinScreen(fence, QSize(1100, 700)); // R6 #844
            m_wellSectionFence = fence;
            // 反向一致性：fence 内用户域/井距动作 → 主面板（程序化 setter
            // 不发信号，与正向接线无环路）。域动作同属用户驱动 → 一并落库
            // （深度域是剖面状态，方向 69）。
            connect(fence, &WellSectionFenceWidget::depthDomainChanged, panel,
                    [panel, saveSectionEdits](wellsection::DepthDomain domain) {
                      panel->setDepthDomain(domain);
                      saveSectionEdits();
                    });
            connect(fence, &WellSectionFenceWidget::spacingModeChanged, panel,
                    [panel](wellsection::SpacingMode mode) {
                      panel->setSpacingMode(mode);
                    });
            connect(fence, &QObject::destroyed, this,
                    [this] { m_wellSectionFence = nullptr; });
            fence->show();
          });
  // #124：工程即将切换——作废在途取数/井间缝（迟到结果按世代丢弃）并清空
  // 面板。新工程在 projectOpened 恢复，catalog 去抖仍负责后续数据刷新；
  // 恢复为空时面板保持空，不再残留上一工程的井。
  if (m_projectSvc) {
    connect(m_projectSvc, &QgisProjectService::projectAboutToClose, panel,
            [this, wf, panel, sessionClosed, boundPath] {
      *sessionClosed = true;
      wf->cancel();
      panel->setBusy(false);
      panel->setWarnings({});
      panel->setSection({});
      clearWellSectionStores();
      boundPath->clear();
    });
    // AppContext 已先绑定工程元数据库；不依赖 catalogPath 是否变化或 300ms 去抖。
    connect(m_projectSvc, &QgisProjectService::projectOpened, panel,
            [this, wf, panel, sessionClosed, catalogPath, restoreWells, toChoices] {
      *sessionClosed = false;
      *catalogPath = m_previewDoc->catalog()->catalogPath();
      panel->setWellChoices(toChoices(wf->wellChoices()));
      restoreWells();
    });
  }

  // 初始装配同恢复路径。分层来源：先喂候选（面板对失效 id 自动回落
  // 默认），再按面板当前有效值同步 workflow——恢复口径单源在面板。
  panel->setWellChoices(toChoices(wf->wellChoices()));
  panel->setTopsSourceChoices(toTopsChoices(wf->topsSourceChoices({})));
  wf->setTopsSource(panel->topsSource());
  restoreWells();
}
