// 层：视图
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"
#include "../../workflow/xmlpreviewsession.h"
#include "../../workflow/documentsession.h"
#include "../../workflow/welltracksourcesession.h"
#include "../notifications/paleonotify.h"
#include "../wellcomposite/wellcompositepanel.h"
#include "../paleoviewport.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSet>
#include <QStackedWidget>
#include <QTimer>
#include <cmath>
#include <memory>

using namespace paleo::datapreview_detail;
namespace {
void addPreviewSwitch(QBoxLayout *layout,QWidget *bar,QWidget *host,
    WellComposite::WellCompositePanel *chart,QStackedWidget *stack,QToolButton *tableButton) {
  auto *row=new PaleoToolRow(bar,host);row->setObjectName(QStringLiteral("wellPreviewModeRow"));row->hide();
  layout->addWidget(row);
  const QPointer<WellComposite::WellCompositePanel> guarded(chart);
  QObject::connect(stack,&QStackedWidget::currentChanged,row,[row,guarded,stack](int index){row->setVisible(index!=0||!guarded||stack->currentWidget()!=guarded);});
  chart->addPreviewAction(DataPreviewTabs::tr("数据列表"),[tableButton]{tableButton->click();});
}
QString coordinateSummary(const WellComposite::ComprehensiveWellData& data) {
  const auto known=[](const QString& value){return value.isEmpty()?QObject::tr("未确认"):value;};
  const auto number=[](double value){return std::isfinite(value)?QString::number(value,'g',12):QObject::tr("未提供");};
  return QObject::tr("井口 X：%1，Y：%2；井坐标系：%3（%4）；工程坐标系：%5（%6）")
      .arg(number(data.x),number(data.y),known(data.coordinateSystem),known(data.coordinateUnit),
           known(data.projectCoordinateSystem),known(data.projectCoordinateUnit));
}

void chooseDocumentCurve(WellComposite::WellCompositePanel *chart,
                         paleo::welltrack::WellTrackSourceSession *session) {
  using paleo::welltrack::CurveChoice;
  if (!session->canEdit())
    return;

  auto *dialog = new QDialog(chart);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(DataPreviewTabs::tr("添加曲线道"));
  dialog->setObjectName(QStringLiteral("wellSourceCurveDialog"));
  auto *layout = new QVBoxLayout(dialog);
  auto *fields = new QFormLayout;
  fields->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *files = new QComboBox(dialog);
  auto *curves = new QComboBox(dialog);
  files->setObjectName(QStringLiteral("wellSourceFile"));
  curves->setObjectName(QStringLiteral("wellSourceCurve"));
  fields->addRow(DataPreviewTabs::tr("来源文件"), files);
  fields->addRow(DataPreviewTabs::tr("曲线"), curves);
  layout->addLayout(fields);
  auto *status = new QLabel(DataPreviewTabs::tr("正在读取工区模型的测井曲线目录…"), dialog);
  status->setTextFormat(Qt::PlainText);
  status->setWordWrap(true);
  layout->addWidget(status);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
  auto *add = buttons->button(QDialogButtonBox::Ok);
  add->setText(DataPreviewTabs::tr("添加曲线道"));
  add->setEnabled(false);
  layout->addWidget(buttons);

  const auto choices = std::make_shared<QVector<CurveChoice>>();
  const auto fillCurves = [choices, files, curves] {
    curves->clear();
    if (files->currentIndex() < 0)
      return;
    const auto sourceId = files->currentData().toLongLong();
    for (qsizetype i = 0; i < choices->size(); ++i) {
      const auto &choice = choices->at(i);
      if (choice.sourceId == sourceId)
        curves->addItem(QStringLiteral("%1 · #%2").arg(choice.curveName, QString::number(choice.curveId)),
                        QVariant::fromValue(i));
    }
  };
  QObject::connect(files, &QComboBox::currentIndexChanged, dialog, fillCurves);
  QObject::connect(curves, &QComboBox::currentIndexChanged, dialog,
                   [add](int index) { add->setEnabled(index >= 0); });
  QObject::connect(session, &paleo::welltrack::WellTrackSourceSession::choicesReady, dialog,
                   [choices, files, status](const QVector<CurveChoice> &result, const QString &error) {
    *choices = error.isEmpty() ? result : QVector<CurveChoice>{};
    files->clear();
    QSet<qlonglong> sources;
    for (const auto &choice : *choices) {
      if (sources.contains(choice.sourceId))
        continue;
      sources.insert(choice.sourceId);
      files->addItem(QStringLiteral("%1 · #%2").arg(choice.fileName, QString::number(choice.sourceId)),
                     choice.sourceId);
      files->setItemData(files->count() - 1, choice.filePath, Qt::ToolTipRole);
    }
    status->setText(!error.isEmpty() ? error : choices->isEmpty()
        ? DataPreviewTabs::tr("此井没有登记在工区模型中的可用 LAS 曲线。")
        : QString());
  });
  const QPointer<paleo::welltrack::WellTrackSourceSession> owner(session);
  QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [dialog, owner, curves, choices] {
    if (!owner || curves->currentIndex() < 0)
      return;
    const auto index = curves->currentData().toLongLong();
    if (index < 0 || index >= choices->size())
      return;
    const auto choice = choices->at(index);
    dialog->accept();
    if (owner)
      owner->addCurve(choice);
  });
  QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  QObject::connect(session, &QObject::destroyed, dialog, &QDialog::reject);
  dialog->open();
  session->requestChoices();
}

void wireDocumentCurveTracks(WellComposite::WellCompositePanel *chart,
                             paleo::document::DocumentSession *document,
                             PaleoTaskService *tasks, const QString &project,
                             const QString &wellEntityId, qlonglong modelWellId) {
  using namespace paleo::welltrack;
  if (!document || !tasks || wellEntityId.isEmpty())
    return;
  auto *session = new WellTrackSourceSession(document, tasks, project, wellEntityId, modelWellId, chart);
  auto *canvas = chart->canvas();
  if (session->canEdit())
    chart->addPreviewAction(DataPreviewTabs::tr("添加曲线道…"),
                            [chart, session] { chooseDocumentCurve(chart, session); });
  QObject::connect(session, &WellTrackSourceSession::tracksChanged, chart,
                   [chart, session](const QVector<Track> &tracks) {
    QList<QPair<QString, QString>> layout;
    layout.reserve(tracks.size());
    for (const auto &track : tracks)
      layout.append({track.id, track.label});
    chart->setDocumentCurveTracks(layout, session->canEdit());
  });
  QObject::connect(session, &WellTrackSourceSession::trackReady,
                   chart, &WellComposite::WellCompositePanel::setDocumentCurveWindow);
  QObject::connect(session, &WellTrackSourceSession::failed, chart, [chart](const QString &error) {
    PaleoNotify::warning(chart, DataPreviewTabs::tr("曲线道操作失败"), error);
  });
  QObject::connect(canvas, &WellComposite::WellCompositeCanvas::documentTrackRemovalRequested,
                   session, &WellTrackSourceSession::removeTrack);
  const auto requestWindow = [chart, canvas, session] {
    if (!chart->isVisible()) {
      session->requestWindow(qQNaN(), qQNaN());
      return;
    }
    const double top = qMax(canvas->minDepth(), canvas->visibleTopDepth());
    const double bottom = qMin(canvas->visibleBottomDepth(), canvas->maxDepth());
    if (std::isfinite(top) && std::isfinite(bottom) && bottom > top)
      session->requestWindow(top, bottom);
  };
  // Canvas geometry stays in metres MD. Feet and TWT are display labels.
  QObject::connect(canvas, &WellComposite::WellCompositeCanvas::viewportChanged, chart, requestWindow);
  QObject::connect(chart, &WellComposite::WellCompositePanel::previewVisibilityChanged, session,
                   [requestWindow, session](bool visible) {
    if (visible)
      requestWindow();
    else
      session->requestWindow(qQNaN(), qQNaN());
  });
  QTimer::singleShot(0, chart, requestWindow);
}
}

void DataPreviewTabs::setDocumentSession(paleo::document::DocumentSession *session) {
  m_document=session;
  if(!session)return;
  connect(session,&paleo::document::DocumentSession::interpretationChanged,this,[this](const QString &wellId) {
    const QString focused=assetIdAt(m_tabs->currentIndex());
    QVector<QPair<qlonglong,QString>> wells;
    const auto keys=m_pageOfAsset.keys();
    for(const auto &key:keys) {
      if(key.startsWith(QLatin1String("model-well:"))) {
        const auto modelWellId=key.mid(11).toLongLong();
        if(!wellId.isEmpty() && (!m_doc || m_doc->entityIdForModelWell(modelWellId)!=wellId))continue;
        wells.append({modelWellId,m_tabs->tabText(m_tabs->indexOf(m_pageOfAsset.value(key)))});
        closeAssetTab(key);
      } else if((wellId.isEmpty() || m_wellEntityOfAsset.value(key)==wellId) &&
                m_pageOfAsset.value(key) && m_pageOfAsset.value(key)->findChild<XmlPreviewSession *>()) {
        rebuildAssetTab(key);
      }
    }
    for(const auto &[id,name]:wells)openModelWell(id,name);
    if(auto *page=m_pageOfAsset.value(focused))m_tabs->setCurrentWidget(page);
  });
}

QWidget *DataPreviewTabs::buildAuxiliaryXmlContent(const QString &abs, const QString &assetId,
                                                   QWidget *host, QVBoxLayout *lay, const QString &expectedSha, bool reference)
{
  auto *chart = new WellComposite::WellCompositePanel(host, WellComposite::WellCompositePanel::PredictionScope::WholeWellPreview);
  chart->setProjectName(catalogProjectDir(m_doc ? m_doc->catalog() : nullptr));
  chart->setObjectName("wellCompositePanel");
  auto *hint = new QLabel(tr("正在后台读取井道图与数据列表…"), host);
  hint->setWordWrap(true);
  hint->setObjectName("xmlPendingHint");
  PaleoTheme::applyThemedStyleSheet(hint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  auto *tableHost = new QWidget(host);
  auto *tableLayout = new QVBoxLayout(tableHost);
  tableLayout->setContentsMargins(0, 0, 0, 0);
  tableLayout->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *tableHint = new QLabel(tr("正在后台读取数据列表…"), tableHost);
  PaleoTheme::applyThemedStyleSheet(tableHint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  tableLayout->addWidget(tableHint);
  auto *stack = new QStackedWidget(host);
  stack->setObjectName("xmlViewStack"); stack->addWidget(chart); stack->addWidget(tableHost);
  auto *bar = new QWidget(host);
  auto *buttons = new QHBoxLayout(bar);
  buttons->setContentsMargins(0, 0, 0, 0); buttons->setSpacing(PaleoTheme::tokens().spacingSm);
  styleViewSwitchBar(bar);
  auto *chartButton = new QToolButton(bar);
  chartButton->setObjectName("btnXmlCompositeView"); chartButton->setText(tr("井道图"));
  chartButton->setCheckable(true); chartButton->setChecked(true);
  auto *tableButton = new QToolButton(bar);
  tableButton->setObjectName("btnXmlTableView"); tableButton->setText(tr("数据列表")); tableButton->setCheckable(true);
  const auto select = [stack, chartButton, tableButton](int index) {
    stack->setCurrentIndex(index); chartButton->setChecked(index == 0); tableButton->setChecked(index == 1);
  };
  connect(chartButton, &QToolButton::clicked, host, [select] { select(0); });
  connect(tableButton, &QToolButton::clicked, host, [select] { select(1); });
  buttons->addWidget(caption8(tr("呈现模式:"), bar)); buttons->addWidget(chartButton);
  buttons->addWidget(tableButton); buttons->addStretch(1);
  addPreviewSwitch(lay,bar,host,chart,stack,tableButton); lay->addWidget(hint); lay->addWidget(stack, 1);
  auto *session = new XmlPreviewSession(host);
  session->setTaskService(m_doc ? m_doc->taskService() : nullptr);
  connect(session, &XmlPreviewSession::ready, host,
      [this, abs, assetId, host, stack, chart, hint, tableHint, tableHost, tableLayout, reference](const XmlPreviewData &data) {
    hint->hide(); delete tableHint;
    buildWorkbookTable(data.table, tableHost, tableLayout);
    if (data.chartOk) {
      chart->loadWellData(data.chart, abs, reference);
      if (data.chart.modelWellId) {
        chart->setPreviewSummary(QStringList{coordinateSummary(data.chart),data.interpretationSummary}.join(QChar('\n')).trimmed());
      }
    }
    else {
      const int selected = stack->currentIndex();
      stack->removeWidget(chart); chart->deleteLater();
      auto *failure = failureState(assetId, tr("井道图无法读取：%1").arg(data.chartError), host);
      stack->insertWidget(0, failure); stack->setCurrentIndex(selected);
    }
  });
  const auto *catalog = m_doc ? m_doc->catalog() : nullptr;
  const auto version = catalog ? catalog->currentVersion(assetId) : CatalogVersion{};
  const auto chosen = m_chosenVersionOfAsset.value(assetId);
  const qlonglong modelSource = (chosen.isEmpty() || chosen == version.id) && version.stage == "RAW"
      ? version.extra.value("areaModelSourceId").toLongLong() : 0;
  const qlonglong modelWell = catalog
      ? catalog->entityById(m_wellEntityOfAsset.value(assetId)).extra.value("areaModelWellId").toLongLong() : 0;
  if(modelSource>0 && modelWell>0)session->setDocumentSession(m_document,m_wellEntityOfAsset.value(assetId));
  session->open(abs, expectedSha, catalogProjectDir(catalog), modelSource, modelWell);
  return host;
}

void DataPreviewTabs::openModelWell(qlonglong modelWellId, const QString &wellName)
{
  if (!m_doc || modelWellId <= 0)
    return;
  const QString key = QStringLiteral("model-well:%1").arg(modelWellId);
  if (QWidget *existing = m_pageOfAsset.value(key))
  {
    m_tabs->setCurrentIndex(m_tabs->indexOf(existing));
    return;
  }
  const QString title = wellName.isEmpty() ? tr("井 %1").arg(modelWellId) : wellName;
  QWidget *page = new QWidget(this);
  auto *pageLay = new QVBoxLayout(page);
  pageLay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm,
                              PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  const int idx = m_tabs->addTab(page, title);
  m_pageOfAsset.insert(key, page);
  m_tabs->setVisible(true);
  m_emptyLabel->setVisible(false);
  m_tabs->setCurrentIndex(idx);

  auto *chart = new WellComposite::WellCompositePanel(page, WellComposite::WellCompositePanel::PredictionScope::WholeWellPreview);
  const QString project = catalogProjectDir(m_doc->catalog());
  chart->setProjectName(project);
  auto *hint = new QLabel(tr("正在后台从数据基座读取 %1 的井道图…").arg(title), page);
  hint->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(hint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  auto *tableHost = new QWidget(page);
  auto *tableLayout = new QVBoxLayout(tableHost);
  tableLayout->setContentsMargins(0, 0, 0, 0);
  auto *tableHint = new QLabel(tr("正在后台读取数据列表…"), tableHost);
  tableLayout->addWidget(tableHint);
  auto *stack = new QStackedWidget(page);
  stack->addWidget(chart);
  stack->addWidget(tableHost);
  auto *bar = new QWidget(page);
  auto *buttons = new QHBoxLayout(bar);
  buttons->setContentsMargins(0, 0, 0, 0);
  styleViewSwitchBar(bar);
  auto *chartButton = new QToolButton(bar);
  chartButton->setText(tr("井道图"));
  chartButton->setCheckable(true);
  chartButton->setChecked(true);
  auto *tableButton = new QToolButton(bar);
  tableButton->setText(tr("数据列表"));
  tableButton->setCheckable(true);
  const auto select = [stack, chartButton, tableButton](int index) {
    stack->setCurrentIndex(index);
    chartButton->setChecked(index == 0);
    tableButton->setChecked(index == 1);
  };
  connect(chartButton, &QToolButton::clicked, page, [select] { select(0); });
  connect(tableButton, &QToolButton::clicked, page, [select] { select(1); });
  buttons->addWidget(caption8(tr("呈现模式:"), bar));
  buttons->addWidget(chartButton);
  buttons->addWidget(tableButton);
  buttons->addStretch(1);
  addPreviewSwitch(pageLay,bar,page,chart,stack,tableButton);
  pageLay->addWidget(hint);
  pageLay->addWidget(stack, 1);
  auto *session = new XmlPreviewSession(page);
  session->setTaskService(m_doc->taskService());
  const QString wellEntityId = m_doc->entityIdForModelWell(modelWellId);
  connect(session, &XmlPreviewSession::ready, page,
          [page, stack, chart, hint, tableHint, tableHost, tableLayout, key, project, wellEntityId,
           modelWellId, this](const XmlPreviewData &data) {
            hint->hide();
            delete tableHint;
            buildWorkbookTable(data.table, tableHost, tableLayout);
            if (data.chartOk && chart->loadWellData(data.chart, QString(), false))
            {
              wireDocumentCurveTracks(chart, m_document, m_doc->taskService(),
                                      project, wellEntityId, modelWellId);
              if (data.chart.modelWellId) {
                QStringList summary{coordinateSummary(data.chart), data.interpretationSummary};
                summary.append(data.chart.dataWarnings);
                chart->setPreviewSummary(summary.join(QChar('\n')).trimmed());
              }
            }
            else
            {
              const int selected = stack->currentIndex();
              stack->removeWidget(chart);
              chart->deleteLater();
              auto *failure = failureState(key, tr("井道图无法读取：%1").arg(data.chartError), page);
              stack->insertWidget(0, failure);
              stack->setCurrentIndex(selected);
            }
          });
  session->setDocumentSession(m_document,wellEntityId);
  session->openModelWell(project,modelWellId,title);
}
