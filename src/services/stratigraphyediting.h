// 层：数据
#pragma once
#include <paleo_data/interpretation.h>

// The manual editing rules come from the supplied 20261029 example's
// StratigraphyWidget. No rendering or persistence is performed here.
namespace paleo::stratigraphy_edit {
struct Group { std::string cycleId; int number=0, direction=0; };
void move(paleo::data::InterpretationDocument&, long long well, const std::string& id, double md);
void properties(paleo::data::InterpretationDocument&, long long well, const std::string& id,
                double md, const std::string& name, bool locked);
std::string add(paleo::data::InterpretationDocument&, long long well, double md, const std::string& name);
void remove(paleo::data::InterpretationDocument&, long long well, const std::string& id);
std::string confirm(paleo::data::InterpretationDocument&, long long well, const std::string& id);
void cycle(paleo::data::InterpretationDocument&, long long well, const std::string& id,
           const std::string& name, int level, int direction);
void hierarchy(paleo::data::InterpretationDocument&, long long well, const std::string& segment,
               const std::vector<Group>& groups);
}
