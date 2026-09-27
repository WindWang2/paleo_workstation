#pragma once
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// metadata/ — PaleoProjectFile：工程束清单（工程根目录 project.paleo）。
// docs/PROJECT_FILE_DESIGN.md：清单是工程唯一入口，.qgz 退居成员之一。
// 纯 Qt JSON，无 QGIS 依赖；写走 QSaveFile 原子替换。
//
// 打开契约：读 .paleo → qgz 成员缺失/坏 → 拒开；catalog/manifest/gpkg/
// areaRules 缺失 → 如实报 lastErrors 仍开。读 .qgz 且旁无 .paleo →
// 收养（写一份清单）。清单损坏/formatVersion 超实现 → 打开报错。

struct PaleoProjectFile
{
  static constexpr const char *kFileName = "project.paleo";

  int formatVersion = 1;
  QString name;
  QString projectId;
  QString createdUtc;

  // 束成员：全部工程根相对路径；允许空串（该成员工程里尚未产生）。
  QString qgz;       // *.qgz —— 唯一硬要求成员
  QString catalog;   // artifacts/metadata/catalog.json
  QString manifest;  // <qgz>.project.sqlite
  QString gpkg;      // <basename>.gpkg
  QString areaRules; // project_area.json

  // 「从工区文件夹新建」的溯源；手工新建为空。
  QString sourceAreaRoot;
  QString sourceAreaImportedUtc;
  QVariantMap sourceStats; // files/rows/failed
};

// <dir>/project.paleo 的路径。
QString paleoProjectFilePath(const QString &projectDir);

// 写清单到 dir（project.paleo）；原子落盘。dir 不存在/不可写 → false+error。
bool writeProjectFile(const QString &projectDir, const PaleoProjectFile &file,
                      QString *error = nullptr);

// 读清单。JSON 坏/非对象/format 不对/version>实现 → ok=false + error。
// opId/id 字段缺失不视为损坏（老清单兼容）——name/qgz 缺席才报成员缺失。
PaleoProjectFile readProjectFile(const QString &path, bool *ok = nullptr,
                                 QString *error = nullptr);

// 束校验：返回缺失的成员文件清单（人类可读 "catalog: artifacts/…"）。
// 只查存在的成员声明；空串成员跳过（工程早期没建不算缺失）。
QStringList missingMembers(const QString &projectDir,
                           const PaleoProjectFile &file);

// 依据 qgz 路径推导一份新清单（basename/name/gpkg/sqlite 约定与
// appcontext 的 manifestPathFor/gpkgPathFor 一致）。
PaleoProjectFile projectFileForQgz(const QString &qgzPath);
