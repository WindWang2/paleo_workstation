// 层：QGIS 封装
#include "qgislayoutservice.h"
#include "qgiserrors_internal.h"

#include <qgslayout.h>
#include <qgslayoutexporter.h>
#include <qgslayoutmanager.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>

namespace
{
using paleo::qgis_detail::setError;

  QString exportResultString(QgisLayoutService *svc, QgsLayoutExporter::ExportResult res)
  {
    switch (res)
    {
      case QgsLayoutExporter::Success: return QStringLiteral("success");
      case QgsLayoutExporter::Canceled: return QStringLiteral("canceled");
      case QgsLayoutExporter::MemoryError: return QStringLiteral("memory error");
      case QgsLayoutExporter::FileError: return QStringLiteral("file error");
      case QgsLayoutExporter::PrintError: return QStringLiteral("print error");
      case QgsLayoutExporter::SvgLayerError: return QStringLiteral("svg layer error");
      case QgsLayoutExporter::IteratorError: return QStringLiteral("iterator error");
    }
    return QStringLiteral("unknown error");
  }
} // namespace

// qgis/ — QgisLayoutService owns QgsLayoutManager lifecycle for the project.
// The full designer dialog is src/app-only (ET9/D12); the service emits
// designerRequested so the UI shell can attach when built.

QgisLayoutService::QgisLayoutService(QgsProject *project, QObject *parent)
  : QObject(parent)
  , m_project(project)
{
}

QgsLayout *QgisLayoutService::createLayout(const QString &name, QString *error)
{
  if (!m_project)
  {
    setError(error, tr("没有可承载图件的工程"));
    return nullptr;
  }
  if (name.isEmpty())
  {
    setError(error, tr("图件名称不能为空"));
    return nullptr;
  }
  if (m_project->layoutManager()->layoutByName(name))
  {
    setError(error, tr("已存在同名图件「%1」").arg(name));
    return nullptr;
  }

  // QgsPrintLayout (not bare QgsLayout): only QgsPrintLayout implements
  // QgsMasterLayoutInterface — the named, manager-ownable layout type.
  auto layout = std::make_unique<QgsPrintLayout>(m_project);
  layout->setName(name);
  layout->initializeDefaults(); // one default page — a pageless layout can't export
  QgsPrintLayout *raw = layout.get();
  if (!m_project->layoutManager()->addLayout(raw)) // takes ownership on success
  {
    setError(error, tr("无法将图件「%1」加入工程").arg(name));
    return nullptr;
  }
  layout.release();
  emit layoutAdded(name);
  return raw;
}

bool QgisLayoutService::removeLayout(const QString &name)
{
  if (!m_project)
    return false;
  QgsMasterLayoutInterface *iface = m_project->layoutManager()->layoutByName(name);
  if (!iface)
    return false;
  if (!m_project->layoutManager()->removeLayout(iface)) // removes + deletes
    return false;
  emit layoutRemoved(name);
  return true;
}

QStringList QgisLayoutService::layoutNames() const
{
  QStringList names;
  if (!m_project)
    return names;
  const QList<QgsMasterLayoutInterface *> layouts = m_project->layoutManager()->layouts();
  names.reserve(layouts.size());
  for (const QgsMasterLayoutInterface *l : layouts)
    names << l->name();
  return names;
}

QgsLayout *QgisLayoutService::layout(const QString &name) const
{
  if (!m_project)
    return nullptr;
  return dynamic_cast<QgsLayout *>(m_project->layoutManager()->layoutByName(name));
}

bool QgisLayoutService::exportPdf(const QString &layoutName, const QString &outPath, QString *error)
{
  QgsLayout *l = layout(layoutName);
  if (!l)
  {
    setError(error, tr("没有名为「%1」的图件").arg(layoutName));
    return false;
  }
  if (outPath.isEmpty())
  {
    setError(error, tr("图件「%1」的输出路径为空").arg(layoutName));
    return false;
  }

  QgsLayoutExporter exporter(l);
  const QgsLayoutExporter::PdfExportSettings settings;
  const QgsLayoutExporter::ExportResult res = exporter.exportToPdf(outPath, settings);
  if (res != QgsLayoutExporter::Success)
  {
    setError(error, tr("图件「%1」导出 PDF 到 %2 失败：%3")
                      .arg(layoutName, outPath, exportResultString(this, res)));
    return false;
  }
  return true;
}
