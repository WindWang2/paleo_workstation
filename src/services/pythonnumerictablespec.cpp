// 层：数据
#include "pythonnumerictablespec.h"
#include "pythonnumerictable.h"
#include "pythongeneratedspec.h"
namespace paleo::python {
AlgorithmSpec numericTableAlgorithmSpec() {
  auto spec=generatedAlgorithmSpec();spec.id="python:numeric_table_compute";
  spec.displayName="Generated numeric table window report";spec.implementationVersion="python-numeric-table-report.v1";
  spec.inputs.clear();AlgoPort table;table.id="TABLE";table.kind="table";table.hidden=true;table.cardinalityMin=1;table.cardinalityMax=1;
  table.axis="source_row";table.constraints={{"contract",QLatin1String(NumericTableInputContract)},
    {"sourceKind","source_table_numeric_projection"},{"maxValueColumns",data::NumericTableMaxColumns},
    {"maxRows",data::NumericTableMaxRows},{"axisDtype","<i8"},{"unitConvention","explicit_projection_declaration"}};
  spec.inputs.append(table);return spec;
}
CapabilitySpec numericTableCapabilitySpec() {
  auto spec=generatedCapabilitySpec();spec.capabilityId=QLatin1String(NumericTableCapability);spec.algorithmId=numericTableAlgorithmSpec().id;
  spec.aliases={QLatin1String(NumericTableTool)};
  spec.exposureReason="Explicit SDK numeric projection and actual read cut; generated-code permission and proven isolation required";
  return spec;
}
}
