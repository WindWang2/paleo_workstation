#include <QtTest>
#include <QElapsedTimer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QScopeGuard>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif

#include "domain/seismic/sgydatacache.h"
#include "domain/seismic/sgyindexbuilder.h"
#include "domain/seismic/sgyindexcache.h"
#include "domain/seismic/sgyvolume.h"
#include "services/paleotaskservice.h"
#include "services/seismictaskservice.h"
#include "services/seismicdataprojection.h"
#include "services/fspathutils.h"
#include "io/areamodel.h"
#include "perfbudget_relax.h"

class TestSeismicIndex : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir tempDir_;
  QString testFilePath_;
  QString largeFilePath_;

  static bool writeSyntheticVolume(const QString &filePath,
                                   int firstInline, int lastInline,
                                   int firstXline, int lastXline,
                                   int ns = 64, int dt = 2000)
  {
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
      return false;

    // 1. Textual Header (3200 bytes)
    QByteArray textHdr(3200, ' ');
    const QString banner = QString(
        "C01 TEST First inline : %1 Last inline : %2 First xline : %3 Last xline : %4")
        .arg(firstInline).arg(lastInline).arg(firstXline).arg(lastXline);
    const QByteArray bannerBytes = banner.toUtf8();
    std::memcpy(textHdr.data(), bannerBytes.constData(), bannerBytes.size());
    file.write(textHdr);

    // 2. Binary Header (400 bytes)
    QByteArray binHdr(400, 0);
    qToBigEndian<qint32>(1, reinterpret_cast<uchar *>(binHdr.data()) + 4);
    qToBigEndian<qint16>(dt, reinterpret_cast<uchar *>(binHdr.data()) + 16);
    qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(binHdr.data()) + 20);
    qToBigEndian<qint16>(5, reinterpret_cast<uchar *>(binHdr.data()) + 24); // IEEE float
    qToBigEndian<qint16>(1, reinterpret_cast<uchar *>(binHdr.data()) + 54); // declared metres
    qToBigEndian<qint16>(0x0100, reinterpret_cast<uchar *>(binHdr.data()) + 300);
    file.write(binHdr);

    // 3. Traces
    int traceIndex = 0;
    const int inlineCount = lastInline - firstInline + 1;
    const int xlineCount = lastXline - firstXline + 1;

    for (int i = 0; i < inlineCount; ++i)
    {
      const int inlineNo = firstInline + i;
      for (int j = 0; j < xlineCount; ++j)
      {
        const int xlineNo = firstXline + j;

        QByteArray trHdr(240, 0);
        qToBigEndian<qint32>(traceIndex + 1, reinterpret_cast<uchar *>(trHdr.data()) + 0);
        qToBigEndian<qint32>(inlineNo, reinterpret_cast<uchar *>(trHdr.data()) + 8);
        qToBigEndian<qint32>(xlineNo, reinterpret_cast<uchar *>(trHdr.data()) + 20);
        qToBigEndian<qint16>(1, reinterpret_cast<uchar *>(trHdr.data()) + 70); // scalar
        qToBigEndian<qint16>(1, reinterpret_cast<uchar *>(trHdr.data()) + 88); // length coordinates
        qToBigEndian<qint32>(1000 + j * 25, reinterpret_cast<uchar *>(trHdr.data()) + 72); // src X
        qToBigEndian<qint32>(5000 + i * 25, reinterpret_cast<uchar *>(trHdr.data()) + 76); // src Y
        qToBigEndian<qint32>(1000 + j * 25, reinterpret_cast<uchar *>(trHdr.data()) + 180); // cdp X
        qToBigEndian<qint32>(5000 + i * 25, reinterpret_cast<uchar *>(trHdr.data()) + 184); // cdp Y
        qToBigEndian<qint16>(static_cast<qint16>(ns), reinterpret_cast<uchar *>(trHdr.data()) + 114);
        qToBigEndian<qint16>(static_cast<qint16>(dt), reinterpret_cast<uchar *>(trHdr.data()) + 116);
        qToBigEndian<qint32>(inlineNo, reinterpret_cast<uchar *>(trHdr.data()) + 188); // inline
        qToBigEndian<qint32>(xlineNo, reinterpret_cast<uchar *>(trHdr.data()) + 192); // crossline
        file.write(trHdr);

        QByteArray samples(ns * 4, 0);
        for (int k = 0; k < ns; ++k)
        {
          const float val = static_cast<float>((i + 1) * 10 + (j + 1) + k * 0.1f);
          quint32 rawBits;
          std::memcpy(&rawBits, &val, 4);
          qToBigEndian<quint32>(rawBits, reinterpret_cast<uchar *>(samples.data()) + k * 4);
        }
        file.write(samples);

        ++traceIndex;
      }
    }
    file.close();
    return true;
  }

private slots:
  void registeredSourceFailsClosedAfterExternalChange()
  {
    const auto file=tempDir_.filePath("registered-source.sgy");QVERIFY(writeSyntheticVolume(file,10,12,20,23,16));
    const auto project=tempDir_.filePath("registered-area");paleo::data::DataService service;std::string error;
    QVERIFY2(service.createProject(paleo::area_model::directory(project).toUtf8().toStdString(),"seismic sources",error),error.c_str());
    paleo::data::ImportReport report;QVERIFY2(service.importFile(file.toUtf8().toStdString(),{},report,error),error.c_str());
    paleo::area_model::registerProject(project,&service);
    const auto cleanup=qScopeGuard([&]{paleo::area_model::unregisterProject(&service);});
    paleo::SeismicDataAccess source;QString detail;QVERIFY2(source.open(file,&detail),qPrintable(detail));
    paleo::data::SectionReadOptions options;options.fullResolution=true;paleo::data::SeismicSectionResult section;
    QVERIFY2(source.line(true,10,options,section,error),error.c_str());QCOMPARE(section.amplitudes.front(),11.f);
    QFile changed(file);QVERIFY(changed.open(QIODevice::ReadWrite));QVERIFY(changed.seek(3600+240));
    float value=1011.f;quint32 bits;std::memcpy(&bits,&value,4);char bytes[4];qToBigEndian(bits,reinterpret_cast<uchar*>(bytes));QCOMPARE(changed.write(bytes,4),qint64(4));
    QVERIFY(changed.setFileTime(QDateTime::currentDateTimeUtc().addSecs(2),QFileDevice::FileModificationTime));changed.close();
    QVERIFY(!source.line(true,10,options,section,error));QVERIFY(!error.empty());
    QVERIFY2(paleo::data::DataService::readSeismicFileLine(file.toUtf8().toStdString(),"inline",10,options,section,error),error.c_str());QCOMPARE(section.amplitudes.front(),1011.f);
  }

  void nativeSectionsKeepDelayAndValuesWithoutViewerIndex()
  {
    const auto file=tempDir_.filePath(QStringLiteral("原生剖面.sgy"));
    QVERIFY(writeSyntheticVolume(file,10,12,20,23,16));
    QFile source(file);QVERIFY(source.open(QIODevice::ReadWrite));
    for(int trace=0;trace<12;++trace){QVERIFY(source.seek(3600+trace*(240+16*4)+108));char delay[2];qToBigEndian<qint16>(100,reinterpret_cast<uchar*>(delay));QCOMPARE(source.write(delay,2),qint64(2));}source.close();
    paleo::data::SectionReadOptions options;options.fullResolution=true;
    paleo::data::SeismicSectionResult section;std::string error;
    QVERIFY2(paleo::data::DataService::readSeismicFileLine(file.toUtf8().toStdString(),"inline",11,options,section,error),error.c_str());
    QCOMPARE(section.columns,4);QCOMPARE(section.samples,16);QCOMPARE(section.firstTime,100.);QCOMPARE(section.amplitudes[0],21.f);
    seismic::SgySliceImage image;QString detail;
    QVERIFY2(paleo::readSeismicDataSlice(file,seismic::SgySliceType::Time,5,image,detail),qPrintable(detail));
    QCOMPARE(image.width,4);QCOMPARE(image.height,3);QVERIFY(std::abs(image.Value(0,2)-11.5f)<.001);
    QVERIFY(!std::filesystem::exists(seismic::SgyIndexCache::CachePathFor(paleo::toFsPath(file))));
    options.maxValues=4;QVERIFY(!paleo::data::DataService::readSeismicFileLine(file.toUtf8().toStdString(),"inline",11,options,section,error));QVERIFY(section.amplitudes.empty());
    options.maxValues=8000000;QVERIFY(!paleo::data::DataService::readSeismicFileLine(file.toUtf8().toStdString(),"inline",11,options,section,error,[]{return true;}));QVERIFY(section.amplitudes.empty());
  }

  void quick3DThenCachedFullResolution()
  {
    const auto path=tempDir_.filePath(QStringLiteral("progressive3d.sgy"));QVERIFY(writeSyntheticVolume(path,1,4,1,512,32));
    PaleoTaskService tasks;seismic::SeismicTaskService service(&tasks);
    bool ready=false,ok=false;QString error;std::shared_ptr<seismic::SgyVolume> coarse,full;
    service.startQuickOpen(path,128,[&](bool success,const seismic::SeismicQuickPreview& preview){ok=success&&preview.ruleVerified;coarse=preview.volume;error=preview.error;ready=true;});
    QTRY_VERIFY_WITH_TIMEOUT(ready,15000);QVERIFY2(ok,qPrintable(error));QVERIFY(coarse&&coarse->IsRuleBased());
    const auto cache=seismic::SgyIndexCache::CachePathFor(paleo::toFsPath(path));QVERIFY(!std::filesystem::exists(cache));
    std::shared_ptr<const seismic::SgySliceImage> image;ready=false;
    service.startSliceExtraction(coarse,seismic::SgySliceType::Inline,2,[&](bool success,auto value,const QString& why){ok=success;image=value;error=why;ready=true;},{},true);
    QTRY_VERIFY_WITH_TIMEOUT(ready,15000);QVERIFY2(ok,qPrintable(error));QVERIFY(image);QCOMPARE(image->width,128);
    ready=false;service.startVolumeLoad(path,[&](bool success,auto value,const QString& why){ok=success;full=value;error=why;ready=true;},true);
    QTRY_VERIFY_WITH_TIMEOUT(ready,15000);QVERIFY2(ok,qPrintable(error));QVERIFY(full&&!full->IsRuleBased());QVERIFY(std::filesystem::exists(cache));
    ready=false;service.startSliceExtraction(full,seismic::SgySliceType::Inline,2,[&](bool success,auto value,const QString& why){ok=success;image=value;error=why;ready=true;},{},true);
    QTRY_VERIFY_WITH_TIMEOUT(ready,15000);QVERIFY2(ok,qPrintable(error));QCOMPARE(image->width,512);QVERIFY(std::abs(image->Value(0,31)-21.f)<.001);
    const auto stamp=std::filesystem::last_write_time(cache);ready=false;
    service.startVolumeLoad(path,[&](bool success,auto value,const QString& why){ok=success&&value&&!value->IsRuleBased();error=why;ready=true;},true);
    QTRY_VERIFY_WITH_TIMEOUT(ready,15000);QVERIFY2(ok,qPrintable(error));QVERIFY(std::filesystem::last_write_time(cache)==stamp);
    QFile changed(path);QVERIFY(changed.open(QIODevice::Append));QCOMPARE(changed.write("x",1),qint64(1));changed.close();
    ready=false;service.startSliceExtraction(full,seismic::SgySliceType::Inline,2,[&](bool success,auto value,const QString& why){ok=success;image=value;error=why;ready=true;},{},true);
    QVERIFY(ready);QVERIFY(!ok);QVERIFY(!image);QVERIFY(!error.isEmpty());
    std::string reason;QVERIFY(!seismic::SgyIndexCache::Load(paleo::toFsPath(path),reason));
  }

  void initTestCase()
  {
    QVERIFY(tempDir_.isValid());
    // 审计 01 M4：索引缓存隔离到本测试的临时目录——直接运行（不经 ctest 沙箱）
    // 也不碰用户/全局缓存目录。
    const QString cacheDir = tempDir_.filePath(QStringLiteral("index-cache"));
    QVERIFY(QDir().mkpath(cacheDir));
    qputenv("SEISMIC_INDEX_CACHE_DIR", QFile::encodeName(cacheDir));
    QCOMPARE(QString::fromStdU16String(seismic::SgyIndexCache::CacheDirectory().u16string()),
             cacheDir);
    // 10 inlines x 10 crosslines = 100 traces
    testFilePath_ = tempDir_.filePath(QStringLiteral("test_grid_100.sgy"));
    QVERIFY(writeSyntheticVolume(testFilePath_, 1, 10, 1, 10));

    // 50 inlines x 20 crosslines = 1000 traces for cancellation test
    largeFilePath_ = tempDir_.filePath(QStringLiteral("test_grid_1000.sgy"));
    QVERIFY(writeSyntheticVolume(largeFilePath_, 1, 100, 1, 100));
  }

  void cleanupTestCase()
  {
    std::string err;
    seismic::SgyIndexCache::Remove(testFilePath_.toStdString(), err);
    seismic::SgyIndexCache::Remove(largeFilePath_.toStdString(), err);
  }

  // 审计 01 M4：未显式覆盖时缓存目录必须是每用户的（XDG / ~/.cache），
  // 不再是多用户共享的 <tmp>/paleo_workstation。
  void cacheDirectoryIsPerUser()
  {
#ifdef Q_OS_WIN
    QSKIP("POSIX 目录解析序；Windows 走 %LOCALAPPDATA%");
#else
    const QByteArray savedOverride = qgetenv("SEISMIC_INDEX_CACHE_DIR");
    const QByteArray savedXdg = qgetenv("XDG_CACHE_HOME");
    const bool hadXdg = qEnvironmentVariableIsSet("XDG_CACHE_HOME");
    const QByteArray savedHome = qgetenv("HOME");
    const auto cacheDirStr = [] {
      return QString::fromStdU16String(seismic::SgyIndexCache::CacheDirectory().u16string());
    };
    QTemporaryDir fake;
    QVERIFY(fake.isValid());
    qunsetenv("SEISMIC_INDEX_CACHE_DIR");

    qputenv("XDG_CACHE_HOME", QFile::encodeName(fake.filePath(QStringLiteral("xdg"))));
    const QString viaXdg = cacheDirStr();
    qunsetenv("XDG_CACHE_HOME");
    qputenv("HOME", QFile::encodeName(fake.filePath(QStringLiteral("home"))));
    const QString viaHome = cacheDirStr();
    qputenv("XDG_CACHE_HOME", "relative/ignored"); // 非绝对路径按 XDG 规范忽略
    const QString viaRelXdg = cacheDirStr();
    qunsetenv("XDG_CACHE_HOME");
    qunsetenv("HOME");
    const QString viaTmp = cacheDirStr(); // 最后回落：tmp 下按 uid 分目录

    qputenv("SEISMIC_INDEX_CACHE_DIR", savedOverride);
    qputenv("HOME", savedHome);
    if (hadXdg)
      qputenv("XDG_CACHE_HOME", savedXdg);
    else
      qunsetenv("XDG_CACHE_HOME");

    QCOMPARE(viaXdg, fake.filePath(QStringLiteral("xdg/paleo_workstation/index-cache")));
    QCOMPARE(viaHome, fake.filePath(QStringLiteral("home/.cache/paleo_workstation/index-cache")));
    QCOMPARE(viaRelXdg, viaHome);
    QVERIFY2(viaTmp.contains(QStringLiteral("/paleo_workstation-%1/").arg(::getuid())),
             qPrintable(viaTmp));
#endif
  }

  void initialIndexingCreatesCacheFile()
  {
    std::string removeErr;
    seismic::SgyIndexCache::Remove(testFilePath_.toStdString(), removeErr);

    PaleoTaskService taskService;
    seismic::SeismicTaskService seismicService(&taskService);

    seismic::SgyIndexPtr resultIndex;
    bool done = false;
    QString error;

    PaleoTask *task = seismicService.startIndexing(
        testFilePath_,
        /*forceReindex=*/true,
        [&](bool success, seismic::SgyIndexPtr index, const QString &err) {
          done = true;
          resultIndex = index;
          error = err;
        });

    QVERIFY(task != nullptr);
    QSignalSpy finishedSpy(task, &PaleoTask::finished);
    QVERIFY(finishedSpy.wait(5000));
    QVERIFY(done);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(resultIndex != nullptr);
    QVERIFY(resultIndex->complete);
    QCOMPARE(resultIndex->traceCount, 100);
    QCOMPARE(resultIndex->inlineMin, 1);
    QCOMPARE(resultIndex->inlineMax, 10);
    QCOMPARE(resultIndex->xlineMin, 1);
    QCOMPARE(resultIndex->xlineMax, 10);

    // Verify cache file was created on disk
    const auto cachePath = seismic::SgyIndexCache::CachePathFor(testFilePath_.toStdString());
    std::error_code ec;
    QVERIFY(std::filesystem::exists(cachePath, ec));
    QVERIFY(std::filesystem::file_size(cachePath, ec) > 0);
  }

  void secondLoadUsesCacheAndIsMuchFaster()
  {
    PaleoTaskService taskService;
    seismic::SeismicTaskService seismicService(&taskService);

    QElapsedTimer timer;
    timer.start();

    seismic::SgyIndexPtr cachedIndex;
    bool done = false;
    QString error;

    PaleoTask *task = seismicService.startIndexing(
        testFilePath_,
        /*forceReindex=*/false,
        [&](bool success, seismic::SgyIndexPtr index, const QString &err) {
          done = true;
          cachedIndex = index;
          error = err;
        });

    QVERIFY(task != nullptr);
    QSignalSpy finishedSpy(task, &PaleoTask::finished);
    QVERIFY(finishedSpy.wait(5000));
    const qint64 elapsedMs = timer.elapsed();

    QVERIFY(done);
    QVERIFY(cachedIndex != nullptr);
    QCOMPARE(cachedIndex->traceCount, 100);
    // Cache loading from disk takes < 50ms (typically < 5ms)
    QVERIFY(elapsedMs < paleo::perfbudget::relaxedBudgetMs(100));
  }

  void companionSgyidxSupported()
  {
    const std::filesystem::path sgyStdPath(testFilePath_.toStdString());
    const auto companionPath = seismic::SgyIndexCache::CompanionPathFor(sgyStdPath);

    // Save as companion .sgyidx
    std::string loadReason;
    auto index = seismic::SgyIndexCache::Load(sgyStdPath, loadReason);
    QVERIFY(index != nullptr);

    std::string saveErr;
    QVERIFY(seismic::SgyIndexCache::Save(index, saveErr, companionPath));
    std::error_code ec;
    QVERIFY(std::filesystem::exists(companionPath, ec));

    // Remove centralized cache to ensure it loads from companion
    const auto centralCache = seismic::SgyIndexCache::CachePathFor(sgyStdPath);
    std::filesystem::remove(centralCache, ec);

    // Load should find companion
    std::string companionReason;
    auto loadedCompanion = seismic::SgyIndexCache::Load(sgyStdPath, companionReason);
    QVERIFY2(loadedCompanion != nullptr, companionReason.c_str());
    QCOMPARE(loadedCompanion->traceCount, 100);

    // Clean companion
    std::filesystem::remove(companionPath, ec);
  }

  void cancellationInterruptsPromptly()
  {
    std::string removeErr;
    seismic::SgyIndexCache::Remove(largeFilePath_.toStdString(), removeErr);

    PaleoTaskService taskService;
    seismic::SeismicTaskService seismicService(&taskService);

    QElapsedTimer cancelTimer;
    PaleoTask *task = seismicService.startIndexing(
        largeFilePath_,
        /*forceReindex=*/true,
        [](bool, seismic::SgyIndexPtr, const QString &) {});

    QVERIFY(task != nullptr);

    cancelTimer.start();
    task->requestCancel();

    QSignalSpy finishedSpy(task, &PaleoTask::finished);
    QVERIFY(finishedSpy.wait(1000));
    const qint64 cancelDurationMs = cancelTimer.elapsed();

    // Cancellation must respond within 50ms
    QVERIFY2(cancelDurationMs < paleo::perfbudget::relaxedBudgetMs(50),
             qPrintable(QString("Cancellation took %1 ms").arg(cancelDurationMs)));
    QCOMPARE(task->state(), PaleoTask::State::Cancelled);

    // Verify no corrupted cache file is published
    const auto cachePath = seismic::SgyIndexCache::CachePathFor(largeFilePath_.toStdString());
    std::error_code ec;
    QVERIFY(!std::filesystem::exists(cachePath, ec));
  }

  void corruptedCacheFallsBackSafely()
  {
    const std::filesystem::path sgyStdPath(testFilePath_.toStdString());
    const auto cachePath = seismic::SgyIndexCache::CachePathFor(sgyStdPath);

    // 1. First ensure a valid cache exists
    PaleoTaskService taskService;
    seismic::SeismicTaskService seismicService(&taskService);
    PaleoTask *task1 = seismicService.startIndexing(testFilePath_, true, nullptr);
    QSignalSpy fin1(task1, &PaleoTask::finished);
    QVERIFY(fin1.wait(5000));

    // 2. Corrupt the cache file by overwriting payload
    QFile cFile(QString::fromStdString(cachePath.string()));
    QVERIFY(cFile.open(QIODevice::ReadWrite));
    cFile.seek(20);
    cFile.write("CORRUPTED_PAYLOAD_GARBAGE_BYTES_TEST");
    cFile.close();

    // 3. SgyIndexCache::Load must detect corruption
    std::string reason;
    auto badIndex = seismic::SgyIndexCache::Load(sgyStdPath, reason);
    QVERIFY(badIndex == nullptr);
    QVERIFY(!reason.empty());

    // 4. SeismicTaskService must recover safely and re-scan
    seismic::SgyIndexPtr recoveredIndex;
    bool done = false;
    PaleoTask *task2 = seismicService.startIndexing(
        testFilePath_,
        /*forceReindex=*/false,
        [&](bool success, seismic::SgyIndexPtr index, const QString &) {
          done = success;
          recoveredIndex = index;
        });

    QSignalSpy fin2(task2, &PaleoTask::finished);
    QVERIFY(fin2.wait(5000));
    QVERIFY(done);
    QVERIFY(recoveredIndex != nullptr);
    QCOMPARE(recoveredIndex->traceCount, 100);
  }

  void dataCacheLruAndEviction()
  {
    // Budget = 20 KB
    seismic::SgyDataCache cache(20 * 1024);
    QCOMPARE(cache.Budget(), 20 * 1024ull);

    // Pin one entry externally: SgyDataCache must NEVER evict in-use entries
    auto pinned = std::make_shared<seismic::SgySliceImage>();
    pinned->width = 10;
    pinned->height = 10;
    pinned->values.assign(100, 999.0f);
    pinned->rgba.assign(400, 255);
    cache.Store("pinned_key", pinned);

    // Store 35 unpinned entries: each entry is ~850 bytes
    for (int i = 0; i < 35; ++i)
    {
      auto img = std::make_shared<seismic::SgySliceImage>();
      img->width = 10;
      img->height = 10;
      img->values.assign(100, static_cast<float>(i));
      img->rgba.assign(400, 255);
      cache.Store(std::string("key_") + std::to_string(i), std::move(img));
    }

    QVERIFY(cache.Evictions() > 0);
    // Pinned entry must survive eviction despite budget pressure
    QVERIFY(cache.Find("pinned_key") != nullptr);
    // Most recent entries should be present
    QVERIFY(cache.Find("key_34") != nullptr);
    // Oldest unpinned entries should have been evicted
    QVERIFY(cache.Find("key_0") == nullptr);
  }

  void asyncSliceAndSectionExtraction()
  {
    PaleoTaskService taskService;
    seismic::SeismicTaskService seismicService(&taskService);

    // Index first
    seismic::SgyIndexPtr index;
    PaleoTask *idxTask = seismicService.startIndexing(testFilePath_, false, [&](bool, seismic::SgyIndexPtr idx, const QString &) {
      index = idx;
    });
    QSignalSpy idxFin(idxTask, &PaleoTask::finished);
    QVERIFY(idxFin.wait(5000));
    QVERIFY(index != nullptr);

    auto volume = std::make_shared<seismic::SgyVolume>();
    volume->AdoptIndex(index);

    // Async slice extraction
    std::shared_ptr<const seismic::SgySliceImage> extractedSlice;
    PaleoTask *sliceTask = seismicService.startSliceExtraction(
        volume,
        seismic::SgySliceType::Inline,
        5,
        [&](bool success, std::shared_ptr<const seismic::SgySliceImage> img, const QString &) {
          if (success) extractedSlice = img;
        });

    QVERIFY(sliceTask != nullptr);
    QSignalSpy sliceFin(sliceTask, &PaleoTask::finished);
    QVERIFY(sliceFin.wait(5000));
    QVERIFY(extractedSlice != nullptr);
    QCOMPARE(extractedSlice->width, 10);
    QCOMPARE(extractedSlice->height, 64);

    // Second call should hit memory LRU cache immediately (returns nullptr task)
    std::shared_ptr<const seismic::SgySliceImage> cachedSlice;
    PaleoTask *cachedTask = seismicService.startSliceExtraction(
        volume,
        seismic::SgySliceType::Inline,
        5,
        [&](bool success, std::shared_ptr<const seismic::SgySliceImage> img, const QString &) {
          if (success) cachedSlice = img;
        });
    QVERIFY(cachedTask == nullptr); // Cache hit returns immediately!
    QTRY_VERIFY(cachedSlice != nullptr);
    QCOMPARE(cachedSlice->width, 10);

    // Async section extraction
    std::vector<glm::ivec2> pathPoints = { {1, 1}, {10, 10} };
    seismic::SgySectionOptions secOptions;
    secOptions.maxColumns = 32;

    std::shared_ptr<const seismic::SgySliceImage> secImage;
    seismic::SgySectionStats secStats;
    PaleoTask *secTask = seismicService.startSectionExtraction(
        volume,
        pathPoints,
        secOptions,
        [&](bool success, std::shared_ptr<const seismic::SgySliceImage> img, const seismic::SgySectionStats &stats, const QString &) {
          if (success) { secImage = img; secStats = stats; }
        });
    QVERIFY(secTask != nullptr);
    QSignalSpy secFin(secTask, &PaleoTask::finished);
    QVERIFY(secFin.wait(5000));
    QVERIFY(secImage != nullptr);
    QVERIFY(secImage->width > 0);
    QCOMPARE(secImage->height, 64);

    // 米口径在 columnDistancesM（columnDistances 保持线号弧长，供
    // SectionGeometry 换算）。本夹具 25m 正交测网（xline 步 25m X 向、
    // inline 步 25m Y 向），对角路径 (1,1)→(10,10) 的米弧长 =
    // 9·√(25²+25²) ≈ 318.20m；线号弧长 9·√2 ≈ 12.73。容差 1m。
    QCOMPARE(int(secStats.columnDistances.size()), secImage->width);
    QVERIFY(std::abs(double(secStats.columnDistances.back()) - 9.0 * std::sqrt(2.0)) < 1.0);
    QCOMPARE(int(secStats.columnDistancesM.size()), secImage->width);
    QVERIFY(secStats.columnDistancesM.size() >= 2);
    QVERIFY2(std::abs(double(secStats.columnDistancesM.back()) - 9.0 * 25.0 * std::sqrt(2.0)) < 1.0,
             qPrintable(QStringLiteral("剖面总长 %1m（期望 ~318.2m）")
                            .arg(secStats.columnDistancesM.back())));
    for (std::size_t c = 1; c < secStats.columnDistancesM.size(); ++c)
      QVERIFY(secStats.columnDistancesM[c] >= secStats.columnDistancesM[c - 1]); // 单调累计
    for (std::size_t c = 1; c < secStats.columnDistances.size(); ++c)
      QVERIFY(secStats.columnDistances[c] >= secStats.columnDistances[c - 1]); // 单调累计
  }
};

QTEST_MAIN(TestSeismicIndex)
#include "tst_seismic_index.moc"
