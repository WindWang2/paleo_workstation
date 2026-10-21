// 层：数据
#pragma once
#include <QByteArray>
#include <QString>
struct BinnedHorizon;
struct HorizonHeader;
bool nativeHorizonGrid(const QByteArray &text, const HorizonHeader &header, BinnedHorizon *grid, QString *error);
