// 层：数据
#pragma once
#include <QString>

class QSqlDatabase;

// metadata/metastore — metadata/project.sqlite 的共享 schema 版本门
// （wave3/model-hardening；docs/SCHEMA_MIGRATION.md）。
// 三个 store（LayerManifest / MapVersionStore / ReleaseStore）以各自命名
// 连接打开同一个 sqlite 文件；user_version 门在连接打开后、任何建表/补列
// 之前执行：
//   user_version <  kUserVersion → 新库或上一版（含 0 与 1）→ 推进到当前版本
//   user_version == kUserVersion → 放行（当前 2：fault_set.surface 列）
//   user_version >  kUserVersion → 本构建不认识的未来版本 → 拒开（错误写明
//                                  版本号），此时还没发生任何写入，原件保留。
// 迁移只走「向前一小步」：每个版本号对应一次单调升级，永不降级。
namespace MetaStore
{
  const int kUserVersion = 2;

  // Reuses the caller's named connection, creates its directory if needed, and
  // checks user_version even for an already-open connection. Invalid on failure.
  //
  // readOnly（#80，单写实例降级 §6）：以 QSQLITE_OPEN_READONLY 打开；文件不存在
  // 时拒开（不 mkpath、不建库）；user_version 只读校验（未来版本仍拒开），
  // 不推进。调用方须为只读连接使用独立连接名（与可写连接互不复用）。
  QSqlDatabase openConnection(const QString &path, const QString &connectionName,
                              QString *error, bool readOnly = false);

  // #80：关闭并从 QtSql 注册表移除「当前线程拥有、databaseName 指向 path」的
  // 全部命名连接（工程切换/关闭时调用，避免句柄常驻与同路径陈旧连接复用）。
  // 调用时不得有存活的 QSqlQuery。返回移除的连接数。
  int closeConnectionsFor(const QString &path);

  // 读 user_version；查询失败回 -1 并置 *error。
  int readUserVersion(QSqlDatabase &db, QString *error = nullptr);

  // 门本体：db 必须已 open。按上述规则检查/推进；拒绝或执行失败 → false +
  // *error（调用方应让 open 整体失败，不得继续建表）。
  bool ensureUserVersion(QSqlDatabase &db, QString *error);
}
