#include "fscript.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>

namespace fscript {

namespace {

// ---------------------------------------------------------------------------------------------
// Opcodes

enum Op : uint16_t {
    OP_CONST,
    OP_LOAD,
    OP_STORE,
    OP_ALOAD,   // a = buffer index; pops index
    OP_ASTORE,  // pops value, index
    OP_TAP,     // pops position, pushes linear interpolated read
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_POW,
    OP_NEG,
    OP_NOT,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_EQ,
    OP_NE,
    OP_AND,
    OP_OR,
    OP_FN1,   // a = function id
    OP_FN2,
    OP_FN3,
    OP_NOISE,
    OP_JMP,
    OP_JZ,
    OP_LOOP,  // backward jump, counted against the budget
    OP_POP,
    OP_END,
};

// ---------------------------------------------------------------------------------------------
// Builtin functions

enum Fn {
    // arity 1
    F_SIN, F_COS, F_TAN, F_ASIN, F_ACOS, F_ATAN, F_SINH, F_COSH, F_TANH, F_EXP, F_LOG, F_LOG2,
    F_LOG10, F_SQRT, F_ABS, F_SIGN, F_FLOOR, F_CEIL, F_ROUND, F_FRACT, F_HZ, F_MTOF, F_VTOF,
    F_DB, F_CLIP, F_SOFT, F_WARP,
    // arity 2
    F_POW, F_ATAN2, F_MIN, F_MAX, F_STEP,
    // arity 3
    F_CLAMP, F_MIX,
};

struct FnInfo {
    const char* name;
    int id;
    int arity;
    const char* doc;
};

const FnInfo kFns[] = {
    {"sin", F_SIN, 1, ""}, {"cos", F_COS, 1, ""}, {"tan", F_TAN, 1, ""},
    {"asin", F_ASIN, 1, ""}, {"acos", F_ACOS, 1, ""}, {"atan", F_ATAN, 1, ""},
    {"sinh", F_SINH, 1, ""}, {"cosh", F_COSH, 1, ""}, {"tanh", F_TANH, 1, ""},
    {"exp", F_EXP, 1, ""}, {"log", F_LOG, 1, ""}, {"log2", F_LOG2, 1, ""},
    {"log10", F_LOG10, 1, ""}, {"sqrt", F_SQRT, 1, ""}, {"abs", F_ABS, 1, ""},
    {"sign", F_SIGN, 1, ""}, {"floor", F_FLOOR, 1, ""}, {"ceil", F_CEIL, 1, ""},
    {"round", F_ROUND, 1, ""}, {"fract", F_FRACT, 1, "x - floor(x)"},
    {"hz", F_HZ, 1, "20 * 1000^p: 0..1 to 20 Hz..20 kHz (0.1 per octave)"},
    {"mtof", F_MTOF, 1, "MIDI note to Hz"},
    {"vtof", F_VTOF, 1, "volts to Hz, 0 V = C4"},
    {"db", F_DB, 1, "decibels to gain"},
    {"clip", F_CLIP, 1, "clamp to -1..1"},
    {"soft", F_SOFT, 1, "cubic soft clip, x - x^3/3 within +-1"},
    {"warp", F_WARP, 1, "tan(pi f / sr), the prewarped TPT gain of cutoff f (Hz)"},
    {"pow", F_POW, 2, ""}, {"atan2", F_ATAN2, 2, ""}, {"min", F_MIN, 2, ""},
    {"max", F_MAX, 2, ""}, {"step", F_STEP, 2, "x >= edge ? 1 : 0 (edge, x)"},
    {"clamp", F_CLAMP, 3, "(x, lo, hi)"},
    {"mix", F_MIX, 3, "a + (b - a) t (a, b, t)"},
};

const FnInfo* findFn(const std::string& name) {
    for (const FnInfo& f : kFns)
        if (name == f.name) return &f;
    return nullptr;
}

const char* const kBuiltinNames[B_COUNT] = {"in",  "in2", "p1", "p2", "p3",  "p4", "cv1", "cv2",
                                            "cv3", "cv4", "sr", "dt", "ch", "out", "out2"};

inline double fn1(int id, double x, double sr) {
    switch (id) {
        case F_SIN: return std::sin(x);
        case F_COS: return std::cos(x);
        case F_TAN: return std::tan(x);
        case F_ASIN: return std::asin(x);
        case F_ACOS: return std::acos(x);
        case F_ATAN: return std::atan(x);
        case F_SINH: return std::sinh(x);
        case F_COSH: return std::cosh(x);
        case F_TANH: return std::tanh(x);
        case F_EXP: return std::exp(x);
        case F_LOG: return std::log(x);
        case F_LOG2: return std::log2(x);
        case F_LOG10: return std::log10(x);
        case F_SQRT: return std::sqrt(x);
        case F_ABS: return std::fabs(x);
        case F_SIGN: return x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0);
        case F_FLOOR: return std::floor(x);
        case F_CEIL: return std::ceil(x);
        case F_ROUND: return std::round(x);
        case F_FRACT: return x - std::floor(x);
        case F_HZ: return 20.0 * std::pow(1000.0, x);
        case F_MTOF: return 440.0 * std::pow(2.0, (x - 69.0) / 12.0);
        case F_VTOF: return 261.6255653005986 * std::pow(2.0, x);
        case F_DB: return std::pow(10.0, x / 20.0);
        case F_CLIP: return x < -1 ? -1.0 : (x > 1 ? 1.0 : x);
        case F_SOFT: {
            double c = x < -1 ? -1.0 : (x > 1 ? 1.0 : x);
            return 1.5 * (c - c * c * c / 3.0);
        }
        case F_WARP: {
            double f = x;
            if (!(f > 0)) f = 0;
            if (f > 0.49 * sr) f = 0.49 * sr;
            return std::tan(M_PI * f / sr);
        }
    }
    return 0;
}

inline double fn2(int id, double a, double b) {
    switch (id) {
        case F_POW: return std::pow(a, b);
        case F_ATAN2: return std::atan2(a, b);
        case F_MIN: return a < b ? a : b;
        case F_MAX: return a > b ? a : b;
        case F_STEP: return b >= a ? 1.0 : 0.0;
    }
    return 0;
}

inline double fn3(int id, double a, double b, double c) {
    switch (id) {
        case F_CLAMP: return a < b ? b : (a > c ? c : a);
        case F_MIX: return a + (b - a) * c;
    }
    return 0;
}

inline int wrapIndex(double x, int size) {
    double fl = std::floor(x);
    long long i = (fl > -1e15 && fl < 1e15) ? (long long)fl : 0;
    i %= size;
    if (i < 0) i += size;
    return (int)i;
}

// ---------------------------------------------------------------------------------------------
// Lexer

enum TokKind { T_NUM, T_IDENT, T_OP, T_DIRECTIVE, T_EOF };

struct Token {
    TokKind kind;
    std::string text;
    double num = 0;
    int line, col;
};

struct CompileError {
    Error e;
};

[[noreturn]] void fail(int line, int col, const std::string& msg) {
    CompileError ce;
    ce.e.line = line;
    ce.e.col = col;
    ce.e.msg = msg;
    throw ce;
}

std::vector<Token> lex(const std::string& s) {
    std::vector<Token> out;
    size_t i = 0;
    int line = 1, col = 1;
    auto adv = [&](size_t n) {
        for (size_t k = 0; k < n && i < s.size(); k++, i++) {
            if (s[i] == '\n') {
                line++;
                col = 1;
            } else {
                col++;
            }
        }
    };
    static const char* const ops[] = {"..", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=",
                                      "/=", "+",  "-",  "*",  "/",  "%",  "^",  "<",  ">",  "=",
                                      "!",  "?",  ":",  "(",  ")",  "[",  "]",  "{",  "}",  ",",
                                      ";"};
    while (i < s.size()) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            adv(1);
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') adv(1);
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            int l0 = line, c0 = col;
            adv(2);
            while (i < s.size() && !(s[i] == '*' && i + 1 < s.size() && s[i + 1] == '/')) adv(1);
            if (i >= s.size()) fail(l0, c0, "unterminated comment");
            adv(2);
            continue;
        }
        Token t;
        t.line = line;
        t.col = col;
        if (c == '#') {
            size_t j = i;
            while (j < s.size() && s[j] != '\n') j++;
            t.kind = T_DIRECTIVE;
            t.text = s.substr(i + 1, j - i - 1);
            adv(j - i);
            out.push_back(t);
            continue;
        }
        if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1]))) {
            size_t j = i;
            while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
            if (j < s.size() && s[j] == '.' && !(j + 1 < s.size() && s[j + 1] == '.')) {
                j++;
                while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
            }
            if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
                size_t k = j + 1;
                if (k < s.size() && (s[k] == '+' || s[k] == '-')) k++;
                if (k < s.size() && std::isdigit((unsigned char)s[k])) {
                    while (k < s.size() && std::isdigit((unsigned char)s[k])) k++;
                    j = k;
                }
            }
            t.kind = T_NUM;
            t.text = s.substr(i, j - i);
            t.num = std::strtod(t.text.c_str(), nullptr);
            adv(j - i);
            if (i < s.size() && (std::isalpha((unsigned char)s[i]) || s[i] == '_'))
                fail(line, col, "unexpected character after number");
            out.push_back(t);
            continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            t.kind = T_IDENT;
            t.text = s.substr(i, j - i);
            adv(j - i);
            out.push_back(t);
            continue;
        }
        bool found = false;
        for (const char* op : ops) {
            size_t n = std::char_traits<char>::length(op);
            if (s.compare(i, n, op) == 0) {
                t.kind = T_OP;
                t.text = op;
                adv(n);
                out.push_back(t);
                found = true;
                break;
            }
        }
        if (!found) fail(line, col, std::string("unexpected character '") + c + "'");
    }
    Token e;
    e.kind = T_EOF;
    e.line = line;
    e.col = col;
    out.push_back(e);
    return out;
}

// ---------------------------------------------------------------------------------------------
// AST

struct Node;
typedef std::unique_ptr<Node> NodeP;

struct Node {
    enum Kind { NUM, VAR, INDEX, UNARY, BINARY, TERNARY, CALL } kind;
    double num = 0;
    std::string name;  // VAR, INDEX (buffer), CALL
    std::string op;    // UNARY, BINARY
    std::vector<NodeP> kids;
    int line = 0, col = 0;
};

struct Stmt;
typedef std::unique_ptr<Stmt> StmtP;

struct Stmt {
    enum Kind { ASSIGN, BLOCK, IF, FOR, INIT, BUF, CONST, FN } kind;
    std::string name;            // ASSIGN target, FOR variable, BUF/CONST/FN name
    std::string op;              // ASSIGN: = += -= *= /=
    NodeP index;                 // ASSIGN to buffer element
    NodeP expr;                  // ASSIGN value, IF condition, FOR from, BUF size, CONST value, FN body
    NodeP expr2;                 // FOR to
    std::vector<StmtP> body;     // BLOCK, INIT, IF then, FOR body
    std::vector<StmtP> other;    // IF else
    std::vector<double> table;   // BUF initialiser
    std::vector<std::string> params;  // FN
    int line = 0, col = 0;
};

// ---------------------------------------------------------------------------------------------
// Parser

struct Parser {
    std::vector<Token> t;
    size_t p = 0;
    Program* prog;

    const Token& cur() const { return t[p]; }
    bool isOp(const char* s) const { return t[p].kind == T_OP && t[p].text == s; }
    bool isIdent(const char* s) const { return t[p].kind == T_IDENT && t[p].text == s; }
    void expectOp(const char* s) {
        if (!isOp(s)) failHere(std::string("expected '") + s + "'");
        p++;
    }
    [[noreturn]] void failHere(const std::string& msg) {
        const Token& k = t[p];
        std::string got = k.kind == T_EOF ? "end of text" : "'" + k.text + "'";
        fail(k.line, k.col, msg + ", found " + got);
    }
    std::string ident(const char* what) {
        if (cur().kind != T_IDENT) failHere(std::string("expected ") + what);
        return t[p++].text;
    }

    NodeP mk(Node::Kind k, const Token& at) {
        NodeP n(new Node);
        n->kind = k;
        n->line = at.line;
        n->col = at.col;
        return n;
    }

    NodeP expr() { return ternary(); }

    NodeP ternary() {
        NodeP c = binary(0);
        if (isOp("?")) {
            Token at = cur();
            p++;
            NodeP a = ternary();
            expectOp(":");
            NodeP b = ternary();
            NodeP n = mk(Node::TERNARY, at);
            n->kids.push_back(std::move(c));
            n->kids.push_back(std::move(a));
            n->kids.push_back(std::move(b));
            return n;
        }
        return c;
    }

    static int prec(const std::string& op) {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "==" || op == "!=") return 3;
        if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
        if (op == "+" || op == "-") return 5;
        if (op == "*" || op == "/" || op == "%") return 6;
        return -1;
    }

    NodeP binary(int minPrec) {
        NodeP lhs = unary();
        while (cur().kind == T_OP) {
            int pr = prec(cur().text);
            if (pr < 0 || pr <= minPrec - 1 || pr < minPrec) break;
            Token at = cur();
            p++;
            NodeP rhs = binary(pr + 1);
            NodeP n = mk(Node::BINARY, at);
            n->op = at.text;
            n->kids.push_back(std::move(lhs));
            n->kids.push_back(std::move(rhs));
            lhs = std::move(n);
        }
        return lhs;
    }

    NodeP unary() {
        if (isOp("-") || isOp("!") || isOp("+")) {
            Token at = cur();
            p++;
            NodeP a = unary();
            if (at.text == "+") return a;
            NodeP n = mk(Node::UNARY, at);
            n->op = at.text;
            n->kids.push_back(std::move(a));
            return n;
        }
        return power();
    }

    NodeP power() {
        NodeP base = primary();
        if (isOp("^")) {
            Token at = cur();
            p++;
            NodeP e = unary();  // right associative, binds tighter than unary minus on the left
            NodeP n = mk(Node::BINARY, at);
            n->op = "^";
            n->kids.push_back(std::move(base));
            n->kids.push_back(std::move(e));
            return n;
        }
        return base;
    }

    NodeP primary() {
        const Token& k = cur();
        if (k.kind == T_NUM) {
            NodeP n = mk(Node::NUM, k);
            n->num = k.num;
            p++;
            return n;
        }
        if (k.kind == T_IDENT) {
            Token at = k;
            p++;
            if (isOp("(")) {
                p++;
                NodeP n = mk(Node::CALL, at);
                n->name = at.text;
                if (!isOp(")")) {
                    for (;;) {
                        n->kids.push_back(expr());
                        if (isOp(",")) {
                            p++;
                            continue;
                        }
                        break;
                    }
                }
                expectOp(")");
                return n;
            }
            if (isOp("[")) {
                p++;
                NodeP n = mk(Node::INDEX, at);
                n->name = at.text;
                n->kids.push_back(expr());
                expectOp("]");
                return n;
            }
            NodeP n = mk(Node::VAR, at);
            n->name = at.text;
            return n;
        }
        if (isOp("(")) {
            p++;
            NodeP n = expr();
            expectOp(")");
            return n;
        }
        failHere("expected a value");
    }

    StmtP mks(Stmt::Kind k, const Token& at) {
        StmtP s(new Stmt);
        s->kind = k;
        s->line = at.line;
        s->col = at.col;
        return s;
    }

    void block(std::vector<StmtP>& into, bool top) {
        expectOp("{");
        while (!isOp("}")) {
            if (cur().kind == T_EOF) failHere("expected '}'");
            statement(into, top);
        }
        p++;
    }

    void bodyOrStatement(std::vector<StmtP>& into) {
        if (isOp("{"))
            block(into, false);
        else
            statement(into, false);
    }

    void directive(const Token& d) {
        std::string s = d.text;
        size_t sp = s.find_first_of(" \t");
        std::string key = s.substr(0, sp);
        std::string val = sp == std::string::npos ? "" : s.substr(sp);
        size_t a = val.find_first_not_of(" \t");
        size_t b = val.find_last_not_of(" \t\r");
        val = a == std::string::npos ? "" : val.substr(a, b - a + 1);
        if (key == "name")
            prog->name = val;
        else if (key == "p1" || key == "p2" || key == "p3" || key == "p4")
            prog->paramLabel[key[1] - '1'] = val;
        else if (key == "in2")
            prog->in2Label = val;
        else if (key == "out")
            prog->outLabel = val;
        else if (key == "out2")
            prog->out2Label = val;
        else
            fail(d.line, d.col, "unknown directive '#" + key + "' (#name, #p1..#p4, #in2, #out, #out2)");
    }

    void statement(std::vector<StmtP>& into, bool top) {
        const Token at = cur();
        if (at.kind == T_DIRECTIVE) {
            if (!top) fail(at.line, at.col, "directives belong at the top level");
            directive(at);
            p++;
            return;
        }
        if (isOp(";")) {
            p++;
            return;
        }
        if (isOp("{")) {
            StmtP s = mks(Stmt::BLOCK, at);
            block(s->body, false);
            into.push_back(std::move(s));
            return;
        }
        if (at.kind != T_IDENT) failHere("expected a statement");
        const std::string& w = at.text;
        if (w == "init" || w == "buf" || w == "const" || w == "fn") {
            if (!top) fail(at.line, at.col, "'" + w + "' belongs at the top level");
            p++;
            if (w == "init") {
                StmtP s = mks(Stmt::INIT, at);
                block(s->body, false);
                into.push_back(std::move(s));
            } else if (w == "buf") {
                StmtP s = mks(Stmt::BUF, at);
                s->name = ident("a buffer name");
                if (isOp("[")) {
                    p++;
                    s->expr = expr();
                    expectOp("]");
                } else if (isOp("=")) {
                    p++;
                    expectOp("[");
                    for (;;) {
                        double sign = 1;
                        if (isOp("-")) {
                            sign = -1;
                            p++;
                        }
                        if (cur().kind != T_NUM) failHere("expected a number");
                        s->table.push_back(sign * cur().num);
                        p++;
                        if (isOp(",")) {
                            p++;
                            if (isOp("]")) break;
                            continue;
                        }
                        break;
                    }
                    expectOp("]");
                } else {
                    failHere("expected '[size]' or '= [values]'");
                }
                if (isOp(";")) p++;
                into.push_back(std::move(s));
            } else if (w == "const") {
                StmtP s = mks(Stmt::CONST, at);
                s->name = ident("a constant name");
                expectOp("=");
                s->expr = expr();
                if (isOp(";")) p++;
                into.push_back(std::move(s));
            } else {
                StmtP s = mks(Stmt::FN, at);
                s->name = ident("a function name");
                expectOp("(");
                if (!isOp(")")) {
                    for (;;) {
                        s->params.push_back(ident("a parameter name"));
                        if (isOp(",")) {
                            p++;
                            continue;
                        }
                        break;
                    }
                }
                expectOp(")");
                expectOp("=");
                s->expr = expr();
                if (isOp(";")) p++;
                into.push_back(std::move(s));
            }
            return;
        }
        if (w == "if") {
            p++;
            StmtP s = mks(Stmt::IF, at);
            s->expr = expr();
            bodyOrStatement(s->body);
            if (isIdent("else")) {
                p++;
                bodyOrStatement(s->other);
            }
            into.push_back(std::move(s));
            return;
        }
        if (w == "for") {
            p++;
            StmtP s = mks(Stmt::FOR, at);
            s->name = ident("a loop variable");
            if (!isIdent("in")) failHere("expected 'in'");
            p++;
            s->expr = expr();
            expectOp("..");
            s->expr2 = expr();
            block(s->body, false);
            into.push_back(std::move(s));
            return;
        }
        if (w == "else") failHere("'else' without 'if'");
        // assignment
        p++;
        StmtP s = mks(Stmt::ASSIGN, at);
        s->name = w;
        if (isOp("[")) {
            p++;
            s->index = expr();
            expectOp("]");
        }
        if (isOp("=") || isOp("+=") || isOp("-=") || isOp("*=") || isOp("/=")) {
            s->op = cur().text;
            p++;
        } else if (isOp("(")) {
            fail(at.line, at.col, "a function call is not a statement; assign its value");
        } else {
            failHere("expected '=' after '" + w + "'");
        }
        s->expr = expr();
        if (isOp(";")) p++;
        into.push_back(std::move(s));
    }
};

// ---------------------------------------------------------------------------------------------
// Code generation

struct Sym {
    enum Kind { BUILTIN, STATE, BUF, CONST, FN } kind;
    int slot = 0;       // BUILTIN, STATE: memory offset; BUF: buffer index
    double value = 0;   // CONST
    const Stmt* fn = nullptr;
};

struct Gen {
    Program* prog;
    std::map<std::string, Sym> syms;
    std::vector<std::map<std::string, int>> scopes;  // function parameters -> temp slot
    std::vector<Ins>* code = nullptr;
    int depth = 0, maxDepth = 0;
    int fnDepth = 0;
    int tempCount = 0;
    std::set<std::string> initAssigned;

    static const int kMaxStack = 256;

    void emit(Op op, int a = 0) {
        Ins i;
        i.op = op;
        i.a = a;
        code->push_back(i);
    }
    void push(int n = 1) {
        depth += n;
        if (depth > maxDepth) maxDepth = depth;
    }
    void pop(int n = 1) { depth -= n; }

    int constIndex(double v) {
        for (size_t i = 0; i < prog->consts.size(); i++)
            if (prog->consts[i] == v && std::signbit(prog->consts[i]) == std::signbit(v)) return (int)i;
        prog->consts.push_back(v);
        return (int)prog->consts.size() - 1;
    }

    int newSlot(const std::string& name, int size, bool buf, bool fresh) {
        Var v;
        v.name = name;
        v.offset = prog->memSize;
        v.size = size;
        v.buf = buf;
        v.fresh = fresh;
        prog->vars.push_back(v);
        prog->memSize += size;
        return v.offset;
    }

    // Looks a name up as a value (parameters first).
    bool lookupParam(const std::string& name, int& slot) {
        if (!scopes.empty()) {
            auto it = scopes.back().find(name);
            if (it != scopes.back().end()) {
                slot = it->second;
                return true;
            }
        }
        return false;
    }

    // Constant folding. Returns true and the value if n is a compile-time constant.
    bool constant(const Node* n, double& v, double sr = 48000) {
        switch (n->kind) {
            case Node::NUM: v = n->num; return true;
            case Node::VAR: {
                int slot;
                if (lookupParam(n->name, slot)) return false;
                auto it = syms.find(n->name);
                if (it != syms.end() && it->second.kind == Sym::CONST) {
                    v = it->second.value;
                    return true;
                }
                return false;
            }
            case Node::UNARY: {
                double a;
                if (!constant(n->kids[0].get(), a)) return false;
                v = n->op == "-" ? -a : (a == 0 ? 1 : 0);
                return true;
            }
            case Node::BINARY: {
                double a, b;
                if (!constant(n->kids[0].get(), a) || !constant(n->kids[1].get(), b)) return false;
                v = binop(n->op, a, b);
                return true;
            }
            case Node::TERNARY: {
                double c, a, b;
                if (!constant(n->kids[0].get(), c) || !constant(n->kids[1].get(), a) ||
                    !constant(n->kids[2].get(), b))
                    return false;
                v = c != 0 ? a : b;
                return true;
            }
            case Node::CALL: {
                const FnInfo* f = findFn(n->name);
                if (!f || f->id == F_WARP || (int)n->kids.size() != f->arity) return false;
                double a[3];
                for (int i = 0; i < f->arity; i++)
                    if (!constant(n->kids[i].get(), a[i])) return false;
                if (f->arity == 1)
                    v = fn1(f->id, a[0], sr);
                else if (f->arity == 2)
                    v = fn2(f->id, a[0], a[1]);
                else
                    v = fn3(f->id, a[0], a[1], a[2]);
                return true;
            }
            default: return false;
        }
    }

    static double binop(const std::string& op, double a, double b) {
        if (op == "+") return a + b;
        if (op == "-") return a - b;
        if (op == "*") return a * b;
        if (op == "/") return a / b;
        if (op == "%") return a - b * std::floor(a / b);
        if (op == "^") return std::pow(a, b);
        if (op == "<") return a < b;
        if (op == "<=") return a <= b;
        if (op == ">") return a > b;
        if (op == ">=") return a >= b;
        if (op == "==") return a == b;
        if (op == "!=") return a != b;
        if (op == "&&") return a != 0 && b != 0;
        if (op == "||") return a != 0 || b != 0;
        return 0;
    }

    static Op binOpcode(const std::string& op) {
        if (op == "+") return OP_ADD;
        if (op == "-") return OP_SUB;
        if (op == "*") return OP_MUL;
        if (op == "/") return OP_DIV;
        if (op == "%") return OP_MOD;
        if (op == "^") return OP_POW;
        if (op == "<") return OP_LT;
        if (op == "<=") return OP_LE;
        if (op == ">") return OP_GT;
        if (op == ">=") return OP_GE;
        if (op == "==") return OP_EQ;
        if (op == "!=") return OP_NE;
        if (op == "&&") return OP_AND;
        return OP_OR;
    }

    int bufIndex(const std::string& name, int line, int col) {
        auto it = syms.find(name);
        if (it == syms.end() || it->second.kind != Sym::BUF) fail(line, col, "'" + name + "' is not a buffer");
        return it->second.slot;
    }

    void expr(const Node* n) {
        double cv;
        if (constant(n, cv)) {
            emit(OP_CONST, constIndex(cv));
            push();
            return;
        }
        switch (n->kind) {
            case Node::NUM: break;  // folded above
            case Node::VAR: {
                int slot;
                if (lookupParam(n->name, slot)) {
                    emit(OP_LOAD, slot);
                    push();
                    return;
                }
                auto it = syms.find(n->name);
                if (it == syms.end()) fail(n->line, n->col, "unknown variable '" + n->name + "' (never assigned)");
                const Sym& s = it->second;
                if (s.kind == Sym::BUF)
                    fail(n->line, n->col, "'" + n->name + "' is a buffer; index it as " + n->name + "[i]");
                if (s.kind == Sym::FN) fail(n->line, n->col, "'" + n->name + "' is a function; call it");
                emit(OP_LOAD, s.slot);
                push();
                return;
            }
            case Node::INDEX: {
                int b = bufIndex(n->name, n->line, n->col);
                expr(n->kids[0].get());
                emit(OP_ALOAD, b);
                return;
            }
            case Node::UNARY:
                expr(n->kids[0].get());
                emit(n->op == "-" ? OP_NEG : OP_NOT);
                return;
            case Node::BINARY:
                expr(n->kids[0].get());
                expr(n->kids[1].get());
                emit(binOpcode(n->op));
                pop();
                return;
            case Node::TERNARY: {
                expr(n->kids[0].get());
                size_t jz = code->size();
                emit(OP_JZ);
                pop();
                expr(n->kids[1].get());
                size_t jmp = code->size();
                emit(OP_JMP);
                pop();
                (*code)[jz].a = (int)code->size();
                expr(n->kids[2].get());
                (*code)[jmp].a = (int)code->size();
                return;
            }
            case Node::CALL: call(n); return;
        }
    }

    void call(const Node* n) {
        const std::string& name = n->name;
        int argc = (int)n->kids.size();
        if (name == "noise") {
            if (argc != 0) fail(n->line, n->col, "noise() takes no arguments");
            emit(OP_NOISE);
            push();
            return;
        }
        if (name == "tap" || name == "len") {
            int want = name == "tap" ? 2 : 1;
            if (argc != want || n->kids[0]->kind != Node::VAR)
                fail(n->line, n->col, name == "tap" ? "tap(buffer, position)" : "len(buffer)");
            int b = bufIndex(n->kids[0]->name, n->kids[0]->line, n->kids[0]->col);
            if (name == "len") {
                emit(OP_CONST, constIndex(prog->bufs[b].size));
                push();
                return;
            }
            expr(n->kids[1].get());
            emit(OP_TAP, b);
            return;
        }
        auto it = syms.find(name);
        if (it != syms.end() && it->second.kind == Sym::FN) {
            const Stmt* f = it->second.fn;
            if (argc != (int)f->params.size())
                fail(n->line, n->col, name + "() takes " + std::to_string(f->params.size()) + " argument(s)");
            if (fnDepth >= 16) fail(n->line, n->col, "functions nested too deep (recursion?)");
            std::map<std::string, int> scope;
            for (int i = 0; i < argc; i++) {
                expr(n->kids[i].get());
                int slot = newSlot("$" + name + "." + f->params[i] + std::to_string(tempCount++), 1, false, true);
                emit(OP_STORE, slot);
                pop();
                scope[f->params[i]] = slot;
            }
            scopes.push_back(scope);
            fnDepth++;
            expr(f->expr.get());
            fnDepth--;
            scopes.pop_back();
            return;
        }
        const FnInfo* f = findFn(name);
        if (!f) fail(n->line, n->col, "unknown function '" + name + "'");
        if (argc != f->arity)
            fail(n->line, n->col, name + "() takes " + std::to_string(f->arity) + " argument(s)");
        if (f->id == F_WARP) {
            // warp needs sr: tan(pi * clamp(f, 0, 0.49 sr) * dt)
            expr(n->kids[0].get());
            emit(OP_LOAD, B_SR);
            push();
            emit(OP_FN2, -1);  // special: warp(f, sr)
            pop();
            return;
        }
        for (int i = 0; i < argc; i++) expr(n->kids[i].get());
        emit(argc == 1 ? OP_FN1 : (argc == 2 ? OP_FN2 : OP_FN3), f->id);
        pop(argc - 1);
    }

    void stmts(const std::vector<StmtP>& list, bool inInit) {
        for (const StmtP& s : list) stmt(s.get(), inInit);
    }

    void stmt(const Stmt* s, bool inInit) {
        switch (s->kind) {
            case Stmt::INIT: {
                std::vector<Ins>* saved = code;
                code = &prog->initCode;
                stmts(s->body, true);
                code = saved;
                return;
            }
            case Stmt::BUF:
            case Stmt::CONST:
            case Stmt::FN: return;  // declared in the first pass
            case Stmt::BLOCK: stmts(s->body, inInit); return;
            case Stmt::IF: {
                double c;
                if (constant(s->expr.get(), c)) {
                    stmts(c != 0 ? s->body : s->other, inInit);
                    return;
                }
                expr(s->expr.get());
                size_t jz = code->size();
                emit(OP_JZ);
                pop();
                stmts(s->body, inInit);
                if (s->other.empty()) {
                    (*code)[jz].a = (int)code->size();
                } else {
                    size_t jmp = code->size();
                    emit(OP_JMP);
                    (*code)[jz].a = (int)code->size();
                    stmts(s->other, inInit);
                    (*code)[jmp].a = (int)code->size();
                }
                return;
            }
            case Stmt::FOR: {
                int iv = syms[s->name].slot;
                int endSlot = newSlot("$for" + std::to_string(tempCount++), 1, false, true);
                expr(s->expr.get());
                emit(OP_STORE, iv);
                pop();
                expr(s->expr2.get());
                emit(OP_STORE, endSlot);
                pop();
                int top = (int)code->size();
                emit(OP_LOAD, iv);
                emit(OP_LOAD, endSlot);
                push(2);
                emit(OP_LT);
                pop();
                size_t jz = code->size();
                emit(OP_JZ);
                pop();
                stmts(s->body, inInit);
                emit(OP_LOAD, iv);
                emit(OP_CONST, constIndex(1));
                push(2);
                emit(OP_ADD);
                pop();
                emit(OP_STORE, iv);
                pop();
                emit(OP_LOOP, top);
                (*code)[jz].a = (int)code->size();
                return;
            }
            case Stmt::ASSIGN: {
                auto it = syms.find(s->name);
                const Sym& sym = it->second;  // the first pass created it or failed
                if (s->index) {
                    int b = sym.slot;
                    expr(s->index.get());
                    if (s->op != "=") {
                        expr(s->index.get());
                        emit(OP_ALOAD, b);
                        expr(s->expr.get());
                        emit(binOpcode(s->op.substr(0, 1)));
                        pop();
                    } else {
                        expr(s->expr.get());
                    }
                    emit(OP_ASTORE, b);
                    pop(2);
                    return;
                }
                if (s->op != "=") {
                    emit(OP_LOAD, sym.slot);
                    push();
                    expr(s->expr.get());
                    emit(binOpcode(s->op.substr(0, 1)));
                    pop();
                } else {
                    expr(s->expr.get());
                }
                emit(OP_STORE, sym.slot);
                pop();
                return;
            }
        }
    }

    // First pass: declarations and every assigned name, in source order.
    void declare(const std::vector<StmtP>& list, bool inInit) {
        for (const StmtP& sp : list) {
            const Stmt* s = sp.get();
            auto reserved = [&](const std::string& name) {
                if (findFn(name) || name == "noise" || name == "tap" || name == "len")
                    fail(s->line, s->col, "'" + name + "' is a builtin function");
                static const char* const kw[] = {"if", "else", "for", "in", "init", "buf", "const", "fn"};
                for (const char* k : kw)
                    if (name == k) fail(s->line, s->col, "'" + name + "' is a keyword");
            };
            auto taken = [&](const std::string& name) {
                reserved(name);
                if (syms.count(name)) fail(s->line, s->col, "'" + name + "' is already defined");
            };
            switch (s->kind) {
                case Stmt::BUF: {
                    taken(s->name);
                    BufInfo b;
                    if (s->expr) {
                        double v;
                        if (!constant(s->expr.get(), v)) fail(s->line, s->col, "buffer size must be a constant");
                        if (!(v >= 1 && v <= (1 << 20)))
                            fail(s->line, s->col, "buffer size must be 1 to 1048576");
                        b.size = (int)v;
                    } else {
                        b.size = (int)s->table.size();
                        b.init = s->table;
                    }
                    if (prog->memSize + b.size > (1 << 22)) fail(s->line, s->col, "buffers too large in total");
                    b.offset = newSlot(s->name, b.size, true, !b.init.empty());
                    Sym sym;
                    sym.kind = Sym::BUF;
                    sym.slot = (int)prog->bufs.size();
                    prog->bufs.push_back(b);
                    syms[s->name] = sym;
                    break;
                }
                case Stmt::CONST: {
                    taken(s->name);
                    double v;
                    if (!constant(s->expr.get(), v))
                        fail(s->line, s->col, "a const must be computable from numbers and other consts");
                    Sym sym;
                    sym.kind = Sym::CONST;
                    sym.value = v;
                    syms[s->name] = sym;
                    break;
                }
                case Stmt::FN: {
                    taken(s->name);
                    std::set<std::string> seen;
                    for (const std::string& p : s->params)
                        if (!seen.insert(p).second) fail(s->line, s->col, "parameter '" + p + "' repeated");
                    Sym sym;
                    sym.kind = Sym::FN;
                    sym.fn = s;
                    syms[s->name] = sym;
                    break;
                }
                case Stmt::INIT: declare(s->body, true); break;
                case Stmt::BLOCK: declare(s->body, inInit); break;
                case Stmt::IF:
                    declare(s->body, inInit);
                    declare(s->other, inInit);
                    break;
                case Stmt::FOR:
                    assigned(s, s->name, false, inInit);
                    declare(s->body, inInit);
                    break;
                case Stmt::ASSIGN: assigned(s, s->name, (bool)s->index, inInit); break;
            }
        }
    }

    void assigned(const Stmt* s, const std::string& name, bool indexed, bool inInit) {
        auto it = syms.find(name);
        if (it == syms.end()) {
            if (indexed) fail(s->line, s->col, "'" + name + "' is not a buffer (declare it: buf " + name + "[size])");
            if (findFn(name) || name == "noise" || name == "tap" || name == "len")
                fail(s->line, s->col, "'" + name + "' is a builtin function");
            Sym sym;
            sym.kind = Sym::STATE;
            sym.slot = newSlot(name, 1, false, false);
            syms[name] = sym;
            it = syms.find(name);
        }
        const Sym& sym = it->second;
        if (indexed) {
            if (sym.kind != Sym::BUF) fail(s->line, s->col, "'" + name + "' is not a buffer");
        } else if (sym.kind == Sym::BUILTIN) {
            if (sym.slot != B_OUT && sym.slot != B_OUT2)
                fail(s->line, s->col, "'" + name + "' is an input and cannot be assigned");
        } else if (sym.kind == Sym::BUF) {
            fail(s->line, s->col, "'" + name + "' is a buffer; assign an element as " + name + "[i] = ...");
        } else if (sym.kind == Sym::CONST) {
            fail(s->line, s->col, "'" + name + "' is a constant");
        } else if (sym.kind == Sym::FN) {
            fail(s->line, s->col, "'" + name + "' is a function");
        }
        if (inInit) initAssigned.insert(name);
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------------

bool compile(const std::string& src, Program& prog, Error& err) {
    prog = Program();
    try {
        Parser ps;
        ps.t = lex(src);
        ps.prog = &prog;
        std::vector<StmtP> top;
        while (ps.cur().kind != T_EOF) ps.statement(top, true);

        Gen g;
        g.prog = &prog;
        for (int i = 0; i < B_COUNT; i++) {
            Sym s;
            s.kind = Sym::BUILTIN;
            s.slot = i;
            g.syms[kBuiltinNames[i]] = s;
        }
        const struct {
            const char* name;
            double v;
        } consts[] = {{"pi", M_PI}, {"tau", 2 * M_PI}, {"e", M_E}};
        for (auto& c : consts) {
            Sym s;
            s.kind = Sym::CONST;
            s.value = c.v;
            g.syms[c.name] = s;
        }
        g.declare(top, false);
        for (Var& v : prog.vars)
            if (g.initAssigned.count(v.name)) v.fresh = true;
        g.code = &prog.code;
        g.stmts(top, false);
        g.emit(OP_END);
        g.code = &prog.initCode;
        g.emit(OP_END);
        if (g.maxDepth > Gen::kMaxStack) fail(1, 1, "expressions too deeply nested");
    } catch (CompileError& ce) {
        err = ce.e;
        prog = Program();
        return false;
    }
    return true;
}

void prepare(const Program& prog, Voice& v) {
    v.mem.assign(prog.memSize, 0.0);
}

static bool exec(const Program& prog, const std::vector<Ins>& code, Voice& v) {
    double st[Gen::kMaxStack + 4];
    int sp = 0;
    int budget = kLoopBudget;
    double* mem = v.mem.data();
    const double* k = prog.consts.data();
    const BufInfo* bufs = prog.bufs.data();
    const Ins* base = code.data();
    for (const Ins* ip = base;; ip++) {
        switch (ip->op) {
            case OP_CONST: st[sp++] = k[ip->a]; break;
            case OP_LOAD: st[sp++] = mem[ip->a]; break;
            case OP_STORE: mem[ip->a] = st[--sp]; break;
            case OP_ALOAD: {
                const BufInfo& b = bufs[ip->a];
                st[sp - 1] = mem[b.offset + wrapIndex(st[sp - 1], b.size)];
                break;
            }
            case OP_ASTORE: {
                const BufInfo& b = bufs[ip->a];
                double val = st[--sp];
                double idx = st[--sp];
                mem[b.offset + wrapIndex(idx, b.size)] = val;
                break;
            }
            case OP_TAP: {
                const BufInfo& b = bufs[ip->a];
                double pos = st[sp - 1];
                int i0 = wrapIndex(pos, b.size);
                double fr = pos - std::floor(pos);
                if (!(fr >= 0 && fr < 1)) fr = 0;
                int i1 = i0 + 1 == b.size ? 0 : i0 + 1;
                st[sp - 1] = mem[b.offset + i0] + (mem[b.offset + i1] - mem[b.offset + i0]) * fr;
                break;
            }
            case OP_ADD: sp--; st[sp - 1] += st[sp]; break;
            case OP_SUB: sp--; st[sp - 1] -= st[sp]; break;
            case OP_MUL: sp--; st[sp - 1] *= st[sp]; break;
            case OP_DIV: sp--; st[sp - 1] /= st[sp]; break;
            case OP_MOD: {
                sp--;
                double a = st[sp - 1], b = st[sp];
                st[sp - 1] = a - b * std::floor(a / b);
                break;
            }
            case OP_POW: sp--; st[sp - 1] = std::pow(st[sp - 1], st[sp]); break;
            case OP_NEG: st[sp - 1] = -st[sp - 1]; break;
            case OP_NOT: st[sp - 1] = st[sp - 1] == 0 ? 1.0 : 0.0; break;
            case OP_LT: sp--; st[sp - 1] = st[sp - 1] < st[sp]; break;
            case OP_LE: sp--; st[sp - 1] = st[sp - 1] <= st[sp]; break;
            case OP_GT: sp--; st[sp - 1] = st[sp - 1] > st[sp]; break;
            case OP_GE: sp--; st[sp - 1] = st[sp - 1] >= st[sp]; break;
            case OP_EQ: sp--; st[sp - 1] = st[sp - 1] == st[sp]; break;
            case OP_NE: sp--; st[sp - 1] = st[sp - 1] != st[sp]; break;
            case OP_AND: sp--; st[sp - 1] = (st[sp - 1] != 0 && st[sp] != 0); break;
            case OP_OR: sp--; st[sp - 1] = (st[sp - 1] != 0 || st[sp] != 0); break;
            case OP_FN1: st[sp - 1] = fn1(ip->a, st[sp - 1], mem[B_SR]); break;
            case OP_FN2:
                sp--;
                if (ip->a < 0)
                    st[sp - 1] = fn1(F_WARP, st[sp - 1], st[sp]);
                else
                    st[sp - 1] = fn2(ip->a, st[sp - 1], st[sp]);
                break;
            case OP_FN3:
                sp -= 2;
                st[sp - 1] = fn3(ip->a, st[sp - 1], st[sp], st[sp + 1]);
                break;
            case OP_NOISE: {
                uint32_t x = v.rng;
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                v.rng = x;
                st[sp++] = (double)x * (2.0 / 4294967296.0) - 1.0;
                break;
            }
            case OP_JMP: ip = base + ip->a - 1; break;
            case OP_JZ:
                if (st[--sp] == 0) ip = base + ip->a - 1;
                break;
            case OP_LOOP:
                if (--budget < 0) return false;
                ip = base + ip->a - 1;
                break;
            case OP_POP: sp--; break;
            case OP_END: return true;
        }
    }
}

bool reset(const Program& prog, Voice& v, double sampleRate, int channel) {
    if ((int)v.mem.size() != prog.memSize) v.mem.assign(prog.memSize, 0.0);
    double in[B_SR];
    for (int i = 0; i < B_SR; i++) in[i] = v.mem[i];
    std::fill(v.mem.begin(), v.mem.end(), 0.0);
    for (int i = 0; i < B_SR; i++) v.mem[i] = in[i];
    for (const BufInfo& b : prog.bufs)
        for (size_t i = 0; i < b.init.size(); i++) v.mem[b.offset + i] = b.init[i];
    v.mem[B_SR] = sampleRate;
    v.mem[B_DT] = 1.0 / sampleRate;
    v.mem[B_CH] = channel;
    v.rng = 0x9E3779B9u * (uint32_t)(channel + 1);
    return exec(prog, prog.initCode, v);
}

bool run(const Program& prog, Voice& v) {
    return exec(prog, prog.code, v);
}

void transfer(const Program& from, const Voice& vf, const Program& to, Voice& vt) {
    if (vf.mem.size() < (size_t)from.memSize || vt.mem.size() < (size_t)to.memSize) return;
    for (const Var& n : to.vars) {
        if (n.fresh) continue;
        for (const Var& o : from.vars) {
            if (o.name == n.name && o.size == n.size && o.buf == n.buf) {
                for (int i = 0; i < n.size; i++) vt.mem[n.offset + i] = vf.mem[o.offset + i];
                break;
            }
        }
    }
    vt.mem[B_OUT] = vf.mem[B_OUT];
    vt.mem[B_OUT2] = vf.mem[B_OUT2];
    vt.rng = vf.rng;
}

std::string reference() {
    std::string s;
    for (const FnInfo& f : kFns) {
        s += f.name;
        s += f.arity == 1 ? "(x)" : (f.arity == 2 ? "(a, b)" : "(a, b, c)");
        if (*f.doc) {
            s += "  ";
            s += f.doc;
        }
        s += "\n";
    }
    s += "noise()  uniform -1..1\ntap(buf, pos)  linear interpolated read, wrapping\nlen(buf)  buffer size\n";
    return s;
}

}  // namespace fscript
