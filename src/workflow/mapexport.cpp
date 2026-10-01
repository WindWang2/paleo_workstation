// 层：功能
#include "mapexport.h"

#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../metadata/atomicfile.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/layoutexport.h" // PaleoLayoutExport::exportLayout（QGIS 封装层导出核心）

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <qgsprintlayout.h>

namespace
{
  void setError( QString *error, const QString &text )
  {
    if ( error )
      *error = text;
  }
} // namespace

QgsPrintLayout *buildHorizonMapLayout( QgisLayerService *layers, QgisProjectService *projectSvc,
                                       const QString &horizon, QString *error )
{
  return PaleoLayoutExport::buildHorizonMapLayout( layers, projectSvc, horizon, error );
}

QString exportHorizonMapPdf( QgisLayerService *layers, QgisProjectService *projectSvc,
                             const QString &horizon, const QString &outPath, QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return QString();
  };
  if ( outPath.isEmpty() )
    return fail( QObject::tr( "导出路径为空" ) );

  QgsPrintLayout *layout = buildHorizonMapLayout( layers, projectSvc, horizon, error );
  if ( !layout )
    return QString();

  const auto outcome = PaleoLayoutExport::exportLayout(
      layout, outPath, PaleoLayoutExport::Format::Pdf, 300.0,
      PaleoLayoutExport::PageRange() );
  delete layout;
  if ( !outcome.ok )
    return fail( outcome.error );
  return outcome.files.value( 0 );
}

QString registerMapPdfAsset( DataCatalog *catalog, const QString &projectDir,
                             const QString &pdfPath, QString *sha256Out,
                             QString *managedPathOut, QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return QString();
  };
  if ( !catalog )
    return fail( QObject::tr( "未绑定数据目录" ) );
  if ( projectDir.isEmpty() )
    return fail( QObject::tr( "工程目录未设置" ) );
  if ( pdfPath.isEmpty() || !QFile::exists( pdfPath ) )
    return fail( QObject::tr( "找不到 PDF 文件: %1" ).arg( pdfPath ) );

  // 文件摘要先算 —— dedup 与登记共用同一份 SHA-256。
  QString shaErr;
  const QString sha = DataCatalog::sha256FileHex( pdfPath, &shaErr );
  if ( sha.isEmpty() )
    return fail( shaErr.isEmpty() ? QObject::tr( "无法计算 SHA-256: %1" ).arg( pdfPath )
                                  : shaErr );

  // §3 dedup：同 SHA-256 已在库 → 复用既有版本的资产，不新增。
  const CatalogVersion existing = catalog->versionBySha256( sha );
  if ( !existing.id.isEmpty() )
  {
    if ( sha256Out )
      *sha256Out = sha;
    if ( managedPathOut )
      *managedPathOut = DataCatalog::resolvedVersionPath( projectDir, existing );
    return existing.assetId;
  }

  const QString fileName = QFileInfo( pdfPath ).fileName();
  if ( !DataCatalog::isSafePathSegment( fileName ) )
    return fail( QObject::tr( "文件名不是合法路径段: %1" ).arg( fileName ) );

  const QString assetId = catalog->nextAssetId();
  const QString versionId = catalog->nextVersionId();
  const QString relDir =
      DataCatalog::managedPath( QStringLiteral( "output" ), assetId, versionId, fileName );
  if ( relDir.isEmpty() )
    return fail( QObject::tr( "受管路径段不合法（output/%1/%2）" ).arg( assetId, versionId ) );
  const QString relPath = QStringLiteral( "artifacts/" ) + relDir;
  CatalogVersion pending;
  pending.managed = true;
  pending.path = relPath;
  const QString dst = DataCatalog::resolvedVersionPath( projectDir, pending );
  if ( dst.isEmpty() )
    return fail( QObject::tr( "unsafe managed destination: %1" ).arg( relPath ) );
  const QDir dir = QFileInfo( dst ).absoluteDir();
  if ( !dir.exists() && !dir.mkpath( QStringLiteral( "." ) ) )
    return fail( QObject::tr( "cannot create directory %1" ).arg( dir.absolutePath() ) );

  // 受管副本：partial + rename 原子落位，成功后置只读（与 RAW 入库同纪律）。
  const QString partial = dst + QStringLiteral( ".partial" );
  if ( !QFile::copy( pdfPath, partial ) )
    return fail( QObject::tr( "cannot copy %1 → %2" ).arg( pdfPath, partial ) );
  if ( !paleoReplaceFile( partial, dst ) )
  {
    QFile::remove( partial );
    return fail( QObject::tr( "cannot place %1" ).arg( dst ) );
  }
  QFile::setPermissions( dst, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                  QFileDevice::ReadGroup | QFileDevice::ReadOther );

  CatalogAsset asset;
  asset.id = assetId;
  asset.type = QStringLiteral( "document" ); // 图件 PDF 归 document 一类
  asset.format = QStringLiteral( "pdf" );
  asset.displayName = fileName;
  if ( !catalog->addAsset( asset, error ) )
    return fail( error ? *error : QStringLiteral( "catalog addAsset failed" ) );

  CatalogVersion ver;
  ver.id = versionId;
  ver.assetId = assetId;
  ver.stage = QStringLiteral( "OUTPUT" );
  ver.versionNumber = 1;
  ver.managed = true;
  ver.path = relPath;
  ver.sourceUri = QFileInfo( pdfPath ).absoluteFilePath();
  ver.sha256 = sha;
  ver.fileName = fileName;
  if ( !catalog->addVersion( ver, error ) )
    return fail( error ? *error : QStringLiteral( "catalog addVersion failed" ) );

  if ( sha256Out )
    *sha256Out = sha;
  if ( managedPathOut )
    *managedPathOut = dst;
  return assetId;
}
