// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <QColor>

// domain/ — pure C++/Qt value types, NO Qgs* dependencies (§25: domain never leaks QGIS).
// All types roundtrip through QVariantMap for project.sqlite JSON persistence.

struct Horizon {
  QString id;          // "T1"
  QString name;        // display name
  int order = 0;       // stratigraphic order (shallow->deep or convention per project)
  QColor color;
  QVariantMap toMap() const;
  static Horizon fromMap(const QVariantMap &m);
};

struct Facies {
  int code = 0;        // raster cell value
  QString name;        // "河道" etc
  QColor color;
  QVariantMap toMap() const;
  static Facies fromMap(const QVariantMap &m);
};

struct Constraint {
  QString id;
  QString type;        // "line" | "polygon" | "point"
  QString wkt;         // geometry WKT in project CRS
  int targetFaciesCode = -1; // facies this constraint pushes toward
  double weight = 1.0;
  QVariantMap toMap() const;
  static Constraint fromMap(const QVariantMap &m);
};

struct DataAsset {
  QString id;
  QString kind;        // "well" | "seismic" | "boundary" | "grid" | "raster"
  QString uri;         // file path or provider URI
  QString crs;         // authid e.g. "EPSG:4326"
  QString horizon;     // owning horizon, "" if none
  QVariantMap toMap() const;
  static DataAsset fromMap(const QVariantMap &m);
};

struct ValidationIssue {
  enum Severity { Info, Warning, Error };
  Severity severity = Warning;
  QString code;        // machine code, e.g. "DUP_HORIZON_NAME"
  QString message;     // human readable (tr'd at creation)
  QString layerId;     // manifest layer id, "" if n/a
  QString horizon;
  QString wktLocation; // optional locate target
  // wave/mapping-pipeline — 时间残差问题的机器载荷：wellId 定位到井，
  // details 携带 well_x/well_y/inline/time_ms/raster_ms/residual_ms。
  // fromMap 缺省可空（旧记录前向兼容）。
  QString wellId;
  QVariantMap details;
  QVariantMap toMap() const;
  static ValidationIssue fromMap(const QVariantMap &m);
};

// §34 release semantics — semantic version + provenance label.
struct Version {
  int major = 0, minor = 0, patch = 0;
  QString label;       // e.g. "draft" | "release" | provenance tag
  QString toString() const;   // "1.2.3-label"
  static Version parse(const QString &s, bool *ok = nullptr);
  bool isRelease() const { return label == QStringLiteral("release"); }
};
