// Recoiling Buns: the body regions a ripple can run over. Header-only and free of game types.
//
// The butt is the region the ripple was made for. Thighs (the back of each thigh), belly and breasts are optional
// regions on the same body mesh, each with its own on/off, strength, size, delay and wave multipliers. A vertex
// belongs to at most two regions; overlapping regions share it in priority order (butt, thighs, belly, breasts), each
// later one getting only the weight the earlier ones left, so the butt's own weights never change.
#pragma once

#include "Ripple.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace BR
{
    enum Region : std::uint8_t
    {
        kButt,
        kThighs,
        kBelly,
        kBreasts,
        kRegionCount
    };

    // Lobes: one wave centre each. Butt left/right, thigh left/right, belly, breast left/right.
    inline constexpr int                          kLobes = 7;
    inline constexpr std::array<Region, kLobes>    kRegionOf{ kButt, kButt, kThighs, kThighs, kBelly, kBreasts, kBreasts };
    inline constexpr std::uint8_t                  kNoLobe = 0xFF;
    // each lobe's twin on the other side (-1: none) - all around, a vertex takes the nearer of the two waves' centres, so
    // the two run into each other without a seam down her middle
    inline constexpr std::array<int, kLobes> kTwin{ 1, 0, 3, 2, -1, 6, 5 };
    inline constexpr std::array<std::string_view, kRegionCount> kRegionNames{ "Butt", "Thighs", "Belly", "Breasts" };

    struct RegionConfig
    {
        bool  enabled = false;
        float strength = 1.0f;     // multiplies the impact's strength
        float size = 1.0f;         // radius in butt-bone separations (the butt uses RegionScale instead)
        float delay = 0.0f;        // seconds after the impact the region's wave starts
        float speed = 1.0f;        // multipliers on the [Ripple] wave: travel speed, wavelength, decay, dent depth
        float wavelength = 1.0f;
        float decay = 1.0f;
        float dent = 1.0f;
    };

    // The shipped defaults: the butt as it always was and the back of the thighs, every multiplier 1; the others off with
    // settings to start tuning from.
    inline std::array<RegionConfig, kRegionCount> DefaultRegions()
    {
        std::array<RegionConfig, kRegionCount> r{};
        r[kButt] = { true, 1.0f, 2.1f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        r[kThighs] = { true, 1.0f, 1.6f, 0.03f, 1.0f, 1.0f, 1.0f, 1.0f };
        r[kBelly] = { false, 0.4f, 1.3f, 0.05f, 1.0f, 1.0f, 1.0f, 0.5f };
        r[kBreasts] = { false, 0.5f, 1.1f, 0.08f, 1.0f, 1.0f, 1.0f, 0.0f };
        return r;
    }

    // The wave a region runs: the [Ripple] wave with the region's multipliers. With every multiplier 1 it is the [Ripple]
    // wave bit for bit (x * 1.0f == x).
    inline WaveConfig WaveFor(const WaveConfig& a_w, const RegionConfig& a_r)
    {
        WaveConfig w = a_w;
        w.waveSpeed = a_w.waveSpeed * a_r.speed;
        w.wavelength = a_w.wavelength * a_r.wavelength;
        w.decay = a_w.decay * a_r.decay;
        w.dentDepth = a_w.dentDepth * a_r.dent;
        if (w.wavelength < 0.5f) w.wavelength = 0.5f;
        if (w.decay < 0.01f) w.decay = 0.01f;
        return w;
    }
}
