// 层：数据
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace paleo::pi {
inline constexpr char Protocol[]="paleo.pi-agent.v1";
inline constexpr qsizetype MaxFrameBytes=64*1024,MaxDiagnosticBytes=64*1024;
// Canonical compact JSON, uint32 little-endian length, no extra channel framing.
QByteArray encodeFrame(const QJsonObject&);
// Consumes at most one frame. Incomplete input is not an error; failures never yield an object.
bool takeFrame(QByteArray &buffer,QJsonObject &result,bool &complete,QString &error);
bool validCounter(const QString&); // Canonical unsigned 64-bit decimal string; never a JS Number.
bool validSession(const QString&);
}
