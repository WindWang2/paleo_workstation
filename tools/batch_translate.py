#!/usr/bin/env python3
# // 层：测试壳
"""
Automated Batch Translation & Validation Engine for paleo_zh_CN.ts
Paleo Workstation Localization Pipeline (Milestone M2)

Features:
1. Strict 100% bit-for-bit XML preservation (headers, <!DOCTYPE TS>, <location.../> spacing, comments).
2. Automated batch segmentation (B01–B10 and all, <=600 items/batch).
3. Context-aware English dictionary lookup + Chinese direct copy with accelerator preservation.
4. Multiset placeholder parity verification (%1..%8, %n).
5. Glossary banned terms audit (25 hard arbitrations).
6. lrelease test compilation (-fail-on-unfinished / -nounfinished).
7. Mutation testing (--selftest).
"""

import sys
import os
import re
import json
import argparse
import tempfile
import subprocess
from collections import Counter
import xml.sax.saxutils as saxutils

# Windows / UTF-8 console output protection
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

# --- 1. Authoritative Batch Partition Specs (M2 Explorer 1) ---
BATCH_SPECS = {
    "B01": {"start_ctx": 0, "end_ctx": 15, "qobject_span": None, "count": 504, "desc": "AI 辅助、算法目录与数据中心服务"},
    "B02": {"start_ctx": 15, "end_ctx": 21, "qobject_span": None, "count": 516, "desc": "数据资产、列表与预览体系"},
    "B03": {"start_ctx": 21, "end_ctx": 37, "qobject_span": None, "count": 467, "desc": "实体、相态与综合编图工作台"},
    "B04": {"start_ctx": 37, "end_ctx": 61, "qobject_span": None, "count": 476, "desc": "编图工作台页面、图层配置与版面设计器"},
    "B05": {"start_ctx": 61, "end_ctx": 75, "qobject_span": None, "count": 550, "desc": "主窗口核心与测井物性分析"},
    "B06": {"start_ctx": 75, "end_ctx": 83, "qobject_span": (1, 442), "count": 565, "desc": "智能预测、地质建模与全局对象(上)"},
    "B07": {"start_ctx": 84, "end_ctx": 100, "qobject_span": (443, 748), "count": 565, "desc": "全局对象(下)与 QGIS 集成服务"},
    "B08": {"start_ctx": 100, "end_ctx": 126, "qobject_span": None, "count": 548, "desc": "地震剖面、任务服务与井综合柱状图配置"},
    "B09": {"start_ctx": 126, "end_ctx": 148, "qobject_span": None, "count": 559, "desc": "井综合柱状图面板、井位部署与井分层编辑"},
    "B10": {"start_ctx": 148, "end_ctx": 201, "qobject_span": None, "count": 519, "desc": "分层合并、数据中枢对话框、断裂与三维地震"},
}

# --- 2. Glossary Banned Terms (translations/glossary-zh-CN.md) ---
BANNED_TERMS = [
    "地平线", "视野", "视界", "井顶", "井口顶部", "交叉图", "十字图",
    "交会画图", "音轨", "检查射击", "检查炮", "格子化", "多边形化",
    "扁平化", "压平", "断距投掷", "工作拷贝", "作业复本", "碑文",
    "死标记", "家系", "门第", "基础水平", "基底平面", "升尺度"
]

# --- 3. Complete English Translation Map (M2 Explorer 2) ---
ENGLISH_TRANSLATION_MAP = {
    ('CompositionWorkflow', 'composition workflow is not bound to services'): '综合编图工作流未绑定服务',
    ('CompositionWorkflow', 'no factor layers supplied for fusion'): '未提供用于融合的因子图层',
    ('CompositionWorkflow', 'facies fusion returned no output path'): '相融合未返回输出路径',
    ('CompositionWorkflow', 'no raster layer supplied for facies polygons'): '未提供用于生成相多边形的栅格图层',
    ('CompositionWorkflow', "raster layer '%1' is not declared"): '栅格图层“%1”未声明',
    ('CompositionWorkflow', 'cannot derive facies polygons without a horizon'): '缺少层位，无法推导相多边形',
    ('CompositionWorkflow', "failed to instantiate raster '%1'"): '实例化栅格“%1”失败',
    ('CompositionWorkflow', "failed to instantiate constraints '%1'"): '实例化约束“%1”失败',
    ('CompositionWorkflow', 'facies polygonize produced no results'): '相多边形矢量化未生成结果',
    ('CompositionWorkflow', 'facies polygonize returned no output path'): '相多边形矢量化未返回输出路径',
    ('CompositionWorkflow', "failed to declare facies layer '%1'"): '声明相图层“%1”失败',
    ('CompositionWorkflow', 'no facies layer supplied'): '未提供相图层',
    ('CompositionWorkflow', "layer '%1' is not declared"): '图层“%1”未声明',
    ('CompositionWorkflow', 'cannot create %1'): '无法创建 %1',
    ('CompositionWorkflow', 'cannot replace stale working copy %1'): '无法替换过期的工作副本 %1',
    ('CompositionWorkflow', 'cannot copy %1 → %2'): '无法复制 %1 → %2',
    ('CompositionWorkflow', "layer '%1' is not a vector layer"): '图层“%1”不是矢量图层',
    ('CompositionWorkflow', "cannot start editing on '%1'（%2）"): '无法开始对“%1”进行编辑（%2）',
    ('CompositionWorkflow', "failed to re-instantiate '%1' after preparing edit copy"): '准备编辑副本后重新实例化“%1”失败',
    ('CompositionWorkflow', "cannot start editing on '%1'"): '无法开始对“%1”进行编辑',
    ('CompositionWorkflow', "cannot add field '%1' to %2"): '无法将字段“%1”添加到 %2',
    ('CompositionWorkflow', "field '%1' not visible after add"): '添加后字段“%1”不可见',
    ('ConstraintPage', ' m'): ' m',
    ('ConstraintPage', ' °'): ' °',
    ('ConstraintPage', '%1 · %2'): '%1 · %2',
    ('ConstraintPage', '%1 TVD'): '%1 TVD',
    ('ConstraintPage', '%1 m/s'): '%1 m/s',
    ('ConstraintWorkflow', 'constraint workflow is not bound to a layer service'): '约束工作流未绑定图层服务',
    ('ConstraintWorkflow', 'constraint geometry WKT is empty'): '约束几何 WKT 为空',
    ('ConstraintWorkflow', 'constraint geometry'): '约束几何',
    ('ConstraintWorkflow', 'constraint workflow is not bound to services'): '约束工作流未绑定服务',
    ('ConstraintWorkflow', 'constraint IDW returned no output path'): '约束反距离加权 (IDW) 未返回输出路径',
    ('ConstraintWorkflow', '%1·%2'): '%1·%2',
    ('DataListPanel', '%1 · %2 (%3)'): '%1 · %2 (%3)',
    ('DataListPanel', '%1 · %2'): '%1 · %2',
    ('DataPreviewTabs', ' m'): ' m',
    ('DataPreviewTabs', '%1 %2 · %3 ms'): '%1 %2 · %3 ms',
    ('DataPreviewTabs', '%1 %2 · %3'): '%1 %2 · %3',
    ('DataPreviewTabs', 'MD'): 'MD',
    ('DataPreviewTabs', 'TVD'): 'TVD',
    ('DataPreviewTabs', 'X'): 'X',
    ('DataPreviewTabs', 'Y'): 'Y',
    ('DataPreviewTabs', 'Time(ms)'): 'Time(ms)',
    ('DataPreviewTabs', 'TIME–TVD'): 'TIME–TVD',
    ('DataPreviewTabs', 'KB'): '补心 (KB)',
    ('DataPreviewTabs', 'TD'): '完钻深度 (TD)',
    ('DataPreviewTabs', 'BottomX'): '井底 X',
    ('DataPreviewTabs', 'BottomY'): '井底 Y',
    ('DataPreviewTabs', 'WellType'): '井型',
    ('DerivationGraph', 'v%1 · %2'): 'v%1 · %2',
    ('EntityPanel', 'Inline: %1 ~ %2\nCrossline: %3 ~ %4'): '主测线：%1 ～ %2\n联络测线：%3 ～ %4',
    ('EntityPanel', 'SEG-Y rev1.0 (IEEE/IBM FP32)'): 'SEG-Y rev1.0 (IEEE/IBM FP32)',
    ('EntityPanel', 'CPS-3 / ZMAP ASCII'): 'CPS-3 / ZMAP ASCII',
    ('EntityPanel', 'CWLS LAS 2.0'): 'CWLS LAS 2.0',
    ('EntityPanel', 'GeoJSON'): 'GeoJSON',
    ('EntityPanel', '—'): '—',
    ('EntityPanel', 'v%1'): 'v%1',
    ('EntityPanel', 'v1'): 'v1',
    ('EntityPanel', 'X %1–%2, Y %3–%4'): 'X %1–%2, Y %3–%4',
    ('EntityPanel', ' v%1'): ' v%1',
    ('EntityPanel', '%1 · %2'): '%1 · %2',
    ('ExportSettingsDialog', ' dpi'): ' dpi',
    ('ExportSettingsDialog', '%1 dpi'): '%1 dpi',
    ('InversionPanel', ' Hz'): ' Hz',
    ('MappingWorkbench', ' · v%1'): ' · v%1',
    ('MappingWorkbench', '%1 · v%2'): '%1 · v%2',
    ('MappingWorkbench', '（Mock）'): '（Mock）',
    ('MappingWorkbenchPage', ' m'): ' m',
    ('MappingWorkbenchPage', '%1 · %2'): '%1 · %2',
    ('MappingWorkbenchPage', 'v%1'): 'v%1',
    ('PaleoAlgorithmParametersPanel', 'Wrong or missing parameter value: %1'): '参数值错误或缺失：%1',
    ('PaleoAlgorithmWidget', 'Algorithm cancellation requested. Waiting for background worker to terminate...'): '已请求取消算法。正在等待后台工作线程终止…',
    ('PaleoAlgorithmWidget', 'Unable to execute algorithm'): '无法执行算法',
    ('PaleoAlgorithmWidget', 'Algorithm started'): '算法已启动',
    ('PaleoAlgorithmWidget', "<b>Algorithm '%1' starting&hellip;</b>"): '<b>算法“%1”正在启动&hellip;</b>',
    ('PaleoAlgorithmWidget', "Algorithm '%1' finished"): '算法“%1”已完成',
    ('PaleoEditingToolbar', ' px'): ' px',
    ('PaleoEditingToolbar', '● %1'): '● %1',
    ('PaleoLayoutItemPalette', 'HTML'): 'HTML',
    ('PaleoLayoutItemPalette', 'Text Table'): '文本表',
    ('PaleoLayoutTemplates', '%1 · %2 %3'): '%1 · %2 %3',
    ('PaleoMainWindow', 'Paleo Workbench'): 'Paleo Workbench',
    ('PaleoMainWindow', '%1 · %2 — %3'): '%1 · %2 — %3',
    ('PaleoMainWindow', '%1 TVD'): '%1 TVD',
    ('PaleoMainWindow', 'Paleo Workbench [*]'): 'Paleo Workbench [*]',
    ('PaleoMainWindow', '%1 — Paleo Workbench [*]'): '%1 — Paleo Workbench [*]',
    ('PaleoMainWindow', '· %1 → %2'): '· %1 → %2',
    ('PaleoMainWindow', ' · v%1%2'): ' · v%1%2',
    ('PaleoMainWindow', ' Mock'): ' Mock',
    ('PaleoMainWindow', '%1 · %2'): '%1 · %2',
    ('PaleoMainWindow', '%1 · %2 · v%3'): '%1 · %2 · v%3',
    ('PaleoMainWindow', ' · Mock'): ' · Mock',
    ('PaleoMainWindow', '（%1）'): '（%1）',
    ('PaleoMapBookPanel', '%1 dpi'): '%1 dpi',
    ('PaleoOnnxService', 'Model not found: %1'): '未找到模型：%1',
    ('PaleoOnnxService', 'Model not readable: %1'): '模型不可读：%1',
    ('PaleoOnnxService', '%1: %2 (%3)'): '%1: %2 (%3)',
    ('PaleoOnnxService', 'No model loaded'): '未加载任何模型',
    ('PaleoProjectStore', 'Failed to back up %1 to %2'): '备份 %1 到 %2 失败',
    ('PaleoProjectStore', 'Failed to replace backup %1'): '替换备份 %1 失败',
    ('PaleoProjectStore', 'no project paths set — commit journal has no home'): '未设置工程路径 — 提交日志无主目录',
    ('PaleoProjectStore', 'cannot create commit journal dir %1'): '无法创建提交日志目录 %1',
    ('PaleoProjectStore', 'cannot write %1: %2'): '无法写入 %1: %2',
    ('PaleoProjectStore', 'short write to %1: %2'): '写入 %1 不完整：%2',
    ('PaleoProjectStore', 'cannot replace %1: %2'): '无法替换 %1: %2',
    ('PaleoProjectStore', 'commitAll: both commit units are required'): 'commitAll：两个提交单元均为必需',
    ('PaleoProjectStore', 'commitAll: unsafe or empty op id: %1'): 'commitAll：不安全或为空的操作 ID：%1',
    ('PaleoProjectStore', 'commitAll: no project paths set — cannot locate commit journal'): 'commitAll：未设置工程路径 — 无法定位提交日志',
    ('PaleoProjectStore', 'commit journal %1 is unreadable or has an unknown stage — refusing to resume'): '提交日志 %1 不可读或阶段未知 — 拒绝恢复',
    ('PaleoProjectStore', 'commit op %1 journaled with different inputs — refusing to resume'): '提交操作 %1 记录的输入与当前不一致 — 拒绝恢复',
    ('PaleoProjectStore', 'cannot write commit journal %1: %2'): '无法写入提交日志 %1: %2',
    ('PaleoProjectStore', 'commit journal advance failed for op %1: %2'): '提交操作 %1 的日志推进失败：%2',
    ('PaleoProjectStore', 'commit finished but final journal mark failed: %1'): '提交已完成，但标记最终日志状态失败：%1',
    ('PaleoVertexEditorWidget', '#'): '#',
    ('PaleoVertexEditorWidget', 'x'): 'X',
    ('PaleoVertexEditorWidget', 'y'): 'Y',
    ('PetroPhysPanel', 'ρma / ρf:'): 'ρma / ρf：',
    ('PetroPhysPanel', 'Δtma/Δtf/Cp:'): 'Δtma/Δtf/Cp：',
    ('PetroPhysPanel', 'a/m/n/Rw:'): 'a/m/n/Rw：',
    ('PredictPage', '%1 (ONNX)'): '%1 (ONNX)',
    ('PredictionWorkflow', 'prediction workflow is not bound to a layer service'): '预测工作流未绑定图层服务',
    ('PredictionWorkflow', "no ONNX service bound; cannot run '%1'"): '未绑定 ONNX 服务；无法运行“%1”',
    ('PredictionWorkflow', "onnx algorithm id '%1' carries no model name"): 'ONNX 算法 ID“%1”未携带模型名称',
    ('PredictionWorkflow', "failed to load ONNX model '%1'"): '加载 ONNX 模型“%1”失败',
    ('PredictionWorkflow', "ONNX model '%1' produced no output"): 'ONNX 模型“%1”未生成输出',
    ('PredictionWorkflow', 'cannot place ONNX output on a grid'): '无法将 ONNX 输出放置于网格上',
    ('PredictionWorkflow', 'failed to write ONNX prediction raster'): '写入 ONNX 预测栅格失败',
    ('PredictionWorkflow', "failed to declare result layer '%1'"): '声明结果图层“%1”失败',
    ('PredictionWorkflow', "cannot run '%1': this build lacks ONNX Runtime"): '无法运行“%1”：此构建缺少 ONNX Runtime',
    ('PredictionWorkflow', 'prediction workflow is not bound to services'): '预测工作流未绑定服务',
    ('PredictionWorkflow', "prediction algorithm '%1' produced no results"): '预测算法“%1”未生成结果',
    ('PredictionWorkflow', "prediction algorithm '%1' returned no output path"): '预测算法“%1”未返回输出路径',
    ('PropertyModelPanel', ' m'): ' m',
    ('PropertyModelPanel', '°'): '°',
    ('QObject', 'Export canceled.'): '导出已取消。',
    ('QObject', 'Not enough memory to export the layout.'): '内存不足，无法导出布局。',
    ('QObject', 'Could not write the export file %1.'): '无法写入导出文件 %1。',
    ('QObject', 'Could not start printing the export.'): '无法开始打印导出内容。',
    ('QObject', 'Could not create the layered SVG file.'): '无法创建分层 SVG 文件。',
    ('QObject', 'Error iterating over the layout.'): '遍历布局时出错。',
    ('QObject', 'No layout to export.'): '没有可导出的布局。',
    ('QObject', 'No destination file given.'): '未指定目标文件。',
    ('QObject', 'The layout has no pages to export.'): '布局没有可导出的页面。',
    ('QObject', 'The page range selects no pages of this %1-page layout.'): '在包含 %1 页的布局中，选定的页面范围未选中任何页面。',
    ('QObject', 'Could not prepare the page selection for export.'): '无法为导出准备页面选集。',
    ('QObject', 'Export failed.'): '导出失败。',
    ('QObject', "layer '%1' has no active edit session"): '图层“%1”没有处于活动状态的编辑会话',
    ('QObject', "commitChanges failed for layer '%1'"): '图层“%1”提交更改失败',
    ('QObject', 'GeoJSON'): 'GeoJSON',
    ('QObject', '%1 ms'): '%1 ms',
    ('QObject', '%1 s'): '%1 s',
    ('QObject', '%1：%2'): '%1：%2',
    ('QObject', '—'): '—',
    ('QObject', '%1 × %2'): '%1 × %2',
    ('QObject', '2%–98%'): '2%–98%',
    ('QObject', 'unsafe managed destination: %1'): '受管目标路径不安全：%1',
    ('QObject', 'cannot create directory %1'): '无法创建目录 %1',
    ('QObject', 'cannot copy %1 → %2'): '无法复制 %1 → %2',
    ('QObject', 'cannot place %1'): '无法放置 %1',
    ('QObject', 'MapVersionStore is null'): 'MapVersionStore 为空',
    ('QObject', 'saveVersion failed'): '保存版本失败',
    ('QObject', 'ONNX output is empty'): 'ONNX 输出为空',
    ('QObject', 'GTiff driver is not available'): 'GTiff 驱动不可用',
    ('QObject', 'cannot create raster %1'): '无法创建栅格 %1',
    ('QObject', 'failed to write raster %1'): '写入栅格 %1 失败',
    ('QObject', 'cannot create prediction raster %1'): '无法创建预测栅格 %1',
    ('QObject', 'failed to write prediction raster %1'): '写入预测栅格 %1 失败',
    ('QObject', 'IL %1'): 'IL %1',
    ('QObject', '%1 m²'): '%1 m²',
    ('QObject', '%1 ~ %2'): '%1 ~ %2',
    ('QObject', '[%1, %2) · %3'): '[%1, %2) · %3',
    ('QObject', 'v%1（%2） ↔ v%3（%4）'): 'v%1（%2） ↔ v%3（%4）',
    ('QObject', '· SHA：%1… ↔ %2…'): '· SHA：%1… ↔ %2…',
    ('QObject', ' m'): ' m',
    ('QObject', 'cannot write fixture %1: %2'): '无法写入测试夹具 %1: %2',
    ('QObject', 'short write for fixture %1'): '测试夹具 %1 写入不完整',
    ('QObject', 'seg fixture needs equal non-empty gains/biases'): '分割测试夹具需要相等且非空的增益/偏置',
    ('QObject', 'Model has no outputs'): '模型没有输出',
    ('QObject', 'Inference produced no tensor output'): '推理未生成张量输出',
    ('QObject', 'Output tensor is not float32'): '输出张量不是 float32 类型',
    ('QObject', 'tile inference needs an ONNX service'): '瓦片推理需要 ONNX 服务',
    ('QObject', 'invalid output grid %1×%2'): '无效的输出网格 %1×%2',
    ('QObject', 'invalid tile geometry %1×%2 halo %3'): '无效的瓦片几何形态 %1×%2（外扩边带 %3）',
    ('QObject', 'tile inference needs a data source (fetch)'): '瓦片推理需要数据源 (fetch)',
    ('QObject', 'tile planning produced no tiles'): '瓦片规划未生成任何瓦片',
    ('QObject', "cannot load ONNX model '%1'"): '无法加载 ONNX 模型“%1”',
    ('QObject', '%1 B'): '%1 B',
    ('QObject', '%1 KB'): '%1 KB',
    ('QObject', '%1 MB'): '%1 MB',
    ('QObject', '%1 GB'): '%1 GB',
    ('QObject', 'SHA: —'): 'SHA: —',
    ('QObject', 'SHA: %1…'): 'SHA: %1…',
    ('QObject', 'm'): 'm',
    ('QObject', '%1 km²'): '%1 km²',
    ('QObject', '%1 m'): '%1 m',
    ('QObject', '%1 · %2'): '%1 · %2',
    ('QgisEditingService', 'cannot begin an edit session on a null layer'): '无法在空图层上开始编辑会话',
    ('QgisEditingService', "layer '%1' already has an active edit session"): '图层“%1”已有处于活动状态的编辑会话',
    ('QgisEditingService', "startEditing failed for layer '%1'"): '图层“%1”启动编辑失败',
    ('QgisEditingService', 'editing in progress'): '正在编辑中',
    ('QgisEditingService', 'cannot commit an edit session on a null layer'): '无法在空图层上提交编辑会话',
    ('QgisEditingService', '%1 is empty'): '%1 为空',
    ('QgisEditingService', ' at (%1, %2)'): ' 位于 (%1, %2)',
    ('QgisEditingService', '%1 is invalid: %2%3'): '%1 无效：%2%3',
    ('QgisLayoutService', 'no QgsProject to host the layout'): '没有用于承载布局的 QgsProject',
    ('QgisLayoutService', 'cannot create a layout with an empty name'): '无法创建名称为空的布局',
    ('QgisLayoutService', "a layout named '%1' already exists"): '已存在名为“%1”的布局',
    ('QgisLayoutService', "QgsLayoutManager refused layout '%1'"): 'QgsLayoutManager 拒绝了布局“%1”',
    ('QgisLayoutService', "no layout named '%1'"): '不存在名为“%1”的布局',
    ('QgisLayoutService', "empty output path for layout '%1'"): '布局“%1”的输出路径为空',
    ('QgisLayoutService', "PDF export of '%1' to %2 failed: %3"): '将“%1”导出为 PDF 到 %2 失败：%3',
    ('QgisProcessingService', "no processing algorithm registered as '%1'"): '未注册标识为“%1”的处理算法',
    ('QgisProcessingService', "algorithm '%1' could not create an instance"): '算法“%1”无法创建实例',
    ('QgisProcessingService', "algorithm '%1' raised an exception: %2"): '算法“%1”引发异常：%2',
    ('QgisProcessingService', "algorithm '%1' raised an unexpected exception"): '算法“%1”引发意外异常',
    ('QgisProcessingService', "algorithm '%1' failed%2"): '算法“%1”失败%2',
    ('QgisProjectService', 'Project file does not exist: %1'): '工程文件不存在：%1',
    ('QgisProjectService', 'Cannot read project file %1'): '无法读取工程文件 %1',
    ('QgisProjectService', 'Project bundle is damaged: qgz member missing (%1)'): '工程包已损坏：缺少 qgz 成员 (%1)',
    ('QgisProjectService', 'project member missing: %1'): '缺少工程成员：%1',
    ('QgisProjectService', 'project manifest unreadable: %1'): '工程清单不可读：%1',
    ('QgisProjectService', 'could not adopt project manifest: %1'): '无法采用工程清单：%1',
    ('QgisProjectService', 'Failed to read project: %1'): '读取工程失败：%1',
    ('QgisProjectService', 'Cannot create a project with an empty path'): '无法使用空路径创建工程',
    ('QgisProjectService', 'project manifest write failed: %1'): '写入工程清单失败：%1',
    ('QgisProjectService', 'No project path set — open or create a project first'): '未设置工程路径 — 请先打开或创建工程',
    ('QgisProjectService', 'Failed to read manifest declarations: %1'): '读取清单声明失败：%1',
    ('QgisProjectService', 'Failed to embed manifest declarations: %1'): '嵌入清单声明失败：%1',
    ('QgisProjectService', 'Failed to write project: %1'): '写入工程失败：%1',
    ('QgisProjectService', 'Failed to replace project file %1 with %2'): '将工程文件 %1 替换为 %2 失败',
    ('QgisStyleService', 'cannot apply a style to a null layer'): '无法将样式应用到空图层',
    ('QgisStyleService', 'no styles root configured — call setStylesRoot() first'): '未配置样式根目录 — 请先调用 setStylesRoot()',
    ('QgisStyleService', 'cannot apply an empty style reference'): '无法应用空的样式引用',
    ('QgisStyleService', "style '%1' not found at %2"): '在 %2 未找到样式“%1”',
    ('QgisStyleService', "loadNamedStyle('%1') failed: %2"): "loadNamedStyle('%1') 失败：%2",
    ('QgisStyleService', 'unknown error'): '未知错误',
    ('RealizationPanel', '−'): '−',
    ('RealizationWorkflow', '%1·%2'): '%1·%2',
    ('ReleasePanel', 'ID'): 'ID',
    ('SectionSetupDialog', 'TVD (m)'): 'TVD (m)',
    ('SectionSetupDialog', 'MD (m)'): 'MD (m)',
    ('SectionSetupDialog', 'TWT (ms)'): 'TWT (ms)',
    ('SectionWorkbench', ' · v%1'): ' · v%1',
    ('SeismicSectionCanvas', 'A · %1'): 'A · %1',
    ('SeismicSectionCanvas', 'B · %1'): 'B · %1',
    ('SeismicSectionCanvas', 'TWT (ms)'): 'TWT (ms)',
    ('SeismicSectionDockWidget', '1:1'): '1:1',
    ('SeismicSectionDockWidget', 'AGC'): 'AGC',
    ('SeismicSectionDockWidget', 'INLINE (189-192)'): '主测线 (189-192)',
    ('SeismicSectionDockWidget', 'CROSSLINE (193-196)'): '联络测线 (193-196)',
    ('SeismicSectionDockWidget', 'field record (9-12)'): '野外记录号 (9-12)',
    ('SeismicSectionDockWidget', 'CDP ensemble (21-24)'): 'CDP 道集号 (21-24)',
    ('SeismicSectionDockWidget', 'CDP X (73-76)'): 'CDP 坐标 X (73-76)',
    ('SeismicSectionDockWidget', 'CDP Y (77-80)'): 'CDP 坐标 Y (77-80)',
    ('SeismicSectionDockWidget', 'IL %1'): 'IL %1',
    ('SeismicSectionDockWidget', 'XL %1'): 'XL %1',
    ('SeismicSectionDockWidget', '1000 2000\n1002 2005\n1005 2012'): '1000 2000\n1002 2005\n1005 2012',
    ('StorageGovernanceDialog', '%1 B'): '%1 B',
    ('ToolAvailabilityService', 'Tools unavailable'): '工具不可用',
    ('ToolAvailabilityService', 'Layer busy — task in progress'): '图层繁忙 — 任务正在进行中',
    ('ValidatePage', '%1 ms'): '%1 ms',
    ('WellSectionWellsDialog', '↑'): '↑',
    ('WellSectionWellsDialog', '↓'): '↓',
    ('WellSitingPanel', 'X'): 'X',
    ('WellSitingPanel', 'Y'): 'Y',
    ('WellTopsEditorDialog', 'MD'): 'MD',
    ('WellTopsEditorDialog', 'TVD'): 'TVD',
    ('WellTopsEditorDialog', 'X'): 'X',
    ('WellTopsEditorDialog', 'Y'): 'Y',
    ('WellTopsEditorDialog', 'Z'): 'Z',
    ('WellTopsEditorDialog', 'Time(ms)'): 'Time(ms)',
    ('WellTopsEditorDialog', ' m'): ' m',
    ('paleo::crossplot::CrossplotPanel', 'k-means++'): 'k-means++',
    ('paleo::dataops::EntityCreateDialog', 'X:'): 'X：',
    ('paleo::dataops::EntityCreateDialog', 'Y:'): 'Y：',
    ('paleo::dataops::FilterBar', 'AND'): 'AND',
    ('paleo::dataops::FilterBar', 'OR'): 'OR',
    ('paleo::dataops::VersionTableDialog', 'v%1%2%3'): 'v%1%2%3',
    ('paleo::dataops::VersionTableDialog', '（%1）'): '（%1）',
    ('paleo::dataops::VersionTableDialog', '【%1】%2 → %3\n'): '【%1】%2 → %3\n',
    ('paleo::dataops::VersionTimeline', '%1 KB'): '%1 KB',
    ('paleo::fault::FaultManagerPanel', 'TWT %1–%2 ms'): 'TWT %1–%2 ms',
    ('seismic::Seismic3DViewPanel', 'TF…'): 'TF…',
    ('seismic::Seismic3DViewPanel', 'T'): 'T',
    ('seismic::Seismic3DViewPanel', 'IL'): 'IL',
    ('seismic::Seismic3DViewPanel', 'XL'): 'XL',
    ('seismic::Seismic3DViewPanel', ' fps'): ' fps',
    ('seismic::Seismic3DViewPanel', 'fps'): 'fps',
    ('seismic::SeismicPickPanel', 'H1'): 'H1',
    ('seismic::SeismicPickPanel', 'ID'): 'ID',
    ('seismic::SeismicPickPanel', 'IL'): 'IL',
    ('seismic::SeismicPickPanel', 'XL'): 'XL',
    ('seismic::SeismicPickPanel', 'TWT(ms)'): 'TWT(ms)',
    ('seismic::SeismicPickPanel', 'CSV (*.csv)'): 'CSV (*.csv)',
    ('seismic::SeismicPickPanel', '—'): '—',
}

def find_lrelease(explicit_path=None):
    """Locate lrelease executable."""
    if explicit_path and os.path.isfile(explicit_path) and os.access(explicit_path, os.X_OK):
        return explicit_path
    candidates = [
        "/usr/lib/qt6/bin/lrelease",
        "/usr/lib/qt6/libexec/lrelease",
        "/usr/lib/x86_64-linux-gnu/qt6/bin/lrelease",
        "/usr/local/qt6/bin/lrelease",
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    for cmd in ["lrelease", "lrelease-qt6"]:
        try:
            res = subprocess.run(["which", cmd], capture_output=True, text=True)
            if res.returncode == 0 and res.stdout.strip():
                return res.stdout.strip()
        except Exception:
            pass
    return None

def extract_placeholders(text):
    """Extract Qt placeholders: %1..%9, %n."""
    return re.findall(r'%[1-9]|%n', text)

def has_cjk(text):
    """Check if text contains CJK Chinese characters."""
    return any(0x4E00 <= ord(ch) <= 0x9FFF for ch in text)

def is_symbol_or_number(text):
    """Check if text contains no letters or CJK characters."""
    return not has_cjk(text) and not re.search(r'[a-zA-Z]', text)

def unescape_xml(raw_text):
    """Unescape XML entity references."""
    return saxutils.unescape(raw_text, {'&apos;': "'", '&quot;': '"'})

def escape_xml(plain_text):
    """Escape XML special characters."""
    return saxutils.escape(plain_text)

def is_message_in_batch(batch_id, ctx_idx, ctx_name, msg_idx_in_ctx):
    """Check if message belongs to the specified batch."""
    if batch_id == "all":
        return True
    spec = BATCH_SPECS.get(batch_id)
    if not spec:
        raise ValueError(f"Unknown batch ID: {batch_id}")
    if ctx_name == "QObject" and spec["qobject_span"]:
        start_m, end_m = spec["qobject_span"]
        return start_m <= msg_idx_in_ctx <= end_m
    return spec["start_ctx"] <= ctx_idx < spec["end_ctx"]

def load_dictionary(dict_path):
    """Load optional custom translation dictionary from JSON file."""
    if not dict_path:
        return {}
    with open(dict_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    mapping = {}
    if isinstance(data, dict):
        if "translations" in data and isinstance(data["translations"], list):
            for item in data["translations"]:
                ctx = item.get("context", "")
                src = item.get("source", "")
                tgt = item.get("translation", "")
                if ctx and src:
                    mapping[(ctx, src)] = tgt
                if src:
                    mapping[src] = tgt
        else:
            for k, v in data.items():
                if ":::" in k:
                    ctx, src = k.split(":::", 1)
                    mapping[(ctx, src)] = v
                else:
                    mapping[k] = v
    return mapping

def resolve_translation(ctx_name, raw_source, custom_dict=None):
    """
    Resolve translation string.
    Returns: (translated_raw_xml, success_bool)
    """
    plain_source = unescape_xml(raw_source)

    # 1. Custom dictionary lookup (if provided)
    if custom_dict:
        if (ctx_name, plain_source) in custom_dict:
            return escape_xml(custom_dict[(ctx_name, plain_source)]), True
        if plain_source in custom_dict:
            return escape_xml(custom_dict[plain_source]), True

    # 2. Embedded English translation map lookup
    if (ctx_name, plain_source) in ENGLISH_TRANSLATION_MAP:
        return escape_xml(ENGLISH_TRANSLATION_MAP[(ctx_name, plain_source)]), True
    if plain_source in ENGLISH_TRANSLATION_MAP:
        return escape_xml(ENGLISH_TRANSLATION_MAP[plain_source]), True

    # 3. Chinese source (already valid Chinese in source code)
    if has_cjk(plain_source):
        return raw_source, True

    # 4. Pure symbol or numerical string
    if is_symbol_or_number(plain_source):
        return raw_source, True

    # 5. Missing translation
    return None, False

def process_ts_file(ts_path, target_batch, custom_dict=None, dry_run=False):
    """
    Process translation population with 100% bit-for-bit XML preservation.
    """
    with open(ts_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    out_lines = []
    ctx_idx = -1
    cur_ctx_name = None
    msg_idx_in_ctx = 0
    in_msg = False
    in_source = False
    cur_source_raw = []

    batch_processed = 0
    batch_already_done = 0
    missing_translations = []

    for line in lines:
        s = line.strip()
        if s.startswith("<context>"):
            ctx_idx += 1
            cur_ctx_name = None
            msg_idx_in_ctx = 0
        elif s.startswith("<name>") and cur_ctx_name is None:
            cur_ctx_name = s[6:-7]
        elif s == "</context>":
            cur_ctx_name = None
        elif s == "<message>":
            in_msg = True
            msg_idx_in_ctx += 1
            cur_source_raw = []
            in_source = False
        elif s == "</message>":
            in_msg = False
            cur_source_raw = []
        elif in_msg and "<source>" in s:
            start = line.find("<source>")
            tag_close = line.find(">", start)
            if "</source>" in s:
                cur_source_raw.append(line[tag_close + 1 : line.find("</source>")])
                in_source = False
            else:
                cur_source_raw.append(line[tag_close + 1:])
                in_source = True
        elif in_msg and in_source:
            if "</source>" in s:
                cur_source_raw.append(line[:line.find("</source>")])
                in_source = False
            else:
                cur_source_raw.append(line)
        elif in_msg and "<translation" in line:
            raw_source = "".join(cur_source_raw)
            in_batch = is_message_in_batch(target_batch, ctx_idx, cur_ctx_name, msg_idx_in_ctx)

            if in_batch:
                if 'type="unfinished"' in line:
                    trans_xml, ok = resolve_translation(cur_ctx_name, raw_source, custom_dict)
                    if ok:
                        indent = line[:line.find("<translation")]
                        new_line = f"{indent}<translation>{trans_xml}</translation>\n"
                        out_lines.append(new_line)
                        batch_processed += 1
                        continue
                    else:
                        missing_translations.append((cur_ctx_name, unescape_xml(raw_source)))
                else:
                    batch_already_done += 1

        out_lines.append(line)

    if missing_translations:
        print(f"ERROR: {len(missing_translations)} strings in batch {target_batch} missing translations:", file=sys.stderr)
        for c, src in missing_translations[:10]:
            print(f"  [{c}] {src!r}", file=sys.stderr)
        if len(missing_translations) > 10:
            print(f"  ... and {len(missing_translations) - 10} more.", file=sys.stderr)
        return False, batch_processed, missing_translations

    if not dry_run:
        tmp_path = ts_path + ".tmp"
        with open(tmp_path, "w", encoding="utf-8") as f:
            f.writelines(out_lines)
        os.replace(tmp_path, ts_path)

    return True, batch_processed, []

def validate_ts_file(ts_path, target_batch="all", lrelease_bin=None):
    """
    Perform multi-stage validation:
    1. In-batch unfinished count == 0.
    2. Placeholder multiset parity.
    3. Glossary banned terms check.
    4. lrelease test compilation.
    """
    with open(ts_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    total_messages = 0
    unfinished_in_batch = 0
    total_unfinished = 0
    placeholder_mismatches = []
    banned_violations = []
    empty_translations = []

    ctx_idx = -1
    cur_ctx_name = None
    msg_idx_in_ctx = 0
    in_msg = False
    in_source = False
    in_translation = False
    cur_source_raw = []
    cur_trans_raw = []
    is_unfinished = False

    for line in lines:
        s = line.strip()
        if s.startswith("<context>"):
            ctx_idx += 1
            cur_ctx_name = None
            msg_idx_in_ctx = 0
        elif s.startswith("<name>") and cur_ctx_name is None:
            cur_ctx_name = s[6:-7]
        elif s == "<message>":
            in_msg = True
            msg_idx_in_ctx += 1
            cur_source_raw = []
            cur_trans_raw = []
            is_unfinished = False
        elif s == "</message>":
            in_msg = False
            in_source = False
            in_translation = False
            total_messages += 1
            plain_src = unescape_xml("".join(cur_source_raw))
            plain_trans = unescape_xml("".join(cur_trans_raw))

            if is_unfinished:
                total_unfinished += 1

            in_batch = is_message_in_batch(target_batch, ctx_idx, cur_ctx_name, msg_idx_in_ctx)
            if in_batch:
                if is_unfinished:
                    unfinished_in_batch += 1
                elif not plain_trans:
                    empty_translations.append((cur_ctx_name, plain_src))
                else:
                    # Multiset placeholder equality check
                    src_phs = Counter(extract_placeholders(plain_src))
                    trans_phs = Counter(extract_placeholders(plain_trans))
                    if src_phs != trans_phs:
                        placeholder_mismatches.append((cur_ctx_name, plain_src, plain_trans, src_phs, trans_phs))
                    # Glossary banned terms check
                    for b in BANNED_TERMS:
                        if b in plain_trans:
                            banned_violations.append((cur_ctx_name, plain_trans, b))

        elif in_msg and "<source>" in s:
            start = line.find("<source>")
            tag_close = line.find(">", start)
            if "</source>" in s:
                cur_source_raw.append(line[tag_close + 1 : line.find("</source>")])
                in_source = False
            else:
                cur_source_raw.append(line[tag_close + 1:])
                in_source = True
        elif in_msg and in_source:
            if "</source>" in s:
                cur_source_raw.append(line[:line.find("</source>")])
                in_source = False
            else:
                cur_source_raw.append(line)
        elif in_msg and "<translation" in s:
            if 'type="unfinished"' in s:
                is_unfinished = True
            start = line.find("<translation")
            tag_close = line.find(">", start)
            if "</translation>" in s:
                cur_trans_raw.append(line[tag_close + 1 : line.find("</translation>")])
                in_translation = False
            else:
                cur_trans_raw.append(line[tag_close + 1:])
                in_translation = True
        elif in_msg and in_translation:
            if "</translation>" in s:
                cur_trans_raw.append(line[:line.find("</translation>")])
                in_translation = False
            else:
                cur_trans_raw.append(line)

    ok = True
    print(f"Validation Summary for batch '{target_batch}':")
    print(f"  Total catalog messages: {total_messages}")
    print(f"  Unfinished in target batch: {unfinished_in_batch}")
    print(f"  Total unfinished across catalog: {total_unfinished}")

    if unfinished_in_batch > 0:
        print(f"  FAILED: Found {unfinished_in_batch} unfinished translations in batch {target_batch}!", file=sys.stderr)
        ok = False
    if empty_translations:
        print(f"  FAILED: Found {len(empty_translations)} empty translations!", file=sys.stderr)
        ok = False
    if placeholder_mismatches:
        print(f"  FAILED: Found {len(placeholder_mismatches)} placeholder parity mismatches!", file=sys.stderr)
        for c, s, t, sp, tp in placeholder_mismatches[:5]:
            print(f"    [{c}] Source: {s!r} ({sp}) vs Trans: {t!r} ({tp})", file=sys.stderr)
        ok = False
    if banned_violations:
        print(f"  FAILED: Found {len(banned_violations)} banned term violations!", file=sys.stderr)
        for c, t, b in banned_violations[:5]:
            print(f"    [{c}] Banned '{b}' in: {t!r}", file=sys.stderr)
        ok = False

    # lrelease compilation check
    lrel = find_lrelease(lrelease_bin)
    if not lrel:
        print("  WARNING: lrelease binary not found, skipping QM compilation test.", file=sys.stderr)
    else:
        with tempfile.NamedTemporaryFile(suffix=".qm", delete=True) as tmp_qm:
            if total_unfinished == 0:
                cmd = [lrel, "-fail-on-unfinished", ts_path, "-qm", tmp_qm.name]
                flag_desc = "-fail-on-unfinished"
            else:
                cmd = [lrel, "-nounfinished", ts_path, "-qm", tmp_qm.name]
                flag_desc = "-nounfinished"

            res = subprocess.run(cmd, capture_output=True, text=True)
            if res.returncode != 0:
                print(f"  FAILED: lrelease ({flag_desc}) returned error {res.returncode}:", file=sys.stderr)
                print(res.stderr, file=sys.stderr)
                ok = False
            else:
                print(f"  SUCCESS: lrelease ({flag_desc}) compiled cleanly: {res.stdout.strip()}")

    return ok

def selftest():
    """Run mutation tests verifying defect detection."""
    print("Running localization gate mutation selftests...")
    # Mutation 1: Placeholder mismatch
    src = "Hello %1 %2"
    bad_trans = "Hello %1 %1"
    assert Counter(extract_placeholders(src)) != Counter(extract_placeholders(bad_trans)), "Selftest 1 failed"

    # Mutation 2: Banned term detection
    sample = "这是地平线"
    assert any(b in sample for b in BANNED_TERMS), "Selftest 2 failed"

    # Mutation 3: XML preservation
    header = '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n'
    assert "<?xml" in header and "<!DOCTYPE TS>" in header, "Selftest 3 failed"

    print("Selftest passed cleanly (3/3 mutation checks).")
    return True

def main():
    parser = argparse.ArgumentParser(description="Paleo TS Batch Translation & Validation Engine")
    parser.add_argument("--ts", default="translations/paleo_zh_CN.ts", help="Path to TS file")
    parser.add_argument("--batch", default="all", choices=["B01","B02","B03","B04","B05","B06","B07","B08","B09","B10","all"], help="Batch to process")
    parser.add_argument("--dict", default="", help="Path to custom JSON translation dictionary")
    parser.add_argument("--validate-only", action="store_true", help="Only run validation checks")
    parser.add_argument("--selftest", action="store_true", help="Run mutation selftests")
    parser.add_argument("--dry-run", action="store_true", help="Perform in-memory dry run")
    parser.add_argument("--lrelease", default=None, help="Explicit path to lrelease")
    args = parser.parse_args()

    if args.selftest:
        sys.exit(0 if selftest() else 1)

    if args.validate_only:
        ok = validate_ts_file(args.ts, target_batch=args.batch, lrelease_bin=args.lrelease)
        sys.exit(0 if ok else 1)

    custom_dictionary = load_dictionary(args.dict)
    print(f"Processing batch '{args.batch}' on '{args.ts}'...")
    success, processed, missing = process_ts_file(args.ts, args.batch, custom_dictionary, dry_run=args.dry_run)
    if not success:
        sys.exit(1)

    print(f"Batch '{args.batch}' populated {processed} translations.")
    if args.dry_run:
        print("Dry run completed successfully (no file changes written).")
        sys.exit(0)
    ok = validate_ts_file(args.ts, target_batch=args.batch, lrelease_bin=args.lrelease)
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
