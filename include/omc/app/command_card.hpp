#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace omc::app {

enum class ButtonState {
    Empty,
    Enabled,
    Disabled,
    Cooldown,
};

struct CommandButton {
    std::string icon;
    std::string name;
    std::string hotkey;
    ButtonState state = ButtonState::Empty;
    float cooldown = 0;
    bool autocast = false;
    bool is_cancel = false;
    int charges = -1;

    uint32_t select_builder = 0xFFFFFFFFu;

    uint32_t select_larva = 0xFFFFFFFFu;

    bool is_passive = false;
    uint32_t ability = 0;
};

inline bool cooldown_sweep_covers(float cx, float cy, float px, float py, float frac) {
    if (frac <= 0.0f) return false;
    if (frac >= 1.0f) return true;
    const float dx = px - cx, dy = py - cy;
    if (dx == 0.0f && dy == 0.0f) return true;
    float a = std::atan2(dx, -dy);
    if (a < 0.0f) a += 6.28318530718f;
    return a >= (1.0f - frac) * 6.28318530718f;
}

inline ButtonState resolve_button_state(bool requirement_met, float cooldown_frac) {
    if (!requirement_met) return ButtonState::Disabled;
    if (cooldown_frac > 0.0f) return ButtonState::Cooldown;
    return ButtonState::Enabled;
}

struct CardEntry {
    int slot = 0;
    std::string icon, name, hotkey;
    uint32_t ability = 0;
    bool requirement_met = true;
    float cooldown = 0;
    bool autocast = false;
};

struct CommandCard {
    static constexpr int kCols = 5;
    static constexpr int kRows = 3;
    static constexpr int kSlots = kCols * kRows;
    static constexpr int kCancelSlot = kSlots - 1;

    CommandButton slots[kSlots];

    void clear() {
        for (CommandButton& b : slots) b = CommandButton{};
    }
    CommandButton& at(int col, int row) { return slots[row * kCols + col]; }
    const CommandButton& at(int col, int row) const { return slots[row * kCols + col]; }

    void set(int slot, const CommandButton& b) {
        if (slot >= 0 && slot < kSlots) slots[slot] = b;
    }
    void set_cancel(const std::string& icon) {
        CommandButton b;
        b.icon = icon;
        b.name = "Cancel";
        b.hotkey = "Esc";
        b.state = ButtonState::Enabled;
        b.is_cancel = true;
        slots[kCancelSlot] = b;
    }

    void fill(const std::vector<CardEntry>& entries) {
        clear();
        for (const CardEntry& e : entries) {
            if (e.slot < 0 || e.slot >= kSlots) continue;
            CommandButton b;
            b.icon = e.icon;
            b.name = e.name;
            b.hotkey = e.hotkey;
            b.ability = e.ability;
            b.cooldown = e.cooldown;
            b.autocast = e.autocast;
            b.state = resolve_button_state(e.requirement_met, e.cooldown);
            slots[e.slot] = b;
        }
    }

    int count() const {
        int n = 0;
        for (const CommandButton& b : slots)
            if (b.state != ButtonState::Empty) ++n;
        return n;
    }
};

}
