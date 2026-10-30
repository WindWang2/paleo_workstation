// 层：功能
#pragma once
#include "stratigraphyworkflow.h"
#include "../services/paleotaskservice.h"
#include <paleo_stratigraphy/quick_workflow.h>
#include <QPointer>

struct StratigraphyWorkflow::State {
 PaleoTaskService* tasks=nullptr;QPointer<PaleoTask> task;
 QString path,project;quint64 generation=0;bool readOnly=false,dirty=false;
 QString documentError; // #828：打开工程读取解释失败的原因（空 = 无失败）
 QList<StratigraphyWell> wells;paleo::data::InterpretationDocument document;
 paleo::data::CorrelationInputRequest preferred;
 struct Geometry {
  struct Well {
   long long id;
   std::vector<paleo::data::InterpretationBoundary> boundaries;
   std::vector<paleo::data::InterpretationCycle> cycles;
   std::vector<std::string> warnings;
   std::vector<std::pair<std::string,std::string>> seriesStates;
  };
  std::vector<Well> wells;
 };
 std::vector<Geometry> history,future;
 Geometry geometry() const;
 void restore(const Geometry& state);
 void remember(){history.push_back(geometry());if(history.size()>100)history.erase(history.begin());future.clear();}
};
