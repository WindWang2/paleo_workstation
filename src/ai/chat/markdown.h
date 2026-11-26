// 层：功能
#pragma once
#include <QString>

namespace AiMarkdown {
// Produces a QTextDocument HTML fragment for chat. Supports ATX headings,
// quotes, rules, nested lists and task items, aligned pipe tables, fenced code,
// inline emphasis, strikethrough, code and explicit HTTP(S) links. Raw HTML,
// images and unsupported link destinations stay visible as inert text.
QString toHtml(const QString &markdown);
}
