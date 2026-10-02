// 层：数据
#pragma once

// curveexpr — 逐点曲线计算器表达式引擎（纯 std，无 Qt/GIS 依赖；递归下降
// 自研——vendor/系统无可链接表达式库，引新依赖违反钉位策略）。
//
// 语法（轮1 ledger 钉死）：
//   数字      整数/小数/科学计数（1.5e-3）
//   变量      标识符 [A-Za-z_][A-Za-z0-9_]*，编译期对已知曲线名表解析，
//             未知 → 带位置报错（不静默当 0）
//   运算      + - * /   ^（幂，右结合：2^3^2=512；-2^2=-4）   一元 - !
//   比较      < <= > >= == !=    逻辑 && ||（非零为真）
//   函数      where(c,a,b) min(a,b) max(a,b) abs(x) ln(x) log10(x)
//             sqrt(x) exp(x) pow(a,b) clamp(x,lo,hi)
//   分组      ( )
//
// NaN 语义（三值逻辑，与 IEEE 比较的 false 不同——条件掩膜不得把「无数据」
// 当「不满足」）：任何算术/比较/逻辑/函数操作的任一操作数 NaN → 结果 NaN
// （min/max/clamp 同）；where 的条件 NaN → NaN，条件确定时取选中支（未选
// 支即便含 ±∞/NaN 也不污染）；除零 → IEEE ±∞/NaN 如实输出（QC 面负责
// 标记，算子不吞）。
//
// 用法：compile 一次（跨井/跨段复用），evaluate 多次；evaluate 只需提供
// compile 实际用到的变量（usedVariables()）。

#include <string>
#include <utility>
#include <vector>

namespace paleo::curveexpr
{

namespace detail
{
enum class Op
{
  Num, Var,
  Add, Sub, Mul, Div, Pow, Neg,
  Lt, Le, Gt, Ge, Eq, Ne,
  And, Or, Not,
  Where, Min, Max, Abs, Ln, Log10, Sqrt, Exp, PowFn, Clamp
};

struct ExprNode
{
  Op op = Op::Num;
  double num = 0.0;
  int varSlot = -1;           // Op::Var：usedVariables 下标
  int a = -1, b = -1, c = -1; // 子节点（一元用 a，二元 a/b，where/clamp a/b/c）
};
} // namespace detail

class CompiledExpr
{
public:
  // 编译 `text`；`variables` 是合法变量名全集（曲线名）。失败时返回无效
  // 对象且 *error 非空（带字节偏移位置）。空表达式/尾随垃圾/未知标识符/
  // 括号不配对/函数元数错都是编译错误。
  static CompiledExpr compile(const std::string &text,
                              const std::vector<std::string> &variables,
                              std::string *error);

  bool isValid() const { return m_root >= 0; }
  // 编译期实际引用的变量（按首次出现序去重）；空 = 常量表达式。
  std::vector<std::string> usedVariables() const { return m_used; }

  // 逐点求值：`vars` 需覆盖 usedVariables()（多余项忽略）；缺名 → false +
  // error。out 长度 n，每点独立（无跨点状态）。
  bool evaluate(int n,
                const std::vector<std::pair<std::string, const double *>> &vars,
                double *out, std::string *error) const;

private:
  std::vector<detail::ExprNode> m_nodes;
  std::vector<std::string> m_used;
  int m_root = -1;
};

} // namespace paleo::curveexpr
