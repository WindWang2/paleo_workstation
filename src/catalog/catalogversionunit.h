// 层：数据
#pragma once
#include "../domain/stratigraphicunit.h"

class DataCatalog;
struct CatalogVersion;

// Resolve a version's explicit unit, or the common unit of its immutable parents.
// An empty result with inconsistent set distinguishes conflicting or malformed
// evidence from a legacy version with no registered unit.
paleo::strat::UnitSelection analysisResultUnitSelection(const DataCatalog *catalog,
    const CatalogVersion &version, bool *inconsistent = nullptr);
