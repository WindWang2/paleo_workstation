// 层：数据
#pragma once
#include "domain/documentsource.h"
#include "pythoninputbundle.h"
#include "pythonprocess.h"
#include <paleo_data/numeric_table.h>

namespace paleo::python {
inline constexpr char NumericTableCapability[]="python.numeric_table_compute";
inline constexpr char NumericTableTool[]="paleo.python_compute_table";
inline constexpr char NumericTableInputContract[]="paleo.python-numeric-table-input.v1";
struct NumericTableSelection {
  document::SourceRef source;
  qlonglong tableId=0,firstRow=0;
  QString projectionRevision;
  QStringList columnIds;
  int rowCount=0;
};
struct NumericTableReadPlan {
  NumericTableSelection selection;
  document::SourceRecord source; // Host-resolved Catalog source version; never a fabricated table version.
  QString projectDir,modelDirectory,documentId,callerScope;
  qint64 readByteBudget=8*1024*1024;
};
struct NumericTableMaterialization {
  QString code;
  InputBundle bundle;
  QJsonObject metadata;
  data::NumericTableReadStats io;
  qint64 sourceMetadataBytes=0;
  bool ok()const{return code==QLatin1String("ok");}
};
bool numericTableSelectionFromJson(const QJsonObject &,NumericTableSelection &,QString *error=nullptr);
QJsonObject numericTableSelectionJson(const NumericTableSelection &);
QJsonObject numericTableToolRequestSchema();
QString validateNumericTableToolArguments(const QJsonObject &);
// Requires a shared execution permit before SDK I/O or payload allocation.
NumericTableMaterialization materializeNumericTable(const NumericTableReadPlan &,const QByteArray &sourceCode,
  const ExecutionPolicy &,const std::shared_ptr<ExecutionPermit> &,const std::function<bool()> &cancelled={});
bool validateNumericTableInputMetadata(const QJsonObject &);
QJsonObject numericTableReadCutJson(const data::NumericTableReadCut &);
}
