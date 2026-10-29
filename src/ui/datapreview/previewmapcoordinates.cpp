// 层：视图
#include "previewmappage.h"

#include "../paleotheme.h"
#include "../pages/pageshared.h"
#include "qgis/coordinateeditor.h"
#include "qgis/coordinatemaptool.h"
#include "qgis/coordinatetoolsdialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>

void PreviewMapPage::copyCoordinate(bool geographic)
{
  if (!m_hasMousePos)
  {
    m_coordLabel->setText(tr("请先把鼠标移入地图，再复制坐标。"));
    return;
  }
  QString error;
  const QString text = m_canvas->coordinateClipboardText(
      QPointF(m_lastMousePos.x(), m_lastMousePos.y()), geographic, &error);
  if (!error.isEmpty())
  {
    m_coordLabel->setText(error);
    return;
  }
  QApplication::clipboard()->setText(text);
  m_coordLabel->setToolTip(geographic ? tr("已复制 WGS84 经纬度。")
                                     : tr("已复制当前预览地图坐标系坐标。"));
}

void PreviewMapPage::openCoordinateTools()
{
  auto *dialog = m_canvas->createCoordinateToolsDialog(this);
  dialog->dialog()->setAttribute(Qt::WA_DeleteOnClose);
  // WA_DeleteOnClose 只删内层 QDialog——包装随内层一起回收，否则每次开关
  // 对话框泄漏一个死包装（内层 destroyed 也连带拾取工具的清理链生效）。
  connect(dialog->dialog(), &QObject::destroyed, dialog, &QObject::deleteLater);
  paleo::pagesinternal::markPrimaryButton(dialog->convertButton());
  dialog->editor()->xEdit()->setFont(PaleoTheme::monoFont());
  dialog->editor()->yEdit()->setFont(PaleoTheme::monoFont());
  if (auto *result = dialog->dialog()->findChild<QLineEdit *>(QStringLiteral("coordinateResult")))
    result->setFont(PaleoTheme::monoFont());
  if (m_hasMousePos) dialog->setSourcePoint(m_lastMousePos);
  connect(dialog->editor(), &PaleoCoordinateEditor::mapPickRequested, this,
          [this, dialog](PaleoCoordinateEditor *editor) {
            auto *tool = m_canvas->beginCoordinatePick();
            if (!tool) return;
            const auto restore = [guard = QPointer<PaleoCoordinateToolsDialog>(dialog)] {
              // dialog() 判空：内层已毁、包装待删的窗口内保持结构安全
              //（R2-D3 硬化——把正确性从时序论证变成显式守卫）。
              if (guard && guard->dialog())
                guard->dialog()->open();
            };
            connect(tool, &PaleoCoordinateMapTool::pointPicked, dialog,
                    [editor, restore](const auto &point) {
                      editor->showMapPoint(point);
                      restore();
                    });
            connect(tool, &PaleoCoordinateMapTool::cancelled, dialog, [restore] { restore(); });
            connect(dialog, &QObject::destroyed, tool, &QObject::deleteLater);
            dialog->dialog()->hide();
          });
  dialog->dialog()->open();
}
