// vnm_plot typed API tests

#include "test_macros.h"

#include <vnm_plot/core/access_policy.h>
#include <vnm_plot/core/series_builder.h>
#include <vnm_plot/core/types.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace plot = vnm::plot;

namespace {

struct sample_t
{
    // Timestamps are int64 nanoseconds (API convention).
    std::int64_t   t;
    float          v;
    std::int32_t   pad;
    float          v_min;
    float          v_max;
};

struct default_member_initializer_sample_t
{
    std::int64_t   t = 0;
    float          v = 0.0f;
};

struct Non_trivial_sample
{
    Non_trivial_sample()
    :
        t(0),
        v(0.0f)
    {}

    std::int64_t   t;
    float          v;
};

struct sample_base_t
{
    std::int64_t   t;
};

struct Non_standard_layout_sample : sample_base_t
{
    float          v;
};

static_assert(plot::detail::supports_member_pointer_access_v<sample_t>,
    "plain sample_t should support member-pointer access");
static_assert(!plot::detail::supports_member_pointer_access_v<default_member_initializer_sample_t>,
    "default member initializers are unsupported by member-pointer access");
static_assert(!plot::detail::supports_member_pointer_access_v<Non_trivial_sample>,
    "custom default constructors are unsupported by member-pointer access");
static_assert(!plot::detail::supports_member_pointer_access_v<Non_standard_layout_sample>,
    "non-standard-layout samples are unsupported by member-pointer access");
static_assert(std::is_same_v<
    decltype(std::declval<plot::Data_access_policy&>().set_semantics_key(
        std::uint64_t{0x43414C4C}, std::uint64_t{2})),
    plot::Data_access_policy&>);
static_assert(std::is_same_v<
    decltype(std::declval<plot::Data_access_policy_typed<sample_t>&>().
        set_semantics_key(std::uint64_t{0x54595045}, std::uint64_t{3})),
    plot::Data_access_policy_typed<sample_t>&>);

bool test_fnv64_word_hash()
{
    // RFC 9923 basis/prime; independent byte-wise reference over ASCII "policies".
    TEST_ASSERT(plot::detail::fnv1a_mix(
        plot::detail::k_fnv_offset_basis, 0x73656963696c6f70ULL) == 0x22d1fbc6bf4bd197ULL,
        "FNV-1a word mixing should use the standard 64-bit basis and little-endian bytes");
    return true;
}

const plot::Data_access_policy& erased_policy(const plot::Data_access_policy& policy)
{
    return policy;
}

template<typename Sample>
plot::Data_access_policy erased_policy(const plot::Data_access_policy_typed<Sample>& policy)
{
    return policy.erase();
}

template<typename Policy>
bool check_policy_transfer_ownership(const Policy& original)
{
    const sample_t sample{12'500'000'000, 3.5f, 0, 1.0f, 6.0f};
    for (int changed_slot = 0; changed_slot < 3; ++changed_slot) {
        Policy source(original);
        Policy copied(source);
        Policy moved(std::move(source));
        Policy assigned{};
        Policy move_assigned{};
        assigned = source;
        move_assigned = std::move(source);
        Policy* destinations[] = {&copied, &moved, &assigned, &move_assigned};
        for (Policy* destination : destinations) {
            TEST_ASSERT(destination->is_valid(), "transferred policy should remain valid");
            TEST_ASSERT(destination->layout_key == original.layout_key &&
                destination->semantics_key.value == original.semantics_key.value &&
                destination->semantics_key.revision == original.semantics_key.revision &&
                !destination->semantics_key.conservative,
                "copy/move should preserve layout and explicit semantics");
            const auto& before = erased_policy(*destination);
            const auto before_view = plot::detail::make_erased_access_policy_view(before);
            const auto before_key = plot::detail::make_access_policy_cache_key(&before, before_view);
            TEST_ASSERT(before_view.dispatch_kind == plot::detail::access_dispatch_kind_t::MEMBER_POINTER,
                "copy/move should preserve member-pointer dispatch");

            if (changed_slot == 0) {
                destination->get_timestamp = [](const auto&) { return std::int64_t{13'000'000'000}; };
            }
            else
            if (changed_slot == 1) {
                destination->get_value = [](const auto&) { return 8.5f; };
            }
            else {
                destination->get_range = [](const auto&) { return std::make_pair(2.0f, 9.0f); };
            }

            TEST_ASSERT(destination->semantics_key.conservative && destination->semantics_key.value == 0,
                "each transferred slot must invalidate its destination semantics");
            const auto& after = erased_policy(*destination);
            const auto after_view = plot::detail::make_erased_access_policy_view(after);
            TEST_ASSERT(after_view.dispatch_kind == plot::detail::access_dispatch_kind_t::STD_FUNCTION,
                "each transferred slot must invalidate its destination fast path");
            TEST_ASSERT(after_view.timestamp(&sample) ==
                (changed_slot == 0 ? 13'000'000'000 : sample.t),
                "transferred timestamp slot should call the destination accessor");
            TEST_ASSERT(after_view.value(&sample) == (changed_slot == 1 ? 8.5f : sample.v),
                "transferred value slot should call the destination accessor");
            const auto expected_range = changed_slot == 2
                ? std::make_pair(2.0f, 9.0f)
                : std::make_pair(sample.v_min, sample.v_max);
            TEST_ASSERT(after_view.range(&sample) == expected_range,
                "transferred range slot should call the destination accessor");
            if constexpr (std::is_same_v<Policy, plot::Data_access_policy>) {
                const auto after_key = plot::detail::make_access_policy_cache_key(&after, after_view);
                TEST_ASSERT(after_key.revision > before_key.revision,
                    "transferred slot mutation must advance its destination cache revision");
            }
            TEST_ASSERT(!source.semantics_key.conservative &&
                source.semantics_key.value == original.semantics_key.value,
                "destination mutation must not invalidate the copy/move source");
            const auto& erased_source = erased_policy(source);
            const auto source_view = plot::detail::make_erased_access_policy_view(erased_source);
            TEST_ASSERT(source_view.dispatch_kind == plot::detail::access_dispatch_kind_t::MEMBER_POINTER &&
                source_view.timestamp(&sample) == sample.t && source_view.value(&sample) == sample.v &&
                source_view.range(&sample) == std::make_pair(sample.v_min, sample.v_max),
                "destination mutation must leave source accessors and fast path intact");
        }
    }
    return true;
}

bool test_policy_copy_move_ownership()
{
    auto typed = plot::make_access_policy<sample_t>(
        &sample_t::t, &sample_t::v, &sample_t::v_min, &sample_t::v_max);
    typed.set_semantics_key(0x53414D504C45, 7);
    TEST_ASSERT(check_policy_transfer_ownership(typed), "typed policy transfers should own their slots");
    TEST_ASSERT(check_policy_transfer_ownership(typed.erase()), "erased policy transfers should own their slots");
    return true;
}

bool test_policy_assignment_preserves_destination_revision()
{
    const auto typed = plot::make_access_policy<sample_t>(&sample_t::t, &sample_t::v);
    auto source = typed.erase();
    auto destination = source;
    for (int i = 0; i < 12; ++i) {
        destination.get_value = [](const void*) { return 8.5f; };
    }
    auto cache_key = [](const plot::Data_access_policy& policy) {
        return plot::detail::make_access_policy_cache_key(
            &policy, plot::detail::make_erased_access_policy_view(policy));
    };
    const auto before_copy = cache_key(destination);
    destination = source;
    const auto after_copy = cache_key(destination);
    TEST_ASSERT(after_copy.revision > before_copy.revision,
        "copy assignment must advance destination revision even when source revision is older");
    destination = std::move(source);
    const auto after_move = cache_key(destination);
    TEST_ASSERT(after_move.revision > after_copy.revision,
        "move assignment must advance destination revision");
    destination = destination;
    TEST_ASSERT(cache_key(destination) == after_move, "self-copy must preserve cache identity");
    destination = std::move(destination);
    TEST_ASSERT(cache_key(destination) == after_move, "self-move must preserve cache identity");
    return true;
}

bool test_member_offset_matches_offsetof()
{
    TEST_ASSERT(plot::detail::member_offset(&sample_t::t) == offsetof(sample_t, t),
        "member_offset mismatch for t");
    TEST_ASSERT(plot::detail::member_offset(&sample_t::v) == offsetof(sample_t, v),
        "member_offset mismatch for v");
    TEST_ASSERT(plot::detail::member_offset(&sample_t::v_min) == offsetof(sample_t, v_min),
        "member_offset mismatch for v_min");
    TEST_ASSERT(plot::detail::member_offset(&sample_t::v_max) == offsetof(sample_t, v_max),
        "member_offset mismatch for v_max");
    return true;
}

bool test_layout_key_distinguishes_sample_types()
{
    // Cache key is the only renderer-visible identifier of a user sample
    // type now that the byte-level layout is no longer surfaced. Two
    // structurally different sample shapes must produce distinct, non-zero
    // keys so the renderer's caches do not collide across types.
    struct other_sample_t
    {
        std::int64_t   t;
        float          v;
    };

    const auto policy_a = plot::make_access_policy<sample_t>(
        &sample_t::t,
        &sample_t::v,
        &sample_t::v_min,
        &sample_t::v_max);
    const auto policy_b = plot::make_access_policy<other_sample_t>(
        &other_sample_t::t, &other_sample_t::v);

    TEST_ASSERT(policy_a.layout_key != 0, "layout_key should be non-zero");
    TEST_ASSERT(policy_b.layout_key != 0, "layout_key should be non-zero");
    TEST_ASSERT(policy_a.layout_key != policy_b.layout_key,
        "layout_key should differ for differently shaped sample types");

    const auto policy_a2 = plot::make_access_policy<sample_t>(
        &sample_t::t,
        &sample_t::v,
        &sample_t::v_min,
        &sample_t::v_max);
    TEST_ASSERT(policy_a.layout_key == policy_a2.layout_key,
        "layout_key should be deterministic for the same sample shape");

    return true;
}

bool test_member_pointer_semantics_key_is_distinct_from_layout_key()
{
    struct int_timestamp_sample_t
    {
        std::int64_t   t;
        float          v;
    };

    struct fp_timestamp_sample_t
    {
        double         t_seconds;
        float          v;
    };

    const auto int_policy = plot::make_access_policy<int_timestamp_sample_t>(
        &int_timestamp_sample_t::t,
        &int_timestamp_sample_t::v);
    const auto fp_policy = plot::make_access_policy<fp_timestamp_sample_t>(
        &fp_timestamp_sample_t::t_seconds,
        &fp_timestamp_sample_t::v);

    TEST_ASSERT(int_policy.layout_key == fp_policy.layout_key,
        "layout_key should describe byte layout, not full accessor semantics");
    TEST_ASSERT(!int_policy.semantics_key.conservative,
        "member-pointer policies should produce stable semantics keys");
    TEST_ASSERT(!fp_policy.semantics_key.conservative,
        "member-pointer policies should produce stable semantics keys");
    TEST_ASSERT(int_policy.semantics_key.value != 0,
        "member-pointer semantics key should be non-zero");
    TEST_ASSERT(fp_policy.semantics_key.value != 0,
        "member-pointer semantics key should be non-zero");
    TEST_ASSERT(int_policy.semantics_key.value != int_policy.layout_key,
        "semantics key must not reuse layout identity");
    TEST_ASSERT(int_policy.semantics_key.value != fp_policy.semantics_key.value,
        "semantics key should distinguish integer-ns and floating-second timestamp transforms");

    const plot::Data_access_policy erased = int_policy.erase();
    const plot::sample_semantics_key_t effective_key =
        plot::detail::make_sample_semantics_key(&erased);
    TEST_ASSERT(!effective_key.conservative,
        "erased member-pointer policy should keep stable semantics");
    TEST_ASSERT(effective_key.value == int_policy.semantics_key.value,
        "erase() should propagate member-pointer semantics");
    TEST_ASSERT(effective_key.revision == 0,
        "stable member-pointer semantics should not consume accessor revision");

    return true;
}

bool test_make_access_policy_and_erase()
{
    auto policy = plot::make_access_policy<sample_t>(
        &sample_t::t,
        &sample_t::v,
        &sample_t::v_min,
        &sample_t::v_max);

    TEST_ASSERT(policy.is_valid(), "expected typed policy to be valid");

    sample_t s{};
    // 12.5 seconds in nanoseconds. The typed API works in int64 ns; this
    // single literal stands for what the previous double-keyed test wrote
    // as 12.5.
    constexpr std::int64_t k_test_ts_ns = 12'500'000'000;
    s.t     = k_test_ts_ns;
    s.v     = 3.5f;
    s.v_min = 1.0f;
    s.v_max = 6.0f;

    TEST_ASSERT(policy.get_timestamp(s) == k_test_ts_ns, "timestamp accessor mismatch");
    TEST_ASSERT(policy.get_value(s) == 3.5f,             "value accessor mismatch");
    const auto range = policy.get_range(s);
    TEST_ASSERT(range.first == 1.0f && range.second == 6.0f, "range accessor mismatch");

    TEST_ASSERT(policy.layout_key != 0,
        "make_access_policy should populate a non-zero layout_key");

    const plot::Data_access_policy erased = policy.erase();
    TEST_ASSERT(erased.layout_key == policy.layout_key,
        "erase() should propagate layout_key");
    TEST_ASSERT(erased.get_timestamp(&s) == k_test_ts_ns, "erased timestamp accessor mismatch");
    TEST_ASSERT(erased.get_value(&s) == 3.5f,             "erased value accessor mismatch");
    const auto erased_range = erased.get_range(&s);
    TEST_ASSERT(erased_range.first == 1.0f && erased_range.second == 6.0f,
        "erased range accessor mismatch");

    return true;
}

bool test_callable_semantics_key_is_conservative_until_explicit()
{
    plot::Data_access_policy callable;
    callable.get_timestamp = [](const void* sample) -> std::int64_t {
        return static_cast<const sample_t*>(sample)->t;
    };
    callable.get_value = [](const void* sample) {
        return static_cast<const sample_t*>(sample)->v;
    };
    callable.layout_key = 0x43414C4C;

    const plot::sample_semantics_key_t conservative_key =
        plot::detail::make_sample_semantics_key(&callable);
    TEST_ASSERT(conservative_key.conservative,
        "callable policies should default to conservative semantics");
    TEST_ASSERT(conservative_key.value == 0,
        "conservative callable semantics must not reuse layout identity");
    TEST_ASSERT(conservative_key.revision != 0,
        "conservative callable semantics should carry accessor revision");

    callable.set_semantics_key(0x535441424C45, 7);
    const plot::sample_semantics_key_t explicit_key =
        plot::detail::make_sample_semantics_key(&callable);
    TEST_ASSERT(!explicit_key.conservative,
        "explicit non-zero callable semantics should be accepted");
    TEST_ASSERT(explicit_key.value == 0x535441424C45 && explicit_key.revision == 7,
        "explicit callable semantics should preserve caller-maintained key and revision");

    callable.set_semantics_key(0x535441424C45, 8);
    const plot::sample_semantics_key_t revised_key =
        plot::detail::make_sample_semantics_key(&callable);
    TEST_ASSERT(revised_key.value == explicit_key.value && revised_key.revision == 8,
        "explicit callable semantics revision should be caller-updatable");

    callable.get_value = [](const void*) {
        return 12.0f;
    };
    const plot::sample_semantics_key_t mutated_key =
        plot::detail::make_sample_semantics_key(&callable);
    TEST_ASSERT(mutated_key.conservative,
        "mutating a callable accessor should clear explicit semantics until reset");
    TEST_ASSERT(mutated_key.value == 0 && mutated_key.revision > conservative_key.revision,
        "mutated callable semantics should conservatively advance revision");

    return true;
}

bool test_explicit_semantics_key_helpers()
{
    plot::Data_access_policy first;
    first.set_semantics_key(0x5052494345, 2);
    const plot::sample_semantics_key_t first_key =
        plot::detail::make_sample_semantics_key(&first);
    TEST_ASSERT(!first_key.conservative && first_key.value != 0,
        "non-zero semantics key should produce explicit stable semantics");
    TEST_ASSERT(first_key.revision == 2,
        "explicit semantics key should preserve caller revision");

    plot::Data_access_policy second;
    second.set_semantics_key(0x5052494345, 2);
    const plot::sample_semantics_key_t second_key =
        plot::detail::make_sample_semantics_key(&second);
    TEST_ASSERT(second_key.value == first_key.value,
        "identical semantics key values should produce identical semantics");

    first.set_semantics_key(0, 9);
    const plot::sample_semantics_key_t zero_key =
        plot::detail::make_sample_semantics_key(&first);
    TEST_ASSERT(zero_key.conservative && zero_key.value == 0,
        "zero semantics key should restore conservative semantics");

    auto typed = plot::make_access_policy<sample_t>(&sample_t::t, &sample_t::v);
    typed.set_semantics_key(0x54595045, 5);
    const plot::Data_access_policy erased = typed.erase();
    const plot::sample_semantics_key_t typed_key =
        plot::detail::make_sample_semantics_key(&erased);
    TEST_ASSERT(!typed_key.conservative && typed_key.value != 0 &&
        typed_key.revision == 5,
        "typed policy explicit semantics should survive erase()");

    typed.get_value = [](const sample_t&) {
        return 42.0f;
    };
    TEST_ASSERT(typed.semantics_key.conservative,
        "mutating a typed policy should clear explicit semantics");

    return true;
}

bool test_erased_access_view_uses_direct_member_accessors()
{
    auto policy = plot::make_access_policy<sample_t>(
        &sample_t::t,
        &sample_t::v,
        &sample_t::v_min,
        &sample_t::v_max);

    sample_t s{};
    constexpr std::int64_t k_sample_timestamp_ns = 15'000'000'000;
    s.t     = k_sample_timestamp_ns;
    s.v     = 3.5f;
    s.v_min = 1.25f;
    s.v_max = 5.5f;

    const plot::Data_access_policy erased = policy.erase();
    const auto direct_view = plot::detail::make_erased_access_policy_view(erased);
    TEST_ASSERT(
        direct_view.dispatch_kind == plot::detail::access_dispatch_kind_t::MEMBER_POINTER,
        "member-pointer policy should expose a direct erased access view");
    TEST_ASSERT(direct_view.timestamp(&s) == k_sample_timestamp_ns,
        "direct erased timestamp accessor mismatch");
    TEST_ASSERT(direct_view.value(&s) == 3.5f,
        "direct erased value accessor mismatch");
    const auto direct_range = direct_view.range(&s);
    TEST_ASSERT(direct_range.first == 1.25f && direct_range.second == 5.5f,
        "direct erased range accessor mismatch");

    int timestamp_calls = 0;
    plot::Data_access_policy fallback;
    fallback.get_timestamp = [&timestamp_calls](const void* sample) {
        ++timestamp_calls;
        return static_cast<const sample_t*>(sample)->t;
    };
    fallback.get_value = [](const void* sample) {
        return static_cast<const sample_t*>(sample)->v;
    };
    const auto fallback_view =
        plot::detail::make_erased_access_policy_view(fallback);
    TEST_ASSERT(
        fallback_view.dispatch_kind == plot::detail::access_dispatch_kind_t::STD_FUNCTION,
        "capturing callable policy should expose a std::function fallback view");
    TEST_ASSERT(fallback_view.timestamp(&s) == k_sample_timestamp_ns,
        "fallback erased timestamp accessor mismatch");
    TEST_ASSERT(timestamp_calls == 1,
        "fallback erased accessor should invoke the public callable once");

    plot::Data_access_policy mutated                 = erased;
    int                      mutated_timestamp_calls = 0;
    int                      mutated_value_calls     = 0;
    int                      mutated_range_calls     = 0;

    constexpr std::int64_t k_replacement_timestamp_ns = 21'000'000'000;
    mutated.get_timestamp = [&mutated_timestamp_calls](const void*) {
        ++mutated_timestamp_calls;
        return k_replacement_timestamp_ns;
    };
    mutated.get_value = [&mutated_value_calls](const void*) {
        ++mutated_value_calls;
        return 9.25f;
    };
    mutated.get_range = [&mutated_range_calls](const void*) {
        ++mutated_range_calls;
        return std::make_pair(7.0f, 8.0f);
    };

    const auto mutated_view =
        plot::detail::make_erased_access_policy_view(mutated);
    TEST_ASSERT(
        mutated_view.dispatch_kind == plot::detail::access_dispatch_kind_t::STD_FUNCTION,
        "mutating an erased direct policy should clear member-pointer dispatch");
    TEST_ASSERT(mutated_view.timestamp(&s) == k_replacement_timestamp_ns,
        "mutated erased timestamp accessor should use the replacement callable");
    TEST_ASSERT(mutated_view.value(&s) == 9.25f,
        "mutated erased value accessor should use the replacement callable");
    const auto mutated_range = mutated_view.range(&s);
    TEST_ASSERT(mutated_range.first == 7.0f && mutated_range.second == 8.0f,
        "mutated erased range accessor should use the replacement callable");
    TEST_ASSERT(mutated_timestamp_calls == 1 &&
        mutated_value_calls == 1 &&
        mutated_range_calls == 1,
        "mutated erased policy should invoke each replacement callable once");
    TEST_ASSERT(mutated.semantics_key.conservative,
        "mutating an erased direct policy should clear member-pointer semantics");

    plot::Data_access_policy slot_source;
    int slot_source_timestamp_calls = 0;
    slot_source.get_timestamp = [&slot_source_timestamp_calls](const void*) {
        ++slot_source_timestamp_calls;
        return std::int64_t{22'000'000'000};
    };
    slot_source.get_value = [](const void*) {
        return 10.5f;
    };

    plot::Data_access_policy slot_assigned = erased;
    slot_assigned.get_timestamp = slot_source.get_timestamp;
    slot_assigned.get_value     = slot_source.get_value;
    const auto slot_assigned_view =
        plot::detail::make_erased_access_policy_view(slot_assigned);
    TEST_ASSERT(
        slot_assigned_view.dispatch_kind ==
            plot::detail::access_dispatch_kind_t::STD_FUNCTION,
        "assigning accessor slots should clear member-pointer dispatch");
    TEST_ASSERT(slot_assigned_view.timestamp(&s) == std::int64_t{22'000'000'000},
        "slot-assigned policy should use the replacement timestamp slot");
    TEST_ASSERT(slot_assigned_view.value(&s) == 10.5f,
        "slot-assigned policy should use the replacement value slot");
    TEST_ASSERT(slot_source_timestamp_calls == 1,
        "slot-assigned policy should invoke the replacement timestamp slot");

    auto detached_timestamp_slot = erased.get_timestamp;
    detached_timestamp_slot = [](const void*) {
        return std::int64_t{23'000'000'000};
    };
    const auto after_detached_slot_mutation_view =
        plot::detail::make_erased_access_policy_view(erased);
    TEST_ASSERT(
        after_detached_slot_mutation_view.dispatch_kind ==
            plot::detail::access_dispatch_kind_t::MEMBER_POINTER,
        "mutating a detached accessor copy must not clear the source policy");
    TEST_ASSERT(after_detached_slot_mutation_view.timestamp(&s) ==
        k_sample_timestamp_ns,
        "detached accessor mutation must not affect source policy semantics");

    plot::Data_access_policy move_source          = erased;
    auto                     moved_timestamp_slot = std::move(move_source.get_timestamp);
    (void)moved_timestamp_slot;
    const auto after_slot_move_view =
        plot::detail::make_erased_access_policy_view(move_source);
    TEST_ASSERT(
        after_slot_move_view.dispatch_kind !=
            plot::detail::access_dispatch_kind_t::MEMBER_POINTER,
        "moving an accessor slot out should clear the source policy fast path");

    plot::Data_access_policy whole_move_source = erased;
    plot::Data_access_policy whole_move_destination =
        std::move(whole_move_source);
    const auto after_whole_policy_move_view =
        plot::detail::make_erased_access_policy_view(whole_move_destination);
    TEST_ASSERT(
        after_whole_policy_move_view.dispatch_kind ==
            plot::detail::access_dispatch_kind_t::MEMBER_POINTER,
        "moving a whole policy should preserve destination member-pointer dispatch");
    TEST_ASSERT(after_whole_policy_move_view.timestamp(&s) ==
        k_sample_timestamp_ns,
        "whole-policy move should preserve direct timestamp semantics");

    auto typed_mutated                 = policy;
    int  typed_mutated_timestamp_calls = 0;
    typed_mutated.get_timestamp = [&typed_mutated_timestamp_calls](
        const sample_t& sample) {
        ++typed_mutated_timestamp_calls;
        return sample.t + std::int64_t{1};
    };
    const plot::Data_access_policy erased_typed_mutated =
        typed_mutated.erase();
    const auto typed_mutated_view =
        plot::detail::make_erased_access_policy_view(erased_typed_mutated);
    TEST_ASSERT(
        typed_mutated_view.dispatch_kind ==
            plot::detail::access_dispatch_kind_t::STD_FUNCTION,
        "mutating a typed policy should clear member-pointer dispatch before erase");
    TEST_ASSERT(typed_mutated_view.timestamp(&s) == k_sample_timestamp_ns + 1,
        "typed policy mutation should survive erase through std::function dispatch");
    TEST_ASSERT(typed_mutated_timestamp_calls == 1,
        "typed policy mutation should invoke the replacement callable");

    plot::Data_access_policy revision_policy = erased;
    const auto revision_view_before =
        plot::detail::make_erased_access_policy_view(revision_policy);
    const auto revision_key_before = plot::detail::make_access_policy_cache_key(
        &revision_policy,
        revision_view_before);
    revision_policy.get_value = [](const void*) {
        return 11.0f;
    };
    const auto revision_view_after =
        plot::detail::make_erased_access_policy_view(revision_policy);
    const auto revision_key_after = plot::detail::make_access_policy_cache_key(
        &revision_policy,
        revision_view_after);
    TEST_ASSERT(revision_key_before != revision_key_after,
        "access policy cache key should change after accessor mutation");

    auto semantic_mutated      = policy;
    semantic_mutated.get_value = [](const sample_t&) {
        return 13.0f;
    };
    TEST_ASSERT(semantic_mutated.semantics_key.conservative,
        "mutating a typed member-pointer policy should clear stable semantics");
    const plot::Data_access_policy erased_semantic_mutated =
        semantic_mutated.erase();
    const plot::sample_semantics_key_t semantic_mutated_key = plot::detail::make_sample_semantics_key(
        &erased_semantic_mutated);
    TEST_ASSERT(semantic_mutated_key.conservative,
        "erasing a mutated typed policy should expose conservative semantics");

    return true;
    }

bool test_typed_api_floating_point_timestamp_member()
{
    // Some legacy / convenience sample types store the timestamp as a
    // floating-point value in seconds. The vnm_plot API works in int64
    // nanoseconds, so the typed API must auto-convert at the boundary
    // instead of silently truncating with static_cast<int64_t>(double).
    //
    // The original failure mode this test guards against: a Sample with
    // double t = 12.5 (seconds) used to produce get_timestamp() == 12 (ns)
    // because of a blind static_cast. Anything that constructed a view
    // window in seconds (-10 .. 10) ended up with a 20-ns view instead.

    struct fp_sample_t
    {
        double t_seconds;
        float  v;
    };

    auto policy = plot::make_access_policy<fp_sample_t>(
        &fp_sample_t::t_seconds,
        &fp_sample_t::v);

    fp_sample_t s{};
    s.t_seconds = 12.5;
    s.v         = 3.5f;

    // Forward direction: seconds -> int64 ns.
    constexpr std::int64_t k_expected_ns = 12'500'000'000;
    TEST_ASSERT(policy.get_timestamp(s) == k_expected_ns,
        std::string("expected fp timestamp seconds to convert to ns; got ") +
            std::to_string(policy.get_timestamp(s)));

    // Erased policy must propagate the same conversion.
    const plot::Data_access_policy erased = policy.erase();
    TEST_ASSERT(erased.get_timestamp(&s) == k_expected_ns,
        "erased fp timestamp accessor mismatch");

    return true;
}

bool test_series_builder_preview_config()
{
    auto main_source = std::make_shared<plot::Vector_data_source<sample_t>>();
    auto preview_source = std::make_shared<plot::Vector_data_source<sample_t>>();
    auto policy = plot::make_access_policy<sample_t>(&sample_t::t, &sample_t::v, &sample_t::v_min, &sample_t::v_max);

    plot::preview_config_t preview_cfg;
    preview_cfg.data_source   = preview_source;
    preview_cfg.access        = policy.erase();
    preview_cfg.style         = plot::Display_style::AREA;
    preview_cfg.interpolation = plot::Series_interpolation::LINEAR;

    auto series = plot::Series_builder()
        .enabled(false)
        .style(plot::Display_style::LINE)
        .interpolation(plot::Series_interpolation::STEP_AFTER)
        .empty_window_behavior(plot::Empty_window_behavior::HOLD_LAST_FORWARD)
        .nonfinite_policy(plot::Nonfinite_sample_policy::REPLACE_WITH_ZERO)
        .stack_group(7)
        .data_source(main_source)
        .access(policy)
        .preview(preview_cfg)
        .build_value();

    TEST_ASSERT(series.preview_config.has_value(), "expected preview_config to be set");
    TEST_ASSERT(series.preview_config->data_source.get() == preview_source.get(),
        "preview data_source mismatch");
    TEST_ASSERT(series.preview_config->style && *series.preview_config->style == plot::Display_style::AREA,
        "preview style mismatch");
    TEST_ASSERT(series.interpolation == plot::Series_interpolation::STEP_AFTER,
        "series interpolation mismatch");
    TEST_ASSERT(
        series.preview_config->interpolation &&
            *series.preview_config->interpolation == plot::Series_interpolation::LINEAR,
        "preview interpolation mismatch");
    TEST_ASSERT(series.effective_preview_interpolation() == plot::Series_interpolation::LINEAR,
        "effective preview interpolation mismatch");
    TEST_ASSERT(!series.preview_matches_main(),
        "preview should not match main when interpolation/style/source differ");
    TEST_ASSERT(series.empty_window_behavior == plot::Empty_window_behavior::HOLD_LAST_FORWARD,
        "empty_window_behavior mismatch");
    TEST_ASSERT(series.nonfinite_policy == plot::Nonfinite_sample_policy::REPLACE_WITH_ZERO,
        "nonfinite_policy mismatch");
    TEST_ASSERT(series.stack_group == 7, "stack_group mismatch");

    return true;
}

bool test_series_builder_default_interpolation_is_linear()
{
    auto series = plot::Series_builder().build_value();
    auto shared_series = plot::Series_builder()
        .series_label("core-builder")
        .build_shared();

    TEST_ASSERT(series.interpolation == plot::Series_interpolation::LINEAR,
        "default series interpolation should be linear");
    TEST_ASSERT(series.effective_preview_interpolation() == plot::Series_interpolation::LINEAR,
        "default preview interpolation should be linear");
    TEST_ASSERT(shared_series->series_label == "core-builder",
        "core builder build_shared() should preserve inherited setter values");

    return true;
}

} // namespace

int main()
{
    std::cout << "Typed API tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_fnv64_word_hash);
    RUN_TEST(test_policy_copy_move_ownership);
    RUN_TEST(test_policy_assignment_preserves_destination_revision);
    RUN_TEST(test_member_offset_matches_offsetof);
    RUN_TEST(test_layout_key_distinguishes_sample_types);
    RUN_TEST(test_member_pointer_semantics_key_is_distinct_from_layout_key);
    RUN_TEST(test_make_access_policy_and_erase);
    RUN_TEST(test_callable_semantics_key_is_conservative_until_explicit);
    RUN_TEST(test_explicit_semantics_key_helpers);
    RUN_TEST(test_erased_access_view_uses_direct_member_accessors);
    RUN_TEST(test_typed_api_floating_point_timestamp_member);
    RUN_TEST(test_series_builder_preview_config);
    RUN_TEST(test_series_builder_default_interpolation_is_linear);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
