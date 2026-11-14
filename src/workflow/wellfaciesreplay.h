// 层：功能
#pragma once
#include <QJsonObject>
namespace paleo::capability { class CapabilityHost; }
namespace paleo::wellfacies {
// Empty result/code means further execution is required. Only Read is needed for a complete durable replay.
QJsonObject completedReplay(capability::CapabilityHost *host,const QString &chatSession,
                           const QJsonObject &args,QString &code);
}
