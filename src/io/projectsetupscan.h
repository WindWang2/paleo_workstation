// 层：数据
#pragma once
#include "../domain/projectsetup.h"
#include <functional>
ProjectSetupSources discoverProjectSources(const QString &root);
ProjectSetupPreview scanProjectSources(const ProjectSetupSources &sources,
                                      const std::function<bool()> &cancelled = {});
