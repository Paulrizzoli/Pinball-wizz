# Pinball Wizz table files (.pwtable)

A table is a `.pwtable` file in the `Tables` folder next to the game. It is plain JSON (or a zip holding
`table.json` and its images). Pick it from **Table > Choose Table...**.

## Editing in the game

**Table > New Table** makes `My Table.pwtable` from a simple starter layout and opens the editor.
**Table > Edit This Table** opens the table being played. In the editor:

- Click a part to select it, drag it to move it. Walls, slingshots and lane switches show white points that
  can be dragged to reshape them; walls can gain or lose points.
- The left column adds parts (they appear in the middle of the view), copies or deletes the selected part.
- The right column changes the selected part, or the table's name and look when nothing is selected:
  pick a ready-made **Theme** (colour set) or change each colour, and choose a **Picture** for the
  playfield from the PNG/JPG files in the table's folder.
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
| `playfield` | `width`, `length`, `color`, `color2` (gradient), `image` (a PNG or JPG in the zip, or next to the table file; it replaces the painted playfield), `nameX`, `nameY`. |
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

- A group is complete when every part in it has been hit (or after `hits` hits on any of its parts).
  Its lights show progress, then flash and reset.
- `award`: `none`, `multiplier` (bonus and scoring multiplier 2X, 3X, 5X, 10X for the rest of the ball) or
  `extraBall`.
- `ballSave`: seconds after a ball's first hit in which a drained ball is given back, once per ball (a ball
  that drains before hitting anything is given back too). 0 or missing: off.
- A group with no parts yet is ignored.
- At the end of each ball the bonus is scored, times the multiplier, and the multiplier resets.
