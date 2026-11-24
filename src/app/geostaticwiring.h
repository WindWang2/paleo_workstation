// 层：组装根
// app/geostaticwiring.h — I1 地统计原子能力的产品装配（aiwiring 单点调用）。
//
// 注册的适配器持有受控 provider（catalog/document 代次快照）与安全存活期：
// 换工程、清空工程或嵌套重绑时 alive() 失效，旧准备不再向新工程提交；
// 重绑后 aiwiring 再次调用本函数覆盖注册（registerExecutionAdapter 自带
// 该能力旧准备的失效语义）。
#pragma once

#include <QString>
#include <functional>

class DataCatalog;
namespace paleo::capability { class CapabilityHost; }
namespace paleo::document { class DocumentSession; }

// currentBinding：aiwiring 的绑定代次谓词（异步回调里复核装配仍当前）。
void bindGeostatAtomCapabilities( paleo::capability::CapabilityHost *host, DataCatalog *catalog,
                                  paleo::document::DocumentSession *document,
                                  const std::function<bool()> &currentBinding );
