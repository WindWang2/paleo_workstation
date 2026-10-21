// 层：视图
// 井位类预览页的 GIS 底图挂载（#14）：测区全景、井位表、分层落图、井相/
// GeoJSON 成果（如 D53-wells_prediction）、成果图件。统一走
// PreviewBasemapControl（在线预置源 / 自定义 XYZ / 本地文件 / 工程图层）。
// 本文件只做控件与接线；页由各分支照常建，建完后在这里按对象名挂载。
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"
#include "previewbasemapcontrol.h"
#include "previewmappage.h"

#include <QBoxLayout>
#include <QLabel>
#include <QTabWidget>

namespace {
// 显示井位/井点的预览页 → 控件对象名前缀。
QString basemapPrefix(const QString &pageName)
{
  static const QHash<QString, QString> prefixes{
      {QStringLiteral("surveyPreviewPage"), QStringLiteral("survey")},
      {QStringLiteral("wellHeadPreviewPage"), QStringLiteral("wellHead")},
      {QStringLiteral("topsPreviewPage"), QStringLiteral("tops")},
      {QStringLiteral("faciesPreviewPage"), QStringLiteral("facies")},
      {QStringLiteral("mapProductPreviewPage"), QStringLiteral("mapProduct")}};
  return prefixes.value(pageName);
}
} // namespace

bool DataPreviewTabs::applyBasemapToCurrentMap(const QString &spec)
{
  if (!isVisible() || !m_tabs || !m_tabs->currentWidget()) return false;
  auto *control = m_tabs->currentWidget()->findChild<PreviewBasemapControl *>();
  if (!control || !control->isVisible()) return false;
  control->applyChoice(spec);
  return true;
}

QWidget *DataPreviewTabs::attachOnlineBasemap(QWidget *content)
{
  if (!content) return content;
  for (auto *mapPage : content->findChildren<PreviewMapPage *>()) {
    const QString prefix = basemapPrefix(mapPage->objectName());
    if (prefix.isEmpty() || mapPage->findChild<PreviewBasemapControl *>()) continue;
    QLabel *status = nullptr;
    if (prefix == QLatin1String("survey") && m_project) { // 测区全景沿用页底署名条（无工程不建）
      status = content->findChild<QLabel *>(QStringLiteral("surveyBasemapAttribution"));
      if (!status) {
        status = new QLabel(content);
        status->setObjectName(QStringLiteral("surveyBasemapAttribution"));
        status->setWordWrap(true);
        PaleoTheme::applyThemedStyleSheet(status, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
        if (auto *box = qobject_cast<QBoxLayout *>(content->layout())) box->addWidget(status);
      }
    }
    attachPreviewBasemap(mapPage, m_project, prefix, status);
  }
  return content;
}
