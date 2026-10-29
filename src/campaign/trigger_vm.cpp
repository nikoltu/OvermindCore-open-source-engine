#include "omc/campaign/trigger_vm.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace omc::campaign {

bool Value::as_bool() const {
    switch (type) {
        case Type::Bool:
        case Type::Int:
        case Type::Handle: return i != 0;
        case Type::Real: return r != 0.0;
        case Type::String: {
            if (s.empty()) return false;
            std::string t;
            for (char c : s) t.push_back(static_cast<char>(std::tolower((unsigned char)c)));
            return !(t == "false" || t == "0");
        }
    }
    return false;
}

double Value::as_real() const {
    switch (type) {
        case Type::Bool:
        case Type::Int:
        case Type::Handle: return static_cast<double>(i);
        case Type::Real: return r;
        case Type::String: return std::strtod(s.c_str(), nullptr);
    }
    return 0.0;
}

std::int64_t Value::as_int() const {
    switch (type) {
        case Type::Bool:
        case Type::Int:
        case Type::Handle: return i;
        case Type::Real: return static_cast<std::int64_t>(r);
        case Type::String: return std::strtoll(s.c_str(), nullptr, 10);
    }
    return 0;
}

std::string Value::as_string() const {
    switch (type) {
        case Type::Bool: return i ? "true" : "false";
        case Type::Int:
        case Type::Handle: return std::to_string(i);
        case Type::Real: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.10g", r);
            return buf;
        }
        case Type::String: return s;
    }
    return {};
}

namespace {

bool numeric(const Value& v) {
    return v.type == Value::Type::Int || v.type == Value::Type::Real ||
           v.type == Value::Type::Bool || v.type == Value::Type::Handle;
}

std::string canon_op(const std::string& name) {
    std::string t;
    for (char c : name) t.push_back(static_cast<char>(std::tolower((unsigned char)c)));
    if (name == "==" || t == "equal to" || t == "equals") return "==";
    if (name == "!=" || t == "not equal to") return "!=";
    if (name == "<=" || t == "less than or equal to") return "<=";
    if (name == ">=" || t == "greater than or equal to") return ">=";
    if (name == "<" || t == "less than") return "<";
    if (name == ">" || t == "greater than") return ">";
    if (name == "+" || t == "add") return "+";
    if (name == "-" || t == "subtract") return "-";
    if (name == "*" || t == "multiply") return "*";
    if (name == "/" || t == "divide") return "/";
    return name;
}

bool is_compare_op(const std::string& op) {
    return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
}
bool is_arith_op(const std::string& op) {
    return op == "+" || op == "-" || op == "*" || op == "/";
}

bool compare(const Value& a, const std::string& op, const Value& b) {
    if (numeric(a) && numeric(b)) {
        double x = a.as_real(), y = b.as_real();
        if (op == "==") return x == y;
        if (op == "!=") return x != y;
        if (op == "<") return x < y;
        if (op == ">") return x > y;
        if (op == "<=") return x <= y;
        if (op == ">=") return x >= y;
    }
    std::string x = a.as_string(), y = b.as_string();
    if (op == "==") return x == y;
    if (op == "!=") return x != y;
    if (op == "<") return x < y;
    if (op == ">") return x > y;
    if (op == "<=") return x <= y;
    if (op == ">=") return x >= y;
    return false;
}

}

bool TriggerVm::try_host(const std::string& name, const TrigFunction& fn, bool is_action, int depth,
                         Value& out) {
    if (!host_) return false;
    std::vector<Value> args;
    args.reserve(fn.params.size());
    for (const TrigParam& p : fn.params) args.push_back(eval_param(p, depth));
    bool handled = false;
    ResolvedNative in{fn, args, is_action};
    Value r = host_->call(name, in, handled);
    if (!handled) return false;
    ++stats_.native_calls;
    ++stats_.implemented[name];
    out = r;
    return true;
}

const Value* TriggerVm::global(const std::string& var_id) const {
    auto it = globals_.find(var_id);
    return it == globals_.end() ? nullptr : &it->second;
}

void TriggerVm::set_global(const std::string& var_id, Value v) { globals_[var_id] = std::move(v); }

const Trigger* TriggerVm::find_trigger(const std::string& id) const {
    for (const Trigger& t : model_.triggers)
        if (t.id == id) return &t;
    return nullptr;
}

bool TriggerVm::step() {
    if (++stats_.steps > max_steps_) {
        stats_.step_limit_hit = true;
        return false;
    }
    return true;
}

Value TriggerVm::eval_param(const TrigParam& p, int depth) {
    switch (p.kind) {
        case TrigParamKind::Empty:
            return Value::make_int(0);
        case TrigParamKind::Literal: {
            if (p.value_type == "int") return Value::make_int(std::strtoll(p.literal.c_str(), nullptr, 10));
            if (p.value_type == "real" || p.value_type == "fixed")
                return Value::make_real(std::strtod(p.literal.c_str(), nullptr));
            if (p.value_type == "bool") return Value::make_bool(p.literal == "true");
            return Value::make_string(p.literal);
        }
        case TrigParamKind::GameLink:
            return Value::make_string(p.literal);
        case TrigParamKind::GameId:
            return Value::make_handle(std::strtoll(p.literal.c_str(), nullptr, 10));
        case TrigParamKind::Preset:
            return Value::make_string(p.preset_name);
        case TrigParamKind::Variable: {
            auto it = globals_.find(p.ref.id);
            if (it != globals_.end()) return it->second;
            return Value::make_int(0);
        }
        case TrigParamKind::ElementRef:
            return Value::make_string(p.ref.id);
        case TrigParamKind::Function:
            return p.sub.empty() ? Value::make_int(0) : eval_function(p.sub[0], depth + 1);
    }
    return Value::make_int(0);
}

Value TriggerVm::eval_function(const TrigFunction& fn, int depth) {
    if (!step()) return Value::make_int(0);
    ++stats_.functions_evaluated;

    const std::string& n = fn.name;

    if (n == "Comparison") {
        std::string op;
        std::vector<const TrigParam*> operands;
        for (const TrigParam& p : fn.params) {
            if (p.kind == TrigParamKind::Preset && op.empty()) {
                std::string c = canon_op(p.preset_name);
                if (is_compare_op(c)) { op = c; continue; }
            }
            operands.push_back(&p);
        }
        if (op.empty() || operands.size() < 2) return Value::make_bool(false);
        Value a = eval_param(*operands[0], depth);
        Value b = eval_param(*operands[1], depth);
        return Value::make_bool(compare(a, op, b));
    }

    if (n == "And" || n == "Or") {
        bool acc = (n == "And");
        for (const TrigParam& p : fn.params) {
            bool v = eval_param(p, depth).as_bool();
            acc = (n == "And") ? (acc && v) : (acc || v);
        }
        return Value::make_bool(acc);
    }
    if (n == "Not") {
        bool v = fn.params.empty() ? false : eval_param(fn.params[0], depth).as_bool();
        return Value::make_bool(!v);
    }

    if (n == "Arithmetic (Integer)" || n == "Arithmetic (Real)") {
        bool real = (n == "Arithmetic (Real)");
        std::string op;
        std::vector<const TrigParam*> operands;
        for (const TrigParam& p : fn.params) {
            if (p.kind == TrigParamKind::Preset && op.empty()) {
                std::string c = canon_op(p.preset_name);
                if (is_arith_op(c)) { op = c; continue; }
            }
            operands.push_back(&p);
        }
        if (op.empty() || operands.size() < 2) return real ? Value::make_real(0) : Value::make_int(0);
        Value a = eval_param(*operands[0], depth);
        Value b = eval_param(*operands[1], depth);
        if (real) {
            double x = a.as_real(), y = b.as_real(), z = 0;
            if (op == "+") z = x + y; else if (op == "-") z = x - y;
            else if (op == "*") z = x * y; else if (op == "/") z = (y != 0 ? x / y : 0);
            return Value::make_real(z);
        }
        std::int64_t x = a.as_int(), y = b.as_int(), z = 0;
        if (op == "+") z = x + y; else if (op == "-") z = x - y;
        else if (op == "*") z = x * y; else if (op == "/") z = (y != 0 ? x / y : 0);
        return Value::make_int(z);
    }

    Value hv;
    if (try_host(n, fn,  false, depth, hv)) return hv;
    ++stats_.stub_calls;
    ++stats_.unimplemented[n];
    return Value::make_int(0);
}

bool TriggerVm::eval_conditions(const Trigger& t, int depth) {
    for (const TrigFunction& c : t.conditions) {
        if (c.is_comment) continue;
        ++stats_.conditions_evaluated;
        if (!eval_function(c, depth).as_bool()) return false;
    }
    return true;
}

void TriggerVm::exec_action(const TrigFunction& fn, int depth) {
    if (returning_) return;
    if (fn.is_comment) return;
    if (!step()) return;
    ++stats_.actions_executed;

    const std::string& n = fn.name;

    if (n == "Set Variable") {
        const TrigParam* var = nullptr;
        const TrigParam* val = nullptr;
        for (const TrigParam& p : fn.params) {
            if (p.kind == TrigParamKind::Variable && !var) var = &p;
            else if (!val) val = &p;
        }
        if (var) globals_[var->ref.id] = val ? eval_param(*val, depth) : Value::make_int(0);
        return;
    }

    if (n == "Modify Variable (Integer)" || n == "Modify Variable (Real)") {
        bool real = (n == "Modify Variable (Real)");
        const TrigParam* var = nullptr;
        const TrigParam* val = nullptr;
        std::string op;
        for (const TrigParam& p : fn.params) {
            if (p.kind == TrigParamKind::Variable && !var) { var = &p; continue; }
            if (p.kind == TrigParamKind::Preset && op.empty()) {
                std::string c = canon_op(p.preset_name);
                if (is_arith_op(c)) { op = c; continue; }
            }
            if (!val) val = &p;
        }
        if (var) {
            auto it = globals_.find(var->ref.id);
            Value cur = it != globals_.end() ? it->second
                                             : (real ? Value::make_real(0) : Value::make_int(0));
            Value delta = val ? eval_param(*val, depth) : Value::make_int(0);
            if (real) {
                double x = cur.as_real(), y = delta.as_real(), z = x;
                if (op == "+") z = x + y; else if (op == "-") z = x - y;
                else if (op == "*") z = x * y; else if (op == "/") z = (y != 0 ? x / y : x);
                globals_[var->ref.id] = Value::make_real(z);
            } else {
                std::int64_t x = cur.as_int(), y = delta.as_int(), z = x;
                if (op == "+") z = x + y; else if (op == "-") z = x - y;
                else if (op == "*") z = x * y; else if (op == "/") z = (y != 0 ? x / y : x);
                globals_[var->ref.id] = Value::make_int(z);
            }
        }
        return;
    }

    if (n == "Run Trigger") {
        ++stats_.run_triggers;
        const TrigParam* tgt = nullptr;
        bool check = true;
        for (const TrigParam& p : fn.params) {
            if (p.kind == TrigParamKind::ElementRef && p.element_type == "Trigger" && !tgt) tgt = &p;
            if (p.kind == TrigParamKind::Preset) {
                if (p.preset_name.find("Check") != std::string::npos &&
                    (p.preset_name.find("Don't") != std::string::npos ||
                     p.preset_name.find("Dont") != std::string::npos))
                    check = false;
            }
        }
        if (tgt) {
            if (const Trigger* target = find_trigger(tgt->ref.id))
                run_trigger(*target, check, depth + 1);
        }
        return;
    }

    if (n == "Wait") {
        ++stats_.waits;
        for (const TrigParam& p : fn.params) {
            if (p.kind == TrigParamKind::Literal || p.kind == TrigParamKind::Variable) {
                stats_.logical_time += eval_param(p, depth).as_real();
                break;
            }
        }
        return;
    }

    if (n == "If Then Else") {
        ++stats_.if_then_else;
        if (!fn.subs.empty()) {
            bool cond = true;
            bool saw_if = false;
            for (const TrigFunction& sf : fn.subs) {
                if (sf.sub_slot != "If") continue;
                saw_if = true;
                if (!eval_function(sf, depth).as_bool()) { cond = false; break; }
            }
            if (!saw_if) cond = false;
            const char* want = cond ? "Then" : "Else";
            for (const TrigFunction& sf : fn.subs) {
                if (returning_ || stats_.step_limit_hit) break;
                if (sf.sub_slot == want) { ++stats_.branch_actions_executed; exec_action(sf, depth); }
            }
            return;
        }
        std::vector<const TrigParam*> fns;
        for (const TrigParam& p : fn.params)
            if (p.kind == TrigParamKind::Function && !p.sub.empty()) fns.push_back(&p);
        if (fns.empty()) {
            ++stats_.branch_bodies_skipped;
            return;
        }
        bool cond = eval_param(*fns[0], depth).as_bool();
        const TrigParam* branch = cond ? (fns.size() > 1 ? fns[1] : nullptr)
                                       : (fns.size() > 2 ? fns[2] : nullptr);
        if (branch && !branch->sub.empty()) exec_action(branch->sub[0], depth);
        return;
    }

    if (n == "Return") {
        returning_ = true;
        return;
    }

    Value hv;
    if (try_host(n, fn,  true, depth, hv)) return;
    ++stats_.stub_calls;
    ++stats_.unimplemented[n];
}

void TriggerVm::run_trigger(const Trigger& t, bool check_conditions, int depth) {
    if (depth > max_depth_) {
        stats_.depth_limit_hit = true;
        return;
    }
    if (static_cast<std::size_t>(depth) > stats_.max_depth_reached)
        stats_.max_depth_reached = static_cast<std::size_t>(depth);
    if (stats_.step_limit_hit) return;

    if (check_conditions && !eval_conditions(t, depth)) return;

    bool saved = returning_;
    returning_ = false;
    for (const TrigFunction& a : t.actions) {
        if (returning_ || stats_.step_limit_hit) break;
        exec_action(a, depth);
    }
    returning_ = saved;
}

std::size_t TriggerVm::run_init() {
    std::size_t ran = 0;
    for (const Trigger& t : model_.triggers) {
        bool is_init = false;
        for (const TrigFunction& e : t.events)
            if (!e.is_comment && e.name == "Map Initialization") { is_init = true; break; }
        if (!is_init) continue;
        run_trigger(t,  true,  0);
        ++ran;
    }
    return ran;
}

std::size_t TriggerVm::fire_event(const std::string& event_name) {
    std::size_t ran = 0;
    for (const Trigger& t : model_.triggers) {
        bool match = false;
        for (const TrigFunction& e : t.events)
            if (!e.is_comment && e.name == event_name) { match = true; break; }
        if (!match) continue;
        run_trigger(t,  true,  0);
        ++ran;
    }
    return ran;
}

}
