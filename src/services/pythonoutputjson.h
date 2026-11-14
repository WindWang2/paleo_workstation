// 层：数据
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
namespace paleo::python {
// Reject invalid UTF-8, duplicate (including escaped) keys, excessive depth/nodes.
QString parsePythonOutputObject(const QByteArray &bytes,qint64 maxBytes,int maxNodes,QJsonObject &object);
}
