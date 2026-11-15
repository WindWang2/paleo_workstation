// 层：数据
#include "pythontableoutputspec.h"
#include "pythonnumerictable.h"
#include "pythonnumericoutput.h"
#include <QJsonArray>
namespace paleo::python {
AlgorithmSpec tableOutputsAlgorithmSpec() {
  auto spec=numericTableAlgorithmSpec();spec.id="python:numeric_table_outputs";
  spec.displayName="Generated report and typed numeric table";spec.implementationVersion="python-table-outputs.v1";
  AlgoPort table;table.id="TABLE";table.kind="file";
  table.constraints={{"fileExtension","csv"},{"encoding","host-csv.numeric.v1"},{"maxBytes",NumericCsvBytes},
    {"contract","paleo.numeric-table-output.v1"},{"maxRows",8192},{"maxColumns",16}};
  spec.outputs.append(table);return spec;
}
CapabilitySpec tableOutputsCapabilitySpec() {
  auto spec=numericTableCapabilitySpec();spec.capabilityId=QLatin1String(TableOutputsCapability);
  spec.algorithmId=tableOutputsAlgorithmSpec().id;spec.aliases={QLatin1String(TableOutputsTool)};
  spec.exposureReason="Explicit frozen numeric output declaration; host CSV encoding and atomic output set";return spec;
}
}
