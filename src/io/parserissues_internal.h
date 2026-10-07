// 层：数据
#pragma once

// 方向70（Unity 清障）：dlis/lis 解析器匿名 namespace 各带一份同构 addIssue
//（LasIssue 四参追加），UNITY_BUILD 合批即重定义。收拢单一定义；调用点经
// `using paleo::io_detail::addIssue;` 零改动。lasparser 的五参版本（带行号）
// 是不同签名的本地重载，保留原状。

#include "lasdoc.h"

#include <QList>
#include <QString>

namespace paleo::io_detail {

/// 追加一条解析告警（issues 为空则丢弃——只记不中断语义）。
inline void addIssue(QList<LasIssue> *issues, LasIssue::Severity sev,
                     LasIssue::Category cat, const QString &msg)
{
  if (!issues)
    return;
  LasIssue issue;
  issue.severity = sev;
  issue.category = cat;
  issue.message = msg;
  issues->append(issue);
}

} // namespace paleo::io_detail
