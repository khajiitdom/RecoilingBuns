// Recoiling Buns: the settings file the plugin writes on the game's first start when there is none - the shipped ini's
// text after its first line (the plugin writes that line: its title and version). Generated from the ini; edit the ini.
#pragma once

#include <string_view>

namespace BR
{
    inline constexpr std::string_view kDefaultIni =
        R"INI(; A travelling ripple across a woman's buttocks on each thrust that lands, on top of her physics jiggle (CBPC / SMP).
; Distances are game units (1 unit is about 1.4 cm), times are seconds. Lists are comma separated and compared
; case-insensitively. The game picks up changes to this file while it runs (checked once a second).
; This file is written with the defaults on the game's first start with the mod, and no update of the mod ever replaces
; it: your settings stay. To go back to the defaults: Defaults in the settings tab, or delete this file.

[General]
; 1 = on, 0 = off (every mesh goes back to how the game set it up)
Enabled = 1
; 1 = also log every ripple, and while someone is close to her the measured distances / speeds (every 3 s) and the
; impact sound counts (every 5 s), and each step of a SexLab scene. The log is RecoilingBuns.log in SKSE's log folder.
Debug = 0
; Test: stand within 400 units of a woman (her bare skin in view) and press this key for one full-strength ripple - no
; scene needed, it ignores the rules. A quick check that the mod works with your body and setup. Playing a woman
; yourself? The nearest woman is then your own character: look at her in third person.
; DirectInput scan code: 0xC5 = Pause, 0 = off.
TestKey = 0xC5

[Rules]
; Which impacts ripple. 0 = every impact ripples.
Rules = 1
; Impacts while she is in no OStim or SexLab scene (neither installed, or a scene of another framework): 1 = they ripple.
; (The older name OutsideOStim is read too.)
OutsideScenes = 0
; SexLab's scenes (classic SexLab 1.6x and SexLab P+): 1 = followed - they ripple at the stages set by MinStage /
; MaxStage below; 0 = they count as outside a scene (OutsideScenes), and nothing of SexLab's is read. Switched on again
; while a scene runs, that scene is followed from its next stage on.
SexLab = 1
; OStim packs that never ripple: their names exactly as the "modpack" entry of their scene files spells them.
; Empty = no pack is excluded (the default). (OStim scenes only: SexLab's can't be excluded one by one - SexLab = 0
; with OutsideScenes = 0 stops the ripples in all of them.)
ExcludeModpacks =
; OStim scene id prefixes that never ripple: the start of the scene files' names (without .json) that a pack's scenes
; share. Empty = no scene is excluded (the default).
ExcludeScenePrefixes =
; Speed-based scenes (several speeds: OStim's own scenes and most animation packs made for OStim) ripple from
; MinSpeed (included) up to MaxSpeed (excluded), as OStim displays the speed (0.5x, 1x, 1.5x ...). Slower motion barely
; moves the body; from 2.5x on the wave turns into a flicker too quick to see.
MinSpeed = 1.5
MaxSpeed = 2.5
; Stage-based scenes - OStim packs converted from SexLab (one speed each, the stage at the end of the scene id,
; "<name>-3") and SexLab's own scenes - ripple from MinStage (included) up to MaxStage (excluded).
MinStage = 2
MaxStage = 5

[Ripple]
; Height of the wave crest at full strength
Amplitude = 0.8
; How fast the ring travels outward (units per second), and the distance between crests
WaveSpeed = 40
Wavelength = 4.5
; Length of the wave train (bigger = more rings)
PacketWidth = 3.0
; How quickly the wave dies down (seconds to fall to about a third), and the distance over which it thins out
Decay = 0.36
Spread = 7.0
; The short dent right at the point of impact: its depth, radius and how long it lasts
DentDepth = 0.7
DentRadius = 2.5
DentTime = 0.06
; How far the skin actually moves, as a multiple of the wave above (and on meshes with ordinary normal maps, how much
; their lighting follows it): bigger = a wiggle you can see from further away. The shading bands keep their own
; strength (ShadeGain / ShadeDarkest).
MotionScale = 2.0
; Size of the rippling area, relative to the distance between the two butt bones (up to 15)
RegionScale = 2.1
; Each impact's wave runs this long, and at most this many run at once on one mesh
MaxAge = 1.0
MaxEvents = 6

[Regions]
)INI"
        R"INI(; Optional regions besides the butt, on the same body (their bones must be in it: 3BA, BHUNP, UBE): the back of the
; thighs (on by default), the lower belly, the breasts. Each: Enabled (1 = on), Strength (multiplies the ripple's strength), Size (its
; radius, in distances between the two butt bones), Delay (seconds after the impact its wave starts), and multipliers
; on the [Ripple] wave: Speed, Wavelength, Decay, Dent. The butt takes the same settings but Enabled and Size (its
; size is RegionScale). Where two regions overlap they share the skin; the butt's own area never changes.
; AllAround = 1: every region a plain soft sphere around its centre - no cut by which way the skin faces, no gap at the
; butt's cleft, the two sides' waves one field across her middle, every piece of her body the same wave centres. With a
; big size (RegionScale up to 15; about 12 covers her body) the ripple runs over her body without seams, fading out at
; her neck, wrists and ankles (her head, hands and feet are meshes of their own). The butt then takes the skin the other
; regions would have. 0 = the regions as before 1.3.0 (cut to the skin facing away from her, with a gap at the cleft).
AllAround = 1
ButtStrength = 1.0
ButtDelay = 0
ButtSpeed = 1.0
ButtWavelength = 1.0
ButtDecay = 1.0
ButtDent = 1.0
ThighsEnabled = 1
ThighsStrength = 1.0
ThighsSize = 1.6
ThighsDelay = 0.03
ThighsSpeed = 1.0
ThighsWavelength = 1.0
ThighsDecay = 1.0
ThighsDent = 1.0
BellyEnabled = 0
BellyStrength = 0.4
BellySize = 1.3
BellyDelay = 0.05
BellySpeed = 1.0
BellyWavelength = 1.0
BellyDecay = 1.0
BellyDent = 0.5
BreastsEnabled = 0
BreastsStrength = 0.5
BreastsSize = 1.1
BreastsDelay = 0.08
BreastsSpeed = 1.0
BreastsWavelength = 1.0
BreastsDecay = 1.0
BreastsDent = 0

[Shading]
; Moving shading bands in the vertex colours, on skin and on meshes lit from a model-space normal map: those show the
; wave only this way and in their outline. 0 = outline only.
Shading = 1
; Skin with a model-space normal map but no vertex colours (most bodies: CBBE, 3BA, BHUNP) gets them while it ripples -
; only in the copy the game draws, never in the mesh itself, so other mods (RaceMenu overlays, body morphs) never see
; them. Skin with an ordinary (tangent space) normal map and no vertex colours shows the wave in tilted normals instead.
; 0 = only meshes whose shader already uses vertex colours get the bands.
AddColourLayer = 1
; How strongly the bands darken per unit of lost light, and the darkest a band may get (1 = no darkening)
ShadeGain = 2.2
ShadeDarkest = 0.55
; How much each channel (red, green, blue) follows the darkening: 1, 1, 1 = all alike (a plain shadow); a lower red gives a warmer one
ShadeTint = 1.0, 1.0, 1.0
; Tilt the normals of meshes with ordinary (tangent space) normal maps: the wave's shading on clothing, which gets no
; colour bands
TiltNormals = 1
; The light the bands are shaded with: fixed = above and behind her (above and in front for the belly and breasts);
; camera = above the side she is seen from, as high as the fixed light - from behind her the same light, from any other
; side the bands still read as shading
BandLight = fixed

[Impacts]
; What starts a ripple:
;   motion = his thrusts, from his body's movement alone (his genital base closing in on the middle of her buttocks
;            and stopping): a thrust that lands about as close to her as his other thrusts of the last few seconds -
;            wherever her body, his and the pose put the point where they meet - and within ContactDistance ripples
;            once, when it lands, so each wave has the time to cross her buttocks. Nothing else is needed.
;   sound  = optional, with Penetration Physics (PPA): its impact sounds (the files in ClapSounds) say that the two
;            collide, and the thrust that lands with one ripples; in a pose where no thrust can be seen, the sounds
;            ripple themselves after 2 s, one per 0.4 s at most. PPA's sound effects must be on.
)INI"
        R"INI(;   both   = a thrust ripples with a sound, and without one as with motion
; The sound hooks are installed at game start: switching to sound / both takes effect after a restart.
Trigger = motion
; Ripple strength of a thrust (0..1). (The older name SoundStrength is read too.)
Strength = 0.85
; Sound / both: PPA's impact sound files, relative to the Data folder (* = any text in the file name)
ClapSounds = Sound\Fx\ppa\vagina\clap*.wav, Sound\Fx\ppa\anus\plap*.wav
; His closing speed towards her buttocks (units per second): a thrust starts when he closes in faster than a quarter
; of this; with motion, only a thrust at least this fast ripples
MinApproachSpeed = 12
; Motion: a thrust ripples only when it ends at most this far from her (his genital base to the middle of her buttocks)
ContactDistance = 30
; Motion: at least this long between two ripples on her (a wave needs about 0.4 s to cross)
MinInterval = 0.3
; Who is watched: women within ScanDistance of you, partners within PartnerDistance of her
ScanDistance = 1500
PartnerDistance = 120
)INI";
}
