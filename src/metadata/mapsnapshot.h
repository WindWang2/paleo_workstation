// 层：数据
#pragma once
#include "layermanifest.h"
#include <QVariantMap>
#include <functional>

// Owner thread captures SQL facts. Worker sees only values and owned file paths.
struct MapSnapshotInput {
  QString projectDirectory, horizon, operation, provenanceJson, residualSummary;
  QString pdfAssetId, pdfSha256;
  int versionId = 0, versionNumber = 0;
  QVariantList files;
  // #439：调用方传入了图层清单时才写 snapshot_schema。
  bool freezeLayers = false;
  bool valid = false;
};
struct MapSnapshotAccounting {
  int sourceHashes = 0;
  int destinationHashes = 0;
  qint64 bytesCopied = 0;
  int files = 0;
};
struct PreparedMapSnapshot {
  MapSnapshotInput input;
  QString directory, finalDirectory, error;
  QVariantMap provenance;
  QVariantList attestations;
  MapSnapshotAccounting accounting;
  bool cancelled = false, committed = false;
  bool isValid() const { return error.isEmpty() && !cancelled && !directory.isEmpty(); }
};
QString mapSnapshotHash(const QVariantMap &provenance);
PreparedMapSnapshot prepareMapSnapshot(const MapSnapshotInput &input,
    const std::function<bool()> &cancelled = {},
    const std::function<void(double)> &progress = {});
bool verifyPreparedMapSnapshot(const PreparedMapSnapshot &snapshot, QString *error);
void discardPreparedMapSnapshot(const PreparedMapSnapshot &snapshot);
