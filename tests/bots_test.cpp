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
  test_population_spread();
  test_determinism();
  test_lobby_no_mirror_bots();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
