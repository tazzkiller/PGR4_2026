// Development: a driver for tests (local addition, pgr4 project), so a test drives the same clean
// line every time. Off unless pgr4_autopilot_dir is set (never in a release's settings).
// It reads the cars from their vehicle records (+736 the pose: rows x, y, z, position; +880 the
// velocity) and answers the game's XamInputGetState for player 1 with an analog stick and triggers.
// Touching the real controller gives it back at once.
// Commands, a line each in <dir>\drive.txt (the file removed once read):
//   record <file>        every car's position and speed, 10 a second, into <dir>\<file>
//   recordstop
//   follow auto [speed share]    the game's own racing line for this race: <dir>\lines
//                                (tools/ai_lines.py, from ai.pak), at the race's start
//   follow <file> [speed share]  a line file (lines\<name>.txt, or tools/racing_line.py's)
//   hold <steer -1..1> <throttle 0..1> <brake 0..1> <seconds>
//   tap <a|b|x|y|start|back|up|down|left|right>             a button, for the menus
//   stop
// Its state, twice a second, in <dir>\drive_status.txt.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef near
#undef far

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_STRING(pgr4_autopilot_dir, "", "PGR4",
                      "Development: the test driver's folder (drive.txt, drive_status.txt); empty: off");
REXCVAR_DEFINE_DOUBLE(pgr4_autopilot_steer_gain, 2.2, "PGR4",
                      "Development: the test driver's stick for the angle to its target, per radian");
REXCVAR_DEFINE_DOUBLE(pgr4_autopilot_look, 0.55, "PGR4",
                      "Development: how far ahead it aims, seconds at its speed (plus 6 m)");
REXCVAR_DEFINE_DOUBLE(pgr4_autopilot_decel, 9.0, "PGR4",
                      "Development: the braking it counts on, m/s2");

REXCVAR_DEFINE_DOUBLE(pgr4_autopilot_side_gain, 0.05, "PGR4",
                      "Development: its stick per metre off the line");
extern float g_pgr4_camera_eye[3];  // pgr4-native draw.cpp: the road draws' eye
extern int g_pgr4_weather_kind;  // pgr4-native draw.cpp
extern const uint8_t* g_pgr4_guest_base;  // pgr4-native draw.cpp
extern bool g_pgr4_light_rain;
extern bool g_pgr4_race_loaded;  // pgr4-native draw.cpp

namespace {
using Clock = std::chrono::steady_clock;

float BeF(const uint8_t* p) {
  uint32_t v = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void PutBe16(uint8_t* p, int v) {
  p[0] = uint8_t(uint16_t(v) >> 8);
  p[1] = uint8_t(v);
}

struct LinePoint {
  float x, y, z, v;
};
enum class Mode { kOff, kHold, kFollow };

std::mutex g_mutex;
std::unordered_map<uint32_t, uint64_t> g_cars;  // record -> the last frame it was drawn in
uint64_t g_frame = 0;
Mode g_mode = Mode::kOff;
std::vector<LinePoint> g_line;
std::string g_line_name;
std::string g_route;  // the route the game loaded (sub_8264A358), lower case
float g_share = 0.85f;
size_t g_at = 0;
bool g_at_known = false;
bool g_closed = true;
uint32_t g_player = 0;
float g_hold[3] = {};
Clock::time_point g_hold_end, g_last_poll, g_last_status, g_last_record, g_stuck_since, g_last_step;
bool g_stuck = false;
Clock::time_point g_rescue_until;
uint16_t g_tap_buttons = 0;
int g_tap_frames = 0;
std::ofstream g_record;
Clock::time_point g_record_start;
float g_dbg[6] = {};
float g_status_steer = 0, g_status_throttle = 0, g_status_brake = 0, g_status_error = 0,
      g_status_target = 0;
int g_forward_row = 2;
float g_forward_sign = -1.0f;  // the pose's z axis points to the back of the car
uint32_t g_packet = 0x40000000;

std::filesystem::path Dir() { return std::filesystem::path(REXCVAR_GET(pgr4_autopilot_dir)); }

float Pos(const uint8_t* base, uint32_t record, int k) { return BeF(base + record + 736 + 48 + 4 * k); }
float Vel(const uint8_t* base, uint32_t record, int k) { return BeF(base + record + 880 + 4 * k); }
float Speed(const uint8_t* base, uint32_t record) {
  const float x = Vel(base, record, 0), y = Vel(base, record, 1), z = Vel(base, record, 2);
  return std::sqrt(x * x + y * y + z * z);
}
bool Alive(uint32_t record) {
  auto it = g_cars.find(record);
  return it != g_cars.end() && it->second + 30 >= g_frame;
}

// The car the camera is on, kept while it is drawn.
uint32_t Player(const uint8_t* base) {
  if (g_player && Alive(g_player)) return g_player;
  g_player = 0;
  float best = 30.0f * 30.0f;
  for (const auto& [record, frame] : g_cars) {
    if (frame + 30 < g_frame) continue;
    float d2 = 0;
    for (int k = 0; k < 3; ++k) {
      const float e = Pos(base, record, k) - g_pgr4_camera_eye[k];
      d2 += e * e;
    }
    if (d2 < best) best = d2, g_player = record;
  }
  if (g_player) REXLOG_INFO("PGR4 autopilot: car {:08X}", g_player);
  return g_player;
}

bool LoadLine(const std::string& name) {
  std::ifstream in(Dir() / name);
  std::vector<LinePoint> line;
  LinePoint p;
  while (in >> p.x >> p.y >> p.z >> p.v) line.push_back(p);
  if (line.size() < 20) {
    REXLOG_INFO("PGR4 autopilot: no line in {}", name);
    return false;
  }
  g_line = std::move(line);
  g_at_known = false;
  const float dx = g_line.front().x - g_line.back().x, dz = g_line.front().z - g_line.back().z;
  g_closed = dx * dx + dz * dz < 100.0f;
  return true;
}

// The route of this race: of the lines passing under the car at its height and going its way, the
// one whose start is nearest ahead or behind along it (a race starts on its grid, by the line's first
// point). tools/ai_lines.py's lines are every route of every city.
bool LoadLineAuto(const uint8_t* base) {
  const uint32_t car = Player(base);
  if (!car) {
    REXLOG_INFO("PGR4 autopilot: no car yet");
    return false;
  }
  const float px = Pos(base, car, 0), py = Pos(base, car, 1), pz = Pos(base, car, 2);
  const uint8_t* pose = base + car + 736;
  float fx = BeF(pose + 32), fz = BeF(pose + 40);  // the pose's z axis: forward
  const float flen = std::max(std::sqrt(fx * fx + fz * fz), 1e-3f);
  fx /= flen, fz /= flen;
  // The game told which route: its file's name, "<...>\<city><two digits>".
  if (!g_route.empty()) {
    std::string stem = g_route;
    const size_t slash = stem.find_last_of("\\/");
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    if (const size_t dot = stem.find('.'); dot != std::string::npos) stem.resize(dot);
    // The city, then the route from the first digit (st petersburg13_ac01: 13_ac01).
    if (const size_t digit = stem.find_first_of("0123456789"); digit != std::string::npos && digit > 0) {
      std::string city = stem.substr(0, digit), number = stem.substr(digit);
      std::replace(city.begin(), city.end(), ' ', '_');
      std::string best;
      float best_way = -2.0f;
      for (const char* way : {"clockwise", "anticlockwise"}) {
        const bool named = g_route.find("anticlockwise") != std::string::npos
                               ? std::string(way) == "anticlockwise"
                               : g_route.find("clockwise") != std::string::npos
                                     ? std::string(way) == "clockwise"
                                     : true;
        if (!named) continue;
        const std::string file = city + "_" + way + "_" + number;
        std::ifstream in(Dir() / "lines" / (file + ".txt"));
        std::vector<LinePoint> line;
        LinePoint q;
        while (in >> q.x >> q.y >> q.z >> q.v) line.push_back(q);
        if (line.size() < 20) continue;
        size_t at = 0;
        float near2 = 1e12f;
        for (size_t i = 0; i < line.size(); ++i) {
          const float dx = line[i].x - px, dz = line[i].z - pz;
          const float d = dx * dx + dz * dz;
          if (d < near2) near2 = d, at = i;
        }
        const LinePoint& to = line[(at + 5) % line.size()];
        const float tx = to.x - line[at].x, tz = to.z - line[at].z;
        const float w = (tx * fx + tz * fz) / std::max(std::sqrt(tx * tx + tz * tz), 1e-3f);
        REXLOG_INFO("PGR4 autopilot: route {}: {:.1f} m away, way {:.2f}", file, std::sqrt(near2), w);
        if (w > best_way) best_way = w, best = file;
      }
      if (!best.empty()) {
        g_line_name = best;
        return LoadLine("lines/" + best + ".txt");
      }
    }
    REXLOG_INFO("PGR4 autopilot: no line for the route {}", g_route);
  }
  std::ifstream index(Dir() / "lines" / "index.tsv");
  std::string name, hex, best_name;
  int closed, count;
  float best_score = 1e12f;
  while (index >> name >> closed >> hex >> count) {
    std::ifstream in(Dir() / "lines" / (name + ".txt"));
    std::vector<LinePoint> line;
    LinePoint q;
    while (in >> q.x >> q.y >> q.z >> q.v) line.push_back(q);
    if (line.size() < 20) continue;
    size_t at = 0;
    float near2 = 1e12f;
    for (size_t i = 0; i < line.size(); ++i) {
      const float dx = line[i].x - px, dz = line[i].z - pz;
      const float d = dx * dx + dz * dz;
      if (d < near2) near2 = d, at = i;
    }
    if (near2 > 25.0f * 25.0f || std::abs(line[at].y - py) > 6.0f) continue;
    const LinePoint& from = line[at];
    const LinePoint& to = line[(at + 5) % line.size()];
    const float tx = to.x - from.x, tz = to.z - from.z;
    const float tlen = std::max(std::sqrt(tx * tx + tz * tz), 1e-3f);
    const float way = (tx * fx + tz * fz) / tlen;
    if (way < 0.7f) continue;
    const float from_start = float(std::min(at, line.size() - at));
    // Metres from the start; the wet routes (puddles) only when nothing else fits.
    const float score = from_start + (name.find("puddles") != std::string::npos ? 5000.0f : 0.0f);
    REXLOG_INFO("PGR4 autopilot: line {} {:.1f} m away, {:.0f} m from its start, way {:.2f}", name,
                std::sqrt(near2), from_start, way);
    if (score < best_score) best_score = score, best_name = name;
  }
  if (best_name.empty()) {
    REXLOG_INFO("PGR4 autopilot: no line here going this way ({:.1f} {:.1f} {:.1f})", px, py, pz);
    return false;
  }
  g_line_name = best_name;
  return LoadLine("lines/" + best_name + ".txt");
}

void Command(const uint8_t* base, const std::string& text) {
  std::istringstream in(text);
  std::string word;
  in >> word;
  if (word == "record") {
    std::string name;
    in >> name;
    g_record.close();
    g_record.open(Dir() / name, std::ios::trunc);
    g_record << "t car x y z speed\n";
    g_record_start = Clock::now();
  } else if (word == "recordstop") {
    g_record.close();
  } else if (word == "follow") {
    std::string name;
    float share = 0.85f;
    in >> name >> share;
    if (name == "auto" ? LoadLineAuto(base) : LoadLine(name)) {
      if (name != "auto") g_line_name = name;
      g_share = share;
      g_mode = Mode::kFollow;
      g_stuck = false;
      g_rescue_until = Clock::time_point{};
      g_last_step = Clock::now();
    }
  } else if (word == "hold") {
    float seconds = 0;
    in >> g_hold[0] >> g_hold[1] >> g_hold[2] >> seconds;
    g_hold_end = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                    std::chrono::duration<float>(seconds));
    g_mode = Mode::kHold;
  } else if (word == "tap") {
    std::string b;
    in >> b;
    const std::unordered_map<std::string, uint16_t> buttons = {
        {"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008}, {"start", 0x0010},
        {"back", 0x0020}, {"a", 0x1000}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000}};
    auto it = buttons.find(b);
    if (it != buttons.end()) g_tap_buttons = it->second, g_tap_frames = 6;
  } else if (word == "stop") {
    g_mode = Mode::kOff;
  }
  REXLOG_INFO("PGR4 autopilot: {}", text);
}

void Poll(const uint8_t* base, Clock::time_point now) {
  if (now - g_last_poll < std::chrono::milliseconds(200)) return;
  g_last_poll = now;
  const auto path = Dir() / "drive.txt";
  std::ifstream in(path);
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) Command(base, line);
  }
  in.close();
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void Record(const uint8_t* base, Clock::time_point now) {
  if (!g_record.is_open() || now - g_last_record < std::chrono::milliseconds(100)) return;
  g_last_record = now;
  const float t = std::chrono::duration<float>(now - g_record_start).count();
  for (const auto& [record, frame] : g_cars) {
    if (frame + 30 < g_frame) continue;
    g_record << t << ' ' << std::hex << record << std::dec << ' ' << Pos(base, record, 0) << ' '
             << Pos(base, record, 1) << ' ' << Pos(base, record, 2) << ' ' << Speed(base, record)
             << '\n';
  }
  g_record.flush();
}

// Steering and pedals to follow the line. y is up (the world's).
void Follow(const uint8_t* base, uint32_t car, Clock::time_point now, float out[3]) {
  const float px = Pos(base, car, 0), pz = Pos(base, car, 2);
  const float vx = Vel(base, car, 0), vz = Vel(base, car, 2);
  const float speed = std::sqrt(vx * vx + vz * vz);
  const uint8_t* pose = base + car + 736;
  // Which of the pose's rows points forward: the one along the velocity, once it moves.
  if (g_forward_row < 0 && speed > 4.0f) {  // (kept for a car found otherwise)
    float best = 0.7f;
    for (int r = 0; r < 3; ++r) {
      const float d = (BeF(pose + 16 * r) * vx + BeF(pose + 16 * r + 8) * vz) / speed;
      if (std::abs(d) > best) best = std::abs(d), g_forward_row = r, g_forward_sign = d > 0 ? 1.0f : -1.0f;
    }
    if (g_forward_row >= 0)
      REXLOG_INFO("PGR4 autopilot: forward is row {} ({})", g_forward_row, g_forward_sign);
  }
  float fx = vx, fz = vz;
  if (g_forward_row >= 0) {
    fx = g_forward_sign * BeF(pose + 16 * g_forward_row);
    fz = g_forward_sign * BeF(pose + 16 * g_forward_row + 8);
  }
  const float flen = std::sqrt(fx * fx + fz * fz);
  if (flen > 1e-3f) fx /= flen, fz /= flen;

  // The nearest point: everywhere at first, then just ahead of the last one, around the loop.
  const size_t n = g_line.size();
  auto dist2 = [&](size_t i) {
    const float dx = g_line[i].x - px, dz = g_line[i].z - pz;
    return dx * dx + dz * dz;
  };
  size_t best_i = g_at;
  float best_d = dist2(best_i);
  const size_t span = g_at_known && best_d < 30.0f * 30.0f ? std::min<size_t>(n, 200) : n;
  for (size_t k = 0; k < span; ++k) {
    const size_t i = (g_at + k) % n;
    const float d = dist2(i);
    if (d < best_d) best_d = d, best_i = i;
  }
  g_at = best_i;
  g_at_known = true;
  const float error = std::sqrt(best_d);

  // The point to aim at, the speed from here to some way past it.
  const float look = 6.0f + float(REXCVAR_GET(pgr4_autopilot_look)) * speed;
  size_t aim = g_at;
  float along = 0;
  while (along < look && aim != (g_at + n - 1) % n) {
    if (!g_closed && aim + 1 >= n) break;
    const size_t next = (aim + 1) % n;
    const float dx = g_line[next].x - g_line[aim].x, dz = g_line[next].z - g_line[aim].z;
    along += std::sqrt(dx * dx + dz * dz);
    aim = next;
  }
  const float decel = float(REXCVAR_GET(pgr4_autopilot_decel));
  float target = g_line[g_at].v * g_share;
  float ahead = 0;
  for (size_t k = 0, i = g_at; k < n && ahead < 250.0f; ++k) {
    if (!g_closed && i + 1 >= n) break;
    const size_t next = (i + 1) % n;
    const float dx = g_line[next].x - g_line[i].x, dz = g_line[next].z - g_line[i].z;
    ahead += std::sqrt(dx * dx + dz * dz);
    const float v = g_line[next].v * g_share;
    target = std::min(target, std::sqrt(v * v + 2.0f * decel * ahead));
    i = next;
  }
  if (!g_closed && g_at + 30 >= n) target = 0.0f;
  // Off the line: slower, the more it is.
  if (error > 6.0f) target = std::min(target, std::max(12.0f, 45.0f - 2.0f * (error - 6.0f)));

  const float tx = g_line[aim].x - px, tz = g_line[aim].z - pz;
  const float tlen = std::max(std::sqrt(tx * tx + tz * tz), 0.1f);
  // Sign: x-z plane, y up: positive turns right of the heading.
  const float cross = (fz * tx - fx * tz) / tlen;
  const float dot = (fx * tx + fz * tz) / tlen;
  const float angle = std::atan2(cross, dot);
  const float gain = float(REXCVAR_GET(pgr4_autopilot_steer_gain));
  // Plus the side the car is off the line, which the aim alone leaves (it would hold a bend 5 m wide
  // of the line, against the barrier). Positive: the line is to the right.
  const size_t near_next = (g_at + 3) % n;
  const float lx = g_line[near_next].x - g_line[g_at].x, lz = g_line[near_next].z - g_line[g_at].z;
  const float ll = std::max(std::sqrt(lx * lx + lz * lz), 0.1f);
  const float off_x = g_line[g_at].x - px, off_z = g_line[g_at].z - pz;
  const float off_side = (lz * off_x - lx * off_z) / ll;
  const float steer = std::clamp(angle * gain + std::clamp(off_side * float(REXCVAR_GET(pgr4_autopilot_side_gain)), -0.4f, 0.4f), -1.0f, 1.0f);
  const float err = target - speed;
  float throttle = std::clamp(err * 0.35f, 0.0f, 1.0f);
  float brake = err < -1.0f ? std::clamp((-err - 1.0f) * 0.25f, 0.0f, 1.0f) : 0.0f;
  // Less throttle with much steering, as a driver would.
  throttle *= 1.0f - 0.6f * std::abs(steer);

  // Stuck against something: it stops and says so.
  if (speed < 2.0f && throttle > 0.3f) {
    if (!g_stuck) g_stuck = true, g_stuck_since = now;
    if (now - g_stuck_since > std::chrono::seconds(4)) {
      REXLOG_INFO("PGR4 autopilot: stuck, stopped");
      g_mode = Mode::kOff;
    }
  } else {
    g_stuck = false;
  }
  out[0] = steer, out[1] = throttle, out[2] = brake;
  g_dbg[0] = fx, g_dbg[1] = fz, g_dbg[2] = tx, g_dbg[3] = tz, g_dbg[4] = angle, g_dbg[5] = float(aim);
  g_status_error = error;
  g_status_target = target;
}

void Status(const uint8_t* base, uint32_t car, Clock::time_point now) {
  if (now - g_last_status < std::chrono::milliseconds(500)) return;
  g_last_status = now;
  const char* mode = g_mode == Mode::kFollow ? "follow" : g_mode == Mode::kHold ? "hold" : "off";
  std::ofstream out(Dir() / "drive_status.txt", std::ios::trunc);
  out << "mode " << mode << "\nroute " << g_route << "\nline " << g_line_name << "\ncar " << std::hex << car << std::dec << "\ncars " << g_cars.size();
  if (car) {
    out << "\npos " << Pos(base, car, 0) << ' ' << Pos(base, car, 1) << ' ' << Pos(base, car, 2)
        << "\nspeed_kmh " << Speed(base, car) * 3.6f;
  }
  out << "\neye " << g_pgr4_camera_eye[0] << ' ' << g_pgr4_camera_eye[1] << ' ' << g_pgr4_camera_eye[2]
      << "\nline_at " << g_at << '/' << g_line.size() << "\nline_error_m " << g_status_error
      << "\ntarget_kmh " << g_status_target * 3.6f << "\nsteer " << g_status_steer << "\nthrottle "
      << g_status_throttle << "\nbrake " << g_status_brake << "\nforward_row " << g_forward_row
      << "\ndbg_f " << g_dbg[0] << ' ' << g_dbg[1] << "\ndbg_t " << g_dbg[2] << ' ' << g_dbg[3] << "\ndbg_angle " << g_dbg[4] << "\ndbg_aim " << g_dbg[5] << "\nrecording " << (g_record.is_open() ? 1 : 0) << "\n";
}
}  // namespace

// Development: a race's weather forced, to see at will one of a career's (pgr4.toml, read at each race).
REXCVAR_DEFINE_INT32(pgr4_debug_lighting, -1, "PGR4", "Development: the race's LightingID forced (-1: the game's)");
REXCVAR_DEFINE_STRING(pgr4_debug_weather_name, "", "PGR4",
                      "Development: on any circuit, the first weather whose name holds this (Light Rain Dry)");
REXCVAR_DEFINE_INT32(pgr4_debug_weather_script, -1, "PGR4",
                     "Development: the race's WeatherScriptID forced (-1: the game's)");
// A race loading (sub_8264A358, in every build): its weather as the game has it, for the shaders
// (g_pgr4_weather_kind, pgr4-native draw.cpp) and the log. Its settings at 0x82B5E380 (sub_8252BFF8 writes them
// by name); 0x82B585D4 the table of cities (+0 the name, +4 the id, +0x14 the weather records, +0x18 how many);
// a record is 0x7C bytes (+0 the name, +0xC its kind: 0 clear, 1 overcast, 2 fog, 3 rain, 4 snow).
void Pgr4RaceWeather(const uint8_t* base) {
  auto readable = [&](uint32_t a) {
    MEMORY_BASIC_INFORMATION info;
    return a >= 0x10000 && a < 0xC0000000u && VirtualQuery(base + a, &info, sizeof(info)) &&
           info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
  };
  auto be32 = [&](uint32_t a) {
    return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
  };
  auto text_at = [&](uint32_t a) {
    std::string s;
    if (!readable(a)) return s;
    for (int i = 0; i < 80 && base[a + i] >= 32 && base[a + i] < 127; ++i) s += char(base[a + i]);
    return s;
  };
  g_pgr4_guest_base = base;
  const auto force = [base](uint32_t at, int32_t value) {
    if (value < 0) return;
    auto* b = const_cast<uint8_t*>(base) + at;
    b[0] = uint8_t(value >> 24); b[1] = uint8_t(value >> 16); b[2] = uint8_t(value >> 8); b[3] = uint8_t(value);
  };
  force(0x82B5E380 + 0xA20, REXCVAR_GET(pgr4_debug_lighting));
  force(0x82B5E380 + 0xA50, REXCVAR_GET(pgr4_debug_weather_script));
  g_pgr4_weather_kind = -1;
  g_pgr4_light_rain = false;
  constexpr uint32_t kRace = 0x82B5E380;
  const uint32_t city = be32(kRace + 8);
  uint32_t lighting = be32(kRace + 0xA20);
  std::string city_name = "?", weather_name = "?";
  const uint32_t cities = be32(0x82B585D4), city_count = be32(0x82B585DC);
  for (uint32_t c = 0; readable(cities) && c < city_count && c < 64; ++c) {
    const uint32_t entry = be32(cities + c * 4);
    if (!readable(entry) || be32(entry + 4) != city) continue;
    city_name = text_at(be32(entry));
    const uint32_t records = be32(entry + 0x14), record_count = be32(entry + 0x18);
    if (const std::string wanted = REXCVAR_GET(pgr4_debug_weather_name); !wanted.empty() && readable(records)) {
      for (uint32_t w = 0; w < record_count && w < 64; ++w)
        if (text_at(be32(records + w * 0x7C)).find(wanted) != std::string::npos) {
          lighting = w;
          force(kRace + 0xA20, int32_t(w));
          break;
        }
    }
    if (readable(records) && lighting < record_count) {
      const uint32_t record = records + lighting * 0x7C;
      weather_name = text_at(be32(record));
      g_pgr4_light_rain = weather_name.find("Light Rain") != std::string::npos;
      const uint32_t kind = be32(record + 0xC);
      if (kind <= 4)
        g_pgr4_weather_kind = kind != 4 ? int(kind) : weather_name == "Icy" ? 5 : weather_name == "Heavy Snow" ? 6 : 4;
    }
    break;
  }
  g_pgr4_race_loaded = g_pgr4_weather_kind >= 0;  // until a screen loads (src/pgr4_menu.cpp)
  REXLOG_INFO("PGR4 race: {} (CityID {}) route {} reverse {}, weather {} (kind {}, LightingID {}), Puddles {} "
              "RaceModeID {} WeatherScriptID {} TrackSurface {}",
              city_name, city, be32(kRace + 0xC), uint32_t(base[kRace + 0x10]), weather_name,
              g_pgr4_weather_kind, lighting, be32(kRace + 0xA24), be32(kRace + 0xA44),
              int32_t(be32(kRace + 0xA50)), be32(kRace + 0xA6C));
}

// The game loading a route's AI data (sub_8264A358: r4 a string, the route's file without .aid; it
// adds .aid, then .ain and the puddles' folder). An MSVC std::string: its text in place when short,
// else a pointer; which of the two is told by what is there.
void Pgr4AutopilotRouteString(const uint8_t* base, uint32_t string) {
  if (REXCVAR_GET(pgr4_autopilot_dir).empty() || string < 0x10000) return;
  // Only committed memory is read: what looks like a pointer may be the string's own text.
  auto readable = [&](uint32_t a) {
    MEMORY_BASIC_INFORMATION info;
    return a >= 0x10000 && a < 0xC0000000u && VirtualQuery(base + a, &info, sizeof(info)) &&
           info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
  };
  auto text_at = [&](uint32_t a) {
    std::string s;
    if (!readable(a)) return s;
    for (int i = 0; i < 120 && base[a + i] >= 32 && base[a + i] < 127; ++i) s += char(base[a + i]);
    return s;
  };
  auto be32 = [&](uint32_t a) {
    return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
  };
  if (!readable(string)) return;
  // The text itself (a C string, or a short std::string), else behind a pointer.
  std::string found = text_at(string);
  for (uint32_t off : {0u, 4u, 8u}) {
    if (found.size() >= 4) break;
    std::string s = text_at(string + off);
    if (s.size() < 4) s = text_at(be32(string + off));
    if (s.size() > found.size()) found = s;
  }
  std::string hex;
  for (int i = 0; i < 32; ++i) hex += fmt::format("{:02x}", base[string + i]);
  REXLOG_INFO("PGR4 autopilot: route string {} ({})", found, hex);
  if (found.size() >= 4) {
    std::transform(found.begin(), found.end(), found.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    std::lock_guard<std::mutex> lock(g_mutex);
    g_route = found;
  }
}

// The car the camera is on, for the free camera (src/pgr4_freecam.cpp).
bool Pgr4AutopilotCarPosition(const uint8_t* base, float out[3]) {
  if (REXCVAR_GET(pgr4_autopilot_dir).empty()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint32_t car = Player(base);
  if (!car) return false;
  for (int k = 0; k < 3; ++k) out[k] = Pos(base, car, k);
  return true;
}

// The cars drawn this frame (pgr4_hooks.cpp, sub_82417B50).
void Pgr4AutopilotCar(uint32_t record, uint64_t frame) {
  if (REXCVAR_GET(pgr4_autopilot_dir).empty()) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_cars[record] = frame;
  g_frame = std::max(g_frame, frame);
}

// After the game's XamInputGetState for user 0 (pgr4_profile.cpp): true if it answered.
bool Pgr4AutopilotInput(uint8_t* base, uint32_t state) {
  if (REXCVAR_GET(pgr4_autopilot_dir).empty()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto now = Clock::now();
  Poll(base, now);
  Record(base, now);
  const uint32_t car = Player(base);
  Status(base, car, now);

  if (g_tap_frames > 0) {
    --g_tap_frames;
    const uint16_t held = g_tap_frames > 2 ? g_tap_buttons : 0;
    std::memset(base + state + 4, 0, 12);
    PutBe16(base + state + 4, held);
    uint32_t packet = ++g_packet;
    for (int i = 0; i < 4; ++i) base[state + i] = uint8_t(packet >> (24 - 8 * i));
    return true;
  }
  if (g_mode == Mode::kOff) return false;

  // The real controller takes over.
  const uint8_t* s = base + state + 4;
  const int lx = int16_t(uint16_t(s[4]) << 8 | s[5]);
  if (s[0] || s[1] || s[2] > 30 || s[3] > 30 || std::abs(lx) > 9000) {
    REXLOG_INFO("PGR4 autopilot: controller touched, stopped");
    g_mode = Mode::kOff;
    return false;
  }
  float out[3] = {};
  if (g_mode == Mode::kHold) {
    if (now >= g_hold_end) {
      g_mode = Mode::kOff;
      return false;
    }
    std::memcpy(out, g_hold, sizeof(out));
  } else {
    if (!car) return false;
    Follow(base, car, now, out);
  }
  g_status_steer = out[0];
  g_status_throttle = out[1];
  g_status_brake = out[2];
  // X_INPUT_STATE, big-endian: packet, buttons, left trigger, right trigger, sticks.
  std::memset(base + state + 4, 0, 12);
  base[state + 6] = uint8_t(std::clamp(out[2], 0.0f, 1.0f) * 255.0f);
  base[state + 7] = uint8_t(std::clamp(out[1], 0.0f, 1.0f) * 255.0f);
  PutBe16(base + state + 8, int(std::clamp(out[0], -1.0f, 1.0f) * 32767.0f));
  uint32_t packet = ++g_packet;
  for (int i = 0; i < 4; ++i) base[state + i] = uint8_t(packet >> (24 - 8 * i));
  return true;
}
