// 层：数据
#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>
#include <QMap>

namespace paleo {
// Values only: no Catalog, layer declaration or owner QObject on the worker.
struct FrozenRasterInput {
  QString path, versionId, sha256, projection, bandUnit;
  int rows = 0, columns = 0;
  QVector<double> transform;
  QJsonObject featureContract, catalogFeatureContract;
  QMap<QString,QString> companionSha256;
  QString bundleSha256() const;
  QJsonObject toJson() const;
  static bool fromJson(const QJsonObject &, FrozenRasterInput &, QString *error);
};
bool normalizeRasterFeatureContract(const QJsonObject &,QJsonObject &out);
bool freezeRasterInput(const QString &path, const QString &versionId, const QString &sha256,
                       FrozenRasterInput &, const std::function<bool()> &cancelled, QString *error,
                       const QJsonObject &catalogFeatureContract = {});
// Opens one exact source once, checks its bytes on entry and before delivery.
// A changed source may invalidate the entire result; it never silently follows a layer.
class FrozenRasterReader {
public:
  explicit FrozenRasterReader(FrozenRasterInput input);
  ~FrozenRasterReader();
  bool open(const std::function<bool()> &cancelled, QString *error);
  bool read(int row, int column, int rows, int columns, QVector<float> &out,
            const std::function<bool()> &cancelled, QString *error);
  bool validate(const std::function<bool()> &cancelled, QString *error) const;
private:
  struct Impl;
  std::unique_ptr<Impl> d;
};
} // namespace paleo
