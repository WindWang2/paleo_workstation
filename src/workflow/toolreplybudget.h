// 层：功能
// workflow/toolreplybudget.h — H7：一次工具调用最终回包配额的纯值契约。
//
// 与 H1 的整次请求/历史预算分开：这里只回答「本次调用最多能带回多少字节的
// paleo.tool-result.v1 envelope」。controller 在唯一派发点
// （dispatchTurnTool）冻结该值并随调用传递，runner 据此选页，最终化时用同一
// 公式复核；读取端不得从模型 args、历史消息或自报 policy 取得该额度。
#pragma once
#include "../services/toolresultprotocol.h"

namespace paleo::chat {

// pendingResults 含本次调用自身：其余未完成调用各预留最小回复，剩余估算
// token 的 2 倍作为字节预算（当前字符近似口径下 ASCII JSON 的保守上界），
// 再夹在协议最小值与单页上限之间。估算器不是精确 tokenizer——这里只保证
// 回包字节有界，不宣称等于模型的真实 token 成本。
inline int replyQuotaBytes(int pendingResults, int toolTokens, int historyTokenBudget,
                           int minimumReplyTokens) {
  const int reserved = (pendingResults > 0 ? pendingResults - 1 : 0) * minimumReplyTokens;
  const int available = historyTokenBudget - toolTokens - reserved;
  return qBound(kMinimumEnvelopeBytes, available * 2, kResultPageBytes);
}

} // namespace paleo::chat
