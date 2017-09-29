#include "Derivative.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Error.h"
#include "runtime/printer.h"
#include <iostream>

namespace Halide {
namespace Internal {

class VariableFinder : public IRGraphVisitor {
public:
    bool find(const Expr &expr, const Var &var) {
        visited.clear();
        var_name = var.name();
        found = false;
        expr.accept(this);
        return found;
    }

    void visit(const Variable *op) {
        if (op->name == var_name) {
            found = true;
        }
    }

private:
    std::string var_name;
    bool found;
};


class VariableReplacer : public IRMutator {
public:
     Expr replace(const Expr &expr, const std::string &replaced_var_name_, const Expr &replace_expr_) {
        replaced_var_name = replaced_var_name_;
        replace_expr = replace_expr_;
        return mutate(expr);
    }

    void visit(const Variable *op) {
        if (op->name == replaced_var_name) {
            expr = replace_expr;
        } else {
            expr = op;
        }
    }

private:
    std::string replaced_var_name;
    Expr replace_expr;
};


Expr inverse(const Var &var, const Expr &expr) {
    // TODO: replace with a full visitor
    VariableFinder finder;
    if (expr.get()->node_type == IRNodeType::Add) {
        const Add *op = expr.as<Add>();
        bool in_a = finder.find(op->a, var);
        bool in_b = finder.find(op->b, var);
        if (in_a && !in_b) {
            return inverse(var, op->a) - op->b;
        } else if (in_b && !in_a) {
            return inverse(var, op->b) - op->a;
        }
    } else if (expr.get()->node_type == IRNodeType::Sub) {
        const Sub *op = expr.as<Sub>();
        bool in_a = finder.find(op->a, var);
        bool in_b = finder.find(op->b, var);
        if (in_a && !in_b) {
            return inverse(var, op->a) + op->b;
        } else if (in_b && !in_a) {
            return inverse(var, op->b) - op->a;
        }
    } else if (expr.get()->node_type == IRNodeType::Max) {
        const Max *op = expr.as<Max>();
        bool in_a = finder.find(op->a, var);
        bool in_b = finder.find(op->b, var);
        if (in_a && !in_b) {
            return max(inverse(var, op->a), op->b);
        } else if (in_b && !in_a) {
            return max(op->a, inverse(var, op->b));
        }
    } else if (expr.get()->node_type == IRNodeType::Min) {
        const Min *op = expr.as<Min>();
        bool in_a = finder.find(op->a, var);
        bool in_b = finder.find(op->b, var);
        if (in_a && !in_b) {
            return min(inverse(var, op->a), op->b);
        } else if (in_b && !in_a) {
            return min(op->a, inverse(var, op->b));
        }
    } else if (expr.get()->node_type == IRNodeType::Variable) {
        return expr;
    }
    assert(false);
    return expr;
}

std::pair<Expr, Expr> get_min_max_bounds(const Expr &expr, const std::vector<Var> &current_args,
                                         const RDom &current_bounds, const int index) {
    if (expr.get()->node_type == IRNodeType::Add) {
        const Add *op = expr.as<Add>();
        const std::pair<Expr, Expr> a_bounds = get_min_max_bounds(op->a, current_args, current_bounds, index);
        const std::pair<Expr, Expr> b_bounds = get_min_max_bounds(op->b, current_args, current_bounds, index);
        return {a_bounds.first + b_bounds.first, a_bounds.second + b_bounds.second};
    } else if (expr.get()->node_type == IRNodeType::Sub) {
        const Sub *op = expr.as<Sub>();
        const std::pair<Expr, Expr> a_bounds = get_min_max_bounds(op->a, current_args, current_bounds, index);
        const std::pair<Expr, Expr> b_bounds = get_min_max_bounds(op->b, current_args, current_bounds, index);
        return {a_bounds.first - b_bounds.second, a_bounds.second - b_bounds.first};
    } else if (expr.get()->node_type == IRNodeType::Variable) {
        const Variable *var = expr.as<Variable>();
        if (var->reduction_domain.defined()) {
            ReductionVariable rvar = var->reduction_domain.domain()[index];
            return {rvar.min, rvar.min + rvar.extent};
        } else {
            for (int i = 0; i < (int)current_args.size(); i++) {
                if (current_args[i].name() == var->name) {
                    return {current_bounds[i].min(), current_bounds[i].extent()};
                }
            }
        }
    } else if (expr.get()->node_type == IRNodeType::Max) {
        const Max *op = expr.as<Max>();
        const std::pair<Expr, Expr> a_bounds = get_min_max_bounds(op->a, current_args, current_bounds, index);
        const std::pair<Expr, Expr> b_bounds = get_min_max_bounds(op->b, current_args, current_bounds, index);
        return {max(a_bounds.first, b_bounds.first), max(a_bounds.second, b_bounds.second)};
    } else if (expr.get()->node_type == IRNodeType::Min) {
        const Min *op = expr.as<Min>();
        const std::pair<Expr, Expr> a_bounds = get_min_max_bounds(op->a, current_args, current_bounds, index);
        const std::pair<Expr, Expr> b_bounds = get_min_max_bounds(op->b, current_args, current_bounds, index);
        return {min(a_bounds.first, b_bounds.first), min(a_bounds.second, b_bounds.second)};
    } else if (expr.get()->node_type == IRNodeType::IntImm) {
        return {expr, expr};
    }

    internal_error << "Can't infer bounds, Expr type not handled\n";
    return std::pair<Expr, Expr>();
}

std::pair<Expr, Expr> merge_bounds(const std::pair<Expr, Expr> &bounds0, const std::pair<Expr, Expr> &bounds1) {
    return {min(bounds0.first, bounds1.first), max(bounds0.second, bounds1.second)};
};

/** An IR graph visitor that gather the function DAG and sort them in reverse topological order
 */
class FunctionSorter : public IRGraphVisitor {
public:
    void sort(const Expr &expr);
    void sort(const Func &func);

    std::vector<Func> get_functions() const {
        return functions;
    }

    void visit(const Call *op);

private:
    std::vector<Func> functions;
    std::set<std::string> traversed_functions;
};

void FunctionSorter::sort(const Expr &expr) {
    // debug(0) << "FuncSorter: sorting Expr\n";
    visited.clear();
    expr.accept(this);
}

void FunctionSorter::sort(const Func &func) {
    // debug(0) << "FuncSorter: sorting Func " << func.name() << "\n";
    traversed_functions.insert(func.name());
    functions.push_back(Func(func));
    // Traverse from the last update to first
    for (int update_id = func.num_update_definitions() - 1; update_id >= -1; update_id--) {
        if (update_id >= 0) {
            // debug(0) << "  Recurse to update #" << update_id << "\n";
            func.update_value(update_id).accept(this);
        } else {
            // debug(0) << " Recurse to pure definition" << "\n";
            func.value().accept(this);
        }
    }
}

void FunctionSorter::visit(const Call *op) {
    if (op->call_type == Call::Halide) {
        Func func(Function(op->func));
        // debug(0) << "Visiting Call::Halide " << func.name() << "\n";

        // debug(0) << "  Traversed functions = { ";
        // for(auto f : traversed_functions) {
          // debug(0) << f << " ";
        // }
        // debug(0) << "}\n";

        if (traversed_functions.find(func.name()) != traversed_functions.end()) {
            // debug(0) << "  already traversed, not recursing." << "\n";
            return;
        }
        sort(func);
        return;
    }

    // debug(0) << "Visiting other Call" << "\n";
    for (size_t i = 0; i < op->args.size(); i++) {
        include(op->args[i]);
    }
}


/** An IR graph visitor that gather the expression DAG and sort them in topological order
 */
class ExpressionSorter : public IRGraphVisitor {
public:
    void sort(const Expr &expr);

    std::vector<Expr> get_expr_list() const {
        return expr_list;
    }

    void visit(const Call *op);
protected:
    void include(const Expr &e);
private:
    std::vector<Expr> expr_list;
};

void ExpressionSorter::sort(const Expr &e) {
    visited.clear();
    expr_list.clear();
    e.accept(this);
    expr_list.push_back(e);
}

void ExpressionSorter::visit(const Call *op) {
    // No point visiting the arguments of a Halide func or an image
    if (op->call_type == Call::Halide || op->call_type == Call::Image) {
        return;
    }

    for (size_t i = 0; i < op->args.size(); i++) {
        include(op->args[i]);
    }
}


void ExpressionSorter::include(const Expr &e) {
    if (visited.count(e.get())) {
        return;
    } else {
        visited.insert(e.get());
        e.accept(this);
        expr_list.push_back(e);
        return;
    }
}

/**
 *  Visit function calls and determine their bounds.
 *  So when we do f(x, y) = ... we know what the loop bounds are
 */
class BoundsInferencer : public IRGraphVisitor {
public:
    void inference(const Expr &expr);
    void inference(const Func &func);

    void visit(const Call *op);

    std::map<std::string, RDom> get_func_bounds() const {
        return func_bounds;
    }

private:
    std::map<std::string, RDom> func_bounds;
    std::set<std::string> traversed_functions;
    std::vector<Var> current_args;
    RDom current_bounds;
};

void BoundsInferencer::inference(const Expr &expr) {
    visited.clear();
    expr.accept(this);
}

void BoundsInferencer::inference(const Func &func) {
    traversed_functions.insert(func.name());
    // Traverse from the last update to first
    for (int update_id = func.num_update_definitions() - 1; update_id >= -1; update_id--) {
        if (update_id >= 0) {
            func.update_value(update_id).accept(this);
        } else {
            func.value().accept(this);
        }
    }
}

void BoundsInferencer::visit(const Call *op) {
    if (op->call_type == Call::Halide) {
        Func func(Function(op->func));

        std::vector<std::pair<Expr, Expr>> arg_bounds;
        arg_bounds.reserve(op->args.size());
        for (int i = 0; i < (int)op->args.size(); i++) {
            std::pair<Expr, Expr> min_max_bounds = get_min_max_bounds(op->args[i], current_args, current_bounds, i);
            // RDom accepts min/extent instead of min max
            arg_bounds.push_back({min_max_bounds.first, min_max_bounds.second - min_max_bounds.first});
        }
        // Update function bounds
        if (func_bounds.find(func.name()) != func_bounds.end()) {
            RDom prev_rdom = func_bounds[func.name()];
            std::vector<ReductionVariable> domain = prev_rdom.domain().domain();
            assert(arg_bounds.size() == domain.size());
            for (int i = 0; i < (int)arg_bounds.size(); i++) {
                std::pair<Expr, Expr> prev_bounds = {domain[i].min, domain[i].min + domain[i].extent};
                arg_bounds[i] = merge_bounds(prev_bounds, arg_bounds[i]);
            }
        }
        func_bounds[func.name()] = RDom(arg_bounds);

        if (traversed_functions.find(func.name()) != traversed_functions.end()) {
            // already traversed
            return;
        }

        RDom previous_bounds = current_bounds;
        std::vector<Var> previous_args = current_args;

        current_bounds = func_bounds[func.name()];
        current_args = func.args();
        inference(func);
        current_args = previous_args;
        current_bounds = previous_bounds;

        return;
    }

    for (size_t i = 0; i < op->args.size(); i++) {
        include(op->args[i]);
    }
}


/** An IR visitor that computes the derivatives through reverse accumulation
 */
class ReverseAccumulationVisitor : public IRVisitor {
public:
    void propagate_adjoints(const Expr &output, const std::vector<Func> &funcs);
    std::map<std::string, Func> get_adjoint_funcs() const { return adjoint_funcs; }

protected:
    void visit(const Cast *op);
    void visit(const Variable *op);
    void visit(const Add *op);
    void visit(const Sub *op);
    void visit(const Mul *op);
    void visit(const Div *op);
    void visit(const Min *op);
    void visit(const Max *op);
    void visit(const Call *op);
    void visit(const Let *op);

private:
    void accumulate(const Expr &stub, const Expr &adjoint);

    std::map<const BaseExprNode *, Expr> accumulated_adjoints;
    std::map<std::string, Func> adjoint_funcs;
    Func tmp_adjoint_func;
    std::map<std::string, Expr> let_var_mapping;
    std::map<std::string, RDom> func_bounds;
    RDom current_bounds;
    std::vector<Var> current_args;
    std::string current_func_name;
};


void ReverseAccumulationVisitor::propagate_adjoints(const Expr &output, const std::vector<Func> &funcs) {
    if (funcs.size() == 0) {
        debug(0) << "ReverseAccumulationVisitor: no functions to backpropagate to.\n";
        return;
    }

    BoundsInferencer bounds_inferencer;
    debug(0) << "ReverseAccumulationVisitor: infering bounds.\n";
    bounds_inferencer.inference(output);
    func_bounds = bounds_inferencer.get_func_bounds();

    // Create a stub for each function to accumulate adjoints
    for (int i = 0; i < (int)funcs.size(); i++) {
        Func adjoint_func(funcs[i].name() + "_d__");
        adjoint_func(funcs[i].args()) = 0.f;
        adjoint_funcs[funcs[i].name()] = adjoint_func;
    }

    // Propagate output
    ExpressionSorter sorter;
    sorter.sort(output);
    std::vector<Expr> expr_list = sorter.get_expr_list();
    accumulate(output, 1.f);

    // Traverse the expressions in reverse order
    for (auto it = expr_list.rbegin(); it != expr_list.rend(); it++) {
        // Propagate adjoints
        it->accept(this);
    }

    // Traverse functions
    for (int i = 0; i < (int)funcs.size(); i++) {
        const Func &func = funcs[i];
        current_func_name = func.name();

        // Traverse from the last update to first
        for (int update_id = func.num_update_definitions() - 1; update_id >= -1; update_id--) {
            // Topologically sort the expressions
            ExpressionSorter sorter;
            if (update_id >= 0) {
                sorter.sort(func.update_value(update_id));
            } else {
                sorter.sort(func.value());
            }

            // TODO: take lhs other than (x, y, z) into account
            assert(func_bounds.find(func.name()) != func_bounds.end());
            current_bounds = func_bounds[func.name()];
            current_args = func.args();

            std::vector<Expr> expr_list = sorter.get_expr_list();
            // Retrieve previously propagated adjoint
            if (update_id == func.num_update_definitions() - 1) {
                std::vector<Expr> args;
                for (const auto &arg : func.args()) {
                    args.push_back(arg);
                }
                accumulated_adjoints[(const BaseExprNode *)expr_list.back().get()] =
                    Call::make(adjoint_funcs[func.name()].function(), args);
            }

            // Propagate to this temporary Func if we use the same function during update
            tmp_adjoint_func = Func(func.name() + "_d__");
            tmp_adjoint_func(func.args()) = 0.f;

            // Traverse the expressions in reverse order
            for (auto it = expr_list.rbegin(); it != expr_list.rend(); it++) {
                // Propagate adjoints
                it->accept(this);
            }

            // Add back the Func
            Func &adjoint_func = adjoint_funcs[func.name()];
            tmp_adjoint_func(adjoint_func.args()) += adjoint_func(adjoint_func.args());
            adjoint_funcs[func.name()] = tmp_adjoint_func;
        }
    }
}

void ReverseAccumulationVisitor::accumulate(const Expr &stub, const Expr &adjoint) {
    const BaseExprNode *stub_ptr = (const BaseExprNode *)stub.get();
    if (accumulated_adjoints.find(stub_ptr) == accumulated_adjoints.end()) {
        accumulated_adjoints[stub_ptr] = adjoint;
    } else {
        accumulated_adjoints[stub_ptr] += adjoint;
    }
}

void ReverseAccumulationVisitor::visit(const Cast *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    // d/dx cast(x) = 1
    accumulate(op->value, adjoint);
}

void ReverseAccumulationVisitor::visit(const Variable *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    auto it = let_var_mapping.find(op->name);
    if (it != let_var_mapping.end()) {
        accumulate(it->second, Let::make(op->name, it->second, adjoint));
    }
}

void ReverseAccumulationVisitor::visit(const Add *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    // d/da a + b = 1
    accumulate(op->a, adjoint);
    // d/db a + b = 1
    accumulate(op->b, adjoint);
}

void ReverseAccumulationVisitor::visit(const Sub *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    // d/da a - b = 1
    accumulate(op->a, adjoint);
    // d/db a - b = -1
    accumulate(op->b, -adjoint);
}

void ReverseAccumulationVisitor::visit(const Mul *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    // d/da a * b = b
    accumulate(op->a, adjoint * op->b);
    // d/db a * b = a
    accumulate(op->b, adjoint * op->a);
}

void ReverseAccumulationVisitor::visit(const Div *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    // d/da a / b = 1 / b
    accumulate(op->a, adjoint / op->b);
    // d/db a / b = - a / b^2
    accumulate(op->b, - adjoint * op->a / (op->b * op->b));
}

void ReverseAccumulationVisitor::visit(const Min *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    // d/da min(a, b) = a <= b ? 1 : 0
    accumulate(op->a, select(op->a <= op->b, adjoint, 0.f));
    // d/db min(a, b) = b <= a ? 1 : 0
    accumulate(op->b, select(op->b <= op->a, adjoint, 0.f));
}

void ReverseAccumulationVisitor::visit(const Max *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    // d/da max(a, b) = a >= b ? 1 : 0
    accumulate(op->a, select(op->a >= op->b, adjoint, 0.f));
    // d/db max(a, b) = b >= a ? 1 : 0
    accumulate(op->b, select(op->b >= op->a, adjoint, 0.f));
}

void ReverseAccumulationVisitor::visit(const Call *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];
    if (op->name == "exp_f32") {
        // d/dx exp(x) = exp(x)
        for (size_t i = 0; i < op->args.size(); i++) {
            accumulate(op->args[i], adjoint * exp(op->args[i]));
        }
    }

    if (op->func.defined()) {
        // This is a Halide function call
        Function func(op->func);
        // Gather the domain variables of the function
        std::vector<std::string> func_args = func.args();
        std::vector<Var> args;
        std::for_each(func_args.begin(), func_args.end(),
                      [&args](const std::string &name){ args.push_back(Var(name)); });
        // We are scattering to this function
        debug(0) << "Scattering to " << func.name() << "\n";
        debug(0) << "op->args:" << "\n";
        for (const auto &arg : op->args) {
            debug(0) << arg << "\n";
        }
        debug(0) << "adjoint is:" << adjoint << "\n";
        Func& func_to_update = func.name() != current_func_name ?
            adjoint_funcs[func.name()] : tmp_adjoint_func;
        // We want to do this:
        // func_to_update(op->args) += adjoint;
        // But op->args can be invalid lhs, need to canonicalize

        VariableFinder finder;
        VariableReplacer replacer;
        assert(func_bounds.find(func.name()) != func_bounds.end());

        // We canonicalize the left hand side arguments (op->args) so that it's always x, y, z, ...
        for (int i = 0; i < (int)op->args.size(); i++) {
            if (!finder.find(op->args[i], args[i])) {
                // When an argument x doesn't appear in op->args,
                // all x in adjoint needs to be replaced by a RDom looping through the bounds
                // of the current function
                if (finder.find(adjoint, args[i])) {
                    adjoint = replacer.replace(adjoint, args[i].name(), current_bounds[i]);
                }
                // If it's a RVar, we need to replace it with the non-reduction argument
                if (op->args[i].get()->node_type == IRNodeType::Variable) {
                    const Variable *var = op->args[i].as<Variable>();
                    if (var->reduction_domain.defined()) {
                        adjoint = replacer.replace(adjoint, var->name, args[i]);
                    }
                }
            } else {
                // Apply the inverse to rhs
                Expr inverse_arg = inverse(args[i], op->args[i]);
                adjoint = replacer.replace(adjoint, args[i].name(), inverse_arg);
            }
        }

        debug(0) << "adjoint after canonicalization:" << adjoint << "\n";
        func_to_update(args) += adjoint;
        debug(0) << "print(func_to_update)" << "\n";
        print_func(func_to_update);
    }
}

void ReverseAccumulationVisitor::visit(const Let *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    accumulate(op->body, adjoint);
    let_var_mapping[op->name] = op->value;
}

} // namespace Internal

std::map<std::string, Func> propagate_adjoints(const Expr &output) {
    Internal::FunctionSorter sorter;
    Internal::debug(0) << "Propagate: Sorting functions" << "\n";
    sorter.sort(output);
    std::vector<Func> funcs = sorter.get_functions();
    Internal::debug(0) << "Propagate: Sorted Func list:" << "\n";
    for (const auto &func : funcs) {
        Internal::debug(0) << "  . " << func.name() << "\n";
    }
    Internal::ReverseAccumulationVisitor visitor;
    visitor.propagate_adjoints(output, funcs);
    return visitor.get_adjoint_funcs();
}

void print_func(const Func &func) {
    Internal::debug(0) << "Printing function:" << func.name() << "\n";
    Internal::FunctionSorter sorter;
    sorter.sort(func);
    std::vector<Func> funcs = sorter.get_functions();
    for (int i = (int)funcs.size() - 1; i >= 0; i--) {
        Func &func = funcs[i];
        Internal::debug(0) << "  funcs[" << i << "]: " << func.name() << "\n";
        for (int update_id = -1; update_id < func.num_update_definitions(); update_id++) {
            if (update_id >= 0) {
                Internal::debug(0) << "    update:" << func.update_value(update_id) << "\n";
            } else {
                Internal::debug(0) << "    init:" << func.value() << "\n";
            }
        }
    }
}

} // namespace Halide
