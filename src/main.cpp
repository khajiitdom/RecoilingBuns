// Recoiling Buns
//
// A travelling ripple across a woman's buttocks on each thrust of her partner that lands, on top of her physics
// jiggle (CBPC / SMP). It works with whatever body and outfit she wears: no mesh is replaced.
//
//  * Impacts (Impacts.h): by default from the motion alone (Trigger = motion): the nearest partner's genital base (or
//    pelvis) closing in on the middle of her buttocks and turning around is a thrust, and one that lands where they
//    meet (ContactGate) ripples once, when it lands. Optionally (Trigger = sound / both) Penetration Physics' impact
//    sounds say that the two collide (two entry hooks: the sound loader remembers the handles built from PPA's impact
//    files, the sound player counts their plays); without those sounds the motion detection takes over.
//  * Rules (Rules.h): in an OStim scene an impact ripples only at the speeds / stages where the wave can be seen. The
//    scene and its speed come from OStim (its plugin interface and OThread.GetSpeed), each scene's speeds from the
//    scene files, read once after the data load. In a SexLab scene (classic SexLab or SexLab P+: SexLab.h, the SexLab
//    section below) only at those stages too; its stage comes from SexLab's own scene thread.
//  * Ripple (Ripple.h): a damped wave packet runs outward over each cheek from the point nearest the contact. It is
//    written straight into the body's GPU vertex data: while a ripple runs, the shape's vertex buffer is swapped for a
//    dynamic copy of the engine's own vertex data and rewritten every frame; when it is over, the original goes back.
//    RaceMenu / OBody morph data is the base we add to, and nothing goes through the morph system (SMP would rebuild
//    its physics on every morph update).
//  * Shading: bodies are lit from a model-space normal map, which ignores vertex normals, so the wave shows in the
//    outline and as moving shading bands in the vertex colours. A body without vertex colours gets them only in the
//    copy the game draws while a ripple runs: in the 4 bytes of each vertex's position that the skinned model-space
//    shaders never read (Layout.h AliasColour). Its vertex data, its skin partition's layout and its geometry's layout
//    are never changed, so a copy another mod makes of her mesh (RaceMenu overlays, body morphs) at any moment, from
//    any thread, copies the game's own mesh. Clothing with ordinary (tangent space) normal maps gets tilted normals
//    instead.
//  * Frame: the once-per-frame main-thread call in Main::Update drives everything. Every engine write happens on the
//    main thread: there, or in ResetAll at a new game / before a load (SKSE messages, on the main thread too).

#include "PCH.h"

#include "DefaultIni.h"
#include "Impacts.h"
#include "Layout.h"
#include "Regions.h"
#include "Ripple.h"
#include "Rules.h"
#include "SexLab.h"
#include "SMF.h"

#include "PluginInterface/InterfaceExchangeMessage.h"
#include "PluginInterface/Threading/ThreadInterface.h"

#include <DirectXPackedVector.h>
#include <d3d11.h>
#include <queue>

// the plugin's name (its DLL, ini and log are named after it), its title in the log and its author - the build sets
// them (CMakeLists.txt); these are the defaults
#ifndef BR_NAME
#    define BR_NAME "RecoilingBuns"
#endif
#ifndef BR_TITLE
#    define BR_TITLE "Recoiling Buns"
#endif
#ifndef BR_AUTHOR
#    define BR_AUTHOR ""
#endif
#ifndef BR_VERSION   // "major.minor.patch" and its parts (CMake's project version)
#    define BR_VERSION "1.3.0"
#    define BR_VERSION_MAJOR 1
#    define BR_VERSION_MINOR 3
#    define BR_VERSION_PATCH 0
#endif

namespace BR
{
    using V3 = RE::NiPoint3;
    using Clock = std::chrono::steady_clock;
    using Tri = RE::BSGraphics::TriShape;

    constexpr const char* kVersion = BR_VERSION;
    constexpr const char* kIni = "Data/SKSE/Plugins/" BR_NAME ".ini";
    constexpr const char* kSceneFolder = "Data/SKSE/Plugins/OStim/scenes";

    DWORD g_mainThread = 0;      // the thread plugins load on: the game's main thread
    bool  g_renderHooked = false;   // the check right before each render is in (InstallRenderHook): the colour layer may go in

    inline float Dot(const V3& a_a, const V3& a_b) { return a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z; }
    inline V3    Mul(const V3& a_v, float a_s) { return { a_v.x * a_s, a_v.y * a_s, a_v.z * a_s }; }
    inline float Smooth(float a_x)
    {
        a_x = std::clamp(a_x, 0.0f, 1.0f);
        return a_x * a_x * (3.0f - 2.0f * a_x);
    }
    V3 Normalized(const V3& a_v)
    {
        const float l = a_v.Length();
        return l > 1e-12f ? Mul(a_v, 1.0f / l) : V3{ 0, 0, 1 };
    }

    std::string Lowered(std::string_view a_s)
    {
        std::string s(a_s);
        detail::LowerInPlace(s);
        return s;
    }

    const char* OnOff(bool a_on) { return a_on ? "on" : "off"; }

    std::string Join(const std::vector<std::string>& a_items)
    {
        std::string out;
        for (const auto& s : a_items) out += (out.empty() ? "" : ", ") + s;
        return out.empty() ? "none" : out;
    }

    // ------------------------------------------------------------------------------------------------ configuration
    // Data/SKSE/Plugins/<name>.ini. The sections are for the reader only: every key is unique. Read at start and again
    // whenever the file changes (checked once a second).
    struct Config
    {
        // [General]
        bool enabled = true;
        bool debug = false;
        int  testKey = 0xC5;   // DirectInput scan code, 0 = off (0xC5 = Pause; OStim uses the numpad)
        // [Rules]
        RuleConfig rules;
        bool       sexLab = true;   // SexLab's scenes are followed (their stages); false = they count as outside a scene
        // [Ripple] (body units / seconds)
        WaveConfig wave;
        float      motionScale = 2.0f;   // how far the skin moves (and its lighting tilts); the shading bands keep their strength
        float      regionScale = 2.1f;   // rippling area, relative to the distance between the two butt bones
        float      maxAge = 1.0f;        // an impact's wave is dropped after this long
        int        maxEvents = 6;        // waves running at once per shape
        // [Regions] butt (its size is regionScale), thighs, belly, breasts (Regions.h)
        std::array<RegionConfig, kRegionCount> regions = DefaultRegions();
        // all around: every region a plain soft sphere around its centre - no cut by which way the skin faces, no gap at the
        // butt's cleft - and twin waves (the two cheeks, thighs, breasts) one field, and every mesh of hers the same centres:
        // a big region covers her whole body without seams (on by default)
        bool allAround = true;
        // [Shading]
        bool        shading = true;
        bool        addColourLayer = true;   // meshes without vertex colours get a white layer while they ripple
        bool        tiltNormals = true;
        ShadeConfig shade;
        // the light the shading bands are shaded with: 0 = fixed (above and behind her for the butt and thighs, above and in
        // front for the belly and breasts), 1 = follows the camera (above the viewer: from behind her it is the fixed light,
        // from any other side the bands read the same way)
        int bandLight = 0;
        // [Impacts]
        bool                     soundTrigger = false, motionTrigger = true;   // Trigger = motion | sound | both
        float                    soundStrength = 0.85f;                        // Strength (older name SoundStrength)
        std::vector<std::string> clapGlobs{ "Sound\\Fx\\ppa\\vagina\\clap*.wav", "Sound\\Fx\\ppa\\anus\\plap*.wav" };
        float                    minApproach = 12.0f, contactDistance = 30.0f, minInterval = 0.3f;
        float                    scanDistance = 1500.0f, partnerDistance = 120.0f;
    };
    Config            cfg;
    std::uint32_t     g_cfgGen = 0;        // + 1 whenever the settings are replaced from the ini (the settings tab takes them up)
    std::atomic<int>  g_testKey{ 0xC5 };   // what the input thread reads
    std::atomic<bool> g_logSteps{ false }; // cfg.debug, for SexLab's steps (they come on Papyrus threads)
    std::atomic<bool> g_slFollow{ true };  // cfg.sexLab, for the same

    namespace ini
    {
        bool Float(std::string_view a_v, float& a_out)
        {
            a_v = detail::Trim(a_v);
            if (!a_v.empty() && a_v.front() == '+') a_v.remove_prefix(1);
            float      v = 0.0f;
            const auto r = std::from_chars(a_v.data(), a_v.data() + a_v.size(), v);
            if (r.ec != std::errc{} || r.ptr != a_v.data() + a_v.size() || !std::isfinite(v)) return false;
            a_out = v;
            return true;
        }

        bool Int(std::string_view a_v, int& a_out)
        {
            a_v = detail::Trim(a_v);
            int base = 10;
            if (a_v.size() > 2 && a_v[0] == '0' && (a_v[1] == 'x' || a_v[1] == 'X')) {
                base = 16;
                a_v.remove_prefix(2);
            }
            int        v = 0;
            const auto r = std::from_chars(a_v.data(), a_v.data() + a_v.size(), v, base);
            if (r.ec != std::errc{} || r.ptr != a_v.data() + a_v.size()) return false;
            a_out = v;
            return true;
        }

        bool Bool(std::string_view a_v, bool& a_out)
        {
            const std::string s = Lowered(detail::Trim(a_v));
            if (s == "1" || s == "true" || s == "on" || s == "yes") a_out = true;
            else if (s == "0" || s == "false" || s == "off" || s == "no") a_out = false;
            else return false;
            return true;
        }

        // comma separated, each entry trimmed, blank entries dropped
        std::vector<std::string> List(std::string_view a_v, bool a_lower)
        {
            std::vector<std::string> out;
            while (true) {
                const auto       comma = a_v.find(',');
                std::string_view item = detail::Trim(a_v.substr(0, comma));
                if (!item.empty()) out.emplace_back(a_lower ? Lowered(item) : std::string(item));
                if (comma == std::string_view::npos) break;
                a_v.remove_prefix(comma + 1);
            }
            return out;
        }

        enum class Result
        {
            kDone,
            kBad,
            kUnknown
        };

        // one key (lower case) of the ini; every value is held in its range
        Result Apply(Config& a_c, std::string_view a_key, std::string_view a_val)
        {
            const auto num = [&](float& a_dst, float a_lo, float a_hi) {
                float v = 0.0f;
                if (!Float(a_val, v)) return Result::kBad;
                a_dst = std::clamp(v, a_lo, a_hi);
                return Result::kDone;
            };
            const auto whole = [&](int& a_dst, int a_lo, int a_hi) {
                int v = 0;
                if (!Int(a_val, v)) return Result::kBad;
                a_dst = std::clamp(v, a_lo, a_hi);
                return Result::kDone;
            };
            const auto flag = [&](bool& a_dst) { return Bool(a_val, a_dst) ? Result::kDone : Result::kBad; };

            // [General]
            if (a_key == "enabled") return flag(a_c.enabled);
            if (a_key == "debug") return flag(a_c.debug);
            if (a_key == "testkey") return whole(a_c.testKey, 0, 255);
            // [Rules]
            if (a_key == "rules") return flag(a_c.rules.enabled);
            if (a_key == "outsidescenes" || a_key == "outsideostim") return flag(a_c.rules.outsideOStim);
            if (a_key == "sexlab") return flag(a_c.sexLab);
            if (a_key == "excludemodpacks") {
                a_c.rules.excludeModpacks = List(a_val, true);
                return Result::kDone;
            }
            if (a_key == "excludesceneprefixes") {
                a_c.rules.excludePrefixes = List(a_val, true);
                return Result::kDone;
            }
            if (a_key == "minspeed") return num(a_c.rules.minSpeed, 0.0f, 100.0f);
            if (a_key == "maxspeed") return num(a_c.rules.maxSpeed, 0.0f, 100.0f);
            if (a_key == "minstage") return whole(a_c.rules.minStage, 0, 1000);
            if (a_key == "maxstage") return whole(a_c.rules.maxStage, 0, 1001);
            // [Ripple]
            if (a_key == "amplitude") return num(a_c.wave.amplitude, 0.0f, 5.0f);
            if (a_key == "wavespeed") return num(a_c.wave.waveSpeed, 1.0f, 1000.0f);
            if (a_key == "wavelength") return num(a_c.wave.wavelength, 0.5f, 100.0f);
            if (a_key == "packetwidth") return num(a_c.wave.packetWidth, 0.3f, 100.0f);
            if (a_key == "decay") return num(a_c.wave.decay, 0.01f, 10.0f);
            if (a_key == "spread") return num(a_c.wave.spread, 0.1f, 1000.0f);
            if (a_key == "dentdepth") return num(a_c.wave.dentDepth, 0.0f, 5.0f);
            if (a_key == "dentradius") return num(a_c.wave.dentRadius, 0.1f, 50.0f);
            if (a_key == "denttime") return num(a_c.wave.dentTime, 0.005f, 5.0f);
            if (a_key == "motionscale") return num(a_c.motionScale, 0.0f, 5.0f);
            if (a_key == "regionscale") return num(a_c.regionScale, 0.5f, 15.0f);
            if (a_key == "allaround") return flag(a_c.allAround);
            if (a_key == "maxage") return num(a_c.maxAge, 0.05f, 10.0f);
            if (a_key == "maxevents") return whole(a_c.maxEvents, 1, 32);
            // [Regions]: <Region><Setting>, e.g. ThighsEnabled, BellyStrength (ButtSize = RegionScale)
            for (int r = 0; r < kRegionCount; ++r) {
                const std::string p = Lowered(kRegionNames[r]);
                if (!a_key.starts_with(p)) continue;
                const std::string_view k = a_key.substr(p.size());
                auto&                  rc = a_c.regions[r];
                if (k == "enabled") return r == kButt ? Result::kDone : flag(rc.enabled);   // the butt is always on
                if (k == "strength") return num(rc.strength, 0.0f, 5.0f);
                if (k == "size") return r == kButt ? num(a_c.regionScale, 0.5f, 15.0f) : num(rc.size, 0.2f, 15.0f);
                if (k == "delay") return num(rc.delay, 0.0f, 1.0f);
                if (k == "speed") return num(rc.speed, 0.05f, 10.0f);
                if (k == "wavelength") return num(rc.wavelength, 0.1f, 10.0f);
                if (k == "decay") return num(rc.decay, 0.05f, 10.0f);
                if (k == "dent") return num(rc.dent, 0.0f, 10.0f);
            }
            // [Shading]
            if (a_key == "shading") return flag(a_c.shading);
            if (a_key == "addcolourlayer") return flag(a_c.addColourLayer);
            if (a_key == "shadegain") return num(a_c.shade.gain, 0.0f, 20.0f);
            if (a_key == "shadedarkest") return num(a_c.shade.darkest, 0.0f, 1.0f);
            if (a_key == "shadetint") {
                const auto parts = List(a_val, false);
                std::array<float, 3> tint{};
                if (parts.size() != tint.size()) return Result::kBad;
                for (std::size_t k = 0; k < tint.size(); ++k) {
                    if (!Float(parts[k], tint[k])) return Result::kBad;
                    tint[k] = std::clamp(tint[k], 0.0f, 1.0f);
                }
                a_c.shade.tint = tint;
                return Result::kDone;
            }
            if (a_key == "tiltnormals") return flag(a_c.tiltNormals);
            if (a_key == "bandlight") {
                const std::string v = Lowered(detail::Trim(a_val));
                if (v == "fixed" || v == "0") a_c.bandLight = 0;
                else if (v == "camera" || v == "1") a_c.bandLight = 1;
                else return Result::kBad;
                return Result::kDone;
            }
            // [Impacts]
            if (a_key == "trigger") {
                const std::string v = Lowered(detail::Trim(a_val));
                if (v != "sound" && v != "motion" && v != "both") return Result::kBad;
                a_c.soundTrigger = v == "sound" || v == "both";
                a_c.motionTrigger = v == "motion" || v == "both";
                return Result::kDone;
            }
            if (a_key == "strength" || a_key == "soundstrength") return num(a_c.soundStrength, 0.0f, 1.0f);
            if (a_key == "clapsounds") {
                a_c.clapGlobs = List(a_val, false);
                return Result::kDone;
            }
            if (a_key == "minapproachspeed") return num(a_c.minApproach, 0.0f, 1000.0f);
            if (a_key == "contactdistance") return num(a_c.contactDistance, 0.0f, 200.0f);
            if (a_key == "mininterval") return num(a_c.minInterval, 0.0f, 5.0f);
            // older motion strength settings: every thrust now ripples at Strength, whatever the pose - read, nothing to set
            if (a_key == "fullapproachspeed" || a_key == "strengthfloor" || a_key == "frontimpactscale") return Result::kDone;
            if (a_key == "scandistance") return num(a_c.scanDistance, 100.0f, 20000.0f);
            if (a_key == "partnerdistance") return num(a_c.partnerDistance, 10.0f, 1000.0f);
            return Result::kUnknown;
        }
    }

    // No settings file yet (a new install: the download carries none, so an update never replaces a player's settings):
    // the defaults are written with their notes, to edit by hand. A file that is there is never touched here.
    void EnsureIni()
    {
        std::error_code ec;
        if (std::filesystem::exists(kIni, ec) || ec) return;
        std::string text = std::format("; {} {}\r\n", BR_TITLE, kVersion);
        for (const char ch : kDefaultIni) {
            if (ch == '\r') continue;
            if (ch == '\n')
                text += "\r\n";
            else
                text += ch;
        }
        std::ofstream o(kIni, std::ios::binary | std::ios::trunc);
        if (!o || !o.write(text.data(), static_cast<std::streamsize>(text.size()))) {
            SKSE::log::warn("{} could not be written - the defaults are used", kIni);
            return;
        }
        SKSE::log::info("{} written with the defaults (no settings file yet)", kIni);
    }

    Config ReadConfig()
    {
        Config        c;
        std::ifstream in(kIni, std::ios::binary);
        if (!in) {
            SKSE::log::warn("{} not found - the defaults are used", kIni);
            return c;
        }
        std::string raw;
        int         lineNo = 0;
        while (std::getline(in, raw)) {
            ++lineNo;
            std::string_view line = raw;
            if (lineNo == 1 && line.starts_with("\xEF\xBB\xBF")) line.remove_prefix(3);
            line = detail::Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
            const auto eq = line.find('=');
            if (eq == std::string_view::npos) {
                SKSE::log::warn("ini line {}: no '=' - ignored", lineNo);
                continue;
            }
            const std::string key = Lowered(detail::Trim(line.substr(0, eq)));
            std::string_view  val = line.substr(eq + 1);
            if (const auto sc = val.find(';'); sc != std::string_view::npos) val = val.substr(0, sc);
            val = detail::Trim(val);
            switch (ini::Apply(c, key, val)) {
            case ini::Result::kBad:
                SKSE::log::warn("ini line {}: bad value for {}: '{}' - the default is kept", lineNo, key, val);
                break;
            case ini::Result::kUnknown:
                SKSE::log::warn("ini line {}: unknown key '{}' - ignored", lineNo, key);
                break;
            default:
                break;
            }
        }
        return c;
    }

    void LogConfig(const Config& a_c)
    {
        SKSE::log::info("config: enabled {}, debug {}, test key {:#04x}", OnOff(a_c.enabled), OnOff(a_c.debug), a_c.testKey);
        const auto& r = a_c.rules;
        if (!r.enabled) {
            SKSE::log::info("rules: off - every impact ripples");
        } else {
            const std::string stages = r.maxStage > r.minStage ? std::format("stages {} to {}", r.minStage, r.maxStage - 1) : "no stage";
            SKSE::log::info("rules: on - speed-based scenes ripple from {} up to (not at) {}, stage-based scenes (and SexLab's) at {}; outside "
                            "scenes {}; excluded packs: {}; excluded scene id prefixes: {}",
                detail::FormatSpeed(r.minSpeed), detail::FormatSpeed(r.maxSpeed), stages, OnOff(r.outsideOStim), Join(r.excludeModpacks),
                Join(r.excludePrefixes));
        }
        SKSE::log::info("SexLab scenes: {}", a_c.sexLab ? "followed (when SexLab is installed)" : "not followed (they count as outside a scene)");
        const auto& w = a_c.wave;
        SKSE::log::info("ripple: amplitude {:.2f}, wave speed {:.1f}, wavelength {:.2f}, packet width {:.2f}, decay {:.2f} s, spread {:.1f}, "
                        "dent {:.2f} (radius {:.2f}, {:.3f} s), motion scale {:.2f}, region scale {:.2f}, max age {:.2f} s, max events {}",
            w.amplitude, w.waveSpeed, w.wavelength, w.packetWidth, w.decay, w.spread, w.dentDepth, w.dentRadius, w.dentTime, a_c.motionScale,
            a_c.regionScale, a_c.maxAge, a_c.maxEvents);
        for (int g = 0; g < kRegionCount; ++g) {
            const auto& rc = a_c.regions[g];
            if (g != kButt && !rc.enabled) continue;
            SKSE::log::info("region {}: strength {:.2f}, size {:.2f}, delay {:.3f} s, speed x{:.2f}, wavelength x{:.2f}, decay x{:.2f}, dent x{:.2f}",
                kRegionNames[g], rc.strength, g == kButt ? a_c.regionScale : rc.size, rc.delay, rc.speed, rc.wavelength, rc.decay, rc.dent);
        }
        if (a_c.allAround || a_c.bandLight != 0)
            SKSE::log::info("regions all around (no edges): {}, band light: {}", OnOff(a_c.allAround), a_c.bandLight == 1 ? "follows the camera" : "fixed");
        SKSE::log::info("shading: {}, colour layer for meshes without one {}, gain {:.2f}, darkest {:.2f}, tint {:.2f} / {:.2f} / {:.2f}, tilted normals {}",
            OnOff(a_c.shading), OnOff(a_c.addColourLayer), a_c.shade.gain, a_c.shade.darkest, a_c.shade.tint[0], a_c.shade.tint[1],
            a_c.shade.tint[2], OnOff(a_c.tiltNormals));
        SKSE::log::info("impacts: trigger {}, strength {:.2f}, impact sounds {}, min approach {:.0f}/s, contact within {:.1f}, min interval {:.2f} s, "
                        "scan distance {:.0f}, partner distance {:.0f}",
            a_c.soundTrigger && a_c.motionTrigger ? "both" : (a_c.soundTrigger ? "sound" : "motion"), a_c.soundStrength,
            a_c.soundTrigger ? Join(a_c.clapGlobs) : std::string("not used"), a_c.minApproach, a_c.contactDistance, a_c.minInterval, a_c.scanDistance,
            a_c.partnerDistance);
    }

    // ------------------------------------------------------------------------------------------------ OStim scene files
    // Every Data/SKSE/Plugins/OStim/scenes/**/*.json (a mod manager's virtual Data folder shows every mod's files), read once on a
    // worker thread after the data load; the main thread takes the finished table over.
    std::mutex                        g_tableLock;
    std::shared_ptr<const SceneTable> g_tableDone;              // set by the reader
    std::atomic<bool>                 g_tableReady{ false };
    std::shared_ptr<const SceneTable> sceneTable;               // the main thread's
    std::size_t                       g_scenesRead = 0, g_scenesUnreadable = 0;   // written before g_tableReady

    std::size_t CountExcluded(const SceneTable& a_table, const RuleConfig& a_rules)
    {
        std::size_t n = 0;
        a_table.ForEach([&](const std::string& a_id, const SceneMeta& a_meta) {
            if (!ExclusionOf(a_rules, a_id, &a_meta).empty()) ++n;
        });
        return n;
    }

    void LogSceneCounts(const SceneTable& a_table, const RuleConfig& a_rules)
    {
        SKSE::log::info("OStim scenes: {} read, {} excluded by ExcludeModpacks / ExcludeScenePrefixes, {} unreadable{}", g_scenesRead,
            CountExcluded(a_table, a_rules), g_scenesUnreadable,
            a_table.Size() != g_scenesRead ? std::format(" ({} distinct scene ids)", a_table.Size()) : std::string());
    }

    // A scene file is a few kilobytes: anything larger than this is not one (and is not read into memory).
    constexpr std::streamoff kMaxSceneFile = 16 * 1024 * 1024;

    bool ReadText(const std::filesystem::path& a_path, std::string& a_out)
    {
        std::ifstream in(a_path, std::ios::binary | std::ios::ate);
        if (!in) return false;
        const auto size = static_cast<std::streamoff>(in.tellg());
        if (size < 0 || size > kMaxSceneFile) return false;
        a_out.resize(static_cast<std::size_t>(size));
        in.seekg(0);
        return static_cast<bool>(in.read(a_out.data(), size));
    }

    std::string Utf8(const std::u8string& a_s) { return std::string(reinterpret_cast<const char*>(a_s.data()), a_s.size()); }

    void ReadScenes(RuleConfig a_rules)
    {
        try {
            auto            table = std::make_shared<SceneTable>();
            std::size_t     read = 0, unreadable = 0;
            std::error_code ec;
            if (!std::filesystem::is_directory(kSceneFolder, ec)) {
                SKSE::log::info("OStim scenes: no {} folder - every OStim scene counts as unknown", kSceneFolder);
            } else {
                namespace fs = std::filesystem;
                for (fs::recursive_directory_iterator it(kSceneFolder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
                     it.increment(ec)) {
                    std::error_code fe;
                    if (!it->is_regular_file(fe)) continue;
                    const auto& path = it->path();
                    if (Lowered(Utf8(path.extension().u8string())) != ".json") continue;
                    std::string text;
                    if (!ReadText(path, text)) {
                        ++unreadable;
                        continue;
                    }
                    auto meta = ParseScene(text);
                    if (!meta) {
                        ++unreadable;
                        continue;
                    }
                    table->Add(Utf8(path.stem().u8string()), std::move(*meta));
                    ++read;
                }
                if (ec) SKSE::log::warn("OStim scenes: the folder walk stopped early ({})", ec.message());
            }
            g_scenesRead = read;
            g_scenesUnreadable = unreadable;
            LogSceneCounts(*table, a_rules);
            {
                std::scoped_lock l(g_tableLock);
                g_tableDone = std::move(table);
            }
            g_tableReady = true;
        } catch (const std::exception& e) {
            SKSE::log::error("OStim scenes: could not be read ({}) - every OStim scene counts as unknown", e.what());
        }
    }

    // ------------------------------------------------------------------------------------------------ OStim threads
    // OStim calls the listeners from its own threads: they only queue what happened; Tick (main thread) applies it.
    OStim::ThreadInterface* oThreads = nullptr;

    enum class SceneKind
    {
        kStart,
        kNode,
        kSpeed,
        kStop
    };

    struct SceneEvent
    {
        SceneKind               kind = SceneKind::kStart;
        std::int32_t            thread = -1;
        std::vector<RE::FormID> actors;
        std::string             node;   // the scene id now (lower case)
    };
    std::mutex              g_sceneLock;
    std::vector<SceneEvent> g_sceneEvents;
    // OStim sends a few events per scene change and the queue is drained every frame: this many means the main thread
    // is not running (a load), and later events are dropped rather than queued without end.
    constexpr std::size_t kMaxQueuedEvents = 1024;

    void QueueThread(OStim::Thread* a_thread, SceneKind a_kind)
    {
        if (!a_thread) return;
        SceneEvent e;
        e.kind = a_kind;
        e.thread = a_thread->getThreadID();
        for (std::uint32_t i = 0; i < a_thread->getActorCount(); ++i) {
            auto* ta = a_thread->getActor(i);
            auto* a = ta ? static_cast<RE::Actor*>(ta->getGameActor()) : nullptr;
            if (a) e.actors.push_back(a->GetFormID());
        }
        if (a_kind != SceneKind::kStop)
            if (auto* n = a_thread->getCurrentNode())
                if (const char* id = n->getNodeID()) e.node = Lowered(id);
        std::scoped_lock l(g_sceneLock);
        if (g_sceneEvents.size() < kMaxQueuedEvents) g_sceneEvents.push_back(std::move(e));
    }

    struct StartListener : OStim::ThreadEventListener
    {
        void listen(OStim::Thread* a_thread) override { QueueThread(a_thread, SceneKind::kStart); }
    } startListener;
    struct NodeListener : OStim::ThreadEventListener
    {
        void listen(OStim::Thread* a_thread) override { QueueThread(a_thread, SceneKind::kNode); }
    } nodeListener;
    struct SpeedListener : OStim::ThreadEventListener
    {
        void listen(OStim::Thread* a_thread) override { QueueThread(a_thread, SceneKind::kSpeed); }
    } speedListener;
    struct StopListener : OStim::ThreadEventListener
    {
        void listen(OStim::Thread* a_thread) override { QueueThread(a_thread, SceneKind::kStop); }
    } stopListener;

    // SKSE's own messaging functions (set in SKSEPluginLoad): its Dispatch takes the sender's plugin handle
    const SKSE::detail::SKSEMessagingInterface* g_rawMessaging = nullptr;

    // OStim answers the interface message on the listener it registers for every plugin while it loads. SE's SKSE
    // (1.5.97) adds that listener only to the plugin lists that exist at that moment (the plugins loaded before OStim,
    // plus a few spare slots), so for this plugin, loaded well after it, the message finds no OStim listener. Plugin
    // 1's list holds it, so the message is sent again as plugin 1 (or 2, when OStim is plugin 1 and so not on that
    // list; on handle 0, SKSE's own list, OStim has only its listener for SKSE's messages, which ignores this one).
    // With the receiver named, SKSE calls only OStim's listener, once.
    void AskOStimAsAnEarlierPlugin(OStim::InterfaceExchangeMessage& a_msg)
    {
        if (!g_rawMessaging || !g_rawMessaging->Dispatch || !GetModuleHandleW(L"OStim.dll")) return;
        const SKSE::PluginHandle self = SKSE::GetPluginHandle();
        for (SKSE::PluginHandle h = 1; h <= 2 && h < self && !a_msg.interfaceMap; ++h) {
            g_rawMessaging->Dispatch(h, OStim::InterfaceExchangeMessage::MESSAGE_TYPE, &a_msg, sizeof(a_msg), "OStim");
            if (a_msg.interfaceMap)
                SKSE::log::info("OStim's plugin interface: reached through plugin {}'s message list (this plugin loads after OStim)", h);
        }
    }

    void ConnectOStim()
    {
        OStim::InterfaceExchangeMessage msg;
        auto*                           messaging = SKSE::GetMessagingInterface();
        messaging->Dispatch(OStim::InterfaceExchangeMessage::MESSAGE_TYPE, &msg, sizeof(msg), "OStim");
        if (!msg.interfaceMap) messaging->Dispatch(OStim::InterfaceExchangeMessage::MESSAGE_TYPE, &msg, sizeof(msg), nullptr);
        if (!msg.interfaceMap) AskOStimAsAnEarlierPlugin(msg);
        if (!msg.interfaceMap) {
            SKSE::log::info("OStim's plugin interface not found (OStim Standalone 7.5+) - impacts count as outside OStim scenes");
            return;
        }
        oThreads = static_cast<OStim::ThreadInterface*>(msg.interfaceMap->queryInterface(OStim::ThreadInterface::NAME));
        if (!oThreads) {
            SKSE::log::error("OStim has no '{}' interface - impacts count as outside OStim scenes", OStim::ThreadInterface::NAME);
            return;
        }
        const auto v = oThreads->getVersion();
        SKSE::log::info("OStim thread interface v{}.{}.{}.{}", (v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 4) & 0xFFF, v & 0xF);
        oThreads->registerThreadStartListener(&startListener);
        oThreads->registerNodeChangedListener(&nodeListener);
        oThreads->registerSpeedChangedListener(&speedListener);
        oThreads->registerThreadStopListener(&stopListener);
    }

    // OThread.GetSpeed's answers (they may come back on a Papyrus thread)
    struct SpeedAnswer
    {
        std::int32_t  thread = -1;
        std::uint32_t serial = 0;
        int           speed = -1;
    };
    std::mutex               g_answerLock;
    std::vector<SpeedAnswer> g_speedAnswers;
    // About one question per scene a second, drained every frame: this many means the main thread is not running, and
    // later answers are dropped (the next question a second later replaces them).
    constexpr std::size_t kMaxAnswers = 256;
    // Numbers every speed question, across all threads, and never restarts: an answer is used only when it is newer
    // than the last change of its thread (OStim reuses thread ids, so a late answer may be about an earlier scene).
    std::uint32_t g_askSerial = 0;

    class SpeedReply : public RE::BSScript::IStackCallbackFunctor
    {
    public:
        SpeedReply(std::int32_t a_thread, std::uint32_t a_serial) :
            _thread(a_thread), _serial(a_serial)
        {}
        void operator()(RE::BSScript::Variable a_result) override
        {
            const int speed = a_result.IsInt() ? a_result.GetSInt() : -1;
            std::scoped_lock l(g_answerLock);
            if (g_speedAnswers.size() < kMaxAnswers) g_speedAnswers.push_back({ _thread, _serial, speed });
        }
        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

    private:
        std::int32_t  _thread;
        std::uint32_t _serial;
    };

    // a running OStim thread as the main thread knows it
    struct SceneThread
    {
        std::string       scene;          // its scene id (lower case)
        int               speed = -1;     // its speed index, -1 = not known
        std::uint32_t     asked = 0;      // the serial (g_askSerial) of the last speed question
        std::uint32_t     answered = 0;   // the serial of the answer in use; answers up to it are not used again
        Clock::time_point lastAsk{};
    };
    std::unordered_map<std::int32_t, SceneThread> threads;   // OStim thread id -> its state
    std::unordered_map<RE::FormID, std::int32_t>  inThread;  // actor -> the OStim thread she is in

    void AskSpeed(std::int32_t a_id, SceneThread& a_thread, Clock::time_point a_now)
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) return;
        a_thread.lastAsk = a_now;
        a_thread.asked = ++g_askSerial;
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb{ new SpeedReply(a_id, a_thread.asked) };
        static const RE::BSFixedString                           cls("OThread"), fn("GetSpeed");
        // The VM copies the argument values while dispatching; the small argument object is not freed afterwards
        // (a few bytes, no assumption about who owns it).
        if (!vm->DispatchStaticCall(cls, fn, RE::MakeFunctionArguments(static_cast<std::int32_t>(a_id)), cb)) {
            static bool once = false;
            if (!once) {
                once = true;
                SKSE::log::warn("OThread.GetSpeed could not be called - scene speeds stay unknown (speed-based scenes don't ripple)");
            }
        }
    }

    void ClearScenes()
    {
        {
            std::scoped_lock l(g_sceneLock);
            g_sceneEvents.clear();
        }
        {
            std::scoped_lock l(g_answerLock);
            g_speedAnswers.clear();
        }
        threads.clear();
        inThread.clear();
    }

    // ------------------------------------------------------------------------------------------------ SexLab scenes
    // Classic SexLab (1.6x) and SexLab P+ (2.x) run each scene on a thread quest (SexLab.esm's SexLabThread00 .. 14,
    // script sslThreadController). Each step of a scene goes out as SendModEvent(<step>, <thread number>) (SexLab.h),
    // which comes to us as SKSE's mod event, sent from the thread's own script while it runs - so the thread's variables
    // are read right there:
    //   classic  ::Positions_var (who is in the scene) and ::Stage_var (1, 2 ...), both set before the step goes out
    //   P+       _Positions. Its stage (its Stage property: the length of its stage history) is set only after its
    //            StageStart went out, so it is asked for with a call of our own (GetStageHistoryLength) from the main
    //            thread a moment after each step and every 2 s while the scene runs (SexLab.h PlusStagePoll).
    // Whether someone is still in a SexLab scene is SexLab's own test (its animating faction, rank 1 or more - SexLab
    // sets 0 as it lets them go at the end, -1 as it clears them): checked every second, for a missed end.
    // The steps come on Papyrus threads: they are only queued there (g_slLock, no game call while it is held) and the
    // main thread takes them in.
    RE::TESFaction*   g_slFaction = nullptr;   // SexLab.esm 00E50F SexLabAnimatingFaction (the same id in classic and P+)
    std::atomic<bool> g_slOn{ false };         // SexLab is here, and nothing went wrong reading it

    struct SexLabEvent
    {
        int                     thread = -1;
        SexLabStep              step = SexLabStep::kOther;
        bool                    plus = false;   // SexLab P+
        int                     stage = 0;      // classic: its stage (0 = not read); P+: 0 (asked for later)
        int                     before = 0;     // P+, a new stage's step: the stage before (its stage history's length then)
        RE::FormID              quest = 0;      // the thread quest
        std::vector<RE::FormID> actors;
    };
    struct SexLabAnswer
    {
        int           thread = -1;
        std::uint32_t serial = 0;
        int           stage = -1;
    };
    std::mutex                g_slLock;
    std::vector<SexLabEvent>  g_slEvents;
    std::vector<SexLabAnswer> g_slAnswers;

    // a running SexLab thread as the main thread knows it
    struct SexLabThread
    {
        RE::FormID              quest = 0;
        bool                    plus = false;
        int                     stage = 0;   // 0 = not known yet
        std::vector<RE::FormID> actors;
        PlusStagePoll           poll;        // P+: when its stage is asked for
        std::uint32_t           applied = 0; // P+: the serial of the answer in use
        std::uint32_t           stepSerial = 0;   // P+: the last serial asked before its last step (older answers don't count)
        int                     stageBefore = 0;  // P+: the stage before its last StageStart
        bool                    checked = false, canAsk = false;
        Clock::time_point       lastStep{};
    };
    std::unordered_map<int, SexLabThread> slThreads;   // thread number -> its state
    std::unordered_map<RE::FormID, int>   inSexLab;    // actor -> the SexLab thread she is in
    std::uint32_t                         g_slSerial = 0;
    Clock::time_point                     lastSexLabCheck{};

    bool SexLabActive(RE::Actor* a_actor)
    {
        return g_slFaction && a_actor && a_actor->GetFactionRank(g_slFaction, a_actor->IsPlayerRef()) >= 1;
    }

    // a thread quest's script (none: not a SexLab thread)
    RE::BSTSmartPointer<RE::BSScript::Object> ThreadScript(RE::TESForm* a_form)
    {
        RE::BSTSmartPointer<RE::BSScript::Object> obj;
        auto* quest = a_form ? a_form->As<RE::TESQuest>() : nullptr;
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
        if (!quest || !policy) return obj;
        const auto handle = policy->GetHandleForObject(RE::TESQuest::FORMTYPE, quest);
        if (!handle || !vm->FindBoundObject(handle, "sslThreadController", obj)) obj.reset();
        return obj;
    }

    // the script has this function (its own or one of the scripts it extends)
    bool HasFunction(RE::BSScript::Object* a_obj, std::string_view a_name)
    {
        for (auto* t = a_obj ? a_obj->GetTypeInfo() : nullptr; t; t = t->GetParent()) {
            const auto* f = t->GetMemberFuncIter();
            for (std::uint32_t i = 0; f && i < t->GetNumMemberFuncs(); ++i) {
                const char* n = f[i].func ? f[i].func->GetName().c_str() : nullptr;
                if (n && detail::SameName(n, a_name)) return true;
            }
        }
        return false;
    }

    // who an Actor[] holds (empty places left out)
    std::vector<RE::FormID> ScriptActors(const RE::BSScript::Variable* a_var)
    {
        std::vector<RE::FormID> out;
        if (!a_var || !a_var->IsArray()) return out;
        const auto arr = a_var->GetArray();
        if (!arr) return out;
        for (std::uint32_t i = 0; i < arr->size() && i < 16; ++i)
            if (auto* a = (*arr)[i].Unpack<RE::Actor*>()) out.push_back(a->GetFormID());
        return out;
    }

    // a SexLab thread's step (on the Papyrus thread its script runs on, inside its SendModEvent): read and queued
    void SexLabStepCpp(const SKSE::ModCallbackEvent& a_event)
    {
        const char* name = a_event.eventName.c_str();
        const auto  step = ClassifySexLabStep(name ? name : "");
        if (step == SexLabStep::kOther) return;
        const char* arg = a_event.strArg.c_str();
        const int   thread = SexLabThreadNumber(arg ? arg : "");
        if (thread < 0 || !a_event.sender) return;
        auto obj = ThreadScript(a_event.sender);
        if (!obj) return;   // another mod's event of the same name
        SexLabEvent e;
        e.thread = thread;
        e.step = step;
        e.quest = a_event.sender->GetFormID();
        if (const auto* pos = obj->GetVariable("_Positions")) {   // SexLab P+: its stage is asked for later
            e.plus = true;
            e.actors = ScriptActors(pos);
            // at a new stage's step its stage history still holds the stages before (the new one is added after the
            // step): its length is the stage before
            if (step == SexLabStep::kNewStage)
                if (const auto* h = obj->GetVariable("_StageHistory"); h && h->IsArray())
                    if (const auto arr = h->GetArray()) e.before = static_cast<int>(arr->size());
        } else {   // classic SexLab: its stage at every step (moving the scene or changing its actors restarts it at stage 1
                   // with no StageStart - only ActorChangeEnd / ActorsRelocated)
            e.actors = ScriptActors(obj->GetVariable("::Positions_var"));
            if (step != SexLabStep::kEnd)
                if (const auto* st = obj->GetVariable("::Stage_var"); st && st->IsInt()) e.stage = st->GetSInt();
        }
        if (g_logSteps.load(std::memory_order_relaxed))
            SKSE::log::info("SexLab{} thread {}: {} - {} in the scene{}", e.plus ? " P+" : "", thread, name, e.actors.size(),
                e.stage > 0 ? std::format(", stage {}", e.stage) : std::string());
        std::scoped_lock l(g_slLock);
        if (g_slEvents.size() < 1024) g_slEvents.push_back(std::move(e));
    }

    // ---- a fault reading a step must not take the game down: SexLab scenes switch off for the session instead
    DWORD          g_slFaultCode = 0;
    std::uintptr_t g_slFaultAt = 0;
    bool           SexLabStepTry(const SKSE::ModCallbackEvent& a_event)
    {
        try {
            SexLabStepCpp(a_event);
            return true;
        } catch (const std::exception& e) {
            SKSE::log::error("SexLab: {} - SexLab scenes are not followed for the rest of this session", e.what());
        } catch (...) {
            SKSE::log::error("SexLab: an unknown exception - SexLab scenes are not followed for the rest of this session");
        }
        return false;
    }
    int SexLabFaultFilter(EXCEPTION_POINTERS* a_ep)
    {
        g_slFaultCode = a_ep->ExceptionRecord->ExceptionCode;
        g_slFaultAt = reinterpret_cast<std::uintptr_t>(a_ep->ExceptionRecord->ExceptionAddress);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    int SexLabStepSafe(const SKSE::ModCallbackEvent& a_event)
    {
        __try {
            return SexLabStepTry(a_event) ? 1 : 0;
        } __except (SexLabFaultFilter(GetExceptionInformation())) {
            return -1;
        }
    }
    struct SexLabSink : RE::BSTEventSink<SKSE::ModCallbackEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
        {
            // (SexLab = 0: nothing of SexLab's is read at all)
            if (!a_event || !g_slOn.load(std::memory_order_relaxed) || !g_slFollow.load(std::memory_order_relaxed))
                return RE::BSEventNotifyControl::kContinue;
            if (const int r = SexLabStepSafe(*a_event); r != 1) {
                g_slOn = false;
                if (r < 0)
                    SKSE::log::error("SexLab: fault {:08X} at {:X} reading a scene step - SexLab scenes are not followed for the rest of this session",
                        g_slFaultCode, g_slFaultAt);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    } sexLabSink;

    // at the data load: SexLab.esm and its animating faction there = its scenes are followed
    void ConnectSexLab()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh || !dh->LookupModByName("SexLab.esm")) {
            SKSE::log::info("SexLab.esm isn't loaded - no SexLab scenes");
            return;
        }
        g_slFaction = dh->LookupForm<RE::TESFaction>(0x00E50F, "SexLab.esm");
        auto* src = SKSE::GetModCallbackEventSource();
        if (!g_slFaction || !src) {
            SKSE::log::error("SexLab.esm is loaded, but {} - no SexLab scenes", !g_slFaction ? "its animating faction (00E50F) isn't there" : "SKSE's mod events aren't");
            return;
        }
        src->AddEventSink(&sexLabSink);
        g_slOn = true;
        SKSE::log::info("SexLab found - classic SexLab or SexLab P+ (its animating faction {:08X}); whether its scenes are followed: the "
                        "\"SexLab scenes\" line above",
            g_slFaction->GetFormID());
    }

    // a P+ stage question's answer (on a Papyrus thread)
    class SexLabReply : public RE::BSScript::IStackCallbackFunctor
    {
    public:
        SexLabReply(int a_thread, std::uint32_t a_serial) : _thread(a_thread), _serial(a_serial) {}
        void operator()(RE::BSScript::Variable a_result) override
        {
            const int stage = a_result.IsInt() ? a_result.GetSInt() : -1;
            std::scoped_lock l(g_slLock);
            if (g_slAnswers.size() < kMaxAnswers) g_slAnswers.push_back({ _thread, _serial, stage });
        }
        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

    private:
        int           _thread;
        std::uint32_t _serial;
    };

    void ClearSexLab()
    {
        {
            std::scoped_lock l(g_slLock);
            g_slEvents.clear();
            g_slAnswers.clear();
        }
        slThreads.clear();
        inSexLab.clear();
    }

    // ------------------------------------------------------------------------------------------------ device access
    ID3D11Device* Device() { return reinterpret_cast<ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice()); }
    ID3D11DeviceContext* Context()
    {
        auto* rd = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        return rd ? reinterpret_cast<ID3D11DeviceContext*>(rd->context) : nullptr;
    }
    ID3D11Buffer*     AsD3D(RE::ID3D11Buffer* a_b) { return reinterpret_cast<ID3D11Buffer*>(a_b); }
    RE::ID3D11Buffer* AsRE(ID3D11Buffer* a_b) { return reinterpret_cast<RE::ID3D11Buffer*>(a_b); }

    // ------------------------------------------------------------------------------------------------ vertex data access
    std::uint64_t DescBits(const RE::BSGraphics::VertexDesc& a_desc)
    {
        std::uint64_t v;
        std::memcpy(&v, &a_desc, sizeof(v));
        return v;
    }
    void SetDescBits(RE::BSGraphics::VertexDesc& a_desc, std::uint64_t a_bits) { std::memcpy(&a_desc, &a_bits, sizeof(a_bits)); }

    V3 ReadPos(const std::uint8_t* a_v, bool a_full)
    {
        if (a_full) {
            const float* f = reinterpret_cast<const float*>(a_v);
            return { f[0], f[1], f[2] };
        }
        const auto* h = reinterpret_cast<const DirectX::PackedVector::HALF*>(a_v);
        return { DirectX::PackedVector::XMConvertHalfToFloat(h[0]), DirectX::PackedVector::XMConvertHalfToFloat(h[1]),
            DirectX::PackedVector::XMConvertHalfToFloat(h[2]) };
    }

    void WritePos(std::uint8_t* a_v, bool a_full, const V3& a_p)
    {
        if (a_full) {
            float* f = reinterpret_cast<float*>(a_v);
            f[0] = a_p.x;
            f[1] = a_p.y;
            f[2] = a_p.z;
            return;
        }
        auto* h = reinterpret_cast<DirectX::PackedVector::HALF*>(a_v);
        h[0] = DirectX::PackedVector::XMConvertFloatToHalf(a_p.x);
        h[1] = DirectX::PackedVector::XMConvertFloatToHalf(a_p.y);
        h[2] = DirectX::PackedVector::XMConvertFloatToHalf(a_p.z);
    }

    V3 ReadNormal(const std::uint8_t* a_b) { return { a_b[0] / 127.5f - 1.0f, a_b[1] / 127.5f - 1.0f, a_b[2] / 127.5f - 1.0f }; }
    void WriteNormal(std::uint8_t* a_b, const V3& a_n)
    {
        auto q = [](float x) { return static_cast<std::uint8_t>(std::clamp(std::lround((x + 1.0f) * 127.5f), 0L, 255L)); };
        a_b[0] = q(a_n.x);
        a_b[1] = q(a_n.y);
        a_b[2] = q(a_n.z);
    }

    // ------------------------------------------------------------------------------------------------ a rippling shape
    // One impact on one shape: when, how strong, and the wave centre of each lobe (Regions.h) - lobes says which are set
    // (the regions the shape had when it fired: a wave carried over to a shape set up with other regions skips the rest).
    struct Event
    {
        float                  t0{ 0 }, s{ 1 };
        std::array<V3, kLobes> c{};
        std::uint8_t           lobes{ 0 };
    };

    // how a shape shows the shading bands
    enum class Colour
    {
        kNone,    // no shading (outline / tilted normals only)
        kOwn,     // its own vertex colours, the shader already uses them
        kFlag,    // its own white vertex colours: the shader's vertex colour flag is set while it ripples
        kLayer    // no vertex colours: colours in the position's unused w, in the drawn copy only, while it ripples
    };

    struct RShape
    {
        RE::NiPointer<RE::BSGeometry>      geom;
        RE::NiPointer<RE::NiSkinPartition> part;
        std::string                        name, who;   // the shape's name, the woman's name (for the log)
        RE::FormID                         owner{ 0 };
        // the mesh as it was set up (partition 0's data block)
        const std::uint8_t* raw{ nullptr };
        std::uint64_t       desc{ 0 };
        ID3D11Buffer*       vb{ nullptr };
        std::uint32_t       vcount{ 0 }, stride{ 0 }, bytes{ 0 };
        bool                fullPrec{ false }, writeN{ false }, msn{ false };
        int                 nOff{ -1 }, cOff{ -1 };
        Colour              colour{ Colour::kNone };
        Colour              built{ Colour::kNone };   // what the setup chose: a shape that lost its bands since is set up anew
        ColourLayout        layout;    // kLayer: the same layout with the colour in the position's w (AliasColour)
        bool                shared{ false };   // another woman draws from the same mesh: never rippled
        // while it ripples
        bool                                swapped{ false };
        bool                                layered{ false };  // kLayer: the data blocks carry the colour layout now
        ID3D11Buffer*                       orig{ nullptr };   // the buffer the game drew from
        ID3D11Buffer*                       mine{ nullptr };   // our copy, the one drawn (video memory)
        ID3D11Buffer*                       upload{ nullptr }; // where each frame's data is written, copied into mine on the GPU
        std::vector<Tri*>                   tris;              // the data blocks we pointed at our buffer
        RE::NiPointer<RE::NiAVObject>       top;               // her 3D's root (the scan's): copies of her meshes live under it
        // other geometries under her 3D that draw our blocks with a vertex colour flag on (a RaceMenu overlay built while
        // she rippled copies her shader's flags): BeforeRender takes their flag off should their blocks lose the colours
        std::vector<RE::NiPointer<RE::BSGeometry>> flagged;
        RE::NiPointer<RE::BSShaderProperty> flagProp;          // the shader whose vertex colour flag we set
        RE::NiPointer<RE::BSShaderProperty> passProp;          // Debug: the shader whose rebuilt render passes are logged
        int                                 passLog{ 0 };      //   in this many frames
        const std::uint8_t*                 curRaw{ nullptr }; // the data / layout drawn from now
        std::uint32_t                       curStride{ 0 }, curBytes{ 0 };
        int                                 curCOff{ -1 };
        // ripple data: per ripple vertex its first lobe (cheek = 0 / 1 for the butt) and weight, a second lobe and weight
        // where two regions overlap (kNoLobe: none), whether it may take an impact's wave centre for its lobe
        std::vector<std::uint32_t> idx;
        std::vector<float>         w, w2;
        std::vector<std::uint8_t>  cheek, lobe2, hit;
        std::vector<V3>            P, N, Nst;   // rest position, welded normal, stored normal
        std::array<V3, kLobes>     fallback{};  // each lobe's wave centre when no impact point is near it
        std::array<float, kLobes>  lobeR{};     // each lobe's radius: an impact further than this starts at the fallback
        std::uint8_t               lobes{ 0 };  // the lobes this shape has (bit per lobe)
        V3                         mid{};
        float                      sep{ 0 }, R{ 0 };
        std::uint32_t              pelvisIdx{ 0 };
        RE::NiTransform            pelvisS2B;
        std::vector<Event>         events;
    };

    // the skin partitions of her skinned geometries and the vertex buffers they draw from (sorted, distinct)
    struct MeshUse
    {
        std::vector<const void*> parts, buffers;
        // RaceMenu / NiOverride overlay geometries among them ("[Ovl" / "[SOvl"): name, vertex count (0: not skinned)
        std::vector<std::pair<std::string, std::uint32_t>> overlays;
    };

    struct Tracked
    {
        RE::ActorHandle                      handle;
        RE::FormID                           id{ 0 };
        std::string                          name;
        RE::NiAVObject*                      root{ nullptr };
        std::vector<std::unique_ptr<RShape>> shapes;
        std::vector<const void*>             skinned;   // partitions at the last scan (outfit change check)
        MeshUse                              use;       // what her 3D draws from now (shared mesh check)
        bool                                 seen{ false }, rescan{ false }, sharedLogged{ false };
        std::string                          overlayLine;   // her RaceMenu / NiOverride overlays as last logged
        std::string                          ruleLine;  // the last decision logged for her
        // detection
        RE::FormID     partner{ 0 };
        std::string    partnerName;
        StrokeDetector strokes;       // the distance to that partner and his thrusts
        ContactGate    landings;      // motion detection: where his recent thrusts landed
        ImpactTiming   timing;        // which of his thrusts / impact sounds ripple
        V3             landContact;   // where his last thrust landed (world) and how: for a sound right after it
        Stroke         landStroke;
        float          lastImpact{ -100.0f };
        // calibration log
        float statT{ 0 }, statMin{ 1e9f }, statMax{ 0 }, statPeak{ 0 };
        int   statImpacts{ 0 }, statStrokes{ 0 }, statSkipped{ 0 };
    };

    struct Partner
    {
        RE::FormID                    id{ 0 };
        std::string                   name;
        RE::NiPointer<RE::NiAVObject> contact, pelvis;
    };

    std::vector<std::unique_ptr<Tracked>>                tracked;
    std::vector<Partner>                                 partners;
    std::vector<std::pair<ID3D11Buffer*, std::uint64_t>> graveyard;   // released a few frames after use
    std::unordered_set<std::string>                      layerLogged; // shapes whose colour layer was announced
    std::unordered_set<std::string>                      brokenLogged; // shapes whose broken vertex data was announced
    std::uint64_t                                        frameNo{ 0 };
    Clock::time_point                                    startTime = Clock::now(), lastRefresh{}, lastFrame{};
    std::atomic<bool>                                    testRequested{ false };

    float Now() { return std::chrono::duration<float>(Clock::now() - startTime).count(); }

    const char* NameOf(RE::Actor* a_actor)
    {
        const char* nm = a_actor ? a_actor->GetDisplayFullName() : nullptr;
        return nm && *nm ? nm : "?";
    }

    // ------------------------------------------------------------------------------------------------ buffer swap
    // The data blocks (TriShapes) of a partition as they are now: its partitions' blocks, distinct.
    void AddTris(const RE::NiSkinPartition* a_part, std::vector<Tri*>& a_out)
    {
        if (!a_part) return;
        for (std::uint32_t i = 0; i < a_part->numPartitions; ++i)
            if (auto* t = a_part->partitions[i].buffData; t && std::find(a_out.begin(), a_out.end(), t) == a_out.end()) a_out.push_back(t);
    }

    // every data block this shape's geometry and partition hold now: the partition's, the geometry's renderer data
    std::vector<Tri*> CurrentTris(const RShape& a_s)
    {
        std::vector<Tri*> out;
        AddTris(a_s.part.get(), out);
        if (a_s.geom)
            if (auto* rd = a_s.geom->GetGeometryRuntimeData().rendererData; rd && std::find(out.begin(), out.end(), rd) == out.end())
                out.push_back(rd);
        return out;
    }

    Tri* FirstTri(const RShape& a_s)
    {
        auto* part = a_s.part.get();
        return part && part->numPartitions ? part->partitions[0].buffData : nullptr;
    }

    bool Holds(const std::vector<Tri*>& a_list, const Tri* a_t) { return std::find(a_list.begin(), a_list.end(), a_t) != a_list.end(); }

    // A skinned decal (blood splatter on her body): its skin data is not laid out as a mesh's - its bone list and blocks
    // must never be read (reading its bones' names crashed the game). It never ripples and is never looked into.
    bool IsDecal(RE::BSGeometry* a_g) { return a_g && a_g->AsSkinnedDecalTriShape() != nullptr; }

    // Every geometry under her 3D root a_top but a_skip (and skinned decals), with its partition and data blocks (RaceMenu /
    // NiOverride overlays share the body's blocks; effect shader membranes and refits copy the geometry):
    // a_fn(geometry, partition, blocks).
    template <class F>
    void ForOtherDrawers(RE::NiAVObject* a_top, const RE::BSGeometry* a_skip, F&& a_fn)
    {
        if (!a_top) return;
        RE::BSVisit::TraverseScenegraphGeometries(a_top, [&](RE::BSGeometry* g) {
            if (g == a_skip || IsDecal(g)) return RE::BSVisit::BSVisitControl::kContinue;
            auto&             grd = g->GetGeometryRuntimeData();
            auto*             skin = grd.skinInstance.get();
            auto*             part = skin ? skin->skinPartition.get() : nullptr;
            std::vector<Tri*> ts;
            AddTris(part, ts);
            if (auto* rd = grd.rendererData; rd && !Holds(ts, rd)) ts.push_back(rd);
            a_fn(g, part, ts);
            return RE::BSVisit::BSVisitControl::kContinue;
        });
    }

    // The vertex colour flag sits on the geometry's shader, which every partition of it is drawn with. So the colour
    // layer (and setting the flag) needs the whole geometry to be one mesh: every partition's data block the same
    // vertex buffer, vertex data and layout as partition 0's, and the renderer data too when it draws from that buffer.
    // Partitions with their own copies or buffers would be drawn with the flag and a layout without colours.
    bool OneDataBlock(RE::BSGeometry* a_geom, const RE::NiSkinPartition* a_part)
    {
        const Tri* bd0 = a_part && a_part->numPartitions ? a_part->partitions[0].buffData : nullptr;
        if (!bd0) return false;
        const auto twin = [&](const Tri* a_t) {
            return a_t->vertexBuffer == bd0->vertexBuffer && a_t->rawVertexData == bd0->rawVertexData && DescBits(a_t->vertexDesc) == DescBits(bd0->vertexDesc);
        };
        for (std::uint32_t i = 1; i < a_part->numPartitions; ++i) {
            const Tri* t = a_part->partitions[i].buffData;
            if (!t || !twin(t)) return false;
        }
        if (a_geom)
            if (const Tri* rd = a_geom->GetGeometryRuntimeData().rendererData; rd && rd->vertexBuffer == bd0->vertexBuffer && !twin(rd)) return false;
        return true;
    }

    // The vertex colour flag of a live shader. The property keeps the render passes it built - the lighting technique,
    // vertex colours included, is fixed in them - until its lastRenderPassState stops matching. The game's own
    // BSShaderProperty::SetFlags marks them stale (INT_MAX) whenever a bit changes; a plain bit write would leave the
    // old passes drawing. Tick runs after the frame's render, so the next render builds the passes anew before it draws.
    void SetVertexColours(RE::BSShaderProperty* a_sp, bool a_on)
    {
        if (a_on)
            a_sp->flags.set(RE::BSShaderProperty::EShaderPropertyFlag::kVertexColors);
        else
            a_sp->flags.reset(RE::BSShaderProperty::EShaderPropertyFlag::kVertexColors);
        a_sp->lastRenderPassState = (std::numeric_limits<std::int32_t>::max)();
    }

    // Debug: log the shader's render passes a couple of frames from now (after the render has rebuilt them)
    void WatchPasses(RShape& a_s, RE::BSShaderProperty* a_sp)
    {
        if (!cfg.debug || !a_sp) return;
        a_s.passProp.reset(a_sp);
        a_s.passLog = 2;
    }

    // The colour layer's colours, white (no darkening, opaque: the shaders read the alpha too), on a_count vertices.
    void PaintWhite(std::uint8_t* a_data, std::uint32_t a_count, std::uint32_t a_stride, std::uint32_t a_colourOffset)
    {
        for (std::uint32_t v = 0; v < a_count; ++v) std::memset(a_data + static_cast<std::size_t>(v) * a_stride + a_colourOffset, 0xFF, kColourSize);
    }

    // Every block of a partition carries vertex colours in its layout: a shader with the vertex colour flag may draw it.
    bool HasColours(const RE::NiSkinPartition* a_part)
    {
        if (!a_part || !a_part->numPartitions) return false;
        for (std::uint32_t i = 0; i < a_part->numPartitions; ++i)
            if (const Tri* t = a_part->partitions[i].buffData; !t || !HasAttribute(DescBits(t->vertexDesc), Attribute::kColour)) return false;
        return true;
    }

    RE::NiSkinPartition* PartitionOf(const RE::BSGeometry* a_g)
    {
        auto* skin = a_g ? a_g->GetGeometryRuntimeData().skinInstance.get() : nullptr;
        return skin ? skin->skinPartition.get() : nullptr;
    }

    // A copy of her body (a geometry under her 3D drawing her body's partition - ours, or the one her geometry draws
    // now): its vertex colour flag, if a copy of hers, goes off when the blocks it draws have no colours, and with
    // a_layerGoing also when it draws our partition (its colours go with the layer). Our own flag is flagProp's.
    void DropCopiedFlag(RE::BSGeometry* a_g, const RE::NiSkinPartition* a_part, const RE::NiSkinPartition* a_ours, bool a_layerGoing)
    {
        auto* sp = a_g ? a_g->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
        if (!sp || !sp->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kVertexColors)) return;
        if ((a_layerGoing && a_part == a_ours) || !HasColours(a_part)) SetVertexColours(sp, false);
    }

    // Put the shape back the way the game set it up, in this order: the vertex colour flags first - ours, and any that
    // a copy of her mesh took over from her shader while she rippled (a RaceMenu overlay built meanwhile copies its
    // flags) - so no shader ever reads colours from a block without them (a vertex shader that reads colours meeting a
    // layout without them gets no input layout, and the game keeps that failure for the session); then the original
    // buffer and layout on every block that still draws from our buffer; then our buffer goes. Her vertex data, her skin
    // partition's layouts and her geometry's layout were never changed (AliasColour): nothing else goes back.
    // What someone else put in since is left alone, and what may have changed hands is kept rather than freed: a leak
    // is safe, a double free is not.
    void Restore(RShape& a_s)
    {
        if (!a_s.swapped) return;
        const bool layer = a_s.layered;
        const auto L = a_s.layout;
        // 1. the flags, their render passes marked stale: ours, then every copy of her body under her 3D as it is NOW
        // (a flag on blocks without colours, or on our blocks whose colours go now, can only be a copy of ours), and
        // every geometry we saw drawing ours with a copied flag (one detached meanwhile too)
        if (a_s.flagProp) {
            SetVertexColours(a_s.flagProp.get(), false);
            WatchPasses(a_s, a_s.flagProp.get());
            a_s.flagProp.reset();
        }
        const auto* herNow = PartitionOf(a_s.geom.get());
        bool        heldMine = false;   // a block we did not give our buffer to draws from it (a copy of her mesh)
        ForOtherDrawers(a_s.top.get(), nullptr, [&](RE::BSGeometry* g, auto* p, const auto& ts) {
            for (const Tri* t : ts)
                if (!Holds(a_s.tris, t) && AsD3D(t->vertexBuffer) == a_s.mine) heldMine = true;
            if (p && (p == a_s.part.get() || p == herNow)) DropCopiedFlag(g, p, a_s.part.get(), layer);
        });
        for (auto& g : a_s.flagged)
            if (g) {
                if (auto* sp = g->GetGeometryRuntimeData().shaderProperty.get(); layer && sp && sp->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kVertexColors))
                    SetVertexColours(sp, false);
                DropCopiedFlag(g.get(), PartitionOf(g.get()), a_s.part.get(), layer);
            }
        a_s.flagged.clear();
        // every block we changed is still there and still ours: nobody else took over our buffer
        const auto now = CurrentTris(a_s);
        bool       intact = true;
        for (auto* t : a_s.tris)
            if (!Holds(now, t) || AsD3D(t->vertexBuffer) != a_s.mine || (layer && DescBits(t->vertexDesc) != L.desc)) intact = false;
        // 2. block by block (no flag reads colours any more): our layout comes off every block that still has it, and a
        // block that still draws from our buffer gets the original back, giving up the reference SwapIn gave it. A block
        // that draws from another buffer now is someone else's. (Our partition is held: its blocks are alive even when
        // her geometry draws a new one by now.)
        for (auto* t : a_s.tris) {
            if (!Holds(now, t)) continue;   // that block is gone: never touched again
            if (layer && DescBits(t->vertexDesc) == L.desc) SetDescBits(t->vertexDesc, a_s.desc);
            if (AsD3D(t->vertexBuffer) != a_s.mine) continue;   // someone else's now
            t->vertexBuffer = AsRE(a_s.orig);
            a_s.mine->Release();
        }
        // a copy that keeps drawing from our buffer gets her rest shape in it (no frozen wave; white colours, in case it
        // took over our layout) - only while the buffer and her data are certainly ours and alive
        if (heldMine && intact && a_s.mine && a_s.upload && a_s.raw && FirstTri(a_s) && FirstTri(a_s)->rawVertexData == a_s.raw)
            if (auto* ctx = Context()) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(ctx->Map(a_s.upload, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) && m.pData) {
                    std::memcpy(m.pData, a_s.raw, a_s.bytes);
                    if (layer) PaintWhite(static_cast<std::uint8_t*>(m.pData), a_s.vcount, a_s.stride, L.colourOffset);
                    ctx->Unmap(a_s.upload, 0);
                    ctx->CopyResource(a_s.mine, a_s.upload);
                }
            }
        // the upload buffer is only ours (no block draws from it): released now (the runtime keeps it until the GPU's
        // copy from it is done)
        if (a_s.upload) a_s.upload->Release();
        a_s.upload = nullptr;
        // 3. our buffer: released a few frames later - unless a block we gave it to holds another one now (whoever
        // replaced it may have released it), or a copy of her mesh still draws from it
        if (intact && !heldMine) {
            graveyard.emplace_back(a_s.mine, frameNo);
        } else {
            static bool once = false;
            if (!once) {
                once = true;
                SKSE::log::warn("'{}' of {}: {} - left to them", a_s.name, a_s.who,
                    heldMine ? "a copy of her mesh made while it rippled draws from its vertex buffer" : "its vertex buffer was replaced by someone else while it rippled");
            }
        }
        a_s.mine = nullptr;
        a_s.orig = nullptr;
        a_s.tris.clear();
        a_s.curRaw = nullptr;
        a_s.swapped = false;
        a_s.layered = false;
    }

    // Swap our dynamic copy in: a buffer of the same size made from her own vertex data (with the colour layer, every
    // colour white in the position's w), our buffer on every data block that draws from the original - with the colour
    // layer, the colour layout too, on all of those blocks or on none - and LAST the shader's vertex colour flag. Her
    // vertex data, her partitions' layouts and her geometry's layout stay the game's: a copy of her mesh made at any
    // moment, from any thread, is the game's own. The layer and the flag are used only when every partition draws that
    // one mesh (OneDataBlock). A step that fails leaves everything as it was.
    bool SwapIn(RShape& a_s)
    {
        if (a_s.swapped) return true;
        auto* dev = Device();
        auto* bd0 = FirstTri(a_s);
        if (!dev || !bd0 || !bd0->vertexBuffer || bd0->rawVertexData != a_s.raw || DescBits(bd0->vertexDesc) != a_s.desc) return false;
        // the colour layer and the flag need the whole geometry to be one mesh (OneDataBlock): a partition split up
        // since the scan (another plugin gave its partitions their own data) ripples without the shading bands
        if ((a_s.colour == Colour::kLayer || a_s.colour == Colour::kFlag) && !OneDataBlock(a_s.geom.get(), a_s.part.get())) {
            SKSE::log::info("'{}' of {}: its partitions no longer share one data block - it ripples without shading bands", a_s.name, a_s.who);
            a_s.colour = Colour::kNone;
        }
        ID3D11Buffer* origVB = AsD3D(bd0->vertexBuffer);
        const auto&   L = a_s.layout;
        // every block that still draws from the original
        std::vector<Tri*> tris;
        for (auto* t : CurrentTris(a_s))
            if (AsD3D(t->vertexBuffer) == origVB) tris.push_back(t);
        if (tris.empty()) return false;   // cannot happen (block 0 matched)
        // the colour layout goes on every one of them or on none: each still the game's (her data, her layout), and
        // every partition drawing one of them - a block left without it would be drawn by a shader reading colours
        bool layered = a_s.colour == Colour::kLayer;
        if (layered) {
            for (const auto* t : tris)
                if (t->rawVertexData != a_s.raw || DescBits(t->vertexDesc) != a_s.desc) layered = false;
            for (std::uint32_t i = 0; i < a_s.part->numPartitions; ++i)
                if (!Holds(tris, a_s.part->partitions[i].buffData)) layered = false;
            if (!layered) {
                SKSE::log::info("'{}' of {}: not every data block can take the colour layer - it ripples without shading bands", a_s.name, a_s.who);
                a_s.colour = Colour::kNone;
            }
        }
        // the first frame's data: hers, with white colours in the layer
        std::vector<std::uint8_t> first;
        const std::uint8_t*       data = a_s.raw;
        if (layered) {
            first.assign(a_s.raw, a_s.raw + a_s.bytes);
            PaintWhite(first.data(), a_s.vcount, a_s.stride, L.colourOffset);
            data = first.data();
        }
        // Two buffers: the one drawn lives in video memory (DEFAULT), so every draw of her body - and of each copy of it,
        // RaceMenu overlays, in every render pass - reads it there; each frame's data is written to the upload buffer
        // (DYNAMIC) and copied into it once on the GPU. (A DYNAMIC buffer drawn directly is read across the bus by
        // every one of those draws: frames several ms slower with many overlays.)
        const std::uint32_t bytes = a_s.bytes;
        D3D11_BUFFER_DESC   bd{};
        bd.ByteWidth = bytes;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{ data, 0, 0 };
        ID3D11Buffer*          mine = nullptr;
        if (FAILED(dev->CreateBuffer(&bd, &sd, &mine)) || !mine) {
            SKSE::log::error("'{}' of {}: could not create a vertex buffer ({} bytes)", a_s.name, a_s.who, bytes);
            return false;
        }
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Buffer* upload = nullptr;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &upload)) || !upload) {
            SKSE::log::error("'{}' of {}: could not create an upload buffer ({} bytes)", a_s.name, a_s.who, bytes);
            mine->Release();
            return false;
        }

        // our buffer on every block, the colour layout with it (the flag is still off and nothing draws now: Tick runs
        // after the frame's render, on the main thread). Each block holds its own reference, as the game's own blocks
        // do: a block the engine destroys or rebuilds releases its reference, never the others'. Ours (for Draw) is the
        // one CreateBuffer gave.
        for (auto* t : tris) {
            t->vertexBuffer = AsRE(mine);
            mine->AddRef();
            if (layered) SetDescBits(t->vertexDesc, L.desc);
        }
        // LAST: the shader reads vertex colours only once every block holds them (its passes are rebuilt with them by
        // the next render, SetVertexColours)
        if (layered || a_s.colour == Colour::kFlag) {
            auto* sp = a_s.geom->lightingShaderProp_cast();
            if (sp && !sp->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kVertexColors)) {
                SetVertexColours(sp, true);
                a_s.flagProp.reset(sp);
                WatchPasses(a_s, sp);
            }
        }
        a_s.tris = std::move(tris);
        a_s.orig = origVB;
        a_s.mine = mine;
        a_s.upload = upload;
        a_s.layered = layered;
        a_s.curRaw = a_s.raw;
        a_s.curStride = a_s.stride;
        a_s.curCOff = layered ? static_cast<int>(L.colourOffset) : a_s.cOff;
        a_s.curBytes = bytes;
        a_s.swapped = true;
        if (layered && layerLogged.insert(std::format("{:08X}/{}", a_s.owner, a_s.name)).second)
            SKSE::log::info("'{}' of {}: colour layer added for the ripple (in the copy the game draws, layout size unchanged: stride {})", a_s.name, a_s.who,
                a_s.stride);
        if (cfg.debug)
            SKSE::log::info("  '{}': rippling - vertex buffer swapped on {} data block(s){}", a_s.name, a_s.tris.size(),
                a_s.flagProp ? ", vertex colour flag set" : "");
        return true;
    }

    void FlushGraveyard(bool a_all)
    {
        std::erase_if(graveyard, [&](auto& g) {
            if (a_all || frameNo - g.second > 3) {
                if (g.first) g.first->Release();
                return true;
            }
            return false;
        });
    }

    // the shape is still the one the game draws, from the same skin partition and data we set up
    bool StillValid(const RShape& a_s)
    {
        auto* g = a_s.geom.get();
        if (!g || !g->parent) return false;
        auto* skin = g->GetGeometryRuntimeData().skinInstance.get();
        if (!skin || skin->skinPartition.get() != a_s.part.get()) return false;
        auto* bd0 = FirstTri(a_s);
        if (!bd0) return false;
        if (!a_s.swapped) return bd0->vertexBuffer && bd0->rawVertexData == a_s.raw && DescBits(bd0->vertexDesc) == a_s.desc;
        // swapped: block 0 still draws our buffer from the data we copy every frame (Draw reads it), in our layout
        const std::uint64_t desc = a_s.layered ? a_s.layout.desc : a_s.desc;
        const auto          ours = [&](const Tri* a_t) {
            return a_t && AsD3D(a_t->vertexBuffer) == a_s.mine && a_t->rawVertexData == a_s.curRaw && DescBits(a_t->vertexDesc) == desc;
        };
        if (!ours(bd0)) return false;
        // with the colour layer or our vertex colour flag: every partition still draws that mesh (none was rebuilt
        // without colours)
        if (a_s.layered || a_s.flagProp)
            for (std::uint32_t i = 1; i < a_s.part->numPartitions; ++i)
                if (!ours(a_s.part->partitions[i].buffData)) return false;
        return true;
    }

    // ------------------------------------------------------------------------------------------------ shape setup
    // A skinned shape with fewer vertices is an accessory (a strap, a buckle), not a body or a garment worth rippling.
    constexpr std::uint32_t kMinShapeVertices = 300;
    // A shape with fewer vertices inside the rippling area covers too little of the buttocks to show a wave.
    constexpr std::size_t kMinRippleVertices = 40;

    std::unique_ptr<RShape> BuildShape(RE::BSGeometry* a_geom, const Tracked& a_t)
    {
        static const RE::BSFixedString lButt("NPC L Butt"), rButt("NPC R Butt"), pelvis("NPC Pelvis [Pelv]");
        if (a_geom->GetFlags().any(RE::NiAVObject::Flag::kHidden)) return nullptr;   // SMP collision helpers etc.
        if (IsDecal(a_geom)) return nullptr;   // a blood splatter on her body: never a mesh to ripple (its bones can't be read)
        // RaceMenu overlay slots that hold nothing (its blank default texture) are not worth rewriting every frame
        std::string nm = a_geom->name.c_str();
        if (nm.find("[Ovl") != std::string::npos || nm.find("[SOvl") != std::string::npos) {
            std::string diffuse;
            if (auto* lsp = a_geom->lightingShaderProp_cast()) {
                auto* mat = static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material);
                if (mat && mat->textureSet)
                    if (const char* d = mat->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) diffuse = d;
            }
            detail::LowerInPlace(diffuse);
            if (diffuse.empty() || diffuse.find("default.dds") != std::string::npos) return nullptr;
        }
        auto& grd = a_geom->GetGeometryRuntimeData();
        auto* skin = grd.skinInstance.get();
        if (!skin || !skin->skinData || !skin->skinPartition || !skin->bones) return nullptr;
        auto* part = skin->skinPartition.get();
        if (part->numPartitions == 0 || part->vertexCount < kMinShapeVertices) return nullptr;
        auto* bd0 = part->partitions[0].buffData;
        if (!bd0 || !bd0->rawVertexData || !bd0->vertexBuffer) return nullptr;

        int  iL = -1, iR = -1, iP = -1;
        auto nb = skin->skinData->GetBoneCount();
        // the optional regions' bones (3BA / BHUNP / UBE physics bones): back of the thighs, belly, breasts (the deepest
        // breast bone the body has)
        static const RE::BSFixedString thL("NPC L RearThigh"), thR("NPC R RearThigh"), belly("NPC Belly");
        static const std::array<RE::BSFixedString, 5> brL{ "L Breast03", "L Breast02", "L Breast01", "NPC L Breast01", "NPC L Breast" },
            brR{ "R Breast03", "R Breast02", "R Breast01", "NPC R Breast01", "NPC R Breast" };
        int iThL = -1, iThR = -1, iBelly = -1, iBrL = -1, iBrR = -1, rankL = 99, rankR = 99;
        for (std::uint32_t i = 0; i < nb; ++i) {
            auto* bn = skin->bones[i];
            if (!bn) continue;
            if (bn->name == lButt) iL = static_cast<int>(i);
            else if (bn->name == rButt) iR = static_cast<int>(i);
            else if (bn->name == pelvis) iP = static_cast<int>(i);
            else if (bn->name == thL) iThL = static_cast<int>(i);
            else if (bn->name == thR) iThR = static_cast<int>(i);
            else if (bn->name == belly) iBelly = static_cast<int>(i);
            else
                for (int k = 0; k < static_cast<int>(brL.size()); ++k) {
                    if (bn->name == brL[k] && k < rankL) {
                        iBrL = static_cast<int>(i);
                        rankL = k;
                    }
                    if (bn->name == brR[k] && k < rankR) {
                        iBrR = static_cast<int>(i);
                        rankR = k;
                    }
                }
        }
        if (iL < 0 || iR < 0 || iP < 0) return nullptr;

        auto s = std::make_unique<RShape>();
        s->name = a_geom->name.c_str();
        s->who = a_t.name;
        s->owner = a_t.id;
        s->top.reset(a_t.root);
        s->geom.reset(a_geom);
        s->part.reset(part);
        s->raw = bd0->rawVertexData;
        s->vb = AsD3D(bd0->vertexBuffer);
        s->vcount = part->vertexCount;
        // her layout as the game set it up. A data block in our colour layout belongs to another woman's ripple right
        // now (the two share one mesh): that mesh is never rippled anyway (MarkShared), and its buffer is not the game's
        if (const auto pd = DescBits(part->partitions[0].vertexDesc); DescBits(bd0->vertexDesc) != pd)
            if (const auto a = AliasColour(pd); a && a->desc == DescBits(bd0->vertexDesc)) return nullptr;
        auto desc = bd0->vertexDesc;
        s->desc = DescBits(desc);
        s->stride = StrideOf(s->desc);
        s->bytes = s->vcount * s->stride;
        if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || s->stride == 0) return nullptr;
        // Position format from the layout itself: GetSize() counts a 16-byte (full precision) position, so the stride
        // equals it for full precision and is 8 less for half. (Skinned SSE meshes store full precision positions
        // without setting the full-precision flag.)
        if (desc.GetSize() == s->stride) {
            s->fullPrec = true;
        } else if (desc.GetSize() == s->stride + 8) {
            s->fullPrec = false;
        } else {
            SKSE::log::info("  shape '{}': unexpected vertex layout (stride {}, layout size {}) - skipped", s->name, s->stride, desc.GetSize());
            return nullptr;
        }
        if (desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL)) s->nOff = static_cast<int>(OffsetOf(s->desc, Attribute::kNormal));
        if (desc.HasFlag(RE::BSGraphics::Vertex::VF_COLORS)) s->cOff = static_cast<int>(OffsetOf(s->desc, Attribute::kColour));
        // an attribute that would reach past the vertex: a layout we don't trust, that attribute is left alone
        if (s->nOff >= 0 && static_cast<std::uint32_t>(s->nOff) + 3 > s->stride) s->nOff = -1;
        if (s->cOff >= 0 && static_cast<std::uint32_t>(s->cOff) + kColourSize > s->stride) s->cOff = -1;
        bool vcFlag = false;
        if (auto* sp = grd.shaderProperty.get()) {
            s->msn = sp->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals);
            vcFlag = sp->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kVertexColors);
        }
        s->writeN = cfg.tiltNormals && s->nOff >= 0 && !s->msn;
        // skin: the game's skin-tint shader, the one bodies are drawn with
        bool skinTint = false;
        if (auto* lsp = a_geom->lightingShaderProp_cast(); lsp && lsp->material)
            skinTint = static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material)->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint;

        // How the shading bands are shown. A colour layer or the vertex colour flag is added only where the outline
        // would otherwise be the only cue - skin and model-space normal maps; clothing with ordinary normal maps shows
        // the wave in its tilted normals - and only when every partition draws one mesh (OneDataBlock).
        std::string colourText;
        const bool  bands = s->msn || skinTint;
        if (!cfg.shading) {
            colourText = "off (Shading = 0)";
        } else if (s->cOff >= 0 && vcFlag) {
            s->colour = Colour::kOwn;
            colourText = "shading bands";
        } else if (!bands) {
            colourText = s->writeN ? "none (tangent-space normals: tilted normals instead)" : "none (tangent-space normal map)";
        } else if (!OneDataBlock(a_geom, part)) {
            colourText = "none (several data blocks)";
        } else if (s->cOff >= 0) {
            // the shader ignores them: switching it on while she ripples must not show, so they must be white
            bool white = true;
            for (std::uint32_t v = 0; v < s->vcount && white; ++v) {
                const auto* c = s->raw + static_cast<std::size_t>(v) * s->stride + s->cOff;
                white = c[0] == 0xFF && c[1] == 0xFF && c[2] == 0xFF && c[3] == 0xFF;
            }
            if (!cfg.addColourLayer) {
                colourText = "present, shader flag off (AddColourLayer = 0)";
            } else if (!white) {
                colourText = "present but not white, shader flag off - no shading";
            } else {
                s->colour = Colour::kFlag;
                colourText = "shading bands (vertex colour flag set while rippling)";
            }
        } else if (!cfg.addColourLayer) {
            colourText = "none (AddColourLayer = 0)";
        } else if (!s->msn) {
            // the colour layer sits in the position's w, which only the model-space-normal shaders leave unread
            colourText = "none (tangent-space normals: no room for a colour layer)";
        } else if (auto L = AliasColour(s->desc); !L || !s->fullPrec) {
            colourText = "none (layout not supported for a colour layer)";
        } else if (!g_renderHooked) {
            colourText = "none (the check before each render is not in - see the render hook line)";
        } else {
            s->colour = Colour::kLayer;
            s->layout = *L;
            colourText = std::format("shading bands (colour layer while rippling, stride {} kept)", s->stride);
        }
        s->built = s->colour;

        // rest positions and welded normals from the engine's own vertex data and triangles
        std::vector<V3> pos(s->vcount);
        for (std::uint32_t v = 0; v < s->vcount; ++v) pos[v] = ReadPos(s->raw + static_cast<std::size_t>(v) * s->stride, s->fullPrec);
        // vertex data that can't be a mesh (another mod's copy of it went wrong): never rippled, never uploaded again
        constexpr float kFar = 10000.0f;
        if (auto bad = std::find_if(pos.begin(), pos.end(), [](const V3& p) {
                return !(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) || std::fabs(p.x) > kFar || std::fabs(p.y) > kFar ||
                       std::fabs(p.z) > kFar;
            });
            bad != pos.end()) {
            if (brokenLogged.insert(std::format("{:08X}/{}", s->owner, s->name)).second)
                SKSE::log::warn("  shape '{}': its vertex data looks broken (vertex {} at {}, {}, {}) - skipped", s->name, bad - pos.begin(), bad->x,
                    bad->y, bad->z);
            return nullptr;
        }
        std::unordered_map<std::uint64_t, std::uint32_t> weld;
        std::vector<std::uint32_t>                       wid(s->vcount);
        for (std::uint32_t v = 0; v < s->vcount; ++v) {
            auto          q = [](float x) { return static_cast<std::uint64_t>(std::llround(x * 1000.0) + (1LL << 20)) & 0x1FFFFF; };
            std::uint64_t key = q(pos[v].x) | (q(pos[v].y) << 21) | (q(pos[v].z) << 42);
            wid[v] = weld.try_emplace(key, static_cast<std::uint32_t>(weld.size())).first->second;
        }
        std::vector<V3> acc(weld.size(), V3{ 0, 0, 0 });
        std::size_t     tris = 0;
        // all around, on skin: the welded edges (each once per triangle), to find the mesh's open edges
        const bool                 fadeEdges = cfg.allAround && skinTint;
        std::vector<std::uint64_t> edges;
        const auto                 addEdge = [&](std::uint32_t a_x, std::uint32_t a_y) {
            if (a_x != a_y) edges.push_back(a_x < a_y ? (std::uint64_t{ a_x } << 32) | a_y : (std::uint64_t{ a_y } << 32) | a_x);
        };
        for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
            const auto& pt = part->partitions[i];
            // the index data the game draws with (global indices into the shared vertex buffer)
            const std::uint16_t* tl = pt.buffData && pt.buffData->rawIndexData ? pt.buffData->rawIndexData : pt.triList;
            if (!tl) continue;
            for (std::uint32_t t = 0; t < pt.triangles; ++t) {
                std::uint32_t a = tl[3 * t], b = tl[3 * t + 1], c = tl[3 * t + 2];
                if (a >= s->vcount || b >= s->vcount || c >= s->vcount) return nullptr;   // not global indices: unsupported
                V3 fn = (pos[b] - pos[a]).Cross(pos[c] - pos[a]);
                acc[wid[a]] += fn;
                acc[wid[b]] += fn;
                acc[wid[c]] += fn;
                ++tris;
                if (fadeEdges) {
                    addEdge(wid[a], wid[b]);
                    addEdge(wid[b], wid[c]);
                    addEdge(wid[c], wid[a]);
                }
            }
        }
        if (tris == 0) return nullptr;

        auto origin = [&](int bi) { return skin->skinData->GetBoneDataSkinToBone(static_cast<std::uint32_t>(bi)).Invert().translate; };
        V3   bL = origin(iL), bR = origin(iR), bP = origin(iP);
        s->pelvisIdx = static_cast<std::uint32_t>(iP);
        s->pelvisS2B = skin->skinData->GetBoneDataSkinToBone(static_cast<std::uint32_t>(iP));
        s->mid = Mul(bL + bR, 0.5f);
        s->sep = (bL - bR).Length();
        s->R = cfg.regionScale * s->sep;
        if (s->sep < 1.0f || s->mid.y > bP.y) {
            SKSE::log::info("  shape '{}': butt bones not where expected (sep {:.2f}, butt y {:.2f} vs pelvis y {:.2f}) - skipped", s->name, s->sep,
                s->mid.y, bP.y);
            return nullptr;
        }
        // The optional regions on this mesh: switched on, their bones in its skin. Centres in skin space, from the bone
        // heads (offsets measured on 3BA, where the butt bones are 8 units apart, scaled with this body's distance).
        const auto& rg = cfg.regions;
        const float u = s->sep / 8.0f;
        const bool  thighs = rg[kThighs].enabled && iThL >= 0 && iThR >= 0;
        const bool  bellyOn = rg[kBelly].enabled && iBelly >= 0;
        const bool  breasts = rg[kBreasts].enabled && iBrL >= 0 && iBrR >= 0;
        std::array<V3, kLobes> centre{};
        if (thighs) {
            centre[2] = origin(iThL) + V3(0.0f, -3.0f * u, -0.5f * u);   // the back of each thigh, below the gluteal fold
            centre[3] = origin(iThR) + V3(0.0f, -3.0f * u, -0.5f * u);
        }
        if (bellyOn) centre[4] = origin(iBelly) + V3(0.0f, 4.0f * u, -3.0f * u);   // the lower belly, in front of the bone
        if (breasts) {
            centre[5] = origin(iBrL) + V3(0.0f, -2.0f * u, -1.0f * u);   // inside each breast, under its tip
            centre[6] = origin(iBrR) + V3(0.0f, -2.0f * u, -1.0f * u);
        }
        const float rTh = rg[kThighs].size * s->sep, rBe = rg[kBelly].size * s->sep, rBr = rg[kBreasts].size * s->sep;
        const auto  sphere = [](float a_d, float a_r) { return 1.0f - Smooth((a_d - 0.6f * a_r) / (0.4f * a_r)); };
        // All around, on skin: the wave fades out towards the mesh's open edges, where meshes of their own that never ripple
        // join it - over 1.5 butt-bone distances at her neck, wrists and ankles (her head, hands, feet), over half of one
        // at the openings close to her butt (3BA's separate anus and vagina) - so it never ends in a step or a crack there.
        // (Distances along the mesh. On 3BA: neck, wrists and ankles 3.6 to 7.9 butt-bone distances from the butt, the two
        // openings 1.1 and 1.7.)
        std::vector<float> edgeFade;
        if (fadeEdges) {
            std::vector<V3> wpos(weld.size());
            for (std::uint32_t v = 0; v < s->vcount; ++v) wpos[wid[v]] = pos[v];
            std::sort(edges.begin(), edges.end());
            std::vector<std::vector<std::uint32_t>> adj(weld.size());
            std::vector<std::uint8_t>               open(weld.size(), 0);
            for (std::size_t i = 0; i < edges.size();) {
                std::size_t j = i;
                while (j < edges.size() && edges[j] == edges[i]) ++j;
                const auto x = static_cast<std::uint32_t>(edges[i] >> 32), y = static_cast<std::uint32_t>(edges[i] & 0xFFFFFFFFu);
                adj[x].push_back(y);
                adj[y].push_back(x);
                if (j - i == 1) open[x] = open[y] = 1;   // one triangle only: an open edge
                i = j;
            }
            // distance along the mesh (up to a_len) from the open points far from her butt (a_far) or close to it
            const auto from = [&](bool a_far, float a_len, std::size_t& a_seeds) {
                std::vector<float> dist(weld.size(), a_len);
                using Item = std::pair<float, std::uint32_t>;
                std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
                for (std::uint32_t z = 0; z < static_cast<std::uint32_t>(weld.size()); ++z)
                    if (open[z] && ((wpos[z] - s->mid).Length() > 2.5f * s->sep) == a_far) {
                        dist[z] = 0.0f;
                        q.emplace(0.0f, z);
                        ++a_seeds;
                    }
                while (!q.empty()) {
                    const auto [dz, z] = q.top();
                    q.pop();
                    if (dz > dist[z]) continue;
                    for (const auto y : adj[z])
                        if (const float dy = dz + (wpos[y] - wpos[z]).Length(); dy < dist[y]) {
                            dist[y] = dy;
                            q.emplace(dy, y);
                        }
                }
                return dist;
            };
            std::size_t farSeeds = 0, nearSeeds = 0;
            const float lenFar = 1.5f * s->sep, lenNear = 0.5f * s->sep;
            const auto  dFar = from(true, lenFar, farSeeds), dNear = from(false, lenNear, nearSeeds);
            edgeFade.resize(s->vcount);
            for (std::uint32_t v = 0; v < s->vcount; ++v) edgeFade[v] = Smooth(dFar[wid[v]] / lenFar) * Smooth(dNear[wid[v]] / lenNear);
            SKSE::log::info("  shape '{}': all around - the wave fades out at {} open-edge points far from the butt (neck, wrists, ankles) and {} "
                            "close to it (openings)",
                s->name, farSeeds, nearSeeds);
        }
        std::array<std::size_t, kRegionCount> count{};
        for (std::uint32_t v = 0; v < s->vcount; ++v) {
            const V3& p = pos[v];
            V3        n = Normalized(acc[wid[v]]);
            float     dL = (p - bL).Length(), dR = (p - bR).Length(), dm = std::min(dL, dR);
            // all around: the plain sphere; otherwise only behind her, facing back, and not into the cleft
            const bool aa = cfg.allAround;
            float      w = aa ? sphere(dm, s->R) :
                                (1.0f - Smooth((dm - 0.6f * s->R) / (0.4f * s->R))) * Smooth((s->mid.y + 0.35f * s->sep - p.y) / (0.35f * s->sep)) *
                               Smooth((0.45f - n.y) / 0.5f) * Smooth((std::fabs(p.x - s->mid.x) - 0.15f * s->sep) / (0.2f * s->sep));
            // the regions in priority order (butt, thighs, belly, breasts): each takes its weight of what the earlier
            // ones left, up to two per vertex. With the butt alone this is the butt's weight as it always was. (All around
            // on skin, every region's weight fades out towards the open edges: the share they take starts at the fade.)
            std::uint8_t l1 = kNoLobe, l2 = kNoLobe;
            float        w1 = 0.0f, w2 = 0.0f, rem = edgeFade.empty() ? 1.0f : edgeFade[v];
            bool         hit = false;
            const auto   take = [&](std::uint8_t a_lobe, float a_w, bool a_hit) {
                const float wl = a_w * rem;
                if (wl <= 1e-3f || l2 != kNoLobe) return;
                rem -= wl;
                if (l1 == kNoLobe) {
                    l1 = a_lobe;
                    w1 = wl;
                    hit = a_hit;
                } else {
                    l2 = a_lobe;
                    w2 = wl;
                }
            };
            take(dL <= dR ? 0 : 1, w, n.y < -0.45f && std::fabs(p.x - s->mid.x) > 0.3f * s->sep);
            if (thighs) {
                const std::uint8_t k = p.x < s->mid.x ? 2 : 3;
                const float        wt = sphere((p - centre[k]).Length(), rTh) * (aa ? 1.0f : Smooth((0.1f - n.y) / 0.3f));
                take(k, wt, wt > 0.3f);
            }
            if (bellyOn) {
                const float wb = sphere((p - centre[4]).Length(), rBe) * (aa ? 1.0f : Smooth((n.y - 0.1f) / 0.3f));
                take(4, wb, wb > 0.3f);
            }
            if (breasts) {
                const std::uint8_t k = p.x < s->mid.x ? 5 : 6;
                const float        wb = sphere((p - centre[k]).Length(), rBr) * (aa ? 1.0f : Smooth((n.y + 0.3f) / 0.3f));
                take(k, wb, false);   // a breast's wave always starts inside it, under the tip
            }
            if (l1 == kNoLobe) continue;
            s->idx.push_back(v);
            s->w.push_back(w1);
            s->cheek.push_back(l1);
            s->w2.push_back(w2);
            s->lobe2.push_back(l2);
            s->P.push_back(p);
            s->N.push_back(n);
            s->Nst.push_back(s->nOff >= 0 ? ReadNormal(s->raw + static_cast<std::size_t>(v) * s->stride + s->nOff) : n);
            s->hit.push_back(hit);
            ++count[kRegionOf[l1]];
            if (l2 != kNoLobe && kRegionOf[l2] != kRegionOf[l1]) ++count[kRegionOf[l2]];
        }
        if (count[kButt] < kMinRippleVertices) return nullptr;
        for (std::uint8_t k = 0; k < 2; ++k) {
            float best = -1e9f;
            bool  any = false;
            for (std::size_t i = 0; i < s->idx.size(); ++i) {
                if (s->cheek[i] != k || !s->hit[i]) continue;
                float score = -s->P[i].z - 0.5f * std::fabs(s->P[i].x - s->mid.x);
                if (score > best) {
                    best = score;
                    s->fallback[k] = s->P[i];
                    any = true;
                }
            }
            if (!any) return nullptr;
        }
        s->lobes = 0b11;
        s->lobeR[0] = s->lobeR[1] = s->R;
        // each optional lobe: its fallback wave centre is its vertex nearest its centre (on her surface, wherever her
        // body morphs put it); a region with too few vertices on this mesh stays off on it
        std::string extra;
        const std::array<bool, kRegionCount> on{ true, thighs, bellyOn, breasts };
        const std::array<float, kRegionCount> rad{ s->R, rTh, rBe, -1.0f };   // -1: always from the fallback (breasts)
        for (int r = kThighs; r < kRegionCount; ++r) {
            if (!on[r]) continue;
            if (count[r] < kMinRippleVertices) {
                extra += std::format(", {} too few ({})", kRegionNames[r], count[r]);
                continue;
            }
            for (std::uint8_t l = 2; l < kLobes; ++l) {
                if (kRegionOf[l] != r) continue;
                float bd = 1e9f;
                for (std::size_t i = 0; i < s->idx.size(); ++i) {
                    if (s->cheek[i] != l && s->lobe2[i] != l) continue;
                    const float d = (s->P[i] - centre[l]).Length();
                    if (d < bd) {
                        bd = d;
                        s->fallback[l] = s->P[i];
                    }
                }
                if (bd < 1e9f) {
                    s->lobes |= static_cast<std::uint8_t>(1u << l);
                    s->lobeR[l] = rad[r];
                }
            }
            extra += std::format(", {} {}", kRegionNames[r], count[r]);
        }
        SKSE::log::info("  shape '{}': {} verts, stride {}, {} precision, {} ripple verts (radius {:.1f}{}), normals {}, colours {}", s->name, s->vcount,
            s->stride, s->fullPrec ? "full" : "half", s->idx.size(), s->R, extra, s->writeN ? "tilted" : (s->msn ? "model-space map (outline only)" : "none"),
            colourText);
        return s;
    }

    // what her 3D draws from now (cheap: no vertex work) - to notice outfit changes and meshes shared between women
    MeshUse MeshUseOf(RE::NiAVObject* a_root)
    {
        MeshUse out;
        if (!a_root) return out;
        RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* g) {
            // (blood splatters coming and going are no change to her meshes - and their skin data is never read)
            if (IsDecal(g)) return RE::BSVisit::BSVisitControl::kContinue;
            auto* skin = g->GetGeometryRuntimeData().skinInstance.get();
            auto* part = skin ? skin->skinPartition.get() : nullptr;
            if (const char* nm = g->name.c_str(); nm && *nm) {
                const std::string n = Lowered(nm);
                if (n.find("[ovl") != std::string::npos || n.find("[sovl") != std::string::npos)
                    out.overlays.emplace_back(nm, part ? part->vertexCount : 0u);
            }
            if (part) {
                out.parts.push_back(part);
                for (std::uint32_t i = 0; i < part->numPartitions; ++i)
                    if (auto* b = part->partitions[i].buffData; b && b->vertexBuffer) out.buffers.push_back(b->vertexBuffer);
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        for (auto* v : { &out.parts, &out.buffers }) {
            std::sort(v->begin(), v->end());
            v->erase(std::unique(v->begin(), v->end()), v->end());
        }
        return out;
    }

    // Her meshes, looked at again. With the same 3D and no settings reload, a shape whose geometry is still hers and
    // still draws what it was set up from (StillValid) stays as it is - its waves running, its buffer swapped in - so
    // meshes coming and going around it (outfit pieces, SMP collision objects, RaceMenu overlays) don't cut its ripples
    // off. Every other shape goes back to how the game set it up; one set up anew with the same name and vertex count
    // (the same mesh, rebuilt) takes over its waves.
    void ScanShapes(Tracked& a_t, RE::NiAVObject* a_root, std::vector<const void*> a_skinned, std::string_view a_why)
    {
        std::vector<std::unique_ptr<RShape>> keep;
        if (a_root && a_root == a_t.root && !a_t.rescan) {
            std::vector<const RE::BSGeometry*> present;
            RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* g) {
                present.push_back(g);
                return RE::BSVisit::BSVisitControl::kContinue;
            });
            // (one that lost its shading bands since its setup is set up anew: they may be possible again)
            for (auto& s : a_t.shapes)
                if (std::find(present.begin(), present.end(), s->geom.get()) != present.end() && s->colour == s->built && StillValid(*s))
                    keep.push_back(std::move(s));
        }
        // a kept shape not back in her list when this returns - or throws - goes back to how the game set it up
        struct PutBack
        {
            std::vector<std::unique_ptr<RShape>>& shapes;
            ~PutBack()
            {
                for (auto& s : shapes)
                    if (s) Restore(*s);
            }
        } putBack{ keep };
        std::vector<std::pair<std::pair<std::string, std::uint32_t>, std::vector<Event>>> waves;   // (name, vertices) -> waves
        for (auto& s : a_t.shapes) {
            if (!s) continue;
            Restore(*s);
            if (!s->events.empty()) waves.push_back({ { s->name, s->vcount }, std::move(s->events) });
        }
        a_t.shapes.clear();
        a_t.root = a_root;
        a_t.skinned = std::move(a_skinned);
        a_t.rescan = false;
        if (!a_root) return;
        if (keep.empty())
            SKSE::log::info("{} ({:08X}): scanning her meshes ({})", a_t.name, a_t.id, a_why);
        else
            SKSE::log::info("{} ({:08X}): her meshes changed ({}) - {} kept as they are", a_t.name, a_t.id, a_why, keep.size());
        // one entry per skin partition: RaceMenu overlay copies can share the body's vertex buffer (a kept shape's
        // buffer may be our copy now)
        const auto drawsFrom = [](const RE::NiSkinPartition* a_part, const std::unique_ptr<RShape>& a_s) {
            if (!a_s) return false;
            if (a_s->part.get() == a_part) return true;
            const Tri* bd = a_part->numPartitions ? a_part->partitions[0].buffData : nullptr;
            if (!bd) return false;
            const auto* vb = AsD3D(bd->vertexBuffer);
            return (vb && (vb == a_s->vb || (a_s->swapped && vb == a_s->mine))) || bd->rawVertexData == a_s->raw;
        };
        std::unordered_map<const RE::BSShaderProperty*, int> shaderUse;   // geometries per shader
        RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* g) {
            if (IsDecal(g)) return RE::BSVisit::BSVisitControl::kContinue;   // blood splatters: never looked into
            if (const auto* sp = g->GetGeometryRuntimeData().shaderProperty.get()) ++shaderUse[sp];
            if (auto k = std::find_if(keep.begin(), keep.end(), [&](auto& s) { return s && s->geom.get() == g; }); k != keep.end()) {
                a_t.shapes.push_back(std::move(*k));
                return RE::BSVisit::BSVisitControl::kContinue;
            }
            auto* skin = g->GetGeometryRuntimeData().skinInstance.get();
            auto* part = skin ? skin->skinPartition.get() : nullptr;
            const auto dup = [&](const std::unique_ptr<RShape>& s) { return drawsFrom(part, s); };
            if (part && (std::any_of(a_t.shapes.begin(), a_t.shapes.end(), dup) || std::any_of(keep.begin(), keep.end(), dup)))
                return RE::BSVisit::BSVisitControl::kContinue;
            if (auto s = BuildShape(g, a_t)) {
                const auto w = std::find_if(waves.begin(), waves.end(), [&](auto& e) { return e.first.first == s->name && e.first.second == s->vcount; });
                if (w != waves.end()) {
                    s->events = std::move(w->second);
                    waves.erase(w);
                }
                a_t.shapes.push_back(std::move(s));
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        // the vertex colour flag is per shader: one that another of her meshes draws with too (a mesh without the
        // colours) would turn that mesh black while she ripples - such a shape ripples without shading bands
        for (auto& s : a_t.shapes)
            if (s->colour == Colour::kLayer || s->colour == Colour::kFlag)
                if (const auto* sp = s->geom->GetGeometryRuntimeData().shaderProperty.get(); sp && shaderUse[sp] > 1) {
                    Restore(*s);   // a kept shape may be drawing with the layer / flag now
                    s->colour = Colour::kNone;
                    SKSE::log::info("  '{}': its shader is shared with another of her meshes - no shading bands", s->name);
                }
        if (a_t.shapes.empty()) SKSE::log::info("{}: no mesh with butt bones found", a_t.name);
    }

    // ------------------------------------------------------------------------------------------------ actors
    bool IsFemale(RE::Actor* a_actor)
    {
        auto* npc = a_actor ? a_actor->GetActorBase() : nullptr;
        return npc && npc->GetSex() == RE::SEX::kFemale;
    }

    // Without per-actor body morphs (RaceMenu / OBody) several women can draw from one skin partition: a ripple on one
    // would show on all of them. A shape whose partition or vertex buffer another scanned woman also draws from is
    // never rippled.
    void MarkShared()
    {
        std::unordered_map<const void*, std::vector<Tracked*>> users;
        for (auto& t : tracked) {
            for (auto* p : t->use.parts) users[p].push_back(t.get());
            for (auto* b : t->use.buffers) users[b].push_back(t.get());
        }
        for (auto& t : tracked) {
            for (auto& s : t->shapes) {
                const Tracked* other = nullptr;
                for (const void* key : { static_cast<const void*>(s->part.get()), static_cast<const void*>(s->swapped ? s->orig : s->vb) }) {
                    if (auto it = users.find(key); it != users.end())
                        for (auto* u : it->second)
                            if (u != t.get()) other = u;
                }
                const bool shared = other != nullptr;
                if (shared && !s->shared) {
                    Restore(*s);
                    s->events.clear();
                    if (!t->sharedLogged) {
                        t->sharedLogged = true;
                        SKSE::log::info("{}: her body mesh is shared with {} (no per-actor body morphs) - no ripples on it", t->name, other->name);
                    }
                }
                s->shared = shared;
            }
        }
    }

    void Refresh()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        static const RE::BSFixedString genBase("NPC GenitalsBase [GenBase]"), pelvis("NPC Pelvis [Pelv]");
        V3                             pp = player->GetPosition();
        std::vector<RE::Actor*>        nearby;
        auto                           consider = [&](RE::Actor* a) {
            if (a && a->Is3DLoaded() && !a->IsDead() && (a->GetPosition() - pp).Length() < cfg.scanDistance) nearby.push_back(a);
        };
        consider(player);
        if (auto* pl = RE::ProcessLists::GetSingleton()) {
            pl->ForEachHighActor([&](RE::Actor* a) {
                consider(a);
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        partners.clear();
        for (auto& t : tracked) t->seen = false;
        for (auto* a : nearby) {
            auto* root = a->Get3D(false);
            if (!root) continue;
            Partner p;
            p.id = a->GetFormID();
            p.name = NameOf(a);
            p.pelvis.reset(root->GetObjectByName(pelvis));
            p.contact.reset(root->GetObjectByName(genBase));
            if (!p.contact) p.contact = p.pelvis;
            if (p.contact) partners.push_back(std::move(p));
            if (!IsFemale(a)) continue;
            auto     it = std::find_if(tracked.begin(), tracked.end(), [&](auto& t) { return t->id == a->GetFormID(); });
            Tracked* t;
            if (it == tracked.end()) {
                tracked.push_back(std::make_unique<Tracked>());
                t = tracked.back().get();
                t->id = a->GetFormID();
                t->name = NameOf(a);
                t->handle = a->GetHandle();
            } else {
                t = it->get();
            }
            t->seen = true;
            // rescan on a new 3D, a replaced mesh (RaceMenu / OBody morph update), an outfit change or an ini reload
            auto use = MeshUseOf(root);
            // her RaceMenu / NiOverride overlays, whenever they change (what a report's log needs)
            {
                std::string ovl;
                for (std::size_t i = 0; i < use.overlays.size() && i < 12; ++i)
                    ovl += std::format("{}'{}' ({} verts)", ovl.empty() ? "" : ", ", use.overlays[i].first, use.overlays[i].second);
                if (use.overlays.size() > 12) ovl += std::format(" and {} more", use.overlays.size() - 12);
                if (ovl != t->overlayLine) {
                    if (!ovl.empty()) SKSE::log::info("{} ({:08X}): RaceMenu overlays on her: {}", t->name, t->id, ovl);
                    t->overlayLine = std::move(ovl);
                }
            }
            std::string why = t->rescan ? "settings reloaded" : t->root != root ? (t->root ? "new 3D" : "first look") :
                              use.parts != t->skinned ? "her skinned meshes changed" : "";
            if (why.empty())
                if (auto s = std::find_if(t->shapes.begin(), t->shapes.end(), [](auto& s) { return !StillValid(*s); }); s != t->shapes.end())
                    why = std::format("'{}' changed", (*s)->name);
            if (!why.empty()) ScanShapes(*t, root, use.parts, why);
            t->use = std::move(use);
        }
        std::erase_if(tracked, [](auto& t) {
            if (t->seen) return false;
            for (auto& s : t->shapes) Restore(*s);
            return true;
        });
        MarkShared();
    }

    // ------------------------------------------------------------------------------------------------ rules
    // Whether an impact on her ripples now (Rules.h Decide); one log line whenever the outcome's text changes.
    Decision RuleFor(Tracked& a_t)
    {
        bool             inScene = false;
        std::string_view scene;
        int              speed = -1;
        const SceneMeta* meta = nullptr;
        if (auto it = inThread.find(a_t.id); it != inThread.end())
            if (auto th = threads.find(it->second); th != threads.end()) {
                inScene = true;
                scene = th->second.scene;
                speed = th->second.speed;
                meta = sceneTable ? sceneTable->Find(scene) : nullptr;
            }
        const SexLabThread* sl = nullptr;   // her SexLab scene (none while she is in an OStim one)
        int                 slThread = -1;
        if (!inScene && cfg.sexLab && g_slOn.load())
            if (auto it = inSexLab.find(a_t.id); it != inSexLab.end())
                if (auto th = slThreads.find(it->second); th != slThreads.end()) sl = &th->second, slThread = it->second;
        Decision d = sl ? DecideSexLab(cfg.rules, sl->stage) : Decide(cfg.rules, inScene, scene, meta, speed);
        if (!sl && !inScene && cfg.rules.enabled && g_slOn.load() && cfg.sexLab) d.why = "not in an OStim or SexLab scene";
        std::string line = inScene ? std::format("{}: scene '{}' - {} ({})", a_t.name, scene, d.on ? "ripples on" : "no ripples", d.why) :
                           sl      ? std::format("{}: SexLab{} scene (thread {}) - {} ({})", a_t.name, sl->plus ? " P+" : "", slThread,
                                         d.on ? "ripples on" : "no ripples", d.why) :
                                     std::format("{}: {} ({})", a_t.name, d.on ? "ripples on" : "no ripples", d.why);
        if (line != a_t.ruleLine) {
            SKSE::log::info("{}", line);
            a_t.ruleLine = std::move(line);
        }
        return d;
    }

    // a decision may have changed for the women of this thread
    void RecheckThread(std::int32_t a_thread)
    {
        for (auto& t : tracked)
            if (!t->shapes.empty())
                if (auto it = inThread.find(t->id); it != inThread.end() && it->second == a_thread) RuleFor(*t);
    }

    // the queued OStim events and speed answers, then the speed questions that are due (every second for a thread
    // that holds one of the women we follow)
    void UpdateScenes(Clock::time_point a_now)
    {
        std::vector<SceneEvent> events;
        {
            std::scoped_lock l(g_sceneLock);
            events.swap(g_sceneEvents);
        }
        for (auto& e : events) {
            if (e.kind == SceneKind::kStop) {
                threads.erase(e.thread);
                std::erase_if(inThread, [&](const auto& p) { return p.second == e.thread; });
                continue;
            }
            auto& th = threads[e.thread];
            if (e.kind == SceneKind::kStart) th = SceneThread{};
            if (!e.node.empty()) th.scene = e.node;
            // A new scene, node or speed: the speed index known so far no longer holds. It is unknown until the question
            // asked below is answered; answers to earlier questions are not used (they may carry the old speed, or be
            // about an earlier scene on this thread id).
            th.speed = -1;
            th.answered = g_askSerial;
            std::erase_if(inThread, [&](const auto& p) {
                return p.second == e.thread && std::find(e.actors.begin(), e.actors.end(), p.first) == e.actors.end();
            });
            for (auto id : e.actors) inThread[id] = e.thread;
            AskSpeed(e.thread, th, a_now);
            // a speed-based scene is judged (and logged) once its speed is known: its answer rechecks the thread
            if (e.kind == SceneKind::kNode) {
                const SceneMeta* meta = sceneTable ? sceneTable->Find(th.scene) : nullptr;
                if (!meta || !IsMultiSpeed(*meta)) RecheckThread(e.thread);
            }
        }
        std::vector<SpeedAnswer> answers;
        {
            std::scoped_lock l(g_answerLock);
            answers.swap(g_speedAnswers);
        }
        for (auto& a : answers) {
            auto it = threads.find(a.thread);
            if (it == threads.end() || a.serial <= it->second.answered) continue;
            it->second.answered = a.serial;
            // -1: the thread is still starting - asked again a quarter second later rather than a second
            if (a.speed < 0) it->second.lastAsk = a_now - std::chrono::milliseconds(750);
            if (it->second.speed != a.speed) {
                it->second.speed = a.speed;
                RecheckThread(a.thread);
            }
        }
        if (!cfg.enabled) return;
        for (auto& [id, th] : threads) {
            if (a_now - th.lastAsk < std::chrono::seconds(1)) continue;
            const bool holds = std::any_of(tracked.begin(), tracked.end(), [&](auto& t) {
                auto it = inThread.find(t->id);
                return !t->shapes.empty() && it != inThread.end() && it->second == id;
            });
            if (holds) AskSpeed(id, th, a_now);
        }
    }

    // a decision may have changed for the women of this SexLab thread
    void RecheckSexLab(int a_thread)
    {
        for (auto& t : tracked)
            if (!t->shapes.empty())
                if (auto it = inSexLab.find(t->id); it != inSexLab.end() && it->second == a_thread) RuleFor(*t);
    }

    // P+'s stage question for one thread (the main thread). The VM copies the argument values while dispatching; the
    // small argument object is not freed afterwards (a few bytes, no assumption about who owns it).
    bool AskSexLabStage(int a_thread, SexLabThread& a_th, std::uint32_t a_serial)
    {
        auto  obj = ThreadScript(RE::TESForm::LookupByID(a_th.quest));
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!obj || !vm) return false;
        if (!a_th.checked) {
            a_th.checked = true;
            a_th.canAsk = HasFunction(obj.get(), "GetStageHistoryLength");
            if (!a_th.canAsk)
                SKSE::log::warn("SexLab P+ thread {}: its script has no GetStageHistoryLength - its stage stays unknown (no ripples in it)", a_thread);
        }
        if (!a_th.canAsk) return false;
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb{ new SexLabReply(a_thread, a_serial) };
        static const RE::BSFixedString                           fn("GetStageHistoryLength");
        return vm->DispatchMethodCall(obj, fn, RE::MakeFunctionArguments(), cb);
    }

    // The main thread, every frame: SexLab's queued steps, P+'s stage answers, the P+ questions that are due, and every
    // second whether everyone we know in a SexLab scene still is in one (for a missed end). The game's own code runs with
    // none of our locks held.
    void UpdateSexLab(Clock::time_point a_now)
    {
        std::vector<SexLabEvent>  events;
        std::vector<SexLabAnswer> answers;
        {
            std::scoped_lock l(g_slLock);
            events.swap(g_slEvents);
            answers.swap(g_slAnswers);
        }
        if (!g_slOn.load() || !cfg.sexLab) {   // not followed (any more): everyone counts as outside a SexLab scene
            if (!slThreads.empty() || !inSexLab.empty()) {
                slThreads.clear();
                inSexLab.clear();
                for (auto& t : tracked)
                    if (!t->shapes.empty()) RuleFor(*t);
            }
            return;
        }
        const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(a_now.time_since_epoch()).count();
        for (auto& e : events) {
            if (e.step == SexLabStep::kEnd) {
                slThreads.erase(e.thread);
                std::erase_if(inSexLab, [&](const auto& p) { return p.second == e.thread; });
                for (auto& t : tracked)
                    if (!t->shapes.empty() && std::find(e.actors.begin(), e.actors.end(), t->id) != e.actors.end()) RuleFor(*t);
                continue;
            }
            const bool created = !slThreads.contains(e.thread);
            auto&      th = slThreads[e.thread];
            // classic SexLab's scene starts with AnimationStart; P+'s with its first stage's StageStart - its AnimationStart
            // comes once that stage plays (P+'s scene is new on its thread once the one before ended: erased above)
            if (e.step == SexLabStep::kStart && !e.plus) th = SexLabThread{};
            if (created || (e.step == SexLabStep::kStart && !e.plus)) {
                // answers to questions about an earlier scene on this thread don't count; a P+ scene first met on a
                // later step (ActorChangeEnd re-creating it - P+'s stage history still holds the scene before until the
                // new one's first stage starts) is asked for only from its next StageStart / AnimationStart on
                th.stepSerial = g_slSerial;
                th.poll.askAt = (std::numeric_limits<std::int64_t>::max)();
            }
            th.quest = e.quest;
            th.plus = e.plus;
            th.lastStep = a_now;
            if (!e.actors.empty()) {
                std::erase_if(inSexLab, [&](const auto& p) {
                    return p.second == e.thread && std::find(e.actors.begin(), e.actors.end(), p.first) == e.actors.end();
                });
                th.actors = e.actors;
                for (auto id : e.actors) inSexLab[id] = e.thread;
            }
            if (!e.plus) {
                if (e.stage > 0) th.stage = e.stage;   // classic: read at the step itself
            } else if (e.step == SexLabStep::kNewStage) {
                // P+: its new stage is asked for in a moment (SexLab.h PlusStagePoll) - until then it is not known (no
                // ripples), and answers to questions asked before this step don't count
                th.stageBefore = e.before;   // (read at the step: an answer taken in since may already name the new one)
                th.poll.Step(now);
                th.stage = 0;
                th.stepSerial = g_slSerial;
            } else if (e.step == SexLabStep::kStart && th.stage == 0) {
                th.poll.Step(now);   // P+'s AnimationStart: its first stage plays (and is set) - asked for
            }
            RecheckSexLab(e.thread);
        }
        for (auto& a : answers) {
            auto it = slThreads.find(a.thread);
            if (it == slThreads.end() || a.serial <= it->second.applied || a.serial <= it->second.stepSerial) continue;
            auto& th = it->second;
            th.applied = a.serial;
            th.poll.Answered();
            // P+ sets a new stage after the hooks of its StageStart have run: an answer naming the stage before, early
            // after the step, may come from before that (a slow hook) - the next question tells
            if (th.stageBefore > 0 && a.stage == th.stageBefore && now - th.poll.stepAt < PlusStagePoll::kAfterStep[1]) continue;
            if (a.stage >= 0 && a.stage != th.stage) {
                th.stage = a.stage;
                if (cfg.debug) SKSE::log::info("SexLab P+ thread {}: stage {}", a.thread, a.stage);
                RecheckSexLab(a.thread);
            }
        }
        for (auto& [id, th] : slThreads)
            if (cfg.enabled && th.plus && th.poll.Due(now)) {   // (switched off: no questions; asked at once when on again)
                th.poll.Asked(now);
                if (!AskSexLabStage(id, th, ++g_slSerial)) th.poll.Answered();   // no question went out
            }
        // a missed end: whoever SexLab no longer has in a scene (not checked for 2 s after a step: SexLab marks its actors
        // as it starts them)
        if (a_now - lastSexLabCheck >= std::chrono::seconds(1)) {
            lastSexLabCheck = a_now;
            std::vector<RE::FormID> gone;
            for (const auto& [actor, thread] : inSexLab) {
                const auto th = slThreads.find(thread);
                if (th != slThreads.end() && a_now - th->second.lastStep < std::chrono::seconds(2)) continue;
                if (!SexLabActive(RE::TESForm::LookupByID<RE::Actor>(actor))) gone.push_back(actor);
            }
            for (const auto actor : gone) {
                inSexLab.erase(actor);
                for (auto& t : tracked)
                    if (t->id == actor && !t->shapes.empty()) RuleFor(*t);
            }
            std::erase_if(slThreads, [&](const auto& p) {
                return std::none_of(inSexLab.begin(), inSexLab.end(), [&](const auto& q) { return q.second == p.first; });
            });
        }
    }

    // ------------------------------------------------------------------------------------------------ impacts
    // Her pelvis bone as this shape's skin sees it. The skin instance's bone list holds plain pointers into her
    // skeleton, not references: callers check Alive first, and a shape taken off her (no parent) gives none.
    RE::NiAVObject* PelvisNode(RShape& a_s)
    {
        auto* g = a_s.geom.get();
        auto* skin = g && g->parent ? g->GetGeometryRuntimeData().skinInstance.get() : nullptr;
        if (!skin || !skin->bones || !skin->skinData || a_s.pelvisIdx >= skin->skinData->GetBoneCount()) return nullptr;
        return skin->bones[a_s.pelvisIdx];
    }

    // Her 3D is still the one her shapes were scanned from, so her bones are alive. After a disable, a teleport or a
    // 3D reset the old skeleton may be gone before Refresh (every half second) rescans her; this is checked every frame.
    bool Alive(const Tracked& a_t)
    {
        if (!a_t.root || a_t.shapes.empty()) return false;
        const auto a = a_t.handle.get();
        if (!a || !a->Is3DLoaded() || a->Get3D(false) != a_t.root) return false;
        const auto* g = a_t.shapes[0]->geom.get();
        return g && g->parent;
    }

    // skin space of this shape <- world, through her pelvis bone
    std::optional<RE::NiTransform> WorldToSkin(RShape& a_s)
    {
        auto* pel = PelvisNode(a_s);
        if (!pel) return std::nullopt;
        return (pel->world * a_s.pelvisS2B).Invert();
    }

    enum class Fired
    {
        kRipple,    // a ripple started
        kRules,     // the rules said no
        kNothing    // none of her meshes can ripple (shared with another woman, or her 3D is being replaced)
    };

    // An impact on her. a_rules: false for the test key (it ignores the rules).
    Fired Fire(Tracked& a_t, const V3* a_contactW, float a_strength, std::string_view a_why, bool a_rules)
    {
        if (!Alive(a_t)) return Fired::kNothing;   // her 3D is being replaced: Refresh rescans her
        if (a_rules && !RuleFor(a_t).on) {
            ++a_t.statSkipped;
            return Fired::kRules;
        }
        const float now = Now();
        int         fired = 0;
        // each lobe's wave starts at its vertex nearest the contact - or, further than the lobe's radius from it (or a
        // breast: always), at the lobe's fallback centre (a_pc: the contact in the shape's skin space, null: none)
        const auto centres = [](const RShape& a_s, const V3* a_pc) {
            std::array<V3, kLobes> c = a_s.fallback;
            if (!a_pc) return c;
            for (std::uint8_t l = 0; l < kLobes; ++l) {
                if (!(a_s.lobes & (1u << l)) || a_s.lobeR[l] < 0.0f) continue;
                float bd = 1e9f;
                for (std::size_t i = 0; i < a_s.idx.size(); ++i) {
                    if (a_s.cheek[i] != l || !a_s.hit[i]) continue;
                    float d = (a_s.P[i] - *a_pc).Length();
                    if (d < bd) {
                        bd = d;
                        c[l] = a_s.P[i];
                    }
                }
                if (bd > a_s.lobeR[l]) c[l] = a_s.fallback[l];
            }
            return c;
        };
        // all around: every mesh of hers takes the wave centres of her biggest one (in the world), so the waves run on
        // across the seams between her body's pieces (and her clothes) as one
        std::optional<std::array<V3, kLobes>> sharedW;
        std::uint8_t                          sharedLobes = 0;
        if (cfg.allAround) {
            RShape* big = nullptr;
            for (auto& sp : a_t.shapes)
                if (!sp->shared && (!big || sp->idx.size() > big->idx.size())) big = sp.get();
            if (big)
                if (auto inv = WorldToSkin(*big)) {
                    std::optional<V3> pc;
                    if (a_contactW) pc = *inv * *a_contactW;
                    const auto c = centres(*big, pc ? &*pc : nullptr);
                    const auto toWorld = inv->Invert();
                    std::array<V3, kLobes> w{};
                    for (int l = 0; l < kLobes; ++l) w[l] = toWorld * c[l];
                    sharedW = w;
                    sharedLobes = big->lobes;
                }
        }
        for (auto& sp : a_t.shapes) {
            auto& s = *sp;
            if (s.shared) continue;
            Event e;
            e.t0 = now;
            e.s = a_strength;
            e.lobes = s.lobes;
            std::optional<RE::NiTransform> inv;
            if (a_contactW || sharedW) {
                inv = WorldToSkin(s);
                if (!inv && a_contactW) continue;   // (no contact: its own fallback centres, as without all around)
            }
            std::optional<V3> pc;
            if (a_contactW) pc = *inv * *a_contactW;
            e.c = centres(s, pc ? &*pc : nullptr);
            if (sharedW && inv)
                for (int l = 0; l < kLobes; ++l)
                    if (sharedLobes & s.lobes & (1u << l)) e.c[l] = *inv * (*sharedW)[l];
            s.events.push_back(e);
            if (static_cast<int>(s.events.size()) > cfg.maxEvents) s.events.erase(s.events.begin());
            ++fired;
        }
        if (!fired) return Fired::kNothing;
        a_t.lastImpact = now;
        ++a_t.statImpacts;
        if (cfg.debug) SKSE::log::info("{}: ripple ({}) strength {:.2f}", a_t.name, a_why, a_strength);
        return Fired::kRipple;
    }

    // ------------------------------------------------------------------------------------------------ PPA impact sounds
    // PPA plays its impact sounds as loose .wav files through BSAudioManager::GetSoundHandleByFile. We hook that
    // function's entry and remember every handle built from one of PPA's impact files, and count every play of one of
    // those handles; Tick hands each to her ImpactTiming (ConsumeClaps): the sounds say that the two collide, and her
    // ripple starts when his thrust lands.
    std::mutex                                           clapLock;   // the sound hooks read clapIDs on other threads
    std::vector<std::pair<std::uint32_t, std::uint32_t>> clapIDs;    // (file hash, extension) of the impact wavs
    std::size_t                                          clapFiles{ 0 };
    bool                                                 soundHooks{ false };   // both sound hooks are in
    std::atomic<int>                                     pendingClaps{ 0 };

    // sound impacts work: Trigger asks for them, the hooks are in and PPA's impact files were found
    bool SoundActive() { return cfg.soundTrigger && soundHooks && clapFiles > 0; }
    // motion detection: asked for, or standing in for sound impacts that don't work
    bool MotionActive() { return cfg.motionTrigger || (cfg.soundTrigger && !SoundActive()); }

    bool GlobMatch(const std::string& a_name, const std::string& a_pat)
    {
        auto star = a_pat.find('*');
        if (star == std::string::npos) return a_name == a_pat;
        auto pre = a_pat.substr(0, star), post = a_pat.substr(star + 1);
        return a_name.size() >= pre.size() + post.size() && a_name.compare(0, pre.size(), pre) == 0 &&
               a_name.compare(a_name.size() - post.size(), post.size(), post) == 0;
    }

    std::uint32_t ExtKey(const RE::BSResource::FileID& a_id)
    {
        std::uint32_t v;
        std::memcpy(&v, a_id.ext, 4);
        return v;
    }

    void BuildClapIDs()
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ids;
        std::size_t                                          files = 0;
        for (auto& g : cfg.soundTrigger ? cfg.clapGlobs : std::vector<std::string>{}) {   // Trigger = motion: no sound is looked for
            try {
                std::filesystem::path rel(g);
                auto                  dir = std::filesystem::path("Data") / rel.parent_path();
                auto                  pat = Lowered(rel.filename().string());
                std::error_code       ec;
                for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
                    std::error_code fe;
                    if (!e.is_regular_file(fe)) continue;
                    if (!GlobMatch(Lowered(e.path().filename().string()), pat)) continue;
                    for (auto& variant : { (rel.parent_path() / e.path().filename()).string(),
                             (std::filesystem::path("Data") / rel.parent_path() / e.path().filename()).string() }) {
                        RE::BSResource::ID id;
                        id.GenerateFromPath(variant.c_str());
                        ids.emplace_back(id.file, ExtKey(id));
                    }
                    ++files;
                }
            } catch (const std::exception& e) {
                SKSE::log::warn("impact sounds: '{}' could not be searched ({})", g, e.what());
            }
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        {
            std::scoped_lock l(clapLock);
            clapIDs = std::move(ids);
        }
        clapFiles = files;
        if (!cfg.soundTrigger) return;
        SKSE::log::info("impact sounds: {} PPA sound file(s) recognised", files);
        if (files == 0)
            SKSE::log::warn("impact sounds: no PPA impact sound file found (ClapSounds) - motion detection is used instead");
    }

    bool IsClapFile(const RE::BSResource::ID& a_id)
    {
        std::scoped_lock l(clapLock);
        return std::binary_search(clapIDs.begin(), clapIDs.end(), std::make_pair(a_id.file, ExtKey(a_id)));
    }

    // length of the simple prologue instructions we are willing to relocate (no branches, no RIP-relative)
    int InstrLen(const std::uint8_t* a_p)
    {
        int n = 0;
        if ((a_p[0] & 0xF0) == 0x40) ++n;   // REX
        std::uint8_t op = a_p[n++];
        if (op >= 0x50 && op <= 0x5F) return n;   // push / pop reg
        auto modrm = [&](int imm) -> int {
            std::uint8_t m = a_p[n++];
            int          mod = m >> 6, rm = m & 7;
            if (mod == 0 && rm == 5) return -1;   // RIP-relative
            if (mod != 3 && rm == 4) {
                std::uint8_t sib = a_p[n++];
                if (mod == 0 && (sib & 7) == 5) n += 4;
            }
            if (mod == 1) n += 1;
            else if (mod == 2) n += 4;
            return n + imm;
        };
        switch (op) {
        case 0x89:
        case 0x8B:
        case 0x8D:
        case 0x31:
        case 0x33:
        case 0x85:
        case 0x3B:
        case 0x39:
        case 0x2B:
        case 0x03:
        case 0x01:
        case 0x29:
        case 0x0B:
        case 0x09:
        case 0x23:
        case 0x21:
            return modrm(0);
        case 0x83:
            return modrm(1);
        case 0x81:
        case 0xC7:
            return modrm(4);
        default:
            return -1;
        }
    }

    // Detour a function entry: relocate its first whole instructions (only simple ones, see InstrLen) into a stub
    // that jumps back, and put a 5-byte jump to a_thunk in their place. Returns the stub (the original) or 0.
    std::uintptr_t EntryHook(std::uintptr_t a_addr, std::uintptr_t a_thunk, const char* a_what)
    {
        auto*       p = reinterpret_cast<const std::uint8_t*>(a_addr);
        std::string bytes;
        for (int i = 0; i < 16; ++i) bytes += std::format("{:02X} ", p[i]);
        int len = 0;
        while (len < 5) {
            int l = InstrLen(p + len);
            if (l <= 0) {
                SKSE::log::warn("impact sounds: could not hook {} (prologue {})", a_what, bytes);
                return 0;
            }
            len += l;
        }
        auto& tr = SKSE::GetTrampoline();
        auto* stub = static_cast<std::uint8_t*>(tr.allocate(static_cast<std::size_t>(len) + 14));
        std::memcpy(stub, p, static_cast<std::size_t>(len));
        std::uint8_t jmp[6]{ 0xFF, 0x25, 0, 0, 0, 0 };   // jmp qword ptr [rip+0] ; <address after the relocated code>
        std::memcpy(stub + len, jmp, 6);
        std::uintptr_t back = a_addr + static_cast<std::uintptr_t>(len);
        std::memcpy(stub + len + 6, &back, 8);
        tr.write_branch<5>(a_addr, a_thunk, true);
        if (len > 5) {
            std::vector<std::uint8_t> nops(static_cast<std::size_t>(len - 5), 0x90);
            REL::safe_write(a_addr + 5, nops.data(), nops.size());
        }
        SKSE::log::info("impact sounds: {} hooked (prologue {}, {} bytes relocated)", a_what, bytes, len);
        return reinterpret_cast<std::uintptr_t>(stub);
    }

    // PPA loads each impact sound once and replays the handle, so: remember the handles built from PPA's impact
    // files (load hook) and count every Play of one of them (play hook).
    std::mutex                handleLock;
    std::deque<std::uint32_t> clapHandles;
    // PPA keeps a few dozen impact sounds loaded at a time: the handles of this many are remembered, the oldest drop out.
    constexpr std::size_t kMaxClapHandles = 256;
    std::atomic<int>          statLoads{ 0 }, statClapLoads{ 0 }, statClapPlays{ 0 }, statSoundSkipped{ 0 };

    bool IsClapHandle(std::uint32_t a_id)
    {
        std::scoped_lock l(handleLock);
        return std::find(clapHandles.begin(), clapHandles.end(), a_id) != clapHandles.end();
    }

    struct SoundLoadHook
    {
        using Fn = void (*)(RE::BSAudioManager*, RE::BSSoundHandle&, const RE::BSResource::ID&, std::uint32_t, std::uint32_t);
        static inline Fn original{ nullptr };
        static void      thunk(RE::BSAudioManager* a_mgr, RE::BSSoundHandle& a_handle, const RE::BSResource::ID& a_id, std::uint32_t a_flags,
                 std::uint32_t a_priority)
        {
            original(a_mgr, a_handle, a_id, a_flags, a_priority);
            ++statLoads;
            if (a_handle.soundID != RE::BSSoundHandle::kInvalidID && IsClapFile(a_id)) {
                ++statClapLoads;
                std::scoped_lock l(handleLock);
                clapHandles.push_back(a_handle.soundID);
                while (clapHandles.size() > kMaxClapHandles) clapHandles.pop_front();
            }
        }
    };

    struct SoundPlayHook
    {
        using Fn = bool (*)(RE::BSSoundHandle*);
        static inline Fn original{ nullptr };
        static bool      thunk(RE::BSSoundHandle* a_handle)
        {
            bool r = original(a_handle);
            if (a_handle && a_handle->soundID != RE::BSSoundHandle::kInvalidID && IsClapHandle(a_handle->soundID)) {
                ++statClapPlays;
                ++pendingClaps;
            }
            return r;
        }
    };

    bool InstallSoundHooks()
    {
        auto ld = EntryHook(REL::RelocationID(66402, 67664).address(), reinterpret_cast<std::uintptr_t>(&SoundLoadHook::thunk), "sound loader");
        if (!ld) return false;
        SoundLoadHook::original = reinterpret_cast<SoundLoadHook::Fn>(ld);
        auto pl = EntryHook(REL::RelocationID(66355, 67616).address(), reinterpret_cast<std::uintptr_t>(&SoundPlayHook::thunk), "sound player");
        if (!pl) return false;
        SoundPlayHook::original = reinterpret_cast<SoundPlayHook::Fn>(pl);
        return true;
    }

    void SoundStats()
    {
        static Clock::time_point last{};
        static int               lastLoads = 0;
        if (!cfg.debug || Clock::now() - last < std::chrono::seconds(5)) return;
        last = Clock::now();
        int loads = statLoads.load(), cl = statClapLoads.exchange(0), cp = statClapPlays.exchange(0), sk = statSoundSkipped.exchange(0);
        if (cl || cp)
            SKSE::log::info("impact sounds (last 5 s): {} sound loads ({} PPA impact files), {} PPA impact plays, {} skipped by the rules",
                loads - lastLoads, cl, cp, sk);
        lastLoads = loads;
    }

    // A PPA impact sound just started: it belongs to the woman whose partner is closest to her. It says the two collide:
    // the thrust that landed just before it, or the next one, ripples (Detect); it starts a ripple itself only while no
    // thrust can be seen.
    void ConsumeClaps()
    {
        int n = pendingClaps.exchange(0);
        if (n <= 0 || !SoundActive()) return;
        Tracked* best = nullptr;
        float    bd = cfg.partnerDistance;
        for (auto& t : tracked)
            if (t->strokes.Have() && t->partner && !t->shapes.empty() && t->strokes.Distance() < bd) {
                bd = t->strokes.Distance();
                best = t.get();
            }
        if (!best) return;
        const auto act = best->timing.OnSound(Now(), best->lastImpact);
        if (!RuleFor(*best).on) {
            ++statSoundSkipped;
            return;
        }
        if (act == ImpactTiming::Sound::kThrust) {
            // the thrust's own sound came just after it landed: the ripple starts where it landed
            Fire(*best, &best->landContact, cfg.soundStrength,
                std::format("thrust landed, {} at {:.1f}, approach {:.0f}/s, its sound right after", best->partnerName, best->landStroke.dmin,
                    best->landStroke.peak),
                true);
            return;
        }
        if (act != ImpactTiming::Sound::kOwn) return;
        const Partner* p = nullptr;
        for (auto& q : partners)
            if (q.id == best->partner) p = &q;
        if (p && p->contact) {
            V3 contact = p->contact->world.translate;
            Fire(*best, &contact, cfg.soundStrength, std::format("impact sound, no thrust seen, {} at {:.1f}", p->name, bd), true);
        } else {
            Fire(*best, nullptr, cfg.soundStrength, "impact sound, no thrust seen", true);
        }
    }

    // ------------------------------------------------------------------------------------------------ thrusts
    // The distance from her nearest partner's genital base to the middle of her buttocks, every frame; a thrust that
    // lands ripples (at Strength) when an impact sound played lately or plays right after it (ConsumeClaps), or, with
    // motion detection, when it is fast enough and lands where they meet (ContactGate).
    void Detect(Tracked& a_t, float a_dt)
    {
        if (!Alive(a_t)) {   // no body to watch now (or her 3D is being replaced): nobody is close to her
            a_t.strokes.Reset();
            a_t.partner = 0;
            return;
        }
        auto& s0 = *a_t.shapes[0];
        auto* pel = PelvisNode(s0);
        if (!pel) return;
        V3             buttW = pel->world * (s0.pelvisS2B * s0.mid);   // middle of her buttocks: skin -> pelvis bone -> world
        const Partner* best = nullptr;
        float          bestD = cfg.partnerDistance;
        for (auto& p : partners) {
            if (p.id == a_t.id || !p.contact) continue;
            float d = (p.contact->world.translate - buttW).Length();
            if (d < bestD) {
                bestD = d;
                best = &p;
            }
        }
        if (!best) {
            a_t.strokes.Reset();
            a_t.partner = 0;
            return;
        }
        if (best->id != a_t.partner) {
            a_t.partner = best->id;
            a_t.partnerName = best->name;
            a_t.strokes.Reset();
            a_t.landings.Reset();
        }
        const float D = bestD;
        const auto  thrust = a_t.strokes.Update(D, a_dt, cfg.minApproach);

        // calibration statistics while someone is close to her
        if (cfg.debug && D < 40.0f) {
            a_t.statT += a_dt;
            a_t.statMin = std::min(a_t.statMin, D);
            a_t.statMax = std::max(a_t.statMax, D);
            a_t.statPeak = std::max(a_t.statPeak, -a_t.strokes.Speed());
            if (a_t.statT >= 3.0f) {
                SKSE::log::info("{}: {} - distance {:.1f}..{:.1f}, fastest approach {:.0f}/s, strokes {}, ripples {}, skipped by the rules {} (last 3 s)",
                    a_t.name, a_t.partnerName, a_t.statMin, a_t.statMax, a_t.statPeak, a_t.statStrokes, a_t.statImpacts, a_t.statSkipped);
                a_t.statT = 0.0f;
                a_t.statMin = 1e9f;
                a_t.statMax = 0.0f;
                a_t.statPeak = 0.0f;
                a_t.statImpacts = a_t.statStrokes = a_t.statSkipped = 0;
            }
        }

        if (!thrust) return;
        ++a_t.statStrokes;
        const float now = Now();
        V3          contact = best->contact->world.translate;
        if (SoundActive()) {
            // with the impact sounds every thrust that lands counts: a sound is the proof that the two collide
            a_t.landContact = contact;
            a_t.landStroke = *thrust;
            const auto act = a_t.timing.OnThrust(now, a_t.lastImpact);
            if (act == ImpactTiming::Thrust::kRipple) {
                Fire(a_t, &contact, cfg.soundStrength, std::format("thrust landed, {} at {:.1f}, approach {:.0f}/s", best->name, thrust->dmin, thrust->peak),
                    true);
                return;
            }
            if (act == ImpactTiming::Thrust::kSkip) return;   // a sound confirmed it, but the last wave is still crossing
        }
        // motion detection: a thrust that lands about as close to her as his other recent thrusts, within
        // ContactDistance (ContactGate: wherever her body, his and the pose put the point where they meet), fast enough,
        // and MinInterval after her last ripple; the same strength as a thrust the sounds confirm, whatever the pose
        if (!MotionActive()) return;
        const bool met = a_t.landings.Lands(now, thrust->dmin, cfg.contactDistance);
        if (!met || thrust->peak < cfg.minApproach || now - a_t.lastImpact < cfg.minInterval) return;
        Fire(a_t, &contact, cfg.soundStrength, std::format("thrust landed, {} at {:.1f}, approach {:.0f}/s (motion)", best->name, thrust->dmin, thrust->peak),
            true);
    }

    // ------------------------------------------------------------------------------------------------ drawing
    // The shape's buffer for this frame: the current data (in the swapped-in layout) with the rest positions of the
    // ripple's vertices moved along their normals; normals tilted on meshes with ordinary normal maps; colours = rest
    // colour * Shade().
    // a_alive: her 3D is the one her shapes were scanned from (Alive) - her skeleton may be read
    void Draw(RShape& a_s, float a_now, bool a_alive)
    {
        // Debug: the render passes the shader drew with after its vertex colour flag changed - the lighting technique
        // with vertex colours has bit 0 set (vc 1); "none" = she was not drawn in those frames
        if (a_s.passLog && --a_s.passLog == 0 && a_s.passProp) {
            std::string list;
            int         n = 0;
            for (auto* p = a_s.passProp->renderPassList.head; p && n < 8; p = p->next, ++n)
                list += std::format(" {:08X} (vc {})", p->passEnum, (p->passEnum - 0x4800002Du) & 1u);
            SKSE::log::info("  '{}': render passes{}, state {:#x}, vertex colour flag {}", a_s.name, list.empty() ? " none" : list,
                static_cast<std::uint32_t>(a_s.passProp->lastRenderPassState),
                a_s.passProp->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kVertexColors) ? "on" : "off");
            a_s.passProp.reset();
        }
        // an impact's waves are kept until the latest region's (its delay later) has run MaxAge
        // (the regions this shape has: one switched off in the tab keeps rippling until her meshes are set up again)
        float maxDelay = 0.0f;
        for (std::uint8_t l = 0; l < kLobes; ++l)
            if (a_s.lobes & (1u << l)) maxDelay = std::max(maxDelay, cfg.regions[kRegionOf[l]].delay);
        std::erase_if(a_s.events, [&](const Event& e) { return a_now - e.t0 > cfg.maxAge + maxDelay; });
        if (a_s.events.empty() || a_s.shared) {
            a_s.events.clear();
            Restore(a_s);
            return;
        }
        if (!StillValid(a_s)) {   // changed under us: what is still ours goes back now (and the flag off); Refresh rescans her
            Restore(a_s);
            return;
        }
        if (!SwapIn(a_s)) {
            a_s.events.clear();
            return;
        }
        // copies of her body under her 3D with a vertex colour flag on (a RaceMenu overlay built while she ripples copies
        // her shader's flags): BeforeRender and Restore watch them. One stays watched until its flag is off, wherever
        // it draws from by then.
        if (a_s.layered) {
            const auto* herNow = PartitionOf(a_s.geom.get());
            const auto  vc = [](const RE::BSGeometry* g) {
                auto* sp = g ? g->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
                return sp && sp->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kVertexColors);
            };
            std::erase_if(a_s.flagged, [&](const auto& g) { return !vc(g.get()); });
            ForOtherDrawers(a_s.top.get(), a_s.geom.get(), [&](RE::BSGeometry* g, auto* p, const auto&) {
                if (p && (p == a_s.part.get() || p == herNow) && vc(g) &&
                    std::none_of(a_s.flagged.begin(), a_s.flagged.end(), [&](const auto& f) { return f.get() == g; }))
                    a_s.flagged.emplace_back(g);
            });
        }
        auto* ctx = Context();
        if (!ctx) return;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (HRESULT hr = ctx->Map(a_s.upload, 0, D3D11_MAP_WRITE_DISCARD, 0, &m); FAILED(hr) || !m.pData) {
            static bool once = false;
            if (!once) {
                once = true;
                SKSE::log::error("'{}': could not map the vertex buffer (hr {:#x})", a_s.name, static_cast<std::uint32_t>(hr));
            }
            return;
        }
        auto* out = static_cast<std::uint8_t*>(m.pData);
        std::memcpy(out, a_s.curRaw, a_s.curBytes);
        // the colour layer: white everywhere (her data holds the position's w there), the bands below on top
        if (a_s.layered) PaintWhite(out, a_s.vcount, a_s.curStride, static_cast<std::uint32_t>(a_s.curCOff));
        const bool writeC = a_s.colour != Colour::kNone && a_s.curCOff >= 0;
        // shading light: above and behind her (butt, thighs) - above and in front of her for the belly and breasts. Or (Band
        // light = camera) above the viewer's side of her, as high as the fixed light, for every region: from behind her
        // that is the fixed light itself. (Her skeleton is read only while it is alive; otherwise the fixed light.)
        const V3 lBack = Normalized({ 0.0f, -0.45f, 0.89f }), lFront = Normalized({ 0.0f, 0.45f, 0.89f });
        const bool aa = cfg.allAround;
        bool       camLight = false;
        V3         lCam{};
        if (cfg.bandLight == 1 && a_alive)
            if (auto* pcam = RE::PlayerCamera::GetSingleton(); pcam && pcam->cameraRoot)
                if (auto inv = WorldToSkin(a_s)) {
                    V3 toCam = *inv * pcam->cameraRoot->world.translate - a_s.mid;
                    toCam.z = 0.0f;   // only the side she is seen from: the light's height stays the fixed light's
                    if (toCam.Length() > 1e-3f) {
                        lCam = Normalized(Mul(Normalized(toCam), 0.45f) + V3(0.0f, 0.0f, 0.89f));
                        camLight = true;
                    }
                }
        // each region's wave this frame: the [Ripple] wave with its multipliers, its strength and delay (Regions.h)
        struct Run
        {
            WaveConfig wave;
            float      strength, delay;
        };
        std::array<Run, kRegionCount> run{};
        for (int r = 0; r < kRegionCount; ++r) run[r] = { WaveFor(cfg.wave, cfg.regions[r]), cfg.regions[r].strength, cfg.regions[r].delay };
        for (std::size_t i = 0; i < a_s.idx.size(); ++i) {
            const V3& p = a_s.P[i];
            const V3& n = a_s.N[i];
            float     d = 0.0f;
            V3        grad{ 0, 0, 0 };
            bool      any = false;
            for (int slot = 0; slot < 2; ++slot) {
                const std::uint8_t lobe = slot ? a_s.lobe2[i] : a_s.cheek[i];
                if (lobe == kNoLobe) continue;
                const float  wv = slot ? a_s.w2[i] : a_s.w[i];
                const Region rgn = kRegionOf[lobe];
                const Run&   rn = run[rgn];
                // one wave centre's share a_k of this vertex's wave. (An optional region - all around, the butt too - skips
                // where its packet and dent have died out: < 1e-6 of the wave.)
                const auto add = [&](const V3& a_c, float a_tau, float a_es, float a_k) {
                    const V3    rel = p - a_c;
                    const float r = rel.Length();
                    if ((rgn != kButt || aa) && std::fabs(r - rn.wave.waveSpeed * a_tau) > 4.0f * rn.wave.packetWidth && r > 4.0f * rn.wave.dentRadius)
                        return;
                    const float f = Field(rn.wave, r, a_tau, a_es);
                    const float df = (Field(rn.wave, r + 0.05f, a_tau, a_es) - Field(rn.wave, std::max(0.0f, r - 0.05f), a_tau, a_es)) / 0.1f;
                    const float k = wv * a_k;
                    any = true;
                    d += f * k;
                    if (r > 1e-5f) {
                        V3 u = Mul(rel, 1.0f / r);
                        V3 ut = u - Mul(n, Dot(u, n));
                        grad += Mul(ut, df * k);
                    }
                };
                for (auto& e : a_s.events) {
                    if (!(e.lobes & (1u << lobe))) continue;
                    const float tau = a_now - e.t0 - rn.delay;
                    if (tau < 0.0f || tau > cfg.maxAge) continue;
                    const float es = e.s * rn.strength;
                    // all around: the twin centre on her other side blends in across the middle between the two (over two
                    // wavelengths), so both sides' waves are one field - no crease where the nearer centre changes
                    const int tw = aa ? kTwin[lobe] : -1;
                    float     own = 1.0f;
                    if (tw >= 0 && (e.lobes & (1u << tw)))
                        own = Smooth(((p - e.c[tw]).Length() - (p - e.c[lobe]).Length()) / (2.0f * rn.wave.wavelength) + 0.5f);
                    if (own > 0.0f) add(e.c[lobe], tau, es, own);
                    if (own < 1.0f) add(e.c[tw], tau, es, 1.0f - own);
                }
            }
            if (!any) continue;   // at rest: the copied data already holds it
            const std::size_t at = static_cast<std::size_t>(a_s.idx[i]) * a_s.curStride;
            auto*             v = out + at;
            // MotionScale: the movement and the lighting tilt that goes with it; the bands below keep the wave's own slope
            WritePos(v, a_s.fullPrec, p + Mul(n, d * cfg.motionScale));
            if (a_s.writeN) WriteNormal(v + a_s.nOff, Normalized(a_s.Nst[i] - Mul(grad, cfg.motionScale)));
            if (writeC) {
                const V3&   L = camLight ? lCam : (kRegionOf[a_s.cheek[i]] == kBelly || kRegionOf[a_s.cheek[i]] == kBreasts ? lFront : lBack);
                const V3    n2 = Normalized(n - grad);
                const auto  shade = Shade(cfg.shade, Dot(n2, L) - Dot(n, L));
                auto*       c = v + a_s.curCOff;
                const auto* c0 = a_s.curRaw + at + a_s.curCOff;   // her own colours (kFlag / kOwn); the layer's are white
                for (std::size_t k = 0; k < 3; ++k) c[k] = static_cast<std::uint8_t>(std::lround((a_s.layered ? 255.0f : c0[k]) * shade[k]));
            }
        }
        ctx->Unmap(a_s.upload, 0);
        ctx->CopyResource(a_s.mine, a_s.upload);   // into the buffer that is drawn, on the GPU
    }

    // ------------------------------------------------------------------------------------------------ per frame (main thread)
    std::filesystem::file_time_type g_iniTime{};
    std::atomic<bool>               g_dataLoaded{ false };
    bool                            g_failed = false;   // an exception in a frame: off for the session

    void ResetAll()
    {
        for (auto& t : tracked)
            for (auto& s : t->shapes) Restore(*s);
        tracked.clear();
        partners.clear();
        FlushGraveyard(true);
    }

    void ApplyConfig(Config a_c)
    {
        const bool globs = a_c.clapGlobs != cfg.clapGlobs;
        const bool trigger = a_c.soundTrigger != cfg.soundTrigger || a_c.motionTrigger != cfg.motionTrigger;
        cfg = std::move(a_c);
        ++g_cfgGen;
        g_testKey = cfg.testKey;
        g_logSteps = cfg.debug;
        g_slFollow = cfg.sexLab;
        LogConfig(cfg);
        if (globs && g_dataLoaded.load()) BuildClapIDs();
        if ((globs || trigger) && cfg.soundTrigger && !soundHooks)
            SKSE::log::warn("impact sounds: the sound hooks are installed only at game start (Trigger = sound / both then) - motion detection until a restart");
        if (sceneTable) LogSceneCounts(*sceneTable, cfg.rules);
        for (auto& t : tracked) {
            t->rescan = true;   // the meshes are set up again with the new settings
            t->ruleLine.clear();
        }
    }

    void CheckIni(Clock::time_point a_now)
    {
        static Clock::time_point last{};
        if (a_now - last < std::chrono::seconds(1)) return;
        last = a_now;
        std::error_code ec;
        const auto      t = std::filesystem::last_write_time(kIni, ec);
        if (ec || t == g_iniTime) return;
        g_iniTime = t;
        SKSE::log::info("ini changed - reloading");
        ApplyConfig(ReadConfig());
    }

    // ------------------------------------------------------------------------------------------------ the F11 settings tab
    // SKSE Menu Framework's window (SMF.h), when that framework is installed: every setting live. The tab edits a copy of
    // the settings; the frame (Tick) takes it over - values read every frame at once, those that shape her meshes (a
    // region on/off or its size, the shading switches) with her meshes set up again once the sliders rest. "Save"
    // writes them into the ini (its comments kept, missing keys added), "Reload" reads the ini again.

    // the settings that are fixed when her meshes are set up: a change needs them set up anew
    bool NeedsRescan(const Config& a_old, const Config& a_new)
    {
        if (a_old.regionScale != a_new.regionScale || a_old.shading != a_new.shading || a_old.addColourLayer != a_new.addColourLayer ||
            a_old.tiltNormals != a_new.tiltNormals || a_old.allAround != a_new.allAround)
            return true;
        for (int r = kThighs; r < kRegionCount; ++r)
            if (a_old.regions[r].enabled != a_new.regions[r].enabled || (a_new.regions[r].enabled && a_old.regions[r].size != a_new.regions[r].size))
                return true;
        return false;
    }

    std::string Num(float a_v) { return std::format("{}", a_v); }   // the shortest text that reads back as the same float

    // the ini's keys and their values now (the first name is the one written; the others are older names it may carry)
    std::vector<std::pair<std::vector<std::string>, std::string>> IniValues(const Config& a_c)
    {
        const auto b = [](bool a_v) { return std::string(a_v ? "1" : "0"); };
        const auto list = [](const std::vector<std::string>& a_l) {
            std::string s;
            for (const auto& e : a_l) s += (s.empty() ? "" : ", ") + e;
            return s;
        };
        const auto& w = a_c.wave;
        std::vector<std::pair<std::vector<std::string>, std::string>> v{
            { { "Enabled" }, b(a_c.enabled) },
            { { "Debug" }, b(a_c.debug) },
            { { "TestKey" }, std::format("0x{:02X}", a_c.testKey) },
            { { "Rules" }, b(a_c.rules.enabled) },
            { { "OutsideScenes", "OutsideOStim" }, b(a_c.rules.outsideOStim) },
            { { "SexLab" }, b(a_c.sexLab) },
            { { "ExcludeModpacks" }, list(a_c.rules.excludeModpacks) },
            { { "ExcludeScenePrefixes" }, list(a_c.rules.excludePrefixes) },
            { { "MinSpeed" }, Num(a_c.rules.minSpeed) },
            { { "MaxSpeed" }, Num(a_c.rules.maxSpeed) },
            { { "MinStage" }, std::to_string(a_c.rules.minStage) },
            { { "MaxStage" }, std::to_string(a_c.rules.maxStage) },
            { { "Amplitude" }, Num(w.amplitude) },
            { { "WaveSpeed" }, Num(w.waveSpeed) },
            { { "Wavelength" }, Num(w.wavelength) },
            { { "PacketWidth" }, Num(w.packetWidth) },
            { { "Decay" }, Num(w.decay) },
            { { "Spread" }, Num(w.spread) },
            { { "DentDepth" }, Num(w.dentDepth) },
            { { "DentRadius" }, Num(w.dentRadius) },
            { { "DentTime" }, Num(w.dentTime) },
            { { "MotionScale" }, Num(a_c.motionScale) },
            { { "RegionScale", "ButtSize" }, Num(a_c.regionScale) },
            { { "MaxAge" }, Num(a_c.maxAge) },
            { { "MaxEvents" }, std::to_string(a_c.maxEvents) },
            { { "Shading" }, b(a_c.shading) },
            { { "AddColourLayer" }, b(a_c.addColourLayer) },
            { { "ShadeGain" }, Num(a_c.shade.gain) },
            { { "ShadeDarkest" }, Num(a_c.shade.darkest) },
            { { "ShadeTint" }, std::format("{}, {}, {}", Num(a_c.shade.tint[0]), Num(a_c.shade.tint[1]), Num(a_c.shade.tint[2])) },
            { { "TiltNormals" }, b(a_c.tiltNormals) },
            { { "BandLight" }, a_c.bandLight == 1 ? "camera" : "fixed" },
            { { "AllAround" }, b(a_c.allAround) },
            { { "Trigger" }, a_c.soundTrigger && a_c.motionTrigger ? "both" : (a_c.soundTrigger ? "sound" : "motion") },
            { { "Strength", "SoundStrength" }, Num(a_c.soundStrength) },
            { { "ClapSounds" }, list(a_c.clapGlobs) },
            { { "MinApproachSpeed" }, Num(a_c.minApproach) },
            { { "ContactDistance" }, Num(a_c.contactDistance) },
            { { "MinInterval" }, Num(a_c.minInterval) },
            { { "ScanDistance" }, Num(a_c.scanDistance) },
            { { "PartnerDistance" }, Num(a_c.partnerDistance) },
        };
        for (int r = 0; r < kRegionCount; ++r) {
            const std::string n(kRegionNames[r]);
            const auto&       rc = a_c.regions[r];
            if (r != kButt) {
                v.push_back({ { n + "Enabled" }, b(rc.enabled) });
                v.push_back({ { n + "Size" }, Num(rc.size) });
            }
            v.push_back({ { n + "Strength" }, Num(rc.strength) });
            v.push_back({ { n + "Delay" }, Num(rc.delay) });
            v.push_back({ { n + "Speed" }, Num(rc.speed) });
            v.push_back({ { n + "Wavelength" }, Num(rc.wavelength) });
            v.push_back({ { n + "Decay" }, Num(rc.decay) });
            v.push_back({ { n + "Dent" }, Num(rc.dent) });
        }
        return v;
    }

    // The ini with a_c's values: every "Key = value" line it has gets the value (its key's spelling and the comments around
    // it stay), a key it lacks is added in a [Regions] section at the end. Its new time is ours: CheckIni does not reload it.
    bool WriteConfig(const Config& a_c)
    {
        std::string text;
        {
            std::ifstream in(kIni, std::ios::binary);
            if (in) text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const std::string nl = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
        auto              values = IniValues(a_c);
        std::vector<bool> written(values.size(), false);
        std::string       out;
        if (text.starts_with("\xEF\xBB\xBF")) {   // a UTF-8 mark at the start stays, and the first line is read without it
            out = "\xEF\xBB\xBF";
            text.erase(0, 3);
        }
        std::size_t       pos = 0;
        while (pos < text.size()) {
            std::size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string_view t = detail::Trim(line);
            if (!t.empty() && t[0] != ';' && t[0] != '#' && t[0] != '[')
                if (const auto eq = t.find('='); eq != std::string_view::npos) {
                    const std::string_view keyText = detail::Trim(t.substr(0, eq));
                    const std::string      key = Lowered(keyText);
                    for (std::size_t i = 0; i < values.size(); ++i) {
                        if (std::none_of(values[i].first.begin(), values[i].first.end(), [&](const std::string& k) { return Lowered(k) == key; })) continue;
                        line = std::format("{} = {}", keyText, values[i].second);
                        written[i] = true;
                        break;
                    }
                }
            out += line + nl;
        }
        std::string added;
        for (std::size_t i = 0; i < values.size(); ++i)
            if (!written[i]) added += std::format("{} = {}{}", values[i].first.front(), values[i].second, nl);
        if (!added.empty()) {
            if (text.find("[Regions]") == std::string::npos)
                out += nl + "[Regions]" + nl + "; Optional regions besides the butt: <Region><Setting>, Region = Butt / Thighs / Belly / Breasts" + nl;
            out += added;
        }
        std::ofstream o(kIni, std::ios::binary | std::ios::trunc);
        if (!o || !o.write(out.data(), static_cast<std::streamsize>(out.size()))) {
            SKSE::log::error("settings tab: could not write {}", kIni);
            return false;
        }
        o.close();
        std::error_code ec;
        g_iniTime = std::filesystem::last_write_time(kIni, ec);
        SKSE::log::info("settings tab: saved to {}", kIni);
        LogConfig(a_c);
        return true;
    }

    namespace Menu
    {
        namespace IG = SMF::ImGui;
        std::mutex         lock;   // the tab's copy and requests (the tab and Tick are both on the main thread; kept safe anyway)
        Config             working;
        std::uint32_t      workingGen = 0;   // the settings generation (g_cfgGen) working was copied from
        bool               have = false, live = false, save = false, reload = false, defaults = false;
        Clock::time_point  rescanAt{};   // her meshes are set up again then (the last change to a setting that shapes them + 0.4 s)
        bool               registered = false;

        bool Slider(const char* a_label, float* a_v, float a_lo, float a_hi, const char* a_fmt = "%.2f", const char* a_tip = nullptr)
        {
            const bool c = IG::SliderFloat(a_label, a_v, a_lo, a_hi, a_fmt, IG::ImGuiSliderFlags_AlwaysClamp);
            if (a_tip) IG::SetItemTooltip("%s", a_tip);
            return c;
        }

        bool SliderI(const char* a_label, int* a_v, int a_lo, int a_hi, const char* a_tip = nullptr)
        {
            const bool c = IG::SliderInt(a_label, a_v, a_lo, a_hi, "%d", IG::ImGuiSliderFlags_AlwaysClamp);
            if (a_tip) IG::SetItemTooltip("%s", a_tip);
            return c;
        }

        bool Check(const char* a_label, bool* a_v, const char* a_tip = nullptr)
        {
            const bool c = IG::Checkbox(a_label, a_v);
            if (a_tip) IG::SetItemTooltip("%s", a_tip);
            return c;
        }

        void __stdcall Render()
        {
            if (GetCurrentThreadId() != g_mainThread) {   // (the framework draws on the main thread; nothing is shared otherwise)
                static bool once = false;
                if (!once) {
                    once = true;
                    SKSE::log::warn("settings tab: drawn on thread {} instead of the main thread - not shown", GetCurrentThreadId());
                }
                return;
            }
            std::scoped_lock l(lock);
            if (!have || workingGen != g_cfgGen) {   // first open, or the ini was read again since
                working = cfg;
                have = true;
                workingGen = g_cfgGen;
            }
            auto& c = working;
            bool  ch = false;
            IG::Text("%s %s - every setting applies live; Save keeps them in the ini", BR_TITLE, kVersion);
            if (IG::Button("Test ripple##br.test")) testRequested = true;
            IG::SetItemTooltip("One full-strength ripple on the nearest woman (as the test key), on every region switched on");
            IG::SameLine();
            if (IG::Button("Save to ini##br.save")) save = true;
            IG::SameLine();
            if (IG::Button("Reload ini##br.reload")) reload = true;
            IG::SameLine();
            if (IG::Button("Defaults##br.defaults")) defaults = true;
            IG::SetItemTooltip("Every setting back to the shipped defaults (Save writes them)");
            ch |= Check("Enabled##br.enabled", &c.enabled);
            IG::SameLine();
            ch |= Check("Debug log##br.debug", &c.debug, "Log every ripple and the measured distances");

            if (IG::CollapsingHeader("Regions##br.regions", IG::ImGuiTreeNodeFlags_DefaultOpen)) {
                IG::TextWrapped("%s", "The butt, and optional regions on the same body: each its own strength, size, delay after the impact and "
                                      "wave. Where two overlap they share the skin. Their bones must be in the body (3BA, BHUNP, UBE).");
                ch |= Check("All around (no edges)##br.aa", &c.allAround,
                    "Every region a plain soft sphere: no cut by which way the skin faces, no gap at the cleft, one wave across her middle "
                    "and across her body's pieces. With a big butt Size (about 12) it covers her body without seams, fading out at her "
                    "neck, wrists and ankles - and the butt then takes the skin the other regions would have.");
                for (int r = 0; r < kRegionCount; ++r) {
                    auto& rc = c.regions[r];
                    IG::PushID(r);
                    IG::SeparatorText(std::string(kRegionNames[r]).c_str());
                    if (r != kButt) ch |= Check("On##en", &rc.enabled);
                    IG::BeginDisabled(r != kButt && !rc.enabled);
                    ch |= Slider("Strength##st", &rc.strength, 0.0f, 3.0f, "%.2f", "Multiplies the ripple's strength in this region");
                    if (r == kButt)
                        ch |= Slider("Size##sz", &c.regionScale, 0.5f, 15.0f, "%.2f",
                            "Rippling area, in distances between the two butt bones (RegionScale). All around, about 12 covers her body up to her "
                            "neck, wrists and ankles.");
                    else
                        ch |= Slider("Size##sz", &rc.size, 0.2f, 15.0f, "%.2f", "Radius, in distances between the two butt bones");
                    ch |= Slider("Delay##dl", &rc.delay, 0.0f, 0.5f, "%.3f s", "How long after the impact this region's wave starts");
                    if (IG::TreeNode("Wave##wv")) {
                        ch |= Slider("Speed x##sp", &rc.speed, 0.1f, 4.0f, "%.2f");
                        ch |= Slider("Wavelength x##wl", &rc.wavelength, 0.2f, 4.0f, "%.2f");
                        ch |= Slider("Decay x##dc", &rc.decay, 0.2f, 4.0f, "%.2f", "Longer = the wave lives longer");
                        ch |= Slider("Dent x##dn", &rc.dent, 0.0f, 4.0f, "%.2f", "The short dent where the wave starts");
                        IG::TreePop();
                    }
                    IG::EndDisabled();
                    IG::PopID();
                }
            }
            if (IG::CollapsingHeader("Wave##br.wave")) {
                ch |= Slider("Amplitude##br.amp", &c.wave.amplitude, 0.0f, 3.0f, "%.2f", "Height of the wave crest at full strength");
                ch |= Slider("Motion scale##br.ms", &c.motionScale, 0.0f, 5.0f, "%.2f", "How far the skin actually moves, as a multiple of the wave");
                ch |= Slider("Wave speed##br.ws", &c.wave.waveSpeed, 5.0f, 200.0f, "%.1f", "Units per second");
                ch |= Slider("Wavelength##br.wl", &c.wave.wavelength, 0.5f, 20.0f, "%.2f");
                ch |= Slider("Packet width##br.pw", &c.wave.packetWidth, 0.3f, 15.0f, "%.2f", "Length of the wave train (bigger = more rings)");
                ch |= Slider("Decay##br.dc", &c.wave.decay, 0.02f, 3.0f, "%.2f s");
                ch |= Slider("Spread##br.sp", &c.wave.spread, 0.5f, 50.0f, "%.1f", "Distance over which the wave thins out");
                ch |= Slider("Dent depth##br.dd", &c.wave.dentDepth, 0.0f, 3.0f, "%.2f");
                ch |= Slider("Dent radius##br.dr", &c.wave.dentRadius, 0.2f, 10.0f, "%.2f");
                ch |= Slider("Dent time##br.dt", &c.wave.dentTime, 0.005f, 0.5f, "%.3f s");
                ch |= Slider("Max age##br.ma", &c.maxAge, 0.1f, 5.0f, "%.2f s", "Each impact's wave runs this long");
                ch |= SliderI("Waves at once##br.me", &c.maxEvents, 1, 16);
            }
            if (IG::CollapsingHeader("Shading##br.shading")) {
                ch |= Check("Shading bands##br.sh", &c.shading);
                ch |= Check("Colour layer for bodies without vertex colours##br.cl", &c.addColourLayer);
                ch |= Check("Tilt normals (clothing)##br.tn", &c.tiltNormals);
                static const char* const lights[] = { "Fixed (above and behind her)", "Follows the camera" };
                ch |= IG::Combo("Band light##br.bl", &c.bandLight, lights, 2);
                IG::SetItemTooltip("%s", "The light the bands are shaded with. Follows the camera: above the side you see her from, as high as "
                                         "the fixed light - from behind her it is the fixed light itself, from any other side the bands still "
                                         "read as shading.");
                ch |= Slider("Band gain##br.sg", &c.shade.gain, 0.0f, 10.0f, "%.2f", "How strongly the bands darken");
                ch |= Slider("Darkest##br.sd", &c.shade.darkest, 0.0f, 1.0f, "%.2f", "The darkest a band may get (1 = no darkening)");
                ch |= IG::ColorEdit3("Tint##br.st", c.shade.tint.data(), IG::ImGuiColorEditFlags_Float);
                IG::SetItemTooltip("%s", "How much each channel follows the darkening (white = an even shadow)");
            }
            if (IG::CollapsingHeader("Rules##br.rules")) {
                ch |= Check("Rules on##br.ru", &c.rules.enabled, "Off: every impact ripples, any speed or stage");
                ch |= Check("Outside scenes##br.os", &c.rules.outsideOStim);
                ch |= Check("Follow SexLab scenes##br.slf", &c.sexLab);
                ch |= Slider("Min speed##br.mins", &c.rules.minSpeed, 0.0f, 10.0f, "%.1fx");
                ch |= Slider("Max speed (not at)##br.maxs", &c.rules.maxSpeed, 0.0f, 10.0f, "%.1fx");
                ch |= SliderI("Min stage##br.minst", &c.rules.minStage, 0, 20);
                ch |= SliderI("Max stage (not at)##br.maxst", &c.rules.maxStage, 0, 21);
            }
            if (IG::CollapsingHeader("Impacts##br.impacts")) {
                ch |= Slider("Strength##br.str", &c.soundStrength, 0.0f, 1.0f, "%.2f", "Ripple strength of a thrust");
                ch |= Slider("Min approach speed##br.map", &c.minApproach, 0.0f, 100.0f, "%.0f /s");
                ch |= Slider("Contact distance##br.cd", &c.contactDistance, 0.0f, 100.0f, "%.0f");
                ch |= Slider("Min interval##br.mi", &c.minInterval, 0.0f, 2.0f, "%.2f s");
            }
            if (ch) live = true;
        }

        // Tick, first thing: what the tab asked for
        void Consume(Clock::time_point a_now)
        {
            std::scoped_lock l(lock);
            if (reload) {
                reload = live = false;
                ApplyConfig(ReadConfig());
                have = false;
            }
            if (defaults) {
                defaults = live = false;
                Config d;
                d.testKey = cfg.testKey;
                ApplyConfig(d);
                have = false;
            }
            if (live && workingGen != g_cfgGen) {   // the ini was read again since the tab copied the settings: the ini wins
                live = false;
                have = false;
            }
            if (live) {
                live = false;
                const bool rescan = NeedsRescan(cfg, working);
                cfg = working;
                g_testKey = cfg.testKey;
                g_logSteps = cfg.debug;
                g_slFollow = cfg.sexLab;
                for (auto& t : tracked) t->ruleLine.clear();
                if (rescan) rescanAt = a_now + std::chrono::milliseconds(400);
            }
            if (rescanAt != Clock::time_point{} && a_now >= rescanAt) {
                rescanAt = {};
                for (auto& t : tracked) t->rescan = true;
                SKSE::log::info("settings tab: her meshes are set up again with the new settings");
            }
            if (save) {
                save = false;
                WriteConfig(cfg);
            }
        }

        // a_last: the last try (logged when the framework isn't there)
        void Register(bool a_last)
        {
            if (registered) return;
            if (SMF::AddSectionItem(BR_TITLE "/Settings", Render)) {
                registered = true;
                SKSE::log::info("settings tab: added to SKSE Menu Framework's window");
            } else if (a_last) {
                SKSE::log::info("settings tab: SKSE Menu Framework isn't installed - the ini holds every setting");
            }
        }
    }

    void TickFrame()
    {
        ++frameNo;
        const auto now = Clock::now();
        CheckIni(now);
        Menu::Consume(now);
        if (!sceneTable && g_tableReady.load()) {
            std::scoped_lock l(g_tableLock);
            sceneTable = g_tableDone;
        }
        UpdateScenes(now);
        UpdateSexLab(now);
        if (!cfg.enabled) {
            if (!tracked.empty()) {
                ResetAll();
                SKSE::log::info("Enabled = 0 - every mesh is back as the game set it up");
            }
            lastFrame = {};
            testRequested = false;   // (a test asked for now would fire whenever it is switched on again)
            return;
        }
        static const RE::BSFixedString loading(RE::LoadingMenu::MENU_NAME);
        if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(loading)) {
            lastFrame = {};
            testRequested = false;
            return;
        }
        // a frame longer than this (a hitch, a menu that paused the game) measures no speed: the motion detection starts
        // its distance anew, and no step is taken as longer than this
        constexpr float kHitch = 0.1f;
        float dt = lastFrame.time_since_epoch().count() ? std::chrono::duration<float>(now - lastFrame).count() : 0.0f;
        lastFrame = now;
        bool hitch = dt > kHitch;
        if (now - lastRefresh > std::chrono::milliseconds(500)) {
            lastRefresh = now;
            Refresh();
        }
        if (testRequested.exchange(false)) {
            auto*    player = RE::PlayerCharacter::GetSingleton();
            Tracked* best = nullptr;
            float    bd = 400.0f;
            for (auto& t : tracked) {
                auto a = t->handle.get();
                if (!a || !player || !Alive(*t)) continue;
                float d = (a->GetPosition() - player->GetPosition()).Length();
                if (d < bd) {
                    bd = d;
                    best = t.get();
                }
            }
            if (best) {
                std::string names;
                for (auto& s : best->shapes) names += std::format("{}'{}'{}", names.empty() ? "" : ", ", s->name, s->shared ? " (shared)" : "");
                if (Fire(*best, nullptr, 1.0f, std::format("test key, meshes: {}", names), false) == Fired::kNothing)
                    SKSE::log::info("test key: {} - her body mesh is shared with another woman, no ripple", best->name);
            } else {
                SKSE::log::info("test key: no woman with a supported body within 400 units");
            }
        }
        float t = Now();
        for (auto& tr : tracked) {
            if (hitch) tr->strokes.Forget();
            Detect(*tr, std::min(dt, kHitch));
        }
        ConsumeClaps();
        SoundStats();
        for (auto& tr : tracked) {
            const bool alive = cfg.bandLight == 1 && Alive(*tr);   // her skeleton may be read (Band light = camera)
            for (auto& s : tr->shapes) Draw(*s, t, alive);
        }
        FlushGraveyard(false);
    }

    // Right before the frame's render, on the main thread (the render call in Main::Update): every shader whose vertex
    // colour flag is on because of us must still draw blocks that carry colours. Between our frame and this render
    // another mod may have given her geometry a new skin partition (an immediate body morph: RaceMenu / OBody on the
    // main thread, a slider in RaceMenu) - the game's own blocks, without colours. A vertex shader reading colours from
    // a layout without them gets no input layout, and the game keeps that failure for every mesh of that layout for
    // the session: the flag goes off now, before any render pass is built (Tick puts the rest back). The same for her
    // geometry's shader should it be another one by now, and for the copies of her body Draw saw with a flag. With the
    // game paused (a menu: no frame jobs run beside the main thread) her whole 3D is walked too, for a copy made since.
    // It only ever switches flags off, so it also runs after an exception switched the ripple off.
    void BeforeRender()
    {
        if (GetCurrentThreadId() != g_mainThread) return;
        try {
            bool walk = false;
            if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) walk = true;
            if (auto* mn = RE::Main::GetSingleton(); mn && mn->GetRuntimeData().freezeTime) walk = true;
            for (auto& tr : tracked)
                for (auto& s : tr->shapes) {
                    if (!s->swapped) continue;
                    auto* herNow = PartitionOf(s->geom.get());
                    if (!HasColours(herNow)) {
                        if (s->flagProp) {
                            SetVertexColours(s->flagProp.get(), false);
                            WatchPasses(*s, s->flagProp.get());
                            s->flagProp.reset();
                            static bool once = false;
                            if (!once || cfg.debug) {
                                once = true;
                                SKSE::log::info("'{}' of {}: her mesh was replaced by another mod since the last frame - vertex colours off before the render",
                                    s->name, s->who);
                            }
                        }
                        DropCopiedFlag(s->geom.get(), herNow, nullptr, false);   // a shader of hers we did not flag
                    }
                    for (auto& g : s->flagged) DropCopiedFlag(g.get(), PartitionOf(g.get()), nullptr, false);
                    if (walk && s->layered)
                        ForOtherDrawers(s->top.get(), s->geom.get(), [&](RE::BSGeometry* g, auto* p, const auto&) {
                            if (p && (p == s->part.get() || p == herNow)) DropCopiedFlag(g, p, nullptr, false);
                        });
                }
        } catch (...) {
        }
    }

    // The frame, on the main thread, right after Main::Update has rendered it: what we write is drawn by the next
    // frame's render, which also builds any render passes marked stale before it draws. A call from any other thread
    // is refused. Every engine write happens on the main thread: here, or in ResetAll at a new game /
    // before a load (SKSE messages, on the main thread too).
    void Tick()
    {
        if (GetCurrentThreadId() != g_mainThread) {
            static bool once = false;
            if (!once) {
                once = true;
                SKSE::log::warn("frame: called on thread {} instead of the main thread {} - skipped", GetCurrentThreadId(), g_mainThread);
            }
            return;
        }
        if (g_failed) return;
        try {
            TickFrame();
        } catch (const std::exception& e) {
            g_failed = true;
            SKSE::log::error("frame: {} - the ripple is switched off for this session", e.what());
            try {
                ResetAll();
            } catch (...) {
            }
        }
    }

    // ------------------------------------------------------------------------------------------------ hooks and events
    std::string ModuleAt(std::uintptr_t a_addr)
    {
        auto* p = reinterpret_cast<const std::uint8_t*>(a_addr);
        if (p && p[0] == 0xFF && p[1] == 0x25 && *reinterpret_cast<const std::int32_t*>(p + 2) == 0) a_addr = *reinterpret_cast<const std::uintptr_t*>(p + 6);
        HMODULE mod = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(a_addr), &mod) ||
            !mod)
            return std::format("{:X} (no module)", a_addr);
        char path[MAX_PATH]{};
        GetModuleFileNameA(mod, path, MAX_PATH);
        return std::filesystem::path(path).filename().string();
    }

    // The frame's render call in Main::Update (SE 1.5.97 +0x5D2, AE 1.6.1170 +0xA97: it takes Main in rcx and nothing
    // else, and nothing reads what it leaves in rax): BeforeRender, then the render. Without it the colour layer never
    // goes in (BuildShape): the shading bands need that check every frame.
    struct RenderHook
    {
        static void thunk(void* a_main)
        {
            BeforeRender();
            func(a_main);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

#ifdef BR_ANY_VERSION
    // A test build for game versions other than SE 1.5.97 / AE 1.6.1170: where the known offset does not hold the call
    // it holds in those builds, the call is looked for in Main::Update's first kScan bytes - by what it calls (an
    // Address Library id), or by the shape it has in the supported builds.
    constexpr std::size_t kScan = 0x1400;

    std::vector<std::uintptr_t> CallsTo(std::uintptr_t a_fn, std::uintptr_t a_target)
    {
        std::vector<std::uintptr_t> out;
        for (std::size_t i = 0; i + 5 <= kScan; ++i) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(a_fn + i);
            if (p[0] == 0xE8 && a_fn + i + 5 + *reinterpret_cast<const std::int32_t*>(p + 1) == a_target) out.push_back(a_fn + i);
        }
        return out;
    }
#endif

    void InstallRenderHook()
    {
        REL::Relocation<std::uintptr_t> fn{ REL::RelocationID(35565, 36564) };
        const bool                      ae = REL::Module::IsAE();
        std::uintptr_t                  off = ae ? 0xA97 : 0x5D2;
        std::uintptr_t                  site = fn.address() + off;
        const auto*                     p = reinterpret_cast<const std::uint8_t*>(site);
        // the call's neighbourhood as it is in the two supported builds: SE "mov rcx, rbx; call; test dil, dil; je",
        // AE "mov rcx, r12; call; test r15b, r15b; je". Anything else: nothing is touched.
        bool shape = ae ? p[-3] == 0x49 && p[-2] == 0x8B && p[-1] == 0xCC && p[0] == 0xE8 && p[5] == 0x45 && p[6] == 0x84 && p[7] == 0xFF && p[8] == 0x74 :
                          p[-3] == 0x48 && p[-2] == 0x8B && p[-1] == 0xCB && p[0] == 0xE8 && p[5] == 0x40 && p[6] == 0x84 && p[7] == 0xFF && p[8] == 0x74;
#ifdef BR_ANY_VERSION
        // elsewhere: the one call in Main::Update to the frame's render function
        if (!shape)
            if (const auto calls = CallsTo(fn.address(), REL::RelocationID(35556, 36555).address()); calls.size() == 1) {
                site = calls[0];
                off = site - fn.address();
                p = reinterpret_cast<const std::uint8_t*>(site);
                shape = true;
                SKSE::log::info("render hook: the render call found at Main::Update+{:X} (untested game version)", off);
            }
#endif
        if (!shape) {
            SKSE::log::error("render hook: Main::Update+{:X} isn't the render call it is in the supported game builds (bytes {:02X} {:02X} {:02X}) - "
                             "no shading bands on meshes without vertex colours, the ripple itself is unaffected",
                off, p[-1], p[0], p[5]);
            return;
        }
        const std::uintptr_t before = site + 5 + *reinterpret_cast<const std::int32_t*>(p + 1);
        RenderHook::func = SKSE::GetTrampoline().write_call<5>(site, RenderHook::thunk);
        g_renderHooked = true;
        SKSE::log::info("render hook: Main::Update+{:X} chained; the call went to {} before", off, ModuleAt(before));
    }

    // The once-per-frame main-thread call in Main::Update: the original first, then our frame. Whatever the original
    // leaves in rax is handed back.
    struct FrameHook
    {
        static std::uint64_t thunk()
        {
            const std::uint64_t r = func();
            Tick();
            return r;
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    void InstallFrameHook()
    {
        static bool done = false;
        if (done) return;
        done = true;
        REL::Relocation<std::uintptr_t> fn{ REL::RelocationID(35565, 36564) };
        std::uintptr_t                  off = REL::Module::IsAE() ? 0xC26 : 0x748;
        std::uintptr_t                  site = fn.address() + off;
        const auto*                     p = reinterpret_cast<const std::uint8_t*>(site);
        // The call's neighbourhood as it is in the two supported builds: SE 1.5.97 +0x748 = the empty per-frame call, a
        // call before it, "mov rcx, [rip+x]" after it; AE 1.6.1170 +0xC26 = the call right before AE's empty one (the
        // same moment in the frame), then that call, then the same "mov rcx, [rip+x]". Anything else: nothing is touched.
        const auto shapeAt = [](const std::uint8_t* q) {
            return REL::Module::IsAE() ? q[-5] == 0xE8 && q[0] == 0xE8 && q[5] == 0xE8 && q[10] == 0x48 && q[11] == 0x8B && q[12] == 0x0D :
                                         q[-5] == 0xE8 && q[0] == 0xE8 && q[5] == 0x48 && q[6] == 0x8B && q[7] == 0x0D;
        };
        bool shape = shapeAt(p);
#ifdef BR_ANY_VERSION
        // elsewhere: the one call to the function the supported builds call there - or, should another plugin's hook sit
        // on it, the first call of that shape after the frame jobs are joined (the call to the job join), so that our
        // frame still runs with no frame job beside it
        if (!shape) {
            const char* how = nullptr;
            if (const auto calls = CallsTo(fn.address(), REL::RelocationID(76765, 21873).address()); calls.size() == 1) {
                site = calls[0];
                how = "by the function it calls";
            } else if (const auto joins = CallsTo(fn.address(), REL::RelocationID(34554, 35409).address()); joins.size() == 1) {
                for (std::size_t i = joins[0] - fn.address() + 5 + 5; i + 13 <= kScan; ++i)
                    if (shapeAt(reinterpret_cast<const std::uint8_t*>(fn.address() + i))) {
                        site = fn.address() + i;
                        how = "by its shape, after the frame jobs' join";
                        break;
                    }
            }
            if (how) {
                off = site - fn.address();
                p = reinterpret_cast<const std::uint8_t*>(site);
                shape = true;
                SKSE::log::info("frame hook: the per-frame call found at Main::Update+{:X} {} (untested game version)", off, how);
            }
        }
#endif
        if (!shape) {
            SKSE::log::error("frame hook: Main::Update+{:X} isn't the call it is in the supported game builds (bytes {:02X} {:02X} {:02X}) - "
                             "ripples off, nothing touched",
                off, p[-5], p[0], p[5]);
            return;
        }
        // the render call first: without its check (BeforeRender) the colour layer never goes in
        InstallRenderHook();
        const std::uintptr_t before = site + 5 + *reinterpret_cast<const std::int32_t*>(p + 1);
        FrameHook::func = SKSE::GetTrampoline().write_call<5>(site, FrameHook::thunk);
        SKSE::log::info("frame hook: Main::Update+{:X} chained; the call went to {} before (thread {}, plugins loaded on {})", off, ModuleAt(before),
            GetCurrentThreadId(), g_mainThread);
    }

    // the player's first update once the world runs (after the data load, a load or a new game): the frame hook goes
    // in there (main thread, outside the hooked call)
    std::atomic<bool> frameHookWanted{ false };
    struct PlayerUpdateHook
    {
        static void thunk(RE::PlayerCharacter* a_this, float a_delta)
        {
            if (frameHookWanted.load() && frameHookWanted.exchange(false)) InstallFrameHook();
            func(a_this, a_delta);
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static void                                    Install()
        {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
            func = vtbl.write_vfunc(REL::Module::IsVR() ? 0xAF : 0xAD, thunk);
        }
    };

    class InputSink : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        static InputSink* Get()
        {
            static InputSink s;
            return &s;
        }
        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
        {
            const int key = g_testKey.load(std::memory_order_relaxed);
            if (!a_event || key == 0) return RE::BSEventNotifyControl::kContinue;
            for (auto* e = *a_event; e; e = e->next) {
                auto* b = e->AsButtonEvent();
                if (b && b->GetDevice() == RE::INPUT_DEVICE::kKeyboard && b->IsDown() && static_cast<int>(b->GetIDCode()) == key) testRequested = true;
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void OnMessage(SKSE::MessagingInterface::Message* a_msg)
    {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kPostLoad:   // every plugin loaded: SKSE Menu Framework's too, if installed
            Menu::Register(false);
            break;
        case SKSE::MessagingInterface::kPostPostLoad:
            Menu::Register(true);   // (nothing when kPostLoad added it)
            ConnectOStim();
            break;
        case SKSE::MessagingInterface::kInputLoaded:
            if (auto* im = RE::BSInputDeviceManager::GetSingleton()) im->AddEventSink(InputSink::Get());
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            g_dataLoaded = true;
            BuildClapIDs();
            ConnectSexLab();
            try {
                std::thread(ReadScenes, cfg.rules).detach();
            } catch (const std::exception& e) {
                SKSE::log::error("OStim scenes: the reader could not start ({}) - every OStim scene counts as unknown", e.what());
            }
            // also armed here: a world entered without a load or a new game sends neither message
            frameHookWanted = true;
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            frameHookWanted = true;
            break;
        case SKSE::MessagingInterface::kNewGame:
            ResetAll();
            ClearScenes();
            ClearSexLab();
            frameHookWanted = true;
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            ResetAll();
            ClearScenes();
            ClearSexLab();
            break;
        default:
            break;
        }
    }

    void InitLog()
    {
        auto path = SKSE::log::log_directory();
        if (!path) return;
        *path /= BR_NAME ".log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S.%e] %v");
    }
}

SKSEPluginInfo(
    .Version = { BR_VERSION_MAJOR, BR_VERSION_MINOR, BR_VERSION_PATCH, 0 },
    .Name = BR_NAME,
    .Author = BR_AUTHOR,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    BR::g_mainThread = GetCurrentThreadId();   // plugins load on the game's main thread
    BR::InitLog();                             // after Init: NG's SKSE::Init installs its own logger
#ifdef BR_ANY_VERSION
    SKSE::log::info(BR_TITLE " {} (test build for game versions other than 1.5.97 / 1.6.1170)", BR::kVersion);
    // any game version, untested: off only in VR, or without an Address Library for this version (every game address
    // comes from it - without it CommonLib would stop the game)
    {
        const auto ver = a_skse->RuntimeVersion();
        if (REL::Module::IsVR()) {
            SKSE::log::error("this is Skyrim VR - not supported, " BR_TITLE " stays off (nothing hooked)");
            return true;
        }
        const auto      lib = std::format("Data/SKSE/Plugins/{}-{}.bin", REL::Module::IsAE() ? "versionlib" : "version", ver.string("-"));
        std::error_code ec;
        if (!std::filesystem::exists(lib, ec)) {
            SKSE::log::error("this is Skyrim {} and there is no Address Library for it ({} is missing) - " BR_TITLE " stays off (nothing hooked)",
                ver.string("."), lib);
            return true;
        }
        if (ver != SKSE::RUNTIME_SSE_1_5_97 && ver != SKSE::RUNTIME_SSE_1_6_1170)
            SKSE::log::warn("this is Skyrim {} - not one of the versions " BR_TITLE " was tested on (1.5.97, 1.6.1170): running anyway", ver.string("."));
    }
#else
    SKSE::log::info(BR_TITLE " {}", BR::kVersion);
    // the frame hook sits at an offset inside Main::Update, known for SE 1.5.97 and AE 1.6.1170: on any other game
    // version nothing is hooked at all
    if (const auto ver = a_skse->RuntimeVersion(); ver != SKSE::RUNTIME_SSE_1_5_97 && ver != SKSE::RUNTIME_SSE_1_6_1170) {
        SKSE::log::error("this is Skyrim {} - " BR_TITLE " is made for Skyrim Special Edition 1.5.97 and Anniversary Edition 1.6.1170 and stays off (nothing hooked)",
            ver.string("."));
        return true;   // loaded, but nothing of ours is ever called
    }
#endif
    SKSE::AllocTrampoline(256);   // the frame and render calls (14 bytes each), the two sound entry hooks (14 bytes + a stub each)
    BR::EnsureIni();
    {
        std::error_code ec;
        BR::g_iniTime = std::filesystem::last_write_time(BR::kIni, ec);
    }
    BR::cfg = BR::ReadConfig();
    BR::g_testKey = BR::cfg.testKey;
    BR::g_logSteps = BR::cfg.debug;
    BR::g_slFollow = BR::cfg.sexLab;
    BR::LogConfig(BR::cfg);
    BR::PlayerUpdateHook::Install();
    if (BR::cfg.soundTrigger) {
        BR::soundHooks = BR::InstallSoundHooks();
        if (!BR::soundHooks) SKSE::log::warn("impact sounds unavailable - motion detection is used instead");
    }
    BR::g_rawMessaging = static_cast<const SKSE::detail::SKSEMessagingInterface*>(a_skse->QueryInterface(SKSE::LoadInterface::kMessaging));
    SKSE::GetMessagingInterface()->RegisterListener(BR::OnMessage);
    return true;
}
