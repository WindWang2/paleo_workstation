// 层：视图
#include "paleomainwindow.h"

#include "paleoicons.h"
#include "paleotheme.h"
#include "pages/pageshared.h"
#include "qgis/coordinateeditor.h"
#include "qgis/coordinateformat.h"
#include "qgis/coordinatemaptool.h"
#include "qgis/coordinatetoolsdialog.h"
#include "qgis/crsselector.h"
#include "qgis/qgiscanvascontroller.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointF>
#include <QStatusBar>
#include <QToolButton>

void PaleoMainWindow::buildCoordinateTools(QLabel *coordLabel)
{
  auto *button = new QToolButton(this);
  button->setObjectName(QStringLiteral("statusCoordinateToolsButton"));
  auto *menu = new QMenu(button);
  auto *toolsAction = menu->addAction(
      PaleoIcons::qgisTheme(QStringLiteral("mActionMapSettings.svg")),
      tr("坐标定位与转换…"));
  toolsAction->setObjectName(QStringLiteral("statusCoordinateToolsAction"));
  auto *copyMapAction = menu->addAction(
      PaleoIcons::qgisTheme(QStringLiteral("mActionEditCopy.svg")), tr("复制地图坐标"));
  copyMapAction->setObjectName(QStringLiteral("statusCopyMapCoordinates"));
  auto *copyWgsAction = menu->addAction(
      PaleoIcons::qgisTheme(QStringLiteral("mActionEditCopy.svg")), tr("复制 WGS84 坐标"));
  copyWgsAction->setObjectName(QStringLiteral("statusCopyWgs84Coordinates"));
  button->setDefaultAction(toolsAction);
  button->setMenu(menu);
  button->setPopupMode(QToolButton::MenuButtonPopup);
  button->setText(tr("坐标"));
  button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  button->setToolTip(tr("QGIS 坐标定位、格式转换、基准操作与坐标复制"));
  statusBar()->addPermanentWidget(button);
  coordLabel->setToolTip(tr("地图坐标由 QGIS 格式化；使用右侧“坐标”菜单定位、转换或复制。"));

  const auto copy = [this, coordLabel](bool geographic) {
    if (!coordLabel->property("mapX").isValid() || !m_canvasCtl)
    {
      statusBar()->showMessage(tr("请先把鼠标移入地图，再复制坐标。"), 5000);
      return;
    }
    QString error;
    const QString text = m_canvasCtl->coordinateClipboardText(
        QPointF(coordLabel->property("mapX").toDouble(), coordLabel->property("mapY").toDouble()),
        geographic, &error);
    if (!error.isEmpty())
    {
      statusBar()->showMessage(error, 8000);
      return;
    }
    QApplication::clipboard()->setText(text);
    statusBar()->showMessage(geographic ? tr("已复制 WGS84 经纬度。")
                                       : tr("已复制当前地图坐标系坐标。"), 5000);
  };
  connect(copyMapAction, &QAction::triggered, this, [copy] { copy(false); });
  connect(copyWgsAction, &QAction::triggered, this, [copy] { copy(true); });
  connect(toolsAction, &QAction::triggered, this, [this, coordLabel] {
    if (!m_canvasCtl) return;
    auto *canvas = m_canvasCtl->canvas();
    auto *dialog = new PaleoCoordinateToolsDialog(canvas, this);
    dialog->dialog()->setAttribute(Qt::WA_DeleteOnClose);
    paleo::pagesinternal::markPrimaryButton(dialog->convertButton());
    dialog->editor()->xEdit()->setFont(PaleoTheme::monoFont());
    dialog->editor()->yEdit()->setFont(PaleoTheme::monoFont());
    if (auto *result = dialog->dialog()->findChild<QLineEdit *>(QStringLiteral("coordinateResult")))
      result->setFont(PaleoTheme::monoFont());
    if (coordLabel->property("mapX").isValid())
      dialog->setSourcePoint(paleo::coordinates::point(coordLabel->property("mapX").toDouble(),
                                                        coordLabel->property("mapY").toDouble()));
    connect(dialog, &PaleoCoordinateToolsDialog::mapCenterChanged, this, [this] {
      statusBar()->showMessage(tr("已按工程转换上下文定位到结果点。"), 5000);
    });
    connect(dialog->editor(), &PaleoCoordinateEditor::mapPickRequested, this,
            [this, canvas, dialog](PaleoCoordinateEditor *editor) {
              if (m_sitingPickTool) m_sitingPickTool->deleteLater();
              auto *tool = new PaleoCoordinateMapTool(canvas);
              m_sitingPickTool = tool;
              const auto restore = [guard = QPointer<PaleoCoordinateToolsDialog>(dialog)] {
                if (guard) guard->dialog()->open();
              };
              connect(tool, &PaleoCoordinateMapTool::pointPicked, dialog,
                      [editor, restore](const auto &point) {
                        editor->showMapPoint(point);
                        restore();
                      });
              connect(tool, &PaleoCoordinateMapTool::cancelled, dialog, [restore] { restore(); });
              m_canvasCtl->setMapTool(tool->nativeTool());
              dialog->dialog()->hide();
              statusBar()->showMessage(tr("在地图上单击回填坐标；Esc 或右键取消。"), 8000);
            });
    dialog->dialog()->open();
  });
}
