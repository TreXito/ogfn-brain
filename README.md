# ogfn bots

Engine-agnostic C++ bot brain for an offline battle royale. The whole point of
this library is that the bots feel human, so every behavioural claim is pinned by
a test or measured by the bench. Nothing here depends on any game engine or on
any third-party binary.

## Build and run

```bash
make test    # 33000+ assertions, fails if the bots stop feeling human
make bench   # 64 scripted bot lives, prints behavioural telemetry
make         # static library for an engine to link
```

No cmake, no network, no gtest needed. g++ 15 and make are enough.

`tools/autoloop.sh` builds, tests, runs the bench over several seed ranges, checks
the telemetry against human-plausible ranges and prints the next thing to work on.
It runs unattended on a timer; a red loop means a human property regressed.

## What "human" means here, mechanically

Nine layers, each one a place where a naive bot gives itself away:

1. **Perception** (`perception.cpp`): FOV cone, range limit, occlusion, no
   wallhack, and a 4 second memory after losing sight. Lock-on takes ~0.35s.
   Teammates are dropped here, before tracking, so no consumer can aim at one.
2. **Decision cadence** (`brain.cpp`): humans re-plan at roughly 8 Hz, not 60 Hz.
   Bots commit to an intent for a beat (~88% of ticks are "committed"), which is
   what removes the frame-to-frame argmax flicker that makes utility AI read as
   robotic.
3. **Reaction latency** (`brain.cpp`): only a surprise (combat involvement
   changing) costs reaction time, sampled log-normal per event, median 180 to
   320ms by skill. One episode, fixed deadline, never extended. Extending it is
   the bug that freezes a bot solid while it is being shot at.
4. **Idiosyncrasy** (`persona.cpp`): every bot gets a fixed per-action bias plus
   per-tick jitter, so eight bots with the same utility function still play
   differently, repeatably.
5. **Motor** (`motor.cpp`): velocity-limited aim servo with real overshoot,
   track-velocity feed-forward so it can hold a sprinting strafe, target leading
   with per-bot competence, vertical aim and muzzle climb, standing hand jitter,
   mouse slips, and per-class weapons. A gun has a cyclic rate (a rifle ~600 RPM,
   an SMG ~850, a pump shotgun 0.85s, a bolt sniper 1.45s), its own magazine,
   burst window, trigger tolerance and recoil, and a single-shot class cannot be
   held down at all. Holding the trigger is `firing`; a round leaving the barrel
   is `shot_fired`, and they are not the same boolean.
6. **Locomotion** (`locomotion.cpp`): strafes perpendicular to the target with an
   irregular reversal cadence, a favourite lead shoulder held all match, crouch
   taps rate-limited to a hand (~4Hz max) plus a hold-cover peek cycle, jumping
   only to scramble or bail, no steering during a hard fall, and a speed that
   drifts continuously instead of stepping between four constants.
7. **Gaze** (`locomotion.cpp` + `brain.cpp`): the view follows the travel
   direction out of combat, with held shoulder-checks to one flank, and belongs to
   the fight in one. A bot that walks sideways with its crosshair on nothing is
   the tell this exists for.
8. **Loot valuation** (`loot.cpp`): pickups are objects, not a count. Worth is a
   function of what the bot already carries (a big pot at 0 shield is the best
   thing on the floor, the same pot at 100 shield is clutter), distance on a steep
   curve, and how long the circle gives you. Two bots with identical inventories
   disagree because their personas weight it differently.
9. **Building** (`building.cpp`): plans are sequences, not intentions. Losing a
   trade boxes up (three walls, an edited window, closing wall, roof), a healthy
   push ramps with a wall between each ramp, and a peek is a wall plus a window.
   Inputs run at the bot's own hand rate, inputs are occasionally dropped, and a
   finished box goes on cooldown so the bot shoots out of it instead of rebuilding
   it every two seconds.
10. **Squad** (`squad.cpp`): focus fire (everyone on the same body, called or
    simply converged), a revive that outranks the plan and is one committed 14s
    attempt rather than a mode, spacing so two bots never share a doorway, and
    help calls a losing bot makes at most every 4s. An empty squad list is a
    bit-for-bit no-op, so solo lobbies are unchanged.
11. **Committed actions** (`brain.cpp`): healing is a 2 to 5 second animation the
    bot is locked into, can be interrupted by a close threat, and stands still
    behind cover. Being in the storm outranks taking a fight, from the moment you
    step outside. Flee needs real danger: never at full health, never from 40m.

Values are simulated, not real, but the shapes are: right-skewed latencies, a
skill distribution with a heavy tail, and a lobby containing genuinely bad and
genuinely good players.

## What the bench measures, and the bands it enforces

| property | band | why |
| --- | --- | --- |
| trigger duty | 15 to 96% | a bot that never lets go is a hose |
| reaction held | 0 to 8% of ticks | a latched reaction freezes the bot |
| build inputs | 0.5 to 8 /s | below is not building, above is a macro |
| build pieces | 0.02 to 2 /s | turbo-building is machine behaviour |
| piece edits | > 0 | a bot that walls up and cannot shoot out is in a coffin |
| loot decisions | > 0, none at the origin | a decision with no destination is a no-op |
| squad revives / focus / spread | > 0 | the squad layer must not be inert |
| gaze off travel | p50 < 20 deg | the bot faces where it walks |
| healing | 1 to 35% of life | never healing, or always in an animation |
| strafe | > 50% of contact | a bot that stands still in a fight |
| crouch changes | < 90 /min | frame-rate crouch spam |
| shots/min per bot | 5 to 400 | a gun firing on every trigger frame is 3600 RPM |
| shots per burst | 1.5 to 40 | 1 means the trigger is a tap gun, 60 means a hose |

Plus tests pinning: flick speed 300 to 800 deg/s and settle 150 to 350ms,
tracking error against a real sprint strafe, lead signs and magnitudes, vertical
aim, burst length and shots-per-burst distributions, reload and jump cadence,
speed continuity, pool spread, determinism, and that a solo lobby is unchanged by
the squad layer.

## The improvement loop

```bash
tools/autoloop.sh                 # build, check, report
tools/autoloop.sh --engine "..."  # also invoke an agent CLI with the brief
```

## Next work, ranked

1. **Calibrate against a recorded human match.** Every constant was set from a
   human band, none from this game's actual replays. That needs a recording.
2. **Build failure recovery.** The builder plans; it does not notice when a piece
   is refused (no resource, blocked cell) and re-plan around it.
3. **Squad revive live.** Unit tested and measured in the bench, but the engine
   integration has no downed-state or revive verb on 3.5.
4. **Weapon overlap in the lobby.** Each bot now holds one class for the whole
   match. Real lobbies swap guns mid-fight, including to an empty slot.

## Integration contract

```cpp
brain.observe(observation, now);            // ground truth in, FOV-filtered
Decision d = brain.think(world_snapshot);   // intent out
MotorOutput m = motor.aim_at(self, yaw, pitch, target_pos, d.fire, dt, target_motion);
MoveOutput mv = loco.update(dt, yaw, yaw_to_target, dist, engaged, m.firing,
                            motor.reloading(), health_frac, move_state,
                            travel_yaw, travelling);
BuildInput bi = builder.update(dt, d.action == ActionKind::Build, taking_fire,
                               health_frac, yaw_to_threat, height_available);
```

Carry `m.pitch_deg` back into the next `aim_at`, and hand `TargetMotion`
(velocity) to the motor or the bot cannot lead. Loot and ThirdParty read from the
snapshot: `w.loot` (samples, not a count), `w.inventory`, `w.fight_x/y`. A Loot
decision walks to `brain.loot_choice().pos`. Feed `mv.gaze_yaw_deg` back through
`brain.set_gaze`.

`Observation` is ground truth and the bot is only allowed to see what
`Perception` chose to let it see. That is the discipline that keeps the bot
honest: if a bot ever reacts to something outside its FOV, the bug is in the
engine integration, not in the brain.
