// 层：视图
#pragma once

// ui/pages/ — per-page right-dock panels (§42.2 inventory)。
// W5b 拆分后本头只做聚合，保持旧 include 路径可用：
//   DataPage        → datapage.h（薄壳；列表侧 DataListPanel + 实体侧 EntityPanel）
//   PredictPage     → predictpage.h
//   ConstraintPage  → constraintpage.h
//   ComposePage     → composepage.h
//   ValidatePage    → validatepage.h
// 每个面板仍是纯 QWidget + ctor 注入；UI 不直触 Qgs*（§25）——发意图信号，
// 壳/工作流做 GIS 活。

#include "datapage.h"
#include "predictpage.h"
#include "constraintpage.h"
#include "composepage.h"
#include "validatepage.h"
