// Recoiling Buns: SexLab scenes (classic SexLab 1.6x and SexLab P+ 2.x) - which of a SexLab thread's events mean what, and
// when a SexLab P+ scene's stage is asked for. Header-only and free of game types, so the same code also runs outside
// the game.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace BR
{
    // A SexLab thread sends each step of its scene as SendModEvent(<step>, <its thread number>) (sslThreadModel's
    // SetupThreadEvent - the same in classic SexLab and P+). What each step means here:
    enum class SexLabStep
    {
        kOther,       // not one of ours (every mod's SendModEvent comes by)
        kStart,       // the scene started (AnimationStart)
        kNewStage,    // the scene moves to another stage or animation (StageStart, AnimationChange, LeadInEnd)
        kPlaces,      // someone moved, joined or left (PositionChange, ActorChangeEnd, ActorsRelocated)
        kEnd          // the scene ends (AnimationEnding: its actors are let go of next; AnimationEnd: it is over)
    };

    namespace detail
    {
        // the game's strings ignore case: an event's name comes as it was first written anywhere in the game
        inline bool SameName(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                char x = a[i], y = b[i];
                if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
                if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
                if (x != y) return false;
            }
            return true;
        }
    }

    // exact names only: SetupThreadEvent also sends "<step>_<hook>" for every hook a mod set on the thread, and classic
    // SexLab "Player<step>" for a scene with the player in it - those are the same steps again
    inline SexLabStep ClassifySexLabStep(std::string_view a_name)
    {
        using detail::SameName;
        if (SameName(a_name, "AnimationStart")) return SexLabStep::kStart;
        if (SameName(a_name, "StageStart") || SameName(a_name, "AnimationChange") || SameName(a_name, "LeadInEnd")) return SexLabStep::kNewStage;
        if (SameName(a_name, "PositionChange") || SameName(a_name, "ActorChangeEnd") || SameName(a_name, "ActorsRelocated")) return SexLabStep::kPlaces;
        if (SameName(a_name, "AnimationEnding") || SameName(a_name, "AnimationEnd")) return SexLabStep::kEnd;
        return SexLabStep::kOther;
    }

    // the thread number the event carries (its text: "0" .. "14"); -1 = not a thread number
    inline int SexLabThreadNumber(std::string_view a_text)
    {
        if (a_text.empty() || a_text.size() > 3) return -1;
        int v = 0;
        for (const char c : a_text) {
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v <= 255 ? v : -1;
    }

    // SexLab P+'s stage number is the length of its scene's stage history (its own Stage property, the stages played so
    // far). P+ adds a new stage to the history only after the stage's StageStart went out, so the stage is asked for
    // with a call of its own (GetStageHistoryLength): kAfterStep after each step, then every kEvery while the scene
    // runs. An answer comes back on a Papyrus thread; a question out for longer than kLost counts as lost.
    // Times in ms (any clock that only goes forward).
    struct PlusStagePoll
    {
        static constexpr std::array<std::int64_t, 3> kAfterStep{ 300, 800, 1500 };
        static constexpr std::int64_t                kEvery = 2000, kLost = 3000;

        std::int64_t  stepAt = 0;     // the last step
        std::size_t   next = 0;       // the next of kAfterStep (kAfterStep.size() = every kEvery)
        std::int64_t  askAt = 0;      // the next question's time
        std::int64_t  askedAt = -1;   // the question out (-1 = none)
        bool          firstDue = false;   // the last step's first question hasn't gone out yet

        // a step of the scene (it starts or moves on): asked for kAfterStep later - or sooner, when the step before still
        // waits for its first question (P+'s AnimationStart comes a moment after its first stage's StageStart). A question
        // still out is about the stage before (its answer doesn't count - the caller drops it) and doesn't hold this up.
        void Step(std::int64_t a_now)
        {
            askAt = firstDue ? (std::min)(askAt, a_now + kAfterStep[0]) : a_now + kAfterStep[0];
            stepAt = a_now;
            next = 0;
            firstDue = true;
            askedAt = -1;
        }
        bool Due(std::int64_t a_now) const
        {
            if (askedAt >= 0 && a_now - askedAt < kLost) return false;   // the last question is still out
            return a_now >= askAt;
        }
        // a question went out
        void Asked(std::int64_t a_now)
        {
            askedAt = a_now;
            firstDue = false;
            if (next + 1 < kAfterStep.size()) {
                askAt = stepAt + kAfterStep[++next];
            } else {
                next = kAfterStep.size();
                askAt = a_now + kEvery;
            }
        }
        // its answer came
        void Answered() { askedAt = -1; }
    };
}
