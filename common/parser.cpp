// common/parser.cpp
#include "parser.h"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr size_t MAX_LENGTH = 512;   // longest expression we accept
constexpr int    MAX_DEPTH  = 100;   // deepest nesting we accept

// ------------------------------------------------------------------ tokens --

enum class tok { number, ident, plus, minus, star, slash, caret, lparen, rparen, end };

struct token {
    tok         type;
    double      number = 0;   // for numbers
    std::string text;         // for identifiers
    size_t      pos = 0;      // where in the input it started, for error messages
};

[[noreturn]] void fail(const std::string &message, size_t pos) {
    throw std::runtime_error(message + " (at position " + std::to_string(pos + 1) + ")");
}

std::vector<token> tokenize(const std::string &s) {
    std::vector<token> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (std::isspace(c)) { i++; continue; }

        token t;
        t.pos = i;

        if (std::isdigit(c) || c == '.') {
            // A number: digits with at most one decimal point.
            size_t start = i;
            bool seen_dot = false;
            while (i < s.size() && (std::isdigit((unsigned char)s[i]) || (s[i] == '.' && !seen_dot))) {
                if (s[i] == '.') seen_dot = true;
                i++;
            }
            if (i - start == 1 && s[start] == '.') fail("stray '.'", start);
            t.type = tok::number;
            t.number = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        } else if (std::isalpha(c)) {
            // A name: letters then letters/digits.
            size_t start = i;
            while (i < s.size() && std::isalnum((unsigned char)s[i])) i++;
            t.type = tok::ident;
            t.text = s.substr(start, i - start);
        } else {
            switch (c) {
                case '+': t.type = tok::plus;   break;
                case '-': t.type = tok::minus;  break;
                case '*': t.type = tok::star;   break;
                case '/': t.type = tok::slash;  break;
                case '^': t.type = tok::caret;  break;
                case '(': t.type = tok::lparen; break;
                case ')': t.type = tok::rparen; break;
                default:  fail(std::string("unexpected character '") + (char)c + "'", i);
            }
            i++;
        }
        out.push_back(std::move(t));
    }
    token end;
    end.type = tok::end;
    end.pos = s.size();
    out.push_back(std::move(end));
    return out;
}

// ------------------------------------------------------------------ parser --
//
// Grammar, loosest-binding rule first:
//
//   expr    := term   (('+' | '-') term)*
//   term    := unary  (('*' | '/') unary)*
//   unary   := '-' unary | power
//   power   := primary ('^' unary)?
//   primary := number | name | name '(' expr ')' | '(' expr ')'
//
// Each rule is one function; a rule calls the next-tighter rule for its operands.

class parser {
public:
    explicit parser(std::vector<token> toks) : toks_(std::move(toks)) {}

    node_ptr parse_all() {
        node_ptr n = parse_expr();
        if (cur().type != tok::end) fail("unexpected input after the expression", cur().pos);
        return n;
    }

private:
    std::vector<token> toks_;
    size_t i_ = 0;
    int depth_ = 0;

    // Counts how deeply nested we are; throws instead of overflowing the stack.
    struct depth_guard {
        int &d;
        explicit depth_guard(int &depth, size_t pos) : d(depth) {
            if (++d > MAX_DEPTH) fail("expression is nested too deeply", pos);
        }
        ~depth_guard() { --d; }
    };

    const token &cur() const { return toks_[i_]; }
    void advance() { if (cur().type != tok::end) i_++; }

    static node_ptr make(node_kind k, node_ptr l = nullptr, node_ptr r = nullptr) {
        auto n = std::make_unique<expr_node>();
        n->kind = k;
        n->lhs = std::move(l);
        n->rhs = std::move(r);
        return n;
    }

    node_ptr parse_expr() {
        depth_guard g(depth_, cur().pos);
        node_ptr lhs = parse_term();
        while (cur().type == tok::plus || cur().type == tok::minus) {
            node_kind k = cur().type == tok::plus ? node_kind::add : node_kind::sub;
            advance();
            lhs = make(k, std::move(lhs), parse_term());
        }
        return lhs;
    }

    node_ptr parse_term() {
        node_ptr lhs = parse_unary();
        while (cur().type == tok::star || cur().type == tok::slash) {
            node_kind k = cur().type == tok::star ? node_kind::mul : node_kind::div;
            advance();
            lhs = make(k, std::move(lhs), parse_unary());
        }
        return lhs;
    }

    node_ptr parse_unary() {
        depth_guard g(depth_, cur().pos);
        if (cur().type == tok::minus) {
            advance();
            return make(node_kind::neg, parse_unary());
        }
        return parse_power();
    }

    node_ptr parse_power() {
        node_ptr base = parse_primary();
        if (cur().type == tok::caret) {
            advance();
            // The exponent goes through parse_unary, so 2^3^2 is 2^(3^2) and 2^-x works.
            return make(node_kind::pow, std::move(base), parse_unary());
        }
        return base;
    }

    node_ptr parse_primary() {
        const token t = cur();

        if (t.type == tok::number) {
            advance();
            auto n = make(node_kind::num);
            n->value = t.number;
            return n;
        }

        if (t.type == tok::lparen) {
            advance();
            node_ptr inner = parse_expr();
            expect_rparen();
            return inner;
        }

        if (t.type == tok::ident) {
            advance();

            if (cur().type == tok::lparen) {            // name(...)  -> function call
                advance();
                auto n = make(node_kind::call);
                n->fn = lookup_function(t);
                n->lhs = parse_expr();
                expect_rparen();
                return n;
            }

            if (t.text == "x" || t.text == "y") {       // a variable
                auto n = make(node_kind::var);
                n->name = t.text[0];
                return n;
            }
            if (t.text == "pi" || t.text == "e") {      // a constant: becomes a plain number
                auto n = make(node_kind::num);
                n->value = t.text == "pi" ? 3.14159265358979323846 : 2.71828182845904523536;
                return n;
            }
            fail("unknown name '" + t.text + "'", t.pos);
        }

        if (t.type == tok::end) fail("the expression ended unexpectedly", t.pos);
        fail("unexpected symbol", t.pos);
    }

    void expect_rparen() {
        if (cur().type != tok::rparen) fail("expected ')'", cur().pos);
        advance();
    }

    static func_id lookup_function(const token &t) {
        const std::string &s = t.text;
        if (s == "sin")  return func_id::sin;
        if (s == "cos")  return func_id::cos;
        if (s == "tan")  return func_id::tan;
        if (s == "sqrt") return func_id::sqrt;
        if (s == "abs")  return func_id::abs;
        if (s == "exp")  return func_id::exp;
        if (s == "log")  return func_id::log;
        fail("unknown function '" + s + "'", t.pos);
    }
};

} // namespace

node_ptr parse_expression(const std::string &text) {
    if (text.size() > MAX_LENGTH) throw std::runtime_error("expression is too long");
    parser p(tokenize(text));
    return p.parse_all();
}

bool uses_variables(const expr_node *n) {
    if (!n) return false;
    if (n->kind == node_kind::var) return true;
    return uses_variables(n->lhs.get()) || uses_variables(n->rhs.get());
}

double eval(const expr_node *n, double x, double y) {
    switch (n->kind) {
        case node_kind::num: return n->value;
        case node_kind::var: return n->name == 'x' ? x : y;
        case node_kind::add: return eval(n->lhs.get(), x, y) + eval(n->rhs.get(), x, y);
        case node_kind::sub: return eval(n->lhs.get(), x, y) - eval(n->rhs.get(), x, y);
        case node_kind::mul: return eval(n->lhs.get(), x, y) * eval(n->rhs.get(), x, y);
        case node_kind::div: return eval(n->lhs.get(), x, y) / eval(n->rhs.get(), x, y);
        case node_kind::pow: return std::pow(eval(n->lhs.get(), x, y), eval(n->rhs.get(), x, y));
        case node_kind::neg: return -eval(n->lhs.get(), x, y);
        case node_kind::call: {
            double v = eval(n->lhs.get(), x, y);
            switch (n->fn) {
                case func_id::sin:  return std::sin(v);
                case func_id::cos:  return std::cos(v);
                case func_id::tan:  return std::tan(v);
                case func_id::sqrt: return std::sqrt(v);
                case func_id::abs:  return std::fabs(v);
                case func_id::exp:  return std::exp(v);
                case func_id::log:  return std::log(v);
            }
        }
    }
    return 0;
}
