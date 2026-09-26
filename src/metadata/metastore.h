#pragma once
#include <QString>

class QSqlDatabase;

// metadata/metastore — metadata/project.sqlite 的共享 schema 版本门
// （wave3/model-hardening；docs/SCHEMA_MIGRATION.md）。
// 三个 store（LayerManifest / MapVersionStore / ReleaseStore）以各自命名
// 连接打开同一个 sqlite 文件；user_version 门在连接打开后、任何建表/补列
// 之前执行：
//   user_version == 0            → 新库或遗留库（表存在但从未设版本）→ 推进到当前版本
//   user_version == kUserVersion → 放行
//   user_version >  kUserVersion → 本构建不认识的未来版本 → 拒开（错误写明
//                                  版本号），此时还没发生任何写入，原件保留。
// 迁移只走「向前一小步」：每个版本号对应一次单调升级，永不降级。
namespace MetaStore
{
  const int kUserVersion = 1;

  // 读 user_version；查询失败回 -1 并置 *error。
  int readUserVersion(QSqlDatabase &db, QString *error = nullptr);

  // 门本体：db 必须已 open。按上述规则检查/推进；拒绝或执行失败 → false +
  // *error（调用方应让 open 整体失败，不得继续建表）。
  bool ensureUserVersion(QSqlDatabase &db, QString *error);
}
