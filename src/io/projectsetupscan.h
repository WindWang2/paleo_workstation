// 层：数据
#pragma once
#include "../domain/projectsetup.h"
#include <functional>
ProjectSetupSources discoverProjectSources(const QString &root);
ProjectSetupPreview scanProjectSources(const ProjectSetupSources &sources,
                                      const std::function<bool()> &cancelled = {});
void scanProjectMaps(const ProjectSetupSources &sources, ProjectSetupPreview *preview,
                     const std::function<bool()> &cancelled);
bool validateProjectSourceSnapshots(const ProjectSetupPreview &preview, QString *error);
