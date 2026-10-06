// 层：数据
// 方向57：从 dataimportservice.cpp 按格式族析出。公共 API（dataimportservice.h）
// 零改动；跨族共享辅助（setError/readFileOrEmpty/FamilyContext）经
// dataimport_internal.h；importOneFile 分支体族函数亦声明于该头。
#include "dataimportservice.h"

#include "lascache.h"
#include "rasterpyramid.h"
#include "segyindexstore.h"
#include "shacache.h"

#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../metadata/paleoprojectstore.h"
#include "../domain/arearules.h"
#include "horizonbinner.h"
#include "ingestplan.h"
#include "lasparser.h"
#include "welllogread.h" // 方向44：井名提取分派
#include "../domain/projectclassifier.h"
#include "segyreader.h"
#include "wellcompositexml.h"
#include "wellfileparsers.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <QUuid>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal.h>
#include <QStandardPaths>
#include <QUrl>

#include "../metadata/atomicfile.h"

#include <algorithm>
#include <cstdio>
#include "dataimport_internal.h"

using paleo::dataimport_detail::setError;
using paleo::dataimport_detail::readFileOrEmpty;

// ---------------------------------------------------------------------------
// 文档 PDF 预览：doc/docx/ppt/pptx 经 LibreOffice headless 转 PDF，落受管
// DERIVED 版本（parent=RAW），预览标签页用 QtPdf 渲染。原件仍是规范来源；
// 转换失败/无 soffice 如实 Failed，UI 降级为「用系统程序打开」。
// ---------------------------------------------------------------------------

void DataImportService::resolveDocumentConverter()
{
  if (m_converterResolved)
    return;
  m_converter = QStandardPaths::findExecutable(QStringLiteral("soffice"));
  if (m_converter.isEmpty())
    m_converter = QStandardPaths::findExecutable(QStringLiteral("libreoffice"));
  m_converterResolved = true;
}

void DataImportService::setDocumentConverterProgram(const QString &program)
{
  m_converter = program;
  m_converterResolved = true;
}

DataImportService::DocPdfState
DataImportService::documentPdfState(const QString &assetId) const
{
  for (const CatalogVersion &v : m_catalog->versionsForAsset(assetId))
    if (v.stage == QLatin1String("DERIVED") &&
        v.fileName.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
      return DocPdfState::Ready;
  if (m_pdfPending.contains(assetId))
    return DocPdfState::Pending;
  if (m_pdfErrors.contains(assetId))
    return DocPdfState::Failed;
  return DocPdfState::None;
}

QString DataImportService::documentPdfPath(const QString &assetId) const
{
  for (const CatalogVersion &v : m_catalog->versionsForAsset(assetId))
    if (v.stage == QLatin1String("DERIVED") &&
        v.fileName.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
      return absolutePathForVersion(v);
  return QString();
}

QString DataImportService::documentPdfError(const QString &assetId) const
{
  return m_pdfErrors.value(assetId);
}

void DataImportService::ensureDocumentPdf(const QString &assetId)
{
  if (documentPdfState(assetId) != DocPdfState::None)
    return;

  const auto failNow = [this, &assetId](const QString &msg) {
    m_pdfErrors.insert(assetId, msg);
    emit documentPdfFailed(assetId, msg);
  };

  resolveDocumentConverter();
  if (m_converter.isEmpty())
    return failNow(tr("找不到 LibreOffice（soffice）——无法生成 PDF 预览"));

  QString rawAbs, rawVersionId;
  CatalogVersion rawVersion;
  for (const CatalogVersion &v : m_catalog->versionsForAsset(assetId))
    if (v.stage == QLatin1String("RAW"))
    {
      rawAbs = absolutePathForVersion(v);
      rawVersionId = v.id;
      rawVersion = v;
      break;
    }
  if (rawAbs.isEmpty() || !QFile::exists(rawAbs))
    return failNow(tr("原始文件缺失，无法转换"));

  // T8 staleness 补漏：外链 RAW 带指纹时转换前复验——soffice 读的是当前
  // 字节，源被改后转出的 DERIVED 与入库指纹无血缘；失配如实失败 + 下游
  // DERIVED 标过时（与剖面解码路径同一纪律，previewdoc.cpp 的两处之外的
  // 第三个失配入口）。
  if (!rawVersion.managed && !rawVersion.sha256.isEmpty())
  {
    QString verr;
    if (!m_catalog->verifyExternalVersionSha(rawVersion, &verr))
    {
      QString markErr;
      if (!m_catalog->markDownstreamStale(rawVersion.id,
                                          QStringLiteral("上游外链版本 sha 校验失败"),
                                          &markErr))
        qWarning() << "markDownstreamStale:" << markErr;
      return failNow(verr);
    }
  }

  m_pdfPending.insert(assetId);
  m_pdfQueue.append(assetId);
  startNextDocumentPdf();
}

void DataImportService::startNextDocumentPdf()
{
  if (m_pdfProc || m_pdfQueue.isEmpty())
    return;

  m_pdfCurrent = m_pdfQueue.takeFirst();
  m_pdfCurrentVersionId = m_catalog->nextVersionId();

  QString rawAbs;
  for (const CatalogVersion &v : m_catalog->versionsForAsset(m_pdfCurrent))
    if (v.stage == QLatin1String("RAW"))
    {
      rawAbs = absolutePathForVersion(v);
      m_pdfRawVersionId = v.id;
      break;
    }

  const QString relDir = QStringLiteral("artifacts/derived/%1/%2")
                             .arg(m_pdfCurrent, m_pdfCurrentVersionId);
  const QString outDir = QDir(m_projectDir).absoluteFilePath(relDir);
  if (!QDir().mkpath(outDir))
    return finishDocumentPdf(-1);
  m_pdfOutFile = outDir + QLatin1Char('/') +
                 QFileInfo(rawAbs).completeBaseName() + QStringLiteral(".pdf");

  // 独立 UserInstallation：避开 LibreOffice 单实例 profile 锁，且 URL 合规。
  const QString profileDir = QDir::temp().filePath(
      QStringLiteral("paleo-lo-profile-%1").arg(QCoreApplication::applicationPid()));
  const QString profile = QStringLiteral("-env:UserInstallation=") +
                          QUrl::fromLocalFile(profileDir).toString();

  m_pdfProc = new QProcess(this);
  connect(m_pdfProc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this, [this](int code, QProcess::ExitStatus status) {
            finishDocumentPdf(status == QProcess::NormalExit ? code : -1);
          });
  m_pdfProc->start(m_converter,
                   {QStringLiteral("--headless"), QStringLiteral("--norestore"),
                    profile, QStringLiteral("--convert-to"), QStringLiteral("pdf"),
                    QStringLiteral("--outdir"), outDir, rawAbs});
}

void DataImportService::finishDocumentPdf(int exitCode)
{
  const QString assetId = m_pdfCurrent;
  QString err;
  bool ok = false;

  if (exitCode == 0 && QFile::exists(m_pdfOutFile))
  {
    QFile f(m_pdfOutFile);
    if (f.open(QIODevice::ReadOnly))
    {
      QCryptographicHash hash(QCryptographicHash::Sha256);
      hash.addData(&f);
      f.close();
      QFile::setPermissions(m_pdfOutFile, QFileDevice::ReadOwner |
                                              QFileDevice::ReadUser |
                                              QFileDevice::ReadGroup |
                                              QFileDevice::ReadOther);

      CatalogVersion d;
      d.id = m_pdfCurrentVersionId;
      d.assetId = assetId;
      d.stage = QStringLiteral("DERIVED");
      d.versionNumber = m_catalog->currentVersion(assetId).versionNumber + 1;
      d.managed = true;
      d.path = QStringLiteral("artifacts/derived/%1/%2/%3")
                   .arg(assetId, m_pdfCurrentVersionId, QFileInfo(m_pdfOutFile).fileName());
      d.sourceUri = m_converter;
      d.sha256 = QString::fromLatin1(hash.result().toHex());
      d.fileName = QFileInfo(m_pdfOutFile).fileName();
      d.parentVersionIds = QStringList{m_pdfRawVersionId};
      d.extra.insert(QStringLiteral("generator"), QStringLiteral("libreoffice"));
      ok = m_catalog->addVersion(d, &err);
    }
    else
      err = f.errorString();
  }
  else
    err = m_pdfProc ? QString::fromLocal8Bit(m_pdfProc->readAllStandardError()).trimmed()
                    : tr("无法创建输出目录");
  if (err.isEmpty() && !ok)
    err = tr("soffice 退出码 %1，未产出 PDF").arg(exitCode);

  if (!ok && QFile::exists(m_pdfOutFile))
    QFile::remove(m_pdfOutFile);

  if (m_pdfProc)
  {
    m_pdfProc->deleteLater();
    m_pdfProc = nullptr;
  }
  m_pdfCurrent.clear();
  m_pdfPending.remove(assetId);

  if (ok)
    emit documentPdfReady(assetId);
  else
  {
    m_pdfErrors.insert(assetId, err);
    emit documentPdfFailed(assetId, err);
  }
  startNextDocumentPdf();
}


// ---------------------------------------------------------------------------
// wave4/runtime-resilience：外链源重定位（TODOS P3「重新定位文件」恢复路径）。
// 「找不到源文件」死胡同的出口——但出口不是换内容：流式重算候选文件 SHA-256，
// 与该版本入库时留底一致才接受。版本记录不可变：不改写旧记录，而是追加一条
// 同内容、指向新路径的外链 RAW 版本（extra.relocatedFrom 留血统），
// currentVersion（取最高 versionNumber）从此解析到新路径；dedup 的
// versionBySha256 只认文件仍在且重哈希一致的版本——死路径旧记录自动出局，
// 重导/预览两条链路都不需要特判。不一致 → 拒解，catalog 一字不动（不静默
// 换源）。新路径落在工程目录内也仍按 external 记（不升级为 managed——工程
// 目录内容物是 catalog 自己的产物，混入外部文件会破坏受管面语义）。
// ---------------------------------------------------------------------------
QString DataImportService::relocateVersionSource(const QString &versionId,
                                                 const QString &newPath, QString *error)
{
  QString internalError;
  if (!error)
    error = &internalError;
  else
    error->clear();
  const auto fail = [&](const QString &msg) -> QString {
    setError(error, msg);
    return QString();
  };

  // 审计 02 M-8：活 catalog 只在 owner 线程读写——跨线程调用如实拒绝。
  if (QThread::currentThread() != m_catalog->thread())
    return fail(offThreadError());
  if (m_projectDir.isEmpty())
    return fail(QStringLiteral("project dir is not set"));
  if (!m_catalogReady)
    return fail(QStringLiteral("catalog 拒绝写入：%1")
                    .arg(m_catalogOpenError.isEmpty()
                             ? QStringLiteral("catalog 打开失败")
                             : m_catalogOpenError));
  const CatalogVersion v = m_catalog->versionById(versionId);
  if (v.id.isEmpty())
    return fail(QStringLiteral("版本不存在: %1").arg(versionId));
  if (v.managed)
    return fail(QStringLiteral("受管版本不支持重定位（受管文件属于工程目录，丢失应重导）: %1")
                    .arg(v.path));
  if (v.sha256.isEmpty())
    return fail(QStringLiteral("该版本入库时未留 SHA-256，无法核验新文件内容"));
  if (newPath.isEmpty() || !QFile::exists(newPath))
    return fail(QStringLiteral("找不到源文件: %1").arg(newPath));
  const QFileInfo fi(newPath);
  if (!fi.isFile())
    return fail(QStringLiteral("不是普通文件: %1").arg(newPath));
  if (!DataCatalog::isSafePathSegment(fi.fileName()))
    return fail(QStringLiteral("文件名不是合法路径段: %1").arg(fi.fileName()));
  const QString abs = fi.absoluteFilePath();
  if (abs == v.path)
    return versionId; // 幂等：同一文件已在原位恢复，不动 catalog

  // 流式 SHA-256 复验（与导入/外链校验同一面）。
  QString herr;
  const QString sha = ShaCache::shared().sha256Hex(abs, &herr); // D7.7
  if (sha.isEmpty())
    return fail(herr.isEmpty() ? QStringLiteral("cannot hash %1").arg(abs) : herr);
  if (sha.compare(v.sha256, Qt::CaseInsensitive) != 0)
    return fail(QStringLiteral("文件内容与原版本不符（SHA-256 不一致）——已拒绝重定位"));

  CatalogVersion relocated;
  relocated.id = m_catalog->nextVersionId();
  relocated.assetId = v.assetId;
  relocated.stage = v.stage.isEmpty() ? QStringLiteral("RAW") : v.stage;
  relocated.versionNumber =
      m_catalog->currentVersion(v.assetId).versionNumber + 1;
  relocated.managed = false;
  relocated.path = abs;
  relocated.sourceUri = abs;
  relocated.sha256 = v.sha256; // 已验证一致——沿用留底值
  relocated.fileName = fi.fileName();
  relocated.extra = v.extra;   // 外链夹带的图例等元数据随内容一起搬
  relocated.extra.insert(QStringLiteral("relocatedFrom"), v.id);
  if (!m_catalog->addVersion(relocated, error))
    return fail(error->isEmpty() ? QStringLiteral("catalog addVersion failed") : *error);
  qInfo("relocate: %s -> %s (asset %s, from version %s)", qPrintable(v.path),
        qPrintable(abs), qPrintable(v.assetId), qPrintable(v.id));
  return relocated.id;
}
