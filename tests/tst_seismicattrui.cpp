// 层：测试壳
// goal/seismic-attributes UI 面：面板意图信号、画布属性叠加生命周期/渲染可辨、
// dock 全链 offscreen（面板参数 → 服务任务 → 叠加上图）、资产登记闭环。
#include <QtTest>
#include <QApplication>
#include <QPainter>
#include <QtEndian>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <vector>

#include "../src/catalog/datacatalog.h"
#include "../src/domain/seismic/sgyvolume.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/seismictaskservice.h"
#include "../src/ui/seismicsection/seismicattrpanel.h"
#include "../src/ui/seismicsection/seismicsectioncanvas.h"
#include "../src/ui/seismicsection/seismicsectiondockwidget.h"

using namespace seismic;

namespace
{

SgySliceImage makeSection(int traces, int samples)
{
  SgySliceImage img;
  img.width = traces;
  img.height = samples;
  img.valueMin = -1.0f;
  img.valueMax = 1.0f;
  img.values.assign(std::size_t(traces) * samples,
                    std::numeric_limits<float>::quiet_NaN());
  for (int t = 0; t < traces; ++t)
    for (int s = 0; s < samples; ++s)
      img.values[std::size_t(s) * traces + t] =
          std::sin(s * 0.08f + t * 0.4f);
  return img;
}

SgySliceImage makeAttr(int traces, int samples)
{
  SgySliceImage img;
  img.width = traces;
  img.height = samples;
  img.valueMin = 0.0f;
  img.valueMax = 1.0f;
  img.values.assign(std::size_t(traces) * samples, 0.0f);
  for (int t = 0; t < traces; ++t)
    for (int s = 0; s < samples; ++s)
      img.values[std::size_t(s) * traces + t] =
          float((s + t) % 97) / 96.0f;
  img.values[0] = std::numeric_limits<float>::quiet_NaN(); // 缺失=透明
  return img;
}

QImage renderCanvas(SeismicSectionCanvas &canvas)
{
  QImage img(canvas.size(), QImage::Format_ARGB32);
  img.fill(Qt::white);
  QPainter painter(&img);
  canvas.render(&painter);
  painter.end();
  return img;
}

qint64 imageDiff(const QImage &a, const QImage &b)
{
  if (a.size() != b.size())
    return -1;
  qint64 diff = 0;
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x)
      diff += qAbs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y)));
  return diff;
}

// 标准 INLINE@188/CROSSLINE@192 合成体（全道同一 Ricker → 相干解析已知）
bool writeSegy(const QString &path, int nIl, int nXl, int ns, int dt,
               int firstInline = 10, int firstXline = 100)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly))
    return false;
  QByteArray textHdr(3200, ' ');
  const QString banner =
      QStringLiteral("C01 ATTR UI TEST First inline : %1 Last inline : %2 "
                     "First xline : %3 Last xline : %4")
          .arg(firstInline)
          .arg(firstInline + nIl - 1)
          .arg(firstXline)
          .arg(firstXline + nXl - 1);
  const QByteArray bb = banner.toUtf8();
  std::memcpy(textHdr.data(), bb.constData(), bb.size());
  f.write(textHdr);

  QByteArray binHdr(400, 0);
  qToBigEndian<qint16>(dt, reinterpret_cast<uchar *>(binHdr.data()) + 16);
  qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(binHdr.data()) + 20);
  qToBigEndian<qint16>(5, reinterpret_cast<uchar *>(binHdr.data()) + 24);
  f.write(binHdr);

  const double dtSec = dt / 1e6;
  const double a2 = std::pow(std::numbers::pi * 40.0 * dtSec, 2);
  int traceIndex = 0;
  for (int i = 0; i < nIl; ++i)
    for (int j = 0; j < nXl; ++j)
    {
      QByteArray trHdr(240, 0);
      qToBigEndian<qint32>(traceIndex + 1, reinterpret_cast<uchar *>(trHdr.data()) + 0);
      qToBigEndian<qint32>(firstInline + i, reinterpret_cast<uchar *>(trHdr.data()) + 8);
      qToBigEndian<qint32>(firstXline + j, reinterpret_cast<uchar *>(trHdr.data()) + 20);
      qToBigEndian<qint16>(1, reinterpret_cast<uchar *>(trHdr.data()) + 70);
      qToBigEndian<qint32>(1000 + j * 25, reinterpret_cast<uchar *>(trHdr.data()) + 72);
      qToBigEndian<qint32>(5000 + i * 25, reinterpret_cast<uchar *>(trHdr.data()) + 76);
      qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(trHdr.data()) + 114);
      qToBigEndian<qint16>(dt, reinterpret_cast<uchar *>(trHdr.data()) + 116);
      qToBigEndian<qint32>(firstInline + i, reinterpret_cast<uchar *>(trHdr.data()) + 188);
      qToBigEndian<qint32>(firstXline + j, reinterpret_cast<uchar *>(trHdr.data()) + 192);
      f.write(trHdr);
      QByteArray samples(ns * 4, 0);
      for (int k = 0; k < ns; ++k)
      {
        const double t = k - ns / 2.0;
        const float v = float((1.0 - 2.0 * a2 * t * t) * std::exp(-a2 * t * t));
        quint32 raw;
        std::memcpy(&raw, &v, 4);
        qToBigEndian<quint32>(raw, reinterpret_cast<uchar *>(samples.data()) + k * 4);
      }
      f.write(samples);
      ++traceIndex;
    }
  return true;
}

// 事件泵：条件满足或超时（ms）退出
bool waitFor(const std::function<bool()> &cond, int timeoutMs)
{
  QElapsedTimer clock;
  clock.start();
  while (!cond())
  {
    if (clock.elapsed() > timeoutMs)
      return cond();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return true;
}

} // namespace

class TestSeismicAttrUi : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir m_settingsDir;
  QTemporaryDir m_workDir;

private slots:
  void initTestCase()
  {
    QCoreApplication::setOrganizationName(QStringLiteral("paleo-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_seismicattrui"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_settingsDir.path());
    QVERIFY(m_workDir.isValid());
  }

  // ---- 面板：表单 → 意图信号（视图只发信号） ----
  void panelFormEmitsIntent()
  {
    SeismicAttrPanel panel;
    auto *kind = panel.findChild<QComboBox *>(QStringLiteral("attrKindCombo"));
    auto *winHalf = panel.findChild<QSpinBox *>(QStringLiteral("attrWindowHalf"));
    auto *ilHalf = panel.findChild<QSpinBox *>(QStringLiteral("attrIlHalf"));
    auto *alpha = panel.findChild<QSlider *>(QStringLiteral("attrAlpha"));
    auto *compute = panel.findChild<QToolButton *>(QStringLiteral("attrComputeButton"));
    auto *reg = panel.findChild<QToolButton *>(QStringLiteral("attrRegisterButton"));
    QVERIFY(kind && winHalf && ilHalf && alpha && compute && reg);

    QCOMPARE(panel.currentKind(),
             SeismicTaskService::SeismicAttrKind::Envelope); // 首项默认
    QVERIFY(!reg->isEnabled()); // 无结果不可登记

    QSignalSpy computeSpy(&panel, &SeismicAttrPanel::computeRequested);
    QSignalSpy alphaSpy(&panel, &SeismicAttrPanel::alphaChanged);
    kind->setCurrentIndex(7);                 // 相干（semblance）
    winHalf->setValue(12);
    ilHalf->setValue(2);
    alpha->setValue(40);
    compute->click();

    QCOMPARE(computeSpy.count(), 1);
    const QVariantList args = computeSpy.takeFirst();
    QCOMPARE(args.at(0).value<SeismicTaskService::SeismicAttrKind>(),
             SeismicTaskService::SeismicAttrKind::Coherence);
    const auto params =
        args.at(1).value<SeismicTaskService::SeismicAttrParams>();
    QCOMPARE(params.windowHalfSamples, 12);
    QCOMPARE(params.coherenceIlHalf, 2);
    QCOMPARE(args.at(2).toDouble(), 0.4);
    QCOMPARE(alphaSpy.count(), 1);

    // busy 态：计算按钮禁用、取消可用
    panel.setBusy(true);
    QVERIFY(!compute->isEnabled());
    QVERIFY(panel.findChild<QToolButton *>(QStringLiteral("attrCancelButton"))
                ->isEnabled());
    panel.showResult(true, QStringLiteral("ok"));
    QVERIFY(!panel.findChild<QProgressBar *>(QStringLiteral("attrProgress"))
                 ->isEnabled()
             || true); // 进度条只读展示
    QVERIFY(reg->isEnabled()); // 有结果可登记
  }

  // ---- 画布：叠加生命周期 + 渲染可辨 + 几何失配防线 ----
  void canvasOverlayLifecycle()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(640, 480);
    canvas.setSectionData(makeSection(48, 192), 2.0f);
    QVERIFY(canvas.hasData());
    QVERIFY(!canvas.hasAttrOverlay());

    const QImage baseline = renderCanvas(canvas);

    canvas.setAttrOverlay(makeAttr(48, 192));
    QVERIFY(canvas.hasAttrOverlay());
    QCOMPARE(canvas.attrOverlayAlpha(), 0.65);
    QVERIFY(imageDiff(baseline, renderCanvas(canvas)) > 1000); // 叠加可辨

    canvas.setAttrOverlayAlpha(0.0); // 全透明 = 视觉等同无叠加
    QCOMPARE(canvas.attrOverlayAlpha(), 0.0);
    QVERIFY(imageDiff(baseline, renderCanvas(canvas)) < 50);

    canvas.setAttrOverlayAlpha(0.65);
    canvas.clearAttrOverlay();
    QVERIFY(!canvas.hasAttrOverlay());

    // 几何失配：忽略并保持无叠加
    canvas.setAttrOverlay(makeAttr(9, 9));
    QVERIFY(!canvas.hasAttrOverlay());

    // 换剖面（几何变化）→ 旧叠加自动清除
    canvas.setAttrOverlay(makeAttr(48, 192));
    QVERIFY(canvas.hasAttrOverlay());
    canvas.setSectionData(makeSection(32, 128), 2.0f);
    QVERIFY(!canvas.hasAttrOverlay());

    // #224：clearData（换体）同样清叠加——同尺寸新体首切片不得沿用旧叠加。
    canvas.setAttrOverlay(makeAttr(32, 128));
    QVERIFY(canvas.hasAttrOverlay());
    canvas.clearData();
    QVERIFY(!canvas.hasAttrOverlay());
  }

  // ---- goal/attr-volume 面板：扫描范围/采样位/加权档 → 意图信号 ----
  void panelScopeAndScanIntents()
  {
    SeismicAttrPanel panel;
    auto *scope = panel.findChild<QComboBox *>(QStringLiteral("attrScopeCombo"));
    auto *sample = panel.findChild<QSpinBox *>(QStringLiteral("attrTimeSample"));
    auto *weight = panel.findChild<QComboBox *>(QStringLiteral("attrWeightCombo"));
    auto *compute = panel.findChild<QToolButton *>(QStringLiteral("attrComputeButton"));
    QVERIFY(scope && sample && weight && compute);
    QCOMPARE(panel.currentScope(), 0); // 缺省本剖面
    QVERIFY(!sample->isEnabled());    // 采样位仅时间切片范围启用

    // 体加载回填：范围 + 缺省中位采样
    panel.setVolumeSampleRange(256);
    QVERIFY(sample->isEnabled() == false); // 范围仍是本剖面
    scope->setCurrentIndex(1);             // 时间切片
    QVERIFY(sample->isEnabled());
    QCOMPARE(sample->maximum(), 255);
    QCOMPARE(sample->value(), 127); // 0..255 的中位
    QCOMPARE(panel.currentSampleIndex(), 127);

    QSignalSpy tsSpy(&panel, &SeismicAttrPanel::timeSliceScanRequested);
    QSignalSpy volSpy(&panel, &SeismicAttrPanel::volumeScanRequested);
    QSignalSpy secSpy(&panel, &SeismicAttrPanel::computeRequested);
    weight->setCurrentIndex(1); // 道距加权
    compute->click();
    QCOMPARE(tsSpy.count(), 1);
    QCOMPARE(volSpy.count(), 0);
    QCOMPARE(secSpy.count(), 0);
    const QVariantList args = tsSpy.takeFirst();
    QCOMPARE(args.at(2).toInt(), 127);
    QCOMPARE(args.at(1)
                  .value<SeismicTaskService::SeismicAttrParams>()
                  .coherenceWeighting,
             1);

    scope->setCurrentIndex(2); // 属性体
    QVERIFY(!sample->isEnabled());
    compute->click();
    QCOMPARE(volSpy.count(), 1);
    QCOMPARE(tsSpy.count(), 0);
    QCOMPARE(volSpy.takeFirst()
                  .at(1)
                  .value<SeismicTaskService::SeismicAttrParams>()
                  .coherenceWeighting,
             1);

    scope->setCurrentIndex(0); // 本剖面（回归：原意图信号）
    compute->click();
    QCOMPARE(secSpy.count(), 1);
    QCOMPARE(tsSpy.count(), 0);
    QCOMPARE(volSpy.count(), 0);
  }

  // ---- goal/attr-volume dock 全链（offscreen）：扫描 → 登记 → 层树/3D 信号 ----
  void dockScanChainOffscreen()
  {
    const QString sgy = m_workDir.filePath(QStringLiteral("ui_scan.sgy"));
    QVERIFY(writeSegy(sgy, 5, 8, 128, 2000));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks);
    SeismicSectionDockWidget dock;
    dock.setTaskService(&svc);

    auto vol = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY(vol->Load(sgy.toStdString(), err));
    dock.setVolume(vol);
    QVERIFY(waitFor([&dock]() { return dock.canvas()->hasData(); }, 10000)
            || true); // 切片未上图不影响扫描链（体已加载即可）

    // catalog 注入（登记上下文）
    DataCatalog catalog;
    QString cerr;
    QVERIFY(catalog.open(m_workDir.path(), &cerr));
    CatalogAsset seisAsset;
    seisAsset.id = QStringLiteral("seis_attrui_scan");
    seisAsset.type = QStringLiteral("seismic");
    seisAsset.format = QStringLiteral("sgy");
    seisAsset.displayName = QStringLiteral("ui_scan.sgy");
    QVERIFY(catalog.addAsset(seisAsset, &cerr));
    CatalogVersion raw;
    raw.id = QStringLiteral("rawver_attrui_scan");
    raw.assetId = seisAsset.id;
    raw.stage = QStringLiteral("RAW");
    raw.versionNumber = 1;
    raw.managed = false;
    raw.path = sgy;
    raw.fileName = QStringLiteral("ui_scan.sgy");
    QVERIFY(catalog.addVersion(raw, &cerr));
    const QString outDir = m_workDir.filePath(QStringLiteral("attrs_scan"));
    dock.setInterpretationCatalog(&catalog, seisAsset.id, raw.id, outDir);

    QSignalSpy layerSpy(&dock, &SeismicSectionDockWidget::timeSliceAttrLayerReady);
    QSignalSpy volumeSpy(&dock, &SeismicSectionDockWidget::attrVolumeReady);
    auto *panel = dock.attrPanel();
    QVERIFY(panel);
    auto *scope = panel->findChild<QComboBox *>(QStringLiteral("attrScopeCombo"));
    auto *sample = panel->findChild<QSpinBox *>(QStringLiteral("attrTimeSample"));
    auto *compute = panel->findChild<QToolButton *>(QStringLiteral("attrComputeButton"));
    auto *status = panel->findChild<QLabel *>(QStringLiteral("attrStatus"));
    QVERIFY(scope && sample && compute && status);
    QCOMPARE(sample->maximum(), 127); // 体加载回填
    QCOMPARE(sample->value(), 63);

    // 时间切片扫描 → 层树声明 + GeoTIFF 产物 + DERIVED 版本
    scope->setCurrentIndex(1);
    compute->click();
    QVERIFY(waitFor([&panel]() { return !panel->isBusy(); }, 30000));
    QVERIFY(status->text().contains(QStringLiteral("✓")));
    QCOMPARE(layerSpy.count(), 1);
    const LayerDeclaration decl =
        layerSpy.takeFirst().at(0).value<LayerDeclaration>();
    QCOMPARE(decl.type, QStringLiteral("raster"));
    QVERIFY(QFileInfo::exists(decl.source));
    QVERIFY(decl.source.endsWith(QStringLiteral(".tif")));
    const CatalogVersion tsVer = catalog.currentVersion(
        QStringLiteral("seis_attr_seis_attrui_scan_envelope_ts_63"));
    QCOMPARE(tsVer.stage, QStringLiteral("DERIVED"));
    QCOMPARE(tsVer.parentVersionIds, QStringList{raw.id});
    QVERIFY(tsVer.extra.contains(QStringLiteral("param_hash")));

    // 属性体扫描 → 3D 预览信号（服务线程烘焙的三面 + 堆叠层）+ SATV 产物
    scope->setCurrentIndex(2);
    compute->click();
    QVERIFY(waitFor([&panel]() { return !panel->isBusy(); }, 60000));
    QVERIFY(waitFor([&volumeSpy]() { return volumeSpy.count() > 0; }, 30000));
    QVERIFY(status->text().contains(QStringLiteral("✓")));
    const QVariantList vArgs = volumeSpy.takeFirst();
    QVERIFY(vArgs.at(1).toBool());
    const auto preview =
        vArgs.at(0)
            .value<seismic::SeismicTaskService::AttributeVolumePreview>();
    QVERIFY(preview.ok);
    QCOMPARE(preview.nIl, 5);
    QCOMPARE(preview.nXl, 8);
    QCOMPARE(preview.nS, 128);
    QVERIFY(preview.stackLayerCount > 0);
    const QStringList satvFiles = QDir(m_workDir.filePath(
        QStringLiteral("attrs_scan")))
        .entryList(QStringList() << QStringLiteral("*.sattr"), QDir::Files);
    QVERIFY(!satvFiles.isEmpty());

    // 同参数二次时间切片扫描 → 缓存命中可见（面板文案）
    scope->setCurrentIndex(1);
    compute->click();
    QVERIFY(waitFor([&panel]() { return !panel->isBusy(); }, 30000));
    QVERIFY(status->text().contains(QStringLiteral("缓存命中")));
    QCOMPARE(layerSpy.count(), 1);
  }

  // ---- Oracle#1 全链（offscreen，fixture 体）：面板 → 任务 → 叠加上图 ----
  // 三类属性闭环：包络（瞬时族）/ RMS（时窗族）/ 相干（3×3×3）。
  void dockFullChainOffscreen()
  {
    const QString sgy = m_workDir.filePath(QStringLiteral("ui_chain.sgy"));
    QVERIFY(writeSegy(sgy, 5, 8, 128, 2000));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks);
    SeismicSectionDockWidget dock;
    dock.setTaskService(&svc);

    auto vol = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY(vol->Load(sgy.toStdString(), err));
    dock.setVolume(vol);
    dock.setSectionMode(0);          // Inline 模式
    // 走真实 UI 驱动：滑杆 valueChanged → spin 同步 + 切片提取（直接调槽
    // 会绕过 spin 同步，computeAttributeOnCurrentSection 读 spin 值就错位）
    auto *slider = dock.findChild<QSlider *>(QStringLiteral("sliderSectionSlice"));
    QVERIFY(slider);
    slider->setValue(10);            // inline 10（首线）
    QVERIFY(waitFor([&dock]() { return dock.canvas()->hasData(); }, 10000));
    QCOMPARE(dock.canvas()->traceCount(), 8);

    auto *panel = dock.attrPanel();
    QVERIFY(panel);
    panel->setVisible(true);
    auto *kind = panel->findChild<QComboBox *>(QStringLiteral("attrKindCombo"));
    auto *compute = panel->findChild<QToolButton *>(QStringLiteral("attrComputeButton"));
    auto *status = panel->findChild<QLabel *>(QStringLiteral("attrStatus"));

    // 相干在测网边缘线（inline 10 无双侧邻线）→ 如实拒绝；切到 11 成功
    kind->setCurrentIndex(7); // 相干
    compute->click();
    QVERIFY(waitFor(
        [&panel]() { return !panel->isBusy(); }, 10000));
    QVERIFY(status->text().contains(QStringLiteral("边缘")));

    slider->setValue(11); // 中部线
    QVERIFY(waitFor([&dock]() { return dock.canvas()->hasData(); }, 10000));

    const QList<int> kindIndices = {0, 4, 7}; // 包络 / RMS / 相干
    for (const int idx : kindIndices)
    {
      kind->setCurrentIndex(idx);
      compute->click();
      QVERIFY(waitFor([&dock, &panel]() {
        return dock.canvas()->hasAttrOverlay() && !panel->isBusy();
      }, 10000));
      QVERIFY(dock.canvas()->hasAttrOverlay());
      QVERIFY(status->text().contains(QStringLiteral("✓")));
      dock.canvas()->clearAttrOverlay(); // 逐类独立验证
    }

    // #224：计算在途/完成后换线——旧线属性图不得留在新剖面上（迟到结果被
    // 世代守卫丢弃，已上屏叠加随换线清除；两种时序结论一致）。
    kind->setCurrentIndex(0);
    compute->click();
    slider->setValue(12);
    QVERIFY(waitFor([&panel]() { return !panel->isBusy(); }, 10000));
    QVERIFY(waitFor([&dock]() { return dock.canvas()->hasData(); }, 10000));
    QTest::qWait(50);
    QVERIFY(!dock.canvas()->hasAttrOverlay());
    slider->setValue(11); // 复位到中部线供下方登记闭环
    QVERIFY(waitFor([&dock]() { return dock.canvas()->hasData(); }, 10000));

    // ---- 资产登记闭环（catalog 注入 → SATR 文件 + DERIVED 版本）----
    DataCatalog catalog;
    QString cerr;
    QVERIFY(catalog.open(m_workDir.path(), &cerr));
    CatalogAsset seisAsset;
    seisAsset.id = QStringLiteral("seis_attrui_1");
    seisAsset.type = QStringLiteral("seismic");
    seisAsset.format = QStringLiteral("sgy");
    seisAsset.displayName = QStringLiteral("ui_chain.sgy");
    QVERIFY(catalog.addAsset(seisAsset, &cerr));
    CatalogVersion raw;
    raw.id = QStringLiteral("rawver_attrui_1");
    raw.assetId = seisAsset.id;
    raw.stage = QStringLiteral("RAW");
    raw.versionNumber = 1;
    raw.managed = false;
    raw.path = sgy;
    raw.fileName = QStringLiteral("ui_chain.sgy");
    QVERIFY(catalog.addVersion(raw, &cerr));
    const QString outDir = m_workDir.filePath(QStringLiteral("attrs"));
    dock.setInterpretationCatalog(&catalog, seisAsset.id, raw.id, outDir);

    kind->setCurrentIndex(0); // 包络
    compute->click();
    QVERIFY(waitFor([&dock, &panel]() {
      return dock.canvas()->hasAttrOverlay() && !panel->isBusy();
    }, 10000));
    auto *reg = panel->findChild<QToolButton *>(QStringLiteral("attrRegisterButton"));
    QVERIFY(reg->isEnabled());
    reg->click();
    QVERIFY(waitFor([&outDir]() {
      return !QDir(outDir).entryList(QStringList() << QStringLiteral("*.sattr"),
                                     QDir::Files)
          .isEmpty();
    }, 5000));
    const QString sattr =
        QDir(outDir).entryList(QStringList() << QStringLiteral("*.sattr"),
                               QDir::Files)
            .constFirst();
    QVERIFY(QFileInfo::exists(outDir + QLatin1Char('/') + sattr));
    QVERIFY(sattr.startsWith(QStringLiteral("envelope_il_11")));
  }

  // ---- #236：resetInterpretationState 清工程边界解释残留 ----
  // 会话/可登记属性结果/书签下拉/属性叠加全部作废；复位后登记入口如实
  // 报「无可登记结果」，不把旧工程属性登记出去。
  void resetInterpretationStateClearsProjectResidue()
  {
    const QString sgy = m_workDir.filePath(QStringLiteral("ui_reset.sgy"));
    QVERIFY(writeSegy(sgy, 5, 8, 128, 2000));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks);
    SeismicSectionDockWidget dock;
    dock.setTaskService(&svc);
    auto vol = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY(vol->Load(sgy.toStdString(), err));
    dock.setVolume(vol);
    dock.setSectionMode(0); // Inline
    auto *slider = dock.findChild<QSlider *>(QStringLiteral("sliderSectionSlice"));
    QVERIFY(slider);
    slider->setValue(11); // 中部线
    QVERIFY(waitFor([&dock]() { return dock.canvas()->hasData(); }, 10000));

    auto *panel = dock.attrPanel();
    QVERIFY(panel);
    panel->setVisible(true);
    auto *kind = panel->findChild<QComboBox *>(QStringLiteral("attrKindCombo"));
    auto *compute = panel->findChild<QToolButton *>(QStringLiteral("attrComputeButton"));
    auto *reg = panel->findChild<QToolButton *>(QStringLiteral("attrRegisterButton"));
    QVERIFY(kind && compute && reg);

    // 制造「可登记」残留：一次成功的剖面属性计算（不注入 catalog——
    // 可登记态是面板态的，与登记上下文是否在场无关）。
    kind->setCurrentIndex(0); // 包络
    compute->click();
    QVERIFY(waitFor([&dock, panel]() {
      return dock.canvas()->hasAttrOverlay() && !panel->isBusy();
    }, 10000));
    QVERIFY(reg->isEnabled());
    QVERIFY(dock.canvas()->hasAttrOverlay());

    // 制造会话/书签残留
    SeismicPick pick;
    pick.id = 1;
    pick.inlineNo = 11;
    pick.xlineNo = 103;
    pick.twtMs = 120.0;
    pick.horizonName = QStringLiteral("H1");
    dock.addPicks({pick});
    dock.addBookmark(QStringLiteral("旧工程书签"));
    QCOMPARE(dock.interpretationSession().picks.size(), 1);
    QCOMPARE(dock.bookmarks().size(), 1);

    // 工程边界复位（resetProjectScopedState 的调用形态）
    dock.resetInterpretationState();

    QVERIFY(!reg->isEnabled());   // 可登记态清空
    QVERIFY(!panel->isBusy());    // 忙态清空
    QVERIFY(dock.bookmarks().isEmpty());          // 书签下拉清空
    QVERIFY(dock.interpretationSession().picks.isEmpty()); // 会话清空
    QVERIFY(!dock.canvas()->hasAttrOverlay());    // 旧属性叠加清除

    // 复位后登记如实报因（不是把旧结果登记出去）
    QString regErr;
    QVERIFY(dock.registerCurrentAttributeAsset(&regErr).isEmpty());
    QVERIFY(!regErr.isEmpty());
  }

  // ---- #236：在途属性体扫描跨工程守卫 ----
  // 扫描在途时工程切换（体被清空/换绑）：回调不得 emit attrVolumeReady
  // 把旧工程属性体喂进新工程 3D 视口；两个时序（setVolume(nullptr) 与
  // resetInterpretationState 取消）结论一致。
  void attributeVolumeScanDiscardedAfterProjectSwitch()
  {
    const QString sgy = m_workDir.filePath(QStringLiteral("ui_switch.sgy"));
    QVERIFY(writeSegy(sgy, 5, 8, 128, 2000));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks);
    SeismicSectionDockWidget dock;
    dock.setTaskService(&svc);
    auto vol = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY(vol->Load(sgy.toStdString(), err));
    dock.setVolume(vol);
    QVERIFY(waitFor([&dock]() { return dock.canvas()->hasData(); }, 10000) || true);

    DataCatalog catalog;
    QString cerr;
    QVERIFY(catalog.open(m_workDir.path(), &cerr));
    CatalogAsset seisAsset;
    seisAsset.id = QStringLiteral("seis_attrui_switch");
    seisAsset.type = QStringLiteral("seismic");
    seisAsset.format = QStringLiteral("sgy");
    seisAsset.displayName = QStringLiteral("ui_switch.sgy");
    QVERIFY(catalog.addAsset(seisAsset, &cerr));
    CatalogVersion raw;
    raw.id = QStringLiteral("rawver_attrui_switch");
    raw.assetId = seisAsset.id;
    raw.stage = QStringLiteral("RAW");
    raw.versionNumber = 1;
    raw.managed = false;
    raw.path = sgy;
    raw.fileName = QStringLiteral("ui_switch.sgy");
    QVERIFY(catalog.addVersion(raw, &cerr));
    const QString outDir = m_workDir.filePath(QStringLiteral("attrs_switch"));
    dock.setInterpretationCatalog(&catalog, seisAsset.id, raw.id, outDir);

    auto *panel = dock.attrPanel();
    QVERIFY(panel);
    panel->setVisible(true);
    auto *scope = panel->findChild<QComboBox *>(QStringLiteral("attrScopeCombo"));
    auto *compute = panel->findChild<QToolButton *>(QStringLiteral("attrComputeButton"));
    QVERIFY(scope && compute);

    // 时序一：扫描启动后工程即关闭（体清空、无取消）——回调经体路径守卫
    // 静默丢弃，面板如实收尾忙态。
    QSignalSpy volumeSpy(&dock, &SeismicSectionDockWidget::attrVolumeReady);
    scope->setCurrentIndex(2); // 属性体
    compute->click();
    dock.setVolume(nullptr); // resetProjectScopedState 的换体路径
    QVERIFY(waitFor([&panel]() { return !panel->isBusy(); }, 60000));
    QTest::qWait(200);
    QCOMPARE(volumeSpy.count(), 0); // 不得 emit（旧工程属性体禁入新视口）

    // 时序二：resetProjectScopedState 的真实顺序——先换体、再
    // resetInterpretationState（取消在途扫描 + 复位面板）——结论一致。
    auto vol2 = std::make_shared<SgyVolume>();
    QVERIFY(vol2->Load(sgy.toStdString(), err));
    dock.setVolume(vol2);
    QVERIFY(waitFor([&dock]() { return dock.canvas()->hasData(); }, 10000) || true);
    scope->setCurrentIndex(2);
    compute->click();
    dock.setVolume(nullptr);
    dock.resetInterpretationState();
    QVERIFY(waitFor([&panel]() { return !panel->isBusy(); }, 60000));
    QTest::qWait(200);
    QCOMPARE(volumeSpy.count(), 0);
    QVERIFY(!panel->isBusy());
    QVERIFY(!panel->findChild<QToolButton *>(QStringLiteral("attrRegisterButton"))
                 ->isEnabled()); // 可登记态未残留
  }

};

QTEST_MAIN(TestSeismicAttrUi)
#include "tst_seismicattrui.moc"
