// Hand-rolled test harness: no gtest on this box, no network to fetch it.
// Every test asserts a property a human-like bot must have, so a regression in
// "feels human" shows up as a red test instead of a vibe.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ogfn/ogfn_bots.h"

using namespace ogfn;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                          \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) {                                                \
      ++g_failures;                                               \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); \
    }                                                             \
  } while (0)

static bool contains(const std::vector<std::pair<std::string, float>>& v, const char* s) {
  for (const auto& e : v)
    if (e.first == s) return true;
  return false;
}

static void test_math() {
  std::printf("math\n");
  CHECK(wrap180(370.f) == 10.f, "wrap180 high");
  CHECK(wrap180(-190.f) == 170.f, "wrap180 low");
  CHECK(std::fabs(yaw_to_dir(0.f).x - 1.f) < 1e-5f, "yaw 0 is +x");
  CHECK(std::fabs(yaw_to_dir(90.f).y - 1.f) < 1e-5f, "yaw 90 is +y");
  CHECK(std::fabs(yaw_between({0, 0, 0}, {0, 5, 0}) - 90.f) < 1e-4f, "yaw_between");
}

static void test_perception_fov() {
  std::printf("perception: fov, occlusion, range, memory\n");
  Perception perc;
  Observation obs;
  obs.self_pos = {0.f, 0.f, 0.f};
  obs.self_yaw_deg = 0.f;  // looking +x

  EnemySample front;
  front.id = 1;
  front.pos = {20.f, 0.f, 0.f};
  front.line_of_sight = true;

  EnemySample behind;
  behind.id = 2;
  behind.pos = {-20.f, 0.f, 0.f};
  behind.line_of_sight = true;

  EnemySample occluded;
  occluded.id = 3;
  occluded.pos = {20.f, 2.f, 0.f};
  occluded.line_of_sight = false;

  EnemySample far_away;
  far_away.id = 4;
  far_away.pos = {500.f, 0.f, 0.f};
  far_away.line_of_sight = true;

  obs.enemies = {front, behind, occluded, far_away};
  perc.update(obs, 0.0);
  CHECK(perc.by_id(1) != nullptr && perc.by_id(1)->visible, "front visible");
  CHECK(perc.by_id(2) == nullptr, "behind not tracked at all");
  CHECK(perc.by_id(3) == nullptr, "occlusion has no wallhack");
  CHECK(perc.by_id(4) == nullptr, "range limit respected");

  obs.enemies.clear();
  perc.update(obs, 1.0);
  CHECK(perc.by_id(1) != nullptr, "lost target remembered for a few seconds");
  perc.update(obs, 10.0);
  CHECK(perc.by_id(1) == nullptr, "lost target forgotten after the memory window");
}

static void test_perception_lock_takes_time() {
  std::printf("perception: lock-on takes time\n");
  Perception perc;
  Observation obs;
  obs.self_pos = {0.f, 0.f, 0.f};
  EnemySample e;
  e.id = 7;
  e.pos = {10.f, 0.f, 0.f};
  e.line_of_sight = true;
  obs.enemies = {e};

  perc.update(obs, 0.0);
  CHECK(!perc.by_id(7)->fightable, "not fightable on the first frame");
  for (int i = 1; i <= 40; ++i) perc.update(obs, 0.1 * i);
  CHECK(perc.by_id(7)->fightable, "fightable after the lock window");
}

static WorldSnapshot peaceful_world() {
  WorldSnapshot w;
  w.time = 10.0;
  w.self_pos = {0.f, 0.f, 0.f};
  w.self_health = 100.f;
  w.self_shield = 100.f;
  w.has_weapon = true;
  w.circle_center_x = 0.f;
  w.circle_center_y = 0.f;
  w.distance_to_circle_edge = -50.f;
  w.loot_within_30m = 2;
  return w;
}

static void test_brain_zone_priority() {
  std::printf("brain: dying to storm outranks looting\n");
  Brain b(persona_camper(), 1);
  WorldSnapshot w = peaceful_world();
  w.distance_to_circle_edge = 40.f;  // outside the zone
  const Decision d = b.think(w);
  CHECK(d.action == ActionKind::RotateToCircle || contains(d.scores, "RotateToCircle"),
        "rotates when outside the zone");
}

static void test_brain_engage_vs_flee() {
  std::printf("brain: aggression and health change the fight decision\n");
  {
    Brain b(persona_w_keyer(), 2);
    WorldSnapshot w = peaceful_world();
    w.target_is_visible = true;
    w.target_distance = 20.f;
    const Decision d = b.think(w);
    CHECK(d.action == ActionKind::Engage || contains(d.scores, "Engage"),
          "w-keyer engages at full hp");
  }
  {
    Brain b(persona_camper(), 3);
    WorldSnapshot w = peaceful_world();
    w.target_is_visible = true;
    w.target_distance = 18.f;
    w.self_health = 15.f;
    w.self_shield = 0.f;
    const Decision d = b.think(w);
    CHECK(d.action == ActionKind::Flee || d.action == ActionKind::Build,
          "hurt camper disengages instead of trading");
  }
  {
    Brain b(persona_w_keyer(), 4);
    WorldSnapshot w = peaceful_world();
    w.target_is_visible = true;
    w.target_distance = 20.f;
    w.self_health = 20.f;
    w.self_shield = 0.f;
    const Decision d = b.think(w);
    CHECK(d.action == ActionKind::Engage || d.action == ActionKind::Build ||
              contains(d.scores, "Engage"),
          "a hurt w-keyer still wants the fight");
  }
}

static void test_brain_heal() {
  std::printf("brain: heals when hurt and stocked\n");
  Brain b(persona_camper(), 5);
  WorldSnapshot w = peaceful_world();
  w.self_shield = 0.f;
  w.shield_potions = 2;
  w.enemies_within_30m = 0;
  w.loot_within_30m = 0;
  const Decision d = b.think(w);
  CHECK(d.action == ActionKind::Heal, "heals when safe with shields down");
}

// The plan path used to return the raw argmax Decision, leaving move_to at the
// default (0,0,0). Committed ticks were fine, so live matches looked like an
// intermittent movement bug: bots walked at the map origin on every plan beat.
// Pin the intent on BOTH paths, not just the committed one.
static void test_plan_path_carries_the_intent() {
  std::printf("brain: plan ticks carry a real intent, not a default Decision\n");
  {
    Brain b(persona_rotator(), 21);
    WorldSnapshot w = peaceful_world();
    w.distance_to_circle_edge = 40.f;
    w.circle_next_center_x = 500.f;
    w.circle_next_center_y = 300.f;
    const Decision d = b.think(w);
    CHECK(d.reason == "plan", "first think() is a plan tick");
    CHECK(d.action == ActionKind::RotateToCircle, "rotates out of the storm");
    CHECK(d.move_to.x == 500.f && d.move_to.y == 300.f,
          "plan tick points the bot at the circle, not at the world origin");
  }
  {
    Brain b(persona_w_keyer(), 22);
    WorldSnapshot w = peaceful_world();
    w.target_is_visible = true;
    w.target_distance = 18.f;
    w.self_health = 100.f;
    const Decision d = b.think(w);
    CHECK(!(d.action == ActionKind::RotateToCircle && !d.fire && !d.build_wall &&
                d.move_to.x == 0.f && d.move_to.y == 0.f),
          "no plan tick returns an all-default intent");
  }
}

static void test_reaction_delay_blocks_instant_switch() {
  std::printf("brain: reaction latency prevents instant switching\n");
  Brain b(persona_camper(), 6);
  WorldSnapshot w = peaceful_world();
  w.target_is_visible = true;
  w.target_distance = 15.f;
  b.prime_action(ActionKind::RotateToCircle, w.time);
  const Decision d = b.think(w);
  CHECK(d.reason == "reaction delay", "a surprise contact is not reacted to instantly");
  CHECK(d.action == ActionKind::RotateToCircle, "the bot finishes what it was doing first");
  CHECK(b.is_reacting(w.time), "a reaction is pending");
}

static void test_motor_never_snap_aims() {
  std::printf("motor: aim is a servo, not a snap\n");
  Motor m(persona_w_keyer(), 7);
  const Vec3 self{0.f, 0.f, 0.f};
  const Vec3 target{50.f, 0.f, 0.f};
  float yaw = 0.f;
  float max_step = 0.f;
  for (int i = 0; i < 120; ++i) {
    const MotorOutput o = m.aim_at(self, yaw, target, true, 1.f / 60.f);
    max_step = std::max(max_step, std::fabs(wrap180(o.yaw_deg - yaw)));
    yaw = o.yaw_deg;
  }
  CHECK(max_step < 45.f, "no 180-degree snap in a single tick");
  CHECK(std::fabs(wrap180(yaw)) < 25.f, "eventually settles on the target");
}

static void test_motor_aim_has_standing_jitter() {
  std::printf("motor: aim jitters, it never sits perfectly still\n");
  Motor m(persona_camper(), 8);
  const Vec3 self{0.f, 0.f, 0.f};
  const Vec3 target{30.f, 0.f, 0.f};
  float yaw = 0.f;
  std::vector<float> samples;
  for (int i = 0; i < 300; ++i) {
    const MotorOutput o = m.aim_at(self, yaw, target, true, 1.f / 60.f);
    yaw = o.yaw_deg;
    if (i > 100) samples.push_back(o.yaw_deg);
  }
  float mean = 0.f;
  for (float s : samples) mean += s;
  mean /= static_cast<float>(samples.size());
  float var = 0.f;
  for (float s : samples) var += (s - mean) * (s - mean);
  var /= static_cast<float>(samples.size());
  CHECK(std::sqrt(var) > 0.02f, "non-zero standing jitter");
}

static void test_motor_fire_is_not_instant() {
  std::printf("motor: does not fire on the frame the target appears\n");
  Motor m(persona_camper(), 12);
  const Vec3 self{0.f, 0.f, 0.f};
  // Target 90 degrees off to the side: the first shots cannot be accurate.
  const Vec3 target{0.f, 40.f, 0.f};
  float yaw = 0.f;
  int first_fire_tick = -1;
  for (int i = 0; i < 240; ++i) {
    const MotorOutput o = m.aim_at(self, yaw, target, true, 1.f / 60.f);
    if (o.firing && first_fire_tick < 0) first_fire_tick = i;
    yaw = o.yaw_deg;
  }
  CHECK(first_fire_tick > 15, "no shot before the crosshair is anywhere near the target");
}

static void test_motor_build_cadence_scales_with_skill() {
  std::printf("motor: build cadence tracks skill\n");
  int slow_total = 0, fast_total = 0;
  {
    Persona p = persona_camper();
    p.build_speed = 0.05f;
    Motor m(p, 9);
    for (int i = 0; i < 600; ++i) slow_total += m.build_inputs_this_tick(1.f / 60.f);
  }
  {
    Persona p = persona_rotator();
    p.build_speed = 0.99f;
    Motor m(p, 10);
    for (int i = 0; i < 600; ++i) fast_total += m.build_inputs_this_tick(1.f / 60.f);
  }
  CHECK(fast_total > slow_total * 2, "a skilled builder places far more pieces");
}

static void test_motor_damage_flinch() {
  std::printf("motor: damage reaction has a flinch, never instant\n");
  Motor m(persona_w_keyer(), 11);
  CHECK(m.damage_reacts(100.0, 20.f), "the first hit can be reacted to");
  CHECK(!m.damage_reacts(100.0, 20.f), "the same instant cannot be reacted to twice");
  CHECK(!m.damage_reacts(100.05, 20.f), "no reaction 50ms later");
  CHECK(m.damage_reacts(101.0, 20.f), "reacts after the latency window");
}

static void test_motor_runs_dry_and_reloads() {
  std::printf("motor: a held trigger runs the magazine dry and reloads\n");
  Motor m(persona_w_keyer(), 41);
  m.set_weapon(WeaponProfile(30, 2.6f));
  const Vec3 self{0.f, 0.f, 0.f};
  const Vec3 target{6.f, 0.f, 0.f};  // point blank: duty cycle is 1

  int shots_before_reload = 0;
  bool saw_reload = false;
  int reload_ticks = 0;
  float yaw = 0.f;
  for (int i = 0; i < 60 * 20; ++i) {
    const MotorOutput o = m.aim_at(self, yaw, target, true, 1.f / 60.f);
    yaw = o.yaw_deg;
    if (!saw_reload) {
      if (o.firing) ++shots_before_reload;
      if (m.reloading()) {
        saw_reload = true;
        CHECK(shots_before_reload <= 30, "never fires more rounds than the magazine holds");
        CHECK(shots_before_reload >= 20, "does use most of the magazine before topping up");
      }
    } else {
      if (m.reloading()) ++reload_ticks;
    }
    CHECK(!(o.firing && m.reloading()), "cannot fire mid-reload");
  }
  CHECK(saw_reload, "a bot that holds the trigger eventually reloads");
  CHECK(m.reload_count() > 1, "keeps cycling magazines over a long fight");
  const float reload_secs = static_cast<float>(reload_ticks) / 60.f;
  CHECK(reload_secs > 1.0f, "reloads at a human speed, not instantly");
}

static void test_motor_tops_up_before_being_caught_dry() {
  std::printf("motor: reloads in the quiet instead of only when empty\n");
  Motor m(persona_camper(), 42);
  m.set_weapon(WeaponProfile(30, 2.6f));
  const Vec3 self{0.f, 0.f, 0.f};
  const Vec3 target{40.f, 0.f, 0.f};
  // Burn a third of the mag, then stop shooting for a few seconds.
  float yaw = 0.f;
  for (int i = 0; i < 60 * 12; ++i) {
    const MotorOutput o = m.aim_at(self, yaw, target, true, 1.f / 60.f);
    yaw = o.yaw_deg;
    if (m.rounds_left() < 15) break;
  }
  CHECK(m.rounds_left() < 30, "spent rounds on the first engagement");
  int reloads_before = m.reload_count();
  for (int i = 0; i < 60 * 8; ++i) {
    const MotorOutput o = m.aim_at(self, yaw, target, false, 1.f / 60.f);
    yaw = o.yaw_deg;
  }
  CHECK(m.reload_count() > reloads_before || m.rounds_left() > 15,
        "with nothing to shoot at, tops the magazine back up");
}

static void test_population_spread() {
  std::printf("population: skill spread with a heavy tail\n");
  Rng rng(1234);
  std::vector<float> latency;
  float min_aim = 2.f, max_aim = -1.f;
  for (int i = 0; i < 2000; ++i) {
    const Persona p = Persona::sample(rng, 0.f);
    latency.push_back(p.reaction_ms);
    min_aim = std::min(min_aim, p.aim_skill);
    max_aim = std::max(max_aim, p.aim_skill);
    CHECK(p.reaction_ms >= 90.f && p.reaction_ms <= 900.f, "latency within plausible range");
    CHECK(p.aim_skill > 0.f && p.aim_skill < 1.f, "aim skill bounded");
  }
  float mean = 0.f;
  for (float l : latency) mean += l;
  mean /= static_cast<float>(latency.size());
  float var = 0.f;
  for (float l : latency) var += (l - mean) * (l - mean);
  var /= static_cast<float>(latency.size());
  CHECK(std::sqrt(var) > 20.f, "latency spread is real, not a point mass");
  CHECK(max_aim - min_aim > 0.5f, "the lobby contains both bad and good players");
}

static void test_locomotion_strafes_in_a_fight_not_on_a_metronome() {
  std::printf("locomotion: strafes irregularly while fighting, walks straight otherwise\n");
  const Persona p = persona_rotator();
  {
    Locomotion loc(p, 31);
    MoveState st;
    int flips = 0;
    float prev = loc.strafe_sign();
    bool any_crouch = false, any_jump = false;
    for (int i = 0; i < 60 * 30; ++i) {
      // 30m: far enough that a human taps crouch between shots.
      const MoveOutput o = loc.update(1.f / 60.f, 0.f, 45.f, 30.f, true, i % 3 == 0, false,
                                      0.8f, st);
      if (o.crouching) any_crouch = true;
      if (o.jumping) any_jump = true;
      CHECK(o.strafing, "engaged bots strafe");
      CHECK(std::fabs(wrap180(o.move_yaw_deg - 45.f)) > 45.f,
            "strafe heading is perpendicular to the target, not straight at it");
      if (loc.strafe_sign() != prev) {
        ++flips;
        prev = loc.strafe_sign();
      }
    }
    // A 30s fight: a human reverses a few dozen times, never hundreds.
    CHECK(flips >= 8, "keeps changing direction in a long fight");
    CHECK(flips <= 90, "does not strobe between directions every few frames");
    CHECK(any_crouch, "sometimes crouches mid-fight at range");
    (void)any_jump;
  }
  {
    Locomotion loc(p, 32);
    MoveState st;
    for (int i = 0; i < 60 * 10; ++i) {
      const MoveOutput o = loc.update(1.f / 60.f, 120.f, 120.f, 500.f, false, false, false,
                                      1.f, st);
      CHECK(!o.strafing, "out of combat it walks a straight line, no dancing");
      CHECK(o.speed_scale > 0.5f, "rotating at full speed");
    }
  }
}

static void test_locomotion_never_frame_rate_crouch_spam() {
  std::printf("locomotion: crouch taps at a hand's rate, never at frame rate\n");
  Locomotion loc(persona_w_keyer(), 33);
  CHECK(loc.crouch_tap_period() >= 0.2f, "crouch taps are not a frame-rate flicker");

  MoveState st;
  st.has_cover = true;
  int transitions = 0;
  bool prev = false;
  for (int i = 0; i < 60 * 20; ++i) {
    const MoveOutput o = loc.update(1.f / 60.f, 0.f, 0.f, 40.f, true, i % 4 == 0, false, 0.9f, st);
    if (o.crouching != prev) {
      ++transitions;
      prev = o.crouching;
    }
  }
  // At a 0.24s floor a 20s window allows ~83 taps; anything near 1200 means the
  // bot is toggling crouch every frame, which reads as a stutter, not a player.
  CHECK(transitions <= 90, "crouch changes are rate limited to human tapping");
}

static void test_locomotion_braces_on_a_hard_fall() {
  std::printf("locomotion: cannot steer mid-fall, recovers on landing\n");
  Locomotion loc(persona_camper(), 34);
  MoveState st;
  st.airborne = true;
  st.vertical_speed = -25.f;
  const MoveOutput fall = loc.update(1.f / 60.f, 90.f, 90.f, 10.f, true, false, false, 1.f, st);
  CHECK(fall.bracing, "a fast fall sets the brace flag");
  CHECK(fall.speed_scale <= 0.2f, "cannot drift sideways at terminal velocity");
  CHECK(!fall.strafing, "no air strafing during a heavy fall");

  // Touch down and check the recovery beat: no jumping on the landing frame.
  MoveState ground;
  ground.airborne = false;
  ground.vertical_speed = -25.f;
  const MoveOutput land = loc.update(1.f / 60.f, 90.f, 90.f, 10.f, false, false, false, 1.f, ground);
  CHECK(!land.jumping, "never jumps on the landing frame");
  CHECK(land.speed_scale < 0.8f, "loses a beat on a hard landing");
}

static void test_locomotion_jump_cadence_is_human() {
  std::printf("locomotion: jumps on purpose, not on a beat\n");
  Locomotion loc(persona_w_keyer(), 35);
  MoveState st;
  int jumps = 0;
  for (int i = 0; i < 60 * 60; ++i) {
    // Low health and engaged: the one situation where a human bails out a lot.
    const MoveOutput o = loc.update(1.f / 60.f, 0.f, 0.f, 15.f, true, false, false, 0.2f, st);
    if (o.jumping) {
      ++jumps;
      // Simulate the hop so the airborne gate is exercised, not bypassed.
      st.airborne = true;
      st.vertical_speed = 5.f;
    } else if (st.airborne) {
      st.vertical_speed -= 30.f * (1.f / 60.f);
      if (st.vertical_speed < 0.f && i % 7 == 0) {
        st.airborne = false;
        st.vertical_speed = 0.f;
      }
    }
  }
  CHECK(jumps > 0, "a cornered bot does try to jump out");
  CHECK(jumps <= 70, "but not once a second for a whole minute");
}

static void test_building_boxes_up_under_fire() {
  std::printf("building: boxes up rather than placing one wall and hiding\n");
  Builder b(persona_w_keyer(), 51);
  int walls = 0, ramps = 0, floors = 0, cones = 0;
  std::vector<float> yaws;
  bool saw_edit = false;
  // Losing a trade, being shot at, with sky above: the box case.
  for (int i = 0; i < 60 * 8; ++i) {
    const BuildInput in = b.update(1.f / 60.f, true, true, 0.2f, 30.f, true);
    if (in.piece == PieceKind::Wall) ++walls;
    if (in.piece == PieceKind::Ramp) ++ramps;
    if (in.piece == PieceKind::Floor) ++floors;
    if (in.piece == PieceKind::Cone) ++cones;
    if (in.edit) saw_edit = true;
    if (in.piece != PieceKind::None) yaws.push_back(in.yaw_deg);
  }
  CHECK(walls >= 4, "a losing bot puts walls on more than one side");
  CHECK(floors >= 1, "and roofs itself, it does not fight in an open-top box");
  CHECK(cones == 0, "does not spam cones for no reason");
  CHECK(saw_edit, "uses the pieces it places: an edit happens, not just placement");

  // Four walls of a box must go around, not all in the same plane.
  int distinct = 0;
  for (size_t i = 0; i < yaws.size(); ++i) {
    bool novel = true;
    for (size_t j = 0; j < i; ++j)
      if (std::fabs(wrap180(yaws[i] - yaws[j])) < 10.f) novel = false;
    if (novel) ++distinct;
  }
  CHECK(distinct >= 2, "the pieces fan around the bot instead of stacking in one plane");
}

static void test_building_is_not_a_macro() {
  std::printf("building: input rate is a hand's, and it misses inputs\n");
  {
    Persona slow = persona_camper();
    slow.build_speed = 0.05f;
    Persona fast = persona_rotator();
    fast.build_speed = 0.99f;
    Builder bs(slow, 52), bf(fast, 53);
    CHECK(bf.input_hz() > bs.input_hz() * 2, "skill changes build speed");
    CHECK(bs.input_hz() >= 1.5f && bf.input_hz() <= 8.f, "input rate stays in the human band");

    int slow_n = 0, fast_n = 0;
    for (int i = 0; i < 60 * 10; ++i) {
      if (bs.update(1.f / 60.f, true, true, 0.4f, 0.f, true).piece != PieceKind::None) ++slow_n;
      if (bf.update(1.f / 60.f, true, true, 0.4f, 0.f, true).piece != PieceKind::None) ++fast_n;
    }
    const float slow_hz = static_cast<float>(slow_n) / 10.f;
    const float fast_hz = static_cast<float>(fast_n) / 10.f;
    CHECK(slow_hz < 4.f, "a slow builder is under 4 pieces/s, not turbo-building");
    CHECK(fast_hz > slow_hz, "a fast builder places more");
    CHECK(fast_hz < 9.f, "even the fastest bot does not place at frame rate");
    // Missing inputs: the achieved rate sits under the nominal rate.
    CHECK(fast_hz < bf.input_hz(), "inputs are occasionally dropped, like a hand");
  }
}

static void test_build_grid_places_pieces_where_a_player_would() {
  std::printf("build grid: pieces land on the cell edges, boxes close\n");
  BuildGrid g;
  const Vec3 self{100.f, 200.f, 0.f};
  const float yaw = 0.f;  // facing +x

  const BuildPiece w0 = g.place(PieceKind::Wall, self, yaw, 0);
  CHECK(w0.valid, "a wall is placeable");
  CHECK(std::fabs(w0.pos.x - (self.x + g.cell * 0.5f)) < 1e-3f, "first wall goes half a cell ahead");
  CHECK(std::fabs(w0.pos.y - self.y) < 1e-3f, "and stays on the player's line");
  CHECK(std::fabs(w0.pos.z - self.z) < 1e-3f, "at the player's own height");

  // Four walls of a box: each one must be half a cell from the player along a
  // different axis, so the box actually closes.
  std::vector<Vec3> corners;
  for (int step = 0; step < 4; ++step) {
    const BuildPiece w = g.place(PieceKind::Wall, self, yaw, step);
    CHECK(std::fabs(length(w.pos - self) - g.cell * 0.5f) < 1e-3f,
          "every box wall is the same distance from the player");
    corners.push_back(w.pos);
  }
  int distinct = 0;
  for (size_t i = 0; i < corners.size(); ++i) {
    bool novel = true;
    for (size_t j = 0; j < i; ++j)
      if (length(corners[i] - corners[j]) < 1e-2f) novel = false;
    if (novel) ++distinct;
  }
  CHECK(distinct == 4, "four walls occupy four different sides, not one plane");

  const BuildPiece roof = g.place(PieceKind::Floor, self, yaw, 0);
  CHECK(roof.valid && std::fabs(roof.pos.z - (self.z + g.wall_height)) < 1e-3f,
        "the roof sits one wall tier above the player");

  const BuildPiece ramp = g.place(PieceKind::Ramp, self, yaw, 0);
  CHECK(ramp.valid && std::fabs(ramp.pos.x - (self.x + g.cell)) < 1e-3f,
        "a ramp occupies the cell in front, not the player's own cell");
  CHECK(std::fabs(ramp.yaw_deg - yaw) < 1e-3f, "a ramp points where the player faces");

  CHECK(!g.place(PieceKind::None, self, yaw, 0).valid, "no piece for None");
}

static void test_building_uses_the_box_instead_of_rebuilding_it() {
  std::printf("building: fights from the box, does not rebuild it every second\n");
  Builder b(persona_w_keyer(), 57);
  int pieces = 0;
  for (int i = 0; i < 60 * 30; ++i) {
    const BuildInput in = b.update(1.f / 60.f, true, true, 0.2f, 30.f, true);
    if (in.piece != PieceKind::None) ++pieces;
  }
  // A 5-piece box in under a second, then 5 seconds of shooting out of the
  // window: 30s of sustained fire is about 30 pieces. Without a cooldown the same
  // bot placed 150, which is a macro.
  CHECK(pieces > 5, "actually builds under pressure");
  CHECK(pieces < 60, "does not rebuild the same box continuously (macro tell)");
}

static void test_building_goal_selection() {
  std::printf("building: threat and health pick the plan\n");
  {
    Builder b(persona_rotator(), 54);
    for (int i = 0; i < 120; ++i) b.update(1.f / 60.f, false, false, 1.f, 0.f, true);
    CHECK(b.total_placed() == 0, "nothing is built when nothing is happening");
    CHECK(b.goal() == BuildGoal::None, "no goal without a threat");
  }
  {
    Builder b(persona_rotator(), 55);
    bool saw_push = false;
    for (int i = 0; i < 60 * 6; ++i) {
      const BuildInput in = b.update(1.f / 60.f, true, false, 0.9f, 0.f, true);
      if (in.piece == PieceKind::Ramp) saw_push = true;
    }
    CHECK(saw_push, "a healthy bot pushes with ramps for height");
  }
  {
    // Indoors: nothing to ramp to, so it must not spam ramps at a ceiling.
    Builder b(persona_rotator(), 56);
    int ramps = 0;
    for (int i = 0; i < 60 * 6; ++i) {
      const BuildInput in = b.update(1.f / 60.f, true, false, 0.9f, 0.f, false);
      if (in.piece == PieceKind::Ramp) ++ramps;
    }
    CHECK(ramps == 0, "no ramp spam when there is no height to gain");
  }
}

static void test_loot_classification() {
  std::printf("loot: item names classify correctly\n");
  // The engine hands over asset paths, so these are real 3.5-era shapes.
  CHECK(classify_item("/Game/Athena/Items/Consumables/Shields/Athena_Shield_Potion") ==
            ItemClass::ShieldBig,
        "big shield potion");
  CHECK(classify_item("/Game/Athena/Items/Consumables/Shields/Athena_Small_Shield_Potion") ==
            ItemClass::ShieldPotion,
        "small shield is not confused with the big one (substring order matters)");
  CHECK(classify_item("/Game/Weapons/Shotgun/Athena_Shotgun_Pump") == ItemClass::Shotgun,
        "shotgun, not generic AR");
  CHECK(classify_item("/Game/Weapons/Sniper/Athena_Sniper_Bolt") == ItemClass::Sniper, "sniper");
  CHECK(classify_item("/Game/Weapons/Rifle/Athena_Assault_Rifle") == ItemClass::AssaultRifle,
        "assault rifle");
  CHECK(classify_item("Athena_Medkit") == ItemClass::Medkit, "medkit");
  CHECK(classify_item("Athena_Bandage") == ItemClass::Bandage, "bandage");
  CHECK(classify_item("Ammo_Medium") == ItemClass::Ammo, "ammo");
  CHECK(classify_item("Wood_Resource") == ItemClass::Materials, "materials");
  CHECK(classify_item("Something_Weird") == ItemClass::Unknown, "unknown stays unknown");
}

static void test_loot_valuation_prefers_what_you_lack() {
  std::printf("loot: worth depends on what the bot already has\n");
  LootValuer v(persona_rotator(), 61);
  const Vec3 self{0.f, 0.f, 0.f};

  Inventory empty;
  empty.shield = 0;
  Inventory loaded;
  loaded.shield = 100;
  loaded.big_shields = 3;

  const LootSample big{1, Vec3(10.f, 0.f, 0.f), ItemClass::ShieldBig, 1};
  const float w_empty = v.worth_of(LootSample{7, Vec3(10.f, 0.f, 0.f), ItemClass::ShieldBig, 1},
                                  empty, 10.f, 60.f, false);
  const float w_loaded = v.worth_of(LootSample{7, Vec3(10.f, 0.f, 0.f), ItemClass::ShieldBig, 1},
                                    loaded, 10.f, 60.f, false);
  CHECK(w_empty > w_loaded * 3.f, "a big pot means far more with no shield than at full shield");

  // Shields beat bandages when shields are down: that is the S3 economy.
  const float w_bandage = v.worth_of(LootSample{8, Vec3(10.f, 0.f, 0.f), ItemClass::Bandage, 5},
                                     empty, 10.f, 60.f, false);
  CHECK(w_empty > w_bandage, "shields outrank bandages when the shield bar is empty");

  // A second AR is not worth a walk when you already have one.
  Inventory armed;
  armed.weapon_tier = 3;
  const float w_ar_armed = v.worth_of(LootSample{9, Vec3(10.f, 0.f, 0.f), ItemClass::AssaultRifle, 1},
                                      armed, 10.f, 60.f, false);
  Inventory unarmed;
  const float w_ar_unarmed = v.worth_of(LootSample{9, Vec3(10.f, 0.f, 0.f), ItemClass::AssaultRifle, 1},
                                        unarmed, 10.f, 60.f, false);
  CHECK(w_ar_unarmed > w_ar_armed * 3.f, "a gun matters when you have none, not when you do");
  (void)big;
  (void)self;
}

static void test_loot_distance_and_clock() {
  std::printf("loot: distance and the closing circle both cost worth\n");
  LootValuer v(persona_rotator(), 62);
  Inventory inv;
  inv.shield = 0;
  const LootSample pot{5, Vec3(0.f, 0.f, 0.f), ItemClass::ShieldBig, 1};

  const float near = v.worth_of(pot, inv, 3.f, 60.f, false);
  const float mid = v.worth_of(pot, inv, 15.f, 60.f, false);
  const float far = v.worth_of(pot, inv, 40.f, 60.f, false);
  CHECK(near > mid && mid > far, "further loot is worth less");
  CHECK(far * 4.f < near, "the cost curve is steep, not linear (no vacuuming the map)");

  const float lots_of_time = v.worth_of(pot, inv, 10.f, 60.f, false);
  const float no_time = v.worth_of(pot, inv, 10.f, 4.f, false);
  CHECK(lots_of_time > no_time * 2.f, "with the circle closing, the same pot is barely worth it");

  const float calm = v.worth_of(pot, inv, 10.f, 60.f, false);
  const float fighting = v.worth_of(pot, inv, 10.f, 60.f, true);
  CHECK(calm > fighting * 3.f, "looting mid-fight is nearly always wrong");
}

static void test_loot_choice_picks_the_best_not_the_nearest() {
  std::printf("loot: walks to what is worth it, not to the closest thing\n");
  LootValuer v(persona_rotator(), 63);
  Inventory inv;
  inv.shield = 0;
  const std::vector<LootSample> samples = {
      {1, Vec3(3.f, 0.f, 0.f), ItemClass::Ammo, 30},
      {2, Vec3(8.f, 0.f, 0.f), ItemClass::ShieldBig, 1},
      {3, Vec3(5.f, 0.f, 0.f), ItemClass::Bandage, 5},
  };
  const LootChoice c = v.choose(samples, inv, Vec3(0.f, 0.f, 0.f), 60.f, false);
  CHECK(c.want, "there is something worth taking");
  CHECK(c.pickup_id == 2, "the big pot at 8m beats the ammo at 3m");
  CHECK(c.cls == ItemClass::ShieldBig, "and the choice reports what it is");

  // With everything topped off, clutter is not worth a detour.
  Inventory full;
  full.shield = 100;
  full.big_shields = 3;
  full.small_shields = 5;
  full.weapon_tier = 4;
  full.ammo = 500;
  full.medkits = 3;
  full.bandages = 10;
  const std::vector<LootSample> junk = {
      {4, Vec3(6.f, 0.f, 0.f), ItemClass::Ammo, 10},
      {5, Vec3(9.f, 0.f, 0.f), ItemClass::Bandage, 5},
  };
  const LootChoice none = v.choose(junk, full, Vec3(0.f, 0.f, 0.f), 60.f, false);
  CHECK(!none.want, "a fully kitted bot does not detour for clutter");
}

static void test_loot_preferences_differ_between_bots() {
  std::printf("loot: two bots with the same inventory still disagree\n");
  // Greedy vs cautious on the same floor.
  Persona greedy = persona_w_keyer();
  greedy.greed = 1.f;
  greedy.caution = 0.f;
  Persona careful = persona_camper();
  careful.greed = 0.05f;
  careful.caution = 1.f;
  LootValuer vg(greedy, 64), vc(careful, 65);
  Inventory inv;
  inv.shield = 20;
  const LootSample far_pot{6, Vec3(38.f, 0.f, 0.f), ItemClass::ShieldBig, 1};
  const float wg = vg.worth_of(far_pot, inv, 38.f, 60.f, false);
  const float wc = vc.worth_of(far_pot, inv, 38.f, 60.f, false);
  CHECK(wg > wc, "the greedy bot values the same far pot more than the cautious one");

  // A builder values materials far above a bot that never builds.
  Persona builder = persona_rotator();
  builder.build_speed = 1.f;
  Persona nonbuilder = persona_camper();
  nonbuilder.build_speed = 0.05f;
  LootValuer vb(builder, 66), vn(nonbuilder, 67);
  const LootSample mats{7, Vec3(10.f, 0.f, 0.f), ItemClass::Materials, 100};
  CHECK(vb.worth_of(mats, Inventory{}, 10.f, 60.f, false) >
            vn.worth_of(mats, Inventory{}, 10.f, 60.f, false),
        "a fast builder cares about materials more than a camper does");
}

static void test_brain_loot_intent_has_a_destination() {
  std::printf("brain: Loot and ThirdParty produce a real destination\n");
  {
    Brain b(persona_rotator(), 68);
    WorldSnapshot w = peaceful_world();
    w.circle_next_center_x = 0.f;
    w.circle_next_center_y = 0.f;
    w.loot_within_30m = 2;
    w.inventory.shield = 0;
    w.loot = {{11, Vec3(8.f, 0.f, 0.f), ItemClass::ShieldBig, 1},
              {12, Vec3(2.f, 0.f, 0.f), ItemClass::Ammo, 30}};
    b.prime_action(ActionKind::Loot, w.time);
    const Decision d = b.think(w);
    CHECK(d.action == ActionKind::Loot || contains(d.scores, "Loot"), "looting is on the table");
    if (d.action == ActionKind::Loot) {
      CHECK(d.move_to.x > 4.f, "walks to the big pot, not the ammo at 2m");
      CHECK(b.loot_choice().want, "and reports a real choice");
    }
  }
  {
    // ThirdParty used to fall through to `default` and return (0,0,0), so the
    // action did literally nothing. It must now walk at the fight.
    Brain b(persona_third_party(), 69);
    WorldSnapshot w = peaceful_world();
    w.enemies_currently_fighting_each_other = 2;
    w.fight_x = 120.f;
    w.fight_y = -40.f;
    b.prime_action(ActionKind::ThirdParty, w.time);
    const Decision d = b.think(w);
    if (d.action == ActionKind::ThirdParty)
      CHECK(d.move_to.x == 120.f && d.move_to.y == -40.f,
            "third-partying walks at the fight, not the centre of the map");
  }
  {
    // HoldAngle legitimately means "stay here", so its move_to is self, never the
    // origin, which is what a missing case would have produced.
    Brain b(persona_camper(), 70);
    WorldSnapshot w = peaceful_world();
    w.self_pos = Vec3(700.f, -250.f, 0.f);
    w.enemies_within_30m = 2;
    b.prime_action(ActionKind::HoldAngle, w.time);
    const Decision d = b.think(w);
    if (d.action == ActionKind::HoldAngle)
      CHECK(d.move_to.x == 700.f && d.move_to.y == -250.f,
            "holding an angle means holding position, not walking to the origin");
  }
}

static void test_perception_ignores_teammates() {
  std::printf("squad: a teammate is never a target\n");
  Perception perc;
  Observation obs;
  obs.self_pos = {0.f, 0.f, 0.f};
  obs.self_yaw_deg = 0.f;
  EnemySample mate;
  mate.id = 1;
  mate.pos = {8.f, 0.f, 0.f};   // right in front, in the FOV, in range
  mate.line_of_sight = true;
  mate.is_teammate = true;
  EnemySample enemy;
  enemy.id = 2;
  enemy.pos = {20.f, 0.f, 0.f};
  enemy.line_of_sight = true;
  obs.enemies = {mate, enemy};
  perc.update(obs, 0.0);
  CHECK(perc.by_id(1) == nullptr, "a teammate in front of the muzzle is not tracked");
  CHECK(perc.nearest(true) != nullptr && perc.nearest(true)->id == 2,
        "the nearest TARGET is the enemy, not the squadmate standing closer");
  CHECK(perc.targets().size() == 1, "teammates never enter the target list at all");
}

static void test_squad_focus_fire() {
  std::printf("squad: everyone shoots the same target\n");
  SquadBrain a(71), b(72);
  const Vec3 self{0.f, 0.f, 0.f};
  {
    // A teammate is already shooting target 5, which is also visible to me.
    Teammate mate;
    mate.id = 2;
    mate.pos = Vec3(5.f, 0.f, 0.f);
    mate.health = 200.f;
    mate.target_id = 5;
    const SquadDirective d = a.update({mate}, {3, 5}, self, 1.f, true, false, 1000);
    CHECK(d.focus_target_id == 5, "joins the target a squadmate is already shooting");
  }
  {
    // Nobody calling: two bots given the same enemy list must converge anyway, or a
    // squad splits its damage and loses every even fight.
    const SquadDirective da = a.update({}, {7, 2, 9}, self, 1.f, true, false, 2000);
    const SquadDirective db = b.update({}, {9, 2, 7}, self, 1.f, true, false, 2000);
    CHECK(da.focus_target_id == db.focus_target_id, "uncalled targets still converge");
    CHECK(da.focus_target_id == 2, "on the lowest id, deterministically");
  }
  {
    // A called target that I cannot see is not my focus: I would walk at a wall.
    Teammate mate;
    mate.id = 2;
    mate.pos = Vec3(5.f, 0.f, 0.f);
    mate.target_id = 42;   // not in my visible list
    const SquadDirective d = a.update({mate}, {3}, self, 1.f, true, false, 3000);
    CHECK(d.focus_target_id == 3, "a call for something out of my view falls back to what I can see");
  }
}

static void test_squad_revives_before_looting() {
  std::printf("squad: a downed squadmate outranks the plan\n");
  SquadBrain sb(73);
  const Vec3 self{0.f, 0.f, 0.f};
  Teammate body;
  body.id = 4;
  body.pos = Vec3(6.f, 0.f, 0.f);
  body.downed = true;

  const SquadDirective not_in_fight = sb.update({body}, {}, self, 0.9f, false, false, 1000);
  CHECK(not_in_fight.should_revive, "goes for the body when not fighting");
  CHECK(not_in_fight.revive_id == 4, "and names which body");
  CHECK(not_in_fight.focus_target_id == 0, "does not call a target while reviving");

  // Already being revived by someone else: do not stack on the body.
  Teammate taken = body;
  taken.revive_in_progress = true;
  const SquadDirective taken_d = sb.update({taken}, {}, self, 0.9f, false, false, 1100);
  CHECK(!taken_d.should_revive, "leaves a revive that someone else is already doing");

  // A body 60m away is not this bot's job.
  Teammate far_body = body;
  far_body.pos = Vec3(60.f, 0.f, 0.f);
  const SquadDirective far_d = sb.update({far_body}, {}, self, 0.9f, false, false, 1200);
  CHECK(!far_d.should_revive, "does not cross the map for a body that is not nearby");

  // Push comes to shove: fight first, revive after.
  const SquadDirective pushed = sb.update({body}, {9}, self, 0.9f, true, true, 1300);
  CHECK(!pushed.should_revive, "does not revive into a live fight on top of the body");
}

static void test_squad_revive_is_one_attempt_not_a_mode() {
  std::printf("squad: a revive is one committed attempt, then the bot plays on\n");
  SquadBrain sb(78);
  const Vec3 self{0.f, 0.f, 0.f};
  Teammate body;
  body.id = 4;
  body.pos = Vec3(6.f, 0.f, 0.f);
  body.downed = true;

  // Committed at the sighting, and held for the walk/revive window.
  CHECK(sb.update({body}, {}, self, 0.9f, false, false, 1000).should_revive,
        "commits to the body");
  CHECK(sb.update({body}, {}, self, 0.9f, false, false, 8000).should_revive,
        "holds the commitment while walking to it");
  // Window over: the bot must go back to playing, not stand on the body forever.
  // An unbounded rule made this 100% of ticks, i.e. a permanently stuck bot.
  CHECK(!sb.update({body}, {}, self, 0.9f, false, false, 40000).should_revive,
        "gives up after the hold window instead of stalling on the body");
  CHECK(!sb.update({body}, {}, self, 0.9f, false, false, 60000).should_revive,
        "and does not re-arm on the same body");

  // A DIFFERENT body later is a fresh attempt: the latch is per body.
  Teammate second;
  second.id = 9;
  second.pos = Vec3(7.f, 0.f, 0.f);
  second.downed = true;
  CHECK(sb.update({second}, {}, self, 0.9f, false, false, 90000).should_revive,
        "a new body is a new attempt");
}

static void test_squad_spacing_and_help() {
  std::printf("squad: does not stack, and calls for help\n");
  SquadBrain sb(74);
  const Vec3 self{0.f, 0.f, 0.f};
  {
    Teammate close;
    close.id = 2;
    close.pos = Vec3(1.f, 0.f, 0.f);   // 1m away: inside one grenade
    const SquadDirective d = sb.update({close}, {}, self, 1.f, false, false, 1000);
    CHECK(d.should_spread, "standing inside a squadmate is a spread order");
    CHECK(d.spacing >= 3.f, "and the target distance is real spacing, not a nudge");
    CHECK(d.spread_from.x == 1.f, "spreads relative to the squadmate");
  }
  {
    Teammate apart;
    apart.id = 2;
    apart.pos = Vec3(12.f, 0.f, 0.f);
    const SquadDirective d = sb.update({apart}, {}, self, 1.f, false, false, 1000);
    CHECK(!d.should_spread, "no spread order when already apart");
  }
  {
    // Help calls: only when actually losing, and rate limited.
    SquadBrain s2(75);
    const SquadDirective d1 = s2.update({}, {1}, self, 0.25f, true, true, 1000);
    CHECK(d1.requesting_help, "a hurt bot in a losing fight calls for help");
    const SquadDirective d2 = s2.update({}, {1}, self, 0.25f, true, true, 1500);
    CHECK(!d2.requesting_help, "but does not spam the call");
    const SquadDirective d3 = s2.update({}, {1}, self, 0.25f, true, true, 9000);
    CHECK(d3.requesting_help, "calls again later if still losing");
    SquadBrain s3(76);
    const SquadDirective healthy = s3.update({}, {1}, self, 0.95f, true, true, 1000);
    CHECK(!healthy.requesting_help, "a healthy bot does not ask for help");
  }
}

static void test_brain_squad_revive_overrides_the_plan() {
  std::printf("brain: a revive becomes the decision, with a real destination\n");
  Brain b(persona_rotator(), 77);
  WorldSnapshot w = peaceful_world();
  w.self_pos = Vec3(100.f, 100.f, 0.f);
  w.circle_next_center_x = 900.f;
  w.circle_next_center_y = 900.f;
  Teammate body;
  body.id = 3;
  body.pos = Vec3(106.f, 100.f, 0.f);
  body.downed = true;
  w.teammates = {body};
  const Decision d = b.think(w);
  CHECK(d.action == ActionKind::Revive, "the action is Revive, not a disguised rotation");
  CHECK(d.move_to.x == 106.f && d.move_to.y == 100.f, "and it walks to the body");
  CHECK(d.reason == "revive squadmate", "the reason says why");
}

static void test_brain_solo_behaviour_is_unchanged() {
  std::printf("brain: with no teammates, squad logic is a no-op\n");
  for (uint64_t seed = 80; seed < 90; ++seed) {
    Brain solo(persona_rotator(), seed);
    Brain squadded(persona_rotator(), seed);
    WorldSnapshot base = peaceful_world();
    base.time = 0.0;
    std::string a, b2;
    std::string c, d2;
    for (int i = 0; i < 300; ++i) {
      WorldSnapshot w = base;
      w.time = i * (1.0 / 60.0);
      w.self_health = 50.f + static_cast<float>(i % 5) * 10.f;
      w.loot_within_30m = i % 4;
      w.loot.clear();
      w.loot.push_back({1, Vec3(6.f, 0.f, 0.f), ItemClass::ShieldBig, 1});
      w.enemies_within_30m = i % 3;
      w.target_is_visible = (i % 4) < 2;
      w.target_distance = 12.f + static_cast<float>(i % 30);
      a += to_string(solo.think(w).action);
      // Same world, but the snapshot claims an empty squad list.
      WorldSnapshot ws = w;
      ws.teammates.clear();
      b2 += to_string(squadded.think(ws).action);
    }
    CHECK(a == b2, "a solo bot with an empty squad list behaves identically");
    (void)c;
    (void)d2;
  }
}

static std::string traced_run(uint64_t seed, int ticks) {
  Brain b(persona_rotator(), seed);
  std::string trace;
  for (int i = 0; i < ticks; ++i) {
    WorldSnapshot w = peaceful_world();
    w.time = i * (1.0 / 60.0);
    w.self_health = 40.f + 60.f * static_cast<float>(i % 7) / 7.f;
    w.distance_to_circle_edge = 5.f * static_cast<float>((i % 9) - 4);
    w.target_is_visible = (i % 3) != 0;
    w.target_distance = 12.f + static_cast<float>(i % 50);
    trace += to_string(b.think(w).action);
    trace += ",";
  }
  return trace;
}

// A world where several actions are plausible at once: hurt sometimes, safe
// sometimes, loot sometimes, a fight sometimes. This is the only kind of world
// in which "do bots behave differently" is a meaningful question.
static WorldSnapshot mixed_world(int i) {
  WorldSnapshot w;
  w.time = i * (1.0 / 60.0);
  w.has_weapon = true;
  w.self_health = 40.f + 50.f * static_cast<float>(i % 7) / 7.f;
  w.self_shield = 100.f * static_cast<float>(i % 4) / 4.f;
  w.shield_potions = i % 3;
  w.distance_to_circle_edge = 10.f * static_cast<float>((i % 9) - 4);
  w.loot_within_30m = i % 4;
  w.enemies_within_30m = i % 3;
  w.enemies_currently_fighting_each_other = (i % 5) == 0 ? 1 : 0;
  w.target_is_visible = (i % 4) < 2;
  w.target_distance = 12.f + static_cast<float>(i % 55);
  w.target_health = 40.f + static_cast<float>(i % 60);
  w.target_is_reloading = (i % 6) == 0;
  return w;
}

static std::string mixed_trace(const Persona& p, uint64_t seed, int ticks) {
  Brain b(p, seed);
  std::string t;
  for (int i = 0; i < ticks; ++i) {
    t += to_string(b.think(mixed_world(i)).action);
    t += ",";
  }
  return t;
}

static void test_determinism() {
  std::printf("determinism: same seed, same decisions\n");
  CHECK(traced_run(42, 600) == traced_run(42, 600),
        "identical seeds produce identical behaviour");
  CHECK(traced_run(42, 600) != traced_run(43, 600),
        "different seeds produce different behaviour");
  const Persona p = persona_rotator();
  CHECK(mixed_trace(p, 77, 400) == mixed_trace(p, 77, 400),
        "determinism holds in a contested world too");
}

static void test_lobby_no_mirror_bots() {
  std::printf("lobby: with all else equal, bots still diverge\n");
  const int n = 8;
  std::vector<std::string> traces;
  for (int s = 0; s < n; ++s) {
    Rng seeder(900ull + static_cast<uint64_t>(s));
    const Persona p = Persona::sample(seeder, 0.f);
    traces.push_back(mixed_trace(p, 500ull + static_cast<uint64_t>(s), 400));
  }

  int distinct = 0;
  for (size_t i = 0; i < traces.size(); ++i)
    for (size_t j = i + 1; j < traces.size(); ++j)
      if (traces[i] != traces[j]) ++distinct;
  CHECK(distinct >= 20, "the lobby is not eight copies of one bot");

  // Same persona, different seeds: the noise alone must be enough to separate
  // two otherwise identical bots, otherwise a lobby of one archetype is a mob.
  const Persona twin = persona_rotator();
  CHECK(mixed_trace(twin, 1, 400) != mixed_trace(twin, 2, 400),
        "identical personas with different seeds still differ");
}

int main() {
  test_math();
  test_perception_fov();
  test_perception_lock_takes_time();
  test_brain_zone_priority();
  test_brain_engage_vs_flee();
  test_brain_heal();
  test_plan_path_carries_the_intent();
  test_reaction_delay_blocks_instant_switch();
  test_motor_never_snap_aims();
  test_motor_aim_has_standing_jitter();
  test_motor_fire_is_not_instant();
  test_motor_build_cadence_scales_with_skill();
  test_motor_damage_flinch();
  test_motor_runs_dry_and_reloads();
  test_motor_tops_up_before_being_caught_dry();
  test_locomotion_strafes_in_a_fight_not_on_a_metronome();
  test_locomotion_never_frame_rate_crouch_spam();
  test_locomotion_braces_on_a_hard_fall();
  test_locomotion_jump_cadence_is_human();
  test_building_boxes_up_under_fire();
  test_building_uses_the_box_instead_of_rebuilding_it();
  test_build_grid_places_pieces_where_a_player_would();
  test_loot_classification();
  test_loot_valuation_prefers_what_you_lack();
  test_loot_distance_and_clock();
  test_loot_choice_picks_the_best_not_the_nearest();
  test_loot_preferences_differ_between_bots();
  test_brain_loot_intent_has_a_destination();
  test_perception_ignores_teammates();
  test_squad_focus_fire();
  test_squad_revives_before_looting();
  test_squad_revive_is_one_attempt_not_a_mode();
  test_squad_spacing_and_help();
  test_brain_squad_revive_overrides_the_plan();
  test_brain_solo_behaviour_is_unchanged();
  test_building_is_not_a_macro();
  test_building_goal_selection();
  test_population_spread();
  test_determinism();
  test_lobby_no_mirror_bots();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
