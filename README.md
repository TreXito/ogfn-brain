# ogfn bots

Engine-agnostic C++ bot brain for an offline battle royale. The whole point of
this library is that the bots feel human, so every behavioural claim is pinned by
a test or measured by the bench. Nothing here depends on any game engine or on
any third-party binary.

## Build and run

```bash
make test    # 4000+ assertions, fails if the bots stop feeling human
make bench   # 64 scripted bot lives, prints behavioural telemetry
make         # static library for an engine to link
```

No cmake, no network, no gtest needed. g++ 15 and make are enough.

## What "human" means here, mechanically

Eight layers, each one a place where a naive bot gives itself away:

1. **Perception** (`perception.cpp`): FOV cone, range limit, occlusion, no
   wallhack, and a 4 second memory after losing sight. Lock-on takes ~0.35s.
2. **Decision cadence** (`brain.cpp`): humans re-plan at roughly 8 Hz, not 60 Hz.
   Bots commit to an intent for a beat (87% of ticks are "committed"), which is
   what removes the frame-to-frame argmax flicker that makes utility AI read as
   robotic.
3. **Reaction latency** (`brain.cpp`): only a surprise (combat involvement
   changing) costs reaction time, sampled log-normal per event, median 180 to
   320ms by skill. One episode, fixed deadline, never extended. Extending it is
   the bug that freezes a bot solid while it is being shot at.
4. **Idiosyncrasy** (`persona.cpp`): every bot gets a fixed per-action bias plus
   per-tick jitter, so eight bots with the same utility function still play
   differently, repeatably.
5. **Motor** (`motor.cpp`): velocity-limited aim servo with overshoot, standing
   hand jitter, occasional mouse slips, trigger discipline that taps at range and
   holds up close, a magazine that actually runs dry mid-fight, build cadence
   scaled to skill, and a flinch before reacting to damage.
6. **Locomotion** (`locomotion.cpp`): strafing perpendicular to the target with
   an irregular reversal cadence, a favourite lead shoulder held all match,
   crouch taps rate-limited to a hand (~4Hz max) plus a hold-cover peek cycle,
   jumping only to scramble or bail, and no steering at all during a hard fall.
7. **Loot valuation** (`loot.cpp`): pickups are objects, not a count. Worth is a
   function of what the bot already carries (a big pot at 0 shield is the best
   thing on the floor, the same pot at 100 shield is clutter), distance on a steep
   curve, and how long the circle gives you. Two bots with identical inventories
   disagree because their personas weight it differently.
8. **Building** (`building.cpp`): plans are sequences, not intentions. Losing a
   trade boxes up (three walls, an edited window, closing wall, roof), a healthy
   push ramps with a wall between each ramp, and a peek is a wall plus a window.
   Inputs run at the bot's own hand rate, inputs are occasionally dropped, and a
   finished box goes on cooldown so the bot shoots out of it instead of
   rebuilding it every two seconds.

Values are simulated, not real, but the shapes are: right-skewed latencies, a
skill distribution with a heavy tail, and a lobby containing genuinely bad and
genuinely good players.

## The improvement loop

`tools/autoloop.sh` is the loop that keeps this improving while nobody watches.
It runs the tests, runs the bench over a seed range, checks the telemetry against
human-plausible ranges, appends a report to `logs/autoloop.log`, and prints the
next thing to work on.

```bash
tools/autoloop.sh                 # check + report, no engine call
tools/autoloop.sh --engine "..."  # also invoke an agent CLI with the brief
```

## Next work, ranked

1. **Squad and teammate behaviour.** Everything here is a solo bot. Add pack
   logic: shared target calling, revive intent, not stacking on one another.
2. **Interpolation tuning against real matches.** Record a human playing, compare
   reaction and trigger telemetry against `bench`, and move the constants.
3. **Weapon feel per class.** One `WeaponProfile` now exists (mag + reload), but
   shotgun, AR and sniper still share one trigger discipline curve.
4. **Build failure recovery.** The builder plans; it does not notice when a piece
   is refused (no resource, blocked cell) and re-plan around it.

Done: movement model (strafe, peek, crouch, fall bracing), the magazine,
building as a fight (box up, covered ramp push, window edits), and loot
valuation.

## Integration contract

An engine only needs two calls per tick:

```cpp
brain.observe(observation, now);          // ground truth in
Decision d = brain.think(world_snapshot); // intent out
MotorOutput m = motor.aim_at(self, yaw, target_pos, d.fire, dt);
MoveOutput mv = loco.update(dt, yaw, yaw_to_target, dist, d.action == ActionKind::Engage,
                            m.firing, motor.reloading(), health_frac, move_state);
BuildInput bi = builder.update(dt, d.action == ActionKind::Build, taking_fire,
                               health_frac, yaw_to_threat, height_available);
// Loot and ThirdParty read from the world snapshot: w.loot (samples, not a count),
// w.inventory, w.fight_x/y. A Loot decision walks to brain.loot_choice().pos.
```

`Observation` is ground truth and the bot is only allowed to see what
`Perception` chose to let it see. That is the discipline that keeps the bot
honest: if a bot ever reacts to something outside its FOV, the bug is in the
engine integration, not in the brain.
