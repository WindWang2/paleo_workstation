// 层：功能
#include "domaintools.h"

void appendAgentOperationTools(QVector<AiToolSpec> &specs) {
  const auto text=[](const char *key,bool required) {return AiToolParamSpec{QLatin1String(key),"string",QString(),required,{}};};
  const auto integer=[](const char *key) {return AiToolParamSpec{QLatin1String(key),"integer",QString(),false,{}};};
  const auto response=QStringLiteral(R"({"type":"object","required":["code"],"properties":{"ok":{"type":"boolean"},"code":{"type":"string"},"operationId":{"type":"string"},"execution":{"type":"string"},"commitState":{"type":"string"},"presentation":{"type":"string"},"revision":{"type":"string"}}})");
  AiToolSpec tile;tile.name="paleo.tile_classification_v2";tile.title=QObject::tr("地震相 tile 分类");
  tile.description=QObject::tr("版本 2：prepare 以 source_version_id 和 model 冻结源栅格、网格及模型；compute-only 只接受 prepare 的 source_operation_id，生成私有三端口结果；publish 只接受 computed 的 source_operation_id，一次登记三端口并呈现图层。后两阶段不能重新指定来源、模型或参数。");
  tile.parameters={text("client_request_id",true),text("publish_intent",true),text("source_version_id",false),
    text("source_operation_id",false),text("model",false),{"parameters","object",QString(),false,{}}};
  tile.parameters[1].enumValues={"prepare","compute-only","publish"};
  tile.effects=paleo::host::Read|paleo::host::PrivateStage;tile.resultSchemaJson=response;specs.append(tile);
  struct Entry {const char *name;QString title,description;};
  const QVector<Entry> entries{
    {"paleo.operation_get",QObject::tr("操作状态"),QObject::tr("读取同一聊天作用域内的持久操作状态；分别报告执行、数据提交与呈现事实。")},
    {"paleo.operation_events",QObject::tr("操作事件"),QObject::tr("按 after_revision 十进制字符串读取有界事件页；历史过期明确报错，不回退第一页。")},
    {"paleo.operation_resume",QObject::tr("恢复操作"),QObject::tr("恢复原操作：已提交时修复凭证或准确呈现；未提交时按冻结计划恢复。不会创建第二个业务操作。")},
    {"paleo.operation_cancel",QObject::tr("取消操作"),QObject::tr("持久化取消请求；已提交时返回 cancel_after_commit，保留成果。")},
    {"paleo.operation_results",QObject::tr("操作结果"),QObject::tr("列出原操作的准确结果端口、字节数与指纹；结果引用不暴露路径。")},
    {"paleo.operation_read",QObject::tr("读取操作结果"),QObject::tr("按准确 operation_id、port_id 和十进制 offset 读取有界二进制页，以 base64 返回。")}
  };
  for(const auto &entry:entries) {
    AiToolSpec spec;spec.name=QLatin1String(entry.name);spec.title=entry.title;spec.description=entry.description;
    spec.resultSchemaJson=response;
    spec.parameters={text("operation_id",true)};
    if(spec.name.endsWith("_events"))spec.parameters+={text("after_revision",false),integer("page_size"),integer("max_bytes")};
    if(spec.name.endsWith("_read"))spec.parameters+={text("port_id",true),text("offset",false),integer("max_bytes")};
    spec.effects=paleo::host::Read;
    if(spec.name.endsWith("_cancel"))spec.effects|=paleo::host::PrivateStage;
    specs.append(spec);
  }
}
