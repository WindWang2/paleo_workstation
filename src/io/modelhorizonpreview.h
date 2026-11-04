// 层：数据
#pragma once
#include <QString>
#include <functional>
namespace paleo::area_model { enum class Read; }
struct BinnedHorizon;

// Read stored model points, without reparsing or requiring the external file.
paleo::area_model::Read readModelHorizonPreview(const QString &source, BinnedHorizon &grid,
    QString *error, const std::function<bool()> &cancelled = {});
