// 层：视图
// paleomainwindow_status — 状态栏域（方向 83 拆 TU）：层位/坐标/比例尺/CRS/
// 底图来源读数 + 工程打开进度与取消钮。原 buildShell 的状态栏段按原文行序
// 移入（错误胶囊段在 paleomainwindow_errorhub.cpp 的 wireErrorHubStatus）。
#include "paleomainwindow.h"

#include "paleotheme.h" // T32：坐标/比例尺读数的 mono 数字面 token

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../linkage/selectioncontext.h"
#include "qgis/projectmapreference.h" // 坐标读数的经纬度换算 + 底图来源标注

#include <qgsmapcanvas.h>
#include <qgspointxy.h>

#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QLocale>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QToolButton>

namespace {
// 短暂的 QGIS 接管期拦住编辑输入，保留绘制、计时器与关闭事件。
// 不逐个禁用整窗控件，避免全树 EnabledChange 引发重排/重绘风暴。
class ProjectHandoffInputGuard final : public QObject {
public:
  explicit ProjectHandoffInputGuard(QWidget *window) : QObject(window), m_window(window) {}
  void setActive(bool active) { m_active = active; }
protected:
  bool eventFilter(QObject *receiver, QEvent *event) override {
    if (!m_active || !m_window) return false;
    switch (event->type()) {
    case QEvent::KeyPress: case QEvent::KeyRelease: case QEvent::ShortcutOverride:
    case QEvent::Shortcut:
    case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick: case QEvent::MouseMove: case QEvent::Wheel:
    case QEvent::TouchBegin: case QEvent::TouchUpdate: case QEvent::TouchEnd:
    case QEvent::TabletPress: case QEvent::TabletMove: case QEvent::TabletRelease:
    case QEvent::DragEnter: case QEvent::DragMove: case QEvent::Drop:
      break;
    default: return false;
    }
    auto *widget = qobject_cast<QWidget *>(receiver);
    if ((widget && (widget == m_window || m_window->isAncestorOf(widget))) ||
        (event->type() == QEvent::Shortcut && QApplication::activeWindow() == m_window)) {
      event->accept();
      return true;
    }
    return false;
  }
private:
  QPointer<QWidget> m_window;
  bool m_active = false;
};
}

// 状态栏族（原行序 883-1011；错误胶囊/ErrorHub 同步两段由 wireErrorHubStatus
// 在 horizon/coords 段之后、工程打开进度段之前接入——见该函数注记）。
void PaleoMainWindow::buildStatusBar()
{
  // ---- status bar: active horizon ----
  //（「数据提供器：N」常驻诊断标签已移除——provider 计数是启动期自检信息，
  // 不属于用户态状态栏；诊断仍可从日志/QgisRuntime 读。）
  auto *horizonLabel = new QLabel(this);
  horizonLabel->setObjectName(QStringLiteral("statusHorizon"));
  const auto horizonText = [](const QString &h) {
    return tr("层位：%1").arg(h.isEmpty() ? QStringLiteral("—") : h);
  };
  horizonLabel->setText(horizonText(m_selection ? m_selection->activeHorizon() : QString()));
  statusBar()->addPermanentWidget(horizonLabel);

  // Canvas-fed status readouts (the dedicated QGIS statusbar coordinate/scale
  // widgets are app-only in 4.2 — plain labels fed by canvas signals instead).
  if (m_canvasCtl)
  {
    QgsMapCanvas *cv = m_canvasCtl->canvas();
    auto *coordLabel = new QLabel(this);
    coordLabel->setObjectName(QStringLiteral("statusCoords"));
    coordLabel->setFont(PaleoTheme::monoFont()); // T32：坐标读数是数字面
    auto *scaleLabel = new QLabel(this);
    scaleLabel->setObjectName(QStringLiteral("statusScale"));
    scaleLabel->setFont(PaleoTheme::monoFont()); // T32：比例尺读数是数字面
    connect(cv, &QgsMapCanvas::xyCoordinates, this,
            [coordLabel, cv](const QgsPointXY &p) {
              // 固定 3 位小数（默认 arg(double) 只有 6 位有效数字，读数
              // 位数随量级跳动）；tnum 等宽数字面下宽度稳定。
              QgsPointXY geographic;
              if (cv->mapSettings().destinationCrs().type() != Qgis::CrsType::Engineering &&
                  paleo::mapreference::transformPoint(cv, p, cv->mapSettings().destinationCrs(),
                      QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326")), &geographic)) {
                coordLabel->setText(tr("经纬度 %1, %2").arg(QLocale().toString(geographic.x(), 'f', 6),
                                                            QLocale().toString(geographic.y(), 'f', 6)));
                return;
              }
              coordLabel->setText(QStringLiteral("%1, %2")
                                      .arg(QLocale().toString(p.x(), 'f', 3),
                                           QLocale().toString(p.y(), 'f', 3)));
            });
    auto updateScale = [scaleLabel, cv] {
      scaleLabel->setText(QStringLiteral("1:%1").arg(static_cast<qlonglong>(cv->scale())));
    };
    connect(cv, &QgsMapCanvas::scaleChanged, this, [updateScale](double) { updateScale(); });
    connect(cv, &QgsMapCanvas::extentsChanged, this, updateScale);
    updateScale();
    statusBar()->addPermanentWidget(coordLabel);
    statusBar()->addPermanentWidget(scaleLabel);

    // §4 预览壳：坐标读数旁标明坐标系——与 PDF 页脚（mapexport.cpp）同一句
    // 「工程坐标 · 米 · 未投影」（DESIGN.md 状态文字 #5D6E80，次级文案同色）。
    auto *crsLabel = new QLabel(this);
    crsLabel->setObjectName(QStringLiteral("statusCrs"));
    const auto updateCrs = [crsLabel, cv] {
      const auto crs = cv->mapSettings().destinationCrs();
      crsLabel->setText(crs.type() == Qgis::CrsType::Engineering ? tr("工程坐标 · 米 · 未投影")
          : crs.authid().isEmpty() ? crs.description() : crs.authid());
      crsLabel->setToolTip(crs.description());
    };
    updateCrs();
    connect(cv, &QgsMapCanvas::destinationCrsChanged, this, updateCrs);
    PaleoTheme::applyThemedStyleSheet(
        crsLabel, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    statusBar()->addPermanentWidget(crsLabel);
    auto *sources = new QLabel(this);
    sources->setObjectName(QStringLiteral("basemapAttribution"));
    PaleoTheme::applyThemedStyleSheet(sources, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    const auto updateSources = [sources, cv] {
      sources->setText(paleo::mapreference::attribution(cv));
      sources->setVisible(!sources->text().isEmpty());
    };
    connect(cv, &QgsMapCanvas::layersChanged, this, updateSources);
    updateSources();
    statusBar()->addWidget(sources, 1);
  }
  if (m_selection)
    connect(m_selection, &SelectionContext::activeHorizonChanged, horizonLabel,
            [horizonLabel, horizonText](const QString &h) { horizonLabel->setText(horizonText(h)); });

  // ---- 错误历史状态栏胶囊 + ErrorHub 计数同步（方向 64 呈现域 TU）----
  wireErrorHubStatus();

  if (m_projectSvc) {
    auto *openStatus = new QLabel(this);
    openStatus->setObjectName(QStringLiteral("projectOpenStatus"));
    auto *openProgress = new QProgressBar(this);
    openProgress->setObjectName(QStringLiteral("projectOpenProgress"));
    openProgress->setRange(0, 100);
    openProgress->setMaximumWidth(160);
    auto *cancelOpen = new QToolButton(this);
    cancelOpen->setObjectName(QStringLiteral("cancelProjectOpenButton"));
    cancelOpen->setText(tr("取消打开"));
    cancelOpen->setAutoRaise(true);
    statusBar()->addPermanentWidget(openStatus);
    statusBar()->addPermanentWidget(openProgress);
    statusBar()->addPermanentWidget(cancelOpen);
    openStatus->hide(); openProgress->hide(); cancelOpen->hide();
    auto *handoffGuard = new ProjectHandoffInputGuard(this);
    qApp->installEventFilter(handoffGuard);
    connect(cancelOpen, &QToolButton::clicked, m_projectSvc, &QgisProjectService::cancelOpen);
    connect(m_projectSvc, &QgisProjectService::openActiveChanged, this,
            [this, openStatus, openProgress, cancelOpen, handoffGuard](bool active) {
      if (!active) handoffGuard->setActive(false);
      openStatus->setVisible(active); openProgress->setVisible(active); cancelOpen->setVisible(active);
      if (active) openProgress->setValue(0);
      for (const auto &name : {"openProjectButton", "newProjectButton", "importFromFolderButton"})
        if (auto *button = findChild<QPushButton *>(QString::fromLatin1(name)))
          button->setEnabled(!active);
    });
    connect(m_projectSvc, &QgisProjectService::openProgress, this,
            [openStatus, openProgress, cancelOpen, handoffGuard](int percent, const QString &status) {
      openProgress->setValue(percent);
      openStatus->setText(status);
      openStatus->setToolTip(status);
      cancelOpen->setEnabled(percent < 85);
      handoffGuard->setActive(percent >= 85);
    });
    connect(m_projectSvc, &QgisProjectService::openFinished, this, [this](bool success) {
      statusBar()->showMessage(success ? tr("工程打开完成") : m_projectSvc->lastOpenCancelled()
          ? tr("已取消打开工程") : tr("工程打开失败"), 5000);
    });
  }
}
