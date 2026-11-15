// The rain's drops running down a car (local change, pgr4 project), shared by the glass
// (car_reflect_hooks.hlsli) and the paint (car_paint_hooks.hlsli). The texture's drops stay where they
// are, and a few more are drawn here in their manner: first a splash alone where it lands, the drop
// growing out of it as it thins, then the drop running down with a short trail, each at its own
// time, place, size and speed. pgr4_camera.x: their clock (draw.cpp), whatever the car's speed.
// Surfaces close to the camera (the views from the car) have none.
#ifndef PGR4_DROPS_RUN_HLSLI
#define PGR4_DROPS_RUN_HLSLI
// By the race's weather (Tazz, 06/11/2026). Snow and light snow: the rain's texture on the car, half the
// flakes. Heavy snow (draw.cpp): the lying snow and all the flakes, without the rain's texture.
#define XeHeavySnow (pgr4_shelter.z > 0.5)
// Whether it snows (draw.cpp): snow.hlsli on the paint, and no running drops.
#define XeSnowing (frac(pgr4_camera.w * 0.5) > 0.25)
// The car the camera is on (draw.cpp): the running drops and the wiper are for it alone.
// Every car (Tazz, 04/11/2026): the car the camera is on was not told reliably (pgr4_camera.w > 1.5),
// so the drops and the snow did not show on it.
#define XeOurCar true
// How much frost covers this pixel (set when the drops are read): no reflection through it.
static float gXeFrostMask = 0.0;
float XeFrostPatch(float3 n);
#ifndef PGR4_TUNE_kDropsGlideAngle
#define PGR4_TUNE_kDropsGlideAngle 0.0
#define PGR4_TUNE_kDropsStretch 0.1
#define PGR4_TUNE_kDropsNear 0.7
#endif
static const float kDropsGlideAngle = PGR4_TUNE_kDropsGlideAngle;
static const float kDropsStretch = PGR4_TUNE_kDropsStretch;
static const float kDropsNear = PGR4_TUNE_kDropsNear;
// A drop shows the sky, white in the grey of a storm: from afar, less of the reflection where it lies (Tazz,
// 06/11/2026). gXeDropDim: set where the drops are read, used by XeEnvImproved, which comes after.
static const float kDropDim = 0.5;
static const float kDropDimPaint = 0.85;
static float gXeDropDim = 0.0;
// Seconds since the wiper's blade passed here (car_reflect_hooks.hlsli): an older drop is gone for good.
static float gXeDropsWiped = 1e6;
// How much the wind of the speed and the bends turn the running drops, 0 to 1.
static const float kDropsTurn = 0.25;
static const float kSplashCells = 16.0;  // a texture's width, about 1.5 m: cells of 9 cm
static const float kRunnerColumns = 22.0;  // a texture's width, about 1.5 m: columns of 7 cm
static const float kRunnerShare = 1.0;     // the cycles with a drop
static const float kRunnerSpan = 0.42;     // a cell's height, 63 cm
static const float kRunnerRun = 0.2;       // how far a drop slides, 30 cm
static const float kRunnerTrail = 0.05;    // its trail, 7 cm
// No sine: over the cells' numbers, it left runs of alike values, seen as lines (Hoskins' hash).
float XeDropsHash(float n) {
  n = frac(n * 0.1031);
  n *= n + 33.33;
  n *= n + n;
  return frac(n);
}
// A cell's number from its two coordinates, as the snow's (Hoskins' hash): a sum of the two gave the same
// number, so the same drop, to cells far apart.
float XeDropsCell(float2 at) {
  float3 q = frac(float3(at.x, at.y, at.x) * 0.1031);
  q += dot(q, q.yzx + 33.33);
  return floor(frac((q.x + q.y) * q.z) * 100000.0);
}
// uv: the coordinates the texture's drops are read at; to_eye: the vector from the surface to the eye;
// flat: under this lean (the sine of the slope) the drop lands and stays, gone without running: water
// does not run on a flat roof, and "down" means nothing there.
// How stretched the coordinates are here, as a drop would be (1: round; at the body's joins, much more):
// the ratio of how fast they run in the surface's two main directions. uv: the coordinates; to_eye:
// the vector from the surface to the eye, whose steps are the surface's.
float XeDropsStretchOf(float2 uv, float3 to_eye) {
  const float3 px = ddx(to_eye), py = ddy(to_eye);
  const float a = max(length(px), 1e-6);
  const float3 e1 = px / a;
  const float b = dot(py, e1);
  const float c = max(length(py - b * e1), 1e-6);
  const float2 u1 = ddx(uv) / a;
  const float2 u2 = (ddy(uv) - b * u1) / c;
  const float sum = dot(u1, u1) + dot(u2, u2);
  const float det = u1.x * u2.y - u1.y * u2.x;
  const float root = sqrt(max(sum * sum - 4.0 * det * det, 0.0));
  return sqrt((sum + root) / max(sum - root, 1e-12));
}
// Where the paint's coordinates are stretched, the drops are read at coordinates fixed on the car, which
// are never stretched: the surface's place in the frame of the piece drawn (draw.cpp, as its vertex shader
// has it, for any car: pgr4_view[2].xyz, its centre from the eye; the .w of the three rows, its rotation,
// a quaternion with w >= 0), seen
// along the car's axis the surface faces most. kDropsCarScale: the paint's own coordinates per metre.
static const float kDropsCarScale = 0.53;
struct XeDropsCarFrame {
  float2 faces[3];  // the coordinates along each of the car's axes
  float3 weights;   // how much each suits this surface; their sum is 1
  float2 best;      // the one that suits it most, for what cannot be blended
  float on_car;     // 0 when the draw has no frame (far away)
};
XeDropsCarFrame XeDropsCarFrameAt(float3 to_eye, float3 normal) {
  const float3 q = float3(pgr4_view[0].w, pgr4_view[1].w, pgr4_view[2].w);
  const float qw = sqrt(max(1.0 - dot(q, q), 0.0));
  const float3 x_axis = float3(1.0 - 2.0 * (q.y * q.y + q.z * q.z), 2.0 * (q.x * q.y - qw * q.z), 2.0 * (q.x * q.z + qw * q.y));
  const float3 z_axis = float3(2.0 * (q.x * q.z - qw * q.y), 2.0 * (q.y * q.z + qw * q.x), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
  const float3 y_axis = cross(z_axis, x_axis);
  const float3 from_car = -to_eye - pgr4_view[2].xyz;
  const float3 place = float3(dot(from_car, x_axis), dot(from_car, y_axis), dot(from_car, z_axis)) * kDropsCarScale;
  const float3 n = normalize(normal);
  float3 w = abs(float3(dot(n, x_axis), dot(n, y_axis), dot(n, z_axis)));
  const float3 w2 = w * w;
  w = w2 * w2 * w2 * w2 + 1e-5;
  XeDropsCarFrame f;
  f.faces[0] = place.zy;
  f.faces[1] = place.xz;
  f.faces[2] = place.xy;
  f.weights = w / (w.x + w.y + w.z);
  f.best = f.weights.x >= f.weights.y && f.weights.x >= f.weights.z ? f.faces[0]
           : f.weights.y >= f.weights.z                             ? f.faces[1]
                                                                    : f.faces[2];
  f.on_car = 1.0 - smoothstep(3.5, 4.5, length(from_car));
  return f;
}
// On the paint (flat > 0): how much the surface is a side of the car, facing outwards more than up. No
// splash there, only drops running.
float XeDropsOnSide(XeInput i, float flat) {
  return flat > 0.0 ? 1.0 - smoothstep(0.3, 0.55, abs(normalize(i.r1.xyz).y)) : 0.0;
}
float XeDropsDrop(XeInput i, float2 uv, float3 to_eye, float flat, float outside) {
  const float2 uv_x = ddx(uv), uv_y = ddy(uv);
  float2 way = abs(uv_y.x) > abs(uv_y.y) ? float2(sign(uv_y.x), 0.0) : float2(0.0, sign(uv_y.y));
  float tilt = 1.0;
  float2 lean_uv = float2(0.0, 0.0), down_uv = float2(0.0, 0.0), wind_uv = float2(0.0, 0.0);
  {
    const float3 step_x = -ddx(to_eye), step_y = -ddy(to_eye);
  const float xx = dot(step_x, step_x), xy = dot(step_x, step_y), yy = dot(step_y, step_y);
  const float det = xx * yy - xy * xy;
  if (det > 1e-14 * xx * yy + 1e-30) {
    const float a = (yy * -step_x.y - xy * -step_y.y) / det;
    const float b = (xx * -step_y.y - xy * -step_x.y) / det;
    const float2 along_uv = a * uv_x + b * uv_y;
    down_uv = along_uv;
    // In a bend (pgr4_view[0], draw.cpp): the same for the way they lean, as a step sideways.
    const float3 lean = pgr4_view[0].xyz * kDropsTurn;
    lean_uv = ((yy * dot(step_x, lean) - xy * dot(step_y, lean)) * uv_x +
               (xx * dot(step_y, lean) - xy * dot(step_x, lean)) * uv_y) / det;
    // The wind of the car's speed (pgr4_view[1]): towards the back of the car, as laid on the surface.
    const float3 wind = pgr4_view[1].xyz * kDropsTurn;
    wind_uv = ((yy * dot(step_x, wind) - xy * dot(step_y, wind)) * uv_x +
               (xx * dot(step_y, wind) - xy * dot(step_x, wind)) * uv_y) / det;
    if (dot(along_uv, along_uv) > 0.0) {
      const float turn_to = round(atan2(along_uv.y, along_uv.x) / 0.174533) * 0.174533;
      way = float2(cos(turn_to), sin(turn_to));
    }
    // How much the surface leans: the world's down laid on it, its length.
    const float3 on_surface = a * step_x + b * step_y;
    tilt = length(on_surface);
  }
  }
  float runs = flat > 0.0 ? smoothstep(flat, flat * 1.6, tilt) : 1.0;
  const float wind_force = length(pgr4_view[1].xyz) * kDropsTurn;
  // How far the wind carries them along the way down: against it on a windscreen (they climb), with it
  // on a rear window; 1 alone, without wind.
  // Measured against a slope of at least 0.35: on a gentle one the way down is short, and the least wind
  // or bend would swing a drop from one extreme to the other.
  const float down_length = max(length(down_uv) * max(0.35 / max(tilt, 1e-3), 1.0), 1e-5);
  float net = clamp(1.0 + dot(wind_uv, way) / down_length, -1.0, 1.0);
  // On the paint's gentle slopes a drop still slides a little as it fades, along the slope; on what is
  // truly flat there is no way to slide.
  const float creeps = flat > 0.0 ? 0.35 * smoothstep(0.02, 0.06, tilt) : 0.0;
  bool flat_part = runs <= 0.0 && creeps <= 0.0;
  if (flat_part) {
    // Flat: the wind alone moves them, once it blows; the same cells everywhere otherwise.
    if (wind_force > 0.05 && dot(wind_uv, wind_uv) > 0.0) {
      const float towards = round(atan2(wind_uv.y, wind_uv.x) / 0.174533) * 0.174533;
      way = float2(cos(towards), sin(towards));
      runs = saturate(wind_force);
      net = 1.0;
    } else {
      way = float2(0.0, 1.0);
    }
  }
  const float angle = kDropsGlideAngle * 0.0174533;
  way = float2(way.x * cos(angle) - way.y * sin(angle), way.x * sin(angle) + way.y * cos(angle));
  const float2 side = float2(-way.y, way.x);
  const float across = dot(uv, side) * kRunnerColumns;
  const float column = floor(across);
  const float along = dot(uv, way);
  // A brick wall of cells, each with its own drop at its own time: a drop appears, slides quickly,
  // is gone (the first quarter of the cycle), and the next one, later, is another drop elsewhere in
  // the cell, or none. So they are scattered, on no line.
  const float row_start = along + XeDropsHash(column) * kRunnerSpan;
  const float row = floor(row_start / kRunnerSpan);
  const float in_cell = row_start - row * kRunnerSpan;
  // In a bend the columns slant, so the drops run aslant in them; nothing changes on a straight.
  const float slant = runs > 0.0 && !flat_part
                          ? clamp((dot(lean_uv, side) + dot(wind_uv, side)) / down_length, -1.0, 1.0)
                          : 0.0;
  const float slanted = across - slant * in_cell * kRunnerColumns;
  const float cell = XeDropsCell(float2(floor(slanted), row));
  const float seed = XeDropsHash(cell);
  // Cycles of very different lengths (4 to 16 a clock's turn), and in each one the drop comes at any
  // moment: nothing falls in step. A drop lasts about the same whatever the cycle (1.2 to 2 s).
  const float cycles = floor(4.0 + 13.0 * XeDropsHash(cell + 1.0));
  const float cycle_time = pgr4_camera.x * cycles + seed * 37.0;
  const float phase = frac(cycle_time);
  const float key = cell + floor(cycle_time) * 17.0;
  // Shorter on what faces up on the paint (0.9 to 1.5 s, most of it fading), where it mostly stays; on
  // the car's sides it keeps its time, which sets how fast it runs.
  const float briefer = flat > 0.0 ? lerp(0.75, 1.0, XeDropsOnSide(i, flat)) : 1.0;
  const float lasts = min(0.021 * cycles * (0.8 + 1.87 * XeDropsHash(key + 10.0)) * briefer, 0.9);
  const float slide = (phase - XeDropsHash(key + 11.0) * (1.0 - lasts)) / lasts;
  if (slide < 0.0 || slide >= 1.0 || XeDropsHash(key + 9.0) > kRunnerShare) return 0.0;
  // Its age in seconds: the clock's rate as draw.cpp sets it (pgr4_native_drops_glide_idle and _rate, its
  // speed from pgr4_camera.z).
  const float age = slide * lasts / (cycles * (0.014 + 0.12 * pgr4_camera.z));
  if (age > gXeDropsWiped) return 0.0;
  // None lands while the game's own rain stops (a bridge, a tunnel; draw.cpp): those it had run on and go.
  if (age >= pgr4_shelter.y && age < pgr4_shelter.x) return 0.0;
  // Light rain: 75 % of the drops of heavy rain and storms (Tazz, 06/11/2026).
  if (pgr4_shelter.w > 0.5 && XeDropsHash(key + 12.0) > 0.75) return 0.0;
  // First the splash alone, where it landed. As the splash thins, the drop grows out of its middle,
  // and only then runs, gently at first and gathering speed.
  const float start = kRunnerTrail + XeDropsHash(key + 2.0) * (kRunnerSpan - kRunnerTrail - kRunnerRun);
  const float impact = saturate(slide / 0.24);
  const float on_side = XeDropsOnSide(i, flat);
  // On a side it is whole from the start, with no splash to grow out of.
  const float emerge = lerp(smoothstep(0.05, 0.17, slide), 1.0, on_side);
  const float running = saturate((slide - 0.15) / 0.85);
  // Each drop its own speed: in the same time, it runs a third to all of the way.
  const float head = start + abs(net) * max(runs, creeps) * pow(running, 1.6) * kRunnerRun * (0.33 + 0.67 * XeDropsHash(key + 12.0));
  // Carried back up by the wind, it runs the other way: the cell read upside down, its trail behind.
  const float in_run = net >= 0.0 ? in_cell : kRunnerSpan - in_cell;
  const float d = in_run - head;
  // Anywhere across its column but its edges: not in a line with those above and below.
  const float side_shift = (XeDropsHash(key + 4.0) - 0.5) * 0.7;
  const float x = (frac(slanted) - 0.5 - side_shift) / kRunnerColumns;  // in the texture's units, as d
  // Every drop its own: size, how long, how much it narrows above; an uneven outline.
  // Never under a few pixels: from afar a drop would fall between them and vanish.
  const float pixel = max(length(uv_x), length(uv_y));
  // Smaller on the paint (flat > 0), which is seen larger than a window from the usual view.
  const float size = min(max((0.003 + 0.0035 * XeDropsHash(key + 3.0) * XeDropsHash(key + 5.0) * 1.6) * (flat > 0.0 ? 0.55 : 1.0),
                             pixel * (flat > 0.0 ? 0.8 : 1.5)), 0.016);
  // Faster, a drop is drawn out and thinner, its trail longer (the clock, draw.cpp, makes it quicker
  // and more frequent). pgr4_camera.z: the car's speed, 0 to 1, about 225 km/h.
  const float fast = pgr4_camera.z;
  const float long_by = (1.05 + 0.65 * XeDropsHash(key + 6.0)) * (1.0 + 1.6 * fast);
  const float point_by = 0.15 + 0.6 * XeDropsHash(key + 7.0);
  const float height = size * long_by;
  const float above = d < 0.0 ? saturate(-d / (height * 1.8)) : 0.0;
  const float w = size * (1.0 - 0.3 * fast) * (1.0 - point_by * above * above);
  const float h = d < 0.0 ? height * (1.2 + point_by) : height;
  const float turn = atan2(x, d);
  const float uneven = 1.0 + 0.09 * sin(turn * 3.0 + seed * 40.0) + 0.06 * sin(turn * 5.0 + seed * 91.0);
  const float r = sqrt((x * x) / (w * w) + (d * d) / (h * h)) / uneven;
  // As transparent as the texture's, no bright outline: a soft body, a little stronger low down,
  // fading to nothing at its edge.
  const float lower = saturate(0.5 + 0.5 * d / height);
  // On what faces up on the paint, it fades over most of its life and shrinks as it goes, as a drop
  // spreading out; elsewhere over the end of its run, as it was.
  const float fades_over = flat > 0.0 ? lerp(0.6, 0.25, on_side) : 0.25;
  const float leaving = smoothstep(0.0, 1.0, saturate((1.0 - slide) / fades_over));
  const float shrinks = flat > 0.0 ? lerp(0.65 + 0.35 * leaving, 1.0, on_side) : 1.0;
  const float r_grown = r / max((0.4 + 0.6 * emerge) * shrinks, 0.01);
  const float bead = (1.0 - smoothstep(0.35, 1.0, r_grown)) * (0.16 + 0.24 * lower);
  // The trail: narrow, short, faint, broken here and there.
  const float back = d < 0.0 ? saturate(1.0 + d / (kRunnerTrail * (0.5 + XeDropsHash(key + 8.0)) * (1.0 + 2.0 * fast))) : 0.0;
  const float trail_w = size * (0.15 + 0.35 * back);
  const float broken = 0.55 + 0.45 * sin(along * 260.0 + seed * 70.0);
  const float trail = back * back * 0.2 * broken * smoothstep(trail_w, trail_w * 0.4, abs(x));
  // The splash where it landed: a ring growing and fading, and a few droplets thrown around it.
  const float2 from_impact = float2(x, in_run - start);
  const float distance = length(from_impact);
  const float ring_at = size * (1.0 + 1.9 * impact);
  const float ring = (1.0 - impact) * 0.42 * saturate(1.0 - abs(distance - ring_at) / (size * 0.32));
  float droplets = 0.0;
  for (int k = 0; k < 6; ++k) {
    const float towards = (k + XeDropsHash(key + 20.0 + k)) * 1.0472;
    const float2 spot = float2(cos(towards), sin(towards)) * size * (1.3 + 1.6 * impact * (0.6 + 0.4 * XeDropsHash(key + 30.0 + k)));
    const float small = size * (0.14 + 0.14 * XeDropsHash(key + 40.0 + k));
    droplets = max(droplets, saturate(1.0 - length(from_impact - spot) / small));
  }
  droplets *= saturate((1.0 - impact) * 1.6) * 0.5;
  // In quickly but softly when it lands, gone softly at the end of its run.
  const float life = leaving * saturate(slide / 0.05);
  const float grown = emerge;
  const float value = saturate(max(max(bead * grown, trail * saturate(running * 4.0) * runs), max(ring, droplets) * (1.0 - on_side)) * (1.0 + kDropsStretch)) * life * outside;
  // Where the wind just balances the slope, the drop is carried the other way from then on and its place
  // changes: unseen there.
  return value * smoothstep(0.0, 0.2, abs(net));
}
// Many small splashes with no drop, everywhere the rain lands: a cell of 9 cm each, at its own time,
// a burst for a seventh of a second. Here and there, not in step.
float XeDropsSplash(float2 uv, float flat) {
  if (pgr4_shelter.y == 0.0) return 0.0;  // no rain lands under a bridge, in a tunnel (draw.cpp)
  const float2 grid = uv * kSplashCells;
  const float row = floor(grid.y);
  const float2 shifted = float2(grid.x + XeDropsHash(row + 0.5), grid.y);
  const float2 place = floor(shifted);
  const float cell = XeDropsCell(place + 7.0);
  const float seed = XeDropsHash(cell);
  const float cycles = floor(3.0 + 11.0 * XeDropsHash(cell + 1.0));
  const float cycle_time = pgr4_camera.x * cycles + seed * 53.0;
  const float phase = frac(cycle_time);
  const float key = cell + floor(cycle_time) * 19.0;
  const float lasts = 0.002 * cycles * (0.8 + 0.5 * XeDropsHash(key + 2.0));
  const float start_at = XeDropsHash(key + 3.0) * (1.0 - lasts);
  const float t = (phase - start_at) / lasts;
  if (t < 0.0 || t >= 1.0) return 0.0;
  if (pgr4_shelter.w > 0.5 && XeDropsHash(key + 9.0) > 0.75) return 0.0;  // light rain: 75 % of them
  // Never under a few pixels: from afar it would fall between them and vanish.
  const float pixel = max(length(ddx(uv)), length(ddy(uv)));
  const float size = min(max((0.0016 + 0.0024 * XeDropsHash(key + 4.0) * XeDropsHash(key + 5.0)) * (flat > 0.0 ? 0.55 : 1.0),
                             pixel * (flat > 0.0 ? 0.7 : 1.3)), 0.012);
  // Its place in the cell, far enough from the edges for all of it to show.
  const float2 centre = 0.3 + 0.4 * float2(XeDropsHash(key + 6.0), XeDropsHash(key + 7.0));
  const float2 from = (frac(shifted) - centre) / kSplashCells;
  const float dist = length(from);
  // A burst and nothing else: no drop, no droplets. A ring broken into uneven arcs, opening and gone.
  const float ring_at = size * (0.7 + 2.0 * t);
  const float ring = saturate(1.0 - abs(dist - ring_at) / (size * 0.4));
  const float turn = atan2(from.y, from.x);
  const float arcs = 0.35 + 0.65 * abs(sin(turn * (2.0 + floor(3.0 * XeDropsHash(key + 8.0))) + seed * 40.0));
  return ring * arcs * 0.42 * (1.0 - t) * saturate(t / 0.08);
}
float XeDropsRunner(XeInput i, float2 uv, float3 to_eye, float flat) {
  if (!XeOurCar) return 0.0;
#ifdef XE_HAS_POS
  const float outside = smoothstep(kDropsNear, kDropsNear + 0.4, i.pos.w);
#else
  const float outside = 1.0;
#endif
  if (outside <= 0.0) return 0.0;
  // On the paint (flat > 0), none at the joins between the body's parts: where the coordinates jump or
  // are stretched (a texture's width is about 1.5 m, so some 0.7 a metre), and where the surface
  // bends sharply (the rounded edges of a panel: the normal turning fast over the metres).
  float clean = 1.0;
  if (flat > 0.0) {
    const float metres = sqrt(dot(ddx(to_eye), ddx(to_eye)) + dot(ddy(to_eye), ddy(to_eye))) + 1e-6;
    const float texture_per_metre = sqrt(dot(ddx(uv), ddx(uv)) + dot(ddy(uv), ddy(uv))) / metres;
    const float bend = sqrt(dot(ddx(i.r1.xyz), ddx(i.r1.xyz)) + dot(ddy(i.r1.xyz), ddy(i.r1.xyz))) / metres;
    clean = smoothstep(0.03, 0.08, texture_per_metre) * (1.0 - smoothstep(8.0, 16.0, texture_per_metre)) *
            (1.0 - smoothstep(1.5, 4.0, bend));
    if (clean <= 0.0) return 0.0;
  }
  const float splash = saturate(XeDropsSplash(uv, flat) * (1.0 + kDropsStretch)) * outside * (1.0 - XeDropsOnSide(i, flat));
  // About ten times the drops: two more readings of the cells, 4 and 5 times as many, so each drop some 2
  // times smaller; at their own sizes and turned, so their rows and columns fall in with no other's. Each
  // runs down all the same: the way is found from the coordinates given.
  const float drops = max(XeDropsDrop(i, uv, to_eye, flat, outside),
                          max(XeDropsDrop(i, float2(0.92 * uv.x - 0.39 * uv.y, 0.39 * uv.x + 0.92 * uv.y) * 2.0 + float2(0.37, 0.19), to_eye, flat, outside),
                              XeDropsDrop(i, float2(0.8 * uv.x + 0.6 * uv.y, -0.6 * uv.x + 0.8 * uv.y) * 2.24 + float2(0.61, 0.83), to_eye, flat, outside)));
  return max(drops, splash) * clean;
}
#endif
