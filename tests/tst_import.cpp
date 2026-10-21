#include <QtTest>
#include "importuxfixtures.h"
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QThreadPool>
#include <QDirIterator>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QPdfDocument>

#include <algorithm>
#include <atomic>
#include <thread>
#if defined(Q_OS_UNIX)
#include <sys/stat.h> // mkfifo（文件夹导入的「非普通文件」行）
#include <sys/wait.h>
#include <csignal>
#include <cerrno>
#endif

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/geojsonaffine.h"
#include "../src/io/ingestplan.h"
#include "../src/io/lasparser.h"
#include "../src/services/welllogset.h" // 方向44：导入绑定后并集可达
#include "fixtures/mkprojectfixture.h"  // 方向95：mini 夹具扩散（整工程生产产物）
#include "welllogfixturewriters.h" // 方向44：DLIS/LIS 夹具写入器
#include "../src/domain/projectclassifier.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "perfbudget_relax.h"

#include <gdal.h>
#include <gdal_priv.h>
#include <qgsmaplayer.h>

// project_area 导入契约（docs/PROJECT_AREA_PLAN.md §2/§3）在真实服务栈上的验收：
// 实体解析/受管 RAW（SHA-256+只读）/外链/多井关联/双候选 unresolved/未决层位/
// D61 派生栅格与图层 CRS/LAS 解析。旧 §41.2 行为（data/<kind>/ + 矢量声明）
// 已被替换：.dat 不再交给 OGR，非栅格不再声明成矢量。
class TestImport : public QObject
{
  Q_OBJECT

  struct Stack
  {
    QgisProjectService projectSvc;
    std::unique_ptr<LayerManifest> manifest;
    std::unique_ptr<QgisLayerService> layerSvc;
    std::unique_ptr<PaleoProjectStore> store;
    std::unique_ptr<DataImportService> importSvc;
    QString metaPath;
  };

  static bool writeFile(const QString &path, const QByteArray &content)
  {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
      return false;
    f.write(content);
    return true;
  }

  // 受管副本是只读的（锁真只读）：Windows 只读属性挡 QFile::remove——
  // 先授写权限再删（POSIX 侧 chmod 同义无害）。
  static bool removeManagedFile(const QString &path)
  {
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ReadUser | QFileDevice::WriteUser);
    return QFile::remove(path);
  }

  static std::unique_ptr<Stack> makeStack(const QString &projectDir, QString *errOut = nullptr)
  {
    auto s = std::make_unique<Stack>();
    s->metaPath = QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite"));
    if (!s->projectSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))))
    {
      if (errOut)
        *errOut = s->projectSvc.lastErrors().join(QLatin1Char(';'));
      return nullptr;
    }
    s->manifest = std::make_unique<LayerManifest>(s->metaPath);
    if (!s->manifest->open(errOut))
      return nullptr;
    s->layerSvc = std::make_unique<QgisLayerService>(&s->projectSvc, s->manifest.get());
    s->store = std::make_unique<PaleoProjectStore>();
    s->store->setProjectPaths(QDir(projectDir).filePath(QStringLiteral("proj.qgz")),
                              QDir(projectDir).filePath(QStringLiteral("project.gpkg")),
                              s->metaPath);
    s->importSvc = std::make_unique<DataImportService>(s->store.get());
    QObject::connect(s->importSvc.get(), &DataImportService::layerDeclared,
                     s->layerSvc.get(), [layerSvc = s->layerSvc.get()](const LayerDeclaration &decl) {
                       QString err;
                       layerSvc->declare(decl, &err);
                     });
    s->importSvc->setProjectDir(projectDir);
    return s;
  }

  static QString fixture(const QString &name)
  {
    return QStringLiteral(PROJECT_FIXTURE_DIR) + QLatin1Char('/') + name;
  }

  // 摆进带语义的目录再导入（分类按路径段：井分层/时深/层位）。
  static QString stageFixture(const QTemporaryDir &tmp, const QString &dir,
                              const QString &name, const QString &asName = QString())
  {
    const QString d = tmp.filePath(dir);
    if (!QDir().mkpath(d))
      return QString();
    const QString dst = QDir(d).filePath(asName.isEmpty() ? name : asName);
    if (!QFile::copy(fixture(name), dst))
      return QString();
    return dst;
  }

  static QString sha256OfFile(const QString &path)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return QString::fromLatin1(h.result().toHex());
  }

  // 在 service 打开前预置目录（预置井实体走独立 DataCatalog 落盘）。
  static bool seedCatalogWithSingleWell(const QString &projectDir)
  {
    DataCatalog pre;
    QString err;
    if (!pre.open(projectDir, &err))
      return false;
    CatalogEntity w;
    w.id = QStringLiteral("well-A1");
    w.entityType = QStringLiteral("well");
    w.name = QStringLiteral("A1");
    return pre.addEntity(w, &err);
  }

  // D12：歧义改用同名两井构造（uwi/aliases 字段已剥离）。
  static bool seedCatalogWithAliasWells(const QString &projectDir)
  {
    DataCatalog pre;
    QString err;
    if (!pre.open(projectDir, &err))
      return false;
    CatalogEntity w1;
    w1.id = QStringLiteral("well-X1");
    w1.entityType = QStringLiteral("well");
    w1.name = QStringLiteral("dup");
    if (!pre.addEntity(w1, &err))
      return false;
    CatalogEntity w2;
    w2.id = QStringLiteral("well-X2");
    w2.entityType = QStringLiteral("well");
    w2.name = QStringLiteral("dup");
    return pre.addEntity(w2, &err);
  }

private slots:
  void initTestCase()
  {
    QVERIFY(QgisRuntime::isInitialized());
  }

  // T4（data-foundation）：锁降级只读——导入在算 SHA/复制字节之前早拒，
  // 不留 artifacts/raw 孤儿文件。
  // 方向57 阻塞解除语义测试：PDF 转换在途时销毁服务，析构不得阻塞主线程
  // 等进程退出（原实现 ~DataImportService 里 waitForFinished(2000)——事件
  // 循环冻结最多 2s）。新实现 kill + deleteLater（与 setProjectDir 的工程
  // 切换清理同一模式）。阈值取旧 2s 上限的四分之三：旧实现必红、新实现
  // 数十毫秒级即回，两端都不贴边。
  void documentTeardownDoesNotBlockOnInflightConversion()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    // 长睡转换器：无论参数睡 30s 再退（模拟卡住的 soffice）。
    // Windows 用 .cmd（QProcess/CreateProcess 可直接拉起批处理），POSIX 用 .sh。
    const QString stub = tmp.filePath(QStringLiteral("sleepy_converter") +
#ifdef Q_OS_WIN
                                      QStringLiteral(".cmd"));
    {
      QFile f(stub);
      QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
      f.write("@ping -n 30 127.0.0.1 > nul\r\n");
      f.close();
    }
#else
                                      QStringLiteral(".sh"));
    {
      QFile f(stub);
      QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
      f.write("#!/bin/sh\nsleep 30\n");
      f.close();
      QFile::setPermissions(stub, QFileDevice::ExeOwner | QFileDevice::ReadOwner |
                                     QFileDevice::WriteOwner);
    }
#endif
    svc.setDocumentConverterProgram(stub);

    QString err;
    const QString docSrc = tmp.filePath(QStringLiteral("stub.doc"));
    {
      QFile df(docSrc);
      QVERIFY(df.open(QIODevice::WriteOnly));
      df.write("not a real doc");
      df.close();
    }
    const QString docId = svc.importProjectFile(docSrc, &err);
    QVERIFY2(!docId.isEmpty(), qPrintable(err));

    svc.ensureDocumentPdf(docId);
    QCOMPARE(svc.documentPdfState(docId), DataImportService::DocPdfState::Pending);

    QElapsedTimer t0;
    t0.start();
    stack.reset(); // 析构：旧实现在此 waitForFinished(2000)
    const qint64 elapsed = t0.elapsed();
    QVERIFY2(elapsed < paleo::perfbudget::relaxedBudgetMs(1500),
             qPrintable(QStringLiteral("destroy blocked %1ms").arg(elapsed)));
  }

  // FE-004：LibreOffice profile 目录必须随机名且随服务析构清理——旧实现
  // 固定名 /tmp/paleo-lo-profile-<pid> 是多用户机上的预布设面（symlink），
  // 且从不清理（每个转换进程在 /tmp 永久遗留一个 profile）。
  void documentConversionProfileDirIsRandomAndCleanedUp()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    // 长睡转换器（复用 documentTeardown 的口径）：转换在途即可观测 profile。
    const QString stub = tmp.filePath(QStringLiteral("sleepy_profile") +
#ifdef Q_OS_WIN
                                      QStringLiteral(".cmd"));
    {
      QFile f(stub);
      QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
      f.write("@ping -n 30 127.0.0.1 > nul\r\n");
      f.close();
    }
#else
                                      QStringLiteral(".sh"));
    {
      QFile f(stub);
      QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
      f.write("#!/bin/sh\nsleep 30\n");
      f.close();
      QFile::setPermissions(stub, QFileDevice::ExeOwner | QFileDevice::ReadOwner |
                                     QFileDevice::WriteOwner);
    }
#endif
    svc.setDocumentConverterProgram(stub);
    QString err;
    const QString docSrc = tmp.filePath(QStringLiteral("stub_profile.doc"));
    {
      QFile df(docSrc);
      QVERIFY(df.open(QIODevice::WriteOnly));
      df.write("not a real doc");
      df.close();
    }
    const QString docId = svc.importProjectFile(docSrc, &err);
    QVERIFY2(!docId.isEmpty(), qPrintable(err));
    svc.ensureDocumentPdf(docId);

    const QStringList before = QDir::temp().entryList(
        {QStringLiteral("paleo-lo-profile-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    QVERIFY2(!before.contains(QStringLiteral("paleo-lo-profile-%1").arg(
                                  QCoreApplication::applicationPid())),
             "profile 目录不得用进程号可预测名（多用户机预布设面）");
    stack.reset(); // 析构：profile 随 QTemporaryDir 清理
    const QStringList after = QDir::temp().entryList(
        {QStringLiteral("paleo-lo-profile-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(after.size(), 0);
  }

  // 方向 94：resolver 三级探测序 env(PALEO_SOFFICE) > vendored
  // (vendor/libreoffice/program/soffice[.exe] 自应用目录向上找) > PATH。
  // env 层无条件赢（部署逃生门，即使路径尚不存在）；vendored 树在场时
  // 必须命中且不得越级掉 PATH；vendored 缺席的瘦宿主不强断言 PATH 层
  //（宿主装没装 soffice 不是本测试的语义面）。
  void converterResolutionThreeTierOrder()
  {
    // tier 1：独立服务栈——探测结果会缓存（m_converterResolved），
    // 每层用新栈观测，避免上一层的缓存污染。
    {
      QTemporaryDir tmp;
      const QString projectDir = tmp.filePath(QStringLiteral("proj"));
      QVERIFY(QDir().mkpath(projectDir));
      auto stack = makeStack(projectDir);
      QVERIFY(stack != nullptr);
      qputenv("PALEO_SOFFICE", "/nonexistent/escape/hatch/soffice");
      const QString resolved = stack->importSvc->documentConverterProgram();
      qunsetenv("PALEO_SOFFICE");
      QCOMPARE(resolved, QStringLiteral("/nonexistent/escape/hatch/soffice"));
    }
    // tier 2/3：无 env——PATH 前置一个假 soffice，断言 vendored 仍赢
    //（真序验证：不是「PATH 恰好没有」，而是「有也不让」）。
    {
      QTemporaryDir tmp;
      const QString projectDir = tmp.filePath(QStringLiteral("proj"));
      QVERIFY(QDir().mkpath(projectDir));
      auto stack = makeStack(projectDir);
      QVERIFY(stack != nullptr);
      QDir appDir(QCoreApplication::applicationDirPath());
      QString vendored;
      for (int i = 0; i < 10 && vendored.isEmpty(); ++i)
      {
        const QString cand = appDir.absoluteFilePath(
#ifdef Q_OS_WIN
            QStringLiteral("vendor/libreoffice/program/soffice.exe"));
#else
            QStringLiteral("vendor/libreoffice/program/soffice"));
#endif
        if (QFileInfo(cand).isExecutable())
          vendored = cand;
        else if (!appDir.cdUp())
          break;
      }
      if (vendored.isEmpty())
        QSKIP("vendored LibreOffice absent (run vendor/fetch-libreoffice.sh to enable the vendored-tier assertion)");
      // PATH 首位放假 soffice：Windows 用零字节 .exe（isExecutable 只看
      // 扩展名）；POSIX 需要 +x 位。
      const QString fakeBin = tmp.filePath(QStringLiteral("fake-bin"));
      QVERIFY(QDir().mkpath(fakeBin));
      const QString fake = fakeBin + QLatin1Char('/') +
#ifdef Q_OS_WIN
                           QStringLiteral("soffice.exe");
#else
                           QStringLiteral("soffice");
#endif
      QVERIFY(writeFile(fake, QByteArray()));
#ifndef Q_OS_WIN
      QFile::setPermissions(fake, QFileDevice::ExeOwner | QFileDevice::ReadOwner);
#endif
      const QString oldPath = qEnvironmentVariable("PATH");
      qputenv("PATH", QDir::toNativeSeparators(fakeBin).toUtf8().constData());
      const QString resolved = stack->importSvc->documentConverterProgram();
      if (!oldPath.isEmpty())
        qputenv("PATH", oldPath.toUtf8().constData());
      else
        qunsetenv("PATH");
      QCOMPARE(resolved, vendored);
    }
  }

  // 方向 94：vendored LibreOffice 真转换 round-trip（Linux deb 子集 /
  // Windows msi 子集同一管线）。仅探测到真实 vendored 转换器时跑；断言
  // PDF 字节头 + 页数 > 0（QtPdf）+ DERIVED 版本血统（generator/
  // converterVersion/converterPlatform——跨平台差异可追溯）。
  void vendoredDocumentConversionRoundTrip()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString converter = svc.documentConverterProgram();
    const bool vendored =
        converter.endsWith(QStringLiteral("vendor/libreoffice/program/soffice"),
                           Qt::CaseInsensitive) ||
        converter.endsWith(QStringLiteral("vendor/libreoffice/program/soffice.exe"),
                           Qt::CaseInsensitive);
    if (!vendored)
      QSKIP("vendored LibreOffice absent (run vendor/fetch-libreoffice.sh first)");

    // docx 与 xlsx 双格式走同一条 ensureDocumentPdf 线。
    const QString officeDir = QFileInfo(
        QFINDTESTDATA("fixtures/office/sample.docx")).absolutePath();
    for (const char *ext : {"docx", "xlsx"})
    {
      const QString src = officeDir + QStringLiteral("/sample.") + QLatin1String(ext);
      QVERIFY2(QFile::exists(src), qPrintable(src));
      QString err;
      const QString assetId = svc.importProjectFile(src, &err);
      QVERIFY2(!assetId.isEmpty(), qPrintable(err));

      QSignalSpy ready(&svc, &DataImportService::documentPdfReady);
      QSignalSpy failed(&svc, &DataImportService::documentPdfFailed);
      svc.ensureDocumentPdf(assetId);
      QVERIFY2(ready.wait(180000),
               qPrintable(svc.documentPdfError(assetId)));
      QCOMPARE(svc.documentPdfState(assetId), DataImportService::DocPdfState::Ready);

      QFile pdf(svc.documentPdfPath(assetId));
      QVERIFY2(pdf.open(QIODevice::ReadOnly), qPrintable(pdf.errorString()));
      QVERIFY2(pdf.read(5).startsWith("%PDF"), "PDF magic");
      pdf.close();

      QPdfDocument doc;
      QCOMPARE(doc.load(svc.documentPdfPath(assetId)), QPdfDocument::Error::None);
      QVERIFY2(doc.pageCount() > 0, "page count > 0");

      // DERIVED 血统：generator + 转换器版本/平台（方向 94 PROVENANCE）。
      const auto versions = svc.catalog()->versionsForAsset(assetId);
      const auto it = std::find_if(versions.cbegin(), versions.cend(),
                                   [](const CatalogVersion &v) {
                                     return v.stage == QLatin1String("DERIVED") &&
                                            v.fileName.endsWith(QLatin1String(".pdf"),
                                                                Qt::CaseInsensitive);
                                   });
      QVERIFY(it != versions.cend());
      QCOMPARE(it->extra.value(QStringLiteral("generator")).toString(),
               QStringLiteral("libreoffice"));
      QVERIFY2(!it->extra.value(QStringLiteral("converterVersion")).toString().isEmpty(),
               "converterVersion recorded");
      QVERIFY2(it->extra.value(QStringLiteral("converterVersion")).toString() !=
                   QStringLiteral("unknown"),
               "converterVersion resolved from vendored version metadata");
      QVERIFY2(!it->extra.value(QStringLiteral("converterPlatform")).toString().isEmpty(),
               "converterPlatform recorded");
    }
  }

  // 方向 94 Oracle：失败降级如实——转换器不可用（vendored 缺席且 PATH 无
  // soffice，或部署显式置空）时，ensureDocumentPdf 不装死：Pending 不进、
  // Failed 即刻落账 + 信号带「找不到 LibreOffice」文案（UI 降级「用系统
  // 程序打开」的驱动面）。空串注入 = 强制不可用（部署/测试出口语义）。
  void converterUnavailableFailsHonestly()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    svc.setDocumentConverterProgram(QString());

    const QString officeDir = QFileInfo(
        QFINDTESTDATA("fixtures/office/sample.docx")).absolutePath();
    const QString src = officeDir + QStringLiteral("/sample.docx");
    QVERIFY2(QFile::exists(src), qPrintable(src));
    QString err;
    const QString assetId = svc.importProjectFile(src, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    QSignalSpy failed(&svc, &DataImportService::documentPdfFailed);
    svc.ensureDocumentPdf(assetId);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(svc.documentPdfState(assetId), DataImportService::DocPdfState::Failed);
    QVERIFY2(svc.documentPdfError(assetId).contains(QStringLiteral("找不到 LibreOffice")),
             qPrintable(svc.documentPdfError(assetId)));
    // catalog 侧不留半成品 DERIVED。
    for (const CatalogVersion &v : svc.catalog()->versionsForAsset(assetId))
      QVERIFY(v.stage != QLatin1String("DERIVED"));
  }

  // #555 回归：soffice 挂死不得永久堵死转换队列。假 soffice（PALEO_SOFFICE
  // 指向挂死脚本）+ 短超时（PALEO_SOFFICE_TIMEOUT_MS）——超时必须两段式
  // 整组回收（Unix：孙进程 sleep 60 不留孤儿——只杀直接子会让它活满 60s）、
  // 失败走 documentPdfFailed 带超时文案、队列继续消化后续单（第二单也在
  // 短窗内以同样口径失败，而不是永 Pending）。
  void documentConversionTimeoutRecoversQueue()
  {
    // QVERIFY 失败即返回函数体——RAII 兜底恢复两个环境变量，不渗漏进同
    // 二进制里的后续测试（超时值在每单武装时读取，须全程在场）。
    struct EnvGuard
    {
      ~EnvGuard()
      {
        qunsetenv("PALEO_SOFFICE");
        qunsetenv("PALEO_SOFFICE_TIMEOUT_MS");
      }
    } envGuard;

    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    // 挂死 soffice 假体：孙进程 sleep 60（组覆盖断言用）+ 自身 sleep 30。
    // Windows 用 .cmd（无 pid 断言——进程组语义是 Unix 面板）。
    const QString stub = tmp.filePath(QStringLiteral("hang_soffice") +
#ifdef Q_OS_WIN
                                          QStringLiteral(".cmd"));
    {
      QFile f(stub);
      QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
      f.write("@ping -n 30 127.0.0.1 > nul\r\n");
      f.close();
    }
#else
                                          QStringLiteral(".sh"));
    {
      QFile f(stub);
      QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
      f.write("#!/bin/sh\n"
              "sleep 60 &\n"
              "echo $! > \"$(dirname \"$0\")/hang_child.pid\"\n"
              "echo $$ > \"$(dirname \"$0\")/hang_parent.pid\"\n"
              "exec sleep 30\n");
      f.close();
      QFile::setPermissions(stub, QFileDevice::ExeOwner | QFileDevice::ReadOwner |
                                     QFileDevice::WriteOwner);
    }
#endif
    qputenv("PALEO_SOFFICE", QFile::encodeName(stub).constData());
    qputenv("PALEO_SOFFICE_TIMEOUT_MS", "1500");

    // 两份内容不同的 .doc（dedup 按 SHA-256——同字节会并成同一资产）。
    QString err;
    QStringList docIds;
    for (int i = 0; i < 2; ++i)
    {
      const QString src = tmp.filePath(QStringLiteral("hang_%1.doc").arg(i));
      QFile df(src);
      QVERIFY(df.open(QIODevice::WriteOnly));
      df.write(QStringLiteral("not a real doc %1").arg(i).toUtf8());
      df.close();
      const QString id = svc.importProjectFile(src, &err);
      QVERIFY2(!id.isEmpty(), qPrintable(err));
      docIds.append(id);
    }

    QSignalSpy failed(&svc, &DataImportService::documentPdfFailed);
    svc.ensureDocumentPdf(docIds[0]); // 第一单：在途（挂死）
    QCOMPARE(svc.documentPdfState(docIds[0]), DataImportService::DocPdfState::Pending);
    svc.ensureDocumentPdf(docIds[1]); // 第二单：排队——超时后必须被消化
    // 两单都收口：1.5s 超时 ×2 + 优雅窗口余量，20s 上限远宽于正常路径。
    QVERIFY2(failed.wait(20000), "first conversion did not fail in time");
    QVERIFY2(failed.wait(20000), "queued conversion never drained (queue stuck)");
    QCOMPARE(failed.count(), 2);
    for (const QString &id : std::as_const(docIds))
    {
      QCOMPARE(svc.documentPdfState(id), DataImportService::DocPdfState::Failed);
      QVERIFY2(svc.documentPdfError(id).contains(QStringLiteral("超时")),
               qPrintable(svc.documentPdfError(id)));
    }

#ifndef Q_OS_WIN
    // 整组回收断言：直接子（壳/sleep 30）与孙进程（sleep 60）都必须死。
    // 退出后短暂僵尸态由 QTRY 轮询吸收（init/systemd 收养收割）。
    const auto readPid = [&tmp](const char *name) -> qint64 {
      QFile f(tmp.filePath(QString::fromLatin1(name)));
      return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed().toLongLong() : 0;
    };
    const qint64 parentPid = readPid("hang_parent.pid");
    const qint64 childPid = readPid("hang_child.pid");
    QVERIFY2(parentPid > 0, "stub parent pid recorded");
    QVERIFY2(childPid > 0, "stub grandchild pid recorded");
    QTRY_VERIFY_WITH_TIMEOUT(::kill(parentPid, 0) == -1 && errno == ESRCH, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(::kill(childPid, 0) == -1 && errno == ESRCH, 5000);
#endif
  }

  void lockedReadOnlyImportRefusedEarly()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);

    stack->importSvc->catalog()->setLockedReadOnly(true);
    QString err;
    const QString assetId =
        stack->importSvc->importProjectFile(fixture(QStringLiteral("A1.Las")), &err);
    QVERIFY(assetId.isEmpty());
    QVERIFY(err.contains(QStringLiteral("另一个实例锁定")));
    QVERIFY(stack->importSvc->catalog()->assets().isEmpty());
    // 早拒：受管目录连 raw/ 都没出现（字节没开始复制）。
    QVERIFY(!QFileInfo::exists(QDir(projectDir).filePath(QStringLiteral("artifacts/raw"))));
  }

  // 井位 → 20 井实体（untransformed、surface 坐标），多井文件=每井一条关联。
  void wellHeadCreatesEntitiesAndLinks()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    QString err;
    const QString assetId = svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    DataCatalog *cat = svc.catalog();
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 20);
    const CatalogEntity a1 = cat->entityById(QStringLiteral("well-A1"));
    QCOMPARE(a1.name, QStringLiteral("A1"));
    QVERIFY(a1.hasSurface);
    QCOMPARE(a1.surfaceX, 5288.670);
    QCOMPARE(a1.surfaceY, 8219.940);
    QCOMPARE(a1.coordinateStatus, QStringLiteral("untransformed"));
    // 一份多井文件 → 20 条 well_head 关联（不拆文件）
    QCOMPARE(cat->linksForAsset(assetId).size(), 20);
    QCOMPARE(cat->linksForEntity(QStringLiteral("well-A1")).front().role,
             QStringLiteral("well_head"));
  }

  // A1.Las → 挂 well-A1；受管 RAW 落盘只读且 SHA-256 与源一致；无图层声明。
  void managedRawChecksumAndReadOnly()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err).isEmpty());
    const QString assetId = svc.importProjectFile(fixture(QStringLiteral("A1.Las")), &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    DataCatalog *cat = svc.catalog();
    const QVector<EntityAssetLink> links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.front().entityId, QStringLiteral("well-A1"));
    QCOMPARE(links.front().role, QStringLiteral("well_log"));
    QVERIFY(!links.front().unresolved);

    const CatalogVersion v = cat->currentVersion(assetId);
    QVERIFY(v.managed);
    QCOMPARE(v.stage, QStringLiteral("RAW"));
    const QString abs = svc.absolutePath(assetId);
    QVERIFY(abs.endsWith(QStringLiteral("artifacts/raw/") + assetId + QLatin1Char('/') + v.id +
                         QStringLiteral("/A1.Las")));
    QVERIFY(QFile::exists(abs));
    QCOMPARE(v.sha256, sha256OfFile(fixture(QStringLiteral("A1.Las"))));
    // 只读位：无任何写权限
    const QFile::Permissions perm = QFileInfo(abs).permissions();
    QCOMPARE(int(perm & (QFile::WriteOwner | QFile::WriteUser | QFile::WriteGroup | QFile::WriteOther)), 0);
    // .las 不再声明成矢量图层
    QVERIFY(stack->manifest->all().isEmpty());
    QCOMPARE(svc.assetSource(assetId), abs);
  }

  // DC.dat → 多井分层文件挂多条 tops 关联。
  void topsFileLinksEveryWell()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err).isEmpty());
    const QString staged = stageFixture(tmp, QString::fromUtf8("井分层"), QStringLiteral("DC.dat"));
    QVERIFY(!staged.isEmpty());
    const QString assetId = svc.importProjectFile(staged, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    DataCatalog *cat = svc.catalog();
    const QVector<EntityAssetLink> links = cat->linksForAsset(assetId);
    QVERIFY(links.size() >= 15); // 20 口井分层覆盖面
    bool sawA1 = false;
    for (const EntityAssetLink &l : links)
    {
      QCOMPARE(l.role, QStringLiteral("tops"));
      QVERIFY(!l.unresolved);
      if (l.entityId == QLatin1String("well-A1"))
        sawA1 = true;
    }
    QVERIFY(sawA1);
  }

  // 时深：井名列（# Well : A1）→ well-A1 + time_depth 关联。
  void timeDepthBindsByWellColumn()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err).isEmpty());
    const QString staged = stageFixture(tmp, QString::fromUtf8("时深"), QStringLiteral("A1_TD.dat"));
    QVERIFY(!staged.isEmpty());
    const QString assetId = svc.importProjectFile(staged, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    const QVector<EntityAssetLink> links = svc.catalog()->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.front().entityId, QStringLiteral("well-A1"));
    QCOMPARE(links.front().role, QStringLiteral("time_depth"));
  }

  // 双候选不得猜井或未决入库；显式选择后导入，归属与主文件状态可持久化。
  void ambiguousWellNameRequiresExplicitTarget()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithAliasWells(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack);
    DataImportService &svc = *stack->importSvc;
    const QString path = tmp.filePath(QStringLiteral("dup.Las"));
    QVERIFY(writeFile(path, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. dup : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    auto *cat = svc.catalog();
    const auto revision = cat->mutationSeq();
    QSignalSpy failed(&svc, &DataImportService::importFailed);
    QSignalSpy imported(&svc, &DataImportService::imported);
    QString error;
    QVERIFY(svc.importProjectFile(path, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QCOMPARE(failed.count(), 1);
    QCOMPARE(imported.count(), 0);
    QCOMPARE(cat->mutationSeq(), revision);
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2);
    QVERIFY(cat->assets().isEmpty());
    QVERIFY(cat->links().isEmpty());
    QVERIFY(cat->entities(QStringLiteral("auxiliary")).isEmpty());
    DataImportService::ImportOptions options;
    options.targetWellId = QStringLiteral("well-X1");
    const auto result = svc.importProjectFileEx(path, options, &error);
    QVERIFY2(!result.assetId.isEmpty(), qPrintable(error));
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2);
    const auto links = cat->linksForAsset(result.assetId);
    QCOMPARE(links.size(), 1);
    QVERIFY(!links.first().unresolved);
    QCOMPARE(links.first().entityId, options.targetWellId);
    QVERIFY(links.first().isPrimary);
    DataCatalog reloaded;
    QVERIFY(reloaded.open(projectDir, &error));
    const auto persisted = reloaded.linksForAsset(result.assetId);
    QCOMPARE(persisted.size(), 1);
    QCOMPARE(persisted.first().entityId, options.targetWellId);
    QVERIFY(!persisted.first().unresolved);
  }

  // 内容井名明确且无同名井时新建井；文件名 Ghost 不得覆盖内容名 Ghost9。
  void contentWellNameCreatesNewWellWithoutFilenameGuess()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack);
    DataImportService &svc = *stack->importSvc;
    const QString path = tmp.filePath(QStringLiteral("Ghost.Las"));
    QVERIFY(writeFile(path, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. Ghost9 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    QString error;
    const QString asset = svc.importProjectFile(path, &error);
    QVERIFY2(!asset.isEmpty(), qPrintable(error));
    auto *cat = svc.catalog();
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2);
    const auto wells = cat->wellsMatchingName(QStringLiteral("Ghost9"));
    QCOMPARE(wells.size(), 1);
    QCOMPARE(cat->entityById(wells.first()).name, QStringLiteral("Ghost9"));
    QCOMPARE(cat->entityById(wells.first()).coordinateStatus, QStringLiteral("unknown"));
    QVERIFY(cat->wellsMatchingName(QStringLiteral("Ghost")).isEmpty());
    QVERIFY(cat->entities(QStringLiteral("auxiliary")).isEmpty());
    QVERIFY(cat->linksForEntity(QStringLiteral("well-A1")).isEmpty());
    const auto links = cat->linksForAsset(asset);
    QCOMPARE(links.size(), 1);
    QVERIFY(!links.first().unresolved);
    QCOMPARE(links.first().entityId, wells.first());
    QCOMPARE(links.first().role, QStringLiteral("well_log"));
    DataCatalog reloaded;
    QVERIFY(reloaded.open(projectDir, &error));
    const auto persisted = reloaded.linksForAsset(asset);
    QCOMPARE(persisted.size(), 1);
    QCOMPARE(persisted.first().entityId, wells.first());
    QVERIFY(!persisted.first().unresolved);
  }

  // 无内容井名时，文件名不能创建井或猜已有井；显式选择可完成同一份数据导入。
  void filenameOnlyLogRequiresExplicitTarget()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack);
    auto &svc = *stack->importSvc;
    const QString path = tmp.filePath(QStringLiteral("A1.las"));
    QVERIFY(writeFile(path, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    auto *cat = svc.catalog();
    const auto revision = cat->mutationSeq();
    QString error;
    const auto preview = svc.previewFiles({path}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(preview.size(), 1);
    QVERIFY(preview.first().needsWellChoice);
    QVERIFY(preview.first().suggestedWellId.isEmpty());
    QVERIFY(svc.importProjectFile(path, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QCOMPARE(cat->mutationSeq(), revision);
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 1);
    QVERIFY(cat->assets().isEmpty());
    QVERIFY(cat->links().isEmpty());
    DataImportService::ImportOptions options;
    options.targetWellId = QStringLiteral("well-A1");
    const auto result = svc.importProjectFileEx(path, options, &error);
    QVERIFY2(!result.assetId.isEmpty(), qPrintable(error));
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 1);
    QCOMPARE(cat->assets().size(), 1);
    const auto links = cat->linksForAsset(result.assetId);
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().entityId, options.targetWellId);
    QVERIFY(!links.first().unresolved);
  }

  // 多井分层文件：能解析的井名挂接；未匹配井名也各留一条 unresolved 链接
  //（实体 id 留空、备注记名），不再静默丢掉或挂 aux（§3 修订）。
  void topsFileLeavesUnmatchedNamesUnresolved()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString dir = tmp.filePath(QString::fromUtf8("井分层"));
    QVERIFY(QDir().mkpath(dir));
    const QString topsPath = QDir(dir).filePath(QStringLiteral("tops_mixed.dat"));
    QVERIFY(writeFile(topsPath, QByteArrayLiteral(
        "#WellTops File From SMI\n"
        "#WellName    Name         MD\n"
        "A1           D61          2148.000\n"
        "Ghost9       D61          2150.000\n")));

    QString err;
    const QString assetId = svc.importProjectFile(topsPath, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 1);
    const auto links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 2);
    bool sawResolved = false, sawUnresolved = false;
    for (const EntityAssetLink &l : links)
    {
      QCOMPARE(l.role, QStringLiteral("tops"));
      QCOMPARE(l.entityType, QStringLiteral("well"));
      if (!l.unresolved)
      {
        QCOMPARE(l.entityId, QStringLiteral("well-A1"));
        sawResolved = true;
      }
      else
      {
        QVERIFY(l.entityId.isEmpty());
        QVERIFY(l.note.contains(QStringLiteral("ghost9")));
        sawUnresolved = true;
      }
    }
    QVERIFY(sawResolved && sawUnresolved);
  }

  // 井口行恰好匹配一口已有井 → 挂 well_head，不另建井（§3）。
  void wellHeadRowMatchingExistingWellDoesNotDuplicate()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString dir = tmp.filePath(QString::fromUtf8("井位"));
    QVERIFY(QDir().mkpath(dir));
    const QString whPath = QDir(dir).filePath(QStringLiteral("heads.dat"));
    QVERIFY(writeFile(whPath, QByteArrayLiteral(
        "#WellHead File From SMI\n"
        "#Name      X     Y     KB    TotalDepth\n"
        "A-1        100.0 200.0 0.0   2000.0\n"   // 规范化 a1 → 命中已有 well-A1
        "B9         300.0 400.0 0.0   2100.0\n"))); // 无匹配 → 新建

    QString err;
    const QString assetId = svc.importProjectFile(whPath, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2); // A1 + 新建 B9
    const auto links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 2);
    QStringList linked;
    for (const EntityAssetLink &l : links)
    {
      QCOMPARE(l.role, QStringLiteral("well_head"));
      QVERIFY(!l.unresolved);
      linked.append(l.entityId);
    }
    linked.sort();
    QCOMPARE(linked, QStringList({QStringLiteral("well-A1"), QStringLiteral("well-B9")}));
    QCOMPARE(cat->entityById(QStringLiteral("well-A1")).name, QStringLiteral("A1"));
  }

  // 井口文件内同一规范化井名出现两行 → 两行都 unresolved，不建井（§3）。
  void wellHeadDuplicateNormalizedNameStaysUnresolved()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString dir = tmp.filePath(QString::fromUtf8("井位"));
    QVERIFY(QDir().mkpath(dir));
    const QString whPath = QDir(dir).filePath(QStringLiteral("heads.dat"));
    QVERIFY(writeFile(whPath, QByteArrayLiteral(
        "#WellHead File From SMI\n"
        "#Name      X     Y     KB    TotalDepth\n"
        "X9         100.0 200.0 0.0   2000.0\n"
        "X-9        110.0 210.0 0.0   2000.0\n"))); // 与 X9 规范化同名

    QString err;
    const QString assetId = svc.importProjectFile(whPath, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    QVERIFY(cat->entities(QStringLiteral("well")).isEmpty());
    const auto links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 2);
    for (const EntityAssetLink &l : links)
    {
      QCOMPARE(l.role, QStringLiteral("well_head"));
      QVERIFY(l.unresolved);
      QVERIFY(l.entityId.isEmpty());
      QVERIFY(l.note.contains(QStringLiteral("x9")));
    }
  }

  // 井口行同时匹配两口已有井 → 标 unresolved，实体 id 留空，不新建不合并（§3）。
  void wellHeadRowMatchingTwoWellsStaysUnresolved()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithAliasWells(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString dir = tmp.filePath(QString::fromUtf8("井位"));
    QVERIFY(QDir().mkpath(dir));
    const QString whPath = QDir(dir).filePath(QStringLiteral("heads.dat"));
    QVERIFY(writeFile(whPath, QByteArrayLiteral(
        "#WellHead File From SMI\n"
        "#Name      X     Y     KB    TotalDepth\n"
        "dup        100.0 200.0 0.0   2000.0\n")));

    QString err;
    const QString assetId = svc.importProjectFile(whPath, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2);
    const auto links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QVERIFY(links.front().unresolved);
    QVERIFY(links.front().entityId.isEmpty());
    // D12：同名双候选——备注靠实体 id 消歧
    QVERIFY(links.front().note.contains(QStringLiteral("well-X1")));
    QVERIFY(links.front().note.contains(QStringLiteral("well-X2")));
  }

  // D61 → RAW + DERIVED 栅格（父版本指向 RAW）+ 图层声明；authid 非 4326；
  // A1 落点压在栅格非空像元上（半像元容差）。
  void horizonDerivedRasterAndCrs()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    const QString staged = stageFixture(tmp, QString::fromUtf8("层位"),
                                        QStringLiteral("D61_sample.dat"), QStringLiteral("D61.dat"));
    QVERIFY(!staged.isEmpty());
    const QString assetId = svc.importProjectFile(staged, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    DataCatalog *cat = svc.catalog();
    const CatalogEntity sb = cat->entityById(QStringLiteral("sb-D61"));
    QCOMPARE(sb.entityType, QStringLiteral("sequence_boundary"));
    QVERIFY(!sb.extra.contains(QStringLiteral("pending")));

    const QVector<CatalogVersion> versions = cat->versionsForAsset(assetId);
    QCOMPARE(versions.size(), 2);
    const CatalogVersion raw = cat->currentVersion(assetId);
    QCOMPARE(raw.stage, QStringLiteral("DERIVED"));
    QCOMPARE(raw.versionNumber, 2);
    QCOMPARE(raw.parentVersionIds.size(), 1);
    QVERIFY(raw.extra.contains(QStringLiteral("collisions")));

    // 图层清单只登记要画的结果
    const QVector<LayerDeclaration> decls = stack->manifest->all();
    QCOMPARE(decls.size(), 1);
    QCOMPARE(decls.at(0).layerId, QStringLiteral("horizon.D61"));
    QCOMPARE(decls.at(0).horizon, QStringLiteral("D61"));
    QCOMPARE(decls.at(0).type, QStringLiteral("raster"));

    // 实例化后图层 CRS 不是 EPSG:4326
    QgsMapLayer *layer = stack->layerSvc->instantiate(QStringLiteral("horizon.D61"), &err);
    QVERIFY2(layer != nullptr, qPrintable(err));
    const QString authid = layer->crs().authid();
    QVERIFY2(authid != QLatin1String("EPSG:4326"),
             qPrintable(QStringLiteral("authid=") + authid));

    // A1 (5288.67, 8219.94) 压在 D61 栅格非空像元（半像元容差 → 相邻格都查）。
    GDALAllRegister();
    GDALDatasetH ds = GDALOpen(decls.at(0).source.toUtf8().constData(), GA_ReadOnly);
    QVERIFY2(ds, "open derived raster");
    double gt[6] = {0, 0, 0, 0, 0, 0};
    GDALGetGeoTransform(ds, gt);
    const double px = (5288.67 - gt[0]) / gt[1];
    const double py = (8219.94 - gt[3]) / gt[5];
    int hasNd = 0;
    const double nd = GDALGetRasterNoDataValue(GDALGetRasterBand(ds, 1), &hasNd);
    float v1 = 0, v2 = 0, v3 = 0, v4 = 0;
    const int pxl = qBound(0, int(std::floor(px)), GDALGetRasterXSize(ds) - 1);
    const int pyl = qBound(0, int(std::floor(py)), GDALGetRasterYSize(ds) - 1);
    QVERIFY(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, pxl, pyl, 1, 1, &v1, 1, 1, GDT_Float32, 0, 0) == CE_None);
    QVERIFY(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, qMin(pxl + 1, GDALGetRasterXSize(ds) - 1), pyl, 1, 1,
                 &v2, 1, 1, GDT_Float32, 0, 0) == CE_None);
    QVERIFY(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, pxl, qMin(pyl + 1, GDALGetRasterYSize(ds) - 1), 1, 1,
                 &v3, 1, 1, GDT_Float32, 0, 0) == CE_None);
    QVERIFY(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, qMin(pxl + 1, GDALGetRasterXSize(ds) - 1),
                 qMin(pyl + 1, GDALGetRasterYSize(ds) - 1), 1, 1, &v4, 1, 1, GDT_Float32, 0, 0) == CE_None);
    GDALClose(ds);
    QVERIFY2(hasNd && (v1 != nd || v2 != nd || v3 != nd || v4 != nd),
             "A1 must sit on a filled D61 cell (half-pixel tolerance)");
  }

  // 文件名不在 8 界面集合 → 未决层位实体 + unresolved 关联，不进图层清单。
  void unknownHorizonStaysPending()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    const QString src = stageFixture(tmp, QString::fromUtf8("层位"),
                                      QStringLiteral("D61_sample.dat"), QStringLiteral("ZZ9.dat"));
    QVERIFY(!src.isEmpty());
    QString err;
    const QString assetId = svc.importProjectFile(src, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    const CatalogEntity sb = cat->entityById(QStringLiteral("sb-ZZ9"));
    QCOMPARE(sb.entityType, QStringLiteral("sequence_boundary"));
    QVERIFY(sb.extra.value(QStringLiteral("pending")).toBool());
    const QVector<EntityAssetLink> links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QVERIFY(links.front().unresolved);
    QCOMPARE(stack->manifest->all().size(), 0); // 不进编图 chip/地图
  }

  // 地震外链：版本 managed=false + 源绝对路径；survey 几何冻结；源缺失可检测。
  void seismicExternalLinkAndGeometry()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    const QString sgy = tmp.filePath(QStringLiteral("200P_mini.sgy"));
    QVERIFY(QFile::copy(fixture(QStringLiteral("mini_seismic.sgy")), sgy));
    const QString assetId = svc.importProjectFile(sgy, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    DataCatalog *cat = svc.catalog();
    const CatalogVersion v = cat->currentVersion(assetId);
    QVERIFY(!v.managed);
    QVERIFY(QFileInfo(v.path).isAbsolute());
    // §3：外链也留入库时 SHA-256（流式算的源文件摘要），打开时照它校验。
    QCOMPARE(v.sha256, sha256OfFile(sgy));
    QVERIFY(cat->verifyExternalVersionSha(v));
    QCOMPARE(svc.absolutePath(assetId), v.path);

    const CatalogEntity survey = cat->entityById(QStringLiteral("survey-200P_mini"));
    QCOMPARE(survey.entityType, QStringLiteral("seismic_survey"));
    QCOMPARE(survey.inlineMin, 1000.0);
    QCOMPARE(survey.inlineMax, 1002.0);
    QCOMPARE(survey.sampleIntervalUs, 2000.0);
    QCOMPARE(cat->linksForAsset(assetId).front().role, QStringLiteral("seismic_volume"));

    // 外链缺失检测（预览「找不到源文件」的判定基础）
    QVERIFY(QFile::exists(v.path));
    QFile::remove(v.path);
    QVERIFY(!QFile::exists(v.path));
  }

  // §3 外链篡改：源文件字节变了 → 打开时 SHA-256 校验失败、报固定文案，不解码。
  void externalShaMismatchBlocksOpen()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    const QString sgy = tmp.filePath(QStringLiteral("vol.sgy"));
    QVERIFY(QFile::copy(fixture(QStringLiteral("mini_seismic.sgy")), sgy));
    const QString assetId = svc.importProjectFile(sgy, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    DataCatalog *cat = svc.catalog();
    const CatalogVersion v = cat->currentVersion(assetId);
    QVERIFY(!v.managed);
    QVERIFY(cat->verifyExternalVersionSha(v)); // 未动过 → 校验通过

    // 尾部加一个字节 → 摘要变了
    QFile f(sgy);
    QVERIFY(f.open(QIODevice::Append));
    QCOMPARE(f.write("X"), 1);
    f.close();
    QString verr;
    QVERIFY(!cat->verifyExternalVersionSha(v, &verr));
    QCOMPARE(verr, QStringLiteral("源文件与入库时的 SHA-256 不一致"));
  }

  // §3 dedup：同一 SHA-256 再导入 → 不新建资产/版本/主关联，报「字节已在库」。
  void sameShaReimportDoesNotDuplicate()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err).isEmpty());
    const QString assetId = svc.importProjectFile(fixture(QStringLiteral("A1.Las")), &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    DataCatalog *cat = svc.catalog();
    const int assetCount = cat->assets().size();
    int versionCount = 0;
    for (const CatalogAsset &a : cat->assets())
      versionCount += cat->versionsForAsset(a.id).size();

    const DataImportService::ImportResult res =
        svc.importProjectFileEx(fixture(QStringLiteral("A1.Las")), &err);
    QVERIFY2(res.outcome == DataImportService::ImportOutcome::AlreadyStored,
             qPrintable(res.message));
    QCOMPARE(res.assetId, assetId); // 指回已存在资产
    QVERIFY(!res.linkAttached);
    QVERIFY(res.message.contains(QStringLiteral("字节已在库")));
    QVERIFY(res.message.contains(QStringLiteral("没有新的关联")));

    QCOMPARE(cat->assets().size(), assetCount); // 没有新资产
    int versionCount2 = 0;
    for (const CatalogAsset &a : cat->assets())
      versionCount2 += cat->versionsForAsset(a.id).size();
    QCOMPARE(versionCount2, versionCount); // 没有新版本

    // A1 的 well_log 仍只有一条主关联——没有第二条。
    int primaryLogs = 0, totalLogs = 0;
    for (const EntityAssetLink &l : cat->linksForEntity(QStringLiteral("well-A1")))
      if (l.role == QLatin1String("well_log"))
      {
        ++totalLogs;
        if (l.isPrimary)
          ++primaryLogs;
      }
    QCOMPARE(totalLogs, 1);
    QCOMPARE(primaryLogs, 1);
  }

  void missingManagedRawCanBeReimported()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err).isEmpty());
    const QString firstId = svc.importProjectFile(fixture(QStringLiteral("A1.Las")), &err);
    QVERIFY2(!firstId.isEmpty(), qPrintable(err));
    const QString oldPath = svc.absolutePath(firstId);
    QVERIFY(removeManagedFile(oldPath));
    const DataImportService::ImportResult retry =
        svc.importProjectFileEx(fixture(QStringLiteral("A1.Las")), &err);
    QCOMPARE(retry.outcome, DataImportService::ImportOutcome::Imported);
    QVERIFY(retry.assetId != firstId);
    const QString recovered = svc.absolutePath(retry.assetId);
    QVERIFY(QFileInfo::exists(recovered));
    QCOMPARE(sha256OfFile(recovered), sha256OfFile(fixture(QStringLiteral("A1.Las"))));
  }

  // #155：上次提交「rename 落位后、applyJournal 前」崩溃留下的受管孤儿 +
  // staging 残留。重开工程后导入必须成功落到不冲突路径，staging 被清扫。
  void orphanedManagedFilesFromCrashedCommitDoNotBlockImport()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    const QString orphanDir = QDir(projectDir).filePath(QStringLiteral("artifacts/raw/ast-1/ver-1"));
    const QString staleStaging =
        QDir(projectDir).filePath(QStringLiteral("artifacts/staging/dead-session/raw/ast-1/ver-1"));
    QVERIFY(QDir().mkpath(orphanDir));
    QVERIFY(QDir().mkpath(staleStaging));
    QVERIFY(QFile::copy(fixture(QStringLiteral("A1.Las")), QDir(orphanDir).filePath(QStringLiteral("A1.Las"))));
    QVERIFY(QFile::copy(fixture(QStringLiteral("A1.Las")), QDir(staleStaging).filePath(QStringLiteral("A1.Las"))));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    QVERIFY(!QDir(QDir(projectDir).filePath(QStringLiteral("artifacts/staging/dead-session"))).exists());
    DataImportService &svc = *stack->importSvc;
    QString err;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
      const DataImportService::ImportResult res =
          svc.importProjectFileEx(fixture(QStringLiteral("A1.Las")), &err);
      if (attempt == 0)
      {
        QVERIFY2(res.outcome == DataImportService::ImportOutcome::Imported, qPrintable(err));
        QVERIFY(res.assetId != QLatin1String("ast-1"));
        const QString path = svc.absolutePath(res.assetId);
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(!path.contains(QStringLiteral("/ast-1/ver-1/")));
      }
      else
      {
        QCOMPARE(res.outcome, DataImportService::ImportOutcome::AlreadyStored);
      }
    }
    // 孤儿不删（只跳号），留给人工核对。
    QVERIFY(QFileInfo::exists(QDir(orphanDir).filePath(QStringLiteral("A1.Las"))));
  }

  void sweepStaleStagingRemovesLeftovers()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.path();
    QCOMPARE(DataImportService::sweepStaleStaging(projectDir), 0);
    QVERIFY(QDir().mkpath(QDir(projectDir).filePath(QStringLiteral("artifacts/staging/a/raw"))));
    QVERIFY(QDir().mkpath(QDir(projectDir).filePath(QStringLiteral("artifacts/staging/b"))));
    QCOMPARE(DataImportService::sweepStaleStaging(projectDir), 2);
    QVERIFY(!QDir(QDir(projectDir).filePath(QStringLiteral("artifacts/staging"))).exists());
  }

  void missingDerivedHorizonCanBeRebuilt()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    const QString source = stageFixture(tmp, QString::fromUtf8("层位"),
                                        QStringLiteral("D61_sample.dat"), QStringLiteral("D61.dat"));
    QVERIFY(!source.isEmpty());
    DataImportService &svc = *stack->importSvc;
    QString err;
    const QString assetId = svc.importProjectFile(source, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    const CatalogVersion oldDerived = svc.catalog()->currentVersion(assetId);
    QVERIFY(removeManagedFile(svc.absolutePathForVersion(oldDerived)));
    const DataImportService::ImportResult retry = svc.importProjectFileEx(source, &err);
    QCOMPARE(retry.outcome, DataImportService::ImportOutcome::AlreadyStored);
    QCOMPARE(retry.assetId, assetId);
    const CatalogVersion rebuilt = svc.catalog()->currentVersion(assetId);
    QVERIFY(rebuilt.id != oldDerived.id);
    QVERIFY(QFileInfo::exists(svc.absolutePathForVersion(rebuilt)));
    const QVector<LayerDeclaration> declared = stack->manifest->all();
    QVERIFY(std::any_of(declared.cbegin(), declared.cend(), [&](const LayerDeclaration &d) {
      return d.layerId == QStringLiteral("horizon.D61") &&
             d.source == svc.absolutePathForVersion(rebuilt);
    }));
  }

  void managedImportRejectsSymlinkedDestination()
  {
    QTemporaryDir tmp;
    QTemporaryDir outside;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    const QString parent = QDir(projectDir).filePath(QStringLiteral("artifacts/raw/ast-1"));
    QVERIFY(QDir().mkpath(parent));
    QVERIFY(QFile::link(outside.path(), QDir(parent).filePath(QStringLiteral("ver-1"))));
    QString error;
    const DataImportService::ImportResult result =
        stack->importSvc->importProjectFileEx(fixture(QStringLiteral("A1.Las")), &error);
    QCOMPARE(result.outcome, DataImportService::ImportOutcome::Failed);
    QVERIFY(!QFileInfo::exists(outside.filePath(QStringLiteral("A1.Las"))));
  }

  // 内容井名首次导入即归属；重导同字节幂等，不重复建井或追加关联。
  void reimportContentIdentifiedLogKeepsExistingAssociation()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack);
    DataImportService &svc = *stack->importSvc;
    const QString path = tmp.filePath(QStringLiteral("Ghost.Las"));
    QVERIFY(writeFile(path, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. Ghost9 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    QString error;
    const QString asset = svc.importProjectFile(path, &error);
    QVERIFY2(!asset.isEmpty(), qPrintable(error));
    auto *cat = svc.catalog();
    const auto wells = cat->wellsMatchingName(QStringLiteral("Ghost9"));
    QCOMPARE(wells.size(), 1);
    const int linksBefore = cat->links().size();
    const auto result = svc.importProjectFileEx(path, &error);
    QCOMPARE(result.outcome, DataImportService::ImportOutcome::AlreadyStored);
    QCOMPARE(result.assetId, asset);
    QVERIFY(!result.linkAttached);
    QVERIFY(result.message.contains(QStringLiteral("字节已在库")));
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2);
    QCOMPARE(cat->assets().size(), 1);
    QCOMPARE(cat->links().size(), linksBefore);
    const auto links = cat->linksForAsset(asset);
    QCOMPARE(links.size(), 1);
    QVERIFY(!links.first().unresolved);
    QCOMPARE(links.first().entityId, wells.first());
    QVERIFY(links.first().isPrimary);
    DataCatalog reloaded;
    QVERIFY(reloaded.open(projectDir, &error));
    QCOMPARE(reloaded.linksForAsset(asset).size(), 1);
    QCOMPARE(reloaded.linksForAsset(asset).first().entityId, wells.first());
  }

  // 导入时命名：ImportOptions.displayName 非空 → 资产显示名按用户给定；
  // 版本物理 fileName 恒为原文件名；空 displayName → 回退原文件 basename。
  void importDisplayNameOptionHonored()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString src = tmp.filePath(QStringLiteral("A9_log.las"));
    QVERIFY(writeFile(src, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    DataImportService::ImportOptions opts;
    opts.displayName = QStringLiteral("A9 主测井曲线");
    QString err;
    const auto res = svc.importProjectFileEx(src, opts, &err);
    QVERIFY2(res.outcome != DataImportService::ImportOutcome::Failed, qPrintable(err));
    QCOMPARE(svc.catalog()->assetById(res.assetId).displayName,
             QStringLiteral("A9 主测井曲线"));
    // 版本物理文件名保持原文件名
    QCOMPARE(svc.catalog()->currentVersion(res.assetId).fileName,
             QStringLiteral("A9_log.las"));

    // 空 displayName → 回退原文件名
    const QString src2 = tmp.filePath(QStringLiteral("B2_log.las"));
    QVERIFY(writeFile(src2, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n200.0 60.0\n")));
    const auto res2 = svc.importProjectFileEx(src2, {}, &err);
    QVERIFY2(res2.outcome != DataImportService::ImportOutcome::Failed, qPrintable(err));
    QCOMPARE(svc.catalog()->assetById(res2.assetId).displayName,
             QStringLiteral("B2_log.las"));
  }

  // 批量确认表「显示名」列（nameOverrides）：经 plan 贯通到资产 displayName；
  // 空白 override 忽略回退原文件 basename；版本物理 fileName 恒不动。
  void batchNameOverridesApplied()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString a = tmp.filePath(QStringLiteral("E5_log.las"));
    QVERIFY(writeFile(a, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n300.0 70.0\n")));
    const QString b = tmp.filePath(QStringLiteral("F6_log.las"));
    QVERIFY(writeFile(b, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n400.0 80.0\n")));

    QString err;
    const auto rows = svc.importFiles({a, b}, &err, {}, {}, {},
                                      {{a, QStringLiteral("E5 井 * 新名字")},
                                       {b, QStringLiteral("   ")}});
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).outcome, FolderRowResult::Outcome::Imported);
    QCOMPARE(rows.at(1).outcome, FolderRowResult::Outcome::Imported);
    // 编辑过的显示名生效；空白 override 回退原文件 basename
    QCOMPARE(svc.catalog()->assetById(rows.at(0).assetId).displayName,
             QStringLiteral("E5 井 * 新名字"));
    QCOMPARE(svc.catalog()->assetById(rows.at(1).assetId).displayName,
             QStringLiteral("F6_log.las"));
    // 物理文件名不随显示名改动
    QCOMPARE(svc.catalog()->currentVersion(rows.at(0).assetId).fileName,
             QStringLiteral("E5_log.las"));
  }

  // 新 SHA-256 仍追加新资产。已决 well_log 不再把后导入的那份顶成唯一
  // 主关联：首份保持 isPrimary / ordinal 0，后份非主 / ordinal 1，两条都在。
  void newShaVersionDemotesPreviousPrimaryLink()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err).isEmpty());
    const QString oldAsset = svc.importProjectFile(fixture(QStringLiteral("A1.Las")), &err);
    QVERIFY2(!oldAsset.isEmpty(), qPrintable(err));

    // 同井新字节 → 新资产；主关联仍是先导入的那份。
    const QString v2 = tmp.filePath(QStringLiteral("A1_v2.las"));
    QVERIFY(QFile::copy(fixture(QStringLiteral("A1.Las")), v2));
    {
      QFile f(v2);
      QVERIFY(f.open(QIODevice::Append));
      QCOMPARE(f.write("\n"), 1);
    }
    const QString newAsset = svc.importProjectFile(v2, &err);
    QVERIFY2(!newAsset.isEmpty(), qPrintable(err));
    QVERIFY(newAsset != oldAsset);

    DataCatalog *cat = svc.catalog();
    QVERIFY(!cat->assetById(oldAsset).id.isEmpty());
    QVERIFY(!cat->assetById(newAsset).id.isEmpty());
    QCOMPARE(cat->linksForAsset(oldAsset).size(), 1);
    QCOMPARE(cat->linksForAsset(newAsset).size(), 1);
    const EntityAssetLink oldLink = cat->linksForAsset(oldAsset).front();
    const EntityAssetLink newLink = cat->linksForAsset(newAsset).front();
    QVERIFY(!oldLink.unresolved);
    QVERIFY(!newLink.unresolved);
    QVERIFY(oldLink.isPrimary);
    QCOMPARE(oldLink.ordinal, 0);
    QVERIFY(!newLink.isPrimary);
    QCOMPARE(newLink.ordinal, 1);
    int resolvedLogs = 0;
    int primaryLogs = 0;
    for (const EntityAssetLink &l : cat->linksForEntity(QStringLiteral("well-A1")))
    {
      if (l.role != QLatin1String("well_log") || l.unresolved)
        continue;
      ++resolvedLogs;
      if (l.isPrimary)
        ++primaryLogs;
    }
    QCOMPARE(resolvedLogs, 2); // 降级不从链接表摘掉曲线
    QCOMPARE(primaryLogs, 1);
  }

  // Oracle 3a：catalog 里先有一口同名井。连续导入两份不同内容的 LAS，
  // 两条已决 well_log 共存；第一份为主且 ordinal 0，第二份非主且 ordinal 1。
  void consecutiveResolvedLasKeepsFirstPrimary()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString firstPath = tmp.filePath(QStringLiteral("A1_first.las"));
    const QString secondPath = tmp.filePath(QStringLiteral("A1_second.las"));
    QVERIFY(writeFile(firstPath, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    QVERIFY(writeFile(secondPath, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n250.5 50.0\n")));

    QString err;
    const QString firstId = svc.importProjectFile(firstPath, &err);
    QVERIFY2(!firstId.isEmpty(), qPrintable(err));
    const QString secondId = svc.importProjectFile(secondPath, &err);
    QVERIFY2(!secondId.isEmpty(), qPrintable(err));
    QVERIFY(firstId != secondId);

    DataCatalog *cat = svc.catalog();
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 1);
    QVERIFY(!cat->assetById(firstId).id.isEmpty());
    QVERIFY(!cat->assetById(secondId).id.isEmpty());
    QCOMPARE(cat->linksForAsset(firstId).size(), 1);
    QCOMPARE(cat->linksForAsset(secondId).size(), 1);

    const EntityAssetLink first = cat->linksForAsset(firstId).front();
    const EntityAssetLink second = cat->linksForAsset(secondId).front();
    QCOMPARE(first.entityId, QStringLiteral("well-A1"));
    QCOMPARE(second.entityId, QStringLiteral("well-A1"));
    QCOMPARE(first.role, QStringLiteral("well_log"));
    QCOMPARE(second.role, QStringLiteral("well_log"));
    QVERIFY(!first.unresolved);
    QVERIFY(!second.unresolved);
    QVERIFY(first.isPrimary);
    QCOMPARE(first.ordinal, 0);
    QVERIFY(!second.isPrimary);
    QCOMPARE(second.ordinal, 1);

    int resolvedLogs = 0;
    for (const EntityAssetLink &l : cat->linksForEntity(QStringLiteral("well-A1")))
      if (l.role == QLatin1String("well_log") && !l.unresolved)
        ++resolvedLogs;
    QCOMPARE(resolvedLogs, 2);
  }

  // 内容明确的两份不同井 LAS 各自创建独立井，首份均为主文件，不产生未决关联。
  void consecutiveContentNamedLasCreateDistinctWells()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString firstPath = tmp.filePath(QStringLiteral("Ghost9.las"));
    const QString secondPath = tmp.filePath(QStringLiteral("OtherWell.las"));
    QVERIFY(writeFile(firstPath, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. Ghost9 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    QVERIFY(writeFile(secondPath, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. OtherWell : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n200.0 50.0\n")));

    QString err;
    const QString firstId = svc.importProjectFile(firstPath, &err);
    QVERIFY2(!firstId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 1);
    QCOMPARE(cat->linksForAsset(firstId).size(), 1);
    QVERIFY(!cat->linksForAsset(firstId).front().unresolved);
    QCOMPARE(cat->entityById(cat->linksForAsset(firstId).front().entityId).name, QStringLiteral("Ghost9"));

    const QString secondId = svc.importProjectFile(secondPath, &err);
    QVERIFY2(!secondId.isEmpty(), qPrintable(err));
    QVERIFY(secondId != firstId);
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2);
    QCOMPARE(cat->assets().size(), 2);
    QCOMPARE(cat->unresolvedLinks().size(), 0);
    for (const QString &id : {firstId, secondId})
    {
      const QVector<EntityAssetLink> links = cat->linksForAsset(id);
      QCOMPARE(links.size(), 1);
      QVERIFY(!links.front().unresolved);
      QVERIFY(!links.front().entityId.isEmpty());
      QVERIFY(links.front().isPrimary);
      QCOMPARE(links.front().role, QStringLiteral("well_log"));
      QCOMPARE(links.front().ordinal, 0); // 两口不同井各自从首份编号
    }
    QCOMPARE(cat->entityById(cat->linksForAsset(firstId).front().entityId).name, QStringLiteral("Ghost9"));
    QCOMPARE(cat->entityById(cat->linksForAsset(secondId).front().entityId).name, QStringLiteral("OtherWell"));
    QVERIFY(cat->linksForAsset(firstId).front().entityId != cat->linksForAsset(secondId).front().entityId);
  }

  // §3 路径卫生：文件名含换行/控制字符或 ".." → 这一行如实失败，不入库。
  void unsafeFileNameRejected()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

#ifdef Q_OS_WIN
    // Windows 文件系统本身禁用换行/控制字符——夹具无法落盘；改为直接对
    // 路径段校验器断言（导入链最终消费的就是它）。POSIX 侧走完整导入链。
    QString err;
    QVERIFY(DataCatalog::managedPath(QStringLiteral("raw"), QStringLiteral("ast"),
                                    QStringLiteral("ver"),
                                    QStringLiteral("bad\nname.las")).isEmpty());
    QVERIFY(DataCatalog::managedPath(QStringLiteral("raw"), QStringLiteral("ast"),
                                    QStringLiteral("ver"),
                                    QStringLiteral("bad\tname.las")).isEmpty());
#else
    // 换行
    const QString nl = tmp.filePath(QStringLiteral("bad\nname.las"));
    QVERIFY(writeFile(nl, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A9 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n1 50.0\n")));
    QString err;
    QVERIFY(svc.importProjectFile(nl, &err).isEmpty());
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains(QStringLiteral("路径段")));

    // 控制字符（Tab）
    const QString tab = tmp.filePath(QStringLiteral("bad\tname.las"));
    QVERIFY(writeFile(tab, QByteArrayLiteral("~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A9 : WELL\n"
                      "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    QVERIFY(svc.importProjectFile(tab, &err).isEmpty());
    QVERIFY(!err.isEmpty());
#endif

    // ".."
    const QString dd = tmp.filePath(QStringLiteral("..evil.las"));
    QVERIFY(writeFile(dd, QByteArrayLiteral("~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A9 : WELL\n"
                      "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    QVERIFY(svc.importProjectFile(dd, &err).isEmpty());
    QVERIFY(!err.isEmpty());

    QVERIFY(svc.assets().isEmpty()); // 一个都没登记
    QVERIFY(stack->manifest->all().isEmpty());
  }

  // 阶段 D：GeoJSON 图例字典（相/亚相/微相 distinct 值）落辅助实体。
  void geojsonLegendDictionaryCollected()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    const QString assetId = svc.importProjectFile(fixture(QStringLiteral("facies.geojson")), &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    const auto links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.front().role, QStringLiteral("reference"));
    const CatalogEntity aux = cat->entityById(links.front().entityId);
    QCOMPARE(aux.entityType, QStringLiteral("auxiliary"));
    QVERIFY(!aux.extra.value(QStringLiteral("georeferenced")).toBool());
    const QVariantMap legend = aux.extra.value(QStringLiteral("legend")).toMap();
    const QStringList facies = legend.value(QString::fromUtf8("相")).toStringList();
    const QStringList subFacies = legend.value(QString::fromUtf8("亚相")).toStringList();
    QCOMPARE(facies.size(), 2);
    QVERIFY(facies.contains(QString::fromUtf8("三角洲前缘")));
    QVERIFY(facies.contains(QString::fromUtf8("滨浅湖")));
    QCOMPARE(subFacies.size(), 2);
    QVERIFY(subFacies.contains(QString::fromUtf8("河口坝")));
    QVERIFY(subFacies.contains(QString::fromUtf8("滩坝")));
    // 未配准：不生成地图图层
    QVERIFY(stack->manifest->all().isEmpty());

    // #588（B.3）：图例字典整读 + 全量 DOM（内存常为文件 3-10 倍）此前无大小
    // 闸——数百 MB～GB 级 .geojson 走导入即 OOM/冻结。收紧共用大文件闸后
    // 导入本身不受影响（aux/reference 照常登记），只是不建图例字典（阶段 D
    // 的附带产物，与打不开文件时同属 best-effort）。
    {
      const QString gatedPath = tmp.filePath(QStringLiteral("facies_gated.geojson"));
      {
        QFile f(gatedPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArrayLiteral(
            "{\"type\":\"FeatureCollection\",\"features\":[{\"type\":\"Feature\","
            "\"properties\":{\"相\":\"三角洲前缘\"},\"geometry\":{\"type\":\"Point\","
            "\"coordinates\":[0,0]}}]}"));
        f.close();
      }
      err.clear();
      const qint64 kRestoreLimit = LasParser::fileSizeLimit();
      LasParser::setFileSizeLimit(8);
      const QString gatedAssetId = svc.importProjectFile(gatedPath, &err);
      LasParser::setFileSizeLimit(kRestoreLimit);
      QVERIFY2(!gatedAssetId.isEmpty(), qPrintable(err));
      const auto gatedLinks = cat->linksForAsset(gatedAssetId);
      QCOMPARE(gatedLinks.size(), 1);
      const CatalogEntity gatedAux = cat->entityById(gatedLinks.front().entityId);
      QVERIFY2(!gatedAux.extra.contains(QStringLiteral("legend")),
               "超限的 GeoJSON 不得整读建图例字典");
    }
  }

  // D11 手工仿射：变换只动 position 前两分量，属性原样；平移+缩放+旋转
  // 顺序（缩放→旋转→平移）按公式校验；输出带 paleo_provisional_affine。
  void geoAffineTransformProducesDerivedCopy()
  {
    QTemporaryDir tmp;
    const QString in = tmp.filePath(QStringLiteral("in.geojson"));
    {
      QFile f(in);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(R"({"type":"FeatureCollection","features":[
        {"type":"Feature","properties":{"相":"滩坝"},
         "geometry":{"type":"Polygon","coordinates":[[[0,0],[100,0],[100,50],[0,0]]]}},
        {"type":"Feature","properties":{"相":"河口坝"},
         "geometry":{"type":"Point","coordinates":[10,20,7]}}]})");
      f.close();
    }

    double b[4];
    QString err;
    QVERIFY2(geoJsonBounds(in, b, &err), qPrintable(err));
    QCOMPARE(b[0], 0.0);
    QCOMPARE(b[2], 100.0);
    QCOMPARE(b[1], 0.0);
    QCOMPARE(b[3], 50.0);

    // 单点公式：sx=2, sy=3, rot=90°, tx=1000, ty=2000
    // (10,20) → scale (20,60) → rot90 (-60,20) → +t (940,2020)
    double ox, oy;
    GeoAffineParams p{1000.0, 2000.0, 2.0, 3.0, 90.0};
    geoAffineApply(p, 10.0, 20.0, &ox, &oy);
    QVERIFY(qAbs(ox - 940.0) < 1e-9 && qAbs(oy - 2020.0) < 1e-9);

    const QString out = tmp.filePath(QStringLiteral("out.geojson"));
    int feats = 0;
    double db[4];
    QVERIFY2(geoAffineTransformFile(in, out, p, &err, &feats, db), qPrintable(err));
    QCOMPARE(feats, 2);
    // 变换后 bbox：x' = -3y+1000 ∈ [850,1000]，y' = 2x+2000 ∈ [2000,2200]
    QVERIFY(qAbs(db[0] - 850.0) < 1e-6 && qAbs(db[2] - 1000.0) < 1e-6);
    QVERIFY(qAbs(db[1] - 2000.0) < 1e-6 && qAbs(db[3] - 2200.0) < 1e-6);

    double rereadBounds[4];
    QVERIFY2(geoJsonBounds(out, rereadBounds, &err), qPrintable(err));
    for (int i = 0; i < 4; ++i)
      QCOMPARE(rereadBounds[i], db[i]);

    QFile rf(out);
    QVERIFY(rf.open(QIODevice::ReadOnly));
    const QJsonDocument doc = QJsonDocument::fromJson(rf.readAll());
    QVERIFY(doc.isObject());
    const QJsonObject root = doc.object();
    QVERIFY(root.contains(QStringLiteral("paleo_provisional_affine")));
    const QJsonArray feats2 = root.value(QStringLiteral("features")).toArray();
    QCOMPARE(feats2.size(), 2);
    // 属性原样 + Point 的第三分量（高程）不动。
    QCOMPARE(feats2.at(0).toObject().value(QStringLiteral("properties"))
                 .toObject().value(QString::fromUtf8("相")).toString(),
             QString::fromUtf8("滩坝"));
    const QJsonArray pt = feats2.at(1).toObject().value(QStringLiteral("geometry"))
                              .toObject().value(QStringLiteral("coordinates")).toArray();
    QCOMPARE(pt.size(), 3);
    QCOMPARE(pt.at(2).toDouble(), 7.0);
    // 空要素集拒绝
    const QString empty = tmp.filePath(QStringLiteral("empty.geojson"));
    {
      QFile f(empty);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(R"({"type":"FeatureCollection","features":[]})");
    }
    QVERIFY(!geoAffineTransformFile(empty, out + QStringLiteral("x"), p, &err));
    // 坏文件拒绝
    QVERIFY(!geoAffineTransformFile(out + QStringLiteral("none"), out, p, &err));
  }

  // 阶段 D：参考资料目录 / HZ28-6-1 命名的 XML 固定辅助参考，不按内容挂井。
  void hz28XmlStaysAuxiliaryReference()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    // 预置井 A1，验证不会并进去
    QVERIFY(seedCatalogWithSingleWell(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString refDir = tmp.filePath(QString::fromUtf8("参考资料"));
    QVERIFY(QDir().mkpath(refDir));
    const QString xmlPath = QDir(refDir).filePath(
        QStringLiteral("HZ28-6-1井综合柱状图-2021-沉积-地化室-未钻遇烃源岩层-测井-惠州勘探室.xml"));
    // 内容故意写成测井 XML——也不允许按内容挂井
    QVERIFY(writeFile(xmlPath, QByteArrayLiteral(
        "<logs><log><logcurveinfo/><logdata>1 2</logdata></log></logs>")));

    QString err;
    const QString assetId = svc.importProjectFile(xmlPath, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    // 井实体只有预置的一口（没有 HZ28 井、没有挂到 A1）
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 1);
    const auto links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.front().entityType, QStringLiteral("auxiliary"));
    QCOMPARE(links.front().role, QStringLiteral("reference"));
    QVERIFY(!links.front().unresolved);
    QVERIFY(cat->linksForEntity(QStringLiteral("well-A1")).isEmpty());
  }

  // 缺失源 → 失败信号，无登记。
  void importMissingSourceFails()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QSignalSpy failedSpy(&svc, &DataImportService::importFailed);
    QSignalSpy importedSpy(&svc, &DataImportService::imported);

    QString err;
    const QString assetId = svc.importFile(QStringLiteral("wells"),
                                           QStringLiteral("/nonexistent/ghost.las"), &err);
    QVERIFY(assetId.isEmpty());
    QVERIFY(!err.isEmpty());
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(failedSpy.at(0).at(1).toString(), QStringLiteral("/nonexistent/ghost.las"));
    QCOMPARE(importedSpy.count(), 0);
    QVERIFY(svc.assets().isEmpty());
    QVERIFY(stack->manifest->all().isEmpty());
  }

  // 「导入工区文件夹」（§3 + autoplan-eng/dx）：递归收普通文件（分类依赖
  // 井位/井分层/时深 路径段）；井口行先处理——LAS 文件名排在井口前面也照
  // 常挂到 A1；逃逸符号链接与非普通文件标 Skipped；单行失败不中断；
  // dedup 行报「字节已在库」。
  void folderImportWellHeadsFirstAndIsolatesFailures()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    using Outcome = FolderRowResult::Outcome;

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井位"))));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井分层"))));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("时深/TD"))));

    // 文件名排在最前的 LAS：证明井口先行是两阶段而非按名序。
    const QString lasPath = QDir(root).filePath(QStringLiteral("0A1.Las"));
    QVERIFY(writeFile(lasPath, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    // 井名缺失的完整 LAS：不凭文件名猜井，逐行失败且不妨碍有效数据。
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("ghost.las")), QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    // 同井第二份 LAS：路径序在 0A1.Las 之后，非主、ordinal 顺延。两条都入库。
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("zzA1b.las")), QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n101.0 50.0\n")));
    // 井位（带 BOM 同真文件）：heads.dat 建 A1+B2；empty.dat 只有注释 → 行失败。
    QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("井位/heads.dat")),
        QByteArrayLiteral("\xEF\xBB\xBF#WellHead File From SMI\n"
                          "#Name      X     Y     KB    TotalDepth\n"
                          "A1           1.0   2.0   0.0   2000.0\n"
                          "B2           3.0   4.0   0.0   2100.0\n")));
    QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("井位/empty.dat")),
                      QByteArrayLiteral("#WellHead File From SMI\n# no rows\n")));
    // tops：A1 已决 + Ghost9 未决 → 单行入库（主关联已写）+ 未决备注。
    QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("井分层/tops.dat")),
        QByteArrayLiteral("#WellTops File From SMI\n#WellName  Name  MD\n"
                          "A1       D61   2148.0\n"
                          "Ghost9   D61   2150.0\n")));
    // 时深在 TD 二级子目录——证明递归下钻而不止一层。
    QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("时深/TD/a1.dat")),
        QByteArrayLiteral("#TimeDepth File From SMI\n# Well : A1\n"
                          "#TIME  TVDSS    TVD      MD\n"
                          "380.0  -252.6   252.6   252.6\n"
                          "382.0  -260.7   260.7   260.7\n")));

#ifndef Q_OS_WIN
    // 逃逸符号链接 → Skipped；根内符号链接 → 正常走（dedup 命中）。
    // Windows：QFile::link 生成 .lnk 快捷方式文件而非符号链接——语义不
    // 成立，两行夹具与断言整体跳过。
    const QString outside = tmp.filePath(QStringLiteral("outside.las"));
    QVERIFY(writeFile(outside, QByteArrayLiteral("x")));
    QVERIFY(QFile::link(outside, QDir(root).filePath(QStringLiteral("escape.las"))));
    QVERIFY(QFile::link(lasPath, QDir(root).filePath(QStringLiteral("mirror.Las"))));
#endif

    // 非普通文件（fifo）→ Skipped 行。
    bool madeFifo = false;
#if defined(Q_OS_UNIX)
    const QString fifoPath = QDir(root).filePath(QStringLiteral("pipe.sock"));
    madeFifo = ::mkfifo(QFile::encodeName(fifoPath).constData(), 0600) == 0;
#endif

    QString err;
    const QVector<FolderRowResult> rows = svc.importFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
#ifdef Q_OS_WIN
    QCOMPARE(rows.size(), 7 + (madeFifo ? 1 : 0));
#else
    QCOMPARE(rows.size(), 9 + (madeFifo ? 1 : 0));
#endif

    // 行序（两阶段 + 阶段内路径序）：井口行后按文件落位取索引——
    // Windows 无符号链接行，mirror/escape 缺席，其余顺移。
    int idx = 2;
    const int rLas = idx++;
    const int rGhost = idx++;
#ifndef Q_OS_WIN
    const int rMirror = idx++;
#endif
    const int rZz = idx++;
    const int rTops = idx++;
    const int rTd = idx++;
#ifndef Q_OS_WIN
    const int rEscape = idx++;
#endif

    const auto expectPath = [&rows](int i, const QString &suffix) {
      QVERIFY2(rows.at(i).path.endsWith(suffix),
               qPrintable(QStringLiteral("row %1 = %2, want *%3")
                              .arg(i).arg(rows.at(i).path, suffix)));
    };

    // 阶段 1：两个 well_head 行在最前（井位 内按路径排序：empty < heads）。
    expectPath(0, QString::fromUtf8("井位/empty.dat"));
    QCOMPARE(rows.at(0).classifiedType, QStringLiteral("well_head"));
    QCOMPARE(rows.at(0).outcome, Outcome::Failed);
    QVERIFY(rows.at(0).message.contains(QStringLiteral("井表没有可用数据行")));
    QVERIFY(rows.at(0).entityName.isEmpty());

    expectPath(1, QString::fromUtf8("井位/heads.dat"));
    QCOMPARE(rows.at(1).classifiedType, QStringLiteral("well_head"));
    QCOMPARE(rows.at(1).outcome, Outcome::Imported);
    QVERIFY(rows.at(1).entityName.contains(QStringLiteral("A1")));
    QVERIFY(rows.at(1).entityName.contains(QStringLiteral("B2")));

    // 阶段 2（路径排序）：0A1.Las、ghost.las、[mirror.Las]、井分层、时深/TD。
    expectPath(rLas, QStringLiteral("0A1.Las"));
    QCOMPARE(rows.at(rLas).classifiedType, QStringLiteral("well_log"));
    QCOMPARE(rows.at(rLas).outcome, Outcome::Imported); // 井口已先行 → 照常挂上
    QCOMPARE(rows.at(rLas).entityName, QStringLiteral("A1"));

    expectPath(rGhost, QStringLiteral("ghost.las"));
    QCOMPARE(rows.at(rGhost).outcome, Outcome::Failed);
    QVERIFY(rows.at(rGhost).entityName.isEmpty());
    QVERIFY(!rows.at(rGhost).message.isEmpty());

#ifndef Q_OS_WIN
    expectPath(rMirror, QStringLiteral("mirror.Las")); // 根内符号链接 → 处理且 dedup
    QCOMPARE(rows.at(rMirror).outcome, Outcome::Imported);
    QVERIFY(rows.at(rMirror).message.contains(QStringLiteral("字节已在库")));
#endif

    expectPath(rZz, QStringLiteral("zzA1b.las")); // 同井第二份 LAS → 入库
    QCOMPARE(rows.at(rZz).outcome, Outcome::Imported);
    QCOMPARE(rows.at(rZz).entityName, QStringLiteral("A1"));

    expectPath(rTops, QString::fromUtf8("井分层/tops.dat"));
    QCOMPARE(rows.at(rTops).classifiedType, QStringLiteral("well_stratification"));
    QCOMPARE(rows.at(rTops).outcome, Outcome::Imported); // 有主关联写出 → 入库
    QVERIFY(rows.at(rTops).entityName.contains(QStringLiteral("A1")));
    QVERIFY(rows.at(rTops).message.contains(QStringLiteral("ghost9"))); // 附未决备注

    expectPath(rTd, QString::fromUtf8("时深/TD/a1.dat"));
    QCOMPARE(rows.at(rTd).classifiedType, QStringLiteral("time_depth"));
    QCOMPARE(rows.at(rTd).outcome, Outcome::Imported);
    QCOMPARE(rows.at(rTd).entityName, QStringLiteral("A1"));

#ifndef Q_OS_WIN
    // Skipped 行缀在最后（按路径排序）：escape.las 先于 pipe.sock。
    expectPath(rEscape, QStringLiteral("escape.las"));
    QCOMPARE(rows.at(rEscape).outcome, Outcome::Skipped);
    QVERIFY(rows.at(rEscape).message.contains(QStringLiteral("符号链接")));
    QVERIFY(rows.at(rEscape).message.contains(QStringLiteral("之外")));
#endif
    if (madeFifo)
    {
      expectPath(idx, QStringLiteral("pipe.sock"));
      QCOMPARE(rows.at(idx).outcome, Outcome::Skipped);
      QVERIFY(rows.at(idx).message.contains(QStringLiteral("普通文件")));
    }

    // 顺序不变式：所有 well_head 行都排在所有非井口行前面。
    int lastHead = -1, firstOther = rows.size();
    for (int i = 0; i < rows.size(); ++i)
    {
      if (rows.at(i).outcome == Outcome::Skipped)
        continue;
      if (rows.at(i).classifiedType == QLatin1String("well_head"))
        lastHead = i;
      else if (firstOther == rows.size())
        firstOther = i;
    }
    QVERIFY(lastHead >= 0 && lastHead < firstOther);

    // catalog：A1+B2 两口井（ghost 不建井）；A1 恰好四条主关联（autoplan
    // 「LAS 排前仍得四条主关联」）；空井口失败行不留链接。路径序第一的
    // 0A1.Las 保持 well_log 主关联（ordinal 0），zzA1b.las 非主（ordinal 1）。
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2);
    QVERIFY(cat->hasEntity(QStringLiteral("well-B2")));
    QStringList roles;
    int logLinks = 0;
    EntityAssetLink firstLas;
    EntityAssetLink secondLas;
    for (const EntityAssetLink &l : cat->linksForEntity(QStringLiteral("well-A1")))
    {
      if (l.isPrimary && !l.unresolved)
        roles.append(l.role);
      if (l.role == QLatin1String("well_log"))
      {
        ++logLinks;
        const QString name = cat->assetById(l.assetId).displayName;
        if (name == QLatin1String("0A1.Las"))
          firstLas = l;
        else if (name == QLatin1String("zzA1b.las"))
          secondLas = l;
      }
    }
    QCOMPARE(logLinks, 2); // 两条 well_log 链接都在
    QVERIFY(!firstLas.assetId.isEmpty());
    QVERIFY(!secondLas.assetId.isEmpty());
    QVERIFY(firstLas.isPrimary);
    QCOMPARE(firstLas.ordinal, 0);
    QVERIFY(!secondLas.isPrimary);
    QCOMPARE(secondLas.ordinal, 1);
    QVERIFY(!cat->assetById(firstLas.assetId).id.isEmpty());
    QVERIFY(!cat->assetById(secondLas.assetId).id.isEmpty());
    std::sort(roles.begin(), roles.end());
    QCOMPARE(roles, QStringList({QStringLiteral("time_depth"), QStringLiteral("tops"),
                                 QStringLiteral("well_head"), QStringLiteral("well_log")}));
    QCOMPARE(cat->assets().size(), 5); // 缺失井名的 ghost 与空井表均不留孤立资产。
    for (const auto &asset : cat->assets())
      for (const auto &version : cat->versionsForAsset(asset.id))
        {
        QVERIFY(!version.sourceUri.endsWith(QStringLiteral("井位/empty.dat")));
        QVERIFY(!version.sourceUri.endsWith(QStringLiteral("ghost.las")));
      }
    QCOMPARE(cat->links().size(), 7);  // 2 井口 + 2 tops + 2 LAS + 1 TD
  }

  // 审计 02 M-8 测试工具：GUI（owner）线程 beginImport → 专用池 worker 上
  // produce → 主线程 **不泵事件** 地 waitForDone（旧 catInvoke/
  // BlockingQueuedConnection 模型在这里必然死锁）→ owner 线程 commit。
  static bool produceOnWorker(const std::shared_ptr<ImportSession> &s,
                              const std::function<void(ImportSession &)> &produce)
  {
    QThreadPool pool;
    pool.setMaxThreadCount(1);
    std::atomic<QThread *> workerThread{nullptr};
    pool.start([&] {
      workerThread = QThread::currentThread();
      produce(*s);
    });
    const bool ok = pool.waitForDone(120000);
    return ok && workerThread.load() != QThread::currentThread();
  }

  static int countFiles(const QString &dir)
  {
    int n = 0;
    QDirIterator it(dir, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
      it.next();
      ++n;
    }
    return n;
  }

  // D1b/D2 + 审计 02 M-8：文件夹导入在 worker 上 produce（只碰 session 的
  // staging 副本），主线程不泵事件也能等到它结束；提交前活 catalog 一字不
  // 动、信号一个不发；owner 线程 commit 后一次入库、按原序发信号。progress
  // 回调按行推进；返回 false 协作取消时整批回滚，不影响此前成功批次。
  void folderImportProducedOnWorkerCommittedOnOwner()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog::resetThreadViolationCount();

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井位"))));
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("0A1.Las")), QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("井位/heads.dat")),
        QByteArrayLiteral("#WellHead File From SMI\n"
                          "#Name      X     Y     KB    TotalDepth\n"
                          "A1           1.0   2.0   0.0   2000.0\n"
                          "B2           3.0   4.0   0.0   2100.0\n")));
    DataCatalog *cat = svc.catalog();
    QSignalSpy importedSpy(&svc, &DataImportService::imported);

    const auto runOnWorker =
        [&](const std::function<bool(int, int, const QString &)> &progress,
            int *assetsBeforeCommit = nullptr,
            DataImportService::CommitStatus expected = DataImportService::CommitStatus::Committed)
        -> std::tuple<QVector<FolderRowResult>, QString, bool> {
      const std::shared_ptr<ImportSession> s = svc.beginImport();
      if (!s)
        return {{}, QStringLiteral("beginImport failed"), false};
      const bool finished = produceOnWorker(s, [&](ImportSession &sess) {
        DataImportService::produceFolder(sess, root, {}, {}, progress);
      });
      if (assetsBeforeCommit)
        *assetsBeforeCommit = cat->assets().size();
      QString cerr;
      const auto st = svc.commitImport(*s, &cerr);
      if (st != expected)
        return {s->rows, cerr, false};
      return {s->rows, cerr.isEmpty() ? s->error : cerr, finished};
    };

    // 全量：两行都入库，实体在 owner 线程提交时建成。progress 分两段（T2
    // 扫描期进度）：plan 哈希段 total==0（两个小文件各一次，done=已见文件
    // 数递增——A-03：旧实现恒发 plan.items.size()，违背 reportScan 的
    // 「已见文件数」契约），执行段 total==2 递增到 2。
    QVector<int> progressSeen;
    QVector<int> totalsSeen;
    int assetsBeforeCommit = -1;
    auto [rows, err, done1] = runOnWorker(
        [&](int done, int total, const QString &) {
          progressSeen.append(done);
          totalsSeen.append(total);
          return true;
        },
        &assetsBeforeCommit);
    QVERIFY2(done1, qPrintable(QStringLiteral("worker produce/commit failed: ") + err));
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(assetsBeforeCommit, 0); // produce 期活 catalog 零写入
    QCOMPARE(totalsSeen, QVector<int>({0, 0, 2, 2}));
    QCOMPARE(rows.size(), 2);
    QCOMPARE(progressSeen, QVector<int>({1, 2, 1, 2}));
    QCOMPARE(importedSpy.count(), 2); // 提交后按原序发射
    using Outcome = FolderRowResult::Outcome;
    for (const auto &r : rows)
      QCOMPARE(r.outcome, Outcome::Imported);
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2); // A1 + B2
    // LAS 行挂到 A1（井口先行建实体的两阶段语义在 worker produce 同样成立）。
    const auto lasRow = std::find_if(rows.begin(), rows.end(), [](const auto &r) {
      return r.path.endsWith(QLatin1String("0A1.Las"));
    });
    QVERIFY(lasRow != rows.end());
    QCOMPARE(lasRow->entityName, QStringLiteral("A1"));
    // 受管字节已从暂存根落位到工程目录，暂存根不留。
    for (const CatalogVersion &v : cat->versionsForAsset(cat->assets().front().id))
      if (v.managed)
        QVERIFY2(QFileInfo::exists(DataCatalog::resolvedVersionPath(projectDir, v)),
                 qPrintable(v.path));
    QVERIFY(!QDir(QDir(projectDir).filePath(QStringLiteral("artifacts/staging"))).exists());

    // 扫描与执行均能取消：整批提交失败，既有库与已发表信号保持原样。
    const auto revisionBeforeCancel = cat->mutationSeq();
    int calls = 0;
    auto [rows2, err2, done2] = runOnWorker(
        [&](int, int total, const QString &) {
          ++calls;
          return total == 0; // 扫描段放行；执行段首行即取消
        }, nullptr, DataImportService::CommitStatus::Failed);
    QVERIFY2(done2, "cancel run did not finish");
    QCOMPARE(calls, 3); // 2 次扫描（哈希段）+ 1 次执行段首行
    QCOMPARE(rows2.size(), 1); // 已处理行保留在报告中，未新增入库。
    for (const auto &row : rows2) {
      QVERIFY(row.outcome != Outcome::Imported);
      QVERIFY(row.outcome != Outcome::Unresolved);
    }
    QVERIFY(err2.contains(QStringLiteral("已取消")));

    // 扫描段取消：哈希回调直接返回 false——plan 停在半途，零行执行、零行
    // 入库（中断续跑语义见 tst_ingestplan）。
    auto [rows3, err3, done3] = runOnWorker(
        [&](int, int, const QString &) { return false; }, nullptr, DataImportService::CommitStatus::Failed);
    QVERIFY2(done3, "scan-cancel run did not finish");
    QVERIFY(rows3.isEmpty());
    QVERIFY(err3.contains(QStringLiteral("已取消")));
    QCOMPARE(cat->mutationSeq(), revisionBeforeCancel);
    QCOMPARE(cat->assets().size(), 2);
    QCOMPARE(importedSpy.count(), 2);
    QVERIFY(!QDir(QDir(projectDir).filePath(QStringLiteral("artifacts/staging"))).exists());
    QCOMPARE(DataCatalog::threadViolationCount(), 0); // worker 从未碰活 catalog
  }

  // 审计 02 M-8：提交失败（catalog 落盘失败）必须如实回到调用方——非空
  // 错误、结果改 Failed、发 importFailed 不发 imported、catalog 内存态与
  // revision 原样、已落位的受管字节撤回、暂存根删除（不留孤儿文件）。
  void failingCommitSurfacesErrorAndRollsBack()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    QVERIFY(cat);
    const QString las = tmp.filePath(QStringLiteral("w1.las"));
    QVERIFY(writeFile(las, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. W1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n101.0 51.0\n")));
    const int assetsBefore = cat->assets().size();
    const int revBefore = cat->catalogRevision();
    const QString rawRoot = QDir(projectDir).filePath(QStringLiteral("artifacts/raw"));
    const int rawFilesBefore = countFiles(rawRoot);

    const std::shared_ptr<ImportSession> s = svc.beginImport();
    QVERIFY(s);
    QVERIFY(produceOnWorker(s, [&](ImportSession &sess) {
      DataImportService::produceFile(sess, las, DataImportService::ImportOptions{});
    }));
    QVERIFY2(s->error.isEmpty(), qPrintable(s->error));
    QCOMPARE(s->fileResult.outcome, DataImportService::ImportOutcome::Imported);
    QVERIFY(QDir(s->stagingRoot).exists()); // 受管字节在暂存根
    QCOMPARE(cat->assets().size(), assetsBefore);

    // 落盘失败注入：SQLite/WAL 只写已持有 fd 的库文件，目录只读挡不住——
    // 用第二连接占写锁（BEGIN IMMEDIATE），catalog 提交撞 SQLITE_BUSY。
    const QString sqlitePath = cat->sqliteCatalogPath();
    QVERIFY(QFile::exists(sqlitePath));
    const QString lockName = QStringLiteral("tst_import_lock_%1")
                                 .arg(reinterpret_cast<quintptr>(this));
    {
      QSqlDatabase lock = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), lockName);
      lock.setDatabaseName(sqlitePath);
      QVERIFY(lock.open());
      QSqlQuery begin(lock);
      QVERIFY(begin.exec(QStringLiteral("BEGIN IMMEDIATE")));
      QSignalSpy importedSpy(&svc, &DataImportService::imported);
      QSignalSpy failedSpy(&svc, &DataImportService::importFailed);
      QString cerr;
      const auto st = svc.commitImport(*s, &cerr);
      QVERIFY(begin.exec(QStringLiteral("ROLLBACK")));
      lock.close();
      lock = QSqlDatabase();
      QSqlDatabase::removeDatabase(lockName);

      QCOMPARE(st, DataImportService::CommitStatus::Failed);
      QVERIFY2(!cerr.isEmpty(), "catalog write failure was swallowed");
      QCOMPARE(s->error, cerr);
      QCOMPARE(s->fileResult.outcome, DataImportService::ImportOutcome::Failed);
      QVERIFY(s->fileResult.assetId.isEmpty());
      QCOMPARE(importedSpy.count(), 0);
      QVERIFY(failedSpy.count() >= 1);
      QCOMPARE(cat->assets().size(), assetsBefore);
      QCOMPARE(cat->catalogRevision(), revBefore);
      QCOMPARE(countFiles(rawRoot), rawFilesBefore); // 落位的字节已撤回
      QVERIFY(!QDir(s->stagingRoot).exists());
    }

    // 同步入口同口径：写锁释放后同一文件照常入库。
    QString err;
    QVERIFY2(!svc.importProjectFile(las, &err).isEmpty(), qPrintable(err));
    QCOMPARE(cat->assets().size(), assetsBefore + 1);
  }

  // 审计 02 M-8：工程切换后旧 session 不得提交进新工程。
  void projectSwitchDiscardsSession()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    const QString las = tmp.filePath(QStringLiteral("w1.las"));
    QVERIFY(writeFile(las, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. W1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    const std::shared_ptr<ImportSession> s = svc.beginImport();
    QVERIFY(s);
    QVERIFY(produceOnWorker(s, [&](ImportSession &sess) {
      DataImportService::produceFile(sess, las, DataImportService::ImportOptions{});
    }));
    svc.setProjectDir(projectDir); // 重开（epoch +1）
    QString cerr;
    QCOMPARE(svc.commitImport(*s, &cerr), DataImportService::CommitStatus::Failed);
    QVERIFY(cerr.contains(QStringLiteral("工程已切换")));
    QVERIFY(svc.catalog()->assets().isEmpty());
    QVERIFY(!QDir(s->stagingRoot).exists());
  }

  // 审计 02 M-8：活 catalog 的写路径带线程亲和断言——非 owner 线程写入被
  // 拒（返回 false + 错误）并计入违规计数；同步导入入口在别的线程如实
  // 拒绝（不再 marshal），beginImport/commitImport 同理。
  void liveCatalogRefusesWritesFromWorker()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    const QString las = tmp.filePath(QStringLiteral("w1.las"));
    QVERIFY(writeFile(las, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. W1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    DataCatalog::resetThreadViolationCount();

    bool addOk = true;
    QString addErr, importErr, assetId = QStringLiteral("x");
    bool sessionNull = false;
    std::thread worker([&] {
      CatalogEntity e;
      e.id = QStringLiteral("well-W9");
      e.entityType = QStringLiteral("well");
      e.name = QStringLiteral("W9");
      addOk = cat->addEntity(e, &addErr);
      assetId = svc.importProjectFile(las, &importErr);
      sessionNull = svc.beginImport() == nullptr;
    });
    worker.join(); // 不泵事件：没有任何东西需要 owner 线程配合
    QVERIFY(!addOk);
    QVERIFY(!addErr.isEmpty());
    QVERIFY(DataCatalog::threadViolationCount() >= 1);
    QVERIFY(!cat->hasEntity(QStringLiteral("well-W9")));
    QVERIFY(assetId.isEmpty());
    QCOMPARE(importErr, DataImportService::offThreadError());
    QVERIFY(sessionNull);
    QVERIFY(cat->assets().isEmpty());
    DataCatalog::resetThreadViolationCount();
  }

  // 执行段中途取消整批不入库；报告、活库、落盘与暂存清理保持一致，重跑完整导入。
  void cancelMidImportLeavesCatalogConsistent()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井位"))));
    QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("井位/heads.dat")),
        QByteArrayLiteral("#WellHead File From SMI\n"
                          "#Name      X     Y     KB    TotalDepth\n"
                          "A1           1.0   2.0   0.0   2000.0\n"
                          "B2           3.0   4.0   0.0   2100.0\n")));
    for (int i = 0; i < 5; ++i)
      QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("L%1.las").arg(i)),
          QByteArrayLiteral("~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A1 : WELL\n"
                            "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n") +
              QByteArray::number(100 + i) + QByteArrayLiteral(".0 50.0\n")));

    const auto checkConsistent = [&] {
      QSet<QString> assetIds;
      for (const CatalogAsset &a : cat->assets())
      {
        assetIds.insert(a.id);
        const QVector<CatalogVersion> vs = cat->versionsForAsset(a.id);
        QVERIFY2(!vs.isEmpty(), qPrintable(a.id));
        for (const CatalogVersion &v : vs)
          QVERIFY2(QFileInfo::exists(v.managed ? DataCatalog::resolvedVersionPath(projectDir, v)
                                               : v.path),
                   qPrintable(v.path));
      }
      for (const EntityAssetLink &l : cat->links())
      {
        QVERIFY2(assetIds.contains(l.assetId), qPrintable(l.assetId));
        if (!l.unresolved)
          QVERIFY2(cat->hasEntity(l.entityId), qPrintable(l.entityId));
      }
      QVERIFY(!QDir(QDir(projectDir).filePath(QStringLiteral("artifacts/staging"))).exists());
      DataCatalog reread;
      QString oerr;
      QVERIFY2(reread.open(projectDir, &oerr), qPrintable(oerr));
      QCOMPARE(reread.assets().size(), cat->assets().size());
      QCOMPARE(reread.links().size(), cat->links().size());
    };

    QSignalSpy importedSpy(&svc, &DataImportService::imported);
    const std::shared_ptr<ImportSession> s = svc.beginImport();
    QVERIFY(s);
    QVERIFY(produceOnWorker(s, [&](ImportSession &sess) {
      DataImportService::produceFolder(sess, root, {}, {},
                                       [](int done, int total, const QString &) {
                                         return total == 0 || done < 3; // 第 3 行后取消
                                       });
    }));
    QCOMPARE(s->rows.size(), 3);
    QVERIFY(s->error.contains(QStringLiteral("已取消")));
    QString cerr;
    QCOMPARE(svc.commitImport(*s, &cerr), DataImportService::CommitStatus::Failed);
    QVERIFY(cerr.contains(QStringLiteral("已取消")));
    int imported = 0;
    for (const FolderRowResult &r : s->rows)
      if (r.outcome == FolderRowResult::Outcome::Imported ||
          r.outcome == FolderRowResult::Outcome::Unresolved)
        ++imported;
    QCOMPARE(imported, 0);
    for (const auto &row : s->rows) {
      QCOMPARE(row.outcome, FolderRowResult::Outcome::Failed);
      QVERIFY(row.message.contains(QStringLiteral("已取消")));
    }
    QCOMPARE(cat->assets().size(), 0);
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 0);
    QVERIFY(cat->links().isEmpty());
    QCOMPARE(importedSpy.count(), 0);
    checkConsistent();

    // 重新导入：前批零提交，所有有效行完整入库。
    QString err;
    const QVector<FolderRowResult> rows2 = svc.importFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows2.size(), 6);
    QCOMPARE(cat->assets().size(), 6);
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2);
    QCOMPARE(importedSpy.count(), 6);
    for (const auto &row : rows2)
      QCOMPARE(row.outcome, FolderRowResult::Outcome::Imported);
    checkConsistent();
  }

  // A1（方向 01）：文件夹导入的行失败可能发生在 addAsset/addVersion 之后
  //（如 SEG-Y 打开失败）——半截 journal 不得随提交入库：不留零版本资产、
  // 不留「行报 Failed 但资产已登记」；同批同内容后续文件也不能 dedup 命中
  // 被撤销的版本（那会挂出指向不存在资产的链接）。
  void failedFolderRowCommitsNothing()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(root));
    // 好行：可解析 LAS。
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("A1.las")),
        QByteArrayLiteral("~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A1 : WELL\n"
                          "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    // 坏行：≥3600 字节但二进制头 ns=0 的 SEG-Y——按扩展名分类成 seismic，
    // addAsset/addVersion 之后族导入里 reader.open 确定性失败。
    const QByteArray badSegy(4000, '\0');
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("bad1.sgy")), badSegy));
    // 同字节第二份：若坏行的半截登记没被撤销，这一行会 dedup 命中并挂出
    // 指向已撤销资产的链接。
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("bad2.sgy")), badSegy));

    const std::shared_ptr<ImportSession> s = svc.beginImport();
    QVERIFY(s);
    QVERIFY(produceOnWorker(s, [&](ImportSession &sess) {
      DataImportService::produceFolder(sess, root, {}, {});
    }));
    int failed = 0, imported = 0;
    for (const FolderRowResult &r : s->rows)
    {
      if (r.outcome == FolderRowResult::Outcome::Failed)
        ++failed;
      else if (r.outcome == FolderRowResult::Outcome::Imported ||
               r.outcome == FolderRowResult::Outcome::Unresolved)
        ++imported;
    }
    QCOMPARE(failed, 2); // 两份坏 SEG-Y 都如实失败，第二份不因半截登记变 dedup
    QCOMPARE(imported, 1);
    QString cerr;
    QCOMPARE(svc.commitImport(*s, &cerr), DataImportService::CommitStatus::Committed);

    // 只剩 LAS 资产：无 seismic 资产、无零版本资产、无悬挂链接、暂存根已清。
    QCOMPARE(cat->assets().size(), 1);
    QSet<QString> assetIds;
    for (const CatalogAsset &a : cat->assets())
    {
      QVERIFY(a.type != QLatin1String("seismic"));
      QVERIFY2(!cat->versionsForAsset(a.id).isEmpty(), qPrintable(a.id));
      assetIds.insert(a.id);
    }
    for (const EntityAssetLink &l : cat->links())
      QVERIFY2(assetIds.contains(l.assetId), qPrintable(l.assetId));
    QVERIFY(!QDir(QDir(projectDir).filePath(QStringLiteral("artifacts/staging"))).exists());
    // 重开工程对拍：盘上同样只有 1 资产。
    DataCatalog reread;
    QString oerr;
    QVERIFY2(reread.open(projectDir, &oerr), qPrintable(oerr));
    QCOMPARE(reread.assets().size(), 1);
  }

  // A1 伴随探针（env 门控）：行级检查点（COW 快照/行）的规模开销——
  // PALEO_IMPORT_SCALE=N（N≥100）时对 N 个小 LAS 跑一次 produceFolder，
  // 只计时打点（qInfo），无绝对断言（口径见 ledger）。
  void folderImportCheckpointOverheadProbe()
  {
    bool okScale = false;
    const int n = qEnvironmentVariableIntValue("PALEO_IMPORT_SCALE", &okScale);
    if (!okScale || n < 100)
      QSKIP("PALEO_IMPORT_SCALE not set — import scale probe skipped");
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    const QString root = tmp.filePath(QStringLiteral("scale"));
    QVERIFY(QDir().mkpath(root));
    const auto lasBytes = [](int i) {
      return QByteArrayLiteral(
                 "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. W") +
             QByteArray::number(i) +
             QByteArrayLiteral(" : WELL\n~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n");
    };
    for (int i = 0; i < n; ++i)
      QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("S%1.las").arg(i)),
                        lasBytes(i)));
    const std::shared_ptr<ImportSession> s = svc.beginImport();
    QVERIFY(s);
    QElapsedTimer clock;
    clock.start();
    QVERIFY(produceOnWorker(s, [&](ImportSession &sess) {
      DataImportService::produceFolder(sess, root, {}, {});
    }));
    qInfo("PERF produceFolder %d rows(ms): %lld", n, clock.elapsed());
    QCOMPARE(int(s->rows.size()), n);
    QString cerr;
    QCOMPARE(svc.commitImport(*s, &cerr), DataImportService::CommitStatus::Committed);
    QCOMPARE(int(svc.catalog()->assets().size()), n);
  }

  // 审计 02 M-8：大量并发导入不死锁。N 个 session 同时在多线程池里 produce，
  // 主线程不泵事件地 waitForDone（旧模型：worker 阻塞等 GUI、GUI 阻塞等
  // worker → 死锁）；随后 owner 线程逐个提交——基线变了的返回 Conflict，
  // 用新 session 重做，直到全部入库。
  void manyConcurrentImportsDoNotDeadlock()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    DataCatalog::resetThreadViolationCount();

    // #503 快速道减样：PALEO_TEST_FAST=1 时并发面 12→4——死锁路径/Conflict
    // 重做/逐项提交语义不变，只减并发规模；默认全量 12（CI 语义不变）。
    const int kN = qEnvironmentVariableIntValue("PALEO_TEST_FAST") > 0 ? 4 : 12;
    QStringList files;
    for (int i = 0; i < kN; ++i)
    {
      const QString f = tmp.filePath(QStringLiteral("c%1.las").arg(i));
      QVERIFY(writeFile(f, QByteArrayLiteral("~Version Information\nVERS. 2.0:\nWRAP. NO:\n"
                                             "~Well\nWELL. C") +
                               QByteArray::number(i) +
                               QByteArrayLiteral(" : WELL\n~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n") +
                               QByteArray::number(100 + i) + QByteArrayLiteral(".0 50.0\n")));
      files.append(f);
    }

    QStringList pending = files;
    int rounds = 0;
    QSignalSpy importedSpy(&svc, &DataImportService::imported);
    while (!pending.isEmpty() && rounds < kN + 1)
    {
      ++rounds;
      QVector<std::shared_ptr<ImportSession>> sessions;
      for (int i = 0; i < pending.size(); ++i)
      {
        sessions.append(svc.beginImport());
        QVERIFY(sessions.back());
      }
      QThreadPool pool;
      pool.setMaxThreadCount(8);
      for (int i = 0; i < pending.size(); ++i)
        pool.start([s = sessions[i], f = pending[i]] {
          DataImportService::produceFile(*s, f, DataImportService::ImportOptions{});
        });
      QVERIFY2(pool.waitForDone(120000), "concurrent produce deadlocked");
      QStringList retry;
      for (int i = 0; i < pending.size(); ++i)
      {
        QString cerr;
        const auto st = svc.commitImport(*sessions[i], &cerr, /*allowConflict=*/true);
        if (st == DataImportService::CommitStatus::Conflict)
          retry.append(pending[i]);
        else
          QVERIFY2(st == DataImportService::CommitStatus::Committed, qPrintable(cerr));
      }
      sessions.clear(); // 冲突 session 析构即删暂存
      pending = retry;
    }
    QVERIFY(pending.isEmpty());
    QCOMPARE(cat->assets().size(), kN);
    QCOMPARE(importedSpy.count(), kN);
    QCOMPARE(DataCatalog::threadViolationCount(), 0);
    QVERIFY(!QDir(QDir(projectDir).filePath(QStringLiteral("artifacts/staging"))).exists());
  }

  // 方向 D1：ImportSession.linkExternal 贯通整批入口（produceFiles →
  // produceItem → importOneFile）——true 时产外链版本（managed=false、path
  // 指回源、sha256 入库冻结），工程 artifacts/raw 下无该资产受管目录；
  // io 层默认 false 保持受管语义（翻转只发生在 workflow/UI 侧）。
  void sessionLinkExternalProducesUnmanagedVersion()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();

    const QString src = tmp.filePath(QStringLiteral("L1.las"));
    QVERIFY(writeFile(src, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. L1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));

    const std::shared_ptr<ImportSession> session = svc.beginImport();
    QVERIFY(session);
    session->linkExternal = true;
    DataImportService::produceFiles(*session, {src}, {}, {});
    QCOMPARE(session->rows.size(), 1);
    QCOMPARE(session->rows.first().outcome, FolderRowResult::Outcome::Imported);
    QString err;
    QCOMPARE(svc.commitImport(*session, &err), DataImportService::CommitStatus::Committed);

    // 按 sourceUri 匹配源路径定位刚导入的资产（比行序/类型过滤更稳）。
    const QString srcAbs = QDir::cleanPath(QFileInfo(src).absoluteFilePath());
    QString assetId;
    for (const CatalogAsset &a : cat->assets())
      if (QDir::cleanPath(QFileInfo(cat->currentVersion(a.id).sourceUri)
                              .absoluteFilePath()) == srcAbs)
        assetId = a.id;
    QVERIFY2(!assetId.isEmpty(), "linkExternal 导入的资产未按 sourceUri 找到");
    const CatalogVersion v = cat->currentVersion(assetId);
    QVERIFY(!v.managed);                                  // 外链，不复制
    QCOMPARE(QDir::cleanPath(v.path), srcAbs);            // 路径指回源
    QVERIFY(!v.sha256.isEmpty());                         // 入库时冻结摘要
    QCOMPARE(v.sha256, sha256OfFile(src));
    // 工程受管区没有该资产的字节目录。
    QVERIFY(!QDir(QDir(projectDir).filePath(
                      QStringLiteral("artifacts/raw/") + assetId))
                 .exists());
    QCOMPARE(cat->assets().size(), 1);
  }

  void folderImportRejectsBadRoots()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    QString err;
    QVERIFY(svc.importFolder(tmp.filePath(QStringLiteral("nonexistent")), &err).isEmpty());
    QVERIFY(!err.isEmpty());

    const QString aFile = tmp.filePath(QStringLiteral("a.las"));
    QVERIFY(writeFile(aFile, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A9 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));
    QVERIFY(svc.importFolder(aFile, &err).isEmpty());
    QVERIFY(!err.isEmpty());

    const QString emptyDir = tmp.filePath(QStringLiteral("empty"));
    QVERIFY(QDir().mkpath(emptyDir));
    QVERIFY(svc.importFolder(emptyDir, &err).isEmpty());
    QVERIFY(!err.isEmpty());

    // 工程之内的子目录（受管区）不能当导入源。
    QVERIFY(QDir().mkpath(QDir(projectDir).filePath(QStringLiteral("artifacts"))));
    QVERIFY(svc.importFolder(QDir(projectDir).filePath(QStringLiteral("artifacts")),
                             &err).isEmpty());
    QVERIFY(!err.isEmpty());

    // PROJECT_FILE_DESIGN 就地工程（源==工程根）：不再硬拒——束成员
    // （proj.qgz/project.paleo）与 artifacts/ 不出行，其余文件照常分类
    // （「从工区文件夹新建」依赖这条路径）。
    const auto inPlaceRows = svc.importFolder(projectDir, &err);
    for (const auto &r : inPlaceRows)
    {
      QVERIFY2(!r.path.endsWith(QStringLiteral("proj.qgz")), qPrintable(r.path));
      QVERIFY2(!r.path.endsWith(QStringLiteral("project.paleo")),
               qPrintable(r.path));
      QVERIFY2(!r.path.contains(QStringLiteral("artifacts/")), qPrintable(r.path));
    }

    // 包住工程目录的上级目录：工程产物子树不出行，只收外面的普通文件。
    const QVector<FolderRowResult> rows =
        svc.importFolder(tmp.path(), &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.front().path, aFile);
  }

  // 确认表后端：previewFolder 与 importFolder 同一枚举/行序（按行索引对齐），
  // typeOverrides 把行重定向到用户改过的类型（forceType 落进资产类型）。
  void folderPreviewMatchesImportOrderAndOverridesApply()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    using Outcome = FolderRowResult::Outcome;

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井位"))));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井分层"))));
    QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("井位/heads.dat")),
        QByteArrayLiteral("#WellHead File From SMI\n#Name X Y KB TD\n"
                          "A1  1.0  2.0  0.0  2000.0\n")));
    const QString topsPath =
        QDir(root).filePath(QString::fromUtf8("井分层/tops.dat"));
    QVERIFY(writeFile(topsPath,
        QByteArrayLiteral("#WellTops File From SMI\n#WellName  Name  MD\n"
                          "A1       D61   2148.0\n")));

    QString err;
    const auto preview = svc.previewFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(preview.size(), 2);
    // 与 importFolder 同一行序：well_head 先行。
    QVERIFY(preview.at(0).path.endsWith(QString::fromUtf8("井位/heads.dat")));
    QCOMPARE(preview.at(0).classifiedType, QStringLiteral("well_head"));
    QCOMPARE(preview.at(1).path, topsPath);
    QCOMPARE(preview.at(1).classifiedType,
             QStringLiteral("well_stratification"));
    QVERIFY(!preview.at(0).skipped && !preview.at(1).skipped);

    // 确认表「改类型」：tops.dat 改成 document → 不再走 tops 解析，
    // 落辅助实体 + reference 链接；行结果 classifiedType 反映新类型。
    QMap<QString, QString> overrides;
    overrides.insert(topsPath, QStringLiteral("document"));
    const auto rows = svc.importFolder(root, &err, overrides);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).path, preview.at(0).path); // 行序对齐预览
    QCOMPARE(rows.at(1).path, preview.at(1).path);
    QCOMPARE(rows.at(0).outcome, Outcome::Imported);
    QCOMPARE(rows.at(1).outcome, Outcome::Imported);
    QCOMPARE(rows.at(1).classifiedType, QStringLiteral("document"));

    bool sawDoc = false;
    for (const CatalogAsset &a : cat->assets())
      if (a.type == QLatin1String("document"))
        sawDoc = true;
    QVERIFY(sawDoc);
  }

  // T22/D5：改成 well_head 的行按生效类型回阶段 1——井建得够早，同一批里
  // 排在它后面的行照常挂到刚建的井。同时验证 importFolder 结果行序 = 生效
  // 类型序（override 后行序不再等于预览序，UI 按路径回行）。
  void folderImportRetypedWellHeadRejoinsPhaseOne()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    using Outcome = FolderRowResult::Outcome;

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(root));
    // 无语义路径 + 无表头 → 分类器落 tabular；固定列记录仍是合法井口表。
    const QString headsPath = QDir(root).filePath(QStringLiteral("zheads.dat"));
    QVERIFY(writeFile(headsPath, QByteArrayLiteral(
        "B2  3.0  4.0  0.0  2100.0\n")));
    // LAS 按文件名排在 zheads.dat 前；不管顺序如何它是阶段 2，能不能挂上
    // 取决于 zheads.dat 是不是阶段 1。
    const QString lasPath = QDir(root).filePath(QStringLiteral("a.las"));
    QVERIFY(writeFile(lasPath, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. B2 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n")));

    QString err;
    const auto preview = svc.previewFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(preview.size(), 2);
    QCOMPARE(preview.at(0).path, lasPath); // 预览按分类器类型：两都行阶段 2，按路径排
    QCOMPARE(preview.at(1).classifiedType, QStringLiteral("tabular"));

    QMap<QString, QString> overrides;
    overrides.insert(headsPath, QStringLiteral("well_head"));
    const auto rows = svc.importFolder(root, &err, overrides);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 2);

    // 生效类型 = well_head → 回阶段 1：井口行在最前，LAS 随后并挂到 B2。
    QCOMPARE(rows.at(0).path, headsPath);
    QCOMPARE(rows.at(0).classifiedType, QStringLiteral("well_head"));
    QCOMPARE(rows.at(0).outcome, Outcome::Imported);
    QVERIFY(rows.at(0).entityName.contains(QStringLiteral("B2")));
    QCOMPARE(rows.at(1).path, lasPath);
    QCOMPARE(rows.at(1).classifiedType, QStringLiteral("well_log"));
    QCOMPARE(rows.at(1).outcome, Outcome::Imported);
    QCOMPARE(rows.at(1).entityName, QStringLiteral("B2"));

    QVERIFY(cat->hasEntity(QStringLiteral("well-B2")));
    int wellLogLinks = 0;
    for (const EntityAssetLink &l : cat->linksForEntity(QStringLiteral("well-B2")))
      if (l.role == QLatin1String("well_log") && l.isPrimary && !l.unresolved)
        ++wellLogLinks;
    QCOMPARE(wellLogLinks, 1);
  }

  // T22：override 只认分类器词表内的类型——非法值忽略，行按分类器原类型走；
  // 词表本身含 tabular/reference，不含 tops/auxiliary（角色名/兜底名不是类型）。
  void folderImportIgnoresInvalidOverrideType()
  {
    QVERIFY(!projectClassifierTypes().contains(QStringLiteral("tops")));
    QVERIFY(!projectClassifierTypes().contains(QStringLiteral("auxiliary")));
    QVERIFY(projectClassifierTypes().contains(QStringLiteral("well_stratification")));
    QVERIFY(projectClassifierTypes().contains(QStringLiteral("tabular")));
    QVERIFY(projectClassifierTypes().contains(QStringLiteral("reference")));
    QVERIFY(isClassifierType(QStringLiteral("well_log")));
    QVERIFY(!isClassifierType(QStringLiteral("not_a_type")));

    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    using Outcome = FolderRowResult::Outcome;

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井分层"))));
    const QString topsPath = QDir(root).filePath(QString::fromUtf8("井分层/tops.dat"));
    QVERIFY(writeFile(topsPath, QByteArrayLiteral(
        "#WellTops File From SMI\n#WellName  Name  MD\n"
        "A1       D61   2148.0\n")));

    QString err;
    QMap<QString, QString> overrides;
    overrides.insert(topsPath, QStringLiteral("not_a_type")); // 非法 → 忽略
    const auto rows = svc.importFolder(root, &err, overrides);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.at(0).classifiedType, QStringLiteral("well_stratification"));
    QCOMPARE(rows.at(0).outcome, Outcome::Unresolved); // 无井 → 未决链接
    for (const CatalogAsset &a : cat->assets())
      QVERIFY(a.type != QLatin1String("not_a_type"));
  }

  // T22：固定辅助收窄到 HZ28-6-1 命名文件——「参考资料」目录内其他 XML 的
  // override 送达后端：改成 well_log → 完整解析曲线后按内容井名归属，
  // 不再被整目录锁成参考；HZ28-6-1 自身给 override 也无效，仍落辅助参考。
  void folderImportReferenceDirXmlOverrideReachesBackend()
  {
    QVERIFY(!isFixedAuxiliaryPath(QString::fromUtf8("/a/参考资料/other.xml")));
    QVERIFY(isFixedAuxiliaryPath(QString::fromUtf8("/a/b/HZ28-6-1测井.xml")));
    QVERIFY(isDefaultReferencePath(QString::fromUtf8("/a/参考资料/doc.pdf")));
    QVERIFY(!isDefaultReferencePath(QString::fromUtf8("/a/普通/doc.pdf")));

    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir)); // 预置 well-A1
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    using Outcome = FolderRowResult::Outcome;

    const QString root = tmp.filePath(QStringLiteral("area"));
    const QString refDir = QDir(root).filePath(QString::fromUtf8("参考资料"));
    QVERIFY(QDir().mkpath(refDir));
    const QByteArray logXml = ImportUxFixtures::xml(QStringLiteral("A1"));
    const QByteArray logXml2(
        "<logs><log><logcurveinfo/><logdata>3 4</logdata></log></logs>");
    const QString otherPath = QDir(refDir).filePath(QStringLiteral("other.xml"));
    const QString hz28Path = QDir(refDir).filePath(QStringLiteral("HZ28-6-1综合.xml"));
    QVERIFY(writeFile(otherPath, logXml));
    QVERIFY(writeFile(hz28Path, logXml2)); // 不同字节——避免 dedup 互相吞掉

    QString err;
    QMap<QString, QString> overrides;
    overrides.insert(otherPath, QStringLiteral("well_log"));   // 参考→测井
    overrides.insert(hz28Path, QStringLiteral("well_head"));   // 锁住行：覆盖无效
    const auto rows = svc.importFolder(root, &err, overrides);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 2);

    // other.xml：override 生效，完整测井内容按 A1 绑定，不创建辅助实体。
    QString otherAsset, hz28Asset;
    for (const CatalogAsset &a : cat->assets())
    {
      if (a.displayName == QStringLiteral("other.xml"))
        otherAsset = a.id;
      if (a.displayName == QStringLiteral("HZ28-6-1综合.xml"))
        hz28Asset = a.id;
    }
    QVERIFY(!otherAsset.isEmpty() && !hz28Asset.isEmpty());
    const auto otherLinks = cat->linksForAsset(otherAsset);
    QCOMPARE(otherLinks.size(), 1);
    QCOMPARE(otherLinks.front().entityType, QStringLiteral("well"));
    QVERIFY(!otherLinks.front().unresolved);
    QCOMPARE(otherLinks.front().entityId, QStringLiteral("well-A1"));
    QVERIFY(otherLinks.front().isPrimary);
    const auto hz28Links = cat->linksForAsset(hz28Asset);
    QCOMPARE(hz28Links.size(), 1);
    QCOMPARE(hz28Links.front().entityType, QStringLiteral("auxiliary"));
    QCOMPARE(hz28Links.front().role, QStringLiteral("reference"));
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 1); // 只有预置 A1
    QCOMPARE(cat->entities(QStringLiteral("auxiliary")).size(), 1); // 仅 HZ28

    for (const auto &r : rows)
      QCOMPARE(r.outcome, Outcome::Imported);
  }

  // T22：确认表「重试」的后端入口——单行按当前（覆盖）类型重导，行结果口径
  // 与 importFolder 相同；修文件后再导成功；非法 forceType 忽略。
  void importFolderRowReimportsSingleFile()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    using Outcome = FolderRowResult::Outcome;

    const QString dir = tmp.filePath(QString::fromUtf8("井位"));
    QVERIFY(QDir().mkpath(dir));
    const QString headsPath = QDir(dir).filePath(QStringLiteral("heads.dat"));
    QVERIFY(writeFile(headsPath,
                      QByteArrayLiteral("#WellHead File From SMI\n# no rows\n")));

    QString err;
    FolderRowResult row =
        svc.importFolderRow(headsPath, QString(), &err);
    QCOMPARE(row.outcome, Outcome::Failed);
    QVERIFY(!err.isEmpty());
    QVERIFY(row.message.contains(QStringLiteral("井表没有可用数据行")));
    QCOMPARE(row.classifiedType, QStringLiteral("well_head"));

    // 修好文件再重导：入库 + 实体名，error 清空。
    QVERIFY(writeFile(headsPath, QByteArrayLiteral(
        "#WellHead File From SMI\n#Name X Y KB TD\n"
        "A1  1.0  2.0  0.0  2000.0\n")));
    row = svc.importFolderRow(headsPath, QString(), &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(row.outcome, Outcome::Imported);
    QVERIFY(row.entityName.contains(QStringLiteral("A1")));

    // forceType 口径同 importFolder 覆盖：合法值改类型，非法值忽略。
    const QString miscPath = tmp.filePath(QStringLiteral("misc.dat"));
    QVERIFY(writeFile(miscPath, QByteArrayLiteral("a,b\n1,2\n")));
    row = svc.importFolderRow(miscPath, QStringLiteral("document"), &err);
    QCOMPARE(row.classifiedType, QStringLiteral("document"));
    QCOMPARE(row.outcome, Outcome::Imported); // document → 辅助实体参考关联
    row = svc.importFolderRow(miscPath, QStringLiteral("bogus_type"), &err);
    QCOMPARE(row.classifiedType, QStringLiteral("tabular")); // 非法 → 分类器原类型
  }

  // 井附件挂井（竞赛工区数据形态，importAuxReferenceFamily 的前置通道）：
  // 岩心照片「<井名>,<深度>m.JPG」→ core + version extra depthMd（图片井道
  // 深度锚）；岩屑目录 xlsx → cuttings（岩性道）；薄片子目录父名识井
  //（A3薄片鉴定/A31868.62粒间孔.JPG）→ lab_analysis；无关键词井附件 →
  // 井域 reference；零匹配 → 原辅助实体路径。
  void wellAttachmentLinksCoreCuttingsLabAndReference()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    QVERIFY(cat != nullptr && cat->isOpen());

    QString err;
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")),
                                   &err)
                 .isEmpty()); // 建井 A1..A20

    const QString corePhoto = QDir(tmp.path()).filePath(
        QString::fromUtf8("2.3岩心资料/A1岩心照片/A1,1849.35m.JPG"));
    const QString cuttings = QDir(tmp.path()).filePath(
        QString::fromUtf8("2.4岩屑录井数据/A1岩屑录井数据.xlsx"));
    const QString thin = QDir(tmp.path()).filePath(
        QString::fromUtf8("2.6薄片岩矿鉴定/A3薄片鉴定/A31868.62粒间孔.JPG"));
    const QString seismo = QDir(tmp.path()).filePath(
        QString::fromUtf8("05.参考井合成地震记录/A12井合成地震记录.jpg"));
    // 回归陷阱：薄片目录下文件名含「岩屑」字样（白云岩屑——岩性描述词），
    // 角色关键词只看目录段，不得误判成 cuttings。
    const QString thinTextTrap = QDir(tmp.path()).filePath(
        QString::fromUtf8("2.6薄片岩矿鉴定/A1薄片鉴定/A11905.95致密富含白云岩屑.JPG"));
    const QString unrelated = QDir(tmp.path()).filePath(
        QStringLiteral("参考资料/说明图.jpg"));
    int seq = 0;
    for (const QString &p : {corePhoto, cuttings, thin, thinTextTrap, seismo, unrelated})
    {
      QVERIFY(QDir().mkpath(QFileInfo(p).absolutePath()));
      // 内容逐文件不同——同字节会被 SHA 去重成同一资产（§3 dedup），测不到
      // 每文件的挂井链接。
      QVERIFY(writeFile(p, QByteArrayLiteral("\xff\xd8\xff\xe0" "fake") +
                                 QByteArray::number(seq++)));
    }

    for (const QString &p : {corePhoto, cuttings, thin, thinTextTrap, seismo, unrelated})
    {
      const QString id = svc.importProjectFile(p, &err);
      QVERIFY2(!id.isEmpty(), qPrintable(p + ": " + err));
    }

    auto linksForRole = [cat](const QString &wellId, const QString &role) {
      QVector<EntityAssetLink> out;
      for (const EntityAssetLink &l : cat->linksForEntity(wellId))
        if (l.role == role && !l.unresolved)
          out.append(l);
      return out;
    };
    const QStringList a1 = cat->wellsMatchingName(QStringLiteral("A1"));
    const QStringList a3 = cat->wellsMatchingName(QStringLiteral("A3"));
    const QStringList a12 = cat->wellsMatchingName(QStringLiteral("A12"));
    QCOMPARE(a1.size(), 1);
    QCOMPARE(a3.size(), 1);
    QCOMPARE(a12.size(), 1);

    // 岩心照片 → core；深度锚进 version extra。
    const auto coreLinks = linksForRole(a1.front(), QStringLiteral("core"));
    QCOMPARE(coreLinks.size(), 1);
    const QVector<CatalogVersion> coreVersions =
        cat->versionsForAsset(coreLinks.front().assetId);
    QCOMPARE(coreVersions.size(), 1);
    QCOMPARE(coreVersions.front().extra.value(QStringLiteral("depthMd")).toDouble(),
             1849.35);
    // 岩屑 xlsx → cuttings。
    QCOMPARE(linksForRole(a1.front(), QStringLiteral("cuttings")).size(), 1);
    // 薄片（父目录识井）→ lab_analysis；无「数字m」深度模式 → 无 depthMd。
    const auto labLinks = linksForRole(a3.front(), QStringLiteral("lab_analysis"));
    QCOMPARE(labLinks.size(), 1);
    QVERIFY(!cat->versionsForAsset(labLinks.front().assetId)
                 .front()
                 .extra.contains(QStringLiteral("depthMd")));
    // 陷阱件挂 A1 的 lab_analysis（不是 cuttings——文件名描述词不参与角色）。
    QCOMPARE(linksForRole(a1.front(), QStringLiteral("lab_analysis")).size(), 1);
    // 合成地震记录（无关键词井附件）→ 井域 reference。
    QCOMPARE(linksForRole(a12.front(), QStringLiteral("reference")).size(), 1);

    // 挂井的附件不再落辅助实体：辅助实体只剩零匹配的「说明图」。
    int auxCount = 0;
    for (const CatalogEntity &e : cat->entities(QStringLiteral("auxiliary")))
    {
      ++auxCount;
      QCOMPARE(e.name, QStringLiteral("说明图"));
    }
    QCOMPARE(auxCount, 1);
  }

  // 可选真数据：PALEO_REAL_PROJECT_AREA 跑一次 importFolder——井口先行后
  // A1 仍得四条主关联；无 Failed 行（与 tst_smoke_realdata 同一门禁变量）。
  void folderImportRealAreaSmoke()
  {
    const QString src = qEnvironmentVariable("PALEO_REAL_PROJECT_AREA");
    if (src.isEmpty() || !QDir(src).exists())
      QSKIP("PALEO_REAL_PROJECT_AREA not set — folder real-data smoke skipped");

    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    QString err;
    QElapsedTimer timer;
    timer.start();
    // The live area also contains its own managed RAW/DERIVED tree. Import the
    // original delivery directories so copies with hashed filenames cannot
    // precede and de-duplicate the named horizon sources in this fixture.
    QVector<FolderRowResult> rows;
    for (const QString &sub : {QString::fromUtf8("井位"), QString::fromUtf8("井曲线"),
                              QString::fromUtf8("井分层"), QString::fromUtf8("时深"),
                              QString::fromUtf8("层位"), QString::fromUtf8("地震体"),
                              QString::fromUtf8("参考相图"), QString::fromUtf8("参考资料")})
    {
      rows += svc.importFolder(QDir(src).filePath(sub), &err);
      QVERIFY2(err.isEmpty(), qPrintable(err));
    }
    QVERIFY(rows.size() >= 60); // 原始交付数据；参考附件可随工区增长

    int nImported = 0, nUnresolved = 0, nFailed = 0, nSkipped = 0;
    bool sawWellHead = false, sawSeismic = false;
    for (const FolderRowResult &r : rows)
    {
      switch (r.outcome)
      {
        case FolderRowResult::Outcome::Imported: ++nImported; break;
        case FolderRowResult::Outcome::Unresolved: ++nUnresolved; break;
        case FolderRowResult::Outcome::Failed:
          ++nFailed;
          qWarning("FOLDER FAIL %s: %s", qPrintable(r.path), qPrintable(r.message));
          break;
        case FolderRowResult::Outcome::Skipped: ++nSkipped; break;
      }
      sawWellHead = sawWellHead || r.classifiedType == QLatin1String("well_head");
      sawSeismic = sawSeismic || r.classifiedType == QLatin1String("seismic");
    }
    qWarning("FOLDER SMOKE %lld ms: %d rows — 入库 %d, 未决 %d, 失败 %d, 跳过 %d",
             timer.elapsed(), int(rows.size()), nImported, nUnresolved, nFailed, nSkipped);
    QCOMPARE(nFailed, 0);
    QVERIFY(sawWellHead && sawSeismic);

    DataCatalog *cat = svc.catalog();
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 20);
    QCOMPARE(cat->entities(QStringLiteral("sequence_boundary")).size(), 8);
    QCOMPARE(cat->entities(QStringLiteral("seismic_survey")).size(), 1);
    QStringList roles;
    for (const EntityAssetLink &l : cat->linksForEntity(QStringLiteral("well-A1")))
      if (l.isPrimary && !l.unresolved)
        roles.append(l.role);
    std::sort(roles.begin(), roles.end());
    QCOMPARE(roles, QStringList({QStringLiteral("time_depth"), QStringLiteral("tops"),
                                 QStringLiteral("well_head"), QStringLiteral("well_log")}));
  }

  // 方向 95：mkproject mini 夹具扩散——导入域不变量在整工程生产产物上断言
  //（QProcess 驱动 paleo_mkproject --manifest mini 全链）。tst_mkprojectfixture
  // 盖 catalog 计数/round-trip/配准残差；本槽补 tst_import 独有面：
  //   · 受管副本 SHA-256 与 mini 源件逐一对拍（14 件 verbatim 导入）；
  //   · 只读锁（写权限位为零）在整工程 15 件受管副本上成立；
  //   · 外链件 sha 冻结 = 入库时源件（mini.sgy + wg1_well.xml——seismic
  //     类型一律外链 + manifest linkExternal，见 dataimport_file.cpp 口径）；
  //   · A3 双测井文件曲线并集（方向44 并集可达 + 跨文件别名协议）。
  // 断言差异如实：井口件是 convert 产物（conv0.dat——staging 转换件已随进程
  // 退出销毁，SHA 无源可拍，只断受管在盘 + RAW 阶段）；既有 synthetic 断言
  // 全保留（双轨：本槽是增广不是替换）。
  void miniFixtureProjectMeetsImportDomainInvariants()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const MkProjectFixture::RunResult run =
        MkProjectFixture::buildMiniProject(tmp.path());
    QVERIFY2(run.ran && run.exitCode == 0,
             qPrintable(run.errorText.isEmpty() ? run.standardOutput
                                                : run.errorText));

    DataCatalog cat;
    QString err;
    QVERIFY2(cat.open(run.projectDir, &err), qPrintable(err));
    QCOMPARE(int(cat.assets().size()), 17);
    QCOMPARE(int(cat.versions().size()), 17); // 每资产恰一个 RAW 版本

    // 按文件名找版本（mini 件名在 manifest 内唯一）。
    const auto versionFor = [&cat](const QString &fileName) {
      for (const CatalogVersion &v : cat.versions())
        if (v.fileName == fileName)
          return v;
      return CatalogVersion();
    };
    // QVERIFY2 宏内含 void return——不能带返回值，路径存在性由调用方断言。
    const auto assertManagedReadOnly = [&](const CatalogVersion &v,
                                           const QString &what) {
      QVERIFY2(v.managed, qPrintable(what));
      QCOMPARE(v.stage, QStringLiteral("RAW"));
      const QString abs = DataCatalog::resolvedVersionPath(run.projectDir, v);
      QVERIFY2(QFile::exists(abs), qPrintable(abs));
      const QFile::Permissions perm = QFileInfo(abs).permissions();
      QVERIFY2(int(perm & (QFile::WriteOwner | QFile::WriteUser |
                           QFile::WriteGroup | QFile::WriteOther)) == 0,
               qPrintable(what));
    };

    // ---- 14 件 verbatim 受管导入：SHA 对拍源件 + 只读锁 ----
    const QStringList verbatimRel = {
        QStringLiteral("strat/tops.dat"),
        QStringLiteral("strat/tops_header.dat"),
        QStringLiteral("las/A1.las"),
        QStringLiteral("las/A2.las"),
        QStringLiteral("las/A3.las"),
        QStringLiteral("las/A3_rerun.las"),
        QString::fromUtf8("2.3岩心资料/A1_1610.5_岩心.jpg"),
        QString::fromUtf8("2.3岩心资料/A2_1625.0_岩心.jpg"),
        QString::fromUtf8("2.3岩心资料/A3_1640.5_岩心.jpg"),
        QString::fromUtf8("2.6薄片岩矿鉴定/A3_1701.0_薄片.jpg"),
        QString::fromUtf8("2.4岩屑录井数据/A1_岩屑录井.csv"),
        QString::fromUtf8("2.4岩屑录井数据/A2_岩屑录井.csv"),
        QString::fromUtf8("2.4岩屑录井数据/A3_岩屑录井.csv"),
        QString::fromUtf8("ref/工区说明.docx"),
    };
    for (const QString &rel : verbatimRel)
    {
      const QString fileName = QFileInfo(rel).fileName();
      const CatalogVersion v = versionFor(fileName);
      QVERIFY2(!v.id.isEmpty(), qPrintable(fileName));
      assertManagedReadOnly(v, fileName);
      const QString src = QDir(MkProjectFixture::miniDir()).filePath(rel);
      QVERIFY2(v.sha256 == sha256OfFile(src),
               qPrintable(QStringLiteral("%1 SHA 与 mini 源件不一致").arg(fileName)));
    }

    // ---- 井口 convert 产物（conv0.dat）：受管在盘（SHA 无源可拍，如实） ----
    const CatalogVersion head = versionFor(QStringLiteral("conv0.dat"));
    QVERIFY2(!head.id.isEmpty(), "manifest 首项井口 convert 产物 conv0.dat 缺失");
    assertManagedReadOnly(head, QStringLiteral("conv0.dat"));

    // ---- 外链件 ×2（mini.sgy：seismic 一律外链；wg1_well.xml：manifest
    // linkExternal）——不入库复制，sha = 入库时源件冻结，路径指回源。 ----
    for (const QString &rel : {QStringLiteral("seis/mini.sgy"),
                               QStringLiteral("outsource/wg1_well.xml")})
    {
      const QString fileName = QFileInfo(rel).fileName();
      const CatalogVersion v = versionFor(fileName);
      QVERIFY2(!v.id.isEmpty(), qPrintable(fileName));
      QVERIFY2(!v.managed, qPrintable(fileName));
      const QString src = QDir(MkProjectFixture::miniDir()).filePath(rel);
      QVERIFY2(v.sha256 == sha256OfFile(src),
               qPrintable(QStringLiteral("%1 外链 sha 与源件不一致").arg(fileName)));
      QCOMPARE(v.path, src);
    }

    // ---- A3 双测井并集（方向44：well_log 链接文件集 + 曲线并集） ----
    QString a3Id;
    for (const CatalogEntity &w : cat.entities(QStringLiteral("well")))
      if (w.name == QStringLiteral("A3"))
        a3Id = w.id;
    QVERIFY2(!a3Id.isEmpty(), "A3 井实体缺失");
    WellLogWarnings logWarnings;
    const QVector<WellLogFile> files =
        WellLogSet::wellLogFiles(&cat, run.projectDir, a3Id, &logWarnings);
    QVERIFY2(files.size() == 2, "A3 双测井（A3.las + A3_rerun.las）");
    QCOMPARE(logWarnings.count, 0);
    const QVector<WellCurveRef> curves =
        WellLogSet::wellCurveIndex(&cat, run.projectDir, a3Id);
    QStringList mnemonicNames;
    int canonicalCount = 0;
    for (const WellCurveRef &c : curves)
    {
      mnemonicNames.append(c.mnemonic);
      if (c.canonical)
        ++canonicalCount;
    }
    // 四曲线两文件 = 并集 8 列：主文件保留原名（canonical ×4），副文件
    // 同名列加 @basename 别名（GR@A3_rerun 等）；深度索引列不进并集。
    QCOMPARE(int(curves.size()), 8);
    QCOMPARE(canonicalCount, 4);
    QVERIFY(mnemonicNames.contains(QStringLiteral("GR")));
    QVERIFY(mnemonicNames.contains(QStringLiteral("GR@A3_rerun")));
    QVERIFY(!mnemonicNames.contains(QStringLiteral("DEPT")));
  }

  // LAS: ~V/~W/~C/~A parsed; 3 curves × 4 rows; NULL token → NaN.
  void lasParsesCurvesAndNulls()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString lasPath = tmp.filePath(QStringLiteral("w01.las"));
    QVERIFY(writeFile(lasPath, QByteArrayLiteral(
        "~Version Information\n"
        " VERS.                  2.0 :   CWLS LOG ASCII STANDARD -VERSION 2.0\n"
        " WRAP.                  NO  :   ONE LINE PER DEPTH STEP\n"
        "~Well Information Block\n"
        "#MNEM.UNIT       DATA            INFORMATION\n"
        " STRT.M              1670.0000 :   START DEPTH\n"
        " STOP.M              1669.5000 :   STOP DEPTH\n"
        " STEP.M               -0.1250 :   STEP\n"
        " NULL.               -999.2500 :   NULL VALUE\n"
        " WELL.                  W-01   :   WELL NAME\n"
        "~Curve Information Block\n"
        " DEPT.M                    :   1  DEPTH\n"
        " GR  .API                  :   2  GAMMA RAY\n"
        " DT  .US/F                 :   3  SONIC DELTA-T\n"
        "~A  DEPT       GR       DT\n"
        "1670.000  82.10  460.00\n"
        "1669.875  81.50  455.20\n"
        "1669.750  -999.25 450.90\n"
        "1669.625  79.80  446.10\n")));

    QStringList names;
    QList<LasCurve> curves;
    QString err;
    QVERIFY2(LasParser::parse(lasPath, names, curves, &err), qPrintable(err));

    QCOMPARE(names, QStringList({QStringLiteral("DEPT"), QStringLiteral("GR"), QStringLiteral("DT")}));
    QCOMPARE(curves.size(), 3);
    QCOMPARE(curves.at(0).name, QStringLiteral("DEPT"));
    QCOMPARE(curves.at(0).unit, QStringLiteral("M"));
    QCOMPARE(curves.at(1).name, QStringLiteral("GR"));
    QCOMPARE(curves.at(1).unit, QStringLiteral("API"));
    QCOMPARE(curves.at(2).descr, QStringLiteral("3  SONIC DELTA-T"));

    for (const LasCurve &c : curves)
      QCOMPARE(c.values.size(), 4);

    QCOMPARE(curves.at(0).values.at(0), 1670.0);
    QCOMPARE(curves.at(0).values.at(3), 1669.625);
    QCOMPARE(curves.at(1).values.at(0), 82.10);
    QVERIFY(qIsNaN(curves.at(1).values.at(2)));  // -999.25 NULL → NaN
    QCOMPARE(curves.at(1).values.at(3), 79.80);
    QCOMPARE(curves.at(2).values.at(2), 450.90);
    QVERIFY(!qIsNaN(curves.at(2).values.at(2)));
  }

  // WRAP YES → hard error; missing file → error.
  void lasFailureModes()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString wrapPath = tmp.filePath(QStringLiteral("wrap.las"));
    QVERIFY(writeFile(wrapPath, QByteArrayLiteral(
        "~Version Information\n"
        " VERS.                  2.0 :   CWLS LOG ASCII STANDARD -VERSION 2.0\n"
        " WRAP.                  YES :   MULTIPLE LINES PER DEPTH STEP\n"
        "~Curve Information\n"
        " DEPT.M : DEPTH\n"
        "~A DEPT\n"
        "1670.0\n")));

    QStringList names;
    QList<LasCurve> curves;
    QString err;
    QVERIFY(!LasParser::parse(wrapPath, names, curves, &err));
    QVERIFY(!err.isEmpty());

    QVERIFY(!LasParser::parse(tmp.filePath(QStringLiteral("ghost.las")), names, curves, &err));
    QVERIFY(!err.isEmpty());
  }

  // ======================= C 包：IngestPlan 三段式 =======================

  // plan 期 sha 去重：已注册字节的项标 duplicateOfVersionId + decision=skip；
  // preview 行带出同一决策（确认表「重复→跳过」的数据源）；执行端跳过、
  // catalog 零增量。
  void planDedupMarksDuplicateAndDecidesSkip()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    using Outcome = FolderRowResult::Outcome;

    QString err;
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err).isEmpty());
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("A1.Las")), &err).isEmpty());

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(root));
    const QString copyPath = QDir(root).filePath(QStringLiteral("A1_copy.las"));
    QVERIFY(QFile::copy(fixture(QStringLiteral("A1.Las")), copyPath));

    DataCatalog *cat = svc.catalog();
    const IngestPlan plan = buildIngestPlan(root, *cat);
    QVERIFY(plan.issues.isEmpty());
    QCOMPARE(plan.items.size(), 1);
    const PlannedItem &item = plan.items.constFirst();
    QCOMPARE(item.entityId, QStringLiteral("well-A1")); // 身份已决 + 重复标记并存
    QVERIFY(!item.duplicateOfVersionId.isEmpty());
    QCOMPARE(item.decision, QStringLiteral("skip"));
    QCOMPARE(plan.duplicateCount(), 1);
    QCOMPARE(plan.unresolvedCount(), 0);

    // 预览行把 plan 期决策带给确认表（UI 显示「重复→跳过」）。
    const auto preview = svc.previewFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(preview.size(), 1);
    QCOMPARE(preview.at(0).decision, QStringLiteral("skip"));

    const int assetCount = cat->assets().size();
    const int linkCount = cat->links().size();
    const auto rows = executeIngestPlan(plan, svc, {}, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.at(0).outcome, Outcome::Skipped);
    QVERIFY(rows.at(0).message.contains(QString::fromUtf8("重复")));
    QCOMPARE(cat->assets().size(), assetCount); // 零增量
    QCOMPARE(cat->links().size(), linkCount);
  }

  // 幂等重跑：同一目录二次 importFolder → plan 全标 skip，行全 Skipped，
  // 资产/版本/链接/实体计数一条不增（不重复登记）。
  void folderPlanRerunRegistersNothingTwice()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    using Outcome = FolderRowResult::Outcome;

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井位"))));
    QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("井位/heads.dat")),
        QByteArrayLiteral("#WellHead File From SMI\n#Name X Y KB TD\n"
                          "A1  1.0  2.0  0.0  2000.0\n")));
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("a1.las")),
        QByteArrayLiteral("~Version Information\nVERS. 2.0:\nWRAP. NO:\n"
                          "~Well\nWELL. A1 : WELL\n~Curve\nDEPT.M :\nGR.API :\n"
                          "~A DEPT GR\n100.0 50.0\n")));

    QString err;
    const auto rows1 = svc.importFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows1.size(), 2);
    for (const auto &r : rows1)
      QCOMPARE(r.outcome, Outcome::Imported);

    int versionCount = 0;
    for (const CatalogAsset &a : cat->assets())
      versionCount += cat->versionsForAsset(a.id).size();
    const int assetCount = cat->assets().size();
    const int linkCount = cat->links().size();
    const int wellCount = cat->entities(QStringLiteral("well")).size();

    const auto rows2 = svc.importFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows2.size(), 2);
    for (const auto &r : rows2)
    {
      QCOMPARE(r.outcome, Outcome::Skipped); // plan 标重复 → 决策 skip → 跳过
      QVERIFY(r.message.contains(QString::fromUtf8("重复")));
    }
    QCOMPARE(cat->assets().size(), assetCount);
    QCOMPARE(cat->links().size(), linkCount);
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), wellCount);
    int versionCount2 = 0;
    for (const CatalogAsset &a : cat->assets())
      versionCount2 += cat->versionsForAsset(a.id).size();
    QCOMPARE(versionCount2, versionCount);
  }

  // shp 族归组：同主名 .shp/.shx/.dbf/.prj → 单个 PlannedItem（members 收
  // 全组）；散件不归组。执行把整族拷进同一受管 RAW 版本目录。
  void shapefileFamilyGroupsIntoOneItem()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    using Outcome = FolderRowResult::Outcome;

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(root));
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("f.shp")), QByteArrayLiteral("shp-bytes")));
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("f.shx")), QByteArrayLiteral("shx-bytes")));
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("f.dbf")), QByteArrayLiteral("dbf-bytes")));
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("f.prj")), QByteArrayLiteral("prj-bytes")));
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("loose.dbf")), QByteArrayLiteral("other"))); // 无族散件

    const IngestPlan plan = buildIngestPlan(root, *cat);
    QCOMPARE(plan.items.size(), 2); // f 族一项 + loose.dbf 一项
    const PlannedItem *fam = nullptr;
    for (const PlannedItem &it : plan.items)
      if (it.path.endsWith(QLatin1String("f.shp")))
        fam = &it;
    QVERIFY(fam != nullptr);
    QCOMPARE(fam->members.size(), 4);
    QVERIFY(fam->members.contains(QDir(root).filePath(QStringLiteral("f.shp"))));
    QVERIFY(fam->members.contains(QDir(root).filePath(QStringLiteral("f.shx"))));
    QVERIFY(fam->members.contains(QDir(root).filePath(QStringLiteral("f.dbf"))));
    QVERIFY(fam->members.contains(QDir(root).filePath(QStringLiteral("f.prj"))));

    QString err;
    const auto rows = executeIngestPlan(plan, svc, {}, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 2);
    const auto famRow = std::find_if(rows.begin(), rows.end(), [](const auto &r) {
      return r.path.endsWith(QLatin1String("f.shp"));
    });
    QVERIFY(famRow != rows.end());
    QCOMPARE(famRow->outcome, Outcome::Imported);

    // 整族进同一受管版本目录。
    QString famAsset;
    for (const CatalogAsset &a : cat->assets())
      if (a.displayName == QLatin1String("f.shp"))
        famAsset = a.id;
    QVERIFY(!famAsset.isEmpty());
    const CatalogVersion v = cat->currentVersion(famAsset);
    QVERIFY(v.managed);
    const QString dir = QFileInfo(svc.absolutePathForVersion(v)).absolutePath();
    QVERIFY(QFileInfo::exists(QDir(dir).filePath(QStringLiteral("f.shp"))));
    QVERIFY(QFileInfo::exists(QDir(dir).filePath(QStringLiteral("f.shx"))));
    QVERIFY(QFileInfo::exists(QDir(dir).filePath(QStringLiteral("f.dbf"))));
    QVERIFY(QFileInfo::exists(QDir(dir).filePath(QStringLiteral("f.prj"))));
  }

  // 单文件入口走同一 planner：已注册文件标 duplicateOfVersionId +
  // decision=skip；importProjectFileEx 的「仍导入」语义不变——dedup 结局
  // AlreadyStored + 指回已存在资产。
  void singleFileImportBuildsOneItemPlan()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();

    QString err;
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err).isEmpty());
    const QString assetId = svc.importProjectFile(fixture(QStringLiteral("A1.Las")), &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    const IngestPlan plan = buildIngestPlan(fixture(QStringLiteral("A1.Las")), *cat);
    QCOMPARE(plan.items.size(), 1);
    const PlannedItem &item = plan.items.constFirst();
    QCOMPARE(item.type, QStringLiteral("well_log"));
    QCOMPARE(item.entityId, QStringLiteral("well-A1")); // plan 期身份匹配
    QVERIFY(!item.duplicateOfVersionId.isEmpty());
    QCOMPARE(item.decision, QStringLiteral("skip"));

    // 显式单文件导入的语义是「仍导入」——内部 dedup 消化同字节结局。
    const DataImportService::ImportResult res =
        svc.importProjectFileEx(fixture(QStringLiteral("A1.Las")), &err);
    QCOMPARE(res.outcome, DataImportService::ImportOutcome::AlreadyStored);
    QCOMPARE(res.assetId, assetId);
  }

  // 歧义井名：plan 期如实标记，执行无显式归属时拒绝入库；用户选择后成功。
  void planAmbiguityRequiresExplicitTarget()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithAliasWells(projectDir)); // 两口同名 "dup" 井
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    using Outcome = FolderRowResult::Outcome;

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(root));
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("dup.Las")),
        QByteArrayLiteral("~Version Information\nVERS. 2.0:\nWRAP. NO:\n"
                          "~Well\nWELL. dup : WELL\n~Curve\nDEPT.M :\nGR.API :\n"
                          "~A DEPT GR\n100.0 50.0\n")));

    const IngestPlan plan = buildIngestPlan(root, *cat);
    QCOMPARE(plan.items.size(), 1);
    const PlannedItem &item = plan.items.constFirst();
    QVERIFY(item.entityAmbiguous);
    QVERIFY(item.entityId.isEmpty()); // 不猜
    QCOMPARE(plan.unresolvedCount(), 1);

    QString err;
    const auto rows = executeIngestPlan(plan, svc, {}, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.at(0).outcome, Outcome::Failed);
    QVERIFY(!rows.at(0).message.isEmpty());
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2);
    QVERIFY(cat->assets().isEmpty());
    QVERIFY(cat->links().isEmpty());
    DataImportService::ImportOptions options;
    options.targetWellId = QStringLiteral("well-X2");
    const auto explicitResult = svc.importProjectFileEx(plan.items.first().path, options, &err);
    QVERIFY2(!explicitResult.assetId.isEmpty(), qPrintable(err));
    const auto links = cat->linksForAsset(explicitResult.assetId);
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().entityId, options.targetWellId);
    QVERIFY(!links.first().unresolved);
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2);
  }

  // buildIngestPlan 纯函数：catalog 不动——revision/实体/资产/链接/版本计数
  // 与 catalog.sqlite（及 catalog.sqlite-wal）落盘字节在构建前后完全一致。
  void buildIngestPlanLeavesCatalogUntouched()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();

    QString err;
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err).isEmpty());
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("A1.Las")), &err).isEmpty());

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(root));
    QVERIFY(QFile::copy(fixture(QStringLiteral("A1.Las")),
                        QDir(root).filePath(QStringLiteral("dup.las"))));
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("new.las")),
        QByteArrayLiteral("~Version Information\nVERS. 2.0:\nWRAP. NO:\n"
                          "~Well\nWELL. A1 : WELL\n~Curve\nDEPT.M :\nGR.API :\n"
                          "~A DEPT GR\n101.0 50.0\n")));

    const int rev = cat->catalogRevision();
    const int assetCount = cat->assets().size();
    const int linkCount = cat->links().size();
    const int entityCount = cat->entities().size();
    int versionCount = 0;
    for (const CatalogAsset &a : cat->assets())
      versionCount += cat->versionsForAsset(a.id).size();
    const QString metaDir = QFileInfo(cat->catalogPath()).absolutePath();
    QFile sqlite(metaDir + QStringLiteral("/catalog.sqlite"));
    QVERIFY(sqlite.open(QIODevice::ReadOnly));
    const QByteArray sqliteBefore = sqlite.readAll();
    sqlite.close();
    QByteArray walBefore;
    {
      QFile wal(metaDir + QStringLiteral("/catalog.sqlite-wal"));
      if (wal.open(QIODevice::ReadOnly))
        walBefore = wal.readAll();
    }

    const IngestPlan plan = buildIngestPlan(root, *cat);
    QCOMPARE(plan.items.size(), 2);
    QCOMPARE(plan.duplicateCount(), 1); // dup.las 命中已注册字节

    QCOMPARE(cat->catalogRevision(), rev);
    QCOMPARE(cat->assets().size(), assetCount);
    QCOMPARE(cat->links().size(), linkCount);
    QCOMPARE(cat->entities().size(), entityCount);
    int versionCount2 = 0;
    for (const CatalogAsset &a : cat->assets())
      versionCount2 += cat->versionsForAsset(a.id).size();
    QCOMPARE(versionCount2, versionCount);
    QVERIFY(sqlite.open(QIODevice::ReadOnly));
    QCOMPARE(sqlite.readAll(), sqliteBefore); // 没落盘
    sqlite.close();
    QByteArray walAfter;
    {
      QFile wal(metaDir + QStringLiteral("/catalog.sqlite-wal"));
      if (wal.open(QIODevice::ReadOnly))
        walAfter = wal.readAll();
    }
    QCOMPARE(walAfter, walBefore);
  }

  // PROJECT_FILE_DESIGN 就地工程：源目录==工程根（「从工区文件夹新建」
  // 形态）时，束成员（project.paleo/proj.qgz）与 artifacts/ 受管子树
  // 不当作源数据出行；数据文件照常分类。工程之内的子目录仍拒。
  void inPlaceProjectSkipsBundleMembers()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir); // createProject 已写 project.paleo
    QVERIFY(stack != nullptr);
    DataCatalog *cat = stack->importSvc->catalog();
    QVERIFY(cat != nullptr);

    // 受管子树内的产物（受管区不应出行）
    QVERIFY(QDir().mkpath(QDir(projectDir).filePath(
        QStringLiteral("artifacts/metadata/commit_journal"))));
    QVERIFY(writeFile(QDir(projectDir).filePath(
                          QStringLiteral("artifacts/metadata/commit_journal/op-x.json")),
                      QByteArrayLiteral("{}")));
    // 源数据
    QVERIFY(writeFile(QDir(projectDir).filePath(QStringLiteral("new.las")),
        QByteArrayLiteral("~Version Information\nVERS. 2.0:\nWRAP. NO:\n"
                          "~Well\nWELL. A1 : WELL\n~Curve\nDEPT.M :\nGR.API :\n"
                          "~A DEPT GR\n101.0 50.0\n")));

    const IngestPlan plan = buildIngestPlan(projectDir, *cat);
    QVERIFY2(plan.issues.isEmpty(), qPrintable(plan.issues.join(';')));
    for (const PlannedItem &it : plan.items)
    {
      QVERIFY2(!it.path.contains(QStringLiteral("artifacts/")),
               qPrintable(it.path));
      QVERIFY2(!it.path.endsWith(QStringLiteral("proj.qgz")),
               qPrintable(it.path));
      QVERIFY2(!it.path.endsWith(QStringLiteral("project.paleo")),
               qPrintable(it.path));
    }
    // new.las 照常成项（束成员排除没有误伤源数据）
    bool foundLas = false;
    for (const PlannedItem &it : plan.items)
      if (it.path.endsWith(QStringLiteral("new.las")))
        foundLas = true;
    QVERIFY(foundLas);

    // 工程之内的子目录（受管区）仍拒——只换口径没开门。
    const IngestPlan inner = buildIngestPlan(
        QDir(projectDir).filePath(QStringLiteral("artifacts")), *cat);
    QVERIFY(inner.items.isEmpty());
    QVERIFY(!inner.issues.isEmpty());
  }

  // ---- 方向 44：DLIS/LIS 读口进导入面 ----

  // 分类词表登记 + 井名绑定：LIS 的 wellsite WELL 分量 → 已决主链接；
  // DLIS 的 ORIGIN.WELL-NAME 同口径。格式字段如实（dlis/lis）。
  void dlisAndLisClassifyBindAndKeepFirstPrimary()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj44"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir)); // 预置 A1
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    // A1 主 LAS（井名 A1 → 已决主）
    const QString lasPath = tmp.filePath(QStringLiteral("A1.Las"));
    QVERIFY(writeFile(lasPath, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.GAPI :\n~A DEPT GR\n100.0 50.0\n101.0 51.0\n")));

    // LIS：wellsite WELL=A1（lisfix 写入器的主夹具，井名等长替换为 A1
    const QString lisPath = tmp.filePath(QStringLiteral("A1-Rt.lis"));
    {
      QByteArray b = lisfix::buildMain().raw;
      b.replace(QByteArrayLiteral("HZ28-6-2"),
                QByteArrayLiteral("A1") + QByteArray(6, ' ')); // 等长（8 字节）：不动偏移
      QFile f(lisPath);
      QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
      f.write(b);
      f.close();
    }

    // DLIS：ORIGIN WELL-NAME=A1
    const QString dlisPath = tmp.filePath(QStringLiteral("A1-Rhob.dlis"));
    {
      using namespace dlisfix;
      QVector<ChannelDef> channels = {
        { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
        { QStringLiteral("RHOB"), QStringLiteral("g/cm3"), 7, {}, QString() },
      };
      QVector<FrameDef> frames = { { QStringLiteral("1000"),
                                     { QStringLiteral("DEPT"), QStringLiteral("RHOB") },
                                     QStringLiteral("BOREHOLE-DEPTH"),
                                     QString() } };
      QByteArray b = sul();
      b += buildFileHeaderEflr(QStringLiteral("IMP"));
      b += buildOriginEflr(QStringLiteral("A1"));
      b += buildChannelEflr(channels);
      b += buildFrameEflr(frames);
      QByteArray s1, s2;
      s1 += fsinglBytes(1000.0f) + fdoublBytes(2.3);
      s2 += fsinglBytes(1000.5f) + fdoublBytes(2.4);
      b += buildFdata(QStringLiteral("1000"), 1, s1);
      b += buildFdata(QStringLiteral("1000"), 2, s2);
      QFile f(dlisPath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(b);
      f.close();
    }

    QString err;
    const QString lasId = svc.importProjectFile(lasPath, &err);
    QVERIFY2(!lasId.isEmpty(), qPrintable(err));
    const QString lisId = svc.importProjectFile(lisPath, &err);
    QVERIFY2(!lisId.isEmpty(), qPrintable(err));
    const QString dlisId = svc.importProjectFile(dlisPath, &err);
    QVERIFY2(!dlisId.isEmpty(), qPrintable(err));

    DataCatalog *cat = svc.catalog();
    QCOMPARE(cat->assetById(lisId).format, QStringLiteral("lis"));
    QCOMPARE(cat->assetById(lisId).type, QStringLiteral("well_log"));
    QCOMPARE(cat->assetById(dlisId).format, QStringLiteral("dlis"));
    QCOMPARE(cat->assetById(dlisId).type, QStringLiteral("well_log"));

    // 井名绑定：三链接全部已决挂 well-A1；导入序 = 首份主/后到非主（Oracle 3）
    int primaryCount = 0;
    int linkCount = 0;
    for (const EntityAssetLink &link : cat->linksForEntity(QStringLiteral("well-A1")))
    {
      if (link.role != QLatin1String("well_log") || link.unresolved)
        continue;
      ++linkCount;
      if (link.isPrimary)
        ++primaryCount;
      QVERIFY(link.note.isEmpty()); // 解析成功不带诚实面 note
    }
    QCOMPARE(linkCount, 3);
    QCOMPARE(primaryCount, 1);

    // 井曲线并集消费面直接可达（格式无感）
    WellLogWarnings warnings;
    const QVector<WellCurveRef> refs =
        WellLogSet::wellCurveIndex(cat, projectDir, QStringLiteral("well-A1"), &warnings);
    QStringList names;
    for (const WellCurveRef &r : refs)
      names.append(r.mnemonic);
    QVERIFY(names.contains(QStringLiteral("GR")));
    QVERIFY(names.contains(QStringLiteral("RHOB")));
  }

  // 截断 DLIS 必须以真实解析错误拒绝，零新增资产/井/链接，不伪装为可用数据。
  void truncatedDlisRejectedBeforeRegistration()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj44t"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    QByteArray whole;
    {
      using namespace dlisfix;
      QVector<ChannelDef> channels = {
        { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
        { QStringLiteral("GR"), QStringLiteral("API"), 2, {}, QString() },
      };
      QVector<FrameDef> frames = { { QStringLiteral("1000"),
                                     { QStringLiteral("DEPT"), QStringLiteral("GR") },
                                     QString(), QString() } };
      whole = sul();
      whole += buildFileHeaderEflr(QStringLiteral("TRUNC"));
      whole += buildOriginEflr(QStringLiteral("NO-SUCH-WELL"));
      whole += buildChannelEflr(channels);
      whole += buildFrameEflr(frames);
      QByteArray s;
      s += fsinglBytes(100.0f) + fsinglBytes(50.0f);
      whole += buildFdata(QStringLiteral("1000"), 1, s);
    }
    const QString dlisPath = tmp.filePath(QStringLiteral("broken.dlis"));
    {
      QFile f(dlisPath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(whole.left(whole.size() - 10)); // 掐掉帧尾 + 部分段体
      f.close();
    }

    QString err;
    QSignalSpy failed(&svc, &DataImportService::importFailed);
    QSignalSpy imported(&svc, &DataImportService::imported);
    const QString assetId = svc.importProjectFile(dlisPath, &err);
    QVERIFY(assetId.isEmpty());
    QVERIFY2(err.contains(QStringLiteral("截断")), qPrintable(err));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(imported.count(), 0);
    DataCatalog *cat = svc.catalog();
    QVERIFY(cat->assets().isEmpty());
    QVERIFY(cat->links().isEmpty());
    QVERIFY(cat->entities(QStringLiteral("well")).isEmpty());
  }

};

int main(int argc, char *argv[])
{
  // Offscreen QGIS bootstrap through the runtime that owns init order.
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestImport tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_import.moc"
