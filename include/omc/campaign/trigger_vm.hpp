#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "omc/campaign/triggers.hpp"

namespace omc::campaign {

struct Value {
    enum class Type { Int, Real, Bool, String, Handle };
    Type type = Type::Int;
    std::int64_t i = 0;
    double r = 0.0;
    std::string s;

    static Value make_int(std::int64_t v) { Value x; x.type = Type::Int; x.i = v; return x; }
    static Value make_real(double v) { Value x; x.type = Type::Real; x.r = v; return x; }
    static Value make_bool(bool v) { Value x; x.type = Type::Bool; x.i = v ? 1 : 0; return x; }
    static Value make_string(std::string v) { Value x; x.type = Type::String; x.s = std::move(v); return x; }
    static Value make_handle(std::int64_t v) { Value x; x.type = Type::Handle; x.i = v; return x; }

    bool as_bool() const;
    double as_real() const;
    std::int64_t as_int() const;
    std::string as_string() const;
};

struct VmStats {
    std::size_t actions_executed = 0;
    std::size_t functions_evaluated = 0;
    std::size_t run_triggers = 0;
    std::size_t conditions_evaluated = 0;
    std::size_t waits = 0;
    std::size_t if_then_else = 0;
    std::size_t branch_bodies_skipped = 0;
    std::size_t branch_actions_executed = 0;
    double logical_time = 0.0;
    std::size_t steps = 0;
    std::size_t max_depth_reached = 0;
    bool step_limit_hit = false;
    bool depth_limit_hit = false;
    std::size_t stub_calls = 0;
    std::map<std::string, std::size_t> unimplemented;
    std::size_t native_calls = 0;
    std::map<std::string, std::size_t> implemented;
};

struct ResolvedNative {
    const TrigFunction& fn;
    const std::vector<Value>& args;
    bool is_action = true;
};

struct NativeHost {
    virtual ~NativeHost() = default;
    virtual Value call(const std::string& name, const ResolvedNative& in, bool& handled) = 0;
};

class TriggerVm {
  public:
    explicit TriggerVm(const TriggerModel& model) : model_(model) {}

    void set_limits(std::size_t max_steps, int max_depth) {
        max_steps_ = max_steps;
        max_depth_ = max_depth;
    }

    std::size_t run_init();

    void run_trigger(const Trigger& t, bool check_conditions, int depth);

    std::size_t fire_event(const std::string& event_name);

    const Value* global(const std::string& var_id) const;
    void set_global(const std::string& var_id, Value v);
    const std::unordered_map<std::string, Value>& globals() const { return globals_; }

    void set_native_host(NativeHost* host) { host_ = host; }
    NativeHost* native_host() const { return host_; }

    const VmStats& stats() const { return stats_; }

  private:
    Value eval_function(const TrigFunction& fn, int depth);
    Value eval_param(const TrigParam& p, int depth);
    void exec_action(const TrigFunction& fn, int depth);
    bool eval_conditions(const Trigger& t, int depth);

    bool step();

    const Trigger* find_trigger(const std::string& id) const;

    const TriggerModel& model_;
    std::unordered_map<std::string, Value> globals_;
    VmStats stats_;
    std::size_t max_steps_ = 500000;
    int max_depth_ = 64;
    bool returning_ = false;
    NativeHost* host_ = nullptr;

    bool try_host(const std::string& name, const TrigFunction& fn, bool is_action, int depth, Value& out);
};

}
