// 层：数据
#include "pythongeneratedoutputspec.h"
#include "pythonnumerictable.h"
#include "pythonseismictrace.h"
#include "pythonnumericoutput_internal.h"
#include <QJsonArray>
#include <QRegularExpression>
namespace paleo::python {
namespace {
QJsonObject inputChoice(const QJsonObject &schema,const char *kind) {
  auto input=schema.value("properties").toObject().value("inputs").toObject().value("oneOf").toArray()[0].toObject();
  auto properties=input.value("properties").toObject();properties["input_kind"]=QJsonObject{{"const",kind}};input["properties"]=properties;
  auto required=input.value("required").toArray();required.append("input_kind");input["required"]=required;return input;
}
QJsonObject imageSchema() {
  return {{"type","object"},{"additionalProperties",false},{"required",QJsonArray{"contract","width","height","format"}},
    {"properties",QJsonObject{{"contract",QJsonObject{{"const","paleo.python-image-declaration.v1"}}},
      {"format",QJsonObject{{"const","rgba8"}}},{"width",QJsonObject{{"type","integer"},{"minimum",1},{"maximum",1024}}},
      {"height",QJsonObject{{"type","integer"},{"minimum",1},{"maximum",1024}}}}}};
}
bool encoderIdentity(const QJsonObject &encoder) {
  auto fixed=encoder;const auto compile=fixed.take("qtCompileVersion"),runtime=fixed.take("qtRuntimeVersion");
  const auto compression=fixed.take("compression");
  static const QRegularExpression version("[0-9]{1,2}\\.[0-9]{1,2}\\.[0-9]{1,3}([A-Za-z0-9_.+-]{0,32})?");
  const auto valid=[&](const QJsonValue &value){return value.isString()&&!value.toString().isEmpty()&&
    version.match(value.toString()).capturedLength()==value.toString().size();};
  // Retain the original reviewed setting for authenticated stored output only.
  // Compute additionally requires the exact current identity below.
  return valid(compile)&&valid(runtime)&&(compression==9||compression==100)&&
    fixed==QJsonObject{{"contract","paleo.png-encoder.v1"},{"implementation","qt.imagewriter.png"},
    {"pixelFormat","RGBA8888"},{"optimizedWrite",false},{"progressiveScanWrite",false}};
}
}
QJsonObject generatedOutputsProfile(const QJsonObject &table,const QJsonObject &image) {
  return {{"contract","paleo.python-generated-output-profile.v1"},{"protocol","paleo.python-output-frame.v3"},
    {"table",table},{"image",image},{"pngEncoder",pngEncoderIdentity()},{"frameBytes",NumericOutputFrameBytes},
    {"csvBytes",NumericCsvBytes},{"pngBytes",ImageOutputPngBytes}};
}
QString validateGeneratedOutputsProfile(const QJsonObject &profile,const ExecutionPolicy &policy,bool currentEncoder) {
  if(!numeric_output::fields(profile,{"contract","protocol","table","image","pngEncoder","frameBytes","csvBytes","pngBytes"})||
      profile.value("contract")!="paleo.python-generated-output-profile.v1"||profile.value("protocol")!="paleo.python-output-frame.v3"||
      profile.value("frameBytes")!=NumericOutputFrameBytes||profile.value("csvBytes")!=NumericCsvBytes||profile.value("pngBytes")!=ImageOutputPngBytes||
      !profile.value("table").isObject()||!profile.value("image").isObject()||!profile.value("pngEncoder").isObject())return "invalid_python_outputs_profile";
  auto code=validateNumericOutputDeclaration(profile.value("table").toObject());if(!code.isEmpty())return code;
  code=validateImageOutputDeclaration(profile.value("image").toObject());if(!code.isEmpty())return code;
  const auto encoder=profile.value("pngEncoder").toObject();if(!encoderIdentity(encoder))return "invalid_python_outputs_profile";
  if(currentEncoder&&encoder!=pngEncoderIdentity())return "python_output_encoder_changed";
  if(!policy.validate().isEmpty())return "python_policy_invalid";
  const auto image=profile.value("image").toObject();const qint64 pixels=qint64(image.value("width").toInt())*image.value("height").toInt()*4;
  const auto maximum=qMin(NumericOutputFrameBytes,policy.outputBytes);if(pixels+14>=maximum)return "python_output_budget_exceeded";
  const auto reservation=imageOutputHostReservation(maximum,profile.value("table").toObject(),image);
  return reservation<0||reservation>policy.memoryBytes?QStringLiteral("python_output_memory_budget"):QString();
}
QJsonObject generatedOutputsToolRequestSchema() {
  auto schema=tableOutputsToolRequestSchema();auto properties=schema.value("properties").toObject();
  const QJsonObject inputs{{"oneOf",QJsonArray{inputChoice(numericTableToolRequestSchema(),"numeric_table"),
    inputChoice(seismicTraceToolRequestSchema(),"seismic_trace")}}};
  const auto publish=properties.value("inputs").toObject().value("oneOf").toArray().at(1);
  properties["inputs"]=QJsonObject{{"oneOf",QJsonArray{inputs,publish}}};
  auto parameters=properties.value("parameters").toObject();auto choices=parameters.value("oneOf").toArray();auto source=choices[0].toObject();
  auto fields=source.value("properties").toObject();fields["output_image"]=imageSchema();auto code=fields.value("code").toObject();
  code["description"]="UTF-8 Python <=16KiB. Input stdin uses the explicit numeric-table or seismic-trace metadata contract and uint32LE metadata length, then binary. "
    "Output exactly PALEOPYO3\\n (ten bytes), uint32LE header length <=16KiB, frame.v3 JSON header, then REPORT, TABLE, raw IMAGE bytes. "
    "Header contract paleo.python-output-frame.v3; ports ordered REPORT(utf8-json,size,sha256), TABLE(paleo.numeric-columns.v1,size,sha256,rows,columns), "
    "IMAGE(rgba8,size,sha256,width,height); every entry also has id. TABLE columns equal output_table exactly; values then LSB packed null mask per column. "
    "REPORT body is paleo.python-generated-report.v1. IMAGE is straight RGBA8 exactly width*height*4; no child PNG, paths, MIME or spatial claims. "
    "Host encodes CSV/PNG and validates/publishes the complete three-port set; numerical values and pixels are not independently recomputed. "
    "No file writes, network, subprocesses or package installation; no source path/SQL input.";
  fields["code"]=code;source["properties"]=fields;auto required=source.value("required").toArray();required.append("output_image");source["required"]=required;
  choices[0]=source;parameters["oneOf"]=choices;properties["parameters"]=parameters;schema["properties"]=properties;
  auto conditions=schema.value("allOf").toArray();auto phase=conditions[0].toObject();auto compute=phase.value("else").toObject();
  auto selected=compute.value("properties").toObject();selected["inputs"]=inputs;selected["parameters"]=source;compute["properties"]=selected;
  phase["else"]=compute;conditions[0]=phase;schema["allOf"]=conditions;return schema;
}
QString validateGeneratedOutputsToolArguments(const QJsonObject &arguments) {
  if(arguments.value("publish_intent")=="publish")return validateNumericTableToolArguments(arguments);
  if(!arguments.value("inputs").isObject()||!arguments.value("parameters").isObject())return "invalid_python_outputs_request";
  auto original=arguments,parameters=arguments.value("parameters").toObject(),inputs=arguments.value("inputs").toObject();
  if(!parameters.value("output_image").isObject()||!parameters.value("output_table").isObject())return "invalid_python_outputs_profile";
  auto code=validateNumericOutputDeclaration(parameters.take("output_table").toObject());if(!code.isEmpty())return code;
  code=validateImageOutputDeclaration(parameters.take("output_image").toObject());if(!code.isEmpty())return code;
  const auto kind=inputs.take("input_kind");original["inputs"]=inputs;original["parameters"]=parameters;
  if(kind=="numeric_table")return validateNumericTableToolArguments(original);
  if(kind=="seismic_trace")return validateSeismicTraceToolArguments(original);
  return "python_output_input_kind_invalid";
}
}
