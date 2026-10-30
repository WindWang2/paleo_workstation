// 层：功能
#pragma once
#include <paleo_stratigraphy/core.h>
#include <paleo_data/service.h>
#include <QJsonObject>
namespace paleo::stratigraphy_native {
paleo::stratigraphy::GeometryOptions geometry(paleo::data::DataService& service,const std::string& view);
QJsonObject geometryJson(const paleo::stratigraphy::GeometryOptions& value);
}
