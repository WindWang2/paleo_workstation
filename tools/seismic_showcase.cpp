#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QScreen>
#include <QSet>
#include <QSplitter>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <iostream>

#include "domain/seismic/nicestep.h"
#include "domain/seismic/sectionwellprojector.h"
#include "domain/seismic/seismiccolormap.h"
#include "domain/seismic/sgyindexcache.h"
#include "domain/seismic/sgyvolume.h"
#include "domain/seismic/timedepthmodel.h"
#include "io/lasparser.h"
#include "io/wellfileparsers.h"
#include "metadata/paleoprojectstore.h"
#include "services/paleotaskservice.h"
#include "services/seismictaskservice.h"
#include "ui/paleotheme.h"
#include "ui/seismic3d/seismic3dviewpanel.h"
#include "ui/seismic3d/seismic3dviewportwidget.h"
#include "ui/seismicsection/seismicsectioncanvas.h"
#include "ui/seismicsection/seismicsectiondockwidget.h"

using namespace seismic;

class SeismicShowcaseWindow : public QMainWindow
{
  Q_OBJECT

public:
  explicit SeismicShowcaseWindow(const QString &sgyPath, const QString &dataDir, QWidget *parent = nullptr)
    : QMainWindow(parent), m_sgyPath(sgyPath), m_dataDir(dataDir)
  {
    setupUi();
    loadData();
  }

  QTabWidget *tabWidget() const { return m_tabWidget; }
  Seismic3DViewPanel *view3dPanel() const { return m_panel3d; }
  SeismicSectionCanvas *sectionCanvas() const { return m_sectionDock ? m_sectionDock->canvas() : nullptr; }
  SeismicSectionCanvas *timeSliceCanvas() const { return m_timeSliceDock ? m_timeSliceDock->canvas() : nullptr; }
  SeismicSectionDockWidget *sectionDock() const { return m_sectionDock; }
  SeismicSectionDockWidget *timeSliceDock() const { return m_timeSliceDock; }

protected:
  void showEvent(QShowEvent *event) override
  {
    QMainWindow::showEvent(event);
    QTimer::singleShot(50, this, [this]() {
      if (m_sectionDock && m_sectionDock->canvas())
        m_sectionDock->canvas()->fitToWindow();
      if (m_timeSliceDock && m_timeSliceDock->canvas())
        m_timeSliceDock->canvas()->fitToWindow();
    });
  }

private:
  void setupUi()
  {
    setWindowTitle(tr("Paleo 地震工区三维体视口与二维剖面井震综合标定"));
    resize(1440, 920);

    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *rootLay = new QVBoxLayout(central);
    rootLay->setContentsMargins(0, 0, 0, 0);
    rootLay->setSpacing(0);

    // 顶部标题横条 (DESIGN.md 浅色专业规范)
    auto *header = new QWidget(central);
    header->setObjectName(QStringLiteral("showcaseHeader"));
    header->setFixedHeight(44);
    header->setStyleSheet(QStringLiteral(
        "#showcaseHeader { background: #FFFFFF; border-bottom: 1px solid #DFE5EC; }"));
    auto *headerLay = new QHBoxLayout(header);
    headerLay->setContentsMargins(16, 0, 16, 0);
    headerLay->setSpacing(12);

    auto *lblTitle = new QLabel(tr("三维地震解释与井震标定工作台"), header);
    lblTitle->setStyleSheet(QStringLiteral(
        "font-size: 11pt; font-weight: bold; color: #24303E;"));
    headerLay->addWidget(lblTitle);

    auto *chipVolume = new QLabel(tr("三维地震体: survey_3d.sgy (50×50×350)"), header);
    chipVolume->setStyleSheet(QStringLiteral(
        "background: #EDF1F5; color: #5D6E80; border-radius: 4px; padding: 3px 8px; font-size: 8.5pt; font-family: 'JetBrains Mono', monospace;"));
    headerLay->addWidget(chipVolume);

    auto *chipWell = new QLabel(tr("校准井: A1 (带分层与 GR 测井曲线)"), header);
    chipWell->setStyleSheet(QStringLiteral(
        "background: #E8F0FE; color: #1B73D0; font-weight: bold; border-radius: 4px; padding: 3px 8px; font-size: 8.5pt; font-family: 'JetBrains Mono', monospace;"));
    headerLay->addWidget(chipWell);

    headerLay->addStretch(1);

    auto *lblHint = new QLabel(tr("三维视口支持按住左键旋转、右键平移、滚轮缩放；二维剖面支持时深双单位切换与测井叠加"), header);
    lblHint->setStyleSheet(QStringLiteral("color: #5D6E80; font-size: 8.5pt;"));
    headerLay->addWidget(lblHint);

    rootLay->addWidget(header);

    // 主 Tab 视口
    m_tabWidget = new QTabWidget(central);
    m_tabWidget->setObjectName(QStringLiteral("showcaseTabs"));
    m_tabWidget->setStyleSheet(QStringLiteral(
        "QTabWidget::pane { border: none; background: #EDF1F5; }"
        "QTabBar::tab { background: #FFFFFF; color: #5D6E80; padding: 8px 20px; font-size: 9pt; font-weight: 500; border: none; border-bottom: 2px solid transparent; }"
        "QTabBar::tab:selected { color: #1B73D0; border-bottom: 2px solid #1B73D0; font-weight: bold; }"
        "QTabBar::tab:hover:!selected { color: #24303E; background: #F8FAFC; }"));

    // Tab 1: 三维地震体视口
    m_panel3d = new Seismic3DViewPanel(m_tabWidget);
    m_panel3d->setObjectName(QStringLiteral("panel3d"));
    m_tabWidget->addTab(m_panel3d, tr("三维地震体视口 (3D Seismic Viewport)"));

    // Tab 2: 二维剖面与井震标定
    auto *tab2Container = new QWidget(m_tabWidget);
    auto *tab2Lay = new QVBoxLayout(tab2Container);
    tab2Lay->setContentsMargins(0, 0, 0, 0);
    tab2Lay->setSpacing(0);

    m_sectionDock = new SeismicSectionDockWidget(tr("二维地震展开剖面与井震标定"), tab2Container);
    m_sectionDock->setObjectName(QStringLiteral("sectionDock"));
    tab2Lay->addWidget(m_sectionDock);
    m_tabWidget->addTab(tab2Container, tr("二维纵/横测线剖面 (2D Section)"));

    // Tab 3: 水平时间切片剖面 (Time Slice)
    auto *tab3Container = new QWidget(m_tabWidget);
    auto *tab3Lay = new QVBoxLayout(tab3Container);
    tab3Lay->setContentsMargins(0, 0, 0, 0);
    tab3Lay->setSpacing(0);

    m_timeSliceDock = new SeismicSectionDockWidget(tr("水平时间切片剖面 (Time Slice)"), tab3Container);
    m_timeSliceDock->setObjectName(QStringLiteral("timeSliceDock"));
    tab3Lay->addWidget(m_timeSliceDock);
    m_tabWidget->addTab(tab3Container, tr("水平时间切片剖面 (Time Slice)"));

    connect(m_tabWidget, &QTabWidget::currentChanged, this, [this](int idx) {
      if (idx == 1 && m_sectionDock && m_sectionDock->canvas()) {
        QTimer::singleShot(20, this, [this]() {
          m_sectionDock->canvas()->fitToWindow();
        });
      } else if (idx == 2 && m_timeSliceDock && m_timeSliceDock->canvas()) {
        QTimer::singleShot(20, this, [this]() {
          m_timeSliceDock->canvas()->fitToWindow();
        });
      }
    });

    rootLay->addWidget(m_tabWidget, 1);
  }

  void loadData()
  {
    // 1. 初始化任务服务与体数据
    m_store = std::make_unique<PaleoProjectStore>();
    m_taskService = std::make_unique<PaleoTaskService>(m_store.get());
    m_seismicTaskService = std::make_unique<SeismicTaskService>(m_taskService.get(), 64 * 1024 * 1024);

    m_volume = std::make_shared<SgyVolume>();
    std::string err;
    if (!m_volume->Load(m_sgyPath.toStdString(), err))
    {
      std::cerr << "Failed to load SGY: " << err << std::endl;
      return;
    }

    // 2. 装配三维体视口
    m_panel3d->setTaskService(m_seismicTaskService.get());
    m_panel3d->setVolume(m_volume);
    m_panel3d->setInline(m_volume->InlineMin() + 25);
    m_panel3d->setCrossline(m_volume->XlineMin() + 25);
    m_panel3d->setTimeSample(180); // ~720ms
    m_panel3d->viewport()->setPresetView(SeismicCameraController::PresetView::Isometric);

    // 3. 关联体数据到二维剖面与时间切片
    m_sectionDock->setVolume(m_volume);
    m_sectionDock->canvas()->setColorMap(SectionColorMapType::RedWhiteBlue);
    m_sectionDock->canvas()->setGain(1.5f);
    m_sectionDock->canvas()->setContrast(1.2f);

    m_timeSliceDock->setSectionMode(2); // Time Slice mode
    m_timeSliceDock->setVolume(m_volume);
    m_timeSliceDock->canvas()->setColorMap(SectionColorMapType::RedWhiteBlue);
    m_timeSliceDock->canvas()->setGain(1.5f);
    m_timeSliceDock->canvas()->setContrast(1.2f);

    // 4. 解析井口、测井分层、时深关系表与 LAS 曲线
    loadWellA1Calibration();
  }

  void loadWellA1Calibration()
  {
    if (m_dataDir.isEmpty())
      return;

    // A. 解析 ExportWellHead.dat 获取 A1 井位
    QFile headFile(m_dataDir + QStringLiteral("/ExportWellHead.dat"));
    double wellX = 5288.67, wellY = 8219.94;
    double wellKb = 0.0, wellTd = 2160.0;
    if (headFile.open(QIODevice::ReadOnly))
    {
      const auto heads = parseWellHeadText(headFile.readAll());
      for (const auto &h : heads)
      {
        if (h.name == QStringLiteral("A1"))
        {
          wellX = h.x;
          wellY = h.y;
          wellKb = h.kb;
          wellTd = h.td;
          break;
        }
      }
      headFile.close();
    }

    // B. 解析 A1_TD.dat 装配 TimeDepthModel
    TimeDepthModel tdModel;
    QFile tdFile(m_dataDir + QStringLiteral("/A1_TD.dat"));
    if (tdFile.open(QIODevice::ReadOnly))
    {
      const auto tdTable = parseTimeDepthText(tdFile.readAll());
      std::vector<TdPoint> tdPoints;
      for (const auto &row : tdTable.rows)
      {
        if (row.hasTvd && row.timeMs > 0.0)
        {
          TdPoint pt;
          pt.depthM = row.tvd;
          pt.timeMs = row.timeMs;
          tdPoints.push_back(pt);
        }
      }
      if (!tdPoints.empty())
        tdModel.setPoints(tdPoints);
      tdFile.close();
    }
    m_sectionDock->canvas()->setTimeDepthModel(tdModel);

    // C. 解析 DC.dat 获取 A1 井关键地质分层（精简代表层，避免微亚段视觉重叠）
    const QSet<QString> keyTops = {
        QStringLiteral("X"), QStringLiteral("A"), QStringLiteral("B"),
        QStringLiteral("C1"), QStringLiteral("D21"), QStringLiteral("D61")
    };
    const QMap<QString, QColor> formColors = {
        {QStringLiteral("X"), QColor(QStringLiteral("#8E24AA"))},
        {QStringLiteral("A"), QColor(QStringLiteral("#3949AB"))},
        {QStringLiteral("B"), QColor(QStringLiteral("#00897B"))},
        {QStringLiteral("C1"), QColor(QStringLiteral("#43A047"))},
        {QStringLiteral("D21"), QColor(QStringLiteral("#FB8C00"))},
        {QStringLiteral("D61"), QColor(QStringLiteral("#E53935"))}
    };

    std::vector<WellTopItem> tops;
    QFile topFile(m_dataDir + QStringLiteral("/DC.dat"));
    if (topFile.open(QIODevice::ReadOnly))
    {
      const auto topRecords = parseWellTopsText(topFile.readAll());
      for (const auto &rec : topRecords)
      {
        if (rec.wellName == QStringLiteral("A1") && rec.hasTvd && keyTops.contains(rec.topName))
        {
          WellTopItem item;
          item.topName = rec.topName;
          item.tvd = rec.tvd;
          item.md = rec.hasMd ? rec.md : rec.tvd;
          item.twtMs = tdModel.DepthToTwtMs(rec.tvd);
          item.color = formColors.value(rec.topName, QColor(QStringLiteral("#1B73D0")));
          tops.push_back(item);
        }
      }
      topFile.close();
    }

    // D. 解析 A1.Las 获取 GR 伽马测井曲线
    std::vector<WellCurveItem> curves;
    QStringList curveNames;
    QList<LasCurve> lasCurves;
    QString lasErr;
    if (LasParser::parse(m_dataDir + QStringLiteral("/A1.Las"), curveNames, lasCurves, &lasErr))
    {
      const LasCurve *depthCurve = nullptr;
      const LasCurve *grCurve = nullptr;
      for (const auto &c : lasCurves)
      {
        if (c.name.compare(QStringLiteral("DEPT"), Qt::CaseInsensitive) == 0)
          depthCurve = &c;
        else if (c.name.compare(QStringLiteral("GR"), Qt::CaseInsensitive) == 0)
          grCurve = &c;
      }

      if (depthCurve && grCurve && depthCurve->values.size() == grCurve->values.size())
      {
        WellCurveItem grItem;
        grItem.curveName = QStringLiteral("GR");
        grItem.color = QColor(QStringLiteral("#2E7D32")); // 饱满地质测井绿
        grItem.minVal = 20.0f;
        grItem.maxVal = 140.0f;

        const int n = depthCurve->values.size();
        const int step = std::max(1, n / 400); // 降采样以获得流畅且清晰的曲线形态
        for (int i = 0; i < n; i += step)
        {
          const double d = depthCurve->values[i];
          const double v = grCurve->values[i];
          if (!std::isnan(d) && !std::isnan(v) && v > 0.0 && v < 300.0)
          {
            grItem.depthsM.push_back(d);
            grItem.twtMs.push_back(tdModel.DepthToTwtMs(d));
            grItem.values.push_back(static_cast<float>(v));
          }
        }
        curves.push_back(grItem);
      }
    }

    // E. 组装 SectionWellInfo 并投射到剖面中央
    SectionWellInfo wellInfo;
    wellInfo.wellId = QStringLiteral("A1");
    wellInfo.wellName = QStringLiteral("井 A1");
    wellInfo.surfaceX = wellX;
    wellInfo.surfaceY = wellY;
    wellInfo.totalDepth = wellTd;
    wellInfo.tops = tops;
    wellInfo.curves = curves;
    wellInfo.isWithinBuffer = true;
    wellInfo.offsetDistanceM = 0.0;
    wellInfo.tracePosition = 25.0; // 投影于剖面中央 (Trace 25)

    m_sectionDock->canvas()->setWells({ wellInfo });
    m_sectionDock->canvas()->setShowWells(true);
    m_sectionDock->canvas()->setShowFormationTops(true);
    m_sectionDock->canvas()->setShowWellCurves(true);

    m_timeSliceDock->canvas()->setTimeDepthModel(tdModel);
    m_timeSliceDock->canvas()->setWells({ wellInfo });
    m_timeSliceDock->canvas()->setShowWells(true);
    m_timeSliceDock->canvas()->setShowFormationTops(true);
  }

private:
  QString m_sgyPath;
  QString m_dataDir;
  std::unique_ptr<PaleoProjectStore> m_store;
  std::unique_ptr<PaleoTaskService> m_taskService;
  std::unique_ptr<SeismicTaskService> m_seismicTaskService;
  std::shared_ptr<SgyVolume> m_volume;

  QTabWidget *m_tabWidget = nullptr;
  Seismic3DViewPanel *m_panel3d = nullptr;
  SeismicSectionDockWidget *m_sectionDock = nullptr;
  SeismicSectionDockWidget *m_timeSliceDock = nullptr;
};

int main(int argc, char *argv[])
{
  // 必须在 QApplication 之前钉死 OpenGL 3.3 Core Profile 格式
  QSurfaceFormat fmt;
  fmt.setVersion(3, 3);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  QSurfaceFormat::setDefaultFormat(fmt);

  QApplication app(argc, argv);
  Q_INIT_RESOURCE(seismic_shaders);

  // 严格执行 DESIGN.md 浅色主题规范与字体系统
  PaleoTheme::applyLightTheme();

  QString sgyPath = QStringLiteral("testdata/project_area/survey_3d.sgy");
  QString dataDir = QStringLiteral("testdata/project_area");
  QString captureDir;
  bool autoCaptureAndExit = false;

  for (int i = 1; i < argc; ++i)
  {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == QStringLiteral("--sgy") && i + 1 < argc)
      sgyPath = QString::fromLocal8Bit(argv[++i]);
    else if (arg == QStringLiteral("--data-dir") && i + 1 < argc)
      dataDir = QString::fromLocal8Bit(argv[++i]);
    else if (arg == QStringLiteral("--capture-dir") && i + 1 < argc)
      captureDir = QString::fromLocal8Bit(argv[++i]);
    else if (arg == QStringLiteral("--auto-capture"))
      autoCaptureAndExit = true;
  }

  if (!QFile::exists(sgyPath))
  {
    sgyPath = QStringLiteral("testdata/project_area/mini_seismic.sgy");
  }

  SeismicShowcaseWindow window(sgyPath, dataDir);
  window.show();

  if (!captureDir.isEmpty() || autoCaptureAndExit)
  {
    // 等待视口与 OpenGL 贴图、着色器完全初始化并渲染
    QTimer::singleShot(1000, [&]() {
      // 1. 抓取 Tab 1 (三维视口)
      window.tabWidget()->setCurrentIndex(0);
      QApplication::processEvents();

      if (!captureDir.isEmpty())
      {
        QDir().mkpath(captureDir);
        const QString p3d = captureDir + QStringLiteral("/seismic_3d_viewport.png");
        QPixmap pm3d = window.view3dPanel()->grab();
        pm3d.save(p3d);
        std::cout << "Captured 3D Viewport: " << p3d.toStdString() << std::endl;
      }

      // 2. 抓取 Tab 2 (二维剖面展开与井震标定)
      QTimer::singleShot(500, [&]() {
        window.tabWidget()->setCurrentIndex(1);
        if (window.sectionCanvas())
          window.sectionCanvas()->fitToWindow();
        QApplication::processEvents();

        QTimer::singleShot(300, [&]() {
          if (!captureDir.isEmpty())
          {
            const QString p2d = captureDir + QStringLiteral("/seismic_2d_section.png");
            QPixmap pm2d = window.sectionCanvas()->grab();
            pm2d.save(p2d);
            std::cout << "Captured 2D Section: " << p2d.toStdString() << std::endl;
          }

          // 3. 抓取 Tab 2 (水平时间切片与井位标定)
          QTimer::singleShot(500, [&]() {
            window.tabWidget()->setCurrentIndex(2);
            if (window.timeSliceCanvas())
              window.timeSliceCanvas()->fitToWindow();
            QApplication::processEvents();

            QTimer::singleShot(300, [&]() {
              if (!captureDir.isEmpty())
              {
                const QString pts = captureDir + QStringLiteral("/seismic_time_slice.png");
                QPixmap pmts = window.timeSliceCanvas()->grab();
                pmts.save(pts);
                std::cout << "Captured Time Slice: " << pts.toStdString() << std::endl;

                const QString pAll = captureDir + QStringLiteral("/seismic_workstation_demo.png");
                QPixmap pmAll = window.grab();
                pmAll.save(pAll);
                std::cout << "Captured Full Window: " << pAll.toStdString() << std::endl;
              }

              if (autoCaptureAndExit)
              {
                std::cout << "Auto-capture complete, exiting demo." << std::endl;
                QTimer::singleShot(200, &app, &QCoreApplication::quit);
              }
            });
          });
        });
      });
    });
  }

  return app.exec();
}

#include "seismic_showcase.moc"
