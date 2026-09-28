#!/usr/bin/env bash
# new_module.sh — 新 src/ 顶层模块脚手架（devex 方向 T9）。
#
# 生成：层标记模板（checker 头三行硬检查）+ 模块 .h/.cpp + 测试骨架；
# 同步：tools/layering_vocab.json（词表外置单点事实——不登记则新目录
# 被 layer-marker 全量判违规，这是 checker 的 by-design 行为）；
# 打印：target 接线片段（粘进 cmake/extra-<wave>.cmake 或 CMakeLists.txt）。
#
# 用法：
#   scripts/new_module.sh <name> <层> [target-lib]
#     name        新顶层目录名（^[a-z][a-z0-9_]*$）
#     层          数据|功能|QGIS 封装|视图|组装根|测试壳（词表既有值）
#     target-lib  接线的静态库（缺省 paleo_<name>——自建新库时手动补 add_library）
#
# 例：scripts/new_module.sh geophysics 数据 paleo_domain
set -euo pipefail
cd "$(dirname "$0")/.."

NAME=${1:-}; LAYER=${2:-}; TARGET=${3:-}
[ -n "$NAME" ] && [ -n "$LAYER" ] || { sed -n '2,14p' "$0" >&2; exit 2; }
[[ "$NAME" =~ ^[a-z][a-z0-9_]*$ ]] || { echo "FAIL 模块名须匹配 ^[a-z][a-z0-9_]*\$：$NAME" >&2; exit 2; }
[ -d "src/$NAME" ] && { echo "FAIL src/$NAME 已存在" >&2; exit 2; }
[ -e "tests/tst_$NAME.cpp" ] && { echo "FAIL tests/tst_$NAME.cpp 已存在" >&2; exit 2; }

VOCAB=tools/layering_vocab.json
[ -f "$VOCAB" ] || { echo "FAIL 缺 $VOCAB" >&2; exit 2; }

# 词表登记 + 校验层值（唯一事实源是 JSON，脚本不自带第二份词表）
python3 - "$NAME" "$LAYER" "$VOCAB" <<'PY'
import json, sys
name, layer, path = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path, encoding="utf-8") as fh:
    vocab = json.load(fh)
values = set(vocab["layers"].values())
if layer not in values:
    sys.exit(f"FAIL 层值 `{layer}` 不在词表 {sorted(values)} 中")
if name in vocab["layers"]:
    sys.exit(f"FAIL 词表已有 {name}")
vocab["layers"][name] = layer
# 反向 ui/ 扫描覆盖所有非豁免层（视图/组装根/测试壳 by design 豁免）
if layer not in ("视图", "组装根", "测试壳"):
    vocab["non_view"].append(name)
with open(path, "w", encoding="utf-8") as fh:
    json.dump(vocab, fh, ensure_ascii=False, indent=2)
    fh.write("\n")
print(f"  .. 词表已登记 src/{name} -> {layer}")
PY

# 层标记模板（头三行内必须 `// 层：<值>`，checker 硬检查）
mkdir -p "src/$NAME"
GUARD=$(printf 'PALEO_%s_H' "$(echo "$NAME" | tr '[:lower:]' '[:upper:]')")
cat > "src/$NAME/$NAME.h" <<EOF
// 层：$LAYER
#ifndef ${GUARD}
#define ${GUARD}

#include <QString>

namespace paleo::${NAME} {

// TODO(<name>): 模块契约一句话——数据只问答/功能只编排（docs/UI_LAYER_PLAN.md）。
QString describe();

} // namespace paleo::${NAME}

#endif // ${GUARD}
EOF
cat > "src/$NAME/$NAME.cpp" <<EOF
// 层：$LAYER
#include "$NAME.h"

namespace paleo::${NAME} {

QString describe() { return QStringLiteral("$NAME"); }

} // namespace paleo::${NAME}
EOF

# 测试骨架（最小链接集 = 本模块库；跑 tools/check_layering.py 应全绿）
cat > "tests/tst_$NAME.cpp" <<EOF
#include <QtTest>

#include "../src/$NAME/$NAME.h"

class Test${NAME^} : public QObject
{
  Q_OBJECT
private slots:
  void smoke()
  {
    QCOMPARE(paleo::${NAME}::describe(), QStringLiteral("$NAME"));
  }
};

int main(int argc, char *argv[])
{
  QCoreApplication app(argc, argv); // 纯数据/功能层无需 QGIS init（按层裁）
  Test${NAME^} tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_$NAME.moc"
EOF

TARGET=${TARGET:-paleo_$NAME}
echo "  .. 生成 src/$NAME/{$NAME.h,$NAME.cpp} + tests/tst_$NAME.cpp"
cat <<EOF

== 接线片段（粘进 cmake/extra-<wave>.cmake；新方向单行注册，合并保留双方行）==
target_sources($TARGET PRIVATE
  src/$NAME/$NAME.cpp)
add_paleo_test(tst_$NAME)                                   # 伞式全链接
# 或最小链接集（链接瘦身，devex T1）：
# add_paleo_test(tst_$NAME LIBS $TARGET)

== 自检 ==
  python3 tools/check_layering.py --strict   # 词表已同步，应绿
EOF
