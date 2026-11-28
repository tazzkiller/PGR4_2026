// The game's side of the audio output probe (local change, pgr4 project, 26/11/2026, development
// only: audio_output_probe_dir). It reads the game and logs; every hooked function runs as it is,
// first, with its own arguments.
//
// What it follows, from the game's code (work/rea, 26/11):
// - The player's audio options, a byte each in the profile at 0x82C1E6B8 + profile * 0x22AA0 (the
//   profile in use at 0x82C88E50), written by the Lua SetOptionsVars (sub_825E2388): +0
//   DisplayType, +1 SoundEffectVolume, +2 MusicVolume, +3 LFEVolume, +4 MenuMusicVolume, +5
//   OpponentBalance, +6 EnvironmentBalance, +7 RoadNoiseBalance, +8 MusicOutputMode, +9
//   HeadphoneMode, +10 OutputMode, +11 RoomSize; the four speaker angles are floats at +12, +16,
//   +20, +24 (the speaker setup screen, gotham.gamelogic.speaker_setup).
// - The audio manager's update (the code around 0x8268C8F4, after sub_82676B88) hands them to the game's audio engine (0x82C89A4C, vtable
//   0x82222340): the volumes, HeadphoneMode and EnvironmentBalance through +0x68 (sub_827E3068 ->
//   sub_82808188, a parameter by handle), OpponentBalance per rival car the same way (sub_82693B40),
//   RoomSize (1.5 / 3 / 6 / 20), the angles and OutputMode (0 and 1: 2 channels, 2: 5) through
//   +0x98 (sub_827E27C8, which stores them at +4524..+4544, puts +4592 at 0.707 or 1.0 and +4596 at
//   0 or 1 by OutputMode, then works out the speakers' gains at +4560 for its +4520 speakers,
//   sub_827E2528), MusicOutputMode through +0xA0 (sub_827E31A8 -> sub_827FFBF0 -> the music voice's
//   XAudio parameter 4, sub_82913C48).
// - The frames the master effect played twice for want of a new one from the game's audio thread
//   (sub_8280E918 -> sub_827FFEE0: +116 of the output ring counts up).
#include <rex/audio/output_probe.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <fmt/format.h>

#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

REX_EXTERN(__imp__sub_827E27C8);
REX_EXTERN(__imp__sub_827E3068);
REX_EXTERN(__imp__sub_827FFBF0);

namespace {

constexpr uint32_t kOutputRing = 0x82C94D38;  // the game's output ring (the master effect's)
constexpr uint32_t kProfiles = 0x82C1E6B8;     // the options of profile 0, 0x22AA0 apart
constexpr uint32_t kProfileInUse = 0x82C88E50;

uint32_t GuestBits(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a + (a >= 0xE0000000u ? 0x1000u : 0u);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float GuestF(const uint8_t* base, uint32_t a) { return std::bit_cast<float>(GuestBits(base, a)); }
int8_t GuestS8(const uint8_t* base, uint32_t a) {
  return int8_t(base[a + (a >= 0xE0000000u ? 0x1000u : 0u)]);
}

std::mutex g_mutex;
std::string g_last_options;
std::map<uint32_t, float> g_params;  // the last value set by handle

std::string Options(const uint8_t* base) {
  const int32_t profile = int32_t(GuestBits(base, kProfileInUse));
  if (profile < 0 || profile > 8) return fmt::format("options: no profile ({})", profile);
  const uint32_t p = kProfiles + uint32_t(profile) * 0x22AA0;
  return fmt::format(
      "options of profile {}: SFX {} music {} LFE {} menu music {} opponents {} environment {} "
      "road noise {} music output {} headphones {} output {} room {}; speaker angles {:.3f} "
      "{:.3f} {:.3f} {:.3f}",
      profile, GuestS8(base, p + 1), GuestS8(base, p + 2), GuestS8(base, p + 3),
      GuestS8(base, p + 4), GuestS8(base, p + 5), GuestS8(base, p + 6), GuestS8(base, p + 7),
      GuestS8(base, p + 8), GuestS8(base, p + 9), GuestS8(base, p + 10), GuestS8(base, p + 11),
      GuestF(base, p + 12), GuestF(base, p + 16), GuestF(base, p + 20), GuestF(base, p + 24));
}

std::string ProbeExtra(const uint8_t* base) {
  std::string text;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    text = Options(base);
  }
  // An object at kOutputRing whose size (+32) is 8; else a pointer to one.
  uint32_t ring = kOutputRing;
  if (GuestBits(base, ring + 32) != 8) ring = GuestBits(base, kOutputRing);
  if (!ring || ring >= 0xF0000000u || GuestBits(base, ring + 32) != 8)
    return text + fmt::format("; game output ring not found at {:08X} FAULT: probe", kOutputRing);
  static uint32_t last = 0;
  static bool seen = false;
  const uint32_t replays = GuestBits(base, ring + 116);
  const uint32_t delta = seen ? replays - last : 0;
  last = replays;
  seen = true;
  return text + fmt::format("; game frames played twice {} (total {}){}", delta, replays,
                            delta ? " FAULT: the game's audio thread late" : "");
}

}  // namespace

// From the game's frame (sub_824C6FD8), once; and the options logged as they change.
void Pgr4AudioProbeAttach(const uint8_t* base) {
  static const bool attached = [base] {
    rex::audio::SetOutputProbeExtra([base] { return ProbeExtra(base); });
    return true;
  }();
  (void)attached;
  if (!rex::audio::OutputProbeEnabled()) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  std::string now = Options(base);
  if (now != g_last_options) {
    REXLOG_INFO("PGR4 audio {}", now);
    g_last_options = std::move(now);
  }
}

// The audio engine's speakers (+0x98): r4 OutputMode, r5 its channels, f1 the room, f2..f5 the
// angles; after it, what it made of them.
REX_HOOK_RAW(sub_827E27C8) {
  const uint32_t engine = ctx.r3.u32, mode = ctx.r4.u32, channels = ctx.r5.u32;
  const double room = ctx.f1.f64, a1 = ctx.f2.f64, a2 = ctx.f3.f64, a3 = ctx.f4.f64,
               a4 = ctx.f5.f64;
  __imp__sub_827E27C8(ctx, base);
  if (!rex::audio::OutputProbeEnabled()) return;
  const uint32_t speakers = GuestBits(base, engine + 4520);
  std::string gains;
  for (uint32_t i = 0; i < speakers && i < 8; ++i)
    gains += fmt::format(" {:.4f}", GuestF(base, engine + 4560 + 4 * i));
  const std::string line = fmt::format(
      "PGR4 audio speakers: output mode {} ({} channels), room {:.2f}, angles {:.4f} {:.4f} "
      "{:.4f} {:.4f}; engine +4592 {:.4f} +4596 {}, {} speakers, gains{}",
      mode, channels, room, a1, a2, a3, a4, GuestF(base, engine + 4592), GuestS8(base, engine + 4596),
      speakers, gains);
  static std::string last;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (line != last) {
    REXLOG_INFO("{}", line);
    last = line;
  }
}

// The audio engine's parameters by handle (+0x68): r4 the handle, f1 the value.
REX_HOOK_RAW(sub_827E3068) {
  const uint32_t handle = ctx.r4.u32;
  const float value = float(ctx.f1.f64);
  __imp__sub_827E3068(ctx, base);
  if (!rex::audio::OutputProbeEnabled()) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto [it, added] = g_params.try_emplace(handle, value);
  if (added || it->second != value) {
    REXLOG_INFO("PGR4 audio parameter {:08X} = {:.4f}{}", handle, value,
                added ? "" : fmt::format(" (was {:.4f})", it->second));
    it->second = value;
  }
}

// The music's output mode (+0xA0 -> here): r4 the mode, set on the music voice.
REX_HOOK_RAW(sub_827FFBF0) {
  const uint32_t mode = ctx.r4.u32;
  __imp__sub_827FFBF0(ctx, base);
  if (rex::audio::OutputProbeEnabled()) REXLOG_INFO("PGR4 audio music output mode {}", mode);
}
