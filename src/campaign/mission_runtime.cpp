#include "omc/campaign/mission_runtime.hpp"

#include <cctype>
#include <string>
#include <vector>

#include "omc/app/map_world.hpp"
#include "omc/app/spawn.hpp"
#include "omc/data/catalog.hpp"

namespace omc::campaign {

namespace {

std::string lower(const std::string& s) {
    std::string t;
    t.reserve(s.size());
    for (char c : s) t.push_back(static_cast<char>(std::tolower((unsigned char)c)));
    return t;
}

bool contains(const std::string& hay, const char* needle) {
    return lower(hay).find(needle) != std::string::npos;
}

class MissionWorldHost : public NativeHost {
  public:
    MissionWorldHost(const data::Catalog& cat, sim::World& world, MissionRunReport& report,
                     sim::Fixed cx, sim::Fixed cy)
        : cat_(cat), world_(world), report_(report), cx_(cx), cy_(cy) {}

    Value call(const std::string& name, const ResolvedNative& in, bool& handled) override {
        handled = false;
        if (name.rfind("Create Unit", 0) == 0) return create_units(in, handled);
        if (contains(name, "kill unit") || contains(name, "remove unit")) return kill_unit(in, handled);
        if (name == "Set Alliance" || name == "Set Player Alliance") return set_alliance(in, handled);
        if (name.rfind("Modify Player Property", 0) == 0 || contains(name, "set resource"))
            return modify_player_property(in, handled);
        return Value::make_int(0);
    }

  private:
    static std::string gamelink(const ResolvedNative& in) {
        for (const TrigParam& p : in.fn.params)
            if (p.kind == TrigParamKind::GameLink) return p.literal;
        return {};
    }

    static std::vector<std::int64_t> scalars(const ResolvedNative& in) {
        std::vector<std::int64_t> out;
        for (std::size_t i = 0; i < in.fn.params.size(); ++i) {
            const TrigParam& p = in.fn.params[i];
            if (p.kind == TrigParamKind::Literal || p.kind == TrigParamKind::Variable)
                out.push_back(in.args[i].as_int());
        }
        return out;
    }

    static bool resolve_point(const ResolvedNative& in, sim::Fixed& x, sim::Fixed& y) {
        for (const TrigParam& p : in.fn.params) {
            if (p.kind != TrigParamKind::Function || p.sub.empty()) continue;
            const TrigFunction& f = p.sub[0];
            if (!contains(f.name, "point")) continue;
            std::vector<std::string> nums;
            for (const TrigParam& sp : f.params)
                if (sp.kind == TrigParamKind::Literal &&
                    (sp.value_type == "real" || sp.value_type == "fixed" || sp.value_type == "int"))
                    nums.push_back(sp.literal);
            if (nums.size() >= 2) {
                x = sim::Fixed::parse(nums[0]);
                y = sim::Fixed::parse(nums[1]);
                return true;
            }
        }
        return false;
    }

    Value create_units(const ResolvedNative& in, bool& handled) {
        const std::string type = gamelink(in);
        if (type.empty()) return Value::make_int(0);
        const std::vector<std::int64_t> sc = scalars(in);
        std::int64_t count = sc.size() > 0 ? sc[0] : 1;
        std::int64_t player = sc.size() > 1 ? sc[1] : 0;
        if (count < 1) count = 1;
        if (count > 64) count = 64;

        sim::Fixed x = cx_, y = cy_;
        if (!resolve_point(in, x, y)) ++report_.created_defaulted_loc;

        std::int64_t made = 0;
        for (std::int64_t k = 0; k < count; ++k) {
            sim::Unit u;
            try {
                u = app::make_unit(cat_, type, static_cast<int>(player), x, y);
            } catch (const std::exception&) {
                break;
            }
            u.alive = true;
            last_created_ = world_.add_unit(u);
            have_last_ = true;
            ++made;
        }
        if (made == 0) return Value::make_int(0);
        report_.created += static_cast<std::size_t>(made);
        handled = true;
        return Value::make_handle(static_cast<std::int64_t>(last_created_));
    }

    Value kill_unit(const ResolvedNative& in, bool& handled) {
        bool refs_last = false;
        for (const TrigParam& p : in.fn.params)
            if (p.kind == TrigParamKind::Function && !p.sub.empty() &&
                contains(p.sub[0].name, "last created unit"))
                refs_last = true;
        if (!refs_last || !have_last_) return Value::make_int(0);
        if (last_created_ < world_.units().size() && world_.unit(last_created_).alive) {
            world_.unit(last_created_).alive = false;
            ++report_.killed;
            handled = true;
        }
        return Value::make_int(0);
    }

    Value set_alliance(const ResolvedNative& in, bool& handled) {
        const std::vector<std::int64_t> sc = scalars(in);
        if (sc.size() < 2) return Value::make_int(0);
        int a = static_cast<int>(sc[0]);
        int b = static_cast<int>(sc[1]);
        if (a == b) return Value::make_int(0);
        std::string mode;
        for (const TrigParam& p : in.fn.params)
            if (p.kind == TrigParamKind::Preset) mode = lower(p.preset_name);
        if (mode.find("ally") != std::string::npos) {
            const std::int32_t team = 1 + (a < b ? a : b);
            world_.player(a).team = team;
            world_.player(b).team = team;
        } else if (mode.find("enemy") != std::string::npos || mode.find("hostile") != std::string::npos) {
            world_.player(a).team = 1000 + a;
            world_.player(b).team = 2000 + b;
        } else {
            return Value::make_int(0);
        }
        ++report_.alliance_changes;
        handled = true;
        return Value::make_int(0);
    }

    Value modify_player_property(const ResolvedNative& in, bool& handled) {
        std::string prop, op;
        for (const TrigParam& p : in.fn.params) {
            if (p.kind != TrigParamKind::Preset) continue;
            const std::string v = lower(p.preset_name);
            if (v.find("mineral") != std::string::npos || v.find("vespene") != std::string::npos ||
                v.find("gas") != std::string::npos || v.find("suppl") != std::string::npos)
                prop = v;
            else if (v.find("set") != std::string::npos || v.find("add") != std::string::npos ||
                     v.find("subtract") != std::string::npos || v.find("increment") != std::string::npos)
                op = v;
        }
        const bool is_min = prop.find("mineral") != std::string::npos;
        const bool is_gas = prop.find("vespene") != std::string::npos || prop.find("gas") != std::string::npos;
        if (!is_min && !is_gas) return Value::make_int(0);

        const std::vector<std::int64_t> sc = scalars(in);
        if (sc.size() < 2) return Value::make_int(0);
        int player = static_cast<int>(sc.front());
        std::int64_t amount = sc.back();

        sim::Player& pl = world_.player(player);
        std::int32_t& bank = is_min ? pl.minerals : pl.gas;
        if (op.find("add") != std::string::npos || op.find("increment") != std::string::npos)
            bank += static_cast<std::int32_t>(amount);
        else if (op.find("subtract") != std::string::npos)
            bank -= static_cast<std::int32_t>(amount);
        else
            bank = static_cast<std::int32_t>(amount);
        if (bank < 0) bank = 0;
        ++report_.resource_changes;
        handled = true;
        return Value::make_int(0);
    }

    const data::Catalog& cat_;
    sim::World& world_;
    MissionRunReport& report_;
    sim::Fixed cx_, cy_;
    std::uint32_t last_created_ = 0;
    bool have_last_ = false;
};

}

MissionRuntime::MissionRuntime(const data::Catalog& cat, const formats::LoadedMap& map) : cat_(cat) {
    world_ = app::build_world(map, [&cat](const std::string& type, sim::Unit& u) {
        const sim::Fixed x = u.x, y = u.y;
        const int player = u.player;
        try {
            sim::Unit m = app::make_unit(cat, type, player, x, y);
            m.alive = true;
            u = m;
        } catch (const std::exception&) {
        }
    });
}

MissionRunReport MissionRuntime::run_init(const TriggerModel& model, VmStats* vm_stats) {
    MissionRunReport report;

    const auto& units = world_.units();
    for (const sim::Unit& u : units)
        if (u.alive) {
            ++report.units_before;
            if (u.player >= 0) {
                const sim::Player& pl = world_.player(u.player);
                report.banks_before[u.player] = {pl.minerals, pl.gas};
            }
        }

    sim::Fixed cx = sim::Fixed::from_int(int(world_.nav().width()) / 2);
    sim::Fixed cy = sim::Fixed::from_int(int(world_.nav().height()) / 2);

    MissionWorldHost host(cat_, world_, report, cx, cy);
    TriggerVm vm(model);
    vm.set_limits( 500000,  64);
    vm.set_native_host(&host);
    vm.run_init();

    for (const sim::Unit& u : world_.units())
        if (u.alive) {
            ++report.units_after;
            if (u.player >= 0) {
                const sim::Player& pl = world_.player(u.player);
                report.banks_after[u.player] = {pl.minerals, pl.gas};
            }
        }
    report.wired = vm.stats().implemented;
    if (vm_stats) *vm_stats = vm.stats();
    return report;
}

}
