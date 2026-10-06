// 层：视图
// 综合图面板·编辑会话与导出（编辑模式/保存派生/sidecar/分层指派/导出打印预设）——自 wellcompositepanel.cpp 拆出（方向 66，行为零变更）
#include "wellcompositepanel.h"
#include "editsession.h"
#include "stratassignment.h"
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPrintDialog>
#include <QPrinter>

namespace WellComposite
{
// ----------------------------------------------------------------------------
// D3.x 编辑模式
// ----------------------------------------------------------------------------
void WellCompositePanel::setEditMode(bool on)
{
  if (m_editSession && m_editSession->isReadOnly() && on)
  {
    // D3.15 只读降级：拒绝进入并给出原因
    m_btnEdit->blockSignals(true);
    m_btnEdit->setChecked(false);
    m_btnEdit->blockSignals(false);
    QMessageBox::information(this, tr("不可编辑"),
                             m_editSession->readOnlyReason().isEmpty()
                                 ? tr("当前资产为只读（RAW 或未授权路径）。")
                                 : m_editSession->readOnlyReason());
    return;
  }
  m_canvas->setEditMode(on);
}

bool WellCompositePanel::editMode() const
{
  return m_canvas->editMode();
}

bool WellCompositePanel::saveDerived()
{
  if (!m_editSession || !m_editSession->isDirty())
    return false;

  // D3.3 派生文档 + manifest 风格摘要（落盘由壳接 derivedDocumentReady；测试
  // 直接断言信号携带的文档内容与审计摘要）
  const ComprehensiveWellData derived = m_editSession->buildDerivedDocument();
  const QString summary = m_editSession->buildManifestStyleSummary();
  // D1（wave/deepen-perf）：审计逐条随行——壳侧登记写「编辑审计」工作表
  emit derivedDocumentReady(derived, summary, m_editSession->auditLines());

  // D3.11 审计摘要同步 sidecar（auditLog 在各编辑操作时已逐条累积）
  if (m_store)
    m_store->save();
  m_editSession->markSaved();
  return true;
}

void WellCompositePanel::setHighContrast(bool on)
{
  m_highContrast = on;
  m_canvas->setHighContrast(on);
}

void WellCompositePanel::onMarkerMoved(const QString &name, double newDepth)
{
  if (!m_editSession)
    return;

  // D3.10 源冲突检测：编辑时源文件被动过 → 警告（重载/分叉由用户选）
  if (!m_sourceDataPath.isEmpty())
  {
    const qint64 cur = QFileInfo(m_sourceDataPath).lastModified().toMSecsSinceEpoch();
    if (m_editSession->sourceChanged(cur))
      m_editSession->checkSourceConflict(cur); // 内部发 sourceConflictDetected
  }

  m_editSession->moveMarker(name, newDepth); // undo 栈 + 审计；documentChanged → 重同步
}

// ----------------------------------------------------------------------------
// sidecar
// ----------------------------------------------------------------------------
void WellCompositePanel::loadSidecar()
{
  if (!m_store)
    return;

  m_canvas->setPins(m_store->pins());
  m_bookmarks = m_store->bookmarks();

  // D3.12 曲线量程/单位覆盖层应用（源 LAS 不动）
  const QVariantMap ov = m_store->curveOverrides();
  if (!ov.isEmpty())
  {
    for (const auto &t : m_canvas->tracks())
    {
      if (auto ct = std::dynamic_pointer_cast<CurveTrack>(t))
      {
        QVector<CurveData> curves = ct->curves();
        for (auto &c : curves)
        {
          const QVariantMap o = ov.value(c.name).toMap();
          if (!o.isEmpty())
            c = applyCurveOverride(c, o);
        }
        ct->setCurves(curves);
      }
    }
  }

  // D3.6 地层指派应用（stratigraphyIntervals 补齐未识别层名的系/统）
  const auto assigns = m_store->stratAssignments();
  if (!assigns.isEmpty() && m_editSession)
  {
    for (const auto &t : m_canvas->tracks())
    {
      if (auto sc = std::dynamic_pointer_cast<StratigraphyCompoundTrack>(t))
        sc->setIntervals(StratAssign::applyAssignments(sc->intervals(), assigns));
    }
  }
  m_canvas->updateAll();
}

void WellCompositePanel::saveSidecar() const
{
  if (!m_store)
    return;
  m_store->setPins(m_canvas->pins());
  m_store->setBookmarks(m_bookmarks);
  m_store->save();
}

void WellCompositePanel::applyStratAssignments(QVector<FormationInterval> *formations) const
{
  // 遗留接口兼容（指派直接作用于 stratigraphyIntervals 展示层，不污染源数据）
  Q_UNUSED(formations);
}

// ----------------------------------------------------------------------------
// 导出（D4.x；引擎实现见 exportengine）
// ----------------------------------------------------------------------------
void WellCompositePanel::exportCurrent(ExportEngine::Format format)
{
  QString filter;
  QString ext;
  switch (format)
  {
  case ExportEngine::Format::Png: filter = tr("PNG 图像 (*.png)"); ext = QStringLiteral("png"); break;
  case ExportEngine::Format::Svg: filter = tr("SVG 矢量 (*.svg)"); ext = QStringLiteral("svg"); break;
  default: filter = tr("PDF 文档 (*.pdf)"); ext = QStringLiteral("pdf"); break;
  }

  const QString suggested = QStringLiteral("%1_综合柱状图.%2").arg(m_wellName.isEmpty() ? QStringLiteral("well") : m_wellName, ext);
  QString path = QFileDialog::getSaveFileName(this, tr("导出综合柱状图"), suggested, filter);
  if (path.isEmpty())
    return;

  ExportEngine::Options opt;
  opt.scaleRatio = m_canvas->scaleRatio();
  opt.topDepth = m_canvas->minDepth();
  opt.bottomDepth = m_canvas->maxDepth();
  opt.includeHeader = true;
  opt.includeLegend = true;
  opt.dpi = 300;
  opt.wellName = m_wellName;
  opt.projectName = m_projectName;

  const QString err = ExportEngine::exportCanvas(*m_canvas, m_data, format, path, opt);
  if (!err.isEmpty())
    QMessageBox::warning(this, tr("导出失败"), err);
  else
    m_lblStatus->setText(tr("已导出: %1").arg(path));
}

void WellCompositePanel::printCurrent()
{
  // D3（wave/deepen-perf）原生打印接线：有系统打印机 → QPrintDialog + QPrinter
  //（与 PDF 导出同一分页渲染管线 exportToPagedDevice；取消 = 用户意图静默返回）。
  // 无打印环境（offscreen/无打印服务）→ 降级为 PDF 导出（QPdfWriter 即打印
  // 数据流，wave/wellcomposite D4.8 决策语义保持）。
  if (ExportEngine::nativePrintAvailable())
  {
    QPrinter printer(QPrinter::HighResolution);
    printer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Portrait,
                                      QMarginsF(12, 14, 12, 14), QPageLayout::Millimeter));
    printer.setDocName(QStringLiteral("%1_综合柱状图").arg(
        m_wellName.isEmpty() ? QStringLiteral("well") : m_wellName));
    QPrintDialog dlg(&printer, this);
    dlg.setWindowTitle(tr("打印综合柱状图"));
    if (dlg.exec() != QDialog::Accepted)
      return;

    ExportEngine::Options opt;
    opt.scaleRatio = m_canvas->scaleRatio();
    opt.topDepth = m_canvas->minDepth();
    opt.bottomDepth = m_canvas->maxDepth();
    opt.includeHeader = true;
    opt.includeLegend = true;
    opt.wellName = m_wellName;
    opt.projectName = m_projectName;
    const QString err = ExportEngine::exportToPagedDevice(*m_canvas, m_data, printer, opt);
    if (!err.isEmpty())
      QMessageBox::warning(this, tr("打印失败"), err);
    else
      m_lblStatus->setText(tr("已发送到打印机: %1").arg(printer.printerName()));
    return;
  }
  m_lblStatus->setText(tr("未检测到系统打印机——打印降级为导出 PDF"));
  exportCurrent(ExportEngine::Format::Pdf);
}

void WellCompositePanel::manageExportPresets()
{
  if (!m_store)
  {
    QMessageBox::information(this, tr("导出预设"), tr("加载井数据后可用（预设按源数据 sidecar 保存）。"));
    return;
  }

  QStringList rows;
  const auto presets = m_store->exportPresets();
  for (const auto &p : presets)
    rows << QStringLiteral("%1 [%2 %3dpi]").arg(p.name, p.format, QString::number(p.dpi));
  QMessageBox::information(this, tr("导出预设"),
                           rows.isEmpty() ? tr("暂无预设。导出一次后可经 sidecar 保存。")
                                          : rows.join(QLatin1Char('\n')));
}

} // namespace WellComposite
