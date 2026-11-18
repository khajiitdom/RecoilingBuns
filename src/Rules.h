// Recoiling Buns: which impacts may ripple - the OStim scene file reader and the ripple rules. Header-only and free of
// game types, so the same code also runs outside the game.
//
// OStim describes each scene in one JSON file, Data/SKSE/Plugins/OStim/scenes/**/<id>.json; the scene id is the file
// name without ".json", in lower case. Two kinds of scene are told apart:
//  * speed-based scenes have several speeds, each with a displayed value (0.5x, 1x, 1.5x, ...) the player steps
//    through: OStim's own scenes and most animation packs made for OStim. They ripple from MinSpeed
//    (1.5x, included) up to MaxSpeed (2.5x, excluded): slower motion barely moves the body, faster motion turns the
//    wave into a flicker too quick to see.
//  * stage-based scenes (the packs converted from SexLab) have one speed each and carry their stage at the end of the
//    id ("<name>-<stage>"). They ripple from MinStage (3, included) up to MaxStage (5, excluded).
// Whole packs (a scene file's "modpack") and scene id prefixes can be excluded; both lists are empty by default.
#pragma once

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace BR
{
    // ---------------------------------------------------------------- text helpers
    namespace detail
    {
        inline char Lower(char a_c) noexcept { return a_c >= 'A' && a_c <= 'Z' ? static_cast<char>(a_c - 'A' + 'a') : a_c; }

        inline bool IsDigit(char a_c) noexcept { return a_c >= '0' && a_c <= '9'; }

        inline bool IsBlank(char a_c) noexcept
        {
            return a_c == ' ' || a_c == '\t' || a_c == '\r' || a_c == '\n' || a_c == '\f' || a_c == '\v';
        }

        // a_s without leading and trailing blanks
        inline std::string_view Trim(std::string_view a_s) noexcept
        {
            while (!a_s.empty() && IsBlank(a_s.front())) a_s.remove_prefix(1);
            while (!a_s.empty() && IsBlank(a_s.back())) a_s.remove_suffix(1);
            return a_s;
        }

        // ASCII case-insensitive comparisons: scene ids, pack names and ini entries are compared this way.
        inline bool EqualsNoCase(std::string_view a_a, std::string_view a_b) noexcept
        {
            if (a_a.size() != a_b.size()) return false;
            for (std::size_t i = 0; i < a_a.size(); ++i)
                if (Lower(a_a[i]) != Lower(a_b[i])) return false;
            return true;
        }

        inline bool StartsWithNoCase(std::string_view a_s, std::string_view a_prefix) noexcept
        {
            return a_s.size() >= a_prefix.size() && EqualsNoCase(a_s.substr(0, a_prefix.size()), a_prefix);
        }

        inline void LowerInPlace(std::string& a_s) noexcept
        {
            for (char& c : a_s) c = Lower(c);
        }

        // A decimal number std::from_chars matched but could not hold (result_out_of_range): +-infinity when its
        // magnitude lies above the double range, +-0 when below it. a_text: optional sign, digits, optional fraction
        // and exponent.
        inline double OutOfRange(std::string_view a_text) noexcept
        {
            std::size_t i = 0;
            const bool  negative = !a_text.empty() && a_text[0] == '-';
            if (!a_text.empty() && (a_text[0] == '-' || a_text[0] == '+')) ++i;
            bool      significant = false;   // a non-zero digit seen
            long long order = 0;             // decimal exponent of the first non-zero digit
            for (; i < a_text.size() && IsDigit(a_text[i]); ++i) {
                if (significant) ++order;
                else if (a_text[i] != '0') significant = true;
            }
            if (i < a_text.size() && a_text[i] == '.') {
                ++i;
                for (long long k = 1; i < a_text.size() && IsDigit(a_text[i]); ++i, ++k) {
                    if (!significant && a_text[i] != '0') {
                        significant = true;
                        order = -k;
                    }
                }
            }
            long long exponent = 0;
            if (i < a_text.size() && (a_text[i] == 'e' || a_text[i] == 'E')) {
                ++i;
                bool down = false;
                if (i < a_text.size() && (a_text[i] == '+' || a_text[i] == '-')) down = a_text[i++] == '-';
                for (; i < a_text.size() && IsDigit(a_text[i]); ++i) exponent = (std::min)(exponent * 10 + (a_text[i] - '0'), 1000000000LL);
                if (down) exponent = -exponent;
            }
            const double magnitude = significant && order + exponent > 0 ? std::numeric_limits<double>::infinity() : 0.0;
            return negative ? -magnitude : magnitude;
        }

        inline void AppendUtf8(std::string& a_out, std::uint32_t a_cp)
        {
            if (a_cp < 0x80) {
                a_out += static_cast<char>(a_cp);
            } else if (a_cp < 0x800) {
                a_out += static_cast<char>(0xC0 | (a_cp >> 6));
                a_out += static_cast<char>(0x80 | (a_cp & 0x3F));
            } else if (a_cp < 0x10000) {
                a_out += static_cast<char>(0xE0 | (a_cp >> 12));
                a_out += static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F));
                a_out += static_cast<char>(0x80 | (a_cp & 0x3F));
            } else {
                a_out += static_cast<char>(0xF0 | (a_cp >> 18));
                a_out += static_cast<char>(0x80 | ((a_cp >> 12) & 0x3F));
                a_out += static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F));
                a_out += static_cast<char>(0x80 | (a_cp & 0x3F));
            }
        }
    }

    // ---------------------------------------------------------------- JSON reader
    // A small recursive-descent reader for OStim's scene files. It accepts all of standard JSON and, because a file it
    // rejects is a scene without rules, a few things hand-edited files contain: a UTF-8 byte order mark, // and /* */
    // comments, a trailing comma before ] or }, raw control characters inside strings, and lone UTF-16 surrogates in
    // \u escapes (read as U+FFFD). Nesting is limited to kMaxDepth arrays / objects, so no input can exhaust the stack.
    // Bad input makes Document return false; it never throws (except std::bad_alloc, which ParseScene catches).
    namespace json
    {
        inline constexpr int kMaxDepth = 128;   // nested arrays / objects, the root included

        struct Value
        {
            enum class Kind : std::uint8_t
            {
                kNull,
                kBool,
                kNumber,
                kString,
                kArray,
                kObject
            };

            Kind                     kind = Kind::kNull;
            bool                     boolean = false;
            double                   number = 0.0;
            std::string              text;    // a string's value (UTF-8)
            std::vector<Value>       items;   // an array's elements, or an object's member values
            std::vector<std::string> keys;    // an object's member names, parallel to items, in file order

            // The object member called a_key (exact spelling; the last one when a name repeats, as JSON readers
            // usually resolve it); nullptr when there is none or this is not an object.
            const Value* Member(std::string_view a_key) const noexcept
            {
                if (kind != Kind::kObject) return nullptr;
                for (std::size_t i = keys.size(); i-- > 0;)
                    if (keys[i] == a_key) return &items[i];
                return nullptr;
            }
        };

        class Reader
        {
        public:
            explicit Reader(std::string_view a_text) noexcept :
                _p(a_text.data()), _end(a_text.data() + a_text.size())
            {}

            // The whole text as one value: false on a syntax error, on anything but blanks / comments after the
            // value, or past kMaxDepth nesting.
            bool Document(Value& a_out)
            {
                if (_end - _p >= 3 && _p[0] == '\xEF' && _p[1] == '\xBB' && _p[2] == '\xBF') _p += 3;
                return Space() && ReadValue(a_out, 0) && Space() && _p == _end;
            }

        private:
            // Skips blanks and comments; false only for an unterminated /* comment.
            bool Space() noexcept
            {
                while (_p < _end) {
                    const char c = *_p;
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                        ++_p;
                    } else if (c == '/' && _end - _p >= 2 && _p[1] == '/') {
                        _p += 2;
                        while (_p < _end && *_p != '\n') ++_p;
                    } else if (c == '/' && _end - _p >= 2 && _p[1] == '*') {
                        _p += 2;
                        for (;;) {
                            if (_end - _p < 2) return false;
                            if (_p[0] == '*' && _p[1] == '/') break;
                            ++_p;
                        }
                        _p += 2;
                    } else {
                        break;
                    }
                }
                return true;
            }

            bool Word(std::string_view a_word) noexcept
            {
                if (static_cast<std::size_t>(_end - _p) < a_word.size() || std::string_view(_p, a_word.size()) != a_word) return false;
                _p += a_word.size();
                return true;
            }

            // a_depth: the nesting depth of the container holding this value (0 for the root).
            bool ReadValue(Value& a_v, int a_depth)
            {
                if (_p == _end) return false;
                switch (*_p) {
                case '{':
                    return ReadObject(a_v, a_depth + 1);
                case '[':
                    return ReadArray(a_v, a_depth + 1);
                case '"':
                    a_v.kind = Value::Kind::kString;
                    return ReadString(a_v.text);
                case 't':
                    a_v.kind = Value::Kind::kBool;
                    a_v.boolean = true;
                    return Word("true");
                case 'f':
                    a_v.kind = Value::Kind::kBool;
                    a_v.boolean = false;
                    return Word("false");
                case 'n':
                    a_v.kind = Value::Kind::kNull;
                    return Word("null");
                default:
                    a_v.kind = Value::Kind::kNumber;
                    return ReadNumber(a_v.number);
                }
            }

            bool ReadObject(Value& a_v, int a_depth)
            {
                if (a_depth > kMaxDepth) return false;
                a_v.kind = Value::Kind::kObject;
                ++_p;   // {
                if (!Space()) return false;
                if (_p < _end && *_p == '}') {
                    ++_p;
                    return true;
                }
                for (;;) {
                    if (_p == _end || *_p != '"') return false;
                    a_v.keys.emplace_back();
                    if (!ReadString(a_v.keys.back()) || !Space() || _p == _end || *_p != ':') return false;
                    ++_p;
                    if (!Space()) return false;
                    a_v.items.emplace_back();
                    if (!ReadValue(a_v.items.back(), a_depth) || !Space() || _p == _end) return false;
                    if (*_p == '}') {
                        ++_p;
                        return true;
                    }
                    if (*_p != ',') return false;
                    ++_p;
                    if (!Space()) return false;
                    if (_p < _end && *_p == '}') {   // a trailing comma
                        ++_p;
                        return true;
                    }
                }
            }

            bool ReadArray(Value& a_v, int a_depth)
            {
                if (a_depth > kMaxDepth) return false;
                a_v.kind = Value::Kind::kArray;
                ++_p;   // [
                if (!Space()) return false;
                if (_p < _end && *_p == ']') {
                    ++_p;
                    return true;
                }
                for (;;) {
                    a_v.items.emplace_back();
                    if (!ReadValue(a_v.items.back(), a_depth) || !Space() || _p == _end) return false;
                    if (*_p == ']') {
                        ++_p;
                        return true;
                    }
                    if (*_p != ',') return false;
                    ++_p;
                    if (!Space()) return false;
                    if (_p < _end && *_p == ']') {   // a trailing comma
                        ++_p;
                        return true;
                    }
                }
            }

            bool Hex4(std::uint32_t& a_cp) noexcept
            {
                if (_end - _p < 4) return false;
                std::uint32_t v = 0;
                for (int i = 0; i < 4; ++i) {
                    const char    c = *_p++;
                    std::uint32_t d = 0;
                    if (c >= '0' && c <= '9') d = static_cast<std::uint32_t>(c - '0');
                    else if (c >= 'a' && c <= 'f') d = static_cast<std::uint32_t>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') d = static_cast<std::uint32_t>(c - 'A' + 10);
                    else return false;
                    v = (v << 4) | d;
                }
                a_cp = v;
                return true;
            }

            bool ReadString(std::string& a_out)
            {
                ++_p;   // the opening quote
                for (;;) {
                    const char* run = _p;
                    while (_p < _end && *_p != '"' && *_p != '\\') ++_p;
                    a_out.append(run, static_cast<std::size_t>(_p - run));
                    if (_p == _end) return false;
                    if (*_p++ == '"') return true;
                    if (_p == _end) return false;
                    switch (*_p++) {
                    case '"': a_out += '"'; break;
                    case '\\': a_out += '\\'; break;
                    case '/': a_out += '/'; break;
                    case 'b': a_out += '\b'; break;
                    case 'f': a_out += '\f'; break;
                    case 'n': a_out += '\n'; break;
                    case 'r': a_out += '\r'; break;
                    case 't': a_out += '\t'; break;
                    case 'u':
                        {
                            std::uint32_t cp = 0;
                            if (!Hex4(cp)) return false;
                            if (cp >= 0xD800 && cp <= 0xDBFF) {
                                // a high surrogate: a low one must follow to make one code point
                                const char*   back = _p;
                                std::uint32_t low = 0;
                                bool          paired = false;
                                if (_end - _p >= 6 && _p[0] == '\\' && _p[1] == 'u') {
                                    _p += 2;
                                    paired = Hex4(low) && low >= 0xDC00 && low <= 0xDFFF;
                                }
                                if (paired) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                } else {
                                    _p = back;
                                    cp = 0xFFFD;
                                }
                            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                                cp = 0xFFFD;
                            }
                            detail::AppendUtf8(a_out, cp);
                            break;
                        }
                    default:
                        return false;
                    }
                }
            }

            // -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?  - beyond the double range: +-infinity / +-0.
            bool ReadNumber(double& a_out) noexcept
            {
                const char* start = _p;
                if (*_p == '-') ++_p;
                if (_p == _end || !detail::IsDigit(*_p)) return false;
                if (*_p == '0') {
                    ++_p;
                } else {
                    while (_p < _end && detail::IsDigit(*_p)) ++_p;
                }
                if (_p < _end && *_p == '.') {
                    ++_p;
                    if (_p == _end || !detail::IsDigit(*_p)) return false;
                    while (_p < _end && detail::IsDigit(*_p)) ++_p;
                }
                if (_p < _end && (*_p == 'e' || *_p == 'E')) {
                    ++_p;
                    if (_p < _end && (*_p == '+' || *_p == '-')) ++_p;
                    if (_p == _end || !detail::IsDigit(*_p)) return false;
                    while (_p < _end && detail::IsDigit(*_p)) ++_p;
                }
                const std::string_view number(start, static_cast<std::size_t>(_p - start));
                double                 d = 0.0;
                const auto             r = std::from_chars(number.data(), number.data() + number.size(), d);
                if (r.ec == std::errc::result_out_of_range) d = detail::OutOfRange(number);
                else if (r.ec != std::errc{} || r.ptr != _p) return false;
                a_out = d;
                return true;
            }

            const char* _p;
            const char* _end;
        };
    }

    // ---------------------------------------------------------------- scene files
    struct SceneMeta
    {
        std::string        modpack;   // the scene file's "modpack" / "modPack" string (any key case), "" when absent
        std::vector<float> speeds;    // one per speed index: "displaySpeed", else "playbackSpeed", else NaN
    };

    namespace detail
    {
        // A string that starts with a number ("2x", "1.5", " 3 x", ".5x") -> that number; nullopt otherwise.
        inline std::optional<double> LeadingNumber(std::string_view a_s) noexcept
        {
            while (!a_s.empty() && IsBlank(a_s.front())) a_s.remove_prefix(1);
            if (!a_s.empty() && a_s.front() == '+') a_s.remove_prefix(1);
            const std::size_t at = !a_s.empty() && a_s.front() == '-' ? 1 : 0;
            const bool        starts = a_s.size() > at &&
                                (IsDigit(a_s[at]) || (a_s[at] == '.' && a_s.size() > at + 1 && IsDigit(a_s[at + 1])));
            if (!starts) return std::nullopt;
            double     d = 0.0;
            const auto r = std::from_chars(a_s.data(), a_s.data() + a_s.size(), d);
            if (r.ec == std::errc::result_out_of_range) return OutOfRange(a_s.substr(0, static_cast<std::size_t>(r.ptr - a_s.data())));
            if (r.ec != std::errc{}) return std::nullopt;
            return d;
        }

        inline std::optional<double> NumberOf(const json::Value& a_v) noexcept
        {
            if (a_v.kind == json::Value::Kind::kNumber) return a_v.number;
            if (a_v.kind == json::Value::Kind::kString) return LeadingNumber(a_v.text);
            return std::nullopt;
        }

        inline float ToFloat(double a_d) noexcept
        {
            constexpr double top = (std::numeric_limits<float>::max)();
            if (a_d > top) return std::numeric_limits<float>::infinity();
            if (a_d < -top) return -std::numeric_limits<float>::infinity();
            return static_cast<float>(a_d);   // NaN stays NaN
        }

        // One entry of "speeds": its displayed speed, else its playback speed, else NaN. A field that holds neither a
        // number nor a string starting with one counts as absent.
        inline float SpeedOf(const json::Value& a_speed) noexcept
        {
            for (const std::string_view key : { std::string_view("displaySpeed"), std::string_view("playbackSpeed") }) {
                if (const json::Value* v = a_speed.Member(key))
                    if (const auto n = NumberOf(*v)) return ToFloat(*n);
            }
            return std::numeric_limits<float>::quiet_NaN();
        }

        // "2.0x", "1.25x", "0.5x": four decimals at most, one at least (huge values in scientific notation).
        inline std::string FormatSpeed(double a_v)
        {
            if (std::fabs(a_v) < 0.00005) a_v = 0.0;   // no "-0.0x"
            char       buf[64];
            const bool fixed = std::fabs(a_v) < 1e9;
            const auto r = fixed ? std::to_chars(buf, buf + sizeof(buf), a_v, std::chars_format::fixed, 4) :
                                   std::to_chars(buf, buf + sizeof(buf), a_v, std::chars_format::scientific, 2);
            if (r.ec != std::errc{}) return "?x";
            std::string s(buf, r.ptr);
            if (fixed)
                while (s.size() >= 3 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
            s += 'x';
            return s;
        }
    }

    // The fields above from one OStim scene file (JSON). nullopt = not a JSON object (or not JSON at all).
    // displaySpeed / playbackSpeed may be numbers or strings that start with a number ("2x" -> 2). Only the root
    // object's "modpack" and "speeds" count (a nested object's fields of the same name do not); when a name repeats,
    // the last one counts. Never throws.
    inline std::optional<SceneMeta> ParseScene(std::string_view a_json)
    {
        try {
            json::Value root;
            if (!json::Reader(a_json).Document(root) || root.kind != json::Value::Kind::kObject) return std::nullopt;
            SceneMeta meta;
            for (std::size_t i = 0; i < root.keys.size(); ++i)
                if (root.items[i].kind == json::Value::Kind::kString && detail::EqualsNoCase(root.keys[i], "modpack"))
                    meta.modpack = root.items[i].text;
            if (const json::Value* speeds = root.Member("speeds"); speeds && speeds->kind == json::Value::Kind::kArray) {
                meta.speeds.reserve(speeds->items.size());
                for (const json::Value& speed : speeds->items) meta.speeds.push_back(detail::SpeedOf(speed));
            }
            return meta;
        } catch (...) {
            return std::nullopt;
        }
    }

    // The stage number at the end of a scene id: "someposition1-3" -> 3, "otherposition-12" -> 12;
    // 0 = none. Only a trailing "-<digits>" counts; a number too large for an int gives INT_MAX.
    inline int StageOf(std::string_view a_sceneId) noexcept
    {
        std::size_t i = a_sceneId.size();
        while (i > 0 && detail::IsDigit(a_sceneId[i - 1])) --i;
        if (i == a_sceneId.size() || i == 0 || a_sceneId[i - 1] != '-') return 0;
        long long n = 0;
        for (std::size_t k = i; k < a_sceneId.size(); ++k) {
            n = n * 10 + (a_sceneId[k] - '0');
            if (n > INT_MAX) return INT_MAX;
        }
        return static_cast<int>(n);
    }

    // A speed-based scene: two speeds or more that have a value.
    inline bool IsMultiSpeed(const SceneMeta& a_meta) noexcept
    {
        int finite = 0;
        for (const float v : a_meta.speeds)
            if (std::isfinite(v) && ++finite >= 2) return true;
        return false;
    }

    // Lower-case scene id -> meta. Ids compare ASCII case-insensitively, so a lookup never needs a lower-case copy.
    class SceneTable
    {
    public:
        // The id is stored in lower case. An id already present keeps the meta it was first added with: the first
        // file read wins.
        void Add(std::string a_idLower, SceneMeta a_meta)
        {
            detail::LowerInPlace(a_idLower);
            _scenes.try_emplace(std::move(a_idLower), std::move(a_meta));
        }

        // nullptr for an unknown id.
        const SceneMeta* Find(std::string_view a_idLower) const
        {
            const auto it = _scenes.find(a_idLower);
            return it != _scenes.end() ? &it->second : nullptr;
        }

        std::size_t Size() const noexcept { return _scenes.size(); }

        // a_fn(const std::string& id, const SceneMeta& meta) for every scene, in no particular order.
        template <class F>
        void ForEach(F&& a_fn) const
        {
            for (const auto& [id, meta] : _scenes) a_fn(id, meta);
        }

    private:
        struct Hash
        {
            using is_transparent = void;

            std::size_t operator()(std::string_view a_s) const noexcept
            {
                std::uint64_t h = 14695981039346656037ull;   // FNV-1a over the lower-case bytes
                for (const char c : a_s) {
                    h ^= static_cast<unsigned char>(detail::Lower(c));
                    h *= 1099511628211ull;
                }
                return static_cast<std::size_t>(h);
            }
        };

        struct Equal
        {
            using is_transparent = void;

            bool operator()(std::string_view a_a, std::string_view a_b) const noexcept { return detail::EqualsNoCase(a_a, a_b); }
        };

        std::unordered_map<std::string, SceneMeta, Hash, Equal> _scenes;
    };

    // ---------------------------------------------------------------- rules
    struct RuleConfig
    {
        bool                     enabled = true;         // false = every impact ripples
        bool                     outsideOStim = false;   // impacts while she is in no OStim (or followed SexLab) scene
        std::vector<std::string> excludeModpacks;        // packs that never ripple (lower case) - empty by default
        std::vector<std::string> excludePrefixes;        // scene id prefixes that never ripple (lower case) - empty by default
        // displayed speed: from minSpeed (included) up to maxSpeed (excluded)
        float minSpeed = 1.5f, maxSpeed = 2.5f;
        // stage: from minStage (included) up to maxStage (excluded)
        int minStage = 2, maxStage = 5;
    };

    struct Decision
    {
        bool        on = false;
        std::string why;   // short log text, e.g. "stage 3", "speed 2.5x - 2.5x and up off"
    };

    // The exclusion a scene falls under: the matching id prefix (trimmed, as configured) or the scene's pack name
    // (trimmed, as its file spells it); "" when none. Entries are trimmed and compared case-insensitively; blank
    // entries match nothing. Pack names must match whole, prefixes only the start of the id.
    inline std::string ExclusionOf(const RuleConfig& a_cfg, std::string_view a_sceneIdLower, const SceneMeta* a_meta)
    {
        for (const std::string& entry : a_cfg.excludePrefixes) {
            const std::string_view prefix = detail::Trim(entry);
            if (!prefix.empty() && detail::StartsWithNoCase(a_sceneIdLower, prefix)) return std::string(prefix);
        }
        if (a_meta) {
            const std::string_view pack = detail::Trim(a_meta->modpack);
            if (!pack.empty())
                for (const std::string& entry : a_cfg.excludeModpacks)
                    if (detail::EqualsNoCase(detail::Trim(entry), pack)) return std::string(pack);
        }
        return {};
    }

    // Whether an impact on a woman ripples. a_inScene: she is in an OStim scene; a_sceneIdLower: its current scene id;
    // a_meta: that scene's file (nullptr when unknown); a_speedIndex: the scene's current speed index (-1 unknown).
    // In this order:
    //  1. rules off -> on;  2. in no OStim scene -> outsideOStim;  3. excluded pack or id prefix -> off;
    //  4. speed-based scene -> on for minSpeed <= displayed speed < maxSpeed (an index past the last speed counts as
    //     the last; unknown index or a speed without a value -> off);
    //  5. a stage number at the end of the id -> on for minStage <= stage < maxStage;  6. else off.
    inline Decision Decide(const RuleConfig& a_cfg, bool a_inScene, std::string_view a_sceneIdLower, const SceneMeta* a_meta,
        int a_speedIndex)
    {
        constexpr double tolerance = 1e-4;   // a speed written as 2.4999 counts as 2.5
        if (!a_cfg.enabled) return { true, "rules off" };
        if (!a_inScene) return { a_cfg.outsideOStim, "not in an OStim scene" };
        if (std::string entry = ExclusionOf(a_cfg, a_sceneIdLower, a_meta); !entry.empty()) return { false, "excluded (" + entry + ")" };

        if (a_meta && IsMultiSpeed(*a_meta)) {
            if (a_speedIndex < 0) return { false, "speed not known yet" };
            const std::size_t index = (std::min)(static_cast<std::size_t>(a_speedIndex), a_meta->speeds.size() - 1);
            const float       v = a_meta->speeds[index];
            if (!std::isfinite(v)) return { false, "speed has no value" };
            std::string why = "speed " + detail::FormatSpeed(v);
            if (v < a_cfg.minSpeed - tolerance) return { false, why + " - below " + detail::FormatSpeed(a_cfg.minSpeed) };
            if (v >= a_cfg.maxSpeed - tolerance) return { false, why + " - " + detail::FormatSpeed(a_cfg.maxSpeed) + " and up off" };
            return { true, std::move(why) };
        }

        if (const int stage = StageOf(a_sceneIdLower); stage > 0) {
            std::string why = "stage " + std::to_string(stage);
            if (stage < a_cfg.minStage) return { false, why + " - before stage " + std::to_string(a_cfg.minStage) };
            if (stage >= a_cfg.maxStage) return { false, why + " - stage " + std::to_string(a_cfg.maxStage) + " and up off" };
            return { true, std::move(why) };
        }
        return { false, "one speed, no stage number" };
    }

    // The same for a SexLab scene (classic SexLab numbers its stages 1, 2 ...; SexLab P+'s stage is the number of stages
    // played so far): 1. rules off -> on;  2. stage not known yet (a_stage < 1) -> off;
    //  3. on for minStage <= stage < maxStage. The exclusion lists are OStim's (pack names and scene ids).
    inline Decision DecideSexLab(const RuleConfig& a_cfg, int a_stage)
    {
        if (!a_cfg.enabled) return { true, "rules off" };
        if (a_stage < 1) return { false, "stage not known yet" };
        std::string why = "stage " + std::to_string(a_stage);
        if (a_stage < a_cfg.minStage) return { false, why + " - before stage " + std::to_string(a_cfg.minStage) };
        if (a_stage >= a_cfg.maxStage) return { false, why + " - stage " + std::to_string(a_cfg.maxStage) + " and up off" };
        return { true, std::move(why) };
    }
}
