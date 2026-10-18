// Batch runner: plays N bot lives through a scripted scenario, then prints
// behavioural telemetry. This is the loop an agent uses to keep tuning the bot
// brain without a game engine, and the same telemetry a human uses to see
// whether the bots stopped feeling robotic.
//
// Build: make bench
// Usage: ./build/bench [lives] [minutes] [seed]
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "ogfn/ogfn_bots.h"

using namespace ogfn;

namespace {

struct Stats {
  std::map<std::string, int> action_counts;
  std::vector<float> shot_intervals;
  std::vector<float> reaction_times;
  float time_hurt = 0.f;
  float time_safe = 0.f;
  int shots = 0;
  int frames = 0;
  int decisions = 0;
  int plan_ticks = 0;
  int committed_ticks = 0;
  int reaction_held = 0;
  int build_inputs = 0;
  float aim_error_sum = 0.f;
  int aim_error_n = 0;
  int trigger_ticks = 0;
  int trigger_windows = 0;
  int bursts = 0;
  std::vector<float> burst_lengths;
};

// A synthetic match: the world evolves on a schedule so the bot experiences
// quiet periods, sudden contacts, a storm and a scramble, in a repeatable way.
WorldSnapshot scripted_world(double t, uint64_t life) {
  WorldSnapshot w;
  w.time = t;
  const double phase = std::fmod(t, 60.0);
  w.has_weapon = true;

  // Storm: closes slowly, forcing rotations twice a minute.
  w.circle_center_x = static_cast<float>(60.0 * std::sin(t / 37.0));
  w.circle_center_y = static_cast<float>(60.0 * std::cos(t / 41.0));
  w.circle_next_center_x = w.circle_center_x * 0.7f;
  w.circle_next_center_y = w.circle_center_y * 0.7f;
  w.circle_radius = static_cast<float>(220.0 - std::min(t / 3.0, 160.0));

  const float self_r = static_cast<float>(std::hypot(w.circle_center_x, w.circle_center_y)) * 0.f;
  (void)self_r;
  w.distance_to_circle_edge = static_cast<float>(40.0 * std::sin(t / 23.0)) - 15.f;

  // Loot and enemies come in waves.
  w.loot_within_30m = static_cast<int>(3.f + 3.f * std::sin(t / 11.0));
  if (w.loot_within_30m < 0) w.loot_within_30m = 0;
  w.enemies_within_30m = static_cast<int>(2.f + 2.f * std::sin(t / 7.0));
  if (w.enemies_within_30m < 0) w.enemies_within_30m = 0;
  w.enemies_currently_fighting_each_other = (std::fmod(t, 17.0) < 4.0) ? 2 : 0;

  // Contacts: periodic fights, longer every third one.
  const double contact_period = 26.0;
  const double since = std::fmod(t + static_cast<double>(life % 5) * 3.0, contact_period);
  const bool in_contact = since < (life % 3 == 0 ? 11.0 : 6.0);
  w.target_is_visible = in_contact;
  w.target_distance = in_contact ? static_cast<float>(8.0 + 30.0 * (since / contact_period)) : 999.f;
  w.target_health = 100.f - static_cast<float>(std::fmod(since * 9.0, 80.0));
  w.target_is_reloading = std::fmod(since, 3.0) < 0.8;

  // Health: drains during contact, recovers in quiet periods.
  w.self_health = in_contact ? static_cast<float>(35.0 + 40.0 * std::sin(t / 5.0)) : 95.f;
  w.self_health = clampf(w.self_health, 5.f, 100.f);
  w.self_shield = in_contact ? static_cast<float>(20.f + 60.f * std::sin(t / 9.0)) : 100.f;
  w.self_shield = clampf(w.self_shield, 0.f, 100.f);
  w.shield_potions = in_contact ? 0 : 3;

  (void)phase;
  return w;
}

Stats run_life(uint64_t seed, double minutes) {
  Stats s;
  Rng seeder(seed * 2654435761ull + 12345ull);
  const Persona p = Persona::sample(seeder, 0.f);
  Brain brain(p, seed);
  Motor motor(p, seed ^ 0x9E3779B9ull);

  const float dt = 1.f / 60.f;
  const int ticks = static_cast<int>(minutes * 60.0 / dt);
  float yaw = 0.f;
  double last_shot = -1e9;
  bool was_firing = false;
  float burst_frames = 0.f;
  Vec3 self_pos{0.f, 0.f, 0.f};

  for (int i = 0; i < ticks; ++i) {
    const double t = static_cast<double>(i) * static_cast<double>(dt);
    WorldSnapshot w = scripted_world(t, seed);
    const Decision d = brain.think(w);
    ++s.frames;
    s.action_counts[to_string(d.action)]++;
    if (d.reason == "plan") ++s.plan_ticks;
    if (d.reason == "committed") ++s.committed_ticks;
    if (d.reason == "reaction delay") ++s.reaction_held;

    if (w.target_is_visible) s.time_hurt += dt; else s.time_safe += dt;

    // Motor: aim and fire at the target when engaged, otherwise scan.
    MotorOutput out;
    if (d.fire && w.target_is_visible && w.target_distance < 80.f) {
      const Vec3 target{w.target_distance, 0.f, 0.f};
      out = motor.aim_at(self_pos, yaw, target, true, dt);
      ++s.trigger_windows;
      if (out.firing) {
        ++s.shots;
        ++s.trigger_ticks;
        if (!was_firing) ++s.bursts;
        if (was_firing && last_shot > -1e8)
          burst_frames += 1.f;
      } else if (was_firing) {
        // A burst just ended: record its length in seconds.
        s.burst_lengths.push_back(burst_frames * dt);
        burst_frames = 0.f;
      }
      was_firing = out.firing;
      last_shot = t;
    } else {
      if (was_firing) {
        s.burst_lengths.push_back(burst_frames * dt);
        burst_frames = 0.f;
        was_firing = false;
      }
      out = motor.idle_look(yaw, dt);
    }
    yaw = out.yaw_deg;
    s.aim_error_sum += motor.aim_error_deg();
    ++s.aim_error_n;
    s.build_inputs += motor.build_inputs_this_tick(dt);

    // Reaction telemetry: how long the bot took to adopt an intent after a
    // surprise contact, using the brain's own gate as the witness.
    if (brain.is_reacting(t)) ++s.decisions;
  }

  // Derive per-life reaction estimate from the exposed deadline.
  return s;
}

float pct(const std::vector<float>& v, float q) {
  if (v.empty()) return 0.f;
  std::vector<float> c = v;
  std::sort(c.begin(), c.end());
  const size_t i = static_cast<size_t>(q * static_cast<float>(c.size() - 1));
  return c[i];
}

}  // namespace

int main(int argc, char** argv) {
  const int lives = argc > 1 ? std::atoi(argv[1]) : 64;
  const double minutes = argc > 2 ? std::atof(argv[2]) : 6.0;
  const uint64_t base_seed = argc > 3 ? static_cast<uint64_t>(std::strtoull(argv[3], nullptr, 10)) : 1;

  Stats total;
  std::vector<float> all_intervals;
  std::vector<float> all_bursts;
  std::vector<float> reaction_ms;
  int distinct_actions_total = 0;

  for (int life = 0; life < lives; ++life) {
    const uint64_t seed = base_seed + static_cast<uint64_t>(life);
    const Stats s = run_life(seed, minutes);
    for (const auto& kv : s.action_counts) total.action_counts[kv.first] += kv.second;
    total.time_hurt += s.time_hurt;
    total.time_safe += s.time_safe;
    total.shots += s.shots;
    total.frames += s.frames;
    total.plan_ticks += s.plan_ticks;
    total.committed_ticks += s.committed_ticks;
    total.reaction_held += s.reaction_held;
    total.build_inputs += s.build_inputs;
    all_intervals.insert(all_intervals.end(), s.shot_intervals.begin(), s.shot_intervals.end());
    total.trigger_ticks += s.trigger_ticks;
    total.trigger_windows += s.trigger_windows;
    total.bursts += s.bursts;
    all_bursts.insert(all_bursts.end(), s.burst_lengths.begin(), s.burst_lengths.end());

    // Count how many distinct actions a single bot actually used. A bot with a
    // plan is a bot with a repertoire, not a one-trick loop.
    const int used = static_cast<int>(s.action_counts.size());
    if (used >= 4) ++distinct_actions_total;
  }

  const float denom = static_cast<float>(total.frames);
  std::printf("lives=%d  minutes/life=%.1f  seed=%llu\n", lives, minutes,
              (unsigned long long)base_seed);
  std::printf("frames=%d  bots with >=4 distinct actions: %d/%d\n", total.frames,
              distinct_actions_total, lives);
  std::printf("plan beats: %.1f%%  committed ticks: %.1f%%  held by reaction: %.2f%%\n",
              100.f * static_cast<float>(total.plan_ticks) / denom,
              100.f * static_cast<float>(total.committed_ticks) / denom,
              100.f * static_cast<float>(total.reaction_held) / denom);
  std::printf("shots=%d  trigger duty: %.1f%%  bursts=%d (%.1f/min)  burst length p50=%.2fs p90=%.2fs\n",
              total.shots,
              100.f * static_cast<float>(total.trigger_ticks) /
                  static_cast<float>(std::max(1, total.trigger_windows)),
              total.bursts,
              static_cast<float>(total.bursts) / (static_cast<float>(lives) * static_cast<float>(minutes)),
              pct(all_bursts, 0.50f), pct(all_bursts, 0.90f));
  std::printf("build inputs total=%d (%.2f/s per bot)\n", total.build_inputs,
              static_cast<float>(total.build_inputs) / (denom / 60.f));
  std::printf("time in contact: %.1f%% of life\n",
              100.f * total.time_hurt / (total.time_hurt + total.time_safe));

  std::printf("\naction mix:\n");
  std::vector<std::pair<std::string, int>> mix(total.action_counts.begin(), total.action_counts.end());
  std::sort(mix.begin(), mix.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  for (const auto& kv : mix) {
    std::printf("  %-16s %7d  %5.1f%%\n", kv.first.c_str(), kv.second,
                100.f * static_cast<float>(kv.second) / denom);
  }

  // Sanity gates so this is usable as a regression check in CI, not just a toy.
  int failures = 0;
  const float held = 100.f * static_cast<float>(total.reaction_held) / denom;
  if (held > 25.f) {
    std::printf("\nFAIL reaction gate is holding >25%% of ticks (latched bot)\n");
    ++failures;
  }
  if (distinct_actions_total < lives * 3 / 4) {
    std::printf("\nFAIL most bots are one-trick loops\n");
    ++failures;
  }
  if (total.shots == 0) {
    std::printf("\nFAIL bots never fired\n");
    ++failures;
  }
  std::printf("\n%s\n", failures == 0 ? "bench OK" : "bench FAILED");
  return failures == 0 ? 0 : 1;
}
