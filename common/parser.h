// common/parser.h
#pragma once
#include <memory>
#include <string>

// The functions the calculator understands. Names are turned into these ids once,
// at parse time, so evaluating never has to compare strings.
enum class func_id { sin, cos, tan, sqrt, abs, exp, log };

enum class node_kind { num, var, add, sub, mul, div, pow, neg, call };

// One node of the expression tree. "sin(x) * 2" becomes:
//
//   mul
//   +-- call(sin)
//   |     +-- var(x)
//   +-- num(2)
struct expr_node {
    node_kind kind = node_kind::num;
    double    value = 0;                 // num: the number
    char      name = 'x';                // var: 'x' or 'y'
    func_id   fn = func_id::sin;         // call: which function
    std::unique_ptr<expr_node> lhs, rhs; // children (unary nodes use only lhs)
};

using node_ptr = std::unique_ptr<expr_node>;

// Turns text into a tree. Throws std::runtime_error with a readable message on bad input.
node_ptr parse_expression(const std::string &text);

// True if the tree mentions x or y anywhere.
bool uses_variables(const expr_node *n);

// Evaluates the tree for one (x, y) pair, in double precision.
double eval(const expr_node *n, double x, double y);
