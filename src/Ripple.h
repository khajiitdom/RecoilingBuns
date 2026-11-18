// Recoiling Buns: the ripple wave field and the shading colour. Header-only and free of game types, so the same code
// also runs outside the game.
//
//  * Field: the surface offset (body units, along the vertex normal) of one impact at distance r from its centre,
//    tau seconds after it, at strength s: a damped wave packet running outward plus a short dent at the centre.
//  * Shade: the colour multipliers for the change in lighting a ripple causes. The body's model-space normal map
//    ignores vertex normals, so the wave is shown through the vertex colour layer as moving shading bands.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace BR
{
    // Ripple shape (body units / seconds). The plugin clamps the loaded values: wavelength >= 0.5,
    // packetWidth >= 0.3, decay >= 0.01; spread and dentTime are guarded inside Field.
    struct WaveConfig
    {
        float amplitude = 0.8f;     // peak height of the wave packet at strength 1
        float waveSpeed = 40.0f;    // outward speed of the packet, units per second
        float wavelength = 4.5f;    // distance between crests
        float packetWidth = 3.0f;   // gaussian width of the packet around its travelling centre
        float decay = 0.36f;        // seconds for the packet to fall to 1/e
        float spread = 7.0f;        // distance over which the packet thins as it widens (1 / sqrt falloff)
        float dentDepth = 0.7f;     // depth of the dent at the impact point at strength 1
        float dentRadius = 2.5f;    // gaussian radius of the dent
        float dentTime = 0.06f;     // seconds for the dent to fall to 1/e
    };

    // Surface offset of one impact. Keep the operations, their order and their float types as they are: the look of
    // the ripple was tuned on exactly this formula.
    inline float Field(const WaveConfig& a_c, float a_r, float a_tau, float a_s)
    {
        constexpr float twoPi = 6.2831853f;
        float sft = a_r - a_c.waveSpeed * a_tau;
        float packet = -std::sin(twoPi * sft / a_c.wavelength) * std::exp(-(sft / a_c.packetWidth) * (sft / a_c.packetWidth));
        float amp = a_c.amplitude * a_s * std::exp(-a_tau / a_c.decay) / std::sqrt(1.0f + a_r / (std::max)(0.1f, a_c.spread));
        float dent = -a_c.dentDepth * a_s * std::exp(-(a_r / a_c.dentRadius) * (a_r / a_c.dentRadius)) *
                     std::exp(-a_tau / (std::max)(0.005f, a_c.dentTime));
        return amp * packet + dent;
    }

    // Shading colour. gain: darkening per unit of lost lighting; darkest: the lowest multiplier a fully tinted
    // channel reaches; tint: how strongly each channel (r, g, b) follows the darkening (1, 1, 1: an even shadow; a
    // lower red gives a warmer one). The defaults are the shipped settings.
    struct ShadeConfig
    {
        float                gain = 2.2f;
        float                darkest = 0.55f;
        std::array<float, 3> tint{ 1.0f, 1.0f, 1.0f };
    };

    namespace detail
    {
        // [0, 1]; NaN -> 0.
        inline float Unit(float a_x) { return a_x > 0.0f ? (a_x < 1.0f ? a_x : 1.0f) : 0.0f; }
    }

    // Colour multipliers (r, g, b) for a change in lighting a_delta = n'.L - n.L (rippled normal against rest normal).
    // dark = clamp(-gain * delta, 0, 1 - darkest), channel k = 1 - dark * tint[k]. Exactly 1 at rest and for any
    // brightening (delta >= 0), and never above 1: the colour layer is white at rest, so swapping it in or out never
    // shows. Out-of-range settings are held in range here as well (gain >= 0, darkest and tint in [0, 1]) and a NaN
    // delta counts as rest, so the result always lies in [0, 1].
    inline std::array<float, 3> Shade(const ShadeConfig& a_c, float a_delta)
    {
        const float gain = a_c.gain > 0.0f ? a_c.gain : 0.0f;
        const float limit = detail::Unit(1.0f - detail::Unit(a_c.darkest));
        float       dark = -gain * a_delta;
        dark = dark > 0.0f ? (dark < limit ? dark : limit) : 0.0f;
        std::array<float, 3> out{};
        for (std::size_t k = 0; k < out.size(); ++k) out[k] = 1.0f - dark * detail::Unit(a_c.tint[k]);
        return out;
    }
}
