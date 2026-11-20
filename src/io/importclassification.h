// 层：数据
#pragma once
#include "../domain/projectclassifier.h"
#include <functional>

// P1-W6：分类详情——除类型外携带「容器已识别但内容损坏/超预算」的拒收
// 理由与取消标记。classifyImportFile 语义不变（旧调用方零改动）；需要区分
// 「合法非井表」与「坏容器」的调用方（宿主导入）走 classifyImportFileDetailed。
struct ImportClassificationDetail
{
  ProjectClassification cls;
  QString containerError; // 非空 = 井表容器的底层读取失败——导入必须带此错误失败，不得静默按通用文件入库
  bool cancelled = false; // cancel 谓词触发（containerError 以「已取消」开头）
};

ProjectClassification classifyImportFile(const QString &path);
ImportClassificationDetail classifyImportFileDetailed(const QString &path,
                                                      const std::function<bool()> &cancel = {});
