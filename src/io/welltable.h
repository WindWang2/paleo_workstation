// 层：数据
#pragma once
#include "../domain/wellrecords.h"
#include <QVariantMap>
#include <functional>
struct ImportSession;
struct CatalogVersion;

namespace paleo::io {
// P1-W5：可选协作取消谓词（返回 true = 停止读取）。缺省空 = 旧行为不变
//（兼容补充）。读取被取消时 WellTable::cancelled 置位、error 以「已取消」
// 开头——取消与格式/内容错误区分，不互相伪装。
using WellTableCancelCheck = std::function<bool()>;

struct WellTable
{
  QString type, crs, error;
  QStringList sheets, columns;
  QVariantMap mapping;
  QVector<QStringList> preview;
  QVector<WellHeadRecord> heads;
  QVector<WellTopRecord> tops;
  WellParseReport report;
  QByteArray normalized;
  bool scanLimited = false; // Scan sample only; full validation/counting is deferred.
  bool containerBroken = false; // P1-W6：底层容器（工作簿/矢量）读取失败——与「读成功但不是井表」区分
  bool cancelled = false;       // P1-W5：取消谓词触发（error 以「已取消」开头）
};
bool isWellTableContainer(const QString &path);
QString wellSourceCrsHint(const QString &text);
bool projectWellCoordinates(QVector<WellHeadRecord> *heads, const QString &source, const QString &destination, QString *error);
WellTable readWellTable(const QString &path, const QVariantMap &mapping = {},
                        const WellTableCancelCheck &cancel = {});
// 同一次读表同时返回显式底界；不追加调用方传入的旧记录。
WellTable readWellTable(const QString &path, const QVariantMap &mapping, QVector<WellTopRecord> *bases,
                        const WellTableCancelCheck &cancel = {});
WellTable previewWellTable(const QString &path, const QVariantMap &mapping = {});
// 同一表的显式层底界；使用独立边界记录，MD/TVD 缺失标记保持一致。
// error 非空且读失败（打不开 / 超底界读取限额）时写入原因并返回空。
// 读成功但确实没有底界行：error 清空，返回空，调用方仍可写顶界。
QVector<WellTopRecord> readWellTableBases(const QString &path, QString *error = nullptr);
QByteArray writeWellLayerTable(const QVector<WellTopRecord> &tops, const QVector<WellTopRecord> &bases);
bool resolveImportedWellDepths(ImportSession &session, QString *error);
struct ValidatedWellInput {
  QString declaredTopDepthUnit;
  QVector<WellHeadRecord> heads;
  QVector<WellTopRecord> tops;
  TimeDepthTable td;
  DeviationTable deviation;
  WellParseReport report;
  QByteArray normalized;
};
bool readImportWellInput(const QString &path, const QString &type, const QString &expectedHash,
                        const QVariantMap &mapping, ValidatedWellInput *out, QString *error,
                        const WellTableCancelCheck &cancel = {});
bool storeNormalizedWellTable(ImportSession &session, const CatalogVersion &raw,
                             const QByteArray &text, QString *error);
}
