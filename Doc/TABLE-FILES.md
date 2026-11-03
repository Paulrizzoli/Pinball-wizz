# Pinball Wizz table files (.pwtable)

A table is a `.pwtable` file in the `Tables` folder next to the game. It is plain JSON (or a zip holding
`table.json` and its images). Pick it from **Table > Choose Table...**.

## Editing in the game

**Table > New Table** makes `My Table.pwtable` from a simple starter layout and opens the editor.
**Table > Edit This Table** opens the table being played. In the editor:

- Click a part to select it, drag it to move it. Walls, slingshots, lane switches, ramps and gates show
  white points that can be dragged to reshape them; walls and ramps can gain or lose points.
- The left column adds parts (they appear in the middle of the view), copies or deletes the selected part.
- The right column changes the selected part, or the table's name and look when nothing is selected:
  pick a ready-made **Theme** (colour set) or change each colour, choose a **Picture** for the
  playfield from the PNG/JPG files in the table's folder, or a painted **Pattern** (grid, or stars and a
  planet).
- The **Rules** tab sets up groups of parts (see Rules below), the shoot-again and multiplier lights and the
  ball save. **Pick on the table** adds parts and lights to a group by clicking them; a group's parts show
  in green and its lights in yellow.
- Right-drag pans, the mouse wheel zooms, **Fit** shows the whole table. **Snap to grid** keeps positions on
  0.1 steps.
- **Undo**/**Redo** (Ctrl+Z, Ctrl+Y), **Save** (Ctrl+S), **Save and Play** saves and starts the table.
- Renaming or deleting a part keeps the rules pointing at the right parts.

Packed (zip) tables can't be edited in the game yet.

Coordinates are in table units: the playfield centre is 0,0, x grows to the right and y grows towards the
player. The default playfield is 16 wide and 29 long.

## Top level

| Key | Meaning |
| --- | --- |
| `format`, `version` | Always `"pinball-wizz-table"` and `1`. |
| `name`, `author` | Shown in the picker and on the score panel. |
| `playfield` | `width`, `length`, `color`, `color2` (gradient), `image` (a PNG or JPG in the zip, or next to the table file; it replaces the painted playfield), `decor` (`grid` or `space`: stars, glows and a ringed planet at `planetX`, `planetY`), `nameX`, `nameY`. |
| `theme` | Colours: `walls`, `rubber`, `accent`, `accent2`, `cabinet`, `panel`, `metal`, `ball`, `display`; `name` is the theme picked in the editor. |
| `ballsPerGame` | 1 to 9, default 3. |
| `parts` | The parts, below. Give a part an `id` to use it in rules. |
| `rules` | Groups, awards and lights, below. |

## Parts

| `type` | Keys |
| --- | --- |
| `wall` | `points`, `closed`, `thickness`, `height`, `color`, `bounce`, `score` |
| `post` | `x`, `y`, `radius`, `score` |
| `slingshot` | `points` (3 corners), `strength`, `color`, `score` |
| `bumper` | `x`, `y`, `radius`, `strength`, `color`, `capColor`, `score` |
| `flipper` | `side` (left/right), `x`, `y`, `length`, `angle` (rest, degrees), `swing`, `strength` |
| `plunger` | `x`, `y`, `width` |
| `drain` | `y`, `x0`, `x1` |
| `lane` | `points` (the 2 ends of the switch line), `score`, `bonus` |
| `target` | `x`, `y`, `width`, `angle` (direction the face looks, degrees), `color`, `score`, `bonus` |
| `light` | `x`, `y`, `radius`, `shape` (circle/arrow), `angle` (arrow), `color`, `label` |
| `drop` | Drop target: `x`, `y`, `width`, `angle` (direction the face looks), `popup` (seconds before it can come back), `strength`, `color`, `score`, `bonus`. It drops when hit and comes back up when its rules group is complete. |
| `kickout` | Kickout hole: `x`, `y`, `radius`, `angle` (direction it kicks the ball, degrees), `strength`, `color`, `score`, `bonus`. It holds the ball for a moment, then kicks it out. |
| `spinner` | `x`, `y`, `width`, `angle` (turn of the spinner, degrees), `friction` (lower spins longer), `color`, `score` (per turn), `bonus` |
| `ramp` | `points` (the path; the ball enters at the first point), `width`, `color` (floor), `railColor`, `score`, `bonus`. It counts when the ball leaves the far end; a flap there stops balls rolling in backwards. |
| `gate` | One-way gate: `points` (2 ends), `color`, `score`. The ball passes from the right-hand side looking from the first point to the second, and is stopped the other way. |

`bonus` is added to the end-of-ball bonus each time the part is hit.

## Rules

```json
"rules": {
  "groups": [
    {"name": "TOP LANES", "parts": ["lane A", "lane B"], "lights": ["lane A light", "lane B light"],
     "score": 2000, "bonus": 2000, "award": "multiplier"},
    {"name": "BUMPERS", "parts": ["bumper 1", "bumper 2"], "hits": 20, "score": 15000,
     "message": "BUMPER FRENZY"}
  ],
  "extraBallLight": "shoot again",
  "multiplierLights": ["mult 2X", "mult 3X", "mult 5X", "mult 10X"],
  "ballSave": 8
}
```

A mission, as on Night Orbit:

```json
{"name": "DROP TARGETS", "parts": ["drop 1", "drop 2", "drop 3"], "award": "startMission",
 "startsMission": "LAUNCH"},
{"name": "LAUNCH", "mission": true, "time": 30, "hint": "SHOOT THE HOLE", "parts": ["launch hole"],
 "lights": ["launch light"], "score": 50000, "award": "extraBall"}
```

- A group is complete when every part in it has been hit (or after `hits` hits on any of its parts).
  Its lights show progress, then flash and reset.
- `award`: `none`, `multiplier` (bonus and scoring multiplier 2X, 3X, 5X, 10X for the rest of the ball),
  `extraBall`, `multiball` (a second ball is launched; not while a multiball is already running) or
  `startMission` (starts the mission group named in `startsMission`).
- A group with `"mission": true` is a mission: it only counts while running. It is started by another
  group's `startMission` award, shows its `name` and `hint`, flashes its lights, and must be completed
  within `time` seconds (0: no limit) or before the ball ends, else it shows TIME UP. Completing it gives
  its own score and award.
- `ballSave`: seconds after a ball's first hit in which a drained ball is given back, once per ball (a ball
  that drains before hitting anything is given back too). 0 or missing: off.
- A group with no parts yet is ignored.
- At the end of each ball the bonus is scored, times the multiplier, and the multiplier resets.

## Night Orbit

`Tables/Night Orbit.pwtable` is the starter table that ships with the game, made with the editor: an
original deep-space layout with a left orbit (spinner and switch), a right ramp, three bumpers, three top
lanes, a bank of three drop targets that starts the LAUNCH mission at the kickout hole, and 2-ball
multiball after five ramp shots. Open it with **Table > Edit This Table** to see how it is put together.

## Skatepark

`Tables/Skatepark.pwtable` is a second original table, with its own painted playfield picture
(`Tables/Skatepark.jpg`, drawn by `tools/tables/make_skatepark.py`, which also writes the table file). It has
a concrete bowl with three traffic-cone bumpers, S-K-8 top lanes, a halfpipe orbit with a wheel spinner, a
mega ramp (four shots start SESSION MULTIBALL), a grind rail up the middle, a DECK bank of four drop targets
that starts the DROP IN mission at the bowl hole (extra ball), and KICK and FLIP targets that start the GRIND
mission (shoot the rail within 25 seconds).
