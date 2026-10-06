// 层：数据
#pragma once
#include <QMap>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

#include <optional>

// metadata/ — PaleoProjectFile：工程束清单（工程根目录 project.paleo）。
// docs/PROJECT_FILE_DESIGN.md：清单是工程唯一入口，.qgz 退居成员之一。
// 纯 Qt JSON，无 QGIS 依赖；写走 QSaveFile 原子替换。
//
// 打开契约：读 .paleo → qgz 成员缺失/坏 → 拒开；catalog/manifest/gpkg/
// areaRules 缺失 → 如实报 lastErrors 仍开。读 .qgz 且旁无 .paleo →
// 收养（写一份清单）。清单损坏/formatVersion 超实现 → 打开报错。

// 工程级地理配准：局部工程米制网格 → 真实地理坐标（WGS84 经纬度）的
// 2D 相似变换（旋转+等比缩放+平移，附切平面锚点换算成度）。
//   E = a*x - b*y + tE    （东向米，a = s·cosθ、b = s·sinθ）
//   N = b*x + a*y + tN    （北向米）
//   lon = anchorLon + E/metersPerDegLon ; lat = anchorLat + N/metersPerDegLat
// 控制点与残差仅作审查溯源（applyGeoreference 不用它们）。
struct PaleoGeoreference
{
  QString kind = QStringLiteral("similarity2d");
  QString targetCrs; // 如 "EPSG:4326"

  double anchorLonDeg = 0.0;   // 切平面锚点（E/N 的度换算基准）
  double anchorLatDeg = 0.0;
  double metersPerDegLon = 0.0;
  double metersPerDegLat = 0.0;

  double a = 0.0, b = 0.0, tE = 0.0, tN = 0.0;

  QString formula;    // 人类可读公式（展示/审查用，机器不解析）
  QString provenance; // 参数来源说明（拟合方法/日期/控制点出处）

  struct ControlPoint
  {
    QString well;
    double x = 0.0, y = 0.0;       // 局部网格坐标
    double lon = 0.0, lat = 0.0;   // 实测经纬度
    double residualM = 0.0;        // 拟合残差（米）
  };
  QVector<ControlPoint> controlPoints;
  double maxResidualM = 0.0;

  // 数值字段全有限且度米系数为正 → 可用。
  bool isComplete() const;
};

// 应用配准：局部网格 (x,y) → WGS84 经纬度。g 不完整 → 返回 false 不写出。
bool applyGeoreference(const PaleoGeoreference &g, double x, double y,
                       double *lonDeg, double *latDeg);

// georeference 节的 JSON 序列/反序列——project.paleo 内嵌与 QgsProject 自定义
// 属性（QgisProjectService 嵌入副本）共用同一形状。坏节 → false+error。
QJsonObject paleoGeoreferenceToJson(const PaleoGeoreference &g);
bool paleoGeoreferenceFromJson(const QJsonObject &o, PaleoGeoreference *g,
                               QString *error);

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

  // 工程级地理配准（可选）。节缺失 = 无配准（现状行为）；节存在但参数
  // 不完整时读端置 georeferenceError（打开如实报 lastErrors，不拦打开）。
  std::optional<PaleoGeoreference> georeference;
  QString georeferenceError;
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
