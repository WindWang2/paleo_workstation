// 层：数据
#include "algorithms/curveexpr.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace paleo::curveexpr
{
namespace
{
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

struct Token
{
  enum class Kind { Number, Ident, Op, End } kind = Kind::End;
  std::string text;
  double num = 0.0;
  size_t offset = 0;
};

class Lexer
{
public:
  explicit Lexer(const std::string &text) : m_text(text) {}

  // 取下一 token；词法错误（非法字符/坏数字）置 error 并返回 End。
  Token next()
  {
    while (m_pos < m_text.size()
           && (m_text[m_pos] == ' ' || m_text[m_pos] == '\t'
               || m_text[m_pos] == '\r' || m_text[m_pos] == '\n'))
      ++m_pos;
    Token t;
    t.offset = m_pos;
    if (m_pos >= m_text.size())
      return t; // End
    const char c = m_text[m_pos];
    if (std::isdigit(static_cast<unsigned char>(c))
        || (c == '.' && m_pos + 1 < m_text.size()
            && std::isdigit(static_cast<unsigned char>(m_text[m_pos + 1]))))
    {
      const char *begin = m_text.c_str() + m_pos;
      char *end = nullptr;
      t.num = std::strtod(begin, &end);
      const size_t len = static_cast<size_t>(end - begin);
      if (len == 0)
      {
        fail("malformed number", m_pos);
        return t;
      }
      t.kind = Token::Kind::Number;
      t.text = m_text.substr(m_pos, len);
      m_pos += len;
      return t;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
    {
      const size_t start = m_pos;
      while (m_pos < m_text.size()
             && (std::isalnum(static_cast<unsigned char>(m_text[m_pos]))
                 || m_text[m_pos] == '_'))
        ++m_pos;
      t.kind = Token::Kind::Ident;
      t.text = m_text.substr(start, m_pos - start);
      return t;
    }
    const auto twoChar = [&](char a, char b) {
      return c == a && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == b;
    };
    if (twoChar('<', '=') || twoChar('>', '=') || twoChar('=', '=')
        || twoChar('!', '=') || twoChar('&', '&') || twoChar('|', '|'))
    {
      t.kind = Token::Kind::Op;
      t.text = m_text.substr(m_pos, 2);
      m_pos += 2;
      return t;
    }
    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == '('
        || c == ')' || c == ',' || c == '<' || c == '>' || c == '!')
    {
      t.kind = Token::Kind::Op;
      t.text = std::string(1, c);
      ++m_pos;
      return t;
    }
    fail(std::string("unexpected character '") + c + "'", m_pos);
    return t; // End
  }

  bool hasError() const { return !m_error.empty(); }
  const std::string &error() const { return m_error; }
  size_t errorOffset() const { return m_errorOffset; }

private:
  void fail(const std::string &msg, size_t off)
  {
    if (m_error.empty())
    {
      m_error = msg;
      m_errorOffset = off;
    }
  }
  const std::string &m_text;
  size_t m_pos = 0;
  std::string m_error;
  size_t m_errorOffset = 0;
};

class Parser
{
public:
  Parser(const std::string &text, const std::vector<std::string> &variables)
      : m_lexer(text), m_variables(variables)
  {
    m_tok = m_lexer.next();
  }

  // 成功：返回根节点下标；失败：-1 且 error() 带位置。
  int parseExpr()
  {
    if (failed())
      return -1;
    int node = parseOr();
    if (failed())
      return -1;
    if (m_tok.kind != Token::Kind::End)
      return err("trailing characters after expression", m_tok.offset);
    return node;
  }

  bool failed() const { return m_failed || m_lexer.hasError(); }
  std::string error() const
  {
    if (m_lexer.hasError())
      return atOffset(m_lexer.error(), m_lexer.errorOffset());
    return atOffset(m_error, m_errorOffset);
  }

  std::vector<detail::ExprNode> takeNodes() { return std::move(m_nodes); }
  std::vector<std::string> takeUsed() { return std::move(m_used); }

private:
  static std::string atOffset(const std::string &msg, size_t off)
  {
    return "offset " + std::to_string(off) + ": " + msg;
  }
  int err(const std::string &msg, size_t off)
  {
    if (!m_failed)
    {
      m_error = msg;
      m_errorOffset = off;
      m_failed = true;
    }
    return -1;
  }
  void advance() { m_tok = m_lexer.next(); }
  bool eat(const char *op)
  {
    if (m_tok.kind == Token::Kind::Op && m_tok.text == op)
    {
      advance();
      return true;
    }
    return false;
  }
  bool expect(const char *op, const std::string &what)
  {
    if (eat(op))
      return true;
    err(what + " (expected '" + op + "')", m_tok.offset);
    return false;
  }
  int addNode(detail::Op op, int a = -1, int b = -1, int c = -1, double num = 0.0,
              int varSlot = -1)
  {
    detail::ExprNode n;
    n.op = op;
    n.a = a;
    n.b = b;
    n.c = c;
    n.num = num;
    n.varSlot = varSlot;
    m_nodes.push_back(n);
    return static_cast<int>(m_nodes.size()) - 1;
  }

  // or := and ( '||' and )*
  int parseOr()
  {
    int lhs = parseAnd();
    if (failed())
      return -1;
    while (m_tok.kind == Token::Kind::Op && m_tok.text == "||")
    {
      advance();
      int rhs = parseAnd();
      if (failed())
        return -1;
      lhs = addNode(detail::Op::Or, lhs, rhs);
    }
    return lhs;
  }

  // and := cmp ( '&&' cmp )*
  int parseAnd()
  {
    int lhs = parseCmp();
    if (failed())
      return -1;
    while (m_tok.kind == Token::Kind::Op && m_tok.text == "&&")
    {
      advance();
      int rhs = parseCmp();
      if (failed())
        return -1;
      lhs = addNode(detail::Op::And, lhs, rhs);
    }
    return lhs;
  }

  static bool isCmpOp(const std::string &t)
  {
    return t == "<" || t == "<=" || t == ">" || t == ">=" || t == "==" || t == "!=";
  }

  // cmp := add ( cmpop add )*   左结合链
  int parseCmp()
  {
    int lhs = parseAdd();
    if (failed())
      return -1;
    while (m_tok.kind == Token::Kind::Op && isCmpOp(m_tok.text))
    {
      const std::string op = m_tok.text;
      advance();
      int rhs = parseAdd();
      if (failed())
        return -1;
      detail::Op o = op == "<" ? detail::Op::Lt
                    : op == "<=" ? detail::Op::Le
                    : op == ">" ? detail::Op::Gt
                    : op == ">=" ? detail::Op::Ge
                    : op == "==" ? detail::Op::Eq
                                 : detail::Op::Ne;
      lhs = addNode(o, lhs, rhs);
    }
    return lhs;
  }

  // add := mul ( ('+'|'-') mul )*
  int parseAdd()
  {
    int lhs = parseMul();
    if (failed())
      return -1;
    while (m_tok.kind == Token::Kind::Op && (m_tok.text == "+" || m_tok.text == "-"))
    {
      const bool plus = m_tok.text == "+";
      advance();
      int rhs = parseMul();
      if (failed())
        return -1;
      lhs = addNode(plus ? detail::Op::Add : detail::Op::Sub, lhs, rhs);
    }
    return lhs;
  }

  // mul := unary ( ('*'|'/') unary )*
  int parseMul()
  {
    int lhs = parseUnary();
    if (failed())
      return -1;
    while (m_tok.kind == Token::Kind::Op && (m_tok.text == "*" || m_tok.text == "/"))
    {
      const bool mul = m_tok.text == "*";
      advance();
      int rhs = parseUnary();
      if (failed())
        return -1;
      lhs = addNode(mul ? detail::Op::Mul : detail::Op::Div, lhs, rhs);
    }
    return lhs;
  }

  // unary := ('-'|'!') unary | pow
  int parseUnary()
  {
    if (m_tok.kind == Token::Kind::Op && m_tok.text == "-")
    {
      advance();
      int inner = parseUnary();
      if (failed())
        return -1;
      return addNode(detail::Op::Neg, inner);
    }
    if (m_tok.kind == Token::Kind::Op && m_tok.text == "!")
    {
      advance();
      int inner = parseUnary();
      if (failed())
        return -1;
      return addNode(detail::Op::Not, inner);
    }
    return parsePow();
  }

  // pow := primary ( '^' unary )?   右结合且指数允许一元负号（2^-3）
  int parsePow()
  {
    int base = parsePrimary();
    if (failed())
      return -1;
    if (m_tok.kind == Token::Kind::Op && m_tok.text == "^")
    {
      advance();
      int exp = parseUnary(); // 右递归：a^b^c = a^(b^c)
      if (failed())
        return -1;
      return addNode(detail::Op::Pow, base, exp);
    }
    return base;
  }

  static bool isKnownFunction(const std::string &name)
  {
    return name == "where" || name == "min" || name == "max" || name == "abs"
           || name == "ln" || name == "log10" || name == "sqrt" || name == "exp"
           || name == "pow" || name == "clamp";
  }

  int varSlotFor(const std::string &name)
  {
    for (size_t i = 0; i < m_used.size(); ++i)
      if (m_used[i] == name)
        return static_cast<int>(i);
    m_used.push_back(name);
    return static_cast<int>(m_used.size()) - 1;
  }

  // 实参表：已消费 '('，解析到 ')'（调用方先 eat("(") 成功后才进来）。
  bool parseArgList(const std::string &fnName, std::vector<int> *args)
  {
    args->clear();
    if (eat(")"))
      return true;
    for (;;)
    {
      int a = parseOr();
      if (failed())
        return false;
      args->push_back(a);
      if (eat(","))
        continue;
      return expect(")", "missing ')' after " + fnName + "(...) arguments");
    }
  }

  int parsePrimary()
  {
    if (m_tok.kind == Token::Kind::Number)
    {
      const int n = addNode(detail::Op::Num, -1, -1, -1, m_tok.num);
      advance();
      return n;
    }
    if (m_tok.kind == Token::Kind::Ident)
    {
      const std::string name = m_tok.text;
      const size_t off = m_tok.offset;
      advance();
      if (eat("("))
      {
        std::vector<int> args;
        if (!parseArgList(name, &args))
          return -1;
        auto arityOk = [&](int want) {
          if (static_cast<int>(args.size()) != want)
          {
            err(name + "() takes " + std::to_string(want) + " argument(s), got "
                    + std::to_string(args.size()),
                off);
            return false;
          }
          return true;
        };
        if (name == "where" || name == "clamp")
        {
          if (!arityOk(3))
            return -1;
          return addNode(name == "where" ? detail::Op::Where : detail::Op::Clamp,
                         args[0], args[1], args[2]);
        }
        if (name == "min" || name == "max" || name == "pow")
        {
          if (!arityOk(2))
            return -1;
          return addNode(name == "min" ? detail::Op::Min
                          : name == "max" ? detail::Op::Max
                                          : detail::Op::PowFn,
                         args[0], args[1]);
        }
        if (name == "abs" || name == "ln" || name == "log10" || name == "sqrt"
            || name == "exp")
        {
          if (!arityOk(1))
            return -1;
          const detail::Op o = name == "abs" ? detail::Op::Abs
                               : name == "ln" ? detail::Op::Ln
                               : name == "log10" ? detail::Op::Log10
                               : name == "sqrt" ? detail::Op::Sqrt
                                                : detail::Op::Exp;
          return addNode(o, args[0]);
        }
        return err("unknown function '" + name + "'", off);
      }
      if (isKnownFunction(name))
        return err("function " + name + "() needs '(...)'", off);
      bool known = false;
      for (const auto &v : m_variables)
      {
        if (v == name)
        {
          known = true;
          break;
        }
      }
      if (!known)
        return err("unknown curve '" + name + "'", off);
      return addNode(detail::Op::Var, -1, -1, -1, 0.0, varSlotFor(name));
    }
    if (eat("("))
    {
      int inner = parseOr();
      if (failed())
        return -1;
      if (!expect(")", "missing ')'"))
        return -1;
      return inner;
    }
    if (m_tok.kind == Token::Kind::End)
      return err("unexpected end of expression", m_tok.offset);
    return err("unexpected token '" + m_tok.text + "'", m_tok.offset);
  }

  Lexer m_lexer;
  Token m_tok;
  const std::vector<std::string> &m_variables;
  std::vector<detail::ExprNode> m_nodes;
  std::vector<std::string> m_used;
  bool m_failed = false;
  std::string m_error;
  size_t m_errorOffset = 0;
};

double evalNode(const std::vector<detail::ExprNode> &nodes, int idx,
                const double *const *valueSlots, int pointIdx)
{
  const detail::ExprNode &nd = nodes[idx];
  const double a = nd.a >= 0 ? evalNode(nodes, nd.a, valueSlots, pointIdx) : 0.0;
  const double b = nd.b >= 0 ? evalNode(nodes, nd.b, valueSlots, pointIdx) : 0.0;
  const double c = nd.c >= 0 ? evalNode(nodes, nd.c, valueSlots, pointIdx) : 0.0;
  using Op = detail::Op;
  switch (nd.op)
  {
    case Op::Num: return nd.num;
    case Op::Var: return valueSlots[nd.varSlot][pointIdx];
    case Op::Add: return a + b;
    case Op::Sub: return a - b;
    case Op::Mul: return a * b;
    case Op::Div: return a / b; // IEEE：x/0 → ±∞、0/0 → NaN（如实）
    case Op::Pow: return std::pow(a, b);
    case Op::Neg: return -a;
    case Op::Lt: return (std::isnan(a) || std::isnan(b)) ? kNan : (a < b ? 1.0 : 0.0);
    case Op::Le: return (std::isnan(a) || std::isnan(b)) ? kNan : (a <= b ? 1.0 : 0.0);
    case Op::Gt: return (std::isnan(a) || std::isnan(b)) ? kNan : (a > b ? 1.0 : 0.0);
    case Op::Ge: return (std::isnan(a) || std::isnan(b)) ? kNan : (a >= b ? 1.0 : 0.0);
    case Op::Eq: return (std::isnan(a) || std::isnan(b)) ? kNan : (a == b ? 1.0 : 0.0);
    case Op::Ne: return (std::isnan(a) || std::isnan(b)) ? kNan : (a != b ? 1.0 : 0.0);
    case Op::And:
      return (std::isnan(a) || std::isnan(b)) ? kNan
                                              : ((a != 0.0 && b != 0.0) ? 1.0 : 0.0);
    case Op::Or:
      return (std::isnan(a) || std::isnan(b)) ? kNan
                                              : ((a != 0.0 || b != 0.0) ? 1.0 : 0.0);
    case Op::Not: return std::isnan(a) ? kNan : (a != 0.0 ? 0.0 : 1.0);
    case Op::Where:
      // 选择语义：条件 NaN → NaN；否则取选中支（未选支即便含 ±∞/NaN
      // 也不污染——其值被丢弃）
      if (std::isnan(a))
        return kNan;
      return a != 0.0 ? b : c;
    case Op::Min: return (std::isnan(a) || std::isnan(b)) ? kNan : (a < b ? a : b);
    case Op::Max: return (std::isnan(a) || std::isnan(b)) ? kNan : (a > b ? a : b);
    case Op::Abs: return std::fabs(a);
    case Op::Ln: return std::log(a);
    case Op::Log10: return std::log10(a);
    case Op::Sqrt: return std::sqrt(a);
    case Op::Exp: return std::exp(a);
    case Op::PowFn: return std::pow(a, b);
    case Op::Clamp:
      if (std::isnan(a) || std::isnan(b) || std::isnan(c))
        return kNan;
      return a < b ? b : (a > c ? c : a);
  }
  return kNan;
}

} // namespace

CompiledExpr CompiledExpr::compile(const std::string &text,
                                   const std::vector<std::string> &variables,
                                   std::string *error)
{
  CompiledExpr out;
  Parser parser(text, variables);
  const int root = parser.parseExpr();
  if (parser.failed() || root < 0)
  {
    if (error)
      *error = parser.failed() ? parser.error() : "empty expression";
    return out;
  }
  out.m_nodes = parser.takeNodes();
  out.m_used = parser.takeUsed();
  out.m_root = root;
  return out;
}

bool CompiledExpr::evaluate(
    int n, const std::vector<std::pair<std::string, const double *>> &vars,
    double *out, std::string *error) const
{
  if (m_root < 0)
  {
    if (error)
      *error = "invalid (uncompiled) expression";
    return false;
  }
  std::vector<const double *> valueSlots(m_used.size(), nullptr);
  for (const auto &v : vars)
  {
    for (size_t i = 0; i < m_used.size(); ++i)
    {
      if (m_used[i] == v.first)
        valueSlots[i] = v.second;
    }
  }
  for (size_t i = 0; i < valueSlots.size(); ++i)
  {
    if (!valueSlots[i])
    {
      if (error)
        *error = "missing input curve '" + m_used[i] + "'";
      return false;
    }
  }
  for (int i = 0; i < n; ++i)
    out[i] = evalNode(m_nodes, m_root, valueSlots.data(), i);
  return true;
}

} // namespace paleo::curveexpr
