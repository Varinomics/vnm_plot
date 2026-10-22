#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vnm::plot::detail {

template <typename Labels, typename Tracker, typename KeyFunc, typename DrawFunc>
bool update_and_draw_faded_labels(
    const Labels&                          labels,
    Tracker&                               tracker,
    float                                  fade_duration_ms,
    bool                                   animate_changes,
    std::chrono::steady_clock::time_point  now,
    KeyFunc&&                              key_fn,
    DrawFunc&&                             draw_fn,
    bool                                   suppress_consecutive_equal_text = false,
    bool                                   labels_left_to_right = true)
{
    using key_t    = typename Tracker::key_type;
    using states_t = std::remove_reference_t<decltype(tracker.states)>;
    using state_t  = typename states_t::mapped_type;

    if (!tracker.initialized) {
        tracker.states.clear();
        tracker.visible_alphas.clear();
        for (const auto& label : labels) {
            state_t state;
            state.alpha     = 1.0f;
            state.direction = 0;
            state.text      = label.text;
            tracker.states.emplace(key_fn(label), std::move(state));
        }
        tracker.last_update = now;
        tracker.initialized = true;
        for (const auto& entry : tracker.states) {
            draw_fn(entry.first, entry.second.text, entry.second.alpha);
            tracker.visible_alphas.emplace(entry.first, entry.second.alpha);
        }
        return false;
    }

    const float dt_ms = std::chrono::duration_cast< std::chrono::duration<float, std::milli>>(
        now - tracker.last_update
    ).count();
    tracker.last_update = now;
    const float step = fade_duration_ms > 0.0f
        ? std::clamp(dt_ms / fade_duration_ms, 0.0f, 1.0f)
        : 1.0f;

    constexpr float k_alpha_eps = 1e-6f;
    for (auto it = tracker.states.begin(); it != tracker.states.end(); ) {
        auto& state = it->second;
        if (state.direction != 0 && step > 0.0f) {
            state.alpha += step * static_cast<float>(state.direction);
        }
        state.alpha = std::clamp(state.alpha, 0.0f, 1.0f);

        if (state.direction > 0 && state.alpha >= 1.0f - k_alpha_eps) {
            state.alpha     = 1.0f;
            state.direction = 0;
        }
        if (state.direction < 0 && state.alpha <= k_alpha_eps) {
            it = tracker.states.erase(it);
            continue;
        }
        if (state.text_mix < 1.0f && step > 0.0f) {
            state.text_mix = std::min(state.text_mix + step, 1.0f);
            if (state.text_mix >= 1.0f - k_alpha_eps) {
                state.text_mix = 1.0f;
                state.previous_text.clear();
            }
        }
        ++it;
    }

    std::unordered_set<key_t> current_values;
    current_values.reserve(labels.size());
    for (const auto& label : labels) {
        const key_t key = key_fn(label);
        current_values.insert(key);

        auto it = tracker.states.find(key);
        if (it == tracker.states.end()) {
            state_t state;
            state.alpha     = animate_changes ? 0.0f : 1.0f;
            state.direction = animate_changes ? 1 : 0;
            state.text      = label.text;
            tracker.states.emplace(key, std::move(state));
            continue;
        }

        auto& state = it->second;
        if (state.text != label.text) {
            if (animate_changes) {
                if (state.text_mix < 1.0f && state.previous_text == label.text) {
                    std::swap(state.text, state.previous_text);
                    state.text_mix = 1.0f - state.text_mix;
                }
                else {
                    state.previous_text = std::move(state.text);
                    state.text          = label.text;
                    state.text_mix      = 0.0f;
                }
            }
            else {
                state.text          = label.text;
                state.previous_text.clear();
                state.text_mix      = 1.0f;
            }
        }
        if (state.direction < 0) {
            if (animate_changes) {
                state.direction = 1;
            }
            else {
                state.alpha     = 1.0f;
                state.direction = 0;
            }
        }
    }

    for (auto it = tracker.states.begin(); it != tracker.states.end(); ) {
        auto& state = it->second;
        if (current_values.find(it->first) == current_values.end() && state.direction >= 0) {
            if (animate_changes) {
                state.direction = -1;
            }
            else {
                it = tracker.states.erase(it);
                continue;
            }
        }
        ++it;
    }

    struct emitted_text_t
    {
        const std::string* text       = nullptr;
        float              alpha      = 0.0f;
        bool               primary    = false;
        bool               suppressed = false;
    };
    class Emitted_texts
    {
    public:
        void push_back(emitted_text_t text) { m_values[m_count++] = text; }
        auto begin()                       { return m_values.begin(); }
        auto end()                         { return m_values.begin() + m_count; }
        auto begin() const                 { return m_values.begin(); }
        auto end() const                   { return m_values.begin() + m_count; }

    private:
        // A label only emits its previous and current text during a crossfade.
        std::array<emitted_text_t, 2> m_values{};
        std::size_t                  m_count = 0;
    };
    struct emitted_state_t
    {
        key_t                       key;
        const std::string*          final_text = nullptr;
        Emitted_texts               texts;
    };

    bool                         any_active = false;
    std::vector<emitted_state_t> emitted_states;
    emitted_states.reserve(tracker.states.size());
    const auto collect_state = [&](const auto& entry) {
        const auto& state = entry.second;
        emitted_state_t emitted{entry.first, &state.text, {}};
        const auto collect_text = [&](
            const std::string& text,
            float              alpha,
            bool               primary)
        {
            if (alpha > 0.0f) {
                emitted.texts.push_back({&text, alpha, primary, false});
            }
        };

        if (state.alpha > 0.0f) {
            const float previous_alpha = state.alpha * (1.0f - state.text_mix);
            collect_text(state.previous_text, previous_alpha, false);
            const float current_alpha = state.alpha * state.text_mix;
            collect_text(state.text, current_alpha, true);
        }
        any_active |= state.direction != 0 || state.text_mix < 1.0f;
        emitted_states.push_back(std::move(emitted));
    };
    if (suppress_consecutive_equal_text && !labels_left_to_right) {
        for (auto it = tracker.states.rbegin(); it != tracker.states.rend(); ++it) {
            collect_state(*it);
        }
    }
    else {
        for (const auto& entry : tracker.states) {
            collect_state(entry);
        }
    }

    if (suppress_consecutive_equal_text) {
        for (std::size_t first = 0; first < emitted_states.size(); ) {
            std::size_t last = first + 1;
            while (last                           <  emitted_states.size() &&
                *emitted_states[first].final_text == *emitted_states[last].final_text)
            {
                ++last;
            }

            emitted_text_t* retained = nullptr;
            for (std::size_t i = first; i < last; ++i) {
                auto primary = std::find_if(
                    emitted_states[i].texts.begin(),
                    emitted_states[i].texts.end(),
                    [](const emitted_text_t& candidate) {
                        return candidate.primary;
                    });
                if (primary == emitted_states[i].texts.end()) { continue;             }
                if (!retained)                                { retained = &*primary; }
                else {
                    retained->alpha = std::max(retained->alpha, primary->alpha);
                    primary->suppressed = true;
                }
            }
            first = last;
        }

        const auto find_primary = [&](std::size_t i, const std::string& text) {
            auto primary = std::find_if(
                emitted_states[i].texts.begin(),
                emitted_states[i].texts.end(),
                [&](const emitted_text_t& candidate) {
                    return
                        candidate.primary     &&
                        !candidate.suppressed &&
                        *candidate.text == text;
                });
            return primary == emitted_states[i].texts.end()
                ? static_cast<emitted_text_t*>(nullptr)
                : &*primary;
        };
        for (std::size_t i = 0; i < emitted_states.size(); ++i) {
            for (auto& candidate : emitted_states[i].texts) {
                if (candidate.primary) {
                    continue;
                }
                emitted_text_t* adjacent_primary = i > 0
                    ? find_primary(i - 1, *candidate.text)
                    : nullptr;
                if (!adjacent_primary && i + 1 < emitted_states.size()) {
                    adjacent_primary = find_primary(i + 1, *candidate.text);
                }
                if (adjacent_primary) {
                    adjacent_primary->alpha =
                        std::max(adjacent_primary->alpha, candidate.alpha);
                    candidate.suppressed = true;
                }
            }
        }

        for (std::size_t i = 0; i < emitted_states.size(); ++i) {
            for (auto& retained_previous : emitted_states[i].texts) {
                if (retained_previous.primary || retained_previous.suppressed) {
                    continue;
                }
                for (std::size_t j = i + 1; j < emitted_states.size(); ++j) {
                    auto duplicate = std::find_if(
                        emitted_states[j].texts.begin(),
                        emitted_states[j].texts.end(),
                        [&](const emitted_text_t& candidate) {
                            return
                                !candidate.primary    &&
                                !candidate.suppressed &&
                                *candidate.text == *retained_previous.text;
                        });
                    if (duplicate == emitted_states[j].texts.end()) {
                        break;
                    }
                    retained_previous.alpha =
                        std::max(retained_previous.alpha, duplicate->alpha);
                    duplicate->suppressed = true;
                }
            }
        }
    }

    tracker.visible_alphas.clear();
    for (const auto& emitted : emitted_states) {
        float visible_alpha = 0.0f;
        for (const auto& text : emitted.texts) {
            if (!text.suppressed) {
                draw_fn(emitted.key, *text.text, text.alpha);
                visible_alpha = std::min(1.0f, visible_alpha + text.alpha);
            }
        }
        if (visible_alpha > 0.0f) {
            tracker.visible_alphas.emplace(emitted.key, visible_alpha);
        }
    }
    return any_active;
}

} // namespace vnm::plot::detail
