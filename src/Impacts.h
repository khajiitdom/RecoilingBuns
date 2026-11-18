// Recoiling Buns: when an impact starts a ripple. Header-only and free of game types, so the same code
// also runs outside the game.
//
//  * StrokeDetector: his thrusts, from the distance between his genital base and the middle of her buttocks, frame by
//    frame. A thrust starts when he closes in faster than kStart x MinApproachSpeed and lands when the closing speed
//    falls below kEnd x MinApproachSpeed: the gap between the two keeps a wobbling speed from counting twice.
//  * ImpactTiming: Penetration Physics (PPA) plays its impact sounds a few times per thrust, some of them while he is
//    pulled back. A ripple per sound starts a new dent every 0.2 s at a fast pace and cuts every wave off before it
//    has crossed her buttocks. So the sounds only say that the two collide, and the ripple starts once per thrust, at
//    the moment it lands: when an impact sound played shortly before, or plays right after. Where no thrust can be
//    seen (a pose that hardly changes the distance), the sounds start the ripples themselves, at most one per
//    kFallbackGap.
//  * ContactGate: without the sounds (Trigger = motion, the default), whether a landed thrust met her: it ends about as
//    close to her as his other recent thrusts, within ContactDistance.
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>

namespace BR
{
    struct Stroke
    {
        float peak = 0.0f;   // fastest closing speed of the thrust, units per second
        float dmin = 0.0f;   // nearest distance it reached
    };

    class StrokeDetector
    {
    public:
        static constexpr float kStart = 0.25f, kEnd = 0.05f;

        // The next distance measures no speed (a hitch, a paused game): it only becomes the new starting point, and a
        // thrust in progress goes on.
        void Forget() { m_have = false; }
        // Nobody close to her, or another partner: everything starts anew.
        void Reset()
        {
            m_have = false;
            m_closing = false;
        }

        // One frame: a_d = the distance now, a_dt = seconds since the last frame. Returns the thrust that landed in
        // this frame, if one did.
        std::optional<Stroke> Update(float a_d, float a_dt, float a_minApproach)
        {
            if (!m_have || a_dt <= 1e-4f) {   // no speed from this frame: a new starting point only
                m_vs = 0.0f;
                m_d = a_d;
                m_have = true;
                if (m_closing) m_dmin = (std::min)(m_dmin, a_d);   // a thrust in progress: the distance it reached still counts
                return std::nullopt;
            }
            const float v = (a_d - m_d) / a_dt;
            m_vs += 0.5f * (v - m_vs);
            m_d = a_d;
            if (m_vs < -kStart * a_minApproach) {
                if (!m_closing) {
                    m_closing = true;
                    m_peak = 0.0f;
                    m_dmin = a_d;
                }
                m_peak = (std::max)(m_peak, -m_vs);
                m_dmin = (std::min)(m_dmin, a_d);
            } else if (m_closing && m_vs > -kEnd * a_minApproach) {
                m_closing = false;
                return Stroke{ m_peak, m_dmin };
            }
            return std::nullopt;
        }

        bool  Have() const { return m_have; }       // a distance was measured since the last Forget / Reset
        float Distance() const { return m_d; }      // the last distance
        float Speed() const { return m_vs; }        // smoothed change of the distance per second (negative = closing)

    private:
        float m_d = 0.0f, m_vs = 0.0f, m_peak = 0.0f, m_dmin = 0.0f;
        bool  m_have = false, m_closing = false;
    };

    // Motion detection (no impact sound to prove that the two touch): a landed thrust is an impact when it ends about as
    // close to her as his other thrusts of the last kWindow seconds - the bottom of his stroke is where they meet, at
    // whatever distance her body, his and the pose put it - and no further than a_cap. Landings further than a_cap are
    // not remembered; the first one in a while counts on its own.
    class ContactGate
    {
    public:
        static constexpr float kWindow = 2.0f;       // seconds of landings that say how close his thrusts come
        static constexpr float kSlack = 2.5f;        // a landing may stop this much short of the closest of them,
        static constexpr float kSlackShare = 0.2f;   // or this share of that distance where that is more

        void Reset() { m_n = 0; }

        bool Lands(float a_now, float a_dmin, float a_cap)
        {
            std::size_t k = 0;   // the landings of the last kWindow seconds stay
            for (std::size_t i = 0; i < m_n; ++i)
                if (a_now - m_t[i] <= kWindow) {
                    m_t[k] = m_t[i];
                    m_d[k] = m_d[i];
                    ++k;
                }
            m_n = k;
            if (!(a_dmin <= a_cap)) return false;
            if (m_n == kMax) {   // full (some 10 thrusts a second): the oldest goes
                for (std::size_t i = 1; i < m_n; ++i) {
                    m_t[i - 1] = m_t[i];
                    m_d[i - 1] = m_d[i];
                }
                --m_n;
            }
            m_t[m_n] = a_now;
            m_d[m_n] = a_dmin;
            ++m_n;
            float lo = a_dmin;
            for (std::size_t i = 0; i < m_n; ++i) lo = (std::min)(lo, m_d[i]);
            return a_dmin <= lo + (std::max)(kSlack, kSlackShare * lo);
        }

    private:
        static constexpr std::size_t kMax = 32;
        float                        m_t[kMax]{}, m_d[kMax]{};
        std::size_t                  m_n = 0;
    };

    // Which thrusts and impact sounds start a ripple on one woman. Times are seconds on one clock; a_lastRipple is the
    // time of her last ripple from any source.
    class ImpactTiming
    {
    public:
        static constexpr float kSoundWindow = 0.75f;   // a thrust ripples when an impact sound played this recently,
        static constexpr float kLateSound = 0.15f;     // or plays this soon after it landed
        static constexpr float kMinGap = 0.3f;         // at least this long between two ripples (a wave needs ~0.4 s)
        static constexpr float kNoThrusts = 2.0f;      // sounds for this long and no thrust landed meanwhile: the sounds
        static constexpr float kFallbackGap = 0.4f;    // ripple on their own, at most one per this (the next thrust waits as long)

        enum class Thrust
        {
            kRipple,   // ripple now
            kWait,     // no impact sound yet: one in the next kLateSound ripples it (OnSound -> kThrust)
            kSkip      // a sound says they collide, but the last ripple is too recent
        };
        enum class Sound
        {
            kNothing,
            kThrust,   // ripple the thrust that landed just before (at its own contact point)
            kOwn       // ripple now: no thrust has landed lately
        };

        Thrust OnThrust(float a_now, float a_lastRipple)
        {
            m_thrust = a_now;
            m_waiting = false;
            if (a_now - m_sound > kSoundWindow) {
                m_waiting = true;
                return Thrust::kWait;
            }
            return Clear(a_now, a_lastRipple) ? Thrust::kRipple : Thrust::kSkip;
        }

        Sound OnSound(float a_now, float a_lastRipple)
        {
            if (a_now - m_sound > kNoThrusts) m_since = a_now;   // the first sound after a quiet spell (a scene start)
            m_sound = a_now;
            if (m_waiting && a_now - m_thrust <= kLateSound) {
                m_waiting = false;
                return Clear(a_now, a_lastRipple) ? Sound::kThrust : Sound::kNothing;
            }
            m_waiting = false;
            // only after the sounds have gone on for kNoThrusts with no thrust: at a scene start his first thrust
            // usually lands after the first sound
            if (a_now - (std::max)(m_thrust, m_since) > kNoThrusts && Clear(a_now, a_lastRipple) && a_now - a_lastRipple >= kFallbackGap) {
                m_own = a_now;
                return Sound::kOwn;
            }
            return Sound::kNothing;
        }

    private:
        // far enough from the last ripple, and from the last ripple a sound started on its own
        bool Clear(float a_now, float a_lastRipple) const { return a_now - a_lastRipple >= kMinGap && a_now - m_own >= kFallbackGap; }

        float m_sound = -100.0f, m_thrust = -100.0f, m_own = -100.0f, m_since = -100.0f;
        bool  m_waiting = false;
    };
}
