# Levitate behaviour spec

What Levitate reproduces from the Java mod [Create](https://modrinth.com/mod/create) **6.0.10** (simibubi and the Creators of Create).
This is the behaviour reference for every feature unit (see [AGENTS.md](../AGENTS.md)). It describes
behaviour only: never copy code or assets from Create.

**Goal: no compromise.** Every block, item, mechanic and screen of Create 6.0.10 exists in Levitate and
behaves the same, on the dedicated server (BDS) and on the client (local worlds). Only what Bedrock
really cannot do is listed in §8, each with the reason and the closest behaviour we ship instead.

**Who owns what.** Each section in §2–§7 names the unit that owns it (§9). The owner may correct and
extend its own sections in its PR (numbers, missing details); a value marked ⚠ is from memory and
**must** be checked by the owner against Create 6.0.10 (in-game behaviour and its default config
values, which are behaviour) before it is implemented. Never edit another unit's section.

## 1. Overview

### 1.1 Platforms

| | Dedicated server (BDS) | Client (LeviLauncher) |
|---|---|---|
| World logic | the mod on the server | the mod on the integrated server of local worlds |
| Rendering of moving parts | players with the client mod see it animated; vanilla players see static models (§1.4) | animated |
| Screens | native screens for players with the client mod; server forms for vanilla players | native screens |
| Resource pack | `PackDeployer` puts it on the world (required by default) | global resource pack stack |

The **client mod is optional** for joining a server: a vanilla Bedrock client gets every mechanic,
static models for rotating parts and form fallbacks for screens. With the client mod (detected by the
companion handshake, U3) a player gets animated kinetics, contraptions, curved tracks, native screens,
the goggles overlay, value boxes, Ponder and every other client-side feature.

### 1.2 Bedrock building blocks

- **Blocks**: data-driven blocks in the behavior pack (`assets/pack/bp/blocks/<id>.json`) with block
  states for everything that must survive a reload and be visible (facing, axis, powered, part, ...).
- **Block entity replacement**: custom blocks can't have block entities. Per-position data
  (inventories, tanks, kinetic speed, settings) lives in `BlockData` (U3), keyed by dimension +
  position, saved in the world DB per chunk, removed when the block is broken or replaced.
- **Kinetic network**: server-side graph (U2) recomputed incrementally on place/break/state change.
- **Moving structures** (contraptions, trains): server entities that carry a block snapshot (U4); the
  client mod renders their blocks; vanilla clients see a placeholder entity (§8).
- **Rendering**: the client mod draws rotating parts, belts, contraptions, curved tracks, fluids in
  tanks, overlays and screens natively. Every such block also has a static Bedrock geometry so that a
  vanilla client sees a correct (non-moving) block.
- **Recipes**: vanilla crafting/furnace/stonecutter recipes as BP recipe files; Create's own recipe
  types (crushing, milling, pressing, mixing, compacting, splashing, smoking via fan, blasting via
  fan, haunting, deploying, filling, emptying, cutting, sandpaper polishing, item application,
  sequenced assembly, mechanical crafting) as JSON data in `assets/recipes/<type>/<id>.json`, loaded by
  the owning unit (U3 provides the loader, §3.3).

### 1.3 Units and ticks

Create uses 20 TPS like Bedrock, so tick counts are the same. Speeds are **RPM**, stress is **SU**
(stress units): a component with impact *i* at speed *s* uses `i·|s|` SU; a source with capacity *c*
at its generated speed *g* provides `c·|g|` SU (Create semantics; config values are per RPM).

### 1.4 What a vanilla client sees

Static geometry of every block (shaft, cog, belt, etc.) in its current orientation; contraptions are
rendered as their blocks being placed/removed at the assembled position when they stop, and as a
placeholder entity while moving (§8); native screens become forms; overlays become action-bar text
(goggles) and chat (value boxes through forms).

## 2. Blocks and items

Ids are `levitate:<create path>` (Create's registry names). Block names come from RP `texts.d`.
Every block drops itself unless noted; loot tables in `loot_tables/blocks/<id>.json`. Recipes: the
Create 6.0.10 recipe for every item (crafting shape and every processing route), written as our own
data files.

### 2.1 Kinetic transmission (U6)

`shaft`, `cogwheel`, `large_cogwheel`, `gearbox`, `vertical_gearbox` (item only, places a gearbox),
`clutch`, `gearshift`, `encased_chain_drive`, `adjustable_chain_gearshift`, `andesite_encased_shaft`,
`brass_encased_shaft`, `andesite_encased_cogwheel`, `brass_encased_cogwheel`,
`andesite_encased_large_cogwheel`, `brass_encased_large_cogwheel`, `sequenced_gearshift`,
`rotation_speed_controller`, `speedometer`, `stressometer`, `belt` connector item is U11.
Encased variants are separate ids (as Create). Every block has `levitate:rotor` (0 full model,
1 without the rotating parts for the client mod, §3.1). Block states (integers):

| Block | States |
|---|---|
| shaft, cogwheel, large_cogwheel, encased shafts, gearbox | `levitate:axis` 0 x / 1 y / 2 z (gearbox: the axis *without* shafts) |
| encased (large) cogwheels | `axis`, `levitate:top_shaft`, `levitate:bottom_shaft` (shaft end on the positive / negative axis face) |
| clutch, gearshift | `axis`, `levitate:powered` |
| encased_chain_drive | `axis`, `levitate:part` (0 none, 1 start, 2 middle, 3 end), `levitate:along_first` (Create's CONNECTED_ALONG_FIRST_COORDINATE) |
| adjustable_chain_gearshift | the chain drive states + `levitate:signal` 0..15 (Create keeps the analog signal in the block entity; here it is a state so the network re-propagates when it changes) |
| sequenced_gearshift | `axis` (Create's VERTICAL + HORIZONTAL_AXIS), `levitate:state` 0..5 (Create's STATE), `levitate:mod` (speed modifier of the running instruction: 0, 1, 2, −1, −2 as 0..4) |
| rotation_speed_controller | `axis` 0 / 2 (HORIZONTAL_AXIS), `levitate:bracket` (large cog on top), `levitate:revision` (toggled to re-attach the network when the target speed changes) |
| speedometer, stressometer | `levitate:facing` 0..5, `levitate:along_first`, `levitate:dial` 0..5 (static needle for vanilla clients, 22.5° steps) |

Drops: encased shafts drop a shaft, encased cogwheels their cogwheel / large cogwheel (as Create);
a gearbox with a horizontal axis drops a vertical gearbox. Recipes (Create 6.0.10, `crafting/kinetics`):
shaft ×8 = andesite alloy / andesite alloy; cogwheel = shaft + planks; large cogwheel = shaft + 2 planks
or cogwheel + planks; gearbox = cogwheel ×4 around an andesite casing (` C / CBC / C `); vertical
gearbox = `C C / B / C C`; gearbox ↔ vertical gearbox and speedometer ↔ stressometer convert
shapeless; clutch = andesite casing + shaft + redstone; gearshift = andesite casing + cogwheel +
redstone; encased chain drive = andesite casing + 3 iron (or zinc) nuggets; adjustable chain
gearshift = encased chain drive + electron tube; sequenced gearshift = brass casing + cogwheel +
electron tube; rotation speed controller = precision mechanism / brass casing; speedometer = compass
/ andesite casing. Deploying: shaft + planks → cogwheel, cogwheel + planks → large cogwheel. Recipes
needing U9 / U10 items wait in `assets/pending_recipes/transmission/`.

### 2.2 Generators (U7)

`hand_crank`, `copper_valve_handle` + `<colour>_valve_handle` (16), `water_wheel`, `large_water_wheel`
(+ structural `water_wheel_structure`), `creative_motor`. Windmill bearing and sails are U21; steam
engine is U18.

- States: `levitate:facing` 0..5 (down, up, north, south, west, east) for the crank, valve handles,
  water wheel and motor; `levitate:axis` 0..2 + `levitate:extension` for the large wheel;
  `levitate:wood` 0..11 (oak, spruce, birch, jungle, acacia, dark oak, mangrove, cherry, bamboo,
  crimson, warped, pale oak; default spruce as Create) for both wheels and the structure;
  `levitate:segment` 0..7 + `levitate:axis` for the structure; `levitate:rotor` (kinetics renderer).
- Drops: itself; the large wheel drops itself, the structure drops nothing (§3.6.1).
- Recipes (Create 6.0.10): hand crank = 3 planks / andesite alloy (right); copper valve handle =
  3 copper sheets / andesite alloy (middle); `<colour>_valve_handle` = copper valve handle + dye
  (shapeless); water wheel = 8 wooden slabs around a shaft; large water wheel = 8 planks around a
  water wheel; creative motor: none (creative only). The first three need U9 / U6 items and wait in
  `assets/pending_recipes/generators/`.

### 2.3 Tools and info (U8)

`wrench`, `goggles` (engineer's goggles, head slot). Both stack to 1. Recipes (Create 6.0.10,
`assets/pending_recipes/goggles/` until U9's sheets and U6's cogwheel exist): wrench `GG / GP / ␣S`
(G golden sheet, P cogwheel, S stick); goggles `␣S␣ / GPG` (S string, G glass, P golden sheet).
The goggles are a wearable (`slot.armor.head`, no protection) with an attachable model (a strap
and two brass lenses on the head; a small copy in the hand). Mechanics in §3.6 (U8).

### 2.4 Materials, food, world (U9)

Metals: `zinc_ore`, `deepslate_zinc_ore`, `raw_zinc`, `raw_zinc_block`, `zinc_ingot`, `zinc_nugget`,
`zinc_block`, `brass_ingot`, `brass_nugget`, `brass_block`, `brass_sheet`, `copper_sheet`, `iron_sheet`,
`golden_sheet`, `copper_nugget`, `andesite_alloy`, `andesite_alloy_block`, `industrial_iron_block`,
`weathered_iron_block`, `crushed_raw_<metal>` (iron, gold, copper, zinc, + compat metals listed by
Create: osmium, platinum, silver, tin, lead, quicksilver, aluminum, uranium, nickel), `powdered_obsidian`,
`cinder_flour`, `wheat_flour`, `dough`, `rose_quartz`, `polished_rose_quartz`, `rose_quartz_block`,
`rose_quartz_tiles`, `small_rose_quartz_tiles`, `electron_tube`, `precision_mechanism`,
`incomplete_precision_mechanism`, `propeller`, `whisk`, `brass_hand`, `crafter_slot_cover`,
`sturdy_sheet`, `unprocessed_obsidian_sheet`, `transmitter`, `pulp`, `cardboard`, `cardboard_block`,
`bound_cardboard_block`, `experience_nugget`, `experience_block`, `refined_radiance`, `shadow_steel`,
`chromatic_compound`, `bar_of_chocolate`, `sweet_roll`, `chocolate_glazed_berries`, `honeyed_apple`,
`builders_tea`, `blaze_cake_base`, `blaze_cake`, `creative_blaze_cake`, `dough`, `tree_fertilizer` is U31.
World generation (Create 6.0.10's worldgen data, details in §3.6.9): zinc ore veins (every overworld
biome, ore size 12, 8 per chunk, uniform y −63..70), striated stone layers (overworld: 1 in 18 chunks,
y −30..70, size 32, six patterns around asurine, crimsite, ochrum, veridium, limestone, scoria;
nether: 1 in 18 chunks, y 40..90, scoria and scorchia patterns).

Decorative stone (U9): for each of `granite, diorite, andesite, calcite, dripstone, deepslate, tuff,
asurine, crimsite, limestone, ochrum, scoria, scorchia, veridium`: the raw stone (for Create's own
stones) and `cut_<s>`, `cut_<s>_stairs/_slab/_wall`, `polished_cut_<s>` (+ stairs/slab/wall),
`cut_<s>_bricks` (+ stairs/slab/wall), `small_<s>_bricks` (+ stairs/slab/wall), `layered_<s>`,
`<s>_pillar`. Stonecutter recipes as Create.

### 2.5 Building blocks (U10)

132 blocks (Create 6.0.10's ids), mechanics in §3.6.1:

- Casings: `andesite_casing`, `brass_casing`, `copper_casing`, `shadow_steel_casing` ("Shadow Casing"),
  `refined_radiance_casing` ("Radiant Casing", light 12), `railway_casing` ("Train Casing").
- Glass: `framed_glass`, `tiled_glass`, `horizontal_framed_glass`, `vertical_framed_glass` and their
  `_pane`s; windows `<wood>_window` + `_window_pane` for oak, spruce, birch, jungle, acacia, dark_oak,
  mangrove, crimson, warped, cherry, bamboo, plus `ornate_iron_`, `industrial_iron_`, `weathered_iron_window`
  (+ panes; Create 6). Glass, windows and panes drop only with silk touch (Create's loot tables).
- Copper roofing: `copper_shingles` / `copper_tiles`, `copper_shingle_stairs` / `copper_tile_stairs`,
  `copper_shingle_slab` / `copper_tile_slab`, each with the `exposed_`, `weathered_`, `oxidized_`
  stages and all of them `waxed_` (48 blocks).
- `andesite_` / `brass_` / `copper_scaffolding`, `_ladder`, `_bars`, `_table_cloth` ("Table Cover").
- Sliding doors `andesite_door` (folds), `brass_door`, `copper_door` (folds), `train_door`,
  `framed_glass_door`; trapdoors `train_trapdoor`, `framed_glass_trapdoor`.
- `metal_girder`, `metal_girder_encased_shaft` (kinetic shaft), `metal_bracket`, `wooden_bracket`
  (items applied to shafts / cogwheels / pipes), `<colour>_table_cloth` (16; shop behaviour is U26),
  `placard`, `copycat_panel`, `copycat_step`. Not ported: `copycat_bars` / `copycat_base` (internal,
  not obtainable in Create). `<colour>_toolbox` is U31, `<colour>_seat` U22, valve handles U7.
- Recipes (Create 6.0.10's data): windows ` # / #X#` ×2 (planks, iron nugget for ornate, industrial /
  weathered iron block), panes 6 → 16, glass stonecutting 1:1 from glass / glass pane, smelting back
  to glass; copper roofing stonecutting 1 ingot → 2, stairs (6 → 4, stonecut 1), slabs (3 → 6,
  stonecut 2), waxing with honeycomb (shapeless); metal scaffolding / ladder 1 → 2, bars 1 → 4,
  table cover 1 → 2 by stonecutting the alloy / ingot; table cloths wool + andesite alloy → 2, dye +
  any other table cloth → that colour; doors / trapdoors shapeless wooden door / trapdoor + casing
  (framed glass for the glass ones, every wooden door / trapdoor id); copycats 1 zinc → 4 by
  stonecutting; girder `PPP/CCC` (iron sheet, alloy) ×8; brackets `SSS/PCP` ×4; placard = item frame +
  brass sheet. Processing: casings by item application (stripped log / wood + alloy / brass /
  copper, railway casing = brass casing + obsidian sheet), deploying with an axe (scrape / unwax) and a
  honeycomb block (wax), splashing industrial → weathered iron window. Recipes with U9's items wait in
  `assets/pending_recipes/building/`. Generated by `tools/gen/building.ps1`.

### 2.6 Belts and depots (U11)

`belt_connector` (item "Mechanical Belt", stack 64; recipe: 6 dried kelp, `DDD/DDD`), `belt` (block,
no item; states `levitate:facing` 0..3 (0 south, 1 west, 2 north, 3 east: start -> end), `levitate:slope`
0 horizontal / 1 upward / 2 downward / 3 vertical / 4 sideways, `levitate:part` 0 start / 1 middle /
2 end / 3 pulley, `levitate:casing` 0 none / 1 andesite / 2 brass, `levitate:dyed` + `levitate:color`
0..15 (Create's dye colour in the block entity; a state here so vanilla clients see it); hardness 0.8,
wool sounds, drops handled in code), `depot` (recipe: andesite alloy + andesite casing, shapeless),
`weighted_ejector` (states `levitate:facing`, `levitate:lid` (plate up), `levitate:rotor`; recipe:
golden sheet / depot / cogwheel in a column). The depot and ejector recipes need U9 / U10 / U6 items
and wait in `assets/pending_recipes/belts/`.

### 2.7 Item logistics (U12)

`andesite_funnel`, `brass_funnel`, `andesite_tunnel`, `brass_tunnel`, `chute`, `smart_chute`,
`item_vault`, `creative_crate`. Behaviour in §3.6 (U12).

| Block | States | Drops / recipe (Create 6.0.10) |
|---|---|---|
| `andesite_funnel`, `brass_funnel` | `levitate:facing` 0..5 (the side the opening faces, away from the attached block), `levitate:extracting`, `levitate:powered`, `levitate:flap` | itself; 2 from andesite alloy over dried kelp / 2 from electron tube, brass ingot, dried kelp (column) |
| `andesite_tunnel`, `brass_tunnel` | `levitate:axis` 0 x / 2 z (the belt's axis), `levitate:shape` 0 straight, 1 window, 2 closed, 3 T left, 4 T right, 5 cross | itself; 2 from 2 andesite alloy over 2 dried kelp / 2 from electron tube, 2 brass ingots, 2 dried kelp |
| `chute` | `levitate:facing` 0 down or 2..5 (diagonal), `levitate:shape` 0 intersection, 1 window, 2 normal, 3 encased | itself; 4 from iron sheet, iron ingot, iron sheet (column) |
| `smart_chute` | `levitate:powered` | itself; brass sheet, chute, electron tube (column) |
| `item_vault` | `levitate:axis` 0 x / 2 z, connected textures `levitate:len` / `levitate:u` / `levitate:v` / `levitate:large` | itself; iron sheet, barrel, iron sheet (column) |
| `creative_crate` | — | itself; creative only (no recipe) |

The recipes use U9's materials (andesite alloy, brass, sheets, electron tube): they wait in
`assets/pending_recipes/funnels/` until U9 is merged. Belt funnels (`andesite_belt_funnel`,
`brass_belt_funnel`: a funnel placed on the side of a belt) come with the belt API (§3.6 U12).

### 2.8 Bulk processing (U13)

`millstone`, `crushing_wheel` (+ `crushing_wheel_controller`, invisible), `encased_fan`, `nozzle`.

- `millstone`: state `levitate:rotor` (client models, §3.1). Hardness 1.5, drops itself (and its
  contents). Recipe: cogwheel / andesite casing / any of `c:stones` (stone, granite, diorite,
  andesite, deepslate, tuff: one Bedrock recipe each).
- `crushing_wheel`: states `levitate:axis` 0..2, `levitate:rotor`. Recipe: mechanical crafting
  (`assets/recipes/mechanical_crafting/crushing_wheel.json`, U16), 2 wheels.
- `crushing_wheel_controller`: states `levitate:facing` 0..5 (Create's Direction order),
  `levitate:valid`; invisible, not selectable, unbreakable by hand, no drops (its contents drop when
  it goes); collision: Create's 13 px slab on its facing side.
- `encased_fan`: states `levitate:facing` 0..5, `levitate:rotor`. Recipe: shaft / andesite casing /
  propeller.
- `nozzle`: state `levitate:facing` 0..5 (the clicked face). Recipe: ` S ` / ` C ` / `SSS`
  (andesite alloy, wool).
- The crafting recipes need U6/U9/U10 items (cogwheel, shaft, andesite casing, propeller, andesite
  alloy) and wait in `assets/pending_recipes/bulk/`.

### 2.9 Basin processing (U14)

`basin`, `mechanical_press`, `mechanical_mixer`, `blaze_burner` (states `blaze` none/smouldering/
fading/kindled/seething), `empty_blaze_burner`, `lit_blaze_burner`.

Bedrock blocks (details in §3.6.14):
- `basin`: `levitate:facing` 0 (no spout) / 2..5 (spout side, Bedrock face). Drops itself and its contents.
- `mechanical_press`: `levitate:axis` 0 (x) / 2 (z) (Create's horizontal facing; only the axis is
  visible), `levitate:rotor` 0/1 (kinetic rotor state, §3.1).
- `mechanical_mixer`: `levitate:rotor` 0/1.
- `blaze_burner`: `levitate:blaze` 1..4 = smouldering, fading, kindled, seething, `levitate:facing`
  2..5 (where the blaze looks), `levitate:rotor` 0/1 (client mod: head drawn separately). Create's
  heat level *none* (no blaze) is the separate block `empty_blaze_burner` (`levitate:facing`), which
  is also the item Create calls "Empty Blaze Burner"; both drop themselves.
- `lit_blaze_burner`: `levitate:facing`, `levitate:flame_type` 0 regular / 1 soul (read by U13 for haunting); drops the empty burner.
- Recipes (Create 6.0.10): basin = andesite alloy `A A`/`AAA`; mechanical press = shaft / andesite
  casing / iron block; mechanical mixer = cogwheel / andesite casing / whisk; empty blaze burner =
  ` I `/`INI`/` I ` (iron sheet, netherrack). They need other units' items and wait in
  `assets/pending_recipes/basin/`.

### 2.10 Assembly (U15)

`deployer`, `mechanical_arm`. Create 6.0.10's sequenced assembly intermediates are
`incomplete_precision_mechanism` and `unprocessed_obsidian_sheet` (U9, with their recipes) and
`incomplete_track` (U5); U15 owns the engine that advances them (§3.6.15).

- `deployer`: states `levitate:facing` 0..5 (down, up, north, south, west, east; faces away from the
  player who places it, towards them when sneaking), `levitate:axis_along_first` 0/1 (Create's
  AXIS_ALONG_FIRST_COORDINATE: the shaft axis; horizontal deployers take a vertical shaft when one is
  above / below and no horizontal one at the sides, vertical ones the shaft offered at the sides or
  the player's horizontal look axis), `levitate:rotor` (client mod: 1 = drawn without its shaft,
  pole and hand). Breaking drops the held item and the items waiting to be handed out.
- `mechanical_arm`: states `levitate:ceiling` (placed against the bottom of a block), `levitate:rotor`.
  Shaft input from below (above on the ceiling). Breaking drops the held item.
- Recipes (Create 6.0.10): deployer = electron tube / andesite casing / brass hand (column);
  mechanical arm = `LLI` / `L  ` / `RC ` (L brass sheet, I electron tube, R precision mechanism,
  C brass casing). Their ingredients are U9 / U10 items: the files wait in
  `assets/pending_recipes/assembly/`.

### 2.11 Crafting tools (U16)

`mechanical_crafter`, `mechanical_saw`, `mechanical_drill` (behaviour in §3.6 "Crafting tools").

- `mechanical_crafter`: states `levitate:facing` 0..5 (the front; only the horizontal values 2..5 are used — Bedrock reads integer states as indices into their value list, so the list starts at 0), `levitate:pointing`
  0..3 (Create's Pointing in its order: up, left, down, right; the arrow on the front),
  `levitate:covered` (a slot cover is on). Full block, brass casing look, the arrow is drawn on
  the front's frame. Kinetic: a small cog turning about the facing axis with shaft ends on the
  front and back (crafter grids mesh like cogs), impact 2.
- `mechanical_saw`: states `levitate:facing` 0..5, `levitate:axis_along_first` (Create's
  AXIS_ALONG_FIRST_COORDINATE), `levitate:rotor` (client mod). A 12 px casing with the blade
  sticking out of the front. Kinetic: shaft along DirectionalAxisKineticBlock's axis, impact 4.
- `mechanical_drill`: states `levitate:facing` 0..5, `levitate:rotor`. A 10 px casing with the
  drill head in front. Kinetic: shaft on the back only, impact 4.
- All three: hardness ~ 2.5 s to mine by hand ⚠, explosion resistance 6, drop themselves. Recipes
  (Create 6.0.10): crafter ×3 = electron tube / brass casing / crafting table (column); saw =
  iron sheet, iron sheet + iron ingot + iron sheet, andesite casing (" A ", "AIA", " C "); drill =
  the same with andesite alloy. Their ingredients are U9 / U10 items: the recipes wait in
  `assets/pending_recipes/crafter/`.

### 2.12 Fluid transport (U17)

`fluid_pipe`, `glass_fluid_pipe`, `encased_fluid_pipe`, `mechanical_pump`, `fluid_valve`, `smart_fluid_pipe`
(details in §3.6.1).

- `fluid_pipe`: states `levitate:down|up|north|south|west|east` (open faces), `levitate:rim_a` /
  `levitate:rim_b` (3 bits each: the faces drawing a rim, down/up/north and south/west/east),
  `levitate:bracket` (0 none, 1 wooden, 2 metal). Hardness 0.5 ⚠ (Create: copper-like, instant-ish).
- `glass_fluid_pipe` (no item; drops `fluid_pipe`): `levitate:axis` 0..2, `levitate:fluid` (0 empty,
  1 water, 2 lava, 3 honey, 4 chocolate, 5 milk, 6 potion / other, 7 tea: what flows inside),
  `levitate:bracket`.
- `encased_fluid_pipe` (no item; drops `fluid_pipe`, as Create's loot): the six face states.
- `mechanical_pump`: `levitate:facing` 0..5 (Bedrock FacingID: down, up, north, south, west, east; the
  arrow), `levitate:rotor` (kinetic model state).
- `fluid_valve`: `levitate:facing`, `levitate:along_first` (Create's AXIS_ALONG_FIRST_COORDINATE),
  `levitate:enabled` (open), `levitate:rotor`.
- `smart_fluid_pipe`: `levitate:face` (0 floor, 1 ceiling, 2 wall), `levitate:facing` (0 north,
  1 south, 2 west, 3 east).
- Recipes (Create 6.0.10, in `assets/pending_recipes/pipes/` until U6/U9 land their ingredients):
  fluid pipe ×4 = copper sheet, copper ingot, copper sheet (row); mechanical pump = cogwheel + fluid
  pipe (shapeless); fluid valve = iron sheet + fluid pipe (shapeless); smart fluid pipe = brass sheet /
  fluid pipe / electron tube (column).
- `pipes_test_tank` (Levitate test block, not in the menu): an 8-bucket fluid handler used by the
  selftests in place of U18's tanks.

### 2.13 Tanks and steam (U18)

`fluid_tank`, `creative_fluid_tank`, `steam_engine`, `steam_whistle` (+ `steam_whistle_extension`), `powered_shaft` (internal).

- `fluid_tank` / `creative_fluid_tank`: full blocks (copper, hardness as Create's copper machines,
  drop themselves; the fluid inside is lost when its share no longer fits, §3.6.1). States
  `levitate:above`, `levitate:below` (another part continues there: Create's TOP / BOTTOM inverted,
  the vertical connected textures), `levitate:shape` (Create's FluidTankBlock.Shape with window and
  plain swapped so a lone windowed tank is the default state: 0 window, 1 plain, 2 window_nw,
  3 window_sw, 4 window_ne, 5 window_se), `levitate:light` (0..15, Create's luminosity of a tank
  holding a glowing fluid). (Bedrock reads integer states by their index in the declared list, so
  every list is in ascending order and defaults are 0.)
- `steam_engine`: attaches to the face of a tank it is placed on (`minecraft:block_face` from
  `minecraft:placement_position`, `minecraft:cardinal_direction` for the floor / ceiling facing);
  it can only be placed on tanks and pops when its tank goes. Drops itself.
- `steam_whistle`: on the side of a tank (wall, bracket towards the tank) or on its top (floor,
  mouth towards the player who placed it); states `levitate:size` (0 medium (the default),
  1 small, 2 large), `levitate:powered`. `steam_whistle_extension` (no item, drops nothing): `levitate:size`,
  `levitate:shape` (0 single = half, 1 double, 2 double_connected).
- `powered_shaft`: what a shaft (U6's `levitate:shaft`, `levitate:axis`) becomes two blocks in front of
  an engine; states `levitate:axis`, `levitate:rotor` (kinetic rotor model); drops a shaft (given by
  the code to survival players who break it: its loot table can't name U6's item before U6 is in the
  pack, so explosions drop nothing).
- `steam_test_heater` (Levitate test block, no item): `levitate:heat` = Create's heat level 0..4,
  used by selftests and e2e until U14's blaze burners exist.
- Recipes (Create 6.0.10, added with U9's sheets / alloy, `assets/pending_recipes/steam/`): fluid
  tank = copper sheet / barrel / copper sheet (column); steam engine = golden sheet / andesite alloy /
  block of copper (column); steam whistle = golden sheet / copper ingot (column). The creative fluid
  tank has no recipe.

### 2.14 Fluid machines (U19)

`spout`, `item_drain`, `hose_pulley` (states `levitate:facing` 2..5, `levitate:rotor`),
`portable_fluid_interface` (state `levitate:facing` 0..5), fluids `honey`, `chocolate`, `tea`
(builder's tea), potion fluids (by potion type and bottle), `honey_bucket`, `chocolate_bucket`, the
world blocks `honey` / `chocolate` (non-flowing sources, §3.6.19, §8). Fluid ids: `minecraft:water`,
`minecraft:lava`, `minecraft:milk`, `levitate:honey`, `levitate:chocolate`, `levitate:tea`,
`levitate:potion/<regular|splash|lingering>/<potion>` (Java potion names, e.g. `long_swiftness`;
Bedrock's wither potion is `decay`). Recipes: spout = copper casing over dried kelp; item drain = iron
bars over copper casing; hose pulley = copper casing, dried kelp block, copper sheet (column);
portable fluid interface = copper casing + chute (shapeless) — all in `assets/pending_recipes/fluids/`
until their ingredients exist. Filling recipes (`assets/recipes/filling/`): honey bottle, builder's
tea, blaze cake, honeyed apple, sweet roll, chocolate glazed berries, grass block (dirt + 500 mB
water), gunpowder / redstone / glowstone dust (cinder flour + 25 mB potion of harming / strength /
night vision); emptying recipes: honey bottle, builder's tea.

### 2.15 Linear contraptions (U20)

`mechanical_piston`, `sticky_mechanical_piston`, `piston_extension_pole`, `mechanical_piston_head`,
`rope_pulley`, `rope`, `pulley_magnet`, `elevator_pulley`, `elevator_contact`, `gantry_carriage`,
`gantry_shaft`. Mechanics in §3.6.20.

- States: pistons `levitate:facing` 0..5, `levitate:axis_first` (Create's AXIS_ALONG_FIRST_COORDINATE),
  `levitate:state` 0 retracted / 1 moving / 2 extended, `levitate:rotor`; pole `levitate:facing`; head
  `levitate:facing`, `levitate:sticky`; rope pulley `levitate:axis` 0 (x) / 2 (z) (1 unused: Bedrock integer states need contiguous values), `levitate:running`,
  `levitate:rotor`; elevator pulley `levitate:facing` (horizontal values only), `levitate:running`, `levitate:rotor`; elevator
  contact `levitate:facing`, `levitate:calling`, `levitate:powering`, `levitate:powered`; gantry shaft
  `levitate:facing`, `levitate:part` 0 start / 1 middle / 2 end / 3 single, `levitate:powered`,
  `levitate:rotor`; gantry carriage `levitate:facing`, `levitate:axis_first`, `levitate:rotor`.
- Drops: the head drops a pole; rope and magnet drop nothing; the elevator contact drops a
  `redstone_contact` (U23) and has no item of its own (it is a redstone contact converted by an elevator).
- Recipes (Create 6.0.10): sticky mechanical piston = slime ball over mechanical piston (shipped);
  mechanical piston = wooden slab / andesite casing / extension pole; 8 extension poles = plank /
  andesite alloy / plank; rope pulley = andesite casing / wool / iron sheet; elevator pulley = brass
  casing / dried kelp block / iron sheet; gantry carriage = wooden slab / andesite casing / cogwheel;
  8 gantry shafts = andesite alloy / redstone / andesite alloy (in `assets/pending_recipes/pistons/`
  until U9 / U10 / U6 land their ingredients).

### 2.16 Rotating contraptions (U21)

`mechanical_bearing`, `windmill_bearing`, `clockwork_bearing`, `sail_frame`, `white_sail` +
`<colour>_sail`, `cart_assembler`, `minecart_coupling` (item), `controller_rail`, `turntable`,
`minecart_contraption` (item; listed under §2.25 too, made by U21 as the pickup of a cart contraption).
Mechanics in §3.6.25.

- Bearings: states `levitate:facing` 0..5 (down, up, north, south, west, east: the turning face) and
  `levitate:rotor` (kinetic render state, §3.1). Placed as Create's DirectionalKineticBlock: facing
  away from the one adjacent block with a shaft end towards it (two on different axes: none), else
  towards the player (sneaking: away). The shaft end is the back face. Names: Mechanical Bearing,
  Windmill Bearing, Clockwork Bearing; they drop themselves.
- Sails: `sail_frame` ("Windmill Sail Frame") and 16 sails (`white_sail` "Windmill Sail",
  `<colour>_sail` "<Colour> Sail"), state `levitate:facing` (placed facing the player, sneaking:
  away), a thin panel (collision as the panel), flammable.
- `turntable`: kinetic, shaft end at the bottom, `levitate:rotor`.
- `cart_assembler`: states `levitate:rail_axis` (0 north-south, 1 east-west), `levitate:rail_type`
  (0 regular, 1 powered, 2 detector, 3 activator, 4 controller: the rail it was placed on),
  `levitate:powered`, `levitate:backwards`. Only placed by using the item on a straight, flat rail
  (anything else: "Place your Cart Assembler on a rail block"); breaking it drops itself and its
  rail (not in creative). Backwards when the placing player looks south or east (Create: the
  player's motion direction is positive); on a controller rail it keeps the rail's direction.
- `controller_rail`: states `levitate:rail_axis`, `levitate:power` 0..15, `levitate:backwards`; runs
  along the player's view, backwards (driving south / east) when placed looking south or east.
- Recipes (Create 6.0.10; ⚠ = not verified): sail = sail frame + white wool ⚠ (shapeless),
  `<colour>_sail` = sail + dye (both in the BP now). Waiting for U6 / U9 / U10 ingredients
  (`assets/pending_recipes/bearings/`): mechanical bearing (wooden slab / andesite casing / shaft),
  windmill bearing (wooden slab / stone / shaft), clockwork bearing (wooden slab / electron tube,
  brass casing, electron tube / shaft), turntable (wooden slab / shaft), sail frame ×8 ⚠ (sticks
  around an andesite alloy), cart assembler (andesite alloy, redstone, andesite alloy / log, -, log),
  controller rail ×6 (gold ingots around a stick, electron tube at the bottom), minecart coupling
  (andesite alloys on a diagonal with an iron sheet).

### 2.17 Contraption support (U4)

`super_glue` (item), `linear_chassis`, `secondary_linear_chassis`, `radial_chassis`, `sticker`.

- `super_glue`: tool, stack 1, 99 uses (one use per glued region; creative uses none). Right-click a
  block: first corner ("Click again to confirm"); right-click the second corner: the box is glued if
  every side is at most `maxSuperGlueRange` (24) blocks ("Selected area is too big") and the corners
  are connected through solid blocks inside the box ("Selected blocks must be connected"); sneak +
  right-click discards ("Selection discarded"). Hitting a glued block while holding glue removes
  its regions. Players holding glue see the regions (particle outlines, §8).
- `linear_chassis` / `secondary_linear_chassis`: states `levitate:axis` 0..2 (x, y, z; from the clicked
  face when placed), `levitate:sticky_top` (positive axis end), `levitate:sticky_bottom`. Slime ball or
  super glue on a non-sticky end makes it sticky; on a sticky end makes both ends sticky; sneak +
  empty hand on a sticky end clears it (no item is used, as Create). Range 1..`maxChassisRange`,
  default `maxChassisRange / 2` = 8, shared by the connected chassis group (U3's value box; until then
  `/create contraption chassis <x y z> <range>`). The secondary chassis connects only to secondary
  chassis (shapeless crafting turns one into the other).
- `radial_chassis`: `levitate:axis`, sticky sides `levitate:sticky_north|south|east|west` (for axis y
  the world sides; axis x: north, south, up, down; axis z: up, down, east, west).
- `sticker`: states `levitate:facing` 0..5 (down, up, north, south, west, east; placed facing the
  player, sneaking: away), `levitate:extended` = powered by redstone (polled every 4 ticks). Extended,
  it holds the block in front on a contraption.
- Recipes (Create 6.0.10): super glue = andesite alloy, slime ball / iron sheet, andesite alloy;
  linear chassis ×3 = andesite alloy / log / andesite alloy; radial chassis ×3 = andesite alloy, log,
  andesite alloy; sticker = andesite alloy, slime ball, andesite alloy / cobblestone, redstone,
  cobblestone. They need U9's andesite alloy and iron sheet and are added with U9; the shapeless
  linear ↔ secondary conversions ship now.

### 2.18 Contraption actors (U22)

`mechanical_harvester`, `mechanical_plough`, `mechanical_roller`, `portable_storage_interface`,
`contraption_controls`, `<colour>_seat` (16, Create's dye order: white, orange, magenta, light_blue,
yellow, lime, pink, gray, light_gray, cyan, purple, blue, brown, green, red, black). Behaviour in §3.6.22.

- States: `levitate:facing` 0..5 (Bedrock facing order, the front). Harvester, plough and roller
  (Create's AttachedActorBlock): placed on a side face they face away from it (on another of
  these actors: its facing), placed on a top / bottom face they face the player; they hang on the
  block behind them (moved with it). Harvester and roller also have `levitate:anim` 0..3 (the
  animation step the client mod shows on contraptions; always 0 in the world). Portable storage
  interface: faces the player (the nearest look direction, 6 ways; sneaking: away), plus
  `levitate:extended` (shown while connected). Contraption controls: faces the player
  (horizontal), plus `levitate:disabled` (the indicator). Seats: no states, 8 px high.
- Drops: themselves. Mining time 2.5 s (actors), 1 s (seats).
- Recipes (Create 6.0.10): harvester = andesite alloy, iron sheet, andesite alloy ×2 rows / _
  andesite casing _; plough = iron sheet ×3 / andesite alloy ×3 / _ andesite casing _; roller =
  electron tube / andesite casing / crushing wheel; portable storage interface = andesite casing +
  chute (shapeless); contraption controls = button / andesite casing / electron tube; seat =
  `<colour>_wool` + any wooden slab (shapeless, in the BP). The actor recipes need other units'
  items and wait in `assets/pending_recipes/actors/`.

### 2.19 Redstone (U23)

`redstone_link`, `analog_lever`, `pulse_repeater`, `pulse_extender`, `pulse_timer`, `powered_latch`,
`powered_toggle_latch`, `content_observer` (Smart Observer), `stockpile_switch` (Threshold Switch),
`redstone_contact`. Create 6.0.10 registers the smart observer and the threshold switch under their
old paths `content_observer` / `stockpile_switch`, so those are our ids. Every block drops itself.
Behaviour, states and numbers: §3.6.23.

Recipes (Create 6.0.10, `c:stones` = stone, `c:rods/wooden` = stick): powered latch = redstone
torch / redstone, lever, redstone / 3 stone; powered toggle latch = redstone torch / lever / 3
stone (both in the BP). Waiting for U9/U10 items in `assets/pending_recipes/redstone/`: analog
lever = stick / andesite casing; pulse repeater = redstone, brass sheet, redstone torch / 3 stone;
pulse extender = _, redstone torch / redstone, brass sheet, redstone torch / 3 stone; pulse timer =
amethyst shard, brass sheet, redstone torch / 3 stone; redstone contact ×2 = iron sheet / cobble,
redstone, cobble / 3 cobble; redstone link ×2 = transmitter / andesite casing; smart observer =
electron tube / brass casing / observer; threshold switch = electron tube / brass casing /
comparator.

### 2.20 Display (U24)

`display_link`, `display_board`, `nixie_tube` + `<colour>_nixie_tube`, `clipboard`.

- `display_link`: states `minecraft:block_face` (the face it was placed on; it reads the block
  behind it) and `levitate:powered`. Hardness 1.5 (soft metal), drops itself. The item first selects
  a target (right-click any block: "Targeted position selected"), sneak-right-click clears it
  ("Cleared position selection"); the next placement binds the link to it when the target is closer
  than `displayLinkRange` (64) blocks ("Successfully bound to targeted position", else "Targeted
  position is too far from here"). Right-click (not sneaking, not the wrench) opens the
  configuration screen; without a valid target: "Link has no valid target, try placing it again".
- `display_board`: states `levitate:facing` (Java 2D facing of the front: 0 S, 1 W, 2 N, 3 E; placed
  towards the player, or with the facing of a board it is placed against unless sneaking),
  `levitate:up` / `levitate:down` (connected rows). Kinetic: a small cog turning about its facing
  axis with shafts on both ends, no stress impact; it shows text only at medium speed or more
  (Create's `getMinimumRequiredSpeedLevel` = MEDIUM, ≥ 30 RPM); flaps change at once above 128 RPM.
  A board item used on a board places the next block beside it (Create's placement helper).
- `nixie_tube` (orange) and the 15 other `<colour>_nixie_tube`s: states `levitate:facing` (0..3) and
  `levitate:face` (0 floor, 1 ceiling, 2 wall, 3 wall reversed). Light level 5, every colour drops the
  orange tube; only the orange one is in the creative inventory.
- `clipboard`: item (stack 1) and block (states `levitate:face` 0 floor / 1 wall / 2 ceiling,
  `levitate:facing`, `levitate:written`). Sneak-right-click places it; a broken clipboard drops
  itself with its pages (creative: only when written); sneak-right-click on a placed one picks it up.
- Recipes (Create 6.0.10): display link = transmitter / brass casing; display board ×2 = andesite
  alloy, electron tube, andesite alloy; nixie tube ×4 = two electron tubes (shapeless); clipboard =
  andesite alloy / paper / planks. They need U9's and U10's items and wait in
  `assets/pending_recipes/display/`. Shapeless clipboard → clipboard clears it (ships now: the crafted
  item carries no pages).

### 2.21 Packages (U25)

`packager`, `repackager`, `package_frogport`, `chain_conveyor`, `<colour>_postbox` (16 dye colours,
white … black; Create's postboxes are dyed, not wooden), the package items `cardboard_package_12x12`,
`cardboard_package_10x12`, `cardboard_package_10x8`, `cardboard_package_12x10` ("Cardboard Package")
and `rare_<creeper|darcy|evan|jinx|kryppers|simi|starlotte|thunder|up|vector>_package` ("Rare
Package"), `package_filter`. `filter` + `attribute_filter` are U3. Mechanics in §3.6.25.

- Packages stack to 1; the package filter to 64. Packager / re-packager / frogport / chain conveyor /
  postboxes drop themselves; a broken packager drops its held and queued packages, a port its 18
  packages, a chain conveyor its packages and (not in creative) the chains of its connections.
- Recipes (Create 6.0.10): `packager` = iron ingot top / iron, cardboard block, iron / redstone, iron,
  redstone; `packager` ↔ `repackager` (shapeless conversions); `chain_conveyor` ×2 = andesite casing
  in a plus around a large cogwheel; `package_frogport` = slime ball / item vault / andesite alloy;
  `<colour>_postbox` = dye / barrel / andesite alloy, and any postbox + dye → that colour;
  `package_filter` = zinc nugget, wool, zinc nugget; `package_filter` alone clears its address. The
  ones with other units' ingredients wait in `assets/pending_recipes/packages/`.

### 2.22 Stock network (U26)

`stock_link`, `stock_ticker`, `factory_gauge`, `redstone_requester`, `blaze_burner` shopkeeper role on
`<colour>_table_cloth`, `shopping_list` (item).

### 2.23 Trains (U5 tracks, U27 trains, U28 signals)

U5: `track`, curved track (no separate id: stored in BlockData + graph), `track_station` is U27.
U27: `track_station`, `small_bogey`, `large_bogey`, `controls`, `schedule` (item), `railway_casing` is
U10, `train_door` is U10. U28: `track_signal`, `track_observer`.

### 2.24 Schematics (U29)

`schematic_table`, `schematicannon`, `empty_schematic`, `schematic`, `schematic_and_quill`,
`crafting_blueprint`.

### 2.25 Equipment (U31)

`<colour>_toolbox` (16), `copper_backtank`, `netherite_backtank`, `copper_diving_helmet`,
`copper_diving_boots`, `netherite_diving_helmet`, `netherite_diving_boots`, `extendo_grip`,
`potato_cannon`, `wand_of_symmetry`, `handheld_worldshaper`, `linked_controller`, `sand_paper`,
`red_sand_paper`, `tree_fertilizer`, `lectern_controller` (block, no item). `minecart_contraption`
is U21's (it is made by its cart assembler). Mechanics in §3.6.31.

- States: toolboxes, backtank blocks and the lectern controller `levitate:facing` 0..3 (Java 2D
  order: south, west, north, east; the block faces the player). Toolboxes and backtanks are block
  items that replace their block item (`minecraft:block_placer` `replace_block_item`) so the item
  can stack to 1 / be worn. Their loot tables are empty: the unit drops them itself with their
  contents (toolbox inventory, backtank air). The lectern controller drops a lectern (+ the
  controller from its data).
- Items (Create 6.0.10 values): toolboxes stack 1; backtanks chest armour, protection 4 (copper) /
  8 (netherite), the item bar (durability 900) shows the air and never breaks; diving helmet
  protection 2 / 3, durability 77 / 407; diving boots 1 / 3, durability 91 / 481 (repaired with
  copper / netherite ingots); extendo grip durability 200; potato cannon 100; sand papers 8 (32-tick
  "eat" use); tree fertilizer stacks 64. Armour toughness and knockback resistance of the netherite
  pieces are not expressible (§8).
- Recipes (Create 6.0.10): toolbox dyeing (any toolbox + a dye, 16 shapeless recipes),
  copper diving helmet (copper ingots around glass), sand paper (paper + sand), red sand paper
  (paper + red sand), tree fertilizer ×2 (2 small flowers + a coral + bone meal; one recipe per
  flower × coral, §8), netherite helmet / boots / backtank (smithing: upgrade template + the copper
  piece + a netherite ingot, or + the netherite piece + the copper piece), brown toolbox (cogwheel /
  golden sheet, chest, golden sheet / leather), copper backtank (andesite alloy, shaft, andesite
  alloy / copper ingot, copper block, copper ingot / copper ingot), copper diving boots (copper
  ingots, andesite alloy) and linked controller (oak buttons around a redstone link; Create takes any
  wooden button) ship in the BP. Extendo grip, potato cannon and wand of
  symmetry are mechanical crafting recipes (`assets/recipes/mechanical_crafting/`, U16); the
  worldshaper has none (creative only). `sandpaper_polishing/rose_quartz` (U15's deployer uses it).

### 2.26 Filters (U3)

`filter` ("List Filter": 18 ghost slots in 2 rows of 9, Allow-List / Deny-List, Respect Data /
Ignore Data, entries may be filters themselves), `attribute_filter` (reference item + attribute
list, Allow-List (Any) / Allow-List (All) / Deny-List; up to 8 attributes ⚠). Both stack to 64
(stacks with different contents don't merge). Right-click opens the filter screen (§4); the lore
shows Create's summary (mode + the first entries). Recipes: `filter` = iron nugget, wool, iron
nugget in a row; `attribute_filter` = brass nugget, wool, brass nugget ⚠ (lands with U9's
`brass_nugget`). Contents live in the item's user data (`lv:filter`, `lv:attribute_filter`).

## 3. Mechanics

### 3.1 Kinetic network (U2)

Code: `src/core/kinetics/` (solver, pure C++, gtest), `src/feature/kinetics/` (world adapter, API,
command, sync), `src/client/kinetics/` (renderer). Contract header: `feature/kinetics/Kinetics.h`.

- **Kinetic blocks** are registered per block id with a `KineticSpec`
  (`LV_REGISTER_KINETIC_BLOCK(id, spec)` / `kinetics::registerBlock`): the integer block states it
  reads (`states`), rotation axis, shaft ends per face (`hasShaftTowards`), small / large cog
  teeth, generator flag, stress defaults and Create's special-case hooks (`axisModifier` for
  directional shaft halves, `propagate` = `propagateRotationTo`, `customConnection`,
  `conveyedSpeed` for the speed controller, `extraNeighbours` = `addPropagationLocations`).
  Builders: `specs::shaft`, `specs::cogwheel`, `specs::generatorShaft`, `specs::gearboxModifier`,
  `specs::splitShaftModifier`. The network follows the world by itself: any change of a
  registered block or of one of its listed states (players, commands, pistons, explosions, other
  units) is applied within the tick; generators call `setGeneratedSpeed`.
- **Connections and ratios** (Create's `RotationPropagator.getRotationSpeedModifier`):
  shaft ends facing each other: ×1 (times the halves' `axisModifier`); small cog ↔ small cog,
  same axis, adjacent sideways: ×−1; large cog → small cog diagonal in the large cog's plane, same
  axis: ×−2 (small → large ×−½); large ↔ large with perpendicular axes, diagonal in the plane of both
  axes: ×±1 (−1 when the offsets along the two axes have different signs); cogs stacked along their
  axis connect through their shaft ends (×1). Small cogs reach the 12 diagonals in their plane,
  large cogs all 12 diagonals. Gearbox halves: the face opposite the source turns the other way,
  perpendicular faces turn the same way when their direction sign differs from the source face's
  (Create's `getAxisModifier`). Gearshift: the far half reverses when powered; clutch: the far half
  disconnects when powered. Chain drives, adjustable chain gearshift, speed controller and belts
  are U6/U11 specs using the hooks.
- **Propagation** (Create's `propagateNewSource` / `propagateMissingSource`): a placed block takes
  its speed from the first powered neighbour (faces in Create's `Direction` order, then diagonals);
  a faster source of the same direction overpowers the slower tree (an overpowered generator turns
  at the network's speed, stays in it and still adds its capacity; when the faster source goes it
  restarts on the next tick); a removed block stops everything it powered, which then re-attaches
  to any other powered neighbour. Every block is re-validated every `kineticValidationFrequency`
  (60) ticks (source gone or stopped → stop).
- **Breaking** (the block pops as its item, Create's `destroyBlock(pos, true)`): two sources
  forcing opposite directions into one block; a ratio that would exceed `maxRotationSpeed`
  (256 RPM; speeds are never clamped); a loop of ratios that would speed itself up (same network,
  conveyed speed above the existing one); a block whose speed changed direction / started / stopped
  too often (flicker score: +5 per change, −1 per tick, breaks above 128). One log line per break.
- **Speed levels** (`speedLevelOf`): none (< 1), slow (≥ 1), medium (≥ `mediumSpeed` 30), fast
  (≥ `fastSpeed` 100).
- **Stress**: a network (named after the generator that started it) uses Σ impact·|speed| SU and
  provides Σ capacity·|generated speed| SU of its generators (driven ones included). When usage >
  capacity (and `disableStress` is off) the whole network is **overstressed**: every member reports
  speed 0 (`speedAt`; `theoreticalSpeedAt` keeps the real value), smoke rises from every member,
  a cloud puff when it recovers; one log line per change. Usage levels for goggles/gauges
  (`stressLevel`): > 1 overstressed, > .75 high, > .5 medium, else low. Impact / capacity per block
  come from the stress table (§5) or a per-block override (`setStressImpact` / `setStressCapacity`).
- **Persistence**: the network state (block, states, speed, source, network, generated speed,
  overrides) is saved per chunk in the world DB under `lv:kinetics:c:<dim>:<cx>:<cz>` (until U3's
  `BlockData` exists) and restored when the world loads, then re-attached as Create's block entities
  do on load. A slow world check re-reads blocks that changed without an event.
- **API for machines**: `speedAt`, `theoreticalSpeedAt`, `nodeAt`, `networkAt`, `networkMembers`,
  `isOverstressed`, `stressImpactOf` / `stressCapacityOf`, `onSpeedChanged(blockId, fn)` (batched
  once per tick), `onBreak(fn)`.
- **Command**: `/create kinetics <pos>` dumps the block and its network; `/create kinetics <pos>
  speed <rpm>` (Levitate debug) sets a generator's speed.
- **Client** (client mod only): a kinetic block with a `rotorState` (int state, e.g.
  `levitate:rotor`) has three models: `0` the full static model (what the world holds and vanilla
  clients see), `1` its non-rotating parts (the client mod tessellates this in the chunk mesh), `2..`
  its rotating parts (`spec.parts`, default one part around the rotation axis). The client mod draws
  each rotating part every frame at Create's angle `(ticks + partialTicks)·speed·0.3 + offset`
  degrees, where offset is 22.5° when the sum of the block's coordinates off the axis is even, else
  0 (11.25 for large cogs), plus the spec's `angleOffset` — so meshing cogs interlock.
- **Speed sync** (client mod only): a minimal channel over Bedrock's `ScriptMessagePacket`, isolated
  in `KineticsSync.cpp` / `client/kinetics/` for U3's companion protocol to absorb: the client says
  hello (`levitate:kinetics/hello`, protocol 1), the server acks and sends a full sync of the
  player's dimension (again after each dimension change), then once per tick the changed speeds per
  chunk (`core/kinetics/SyncCodec.h`).
- **Not yet**: Create's ambient cog/kinetic sound scapes and the coloured rotation-indicator
  particles need original sound / particle assets: U8 (goggles, overlays) or a later U2 PR.
### 3.2 Block data, capabilities, filters, value boxes (U3)

Headers in `src/feature/common/` (pure logic in `src/core/common/`, unit-tested).

- **`BlockData`** (`BlockData.h`): one JSON object per (dim, pos), shared by units through
  top-level keys (`data["kinetics"]`, `data["filter"]`, ...). `get/edit/set/erase/has`,
  `forEachInChunk`, `forEach`. Saved per chunk in the world DB (`lv:common:bd:<dim>:<cx>:<cz>`,
  JSON keyed `"x,y,z"`), loaded lazily, written on every world save. Erased automatically when the
  block at the position changes to another block *type* (break, replace, explosion, vanilla
  piston); state changes keep it. `onErase(fn)` lets owners drop contents first. Contraptions (U4)
  move it with `take(pos)` (before removing the block) and `put(pos, doc)`.
- **Item capability** (`Capabilities.h`, `items::`): handlers per block id (`registerBlock`) or
  generic providers, with `slots`, `getSlot`, `insert(stack, simulate) -> remainder`,
  `extract(predicate, amount, simulate) -> stack`; `face` is the accessed side (Create's
  `Direction side`). Vanilla adapters: every container block actor (chests incl. double chests,
  barrels, shulker boxes, hoppers, dispensers, droppers, brewing stands, jukeboxes, crafters ...)
  with the container's own push/pull rules; furnaces / smokers / blast furnaces sided as hoppers
  (top → input, sides → fuel, bottom ← result); composters (compostables in from the top, one
  per insertion, bone meal out at the bottom when ready). Insertion merges into matching stacks
  first, then fills empty slots, in slot order.
- **Fluid capability** (`fluids::`): `FluidStack {fluid, amount mB}`, handlers with `tanks`,
  `fill(stack, simulate) -> accepted`, `drain(request, simulate) -> drained` (request fluid ""
  = any). 1 bucket = 1000 mB, bottle = 250 mB. Cauldrons hold one bucket of water, lava or powder
  snow and only move whole buckets (as NeoForge's cauldron wrapper). `fluidInItem` knows water /
  lava / powder snow / milk buckets and water bottles; units add theirs (`registerFluidItem`).
- **Heat** (`heat::`): `level(region, pos)` → Create's HeatLevel none / smouldering / fading /
  kindled / seething. A registered provider wins (U14's blaze burner); otherwise Create's
  `#create:passive_boiler_heaters` (fire, soul fire, lava, magma block, lit campfires) give
  smouldering. Recipes' heat condition (Create's HeatCondition): heated needs at least fading,
  superheated needs seething; passive heat is not enough for heated recipes.
- **Filters** (`Filter.h`): `matches(filter, stack, matchData)`, `matchesFluid`, `predicate()`
  with Create's FilterItemStack semantics: an empty filter slot matches everything; a plain item
  matches the same item (data ignored unless matching data); a list filter matches if any entry
  matches (entries recursively, with its Respect Data flag), Deny-List inverts, an *empty* list
  filter only matches list filters; attribute filters combine attribute tests (inverted ones
  negate) as Allow-List (Any) / (All) / Deny-List, an empty one only matches attribute filters;
  attribute filters never match fluids. Attribute types (Create's, same ids and wording):
  placeable, consumable, fluid_container, enchanted, max_enchanted, renamed, damaged,
  badly_damaged (> 75 %), not_stackable, equipable, furnace_fuel, washable (splashing recipe),
  hauntable (haunting recipe), crushable (crushing or milling recipe), smeltable / smokable /
  blastable (vanilla furnace / smoker / blast furnace recipe), compostable, in_tag (Bedrock item
  tags), in_item_group (creative group), added_by (namespace), has_enchant, shulker_fill_level,
  has_fluid, has_name, book_author, book_copy. Units add more with `registerAttribute`.
- **Filter slots** (`ValueBox.h`, `filterslot::`): `registerSlot(blockId, {id, centre, scale,
  onChanged})`; the stack lives in `data["filter"][id]`. Right-click on the slot with an item sets
  a copy (filter items are taken from the hand in survival and given back when replaced),
  right-click with an empty hand clears it (returning a filter item). The client mod shows the
  slot's content in the hover label (below).
- **Value boxes** (`valuebox::`): `registerBox(blockId, {id, label, board, centre, scale, get,
  set, format})`. Create 6 edits values on a *value settings board*: rows (e.g. the creative
  motor's ⟳ / ⟲) × columns 0..max with milestone ticks (`core/common/ValueSettings.h`: plain,
  signed and option boards). A box is hit when the click is within `scale / 2` (default 0.25)
  blocks of its centre (Create's ValueBoxTransform.testHit). Client mod: right-click with the
  wrench or an empty hand opens the board (pointer picks row and value, wheel ±1, Shift+wheel
  jumps milestones, click confirms, right-click / Esc cancels); looking at a box or filter slot
  shows its label and value next to the crosshair. Vanilla: the same right-click opens a form
  (row dropdown + value slider; small boards as a step slider of the formatted values). Edits are
  validated server side (reach 8 blocks, value clamped to the board).
- **Recipe loader** (`Recipes.h`): `recipes::ofType("pressing")` / `loadType` reads
  `<mod>/recipes/<type>/**.json` (copied from `assets/recipes/` at build time) in Create's 1.20 and
  1.21 spellings: item ingredients (item / tag / alternatives / `count`), fluid ingredients
  (fluid or fluid tag + amount), item results with `count` and `chance` (each item rolled, as
  Create), fluid results, `heat_requirement`, `processing_time`; `create:` ids become `levitate:`.
  Tags resolve through `assets/recipes/tags/*.json` fragments (one per unit) and then Bedrock item
  tags. Broken files are skipped with a warning. `/create reload` reloads them.

### 3.3 Client protocol and screens (U3)

- **Companion protocol** (`src/feature/net/Net.h`): two LeviLamina custom packets (client → server,
  server → client) carrying a channel name (`<unit>.<name>`) and a JSON payload. On joining a
  level the client mod sends `net.hello {protocol, mod}` (repeated every 2 s, 15 times, until
  answered); the server answers `net.welcome {protocol, accepted}` and marks the player as a
  *companion* (log `companion joined: <name> (protocol N, ...)`). The server never sends to
  non-companions, so vanilla clients never receive our packets; messages from non-companions
  are ignored. Protocol version 1; the server accepts `kMinProtocolVersion..kProtocolVersion`.
  Units use `net::on(channel, fn)` / `net::send(player, channel, json)` on the server and
  `net::client::on` / `net::client::send` on the client. Typed channels so far:
  `screen.open/update/close/action`, `valuebox.open/set`, `common.hover`.
- **Screens** (server side `src/feature/common/Screens.h`): `screens::registerType({id, data,
  action, form})`, `screens::open(player, type, context)` → native screen for companions
  (`screen.open {type, context, data}`), the form for vanilla players. Native actions come back
  as `screen.action`; the server re-validates the context (e.g. the filter is still in that hand
  slot), runs the action and answers `screen.update` with fresh data (or `screen.close`).
- **Native screen framework** (client, `src/client/screen/Screen.h`): a `Screen` with widgets
  (labels, Create icon buttons, ghost item slots, scroll inputs, tooltips) on a Create-style
  bevelled panel, optionally above the player's inventory (36 slots, clicking picks a ghost copy
  onto the cursor; Shift-click sends it straight to the screen). The framework pushes a vanilla
  host scene so the game frees the mouse, draws the screen instead of the host, routes mouse and
  keyboard input, and closes with Esc or the inventory key. `registerServerScreen(type, factory)`
  binds server-opened types.
- **Filter screens** (first users): list filter — 18 ghost slots (click with a ghost item sets,
  click with an empty cursor / right-click clears), Allow-List / Deny-List and Respect Data /
  Ignore Data buttons with Create's tooltips, reset. Attribute filter — reference slot, attribute
  selector (scroll), "Add attribute to List" / "Add opposite attribute to List", the three modes,
  reset, the selected attributes. Forms: one custom form per filter (a dropdown per slot listing
  the inventory's items, mode, data); the attribute filter's form sets the mode / resets and then
  offers the attributes of a chosen inventory item.

### 3.4 Contraptions (U4)

- **Assembly**: an anchor (bearing, piston, pulley, gantry, cart assembler, train station) asks the
  engine to collect blocks: attached blocks per Create's rules (chassis ranges and sticky faces,
  super glue regions, blocks that hang/attach like torches, doors, beds, banners), up to
  `maxBlocksMoved` 2048 ⚠ and `maxChassisRange` 16 ⚠. Unmovable blocks (bedrock, obsidian ⚠ per Create,
  portals, ...) stop assembly with the reason shown.
- **Moving**: the structure becomes a server entity carrying block states + BlockData; it moves
  (translation / rotation about an axis / following a cart or train) each tick; entities standing
  on it are carried; players collide with its blocks (server collision boxes); seats can be ridden.
- **Actors**: blocks on a contraption may act while moving (`contraption::registerActor`): drills
  break, saws cut, harvesters harvest, ploughs plough, rollers pave, deployers use, PSIs transfer,
  controls toggle actors.
- **Storage**: inventories and tanks on a contraption are merged into contraption storage, usable by
  actors and the PSI.
- **Disassembly**: placing back aligned to the grid (rounding as Create), blocks that can't be placed
  drop as items.
- **Super glue**: drag-select a box (two clicks) with super glue to glue a region (max 24 ⚠ blocks
  in each axis); glued regions render as faint outlines with the client mod; removed by hitting with
  glue / breaking.
- **Chassis**: linear (sticky faces via slime ball, range by scroll value), secondary linear, radial
  (range radius); stickers stick the block in front when powered.
- **Client**: the client mod renders contraption blocks at their interpolated transform, including
  rotating kinetic parts and actors' animations.

Implementation (U4, `src/feature/contraption/Contraption.h` is the contract):

- **API for anchors**: `assemble(AssembleOptions{anchor, start, forced, axis, pivot, kind,
  includeAnchor}) -> AssembleResult{id | errorKey, errorText}`; `find(id)` → `Contraption&` with
  `setTransform(t)` (anchors that compute their position each tick: bearings, pistons),
  `setMotion({velocity, angularVelocity, follow, ticks})` (velocity per tick, follow an actor),
  `stop()`, `storage()`, `anchorData()`, `seat()`; `disassemble(id)`; `all()`. Ids are persisted:
  keep the id in your BlockData, never the pointer. Transform: local block coordinates relative to
  the anchor; `world = origin + offset + pivot + R(local - pivot)`, R = rotation by `angle` degrees
  about `axis` (right-hand rule). `/create contraption assemble|move|rotate|stop|disassemble|list|
  demo` is the test anchor.
- **Assembly rules** (Create's `Contraption#moveBlock`, pure code in `src/core/contraption/Assembly`):
  BFS from the start positions; a neighbour joins when both are in one glue region, when it is
  attached towards the block (torches, ladders, signs, banners, buttons, levers, lanterns, bells,
  rails, carpets, redstone, pressure plates, doors, beds, extended stickers), when the block is slime
  or honey and both can stick (not brittle, not glazed terracotta, supportive, slime ≠ honey), or in
  the forced direction. Chassis add their connected chassis (same kind and axis; radial: along the
  axis) and their included positions (linear: up to the range along each sticky end, stopping at
  air, brittle or non-supportive blocks; radial: a disc of radius `range + 0.5` from each sticky
  side). Doors, beds and double chests move whole. Blocks are moved when they are not air, not
  replaceable and have a collision shape or a block entity (brittle blocks and fence gates always).
  Unmovable: unbreakable blocks (destroy time −1: bedrock, barrier, command/structure blocks, end
  portal frame…) and `unmovableBlocks` (portals, piston parts, moving blocks); obsidian is movable as in
  Create. Errors (world untouched): `create.gui.assembly.exception.unmovableBlock` (a forced
  neighbour or a start block), `.chunkNotLoaded`, `.structureTooLarge` (> `maxBlocksMoved`).
- **Carried data**: block NBT (`name` + states) and block entity NBT (chests keep their pair);
  `registerDataCarrier({name, capture, restore, rotate})` moves other units' per-block data (U3's
  BlockData plugs in here; chassis ranges use it). Merged storage: vanilla chests, trapped chests,
  barrels, shulker boxes, hoppers, dispensers and droppers (`Storage::insertItem/extractItem/slot`);
  tanks through `registerFluidStorage(blockId, {read, write})` on the carried data.
- **Movement each tick**: the anchor's transform or the motion; actors (`registerActor(blockId,
  MovementBehaviour{startMoving, tick, visitNewPosition, stopMoving, isActive})`, Create's
  MovementBehaviour; an actor may `stall` the contraption); entities standing on it are carried
  (the point under their feet follows the transform, rotation included); players and mobs are pushed
  out of its blocks (also while it stands still: the blocks are gone from the world, the contraption
  is the floor); seated entities (`registerSeat(blockId, height)`, entities in a seat block at assembly
  or `seat()`) follow their seat, players leave by sneaking. Unloaded anchor chunk: frozen.
- **Disassembly**: angle rounded to the nearest quarter turn, offset to whole blocks; block states
  rotated (`facing_direction`, `minecraft:facing_direction`, `minecraft:block_face`,
  `minecraft:cardinal_direction`, `pillar_axis`, `direction`, `weirdo_direction`,
  `ground_sign_direction`, `torch_facing_direction`, `rail_direction`, `minecraft:vertical_half` /
  `upside_down_bit` on half turns, `levitate:facing`, `levitate:axis`, chassis sticky sides;
  `registerRotationHandler` for others); solid blocks first, then brittle ones bottom-up; a block whose
  place is occupied drops as an item (with its container's contents) and the world block stays; glue
  regions are placed back transformed. One log line per assembly / disassembly / dropped block.
- **Display**: each block is a vanilla falling-block entity (never falls, never lands; orphans from a
  saved world vanish) at the block's rotated center showing the block rotated by the nearest quarter
  turn; its own rotation carries the axis (pitch) and the remaining angle (yaw), so the client mod
  turns it by the exact angle about its center. Persistence: `lv:contraption:c:<id>` (blocks,
  transform, motion, glue, seats, anchor data), `lv:contraption:glue`, `lv:contraption:chassis`,
  `lv:contraption:stickers`.

### 3.5 Tracks (U5)

Checked against Create 6.0.10 (TrackBlock, TrackShape, TrackPlacement, TrackPaver,
BezierConnection, TrackPropagator, TrackGraph, Navigation). Pure maths in `src/core/tracks/`
(gtest), game side in `src/feature/tracks/`, curve drawing in `src/client/tracks/`.

**Block `levitate:track`.** States `levitate:shape` + 16 · `levitate:shape_hi` = Create's
TrackShape (0 none, 1 ZO, 2 XO, 3 PD, 4 ND, 5–8 ascending N/S/E/W, 9–12 portal N/S/E/W, 13 CR_O,
14 CR_D, 15 CR_PDX, 16 CR_PDZ, 17 CR_NDX, 18 CR_NDZ; Bedrock allows 16 values per state) and
`levitate:turn` (Create's HAS_BE: curves start here). Hardness 0.8, metal sound, drops itself,
collision 2 px (none on slopes), can't stand on another track.
- **Placing** (Create's getStateForPlacement): the shape whose axis is closest to the horizontal
  look (straight or diagonal; yaw when looking straight up/down). A straight track next to a block
  with a sturdy top (and no sturdy bottom above it) along its axis ascends towards it (the negative
  side wins).
- **Crossings**: a straight run laid over an existing track overlays it: ZO+XO → CR_O, PD+ND →
  CR_D, XO/ZO + PD/ND → CR_PDX/CR_PDZ/CR_NDX/CR_NDZ.

**Connecting with the track item** (Create's TrackBlockItem / TrackPlacement):
- Right-click a track: selects the end facing the look direction ("Place track or select a second
  point", item frame sound). Junctions can't be selected (`create.track.junction_start`).
- Right-click a second track (or a free spot: a track is placed there with the placement shape):
  the connection is planned with Create's rules and either built or refused with its message
  (`create.track.*`: `second_point`, `too_far` (> `maxTrackPlacementLength` = 32, config
  `config/tracks.json`), `original_missing`, `perpendicular`, `ascending_s_curve`, `too_sharp`,
  `too_steep`, `slope_turn`, `opposing_slopes`, `leave_slope_ascending`, `leave_slope_descending`,
  `turn_90`), with the deny sound. Rules: turns between 60° and 300° of heading change (90° max per
  curve); 90° turns need a radius ≥ 7, 45° turns ≥ 3.25, plus room for the height change
  (90°: +2 per block above 3, 45°: +1.5 per block above 1.5); S-bends need a forward run of 3 (or
  twice the side offset) and must be flat; straight slopes need 4 blocks per block of height
  (3 from 4 blocks up, at least 6); curves leave/enter slopes only straight. Without Sprint the
  curve is standardised (smallest turn, straight track fills the rest); with Sprint held it takes the
  whole space (Create's "maximized turn").
- Cost (survival): one track per straight block placed + `(segments + 1) / 2` for the curve
  (segments = ⌊length · 2⌋), taken from every slot but the selected one, then the selected one,
  then the off hand (`not_enough_tracks`). A block item in the off hand paves under the tracks
  (Create's TrackPaver: slabs half-step on curves, walls and girders along the sides only;
  `not_enough_pavement`); a metal girder (`levitate:metal_girder`, U10) in the off hand builds the
  curve with girders (twice the track cost in girders).
- Sneak + right-click (on a block or in the air) clears the selection ("Selection Cleared").

**Curves** are stored in the world DB (`lv:tracks:curves:<dim>:<cx>:<cz>`, by the chunk of their
first block) as Create's BezierConnection (end blocks, start points, axes, normals, girder): handles
`radius · 4/3 · tan(angle/4)` (S-bends: the side offset), length from 16 straight pieces. The blocks
the curve passes through (Create's rasterise) hold `levitate:fake_track` (no collision, replaceable,
breakable: breaking it destroys the curve). Breaking either end track removes its curves, dropping
one track per two segments (and two girders each when built with girders) unless in creative or
`doTileDrops` is off; `tracks::dismantleCurves` gives them to a player instead (wrench, U8).

**Graph** (Create's TrackGraph): nodes at track ends, junctions, curve ends, portal crossings and
on the 16-block grid of long straight runs (TrackNodeLocation: x/z on a half-block grid, y per
block); edges straight or curved (both directions stored, curves oriented from the edge's first
node); graphs are the connected components and may span dimensions. Updated incrementally on
every track change (Create's TrackPropagator: remove reachable nodes, rebuild from the first valid
node; splits re-checked). Persisted per dimension in `lv:tracks:graph:<dim>` (nodes with their graph
id, edges with their node1, edge points) so trains can run through unloaded chunks.

**Portal tracks**: a straight track placed against a nether portal along its axis looks up the
linked portal (the game's portal records; overworld ↔ nether at 1:8) and becomes a portal track
(TN/TS/TE/TW) bound to a new portal track on the other side, which must be loaded and free;
otherwise it pops with "Cannot place portal track:" + `missing` / `blocked`. The two are joined by
a zero-length inter-dimensional edge; removing either (or the portal) removes both.

**Contract for U27/U28** (`feature/tracks/Tracks.h`, `core/tracks/Graph.h`): `railways()` (graphs,
nodes, edges, lengths, positions/normals along edges, `canTravelTo`), `locate(dim, pos)`,
`targetAt(pos, facing)` (Create's track targeting), `advance(loc, distance)`, `findPath` (A* over
directed edges, cost = distance + `edgePenalty`, heuristic = distance to the nearest destination;
no U-turns, Create's 7/8 direction rule at junctions), edge points (`addPoint` / `pointsOn` /
`onPointInvalidated`: points survive rebuilds of their edge, owners re-add them when it is gone),
`onGraphChanged`.

**Client**: curves are drawn by the client mod (ties and rails along the curve segments) from
`levitate:tracks` script messages the server sends on join and on every change (see §8).

### 3.6 Feature mechanics

Each wave-2 unit implements the full Create 6.0.10 behaviour of its blocks (§2), including: what
right-click/shift-right-click/wrench do, redstone input and output, comparator output, interaction
with belts/funnels/arms/deployers/pipes where Create has it, processing recipes and timings, sounds and
particles, goggles info, and the client rendering. The unit's PR adds a §3.<unit> subsection here with
the details and numbers it implemented. Short reminders of the less obvious ones:

- **Belts (U11)**: 2–20 blocks long (§3.6.1), connect two shafts in a line or slope (45°), items move at
  speed proportional to RPM, belt carries entities, funnels/tunnels/depots/processing work on belt
  items, belt casing, dye, wrench to adjust, breaking returns belts.
- **Fans (U13)**: push/pull entities and items along up to `fanPushDistance` / `fanPullDistance` 20
  blocks; items in the air stream through water → washing, fire/campfire → smoking, lava → blasting,
  soul fire → haunting (details in "Bulk processing (U13)" below).
- **Blaze burner (U14)**: capture blazes with an empty burner, feed fuel (normal: kindled), blaze
  cake (seething), creative cake; heat levels drive basin recipes and boilers.
- **Steam (U18)**: fluid tank boiler size/water/heat give boiler level 0–18 ⚠; each level drives
  engines at Create's rpm/capacity; whistles pitch by size and height.
- **Packages (U25/U26)**: Create 6.0 logistics: packagers box items with addresses, frogports and
  chain conveyors route them, postboxes receive via trains, stock tickers request by address.
  U25's part is §3.6.25.
- **Trains (U27/U28)**: assembly at a station from bogeys + casing, controls drive manually,
  schedules automate (instructions/conditions as Create), signals split the graph into sections,
  trains brake for red signals, derailment/collision behaviour as Create.
- **Schematics (U29)**: Bedrock `.mcstructure` and Create `.nbt` schematics in `schematics/`;
  placement tool overlays the ghost (client mod), schematicannon prints with gunpowder and materials
  from attached inventories.
- **Ponder (U30)**: hold W over an item to open Ponder (client mod only), index of all scenes,
  animated scenes built from our blocks.

#### 3.6.22 Contraption actors (U22)

Implemented in `src/feature/actors/` (client part `src/client/actors/`); every actor is a
`contraption::registerActor` behaviour (Create's MovementBehaviour). An actor's *active area* is
a point ahead of its centre (rotated with the contraption); whenever that point enters a new
block, the actor *visits* it. Common rules (Create's MovementBehaviour / BlockBreakingMovementBehaviour):

- **Items** an actor collects go into contraption storage when `moveItemsToStorage` (default
  true); the rest drops at the actor with velocity `(motion + (0, 0.5, 0)) · random[0, 0.3)`.
- **Breaking** (plough, roller): a breakable block (not air / liquid / unbreakable) stalls the
  contraption until broken. Speed = `|animation speed| / 500` clamped to [1/128, 16] (roller × 1.5),
  animation speed = `⌊(|motion| · 1000 + 100) / 100⌋ · 100` (blocks per tick); every step adds
  `clamp(⌊speed / hardness⌋, 1, 10 − progress)` to a progress of 10 and waits `⌊hardness / speed⌋`
  ticks, playing the hit sound (0.25 volume); progress only advances while the contraption tries
  to move. Breaking is cancelled when the actor stops.
- **Entities** in a visited block (unless it is a solid full block) are thrown: the motion plus a
  quarter of its length upwards, capped at 4 blocks per tick (players get the motion packet).
  The roller also hurts them: `clamp(6 · |motion|^0.4 + 1, 2, 10)`. Items, display entities and
  seat entities are left alone.
- **Contraption controls** can disable an actor type (below); a disabled actor does nothing.

Actors:

- **Mechanical harvester**: area 0.45 ahead; inactive while moving towards its back. Visited:
  crops (wheat, carrots, potatoes, beetroot, torchflower crop, melon / pumpkin stems, nether wart,
  sweet berry bush, cocoa) are harvested when fully grown (`harvestPartiallyGrown`: any age above 0
  when replanting, berries above 1; cocoa always needs full age) and replanted at age 0 (berry bush
  age 1) when `harvesterReplants`, one seed (the crop's own item) taken from the drops; sugar cane,
  kelp, weeping / twisting / cave vines and bush-type plants (grass, ferns, flowers, saplings,
  roots, fungi, seagrass, dead bush, ...; not mushrooms) are cut to air (water for kelp and
  seagrass); leaves are sheared (each drop with 45 % chance). Cactus, bamboo, melons and pumpkins
  are not harvested (as Create). No stationary effect.
- **Mechanical plough**: area 0.45 ahead; inactive while moving towards its back. Breaks blocks
  without a collision shape (grass, flowers, torches, rails, redstone, snow layers — dropped as with
  an iron shovel) and Create's tracks, never liquids, fake tracks or anything standing on farmland;
  throws entities (no damage); then uses a hoe on the block below each visited block (vanilla:
  grass / dirt / dirt path → farmland, coarse / rooted dirt → dirt, rooted dirt drops hanging roots;
  needs air above). No stationary effect.
- **Mechanical roller**: active only while moving towards its front (not on pulleys); area 0.45
  ahead and 2 blocks down. Settings on its top face (shared along a row of rollers facing the same
  way, up to 99 each side; a placed roller copies its neighbour's): *Paving Material* (filter slot,
  a full-cube block without block entity, not stairs / slabs) and *Roller Mode* (value box):
  *Clear Blocks and Pave* (default), *Straight Fill Below*, *Sloped Fill Below*. Visiting a block:
  hurts and throws entities; in tunnel mode it breaks the blocks with a collision shape 1..2 blocks
  above the visited block (0..2 when the material is in storage; the material itself is kept),
  hardest first, the others at once (drops: nothing from blocks that need a tool, otherwise each
  drop 50 % into storage, the rest is lost); then it paves the visited block's column: layer by
  layer from depth 0 down to `rollerFillDepth` (12), straight = one block, sloped = a diamond of
  radius `⌊(depth + 1) / 2⌋`, tunnel = depth 0 only; a position already holding the material passes,
  a solid non-replaceable one (or a portal) fails, a replaceable one (or leaves) takes one item
  from storage; a layer with a placement stops (falling blocks keep going), an all-failed layer
  below the first stops. After a placement the roller waits 2 ticks (stalled) and fills the same
  column again.
- **Portable storage interface**: a carried PSI's area is 1.85 ahead of its front; a stationary
  PSI there or one block further that faces back at it and is not powered connects (Create's
  timer: connecting sets 14, connected while 4 ≤ timer ≤ `psiTimeout` + 4, a transfer sets
  `psiTimeout` + 4, it counts down every tick, at 0 or when powered it disconnects; the carried PSI
  keeps it alive every tick, 2 ticks after the contraption left it retracts). While connected the
  stationary PSI exposes the contraption's merged inventories through the item capability (funnels,
  chutes, arms ...), and vanilla hoppers next to it work as with a container (one item per 8 ticks
  from a hopper pointing into it, into a hopper below it). A moving contraption is held once the
  carried PSI's active point passes the stationary PSI's centre, until its timer drops to 4
  (`psiTimeout` = 60 ticks after the last transfer). It also connects when the contraption stands
  still. Block state `levitate:extended` shows the connection.
- **Contraption controls**: on a contraption, right-click (empty hand into the air needs the client
  mod; with an item, or when a world block is behind it, any client) toggles the actors its filter
  targets (Create's disabled-actor list; empty filter: all actors): message "All Actors: On/Off" /
  "<item> Actors: On/Off" (action bar), click sound pitch 1.5 on / 0.8 off. Toggling one type
  while everything is off switches every other type off one by one. Stationary: right-click
  toggles it, a change of redstone power sets disabled = powered (polled every 2 ticks); filter
  slot on its top (Targeted Component). Assembled while disabled, its target starts disabled; when
  the contraption stops it remembers whether its target is disabled (indicator). Actors of every
  unit can be disabled (their block item is the type); the controls themselves cannot.
- **Seats**: right-click sits the player (an invisible rideable `levitate:seat` entity, as Create's
  SeatEntity; sneak to leave), or a mob the player leads on a leash within 10 blocks; a mob already
  sitting there is ejected for the player, a player there keeps the seat. A dye of another colour
  recolours it (the dye stays). A living mob (not a shulker / armor stand; hostile ones only with
  `seatHostileMobs`) standing on a free seat sits down (checked every 2 ticks); the seat entity
  disappears when its seat or rider is gone and puts a mob rider half a block up. On a contraption
  the engine carries seated entities (assembled riders stay seated); a moving seat picks up mobs it
  touches, a solid block (or a bottom slab) moving into it puts its rider on top; at disassembly
  riders sit down on the placed seat again. Right-clicking a seat on a contraption sits the player.
- **Client mod**: on moving contraptions the harvester's blades should turn by 50° per block
  travelled (Create: animation speed / 20 per tick) and the roller's drum roll on the ground,
  shown by swapping the display entity's block for the animation step (`levitate:anim`) while it
  is drawn. Not working yet: the display entity's position read in the render hook does not
  follow the drawn movement, so the step stays 0 (follow-up).
- **Client mod**: an empty-hand right-click into the air is sent as `actors.use`, so contraption
  controls and seats on a contraption can be used without an item.
- Config `config/actors.json`, log lines `actors: ...` (harvested / broke / tilled / paved,
  psi connected / disconnected / holds, controls toggled, seat sat down / dyed).
#### 3.6.23 Redstone (U23)

Checked against Create 6.0.10 (RedstoneLinkBlock/BlockEntity, LinkBehaviour, LinkHandler,
RedstoneLinkNetworkHandler, AnalogLeverBlock/BlockEntity, BrassDiodeBlock/BlockEntity,
Pulse{Repeater,Extender,Timer}BlockEntity, PoweredLatchBlock, ToggleLatchBlock, SmartObserverBlock/
BlockEntity, ThresholdSwitchBlock/BlockEntity/Screen, RedstoneContactBlock, ContactMovementBehaviour,
CLogistics). Code: `src/core/redstone/` (state machines, link networks; gtest),
`src/feature/redstone/` (world adapter), `src/client/redstone/` (screen, hover panel).

**Redstone on Bedrock.** Outputs are native: every permutation of our blocks carries
`minecraft:redstone_producer` with its power (`connected_faces` / `strongly_powered_face` per
state), so vanilla dust, repeaters, lamps, pistons read them through the game's circuit system
with no polling (the link receiver and the analog lever emit analog 0..15 via a `levitate:power`
state). Inputs are read from the game's own circuit components around the block once per game
tick (wire = transporter strength, lever / button / redstone block / our producers = producer,
torch = capacitor unless it hangs on the block, repeater / comparator = capacitor only when its
circuit direction faces the block, powered solid block = powered block); that tick is also
Create's block-entity tick. `redstone::signalFrom(region, pos, face)` / `bestNeighborSignal`
(`feature/redstone/Redstone.h`) expose this reader to other units. Blocks are tracked while
loaded (`lv:redstone:blocks`); settings live in BlockData `data["redstone"]`.

- **Redstone link** (`levitate:facing` 0..5 FacingID = clicked face, `receiver`, `powered`,
  `power` 0..15): needs a non-replaceable block behind it (pops off otherwise). Two frequency
  slots on the front (Create's slot centres: upward link (8, 3.01, 10.5) first / (8, 3.01, 5.5)
  second px, sideways links first above, scale .4975): right-click with any item (not the wrench
  or a linked controller, not sneaking) copies it (count 1, never consumed), an empty hand clears;
  a click in the overlap sets both, as Create; item-frame sound (.25, .1). Sneak + empty hand or
  the wrench toggles transmitter / receiver (frequencies kept). Transmitter: strength = max of
  the signal from every side plus undirected wires / powered blocks on every side but the
  attached one (Create's getPower), shown as `powered`. Networks (Create's handler): same
  dimension, same pair of frequencies (item + dye colour), evaluated around the node that
  changed: power = strongest transmitter within `linkRange` (distSqr < 256²) of it, every
  receiver within range of it receives that power; receivers emit it weakly on all sides and
  strongly into the block they hang on. Contract for other units: `feature/redstone/RedstoneLink.h`
  (`link::add/remove/setTransmit/setKey/setListening/move/received/powerAt/anyPower/keyOf`).
- **Analog lever** (`face` floor / wall / ceiling, `facing`, `state` handle 0..15, `power`):
  right-click +1, sneak + empty hand −1 (sneak with an item: nothing), click sound pitch
  `.25 + (state + 5) / 15 · .5`, red dust while on. The output (and neighbours) follow the handle
  15 ticks after the last change (Create's lastChange). Strong power into the attached block.
  Goggles show "Analog Strength: n/15" (client mod hover).
- **Pulse repeater / extender / timer** (`facing` = back / input side, `powered` = input,
  `powering`, `inverted`): the input follows the back signal with vanilla diode scheduling
  (2 ticks). Delay value box on top (8, 2.6, 8): Create's board Ticks / Seconds / Minutes ×
  0..60 (milestone 10), stored 2..72000 ticks, default 2 (timer 20), labels `5t` / `0:07` /
  `3:00`. Repeater: one 2-tick pulse starting delay − 1 ticks after a rising edge, re-armed when
  input and output are off. Extender: on while powered, then `delay` ticks more. Timer: counts
  while unpowered, a pulse (delay 2: every other tick) per period, held at 0 while powered.
  Output 15 into the front (strong) when `powering` xor `inverted`; right-click (not sneaking,
  not the wrench) toggles `inverted` (lever click .3, pitch .6 / .5).
- **Powered latch** (`powered`, `powered_side`, `powering`): back sets, either side resets, 1-tick
  scheduling; a click toggles the output only when both inputs agree. **Powered toggle latch**:
  each rising back edge toggles; a click toggles. Output 15 to the front.
- **Smart observer** (`facing`, `target` wall / floor / ceiling, `powered`): placed facing an
  inventory / tank around it (else away from the player, sneaking: towards). Filter slot on the
  back and top (Create's FilteredDetectorFilterSlot). Every tick: the observed block itself
  matching a non-empty filter → on for 3 ticks; an inventory (U3 item capability) with an item
  passing the filter, or a tank with a passing fluid → on for 6 ticks. Funnels (U12) call
  `observer::onFunnelTransfer` → 4-tick pulse. Emits 15 weakly on every side but the target.
- **Threshold switch** (`facing`, `target`, `level` 0..5, `powered`): every 10 ticks measures the
  target: items (slots × max stack size, filtered count), tanks (capacity, filtered mB) or a
  registered observable (`threshold::registerObservable`); on at or above `onAbove` (default
  128), off at or below `offBelow` (64); the output follows 2 ticks later (inverted if set).
  Right-click (any item but the wrench, not sneak+item) opens the screen: native (Create's
  layout: ≥ / ≤ scroll inputs in items, stacks of 64 or buckets of 1000 mB, item measure, invert
  button, target with "Currently"/range tooltip) for the client mod, a form for vanilla players.
- **Redstone contact** (`facing` 0..5, `powered`): placed facing away from the player (sneaking
  or a facing contact on the clicked block: towards the clicked block); powered while a contact
  (or U20's elevator contact) faces it; emits 15 weakly except into its front. On a contraption
  (U4 actor) it powers the world contact it passes (active area .65 blocks in front, not on the
  first movement) until it moves on / stops.

Config `config/redstone.json`: `linkRange` (256). Log lines: one per player action (placement,
mode toggle, frequency change, lever change, inversion, latch click, threshold settings, a
contact powered by a contraption); circuit-driven state changes are not logged.

#### 3.6.16 Crafting tools (U16)

Checked against Create 6.0.10 (MechanicalCrafterBlock / BlockEntity, RecipeGridHandler,
ConnectedInputHandler, EdgeInteractionHandler, SawBlock / SawBlockEntity, TreeCutter, DrillBlock,
BlockBreakingKineticBlockEntity, BlockBreakingMovementBehaviour). Pure logic in
`src/core/crafter/` (gtest), game side in `src/feature/crafter/`, items drawn by
`src/client/crafter/`. Per-block state in BlockData (`crafter`, `saw`); tracked positions in
`lv:crafter:crafters` / `lv:crafter:machines` (machines also join when the kinetic network sees
them, whatever placed them).

**Mechanical crafter.**
- *Placing* (getStateForPlacement): against another crafter (not sneaking) it copies that
  crafter's facing and its arrow points back at the clicked crafter; otherwise it faces the
  player and, when placed on a face other than its front, its arrow points at the clicked block.
- *Arrow* (getTargetDirection): the face the grid's items leave through; up / down, or the
  viewer's left / right as seen looking at the front. The wrench on the front cycles the arrow
  (up → left → down → right).
- *Slot* (1 item): right-click the front with an item (idle crafter) inserts into the crafter's
  connected input; with an empty hand takes the item back (or the cover off); the same item as
  the slot's also takes it; any item while it works ejects the whole grid. `crafter_slot_cover`
  (U9 item) on an empty front covers the slot (taken in survival; given back when removed).
  Insertion sound: item frame add, volume .25, pitch .5. A covered crafter accepts nothing but
  counts as a filled (empty) cell. The item capability inserts only (one item per slot, idle,
  uncovered), extraction is refused.
- *Connected inputs* (EdgeInteractionHandler, wrench, not sneaking): a click within 3/16 of an
  edge of any face but the front toggles the connection with the neighbour behind that edge
  (same facing, not along the facing axis); sound item frame add .25 / .1. A group shares one
  input filled top row first, then left to right as seen from the front. Disconnecting splits
  the group by adjacency; a removed crafter leaves its group.
- *Starting*: when an item arrives in an idle crafter (and every 20 ticks), if the network turns,
  the chain of crafters linked by arrows (each pointing at the next, same facing) starts when
  every crafter has an item or a cover; a rising redstone edge starts it with at least one item
  (incomplete crafts; Bedrock: power is polled, §8). Loops never start.
- *Phases* (countdown speed = clamp(|RPM|, 4, 250) per tick): crafters without a preceding one
  assemble (1), then export (1000; 0 when they carry only empty cells) into the crafter they
  point at — the grid is merged with the arrow's offset, click sound (pitch 0.5 + cells/16) —
  and wait; a crafter whose predecessors all wait assembles next. The last crafter (no target
  crafter) applies the recipe: the game's crafting table recipes first (grid ≤ 3×3,
  `allowRegularCraftingInCrafter`, firework rockets at most `maxFireworkIngredientsInCrafter`
  items), then Create's `mechanical_crafting` recipes (`assets/recipes/mechanical_crafting/`, up
  to 9×9, `accept_mirrored`). Grid rows read top (highest crafter) to bottom, empty outer rows /
  columns are trimmed. No recipe: every crafter of the chain ejects. Then it crafts (2000; at
  1000 a click at pitch 2 and the craft sound; crit particles in front), and inserts the result
  (plus crafting remainders such as buckets, one cell to the right each) into the block its arrow
  points at (any item handler, belts every tick, others every 20 ticks while items wait);
  nothing there: everything pops out of the front (centre + facing × .75, default pickup delay).
- *Removing* a crafter drops its cover and ejects the whole chain's items (not when a
  contraption moves it).
- Sounds: Create's crafter_click = wooden button click (.25) + trapdoor open (.125); crafter_craft
  = item break (.5, pitch .75) (Bedrock names `random.click`, `random.door_open`, `random.break`).
- Client mod: receives every crafter's state (`crafter.state`) and lays out Create's animation
  (slot item on the front, the grouped grid half a block apart, sliding one block along the arrow
  while exporting, the result growing while crafting); the item icons are drawn with the game's
  GUI item renderer in the block entity pass, which does not show them yet (§8). Every client sees
  the crit particles, the sounds and the result popping out.

**Mechanical saw.**
- *Placing* (DirectionalAxisKineticBlock): faces the player (sneaking: away); vertical saws take
  the blade along the player's horizontal look, horizontal saws prefer a shaft above / below or
  beside them.
- *Processing* (facing up only): one item at a time (`bulkCutting`: whole stacks) from item
  entities resting on it, belts / funnels / arms through the item capability. Recipes: Create's
  `cutting` recipes (`assets/recipes/cutting/`: the generated wood cutting — log / wood →
  stripped (50 ticks), stripped → 6 planks, bamboo block → stripped → 3 planks ⚠, bamboo planks →
  mosaic (20), andesite alloy → 6 shafts (200)) and the game's stonecutter recipes
  (`allowStonecuttingOnSaw`), cutting first; the recipe filter slot (on top, 3 px beside the
  blade; U3 filter slot) keeps the recipes whose first result matches; several recipes cycle per
  item. Time = recipe time (50 for stonecutting) × max(1, count / 5), counted down by
  clamp(|RPM| / 24, 1, 128) per tick; below 5 the results are rolled (once per input item, merged),
  then 20 more; no recipe: 10 ticks, the item passes through. Output: along the item movement
  (±x when not along first, ∓z otherwise; sign of the speed) into a Levitate block that takes
  direct input (belts, depots, saws, crafters…; §8), else ejected (centre + move × .5 + (0, .5, 0),
  motion move × .0625 + (0, .125, 0)).
- *Cutting* (facing sideways): breaks the block in front when sawable (logs, wood, stems,
  leaves, mangrove roots, bamboo, melons, pumpkins, cactus, sugar cane, kelp, chorus plant; never
  saplings), then fells the tree (TreeCutter: the trunk must be freed by the cut; logs / roots
  found upwards in 3×2×3 steps, leaves by increasing leaf distance — computed on Bedrock as the
  vanilla distance through leaves —, mushroom blocks / wart blocks / weeping vines within their
  horizontal reach, attachments: bee nests, cocoa, vines, moss carpets, shroomlights ⚠; bamboo,
  cactus, sugar cane, kelp: everything above; chorus: the connected plant). Felled drops fly
  from their block away from the saw (motion (cut − saw) × distance / 20). It resumes the next
  tick.
- Entities inside the block (but items) are hurt like by the drill.
- Contraption actor: breaks sawable blocks in its path (stalling the contraption), fells their
  trees (not for leaves), drops into the contraption's storage first (`moveItemsToStorage`).

**Mechanical drill.**
- *Placing* (DirectionalKineticBlock): away from a single neighbour with a shaft towards it,
  else facing the player (sneaking: away).
- *Breaking* (both machines): every hit adds clamp((int)(speed / hardness), 1, 10 − progress)
  (speed = |RPM| / 100), then waits (int)(hardness / speed) ticks; at 10 the block breaks with
  its normal drops (no tool) at its centre, then the drill resumes on its next lazy tick
  (10 ticks). Never: air, liquids, unbreakable blocks, Create's non-breakable list. Hit sound
  .25, cracks shown while breaking.
- Entities (not items) inside its block lose min(s/16, 2) + min(s/32, 4) + min(s/64, 4) health
  per hit (s = |RPM|).
- Contraption actor (BlockBreakingMovementBehaviour): visits the block at its centre + facing ×
  .65, hurts entities there (clamp(6·|motion|^0.4 + 1, 2, 10)) and throws them, stalls while
  breaking blocks with collision (no tracks) at speed clamp(|animation speed| / 500, 1/128, 16),
  falling blocks above are broken top-down and re-visited after 10 ticks; inactive while moving
  backwards; drops go to the contraption's storage first.
- Client mod: the blade and the drill head turn with the kinetic renderer (`rotor` bones; the
  drill uses a mirrored bone for negative facings). Vanilla clients see the static blade / head.
#### 3.6.1 Fluid transport (U17)

Checked against Create 6.0.10 (FluidPipeBlock, AxisPipeBlock, GlassFluidPipeBlock, EncasedPipeBlock,
PumpBlock / PumpBlockEntity, FluidValveBlock / FluidValveBlockEntity, SmartFluidPipeBlock /
SmartFluidPipeBlockEntity, FluidPropagator, FluidTransportBehaviour, PipeConnection, FlowSource,
FluidNetwork, OpenEndedPipe, FluidReactions, the open pipe effect handlers, CFluids). Pure model in
`src/core/pipes/` (gtest), world adapter in `src/feature/pipes/`. Fluids are ids (`fluids::FluidStack`):
`minecraft:water|lava|milk|powder_snow`, `levitate:honey|chocolate|tea` (U19), potions
`potion:<Bedrock potion name>`. The **fluid registry** contract (`feature/common/FluidRegistry.h`,
added by this unit) gives each id its name, world source block, colour / render slot, lava interaction
and open-pipe effect; U19 registers its fluids there.

**Connections** (Create's `canConnectTo`, `updateBlockState`): a fluid pipe connects to a neighbour
with a fluid capability on the facing side, a beehive / bee nest or cauldron (Create's vanilla fluid
targets), another fluid pipe (a bracketed one only along its straight axis) or any pipe-like block
open towards it (pump axis, valve / glass / smart pipe axis, open encased face). Placing: every
connectable face opens; a single connection extends straight through; none opens along the look
direction. A neighbour change re-evaluates every face but the one opposite the change; with no
connection left a straight pipe stays, a pipe with brackets never changes. Rims are drawn on open faces
whose neighbour is not a connected fluid pipe; T and cross junctions draw the casing.

**Wrench** (non-sneaking; sneak + wrench is U8's dismantle): on a bracketed pipe removes the bracket
(given back / dropped); on a fluid pipe turns it into a glass pipe along its straight axis (a corner:
the axis nearest the click, not the clicked face's) unless the clicked face lies on that axis; on a glass
pipe makes a regular pipe again (both ends open, then the connection rule); on an encased pipe removes
the casing (the copper casing is not returned: encasing doesn't use it up, as Create); on a pump flips its
facing; on a valve cycles `along_first` when the clicked face lies on its facing axis, else turns its
facing a quarter around the clicked axis keeping the shaft axis when possible ⚠ (Create's default
IWrenchable rotation); on a smart pipe turns it clockwise when the top / bottom is clicked.
**Copper casing** (`levitate:copper_casing`, U10) on a fluid or glass pipe makes an encased pipe with the
same open faces; encased pipes never react to neighbours. **Brackets** (`levitate:metal_bracket`,
`levitate:wooden_bracket`, U10) attach to straight fluid pipes and glass pipes (one is used up in survival,
a replaced bracket comes back), lock the pipe's shape and drop when the pipe is broken.

**Pumps**: a kinetic small cog on its facing axis (no shaft ends; impact 4 su/RPM). Placement:
facing the player (away when sneaking), towards a neighbouring shaft end when one faces it, and towards
a connectable pipe perpendicular to the look when there is one (not when sneaking). The direction of
flow is the arrow (`facing`) and does **not** depend on the rotation direction; the wrench reverses it.
On a speed change of its magnitude the pump wipes the pipes in front and behind and redistributes:
it searches the pipes on each side (breadth first, up to `mechanicalPumpRange` = 16 blocks, the range
end counting as an endpoint), keeps the paths that end at an endpoint (a fluid capability, an open end,
an opposing pump) and adds |RPM| of pressure to every face on them, divided by (faces at that distance
− 1) at branches; behind the pump it is inbound (pull), in front outbound (push). The pump's own faces
get |RPM| in / out every tick. Several pumps add their pressure.

**Flow** (PipeConnection / FluidTransportBehaviour): after a pressure change a pipe waits one tick for
the pumps, flips flows whose pressure reversed on the next and then each face starts a flow where it has
pressure: inbound with what its source offers (open end, fluid handler, the complete outbound flow of the
next pipe), outbound with the fluid that entered the pipe through another face. A flow advances
`1/32 + min(pressure / 128, 1) · 31/32` of the face per tick. Two different fluids entering one pipe
break it (with drops); water + lava leave cobblestone, lava + a fluid with a lava interaction leave that
block (honey → limestone, chocolate → scoria, U9's blocks). Closed valves and smart pipe filters refuse
to pull. **Transfer** (FluidNetwork): an inbound flow from an endpoint searches (16 steps per tick, after
a 2-tick pause on every reset) the complete flows from its face to the endpoints they reach and moves
`max(1, ⌊inbound pressure / 2⌋)` mB per tick from the source to them, split evenly (remainders to the
first), simulated first so nothing is lost. 64 RPM: 32 mB/t; 256 RPM: 128 mB/t.

**Open ends** (OpenEndedPipe, 1000 mB buffer): an open end is a face towards air / a replaceable block /
a cauldron or beehive (not towards a pipe, a pump on that axis, a sturdy face or a fluid handler).
Pulling drains a source block (1000 mB; water keeps its source when two neighbouring water sources
and a solid block or source below would refill it — Java's `waterSourceConversion`, config; lava only
with `lavaSourceConversion`), a full cauldron (1000 mB) or a full beehive (250 mB of honey). Pushing
fills the buffer and places the fluid's source block once it holds 1000 mB (not on a source, not when
`pipesPlaceFluidSourceBlocks` is off; water evaporates in the Nether with the fizz sound); fluids
without a world block (milk, potions, tea, and honey / chocolate until U19 gives them blocks) are used
up 1 mB per transfer while their effect applies. Pouring into another fluid reacts (water on a lava
source: obsidian, on flowing lava: cobblestone; lava on a water source: stone, on flowing water:
cobblestone). Effects on the block in front and one below (two when the pipe faces down): water every
5 ticks puts out burning entities, fire, campfires and candles; lava every 5 ticks sets non-immune
entities on fire for 3 s; milk every 5 ticks clears the effects of living entities; tea every 5 ticks
gives Haste I for 21 ticks (hidden); potions apply the potion's effects on every transfer.

**Valves**: a kinetic shaft on the axis that is neither its facing nor its pipe axis. Positive speed
moves the pointer towards open, zero or negative towards closed, at `min(|RPM| / 320, 1)` per tick; it
opens on reaching 1 and closes on reaching 0 (and stays where it is at 0 RPM). Pipes connect to both
ends whether open or closed. **Smart pipes**: pipe along the facing (floor / ceiling) or vertical (wall);
the filter slot (U3's filter slot, Create's position, scale 0.5) takes any item holding a fluid and a
smart pipe only pulls matching fluids (an empty slot passes everything); a filter change wipes and
redistributes the network.

**Persistence**: pressures, flows and open end buffers in `lv:pipes:model` (Create saves them in the
pipe block entities; the phase restarts after a load), valve pointers alongside, brackets in BlockData
`data["pipes"]["bracket"]`. **Client**: glass pipes show the fluid inside through `levitate:fluid`
(every client, mod or not); pumps' cogs and valves' shafts rotate through the kinetic renderer (U2).
**Command** `/create pipes <pos>` (op): pressures, flows and transfers of a pipe.
### 3.6.25 Packages (U25)

Contract for other units: `src/feature/packages/Packages.h` (package item helpers, address matching,
packager requests and unpacking handlers for U26, postbox access for U27); pure logic in
`src/core/packages/` (`Address.h` globs, `Chain.h` chain geometry / routing table / repacking).

- **Addresses** (Create's `PackageItem.matchAddress`): a blank port / filter address only matches
  blank box addresses; `*` on either side matches everything; equal strings match; otherwise either
  string used as a glob must match the other. Globs (catnip's `Glob.toRegexPattern`): `*`, `?`,
  `[a-z]` / `[!a-z]` classes, `{a,b}` alternatives, `{!a,b}` "not starting with" (after a `*`:
  "not ending with"), `\x` literal; case sensitive; a malformed glob (`[]`, missing `]`, nested or
  unclosed `{`, trailing `\`) matches only the empty string.
- **Package item**: 9 content slots, address, order data (`order_id`, `link_index`,
  `is_final_link`, `fragment_index`, `is_final`, optional order context = Create's
  `PackageOrderWithCrafts` as JSON), all in the item's user data `lv:package`. A new package picks a
  random style: one of the 4 cardboard sizes, or with a 1 in 7500 chance one of the 10 rare styles.
  Lore: `→ address` (gold), the first 3 content lines "Name xN", then "and N more...". Sneak +
  right-click opens it into the inventory ("Package breaks" sound); holding right-click and
  releasing throws it with the bow's power curve (`(t² + 2t) / 3`, t = seconds held, capped at 1,
  thrown only from 0.1; speed 2 × power, snowball sound at volume 0.5, pitch 0.5); right-click on a
  block puts it down there. Packages, shulker boxes and bundles don't fit into packages.
- **Packager** (`levitate:facing` 0–5 = the side the tray faces, the inventory is behind it;
  `levitate:powered`; `levitate:linked`): placed facing away from the first neighbour with an
  inventory in the player's look order (Java's `orderedByNearest`), otherwise towards the player
  (sneaking: away); refused with "Packagers cannot interface with Portable Storage directly" when a
  portable storage interface is behind. A rising redstone edge (polled every tick, see §8) boxes the
  inventory: up to 9 slots stacked as `insertItemStacked` (a non-fitting item such as a package or
  shulker box is packed alone; a package found in the inventory is sent as it is); the address is
  the text of an adjacent sign (non-blank lines of its first non-blank side, joined by spaces; the
  last sign in Down, Up, North, South, West, East order wins). While powered it repeats every 10
  ticks (Create's lazy tick); a 40-tick button cooldown stops a button from sending twice. The
  packager holds one finished package (20-tick tray animation; further packages queue) until a
  frogport above, a funnel / any item capability or a player (right-click) takes it. Packages put in
  (right-click with a package, or the item capability) are unpacked into the inventory when all
  their items fit (unpacking handlers per target block for U14 basins, U16 crafters, creative
  crates). Linked packagers (U26's stock link) ignore redstone; U26 sends requests through
  `attemptToSend` (one package per call, order fragments numbered as Create; busy at 50 / 20 / 10
  queued packages for player / redstone / restock requests).
- **Re-packager**: same block behaviour; unpacking puts the package itself into the inventory; on
  redstone it either takes a non-order package out unchanged or, once every fragment of an order is
  in the inventory (links 0..final, fragments 0..final), merges them into the final packages:
  crafting-pattern packages first (when the order context has crafts), then the ordered stacks in
  order, then the rest, 9 slots per package, all with the order's address and order id, the last one
  carrying the order context.
- **Chain conveyor** (kinetic, vertical axis, shafts up and down, 1 SU per RPM — verified): right-click
  two conveyors with chains to connect them (status messages `create.chain_conveyor.*`; sneak or
  clicking another block clears the selection). Rules: shorter than `maxChainConveyorLength` (32),
  at least 2.5 blocks apart, horizontal distance − 1.5 > 0, slope (|dy| / that distance) ≤ 1, at
  most `maxChainConveyorConnections` (4) each; costs max(round(length / 2.5), 1) chains (survival);
  no conveyor within the 8 neighbours at the same height. Connected conveyors share their rotation
  (ratio 1). Packages loop around the wheel (radius 0.875, 6 px up) at speed / (π · 1.5) · 360 / 360
  degrees per tick and travel along chains at |speed| / 360 blocks per tick; a box leaves the loop
  towards the connection the routing table picks for its address (Create's
  ChainConveyorRoutingTable: ports advertise their filter, entries spread with distance + 1, expire
  after 100 ticks, port entries after 20, `*` ports rank last at distance 1000) and enters the next
  conveyor's loop at the chain's tangent angle + 180 ± 70°. A conveyor carries at most
  `chainConveyorCapacity` (20) boxes (Create counts the loop's boxes plus the chains carrying boxes).
  Reversing the rotation moves boxes on chains to the opposite chain. Breaking a conveyor drops its
  boxes and the chains (not in creative); sneak-wrenching refunds the chains into the inventory.
  With a wrench: right-click a chain to ride it (sneak or let go of the wrench to dismount; on the
  loop the player leaves towards the chain they look along), sneak-right-click a chain to remove it
  (chains refunded). Right-click a chain with a package to hang it there; right-click a hanging box to
  take it.
- **Frogport**: right-click a chain (loop or chain position) with the frogport item to select its
  target, then place it within `packagePortRange` (5) blocks, not below the target ("Can Connect ✔",
  "Too far away", "Cannot connect downward", "No target selected"). It catches boxes whose address
  matches its filter ("Catch packages addressed to...", empty = unaddressed only) when they pass its
  position (the tongue reaches out 4 ticks of travel ahead), unless it is busy or its 18 slots are
  full; every 10 ticks it pushes caught packages into the inventory below (hint "Contains Packages"
  when that fails) and sends packages not addressed to it — from its own slots or the inventory below
  (any package from a packager) — onto the chain, if the conveyor turns and has room. The animation
  takes 10 ticks; a sent box is hung at 0.6. "Only send packages" disables catching. Goggles can be
  put on it (right-click with goggles). Automation (item capability) can insert only packages not
  addressed to the port and extract only packages addressed to it.
- **Postbox**: the same port inventory and address; it targets a train station (right-click the
  station with the postbox, then place it within 5 blocks, below is allowed). `levitate:flag` is up
  while it holds packages, `levitate:open` while its screen is open (barrel sounds). U27's trains use
  `postboxesForStation` / `insertIntoPort` / `extractFromPort`.
- **Package filter**: right-click sets its address (a form); in filter slots it lets packages whose
  address matches through (an empty one behaves like a plain item). Lore `-> address`.
- **Screens**: the port screen (address, send-and-receive / send-only, the 18 package slots: click
  one to take it, click a package in the inventory to put it in) is native for companions with the
  address typed in a form; vanilla clients get one form (address, mode, take a package, put my
  packages in). The package filter's address is a form for everyone (§8).
- **Client mod**: draws both strands of every chain, the boxes hanging on chains (interpolated
  between the 4-tick updates), the frogport's tongue with the box it catches or sends, and the
  packager's tray sliding in and out; the wheel turns as a kinetic rotor.
- **Log lines** (e2e): `packages: packager at <pos> activated by redstone`, `... packed a package
  for '<address>' (<n> stacks)`, `... unpacked a package ...`, `chain conveyors at A and B
  connected`, `frogport at <pos> caught a package for '<address>'`, `... sent a package ...`,
  `<player> opened a package (<n> stacks)`, `<player> threw a package ...`.

### 3.6.14 Basin processing (U14)

Checked against Create 6.0.10 (BasinBlock, BasinBlockEntity, BasinInventory, BasinRecipe,
BasinOperatingBlockEntity, MechanicalPressBlockEntity, PressingBehaviour, MechanicalMixerBlockEntity,
BlazeBurnerBlock(Entity), LitBlazeBurnerBlock, BlazeBurnerBlockItem, CRecipes). Pure numbers in
`src/core/basin/` (gtest), game side in `src/feature/basin/`, client animation in `src/client/basin/`.
Contents live in BlockData (`data["basin"]`, `["press"]`, `["mixer"]`, `["burner"]`); the unit's
blocks are listed in `lv:basin:machines` and ticked while loaded.

**Basin.** 9 input slots and 9 output slots (64 per slot, one stack per item kind, the first free
slot otherwise), 2 input and 2 output tanks of 1000 mB with enforced variety (one tank per fluid).
- Item capability (Create's CombinedInvWrapper): slots 0–8 inputs, 9–17 outputs; insertion only
  into the inputs, extraction from any slot (first matching slot, then more of the same kind).
  Fluid capability (CombinedTankWrapper(output, input)): fill only the input tanks, drain output
  tanks first.
- Right-click: an empty hand takes every item (pickup sound, the operator above stops); a filled
  bucket / water bottle pours into the input tanks if the whole amount fits (the container comes
  back empty; creative keeps it); an empty bucket or glass bottle takes a fluid it can hold (bucket
  1000 mB of water / lava / milk / powder snow / honey / chocolate, bottle 250 mB water); a sponge
  drains the first fluid. A bucket never pours next to the basin. Wrench: Create's onWrenched
  (preferred / disabled spout sides). The filter slot (`filter`, on every side 12 px up, U3's
  filter slot) filters recipes by their first output (a fluid output for fluid-only recipes).
- Item entities that fall on / into the basin are taken into the inputs (every tick by default,
  `basinIntakeInterval`).
- Spout (Create's updateSpoutput, every 10 ticks): a side can be output to when the neighbour has no
  collision (or is a funnel) and the block below it accepts items from that side and is one of
  Create's direct belt inputs (belt, depot, weighted ejector, basin, item drain, crushing wheel
  controller, tunnels; through U3's item capability). The last such side in Create's order south,
  west, north, east wins unless disabled; the preferred side wins when possible. When spouting,
  outputs (and the output buffers when the spout opens) go into a spout buffer that is pushed into
  the target every tick (fluids into its fluid capability, else they stay in the output tanks);
  processing waits while the buffer isn't empty. Basins spouting into a basin wake its operator.
- Every 200 ticks a non-empty basin wakes its operator (Create's recipeBackupCheck).
- Breaking drops inputs, outputs and the spout buffer. A basin can't be placed directly under a
  press / mixer and vice versa (the placed block pops).

**Basin recipes** (Create's BasinRecipe.apply): ingredients are taken one item per ingredient from
the combined slots in order; fluid ingredients need a tank with at least the amount; heat
(`heat_requirement`) is tested against the block under the basin (U3's heat capability: heated =
fading or hotter, superheated = seething). Outputs are rolled once (chance), plus containers left
by consumed items (milk / water / lava / powder snow / honey / chocolate buckets → bucket, honey
bottle → glass bottle), and must all fit (spout or output buffers) or nothing happens. Candidates
are sorted by ingredient count, most first; the first one starts.
- Mixer: `mixing` recipes plus every shapeless crafting-table recipe with 2+ ingredients that is not a
  compression (`allowShapelessInMixer`). Press: `compacting` recipes plus every crafting-table recipe
  (shaped or shapeless) with 4 or 9 identical ingredients (`allowShapedSquareInPress`). Recipe ids
  ending in `_manual_only` are ignored. Create's potion brewing in the mixer needs U19's potion
  fluids (`allowBrewingInMixer`, not yet: §8).
- Recipes (`assets/recipes/`, Create 6.0.10 data, non-compat): pressing iron / gold / copper / brass
  ingot → sheets, sugar cane → paper, pulp → cardboard, dirt / coarse dirt / rooted dirt / mycelium /
  podzol and grass block → dirt path; mixing andesite alloy (andesite + iron or zinc nugget), brass
  (copper + zinc → 2, heated), cardboard pulp, chocolate (heated), chocolate melting, dough,
  honey (honey block, heated), lava from cobblestone (50 mB, superheated), mud, builder's tea
  (heated); compacting andesite / diorite / granite from flint (+100 mB lava), blaze cake base,
  bar of chocolate, honey block, ice (9 snow blocks). Tags in `assets/recipes/tags/basin.json`.

**Mechanical press** (8 SU/RPM, horizontal shaft; placed along a neighbouring shaft, else across the
look direction). Cycle of 240 running ticks advancing `lerp(clamp(|speed| / 512), 1, 60)` per
tick; the work happens at tick 120, then the head returns. Head travel: 1 block in the world,
19/16 on belts / depots, 22/16 in a basin; head offset `(t / 240 · 2)³` going down, `(240 − t) / 240 ·
3` going up. Modes:
- *World*: every 10 ticks (moving) it looks for item entities on the ground in the block below
  (deflated by 1/8) with a `pressing` recipe; at the half cycle it presses the first one (Bedrock
  lists entities in another order than Java, so items without a recipe are skipped): a single
  item (or the whole stack with `bulkPressing`) becomes the results, otherwise one item of the
  stack is pressed and the results drop beside it.
- *Belt / depot*: a pressable item on `levitate:depot` / `levitate:belt` two blocks below (U3 item
  capability) starts a cycle; one item is extracted and its results inserted back (left-overs drop).
- *Basin*: Create's BasinOperatingBlockEntity on the basin two blocks below: when the basin changes
  (or the speed changes) and the press is idle and turning, the first matching recipe starts a cycle;
  after the cycle, the same recipe repeats while it still matches.
Sounds: anvil + item break (wool + item break over a belt), pitch `.75 + |speed| / 1024`.

**Mechanical mixer** (4 SU/RPM, a small cog around Y without shaft ends; needs medium speed ≥ 30
RPM). Running ticks 0–40: the whisk lowers until tick 20, waits there for
`max(⌊log2(int(512 / |speed|))⌋ · ⌈processing_time / 100 · 15⌉ + 1, 1)` ticks (processing time
defaults to 100), applies the recipe, and comes back up; it keeps going while the recipe still matches
(Create's continueWithPreviousRecipe). Losing speed reverses the motion. Bubble sound when the basin
holds fluid, a mixing sound while the whisk is down. Head offset 7/16 at rest, up to 1 7/16 blocks;
whisk speed: half the shaft speed idle, ×2 between ticks 15 and 20.

**Blaze burners.**
- Empty blaze burner (item and block): right-click a blaze to capture it (the blaze despawns; flame,
  hurt and fizz effects) or a mob spawner containing blazes (the spawner stays); either gives a blaze
  burner (one empty burner used unless creative). Flint and steel lights it (`lit_blaze_burner`,
  damages the flint and steel); on a lit burner a shovel puts it out (empty again), soul sand / soul
  soil turns the flame to soul fire. Lit burners give smouldering heat (passive heater), light 12 /
  9, comparator 1 / 2 ⚠ (§8).
- Blaze burner: smouldering without fuel (light 8, 15 when burning). Fuel (Create's tryUpdateFuel):
  the blaze cake is special fuel (3200 ticks, seething), any furnace fuel is normal fuel (its furnace
  burn time, kindled; fading when < 125 ticks remain); special fuel can't be replaced by normal fuel;
  the same kind adds up when ≤ 500 ticks remain, else players (not automation) top normal fuel up to
  10000 ticks. When special fuel runs out, 5000 ticks of normal fuel follow. Fuel containers come back
  (lava bucket → bucket). The creative blaze cake isn't consumed and cycles smouldering → kindled →
  seething → smouldering (the burner then stays creative). Goggles on / empty hand off (shown by the
  client mod). Arms / deployers / funnels feed fuel through the item capability without overflow.
  Shopkeeper role: U26.
- Particles and sounds (server side so every client sees them): smoke and flames (soul flames when
  seething) 1 tick in 4, campfire crackle 1 in 10; lit burners smoke and drip lava / soul particles.
- Client mod: the blaze head looks at the player and turns to the burner's facing while heating a
  basin or tank above, bobbing (Create's BlazeBurnerRenderer offsets).

**Client mod** (`basin.machines` messages, 96 blocks): press head and mixer whisk animated with
Create's offsets (the client runs the cycle between messages, waiting at the work point for the
server), blaze heads, the fluid surface in the basin (height `2/16 + 12/16 · (1 − (1 − f)²)`, f =
mB / 2000, the last fluid's colour). Vanilla clients see the static models (head at rest, whisk at
rest, no fluid).

**Command**: `/create basin demo` (op, Levitate debug) builds a mixer, a press and burners in front of
the player for screenshots.
### 3.6.1 Generators (U7)

Checked against Create 6.0.10 (HandCrankBlock(Entity), ValveHandleBlock(Entity), WaterWheelBlock(Entity),
LargeWaterWheelBlock(Item), WaterWheelStructuralBlock, CreativeMotorBlock(Entity)). Pure rules in
`src/core/generators/` (gtest), game side in `src/feature/generators/`, handle animation and the
large wheel's cells in `src/client/generators/`. Speeds are along the positive axis; a facing
converts them as Create's `convertToDirection` (negative facings negate). Capacities (SU per RPM)
come from the stress table (§5): hand crank / valve handles 8, water wheel 32, large water wheel
128, creative motor 16384.

**Placement** (getStateForPlacement). *Preferred side*: the neighbours with a shaft end towards the
new block; sides on two axes cancel out, on one axis the last in Direction order wins.
- Hand crank / valve handles: facing = the clicked face (the handle points away from the block it
  is put on), or away from the preferred side unless sneaking.
- Water wheel (DirectionalKineticBlock): facing = opposite the player's nearest look direction
  (sneaking: the look direction), or away from the preferred side unless sneaking.
- Creative motor: as the water wheel, but with a preferred side (not sneaking) its shaft faces it.
- Large water wheel (RotatedPillarKineticBlock): axis = the preferred axis, else the nearest look
  axis (sneaking with a preferred axis: the clicked face's axis). The wheel and the 8 cells around
  it in its plane must be replaceable; if not and the clicked face is not along the axis, it tries
  one block further along the clicked face; otherwise nothing is placed and the player sees
  `create.large_water_wheel.not_enough_space` ("Remove Blocks for Placement") in the action bar.

**Hand crank.** Right-click (any item; sneaking with an item uses the item instead) turns it for
10 ticks at 32 RPM (`inUse = 10`, refreshed by every click; Bedrock repeats the click while the
button is held, so holding keeps it turning); sneaking turns it backwards (clockwise). Each click
costs `32 · crankHungerMultiplier` (0.01) exhaustion unless the player holds the extendo grip
(`levitate:extendo_grip`, U31) or is in creative. Sound every 10 ticks while in use: Create's
`cranking` (wood place at volume .075·inUse/2.5, wooden button click-off at .025·inUse/2.5, pitch
.5·(.65 + (10 − inUse)/10)). A crank that was turning when the world was saved is stopped on load.

**Valve handles.** Right-click with anything (sneak + wrench is the wrench's) turns the handle by
its angle (value box "Rotation when used", `create.kinetics.valve_handle.rotated_angle`, board
180 with milestones every 45, rows ⟳ (negative) / ⟲, min 1°, default 45° ⟲): it generates
±32 RPM for `ceil(angle / 9.6) + 2` ticks, clockwise when the angle is negative, reversed when
sneaking; it does nothing while its network already turns (theoretical speed ≠ 0), while turning,
or for 4 ticks after a turn. A dye of another colour dyes the handle (the block becomes
`<colour>_valve_handle`, facing kept, the dye is not used; Create's ValveHandleBlock.clicked); its
angle resets to 45° as Create's new block entity. Players who may not build (adventure) can't use
it. The value box sits on the handle's front (4.5 px out from the mounting face); a click with an
empty hand turns the handle, the wrench there opens the board (U3). The handle model turns from
0 to ±90° (angle ≤ 135) or ±180° over the turn's ticks (client mod; vanilla: static model).
Valve handles make no sound (Create's crank sound is only for the hand crank).

**Water wheels.** A wheel samples the fluid flow in the cells around it in its plane: the 4
neighbours of the water wheel, the 12 cells two blocks out from the large wheel's centre (Create's
SMALL_OFFSETS / LARGE_OFFSETS). The flow is the fluid's flow vector (flowing water or lava,
falling liquid points down; also a liquid in the block's second layer) or ±1 along y in a
bubble column (up from soul sand, down over magma); rain does nothing. Each cell whose flow,
projected onto the wheel's plane and normalised, has |dot| > .5 with the wheel's positive tangent
(its offset rotated 90° about the axis) adds the dot's sign. Speed = clamp(score, −1, 1) · 8 / size
(water wheel ±8 RPM, large ±4 RPM). Re-checked when a block in a sampled cell changes and every 60
ticks (Create's lazy tick); one log line when the speed changes. Right-click with planks of another
wood (any `minecraft:*_planks`, sneaking with an item excluded) changes the wheel's wood (both the
static model and the spinning one), with the planks' break particles and sound (level event 2001);
the planks are not used.

**Large water wheel structure** (WaterWheelStructuralBlock). When a large wheel appears (placed,
`/setblock`, contraption), the 8 cells of its plane become `water_wheel_structure` blocks (segment
and axis states, the wheel's wood) on the next tick; a cell taken by a non-replaceable block breaks
the wheel without a drop (Create's LargeWaterWheelBlock.tick). Breaking a structure block breaks
the wheel (it drops a large water wheel unless a creative player broke it); a removed wheel takes
its structure blocks with it. Planks on a structure block change the whole wheel. `extension` is
set when the block behind the wheel (negative axis side) is another large wheel.

**Creative motor.** Generates its value box speed (default 16 RPM, "Generated Speed in RPM",
`create.kinetics.creative_motor.rotation_speed`, board 256 with milestones every 32, rows ⟳
(negative) / ⟲, labels "64⟲"), along its facing. Boxes on the four sides around the shaft (not the
bottom of a horizontal motor), 3.5 px inside the face and 1 px back from the shaft (Create's
MotorValueBox). Turning the motor (wrench, contraption) re-applies the speed.

**Client mod.** The kinetics renderer spins the wheels (bones per wood on one stacked texture),
the motor's and the handles' shafts; the handles themselves are drawn by `client/generators`:
the hand crank's handle chases `speed · 10/3` (a quarter of the gap per tick, angle in radians =
sum / 360, so it visibly turns faster than the shaft as in Create); the valve handle animates the
turn (the server sends `generators.valve` {pos, target, ticks, backwards, facing} to companions in
the dimension) and follows its network when another source drives it. The structure blocks'
cells are hidden for the client mod, which spins the whole large wheel instead.

Config `config/generators.json`: `crankHungerMultiplier` (0.01, Create's CKinetics key).
#### 3.6.1 Rotating contraptions (U21)

Checked against Create 6.0.10 (MechanicalBearingBlockEntity, WindmillBearingBlockEntity,
ClockworkBearingBlockEntity, BearingContraption, ClockworkContraption, TurntableBlock,
CartAssemblerBlock(Entity), MountedContraption, ControllerRailBlock, OrientedContraptionEntity,
CouplingHandler / CouplingPhysics, MinecartContraptionItem). Pure rules in `src/core/bearings/`
(gtest), game side in `src/feature/bearings/`. Every anchor uses the contraption engine (§3.4);
the bearing's state (running, angle, contraption ids, mode) lives in its BlockData (`bearings`).

- **Mechanical bearing** (stress impact 4 SU/RPM): the structure attached to the block in front is
  assembled when the bearing starts turning (Create's onSpeedChanged sets assembleNextTick) or on an
  empty-hand right-click (not sneaking, may build); it doesn't assemble while the bearing stands
  still. Assembly errors (`create.gui.assembly.exception.*`) are shown to the clicking player and
  kept in the BlockData (goggles, U8). It turns about the facing axis by `speed · 0.3`° per tick
  (right-hand rule, the same sense as its shaft; 0 while overstressed; held while an actor stalls
  it). When the speed changes sign the angle is rounded to a whole degree. When the speed drops to 0
  the structure is placed depending on the **Movement Mode** value box (side faces, 2 px towards the
  back): Always Place when Stopped / Only Place near Initial Angle (|angle| < 45° or > 315°) / Only
  Place when Anchor Destroyed. A right-click while running places it at once (rounded to the
  nearest quarter turn); breaking the bearing places it too.
- **Windmill bearing** (generator, capacity 512 SU/RPM): assembles on right-click regardless of
  speed; the structure must contain at least `minimumWindmillSails` (8) sail-like blocks (sails,
  sail frames, wool: Create's `#create:windmill_sails`), else `not_enough_sails`. It generates
  `clamp(sails / windmillSailsPerRPM, 1, 16)` RPM (8 sails per RPM), clockwise (+) or
  counter-clockwise (−) from the **Rotation Direction** box (changing it while running turns it the
  other way at once), and turns its structure at its generated speed. A right-click stops it: the
  structure is turned back to its assembly angle and placed.
- **Clockwork bearing** (impact 4): on right-click it assembles the hour hand (the structure in front)
  and, behind the hour hand's straight run along the axis (up to 16 blocks), a separate minute hand.
  Each tick a hand turns towards its target at most `|speed · 0.3|`° (hour hand: half):
  hours = `(dayTime / 1000 + 6) % 24`, hour target `−360 / 12 · (hours % 12)` (24-hour mode
  `/ 24`), minutes = `(dayTime % 1000) · 60 / 1000`, minute target `−360 / 60 · minutes`, times −1
  for bearings facing a negative direction (clockwise seen from the front). **Clock Hands** box:
  Hour hand first / Minute hand first / 24-Hour hand first. Stopping (speed 0) or a right-click
  places both hands.
- **Turntable** (impact 4): entities standing on it turn: players are carried around its centre by
  `clamp(speed · 0.3, −16, 16)`° per tick (their view doesn't turn); other entities turn on the spot
  by `speed · 0.3`° (yaw).
- **Sails**: a sail (or frame) is attached to its neighbours in its own plane (Create's
  BlockMovementChecks), so a sail structure moves without glue. A sail or frame in hand on a sail
  (not sneaking) places it next to the clicked one in
  the same plane, on the free side nearest to the click, with the same facing (Create's placement
  helper). Dye on a sail: the sail takes the colour; if it already has it, the nearest neighbour of
  the plane that doesn't; if all do, every connected sail of the plane (up to 100 steps). Shears:
  the same with "no colour" (back to sail frames). Dye is not used up.
- **Cart assembler**: Create's action per rail type for a minecart on it: regular — powered:
  assemble, unpowered: disassemble; powered rail — powered: assemble + accelerate (0.4 b/t along the
  motion, away from a redstone conductor at either end), unpowered: disassemble + brake (velocity
  `(centre − cart) / 16`); detector — empty cart: assemble + accelerate, else disassemble; activator
  — powered: disassemble; controller — powered: assemble + accelerate in the assembler's direction,
  unpowered: disassemble + brake. Assembly/disassembly at most once per 8 ticks. Assembling needs an
  empty rideable (or chest) minecart and a block above the assembler: the structure attached to it
  is lifted, anchored at the assembler and rides the cart; a second cart assembler under the
  structure with an empty cart on it makes a two-cart contraption: the carts are coupled (≥ 2 blocks
  apart) and the structure points from the first cart to the second. Disassembly places the
  structure at the assembler turned to the assembler's direction (coupled: both carts must be on
  disassembling assemblers; it keeps its orientation). **Cart Movement Mode** box: Always face toward
  motion (the structure turns towards the cart's motion along the rail by at most
  `|motion| · 12 / max(1, width / 6)`° per tick) / Pause actors while rotating / Lock rotation.
  If the cart breaks, the structure is placed where it is.
- **Controller rail**: a cart on it is driven at `0.4 · power / 15` b/t in the rail's direction when
  powered and not moving against it; otherwise it is braked to the rail's centre.
- **Minecart coupling**: use on a cart, then on another: coupled at their distance (whole blocks,
  at least 2) if ≤ `maxCartCouplingLength` (32), each cart has at most two couplings and they don't
  form a loop (`create.minecart_coupling.*` messages); one coupling item used (not in creative).
  Coupled carts keep their distance (Create's soft collision step: each cart corrected along the
  coupling by the predicted stretch, clamped to 1.75, projected on its rail, speeds ≤ 0.4). Wrench on
  a coupled cart removes its couplings (items dropped, "Removed all couplings from minecart"); a
  broken cart drops its couplings.
- **Minecart contraption** (item, stack 1): wrench right-click on a cart carrying a contraption
  (survival only when `survivalContraptionPickup`) picks it up as `minecart_contraption` with the
  structure; refused for spawners ("A mystical force…") and for structures over 512 KB of data
  ("…too big to pick up"). Using the item on a rail places a minecart with the structure, facing the
  player's view along the rail.
- **Config** `config/bearings.json` (Create's CKinetics keys and defaults): `windmillSailsPerRPM` 8,
  `minimumWindmillSails` 8, `maxCartCouplingLength` 32, `survivalContraptionPickup` true. Stress
  values come from `config/kinetics.json` (mechanical / clockwork bearing 4, turntable 4, windmill
  capacity 512).
- **Client mod**: contraptions turn smoothly through the contraption renderer (§3.4); the bearings'
  top plate and shaft and the turntable's plate are kinetic rotor parts (§3.1; the clockwork's top
  stays still); clock hands are their own contraptions.
- **Log lines**: `<kind> at <pos> assembled contraption #<id> (...)`, `... disassembled`,
  `windmill_bearing at <pos>: <n> sails, <rpm> RPM`, `bearing at <pos> could not assemble: ...`,
  `cart assembler at <pos> assembled contraption #<id> on minecart <uid>`, `cart contraption #<id>
  disassembled at ...`, `minecarts <a> and <b> coupled (<n> blocks)`, `cart contraption #<id> picked
  up by <player>` / `placed at <pos> by <player>`, `sail at <pos> dyed <colour> by <player>`.
### 3.6.20 Linear contraptions (U20)

Checked against Create 6.0.10 (LinearActuatorBlockEntity, MechanicalPiston*, PistonContraption,
PulleyBlock*, PulleyContraption, ElevatorPulleyBlockEntity, ElevatorContraption, ElevatorColumn,
ElevatorContact*, GantryShaft*, GantryCarriage*, GantryContraptionEntity, ContraptionCollider).
Code: `src/core/pistons/` (maths, gtest), `src/feature/pistons/`, `src/client/pistons/` (ropes);
contract addition `src/feature/contraption/PistonsExt.h` (movement checks, dry-run assembly, world
collision of translating structures, terrain pass-through registry for block-breaking actors).
Per-block state lives in BlockData `data["pistons"]` (and travels with contraptions).

**Linear actuators** (pistons, rope pulleys, elevator pulleys; Create's LinearActuatorBlockEntity):
- Speed: `rpm / 512` blocks per tick clamped to ±0.49 (256 RPM → 0.49). Pistons: a positive speed
  extends a piston facing south, west or down and retracts one facing north, east or up (Create's
  movement modifier); the offset stays in 0..extension length. Pulleys: positive speed lowers.
- A speed change (and an empty-hand right-click, not sneaking, not on the value box) schedules
  *assemble next tick*: running + speed 0 → stop (movement mode decides), running + speed → continue,
  stopped + speed → assemble. A reversal while moving snaps the offset like Create
  (`Math.round(offset * 16) / 16`, integer division).
- Each tick: the structure moves by the speed; the leading blocks are tested one step ahead against
  the world (solid, non-replaceable blocks, unloaded chunks; block-breaking actors registered with
  `ext::registerTerrainPassThrough` pass) and against other piston / pulley structures. A collision
  rounds the offset to the grid and stops (pistons that were extending try to assemble again next
  tick). A contraption stalled by an actor doesn't advance the offset.
- End of range / stop: Create's movement mode value box (`create.contraptions.movement_mode`, on the
  piston's side faces off the extension and shaft axes, 2 px towards the back; on the rope pulley's
  top): *Always Place when Stopped* (default) places the blocks; *Place only in Starting Position*
  places only at the initial offset, otherwise keeps the contraption until it returns; *Place only
  when Anchor Destroyed* keeps it until the actuator is broken. Breaking a running actuator always
  places the structure.
- Sounds: assembly = wooden trapdoor open (0.5, 0.5); placement = wooden trapdoor close (0.35, 0.75).

**Mechanical piston**: extension poles in a straight line behind the piston (and, when extended, the
poles and head in front) define the range (`maxPistonPoles` 64; more → `tooManyPistonPoles`; none →
`noPistonPoles`). The pole line becomes part of the contraption: the head (or a pole) stands for the
piston's own block, back poles slide through the piston, and at placement whatever lands on the
piston's block becomes the piston again (retracted when the head is there, else extended). Push line
(Create's addToInitialFrontier): up to `maxChassisRange` blocks in front of the head, stopping at
blocks that don't need moving, brittle blocks (carpets excepted), an opposing piston head or a block
not supportive on that side; an unmovable block in the line stops assembly with `unmovableBlock`.
Retracting: only sticky pistons take the block directly in front (silently skipped if it can't move or
is push-only). Then Create's structure search with the movement direction forced (poles and heads
carry nothing sideways). Assembly is refused silently when the first step would collide or the piston
is already at the end. Moving pistons are unmovable; extended / retracted pistons can be carried.
Slime ball on the front of a retracted normal piston makes it sticky (consumed in survival). Breaking
the piston breaks the pole line in front (to its head) and behind; breaking the head breaks the poles
back to the piston (retracted); breaking a pole between a piston and its head breaks the line (drops
unless creative). Pole / gantry shaft items used on a pole / shaft extend the line at the end nearer
to the click (Create's PoleHelper). Placement: facing away from the player (sneaking: towards),
`axis_first` from neighbouring shafts as Create's DirectionalAxisKineticBlock. Stress impact 4.

**Rope pulley**: rope and magnet blocks below count as the current offset; range `maxRopeLength` 384
(and the world bottom). The structure below the magnet assembles without a forced direction (the rope
column is anchoring); ropes and magnet are removed while running. Without a structure the rope
lowers on its own until a block that needs moving is below, which it then picks up (Create's
visitNewPosition). Placed back: magnet above the structure, ropes up to the pulley (blocks in the way
break, dropping only when they have no collision). Rope pulleys above other top blocks of the
structure mirror the pulley (multi-rope): they hang their own rope down and place it when the main
pulley stops. Breaking a rope or magnet breaks the whole column and resets the pulley; breaking the
pulley breaks the rope below. Ropes and magnets are brittle and unmovable. Stress impact 4.

**Elevator pulley** (Create 6 elevators): right-click with an empty hand assembles / disassembles
(speed only drives the motion). The cabin is the first solid block below; it needs exactly one
horizontal `redstone_contact` (U23) on it (`no_contacts`, `too_many_contacts`); the column is the x/z in
front of that contact, with the contact's facing reversed; an active column refuses a second cabin
(`column_conflict`). Assembly converts the redstone contacts of the column (same x/z, facing back)
into elevator contacts and targets the current floor. Motion: Create's elevator curve (speed limit
`rpm·2/512` ≤ 1.99, acceleration `lerp(|limit|, 0.0075, 0.0175)` per tick, deceleration
`sqrt(2·|diff|·acc)`); arrived within half a block, then the cabin eases onto the floor (a quarter
of the rest per tick). The cabin always stays a contraption (Place only when Anchor Destroyed). On
arrival (and every 3 ticks while it stands there) the floor's contact powers (redstone 15, light 10)
until another floor is called; elevator contacts call the cabin on a rising redstone edge (they show
*calling*), and a contact facing a redstone contact powers too. Floors: right-click a contact (not with
the wrench) to set the floor identifier (4 chars), description (30 chars) and door control (stored for
U10's sliding doors); blank identifiers are numbered from their neighbours (Create's initNames). The
wrench turns a contact; facing away from its column it becomes a redstone contact again.
Contraption controls (U22) integrate through the cabin's `anchorData()["elevator"]`:
`floors` [[contact y, short, long]], `version`, `column`, `contactY`, `target`, `current`; writing
`request` = contact y calls that floor. Contacts keep `data["pistons"]["floor"]` = the identifier of the
current floor for display links (U24). Stress impact 4.

**Gantries**: gantry shafts (`levitate:part` from same-facing neighbours along the axis; placement
facing away, continuing a neighbouring line, sneaking reverses; a line is powered as a whole by
redstone, polled) carry carriages placed on them (facing the clicked face; the carriage breaks
without a shaft behind it). A carriage assembles its structure (the shaft is anchoring) when its
shaft turns and allows it (unpowered; start: speed > 0, end: speed < 0, middle: any; single: never):
pinion speed `clamp(-rpm/512, ±0.49)` along the shaft's facing. It moves until the shaft under it is
gone, powered or stopped, or until it passes the centre of a shaft that wouldn't let it assemble, or
hits the world; then it is placed (grid aligned). A powered shaft drives the carriage as a gear instead
(Create's pinion ratio). Nested gantries work through the world (an inner carriage starts when its
shaft turns after the outer one stops). Stress impact 0 (Create: setNoImpact; the kinetics table in
`config/kinetics.json` still lists `gantry_shaft` 4 and must become 0).

**Client mod**: pistons, pulleys and gantries rotate their shaft / pinion with the kinetic renderer
(`levitate:rotor`); running pulleys draw their rope (elevators: belt) and magnet from the pulley to the
structure (`pistons.rope` messages, every 3 ticks, extrapolated in between); contraption blocks are
drawn by U4's display entities.

#### 3.6.1 Belts, depots and weighted ejectors (U11)

Checked against Create 6.0.10 (BeltBlock, BeltConnectorItem, BeltSlicer, BeltInventory,
BeltMovementHandler, DepotBehaviour, SharedDepotBlockMethods, WeightedEjectorBlockEntity,
EjectorTargetHandler). Pure rules in `src/core/belts/` (gtest), game side in `src/feature/belts/`,
drawing in `src/client/belts/`. Config `config/belts.json` (§5).

- **Belt connector**: right-click a shaft (U6's `levitate:shaft`; U2's test shaft too) selects it,
  right-click a second shaft connects them when (Create's canConnect) both are loaded, closer than
  `maxBeltLength` (20, Euclidean), same axis, in a line or exactly 45 degrees in the plane
  perpendicular to the axis (vertical shafts: straight only), not turning against each other and only
  replaceable blocks or same-axis shafts in between. One connector is used (survival), wool sound.
  Shafts in between become pulleys; vertical shafts make a sideways belt, a vertical line a vertical
  belt. Sneak + use (block or air) clears the selection; any second click clears it. While a shaft
  is selected, particles show the planned belt towards the shaft looked at (green: valid, red: not).
- **Kinetics**: a belt is a kinetic block (stress 0): pulley segments (start, end, pulley) have shaft
  ends on both faces of the rotation axis (the axis 90 degrees from the facing; sideways: y); all
  segments of one belt turn together, sloped ones through their diagonal neighbours.
- **Items** (BeltInventory): speed `rpm / 480` blocks per tick, START -> END when Create's
  direction-aware speed is positive (`rpm * step(facing) * (x axis ? -1 : 1) > 0`); items keep one
  block apart, stop 0.45 before a blocked end (solid block), 0.25 before a block that takes items
  (another belt, depot, ejector, other units' direct inputs) and fall off an open end (thrown along the
  belt at max(speed, 1/8) plus 1/8 up). Vertical and sideways belts carry nothing (items fall off).
  Belt-to-belt: the end inserts into the block after it (for slopes also the block level with the
  last segment) with the movement direction as side; a belt refuses insertion against its movement,
  side insertions start 0.35 off-centre and slide to the middle. Dropped item entities landing on a
  belt are taken whole at their segment (Create's item handler: one stack per segment until it moved
  on). Right-click with an empty hand takes the stacks within 0.55 of that segment's centre.
- **Entities** (BeltMovementHandler): mobs, items and players standing on a moving belt (|rpm| >= 1)
  move at `rpm / 480` along it, are pulled to the middle (not while walking), climb / descend slopes;
  sneaking or flying players are not carried.
- **Right-click on a belt**: dye (any `*_dye`) colours the whole belt, water (bucket, water bottle)
  removes the colour (nothing used, as Create); a shaft on a middle segment makes it a pulley (1 used);
  andesite / brass casing encases the segment (not used, as Create); the belt connector on the outer
  half of an end extends the belt by one (free; the old end loses its shaft) or merges it with a
  compatible belt end there (2 shafts + 1 connector back); the wrench: on the outer half of an end
  shortens the belt (length > 2; a pulley that becomes the end gives its shaft back), elsewhere splits it
  between this segment and its neighbour on the clicked half (both parts >= 2; a shaft from the
  inventory per new end that isn't a pulley yet); on a cased segment's side the casing comes off, on a
  pulley's side its shaft comes out.
- **Breaking** a segment destroys the whole belt: items drop, pulleys become shafts again, the other
  segments vanish; a survival player gets one belt connector plus a shaft if the broken segment had a
  pulley.
- **Depot** (DepotBehaviour): holds one stack (max stack size) and an 8-slot output buffer.
  Right-click gives back the held stack and the outputs, then puts the held hand stack on it (slides from
  0.25 to the centre). Items arrive from belts / funnels / ejectors (DirectBeltInput) only when it is
  empty, slide to the centre (1/4 of the distance per tick), are processed by the block 2 above (Create's
  BeltProcessingBehaviour, unless the block between has a collision box), outputs go to the buffer.
  Item capability: 9 slots (0 held, 1..8 outputs), insert only into an empty depot, extract outputs
  first, the held stack when it isn't being processed. Sounds: Create's depot slide / plop (sand break
  0.125 / 1.5, item frame add 0.25 / 1.25).
- **Weighted ejector**: a depot with a launch plate; kinetic (impact 2 SU/RPM, shaft on the sides).
  Target: sneak + right-click a block with the ejector item ("Target Selected"), then place it: it
  faces the target and throws there if the target is in line with it, not straight above / below and
  within `maxEjectorDistance` (32) ("Ejecting to [x,y,z]"), else ("Target was not Valid") the block in
  front; placed without a selection it faces away from the player. Launches when charged, turning, not
  powered by redstone (polled every 4 ticks), and an entity stands on the plate or the held stack is
  centred and (stack size value box 0 = any) at least the configured size and the targeted block can take
  it. The stack flies on Create's arc (gravity 0.08, drag 0.98 / 0.91 per tick, apex 1 + 0.3 x distance
  blocks above the higher end) and is inserted into the target's belt input or dropped there; entities
  get the same velocity. The plate stays up 2 ticks and recharges at `|rpm| / 256 / distance` per tick.
  Value box (front and back faces): "Ejected Stack Size" 0..64 (`*` = any).
- **Contract** for other units (`src/feature/belts/BeltApi.h`): transported items (Create's
  TransportedItemStack), `handleCenteredProcessing` / `handleProcessingOnAllItems` / `itemsAt` on a
  belt segment or depot, processors 2 above (`registerProcessor`: received / held -> pass / hold /
  remove), blocks on a belt (`registerAboveInteractor`: funnels / tunnels see each passing item and may
  take it or stop it), direct inputs (`registerDirectInput` / `insertDirect`: what belt ends and
  ejectors insert into; `side` = travel direction), `beltAt`. Belts, depots and ejectors are item
  capabilities too.
- **Data**: BlockData `belt` {c: controller, i: index, n: length} per segment, the controller also
  holds `items`; `depot` {held, incoming, outputs, stackSize, target, airborne, ...}; the index of
  controllers / depots / ejectors in `lv:belts:index`.
- **Client mod**: belts are left out of the chunk mesh and drawn every frame from the same geometry:
  the band texture scrolls at the surface speed (1 px steps), pulleys turn at Create's kinetic angle;
  speeds over `belts.query` / `belts.speed`. The ejector's shaft turns through U2's rotor rendering.
#### 3.6.19 Fluid machines (U19)

Code in `src/feature/fluids/` (pure logic `src/core/fluids/`, gtested), client drawing in
`src/client/fluids/`. Machine state lives in `BlockData` under `"fluids"` and travels with
contraptions (data carrier `fluids`). Every machine exposes the U3 fluid capability.

- **Fluid registry** (`feature/fluids/FluidRegistry.h`, Create's GenericItemEmptying /
  GenericItemFilling / FillingBySpout / PotionFluidHandler): buckets ↔ 1000 mB (water, lava, milk,
  honey, chocolate; powder snow is not a fluid, as in Create), bottles ↔ 250 mB (a regular water
  bottle is water; every other potion item is the potion fluid of its type and bottle; honey bottle
  and builder's tea through the emptying / filling recipes and the U3 container registry). Glass
  bottles take water, potion fluids and tea directly; empty buckets take every fluid with a bucket.
  The spout additionally uses filling recipes and sequenced assembly filling steps.
- **Sequenced assembly** (shared with U15): a transitional item carries Levitate item data
  `"sequenced_assembly": {"recipe": "<recipe id>", "step": n, "progress": f}`; an item matching a
  sequenced assembly recipe's `ingredient` starts at step 0. When step `n mod len(sequence)` is a
  `filling` step and the fluid matches, the spout fills it and advances the step; after
  `len × loops` steps one result is picked by weight (Create's rollResult).
- **Spout**: 1000 mB tank, filled from every side but the nozzle (bottom). It processes the item two
  blocks below (the block between must have no collision, funnels excepted: Create's
  BeltProcessingBehaviour). The item is the slot 0 seen from the top through the item capability of
  a **Levitate** block (depots, belts; vanilla containers have no processing behaviour). Filling
  takes 20 ticks (FILLING_TIME); the item is filled 15 ticks in, one item per cycle; the result goes
  back through the capability (leftovers drop). With too little fluid it waits. A cauldron two
  blocks below is filled with a whole bucket (water, lava, powder snow). Sounds: start / fill.
  Logs: `spout at x y z filled <in> into <out>`, `... filled a cauldron with <fluid>`.
- **Item drain**: 1500 mB tank, pipes extract on every side, nothing is inserted from outside.
  Items enter through the item capability (one at a time when they hold fluid, otherwise the whole
  stack), from item entities landing on top, and travel at 1/8 block per tick away from the side
  they came from (from above: down, starting at the centre). At the centre an item that can be
  emptied is held for 20 ticks; 15 ticks in it is emptied if all of its fluid fits (otherwise it
  waits). At the far edge it goes into the next Levitate block (belt, depot, chute, funnel, drain:
  Create's DirectBeltInputBehaviour), or, if that side is not solid, is ejected as an item entity
  (motion 1/8 outwards + 1/8 up); else it waits. Right-click with a fluid container empties it into
  the tank / fills an empty bucket or bottle from it (Create's FluidHelper exchange); right-click
  with anything else takes the held item; block items without fluid are placed. Breaking drops the
  held item. Logs: `item drain at x y z emptied <item> (<n> mB <fluid>)`, `... passed <item> on`,
  `... ejected <item>`, `... exchanged fluid with <player>`.
- **Hose pulley**: kinetic (impact `hose_pulley` 4 SU/RPM), rotation axis = the clockwise neighbour
  of its facing; placed facing the player (sneak: away). The hose moves `speed / 512` blocks per tick
  (positive speed lowers it) through replaceable blocks and fluids and stops on the first solid
  block (whole blocks). Pipes connect on its back (the side opposite its facing). While still, with
  the hose end in a fluid body: draining removes source blocks (and water from waterlogged blocks)
  top layer first, farthest first, only at or above the hose end, within `hosePulleyRange`; filling
  places sources (with `fluidFillPlaceFluidSourceBlocks` off the fluid is voided) lowest first, nearest first, sideways and
  downwards only; water evaporates in the nether. A body of more than `hosePulleyBlockThreshold`
  fluid blocks is infinite: drained without removing blocks when the fluid is bottomless
  (`bottomlessFluidMode` / lists), filled without placing blocks unless `fillInfinite`. Both go
  through a 1500 mB buffer (Create's HosePulleyFluidHandler); draining invalidates the filling
  search and vice versa. Logs: `hose pulley at x y z stopped at <n> blocks`, `... drained <fluid> at
  ...`, `... placed <fluid> at ...`, `... found an infinite <fluid> source`. Client mod: the hose and
  its end piece are drawn, extrapolated between server updates (channel `fluids.sync`).
- **Portable fluid interface**: Create's PSI rules for fluids. A moving interface finds a
  stationary one directly in front of it or with one block between them, facing it and not powered;
  the contraption stalls once it passes the centre of the block where it found it. The stationary
  interface then exposes the contraption's mounted tanks (U4 storage) on every side; it connects
  after 4 ticks of animation and stays connected `psiTimeout` (60) ticks after the last transfer,
  then the contraption moves on (while it is still there the connection stays open; it closes 2
  ticks after it left). Redstone power disables the stationary interface. Logs: `portable fluid
  interface at x y z connected to contraption #n`, `... on contraption #n moves on`, `...
  disconnected`.
- **Honey and chocolate in the world**: see §8. `levitate:honey` / `levitate:chocolate` are
  non-flowing, non-colliding, translucent source blocks (replaceable, not minable, explosion
  resistance 100). Honey / chocolate buckets place them (on the clicked block if it is replaceable,
  else next to the clicked face; survival keeps an empty bucket); an empty bucket picks them up;
  hose pulleys drain and place them like water. Entities inside get Slowness II (hidden, refreshed
  every 5 ticks) and no fall damage (Create's viscous fluids). Lava next to them (not below) turns
  into obsidian (lava source) or limestone (honey) / scoria (chocolate) for flowing lava, as Create's
  fluid interactions (skipped while U9's stones are missing). Log: `<player> placed / picked up
  <fluid> at ...`.
- Test block `levitate:fluids_test_depot` (not in the creative menu): one item slot from the top and
  an 8000 mB tank when mounted on a contraption; it stands in for U11's depot in the selftests.

#### Bulk processing (U13)

Checked against Create 6.0.10 (MillstoneBlock(Entity), CrushingWheelBlock(Entity),
CrushingWheelControllerBlock(Entity), EncasedFanBlock(Entity), AirCurrent, FanProcessing,
AllFanProcessingTypes, NozzleBlock(Entity), CKinetics). Pure maths in `src/core/bulk/` (gtest), game
side in `src/feature/bulk/`. Machine state lives in BlockData `data["bulk"]`; the unit's blocks are
indexed in `lv:bulk:index` and ticked by the unit.

**Millstone** (impact 4 SU/RPM): a small cog (meshes with cogwheels around it) with a shaft below.
One input slot, 9 output slots. Items go in when thrown on top (item entities resting on it) or
through the item capability (any side; only items with a milling recipe, up to 64). While turning it
counts a timer down by `clamp((int) |RPM / 16|, 1, 512)` per tick: the recipe's `processing_time`
(100 without a recipe), then mills one input item (rolled results, stacked into the outputs; what
doesn't fit is lost, as Create); it pauses while an output slot holds 64. Right-click with an empty
hand takes all outputs into the inventory (the inputs when there are no outputs). The capability
extracts only from the outputs. Item crack particles of the input while milling.

**Crushing wheels** (impact 8 SU/RPM, pillar with shaft ends along its axis): two wheels with the
same axis two blocks apart get a `crushing_wheel_controller` between them (when the block is
replaceable); it is valid when both turn and their speeds have opposite signs, facing
`nearest(axis × side · sign(speed) · step(side))` (Create's updateControllers) — the way items move
through the pair. Checked on placement, on speed changes and every 20 ticks; a removed wheel takes
its controllers with it. A wheel can't stand next to a controller on a non-axis side or next to a
wheel that isn't stacked along their common axis (it pops). Crushing speed = |wheel RPM| / 50.
Controller (valid and turning): the first entity entering it is taken (not creative / spectator
players, no players on peaceful, not items it ejected itself); items are pulled through and taken in
once 0.25 past the centre; the inventory (32 slots) counts `processing_time` (crushing recipe, else
milling, else 100) down by `clamp(4 · crushingSpeed / log2(count), 0.25, 20)` per tick (20 for one
item), applies the recipe once per item below 20 (no recipe: the items are destroyed, as Create)
and at 0 outputs: into the Levitate block below / in front when it has an item handler (Create's
DirectBeltInputBehaviour: belts, depots, chutes, ...), else as item entities 0.55 past the centre
with Create's out-speed (0.25 sideways, 0.5 up, 0 down). Other entities get `crushingDamage` (4) per
hit (Bedrock's hurt cooldown applies) and are moved out below the wheels before a lethal hit; their
drops keep no motion. Item capability: insert a whole stack into an empty, unoccupied controller.

**Encased fan** (impact 2 SU/RPM, shaft at the back, `facing` = the side the air leaves through).
Placement (Create's getStateForPlacement): against a chute it faces the chute's way; else the side
whose neighbour has a shaft pointing at it (unless ambiguous) or the look direction, turned to face
away (sneaking: towards). Air current, rebuilt on speed changes, neighbour changes and every
`fanBlockCheckRate` (30) ticks:
- pushing when the speed converted to the facing's direction (negated for down/north/west) is
  positive, else pulling; reach `lerp(min(|RPM| / fanRotationArgmax, 1), 3, fanPushDistance |
  fanPullDistance)` (256, 20, 20);
- the reach stops at the first full collision box, unloaded chunk or partial block that covers the
  5 test points (then `i + depth + 1/32`); air, liquids, non-colliding blocks and
  `#create:fan_transparent` (iron bars, copper grates, mangrove roots, fences, leaves, campfires,
  blaze burners, sail frames, metal bars, copycat base) let it through;
- processing types (a catalyst changes the air from there on in the flow direction; priority
  splashing 400 > haunting 300 > smoking 200 > blasting 100): water / flowing water (also
  waterlogged) → **splashing**; lava → **blasting**; fire, lit campfire, smouldering blaze burner, lit
  blaze burner with a regular flame → **smoking**; soul fire, lit soul campfire, lit blaze burner with
  a soul flame (U14: state `levitate:flame_type` = 1) → **haunting**; blaze burner at fading or more →
  **blasting**;
- entities in the current (searched every 5 ticks; not creative-flying players or spectators) get
  `motion += clamp(flow · a − motion, ±5) / 8` with `a = |RPM| / (512, sneaking 4096) / (distance to
  the fan's centre / reach)`, and no fall damage;
- items in processing air: after `fanProcessingTime · ((count − 1) / 16 + 1) + 1` ticks (150) the
  stack becomes the outputs (the first stays in place with the same motion and is not processed
  again by that air; the rest spawn next to it): splashing / haunting = their recipes rolled per
  item; smoking = the smoker recipe × count; blasting = the furnace (else blast furnace) recipe ×
  count unless the smoker gives the same item (food burns up); anything else that isn't fire
  resistant burns up in blasting air;
- other entities: splashing hurts endermen, snow golems and blazes (2, drowning) and puts out fire;
  smoking sets on fire 2 s and hurts 2; blasting sets on fire 10 s and hurts 4 (not fire immune);
  haunting gives blindness (30 ticks) and slowness II (20 ticks), and turns a horse into a skeleton
  horse after 100 ticks;
- items on `levitate:belt`, `levitate:depot` and `levitate:weighted_ejector` in the current (or one
  below a horizontal one) are processed the same way through their item capability;
- airflow particles (`levitate:fan_airflow`, density `fanParticleDensity` 0.5 per tick) flow along the
  current at `(reach + 1 − distance) / 16` blocks per tick, tinted per air type (Create's colours);
  processing particles at items (smoke, soul flames, poof, splash).
Other units use `feature/common/BulkExt.h` (`airflow::sourceAt`, `typeAt`, `canProcess`, `process`,
`processingTime`), e.g. chutes for fans blowing up into them.

**Nozzle**: placed on the clicked face; survives only in front of a fan facing the same way (else it
pops). Range = the fan's reach (0 when stopped), pushing like the fan. Every 5 ticks it collects
entities within the range it can see; each tick it pushes them `normalize(diff) · (range −
distance) · (1/32, items 1/128)` away (pulling: towards it, except closer than 1.5); sneaking and
creative-flying entities are left alone; poof particles around it. The nozzle blocks the fan's own
current (its shape stops it at once).

**Client**: rotating parts through the kinetic renderer (millstone stone; crushing wheel; fan shaft
at the network speed and propeller at 5×); every block has a static model for vanilla clients.
#### 3.6.1 Fluid tanks, boilers, steam engines and whistles (U18)

Checked against Create 6.0.10 (FluidTankBlock / BlockEntity / Item, ConnectivityHandler,
CreativeFluidTankBlockEntity, BoilerData, BoilerHeaters, SteamEngineBlock / BlockEntity,
PoweredShaftBlock / BlockEntity, WhistleBlock / BlockEntity, WhistleExtenderBlock). Pure maths in
`src/core/steam/` (gtest), game side in `src/feature/steam/`, drawing in `src/client/steam/`.
Query API for the goggles (U8): `feature/steam/Steam.h` (`goggleLines`, `tank`, `comparatorLevel`,
`whistlePitch`).

**Tank multiblocks** (ConnectivityHandler for vertical containers). A tank is a square base of
width 1..3 (`MAX_SIZE` 3) by height 1..`fluidTankMaxHeight` (32) blocks; the controller is the
lowest x, y, z corner and holds the fluid of the whole structure, `fluidTankCapacity` (8) buckets
per block. On placement (the next tick) the new block searches down (unbounded) and 2 blocks
towards −x / −z for tanks; every part that is its own controller tries to become the min corner of
the largest complete structure (each width 1..3, layer by layer upwards, a layer only counts when
complete, existing structures inside must fit inside it, never past the max height, different
fluids never connect; ties prefer the wider one); the largest candidates form first. Absorbed
parts pour their fluid into the new controller (what doesn't fit is lost). Fluid tanks and creative
tanks never connect. Breaking a part splits its structure into 1×1 tanks: the controller (if it
remains) keeps one block's worth, the others get one block's worth each in y, x, z order, the rest
is lost; they re-connect at once. (Create can duplicate the controller's share when a non-controller
part is broken; Levitate doesn't.) A tank item used on the top or bottom face of a structure wider
than 1, not sneaking, fills the whole next layer (all positions free, enough items in survival).

**Look**: vertical connected textures (top / middle / bottom / single pieces), windows by default
(Create's `setWindows`: width 1 every side, width 2 the two outer faces of each column, width 3 the
middle of each side); the wrench (U8's `levitate:wrench`, not sneaking) toggles them for the whole
structure, except on a boiler; becoming a boiler closes them. After a split every part takes the
window setting of the old structure. A glowing fluid (lava, light 15) lights the parts up to its
level with 15 / 1.2 = 12 and the parts above with 1, only with windows.

**Fluids**: fluid capability on every part (the controller's tank): fills with the same fluid or
into an empty tank, drains any or the requested fluid. Comparator level = Create's
`fractionToRedstoneLevel(amount / capacity)` (floor(14·f + (f > 0)), §8). Right-click (not
sneaking) with a filled bucket / water bottle empties it into the tank when it all fits (Create's
`tryEmptyItemIntoBE`; survival: the empty container comes back), with an empty bucket / glass bottle
takes 1000 / 250 mB (bottles only water); creative players keep their item. Units' buckets
(`<ns>:<fluid>_bucket`, registered with `registerFluidItem`) work too. Sound: bucket empty / fill,
volume .5, pitch `clamp(1 − amount / (capacity per block · 16), 0, 1) / 1.5 + .5 ± .125`.
**Creative tank**: always full of its fluid (`setContainedFluid`), drains never empty it, fills are
accepted and voided; a filled bucket sets its fluid, an empty bucket takes some and empties it.
Contraptions carry tanks with their contents (data carrier `steam:tank`, fluid storage for U4's
mounted storage); they re-form when placed back.

**Boiler** (BoilerData). A tank structure with steam engines (facing away from it on any part) or
whistles (attached to any part) is a boiler: its fluid capability takes water only (any amount),
counting it as supply instead of storing it; it can't be drained. Heat from the blocks under the
bottom layer (BoilerHeaters): none (−1), passive 0 (U3's `heat::level` smouldering: fire, soul fire,
lava, magma, lit campfires, smouldering blaze burners), 1 per fading / kindled burner, 2 per
seething; passive only counts without active heat. Water supply: the mB received every 5 ticks / 5,
the best of the last 10 samples (creative tanks: 200 mB/t). Limits: size `min(18, blocks / 4)`,
water `min(18, ceil(supply) / 10)`, heat; **level = min(active heat, water limit, size limit)**
(0..18). Passive: passive heat with size and water limits ≥ 1. Engine efficiency: passive
`1/8 / engines`; no active heat 0; else 1 when engines ≤ level, otherwise level / engines.
Goggles (Create's lines and colours): "Boiler Status: Passive / Idle / Lvl N / Max", the Size,
Water and Heat bars (`|` per level: dark green up to the minimum, green at it, dark green up to the
value, dark red up to the maximum, grey up to the next multiple of 5, max 18), then either "Water
input rate" with "N mB / 10 mB per Tick" (engines, size ok, no water, heat) or "Kinetic Stress
Capacity:" `efficiency · 16 · max(level, engines) · 1024` su "via N engines". Without engines and
whistles: Create's fluid container lines.

**Steam engine** (SteamEngineBlockEntity, each tick). Its shaft is the block two in front of it
(`getShaftPos`); a plain shaft there whose axis differs from the engine's facing axis becomes a
**powered shaft** (Create's `pickCorrectShaftType` / `SteamEngineBlock.onPlace`), which turns back
into a shaft when no engine two blocks away points at it. A powered shaft takes one engine
(`canBePoweredBy`, not in its first 3 ticks). Each tick: efficiency = clamp(boiler efficiency);
direction = 1 when the efficiency is 0 or the shaft is vertical, else +1 for a facing (for floor /
ceiling engines their horizontal facing) towards +x / +y / +z and −1 otherwise, negated for a z
shaft and for the value box's Counter-Clockwise; when the shaft is driven by another source turning
the other way the engine flips its own setting. The powered shaft generates `direction · 16 ·
(1 + (e ≥ 1 ? 3 : min(2, ⌊4e⌋)))` RPM (64 at full, 48 / 32 / 16 below) with a capacity of `e · 1024 /
modifier` su per RPM (= e · 16384 su: a full engine 16384 su, a passive one 2048 su); no tank, no
shaft or efficiency 0: speed 0. Value box "Rotation Direction" (Clockwise / Counter-Clockwise) on the
engine's four side faces. Puffs of steam (sound `levitate.steam.engine`, evaporation particles) twice
per shaft turn while it runs.

**Whistle** (WhistleBlock / BlockEntity). Pitch = 2 semitones per full extension above it, 1 for a
half (single) one at the top (`updatePitch`, up to 25). Using a whistle item on a whistle or an
extension grows the column (Create's `incrementSize`: up to 6 blocks: a half extension becomes full,
else a half extension is added on the next free block; the item is not used up; place sound pitched
2^(−2i/12)). Wrench: the next size (small → medium → large → small) for the whole column; sneaking
with the wrench on the top extension shortens it by half a block. Extensions without a whistle or
extension below vanish. Powered by redstone (polled, §8) on a boiler with heat (passive or active) it
sounds: the `levitate.steam.whistle_chiff` hiss when it starts, then the tone: one original sample
played at `octave · 2^(−pitch/12)` (sounds `whistle_high` / `whistle_medium` / `whistle_low` are the
same sample at pitch 2 / 1 / .5; restarted every 10 ticks, heard up to 64 blocks), steam particles out
of the mouth every 8 ticks. One log line when it starts / stops.

**Client** (client mod): the server sends tank contents (`steam.tanks`: controller, size, fluid,
amount, capacity, windows, boiler) and engines (`steam.engines`: position, facing, shaft speed and
axis) to companions; the client draws the fluid inside windowed tanks as Create's renderer (1/4
caps, a 1/16 puddle, inset by the hull, the level easing towards the real one) in the fluid's colour,
and each running engine's piston rod, crosshead and connecting rod following Create's
`(1 − sin a) / 4 · 24/16` stroke on a crank pin turning with the shaft.

**Persistence**: tank parts in BlockData (`data["steam"]`: controller, width, height, window,
fluid, amount), engines' value box there too; the engine / whistle / powered shaft index in
`lv:steam:index`, synced tanks in `lv:steam:tanks`. Boiler water supply samples are not saved
(they refill within 50 ticks).
### 3.6.15 Assembly (U15)

Checked against Create 6.0.10 (DeployerBlock(Entity), DeployerHandler, DeployerItemHandler,
BeltDeployerCallbacks, DeployerMovementBehaviour, ArmBlock(Entity), ArmInteractionPoint(Type)s,
ArmInteractionPointHandler, ArmAngleTarget, SequencedAssemblyRecipe, ManualApplicationRecipe).
Pure maths in `src/core/assembly/` (gtest), game side in `src/feature/assembly/`, animation in
`src/client/assembly/`.

**Deployer.** Kinetic (impact 4 SU/RPM). While turning it runs Create's cycle with a timer that
drops by `clamp(|speed|·2, 8, 512)` per tick: *waiting* → (start) *expanding* 1000 → activation →
*retracting* 1000 → *waiting* with a pause of 500. In *waiting* it first hands out excess items
(nothing happens while it has some: timer 10 ticks' worth), moves a held item the filter rejects
out of the hand, skips use-mode activations that would do nothing (a block item facing that block,
a bucket facing its fluid, an item over a belt / depot), and does not start while powered by
redstone (polled, §8). Activation (Create's DeployerHandler.activate) acts on the block two in
front through the dimension's deployer player (§8), standing in the hand 1.5 blocks out, looking
along the facing:
- an entity in that block first (one at random, contraption blocks excluded): *use* interacts with
  it (shearing, milking, feeding animals, trading, ...; food is fed to players), *punch* attacks it;
  the drops of a kill go to the deployer;
- otherwise *punch* mines the block: each activation adds 16× the player's per-tick destroy
  progress (tool, enchantments and effects included) and the block breaks at 1, its drops going
  into the deployer (fire in front is put out instead);
- otherwise *use* right-clicks the block face facing the deployer with the held item (placing
  blocks, bone meal, flint and steel, buckets, hoes, axes, opening / using blocks, other units'
  item and block actions, item application below); when that does nothing the item is used in the
  air (except ender pearls).
Everything the player ends up holding besides the main hand stack becomes items to hand out.
Modes: *use* (default) and *punch*, toggled with the wrench on the front face (action bar shows
"Mode: Use" / "Mode: Attack"). Right-click on the front face swaps the deployer's item with the
player's. Filter slot on the two sides that carry neither the hand nor the shaft. Item capability
(Create's DeployerItemHandler): slots = items to hand out + the hand; insertion goes into the hand
(if the filter allows, merging up to 64), extraction takes the items to hand out first, then the
hand unless a set filter asks for it.

*Deploying on items:* a deployer facing down two blocks above a depot / belt / weighted ejector
(`levitate:depot|belt|weighted_ejector`, reached through their item capability, slot 0) in use mode
starts when that item has a recipe with the held item and, when the hand is out, replaces one item
by the outputs (outputs that don't fit drop on top). Recipes in Create's order: sand paper
(`levitate:sand_paper` / `red_sand_paper`) polishing (`sandpaper_polishing`), the next deploying step
of a sequenced assembly, `deploying`, `item_application` (keep_held_item honoured). The held item is
used up (or damaged by 1 if it has durability). Sounds: item pop, item break when the hand empties.

*On contraptions* (Create's DeployerMovementBehaviour, `contraption::registerActor`): every new
block two blocks ahead (facing turned with the contraption) gets an activation; an empty hand
takes an item the filter allows from the contraption's storage; excess goes into the storage or
drops; while breaking a block the contraption stalls and the deployer hits it every 20 ticks. The
deployer's BlockData travels with it (data carrier `assembly:deployer`; the shaft axis is turned
with the facing).

**Mechanical arm.** Kinetic (impact 2). Selection (Create's ArmInteractionPointHandler): holding
the arm item, right-click an interaction point: first "Take items from <block>" (input), again
"Deposit items to <block>" (output), and so on; left-click removes it; switching away from the arm
item forgets the selection; selected points show particles (blue input, orange output). Placing the
arm keeps the points within `mechanicalArmRange` (5: squared block distance < 25) and reports
"<n> selected Interaction Point(s) removed due to Range Limitations." or "Mechanical Arm has <i>
input(s) and <o> output(s)." Points are stored relative to the arm (they turn with it on
contraptions). Interaction point types (Create's registry): basin, belt, blaze burner, chute /
smart chute, mechanical crafter, crushing wheel controller, deployer, depot / weighted ejector /
train station, andesite / brass funnel, millstone, mechanical saw facing up — all through their
item capability — and the vanilla jukebox, composter (in from the top, bone meal from the bottom),
campfire (cookable food) and respawn anchor (glowstone charges it).
Behaviour (Create's ArmBlockEntity): progress towards the chased point grows by
`min(256, |speed|)/1024` per tick; at a point it collects (the amount the outputs can take,
simulated) or deposits; searching for an input (not while powered) and for an output (every 10
ticks) follows the value box *When Multiple Outputs Available*: Round Robin (after the last used
point, forgetting it when nothing is found), Forced Round Robin (only the next one), Prefer First
Target (always from the first). A jukebox point playing a record makes the arm dance. Item pickup
sound on collecting.

**Animation (client mod).** Deployers send their phase / timer / reach / hand pose and arms the
angles they move between (`assembly.deployer`, `assembly.arm`, companions within 96 blocks); the
client mod draws the pole and hand at Create's hand offset (pointing, holding, punching) and the
arm's turntable, lower / upper arm and claw from Create's inverse kinematics (segments 18/16 and
17/16, folding back to rest first, then reaching out), and spins the shaft / cog with the kinetics
renderer. Vanilla clients see the static models.

**Sequenced assembly** (contract `src/feature/assembly/SequencedAssembly.h`). Recipes
`assets/recipes/sequenced_assembly/*.json` in Create's format (`ingredient`, `transitional_item`,
`loops`, `sequence` of deploying / pressing / cutting / filling recipes, weighted `results`; a
Levitate `tags` object may define tags inline). A machine of another unit asks
`findStep(kPressing|kCutting|kFilling, stack)` (deploying: `findDeployingStep(stack, held)`,
filling: `acceptsFluid` / `fluidAmount`) before its own recipes and calls `advance(step, oneItem)`
per item. The first step accepts the base ingredient or the transitional item, later steps only the
transitional item carrying that recipe's progress (item data `sequenced_assembly` {recipe, step,
progress}); after `sequence.size()·loops` steps one result is picked by weight (Create's
rollResult). The item's lore shows Create's tooltip: "Recipe Sequence", "Progress: s/t",
"Next: <step>" and the remaining steps ("Deploy <item>", "Process in Press", "Cut with Saw",
"Spout <fluid>").

**Item application** (Create's ManualApplicationRecipe, `assets/recipes/item_application/`):
right-clicking a block with the applied item (players and deployers, not sneaking) turns it into
the result block (further results pop out), plays the copper break sound (pitch 1.45) and uses up
the item unless creative or `keep_held_item`. Shipped: andesite / copper / brass casing from
stripped logs and wood, railway casing, bound cardboard; deploying: cogwheel, large cogwheel,
waxing Create's copper shingles / tiles.
#### 3.6.1 Display (U24)

Checked against Create 6.0.10 (AllDisplaySources / AllDisplayTargets, DisplayLinkBlock(Entity /
Item), FlapDisplayBlock(Entity), NixieTubeBlock(Entity), Clipboard*). Code: `src/feature/display/`
(server), `src/client/display/` (text rendering, native screens).

**Contract** (`feature/display/Display.h`, for units whose blocks are display sources / targets):
`registerSource(blockId, Source{id, name, text(ctx, stats) -> lines, options, refreshTicks,
passiveRefresh, multiLine, onSignalReset, onSelected, boardText})`, `registerSourceProvider(applies,
source)` (vanilla block entities), `registerTarget(blockId, Target{id, stats, accept,
acceptSections, lineName})`, `registerTargetProvider`, `sourcesAt` / `targetAt`,
`notifyGatherers(source)` (Create's DisplayLinkBlock.notifyGatherers: links reading that block send
at once), `itemsPassed(source, count)` (tunnels report passing items for `item_throughput` /
`accumulate_items`), `formatValue` (value lists: "1.2k", "3.4m"), `progressBar`. A registration
with the same (block, source id) replaces an earlier one, so the owner of a block can refine a
built-in source. `Context` gives the region, the link, source and target positions, the source's
persisted `config` (Create's sourceConfig) and the game time.

**Display link** (data `BlockData["display_link"]` = target offset, line, source id, config;
positions in `lv:display:links`): every tick its timer grows; at the source's
`getPassiveRefreshTicks` (default 100; stopwatch and item throughput 20) it resets and, unless the
link is powered, reads the source and writes the target (`updateGatheredData`). Redstone: powered
links send nothing; when the power goes the timer resets, the source's `onSignalReset` runs and the
data is sent at once. The source is the block behind the link; a selected source the block no
longer offers is deselected. Lines of a display board are *reserved* by the link that wrote them
(another link skips them while that one still targets the board).

**Sources** (ids, names and options as Create): speedometer `kinetic_speed` (Ignore / Include
Direction, "64 RPM"), stressometer `kinetic_stress` (Progress Bar / Percentage / Stress in SU /
Total Capacity / Remaining SU, "su"), cuckoo clocks `time_of_day` (12 / 24-hour, minutes rounded
down to 5, "--:--" when the clock stands still, nonsense outside the overworld) and `stopwatch`
(h:mm:ss since the last signal reset), smart observer `count_items` / `list_items` /
`count_fluids` / `list_fluids` (contents of the observed block through U3's capabilities and the
observer's filter slot), threshold switch `fill_level` (Percent / Progress Bar), belt / depot /
weighted ejector `item_names`, tunnels `accumulate_items` / `item_throughput` (per second / minute /
hour), seats `entity_name`, nixie tubes `nixie_tube`, and the vanilla `target` → `redstone_power`
(Number / Progress Bar), `enchanting_table` → `enchant_power` (Max Enchanting Cost: twice the
bookshelves, at least 1 + shelves / 2 ⚠ Create rolls Java's bottom-slot cost), command blocks →
`scoreboard` (objective id, value list), `respawn_anchor` → `death_count` (Player Deaths, counted by
Levitate). Every single-line source has Create's "Attached Label" option. Numbers use Create's
formatting (up to 2 decimals, grouped thousands); units get a space on display boards only. Boiler
status (U18), station summary / train status (U27), observed train name (U28), elevator floor
(U20), factory gauge (U26) and package address (U25) are registered by those units.

**Targets**: display board (2 lines per block row, `⌊(16·width − 2) / 3.5⌋` characters per line: 4,
8, 13, 17, …, 145 at 32 wide), nixie tube row (1 line, 2 characters per tube), sign (4 lines,
⚠ 15 characters, front side), lectern (one page per line, ⚠ 256 characters; a book and quill becomes
a written book "Printed Book" by "Data Gatherer" ⚠).

**Display board** (data at the controller `BlockData["display_board"]`, controllers in
`lv:display:boards`): the controller is the top block at the left end as seen from the front
(Create: no UP flag, no identical neighbour clockwise); width up to 32 counter-clockwise, height
down the DOWN flags; rows connect up / down only when the whole row matches. A size change keeps
line colours and glow and clears the text. Right-click a line (hit point moved ⅛ block into the
block; not sneaking): empty hand clears it (needs rotation), dye colours it (needs rotation; white
resets to Create's default `#D3C6BA`), glow ink makes it glow, a named name tag writes its name, a
clipboard writes the entries of its last viewed page (split at line breaks) from that line down.
Every 10 ticks (Create's lazy tick) the state goes to companions (`display.board`); the client mod
spins every character through Create's flap cycles (letters " ABCDEFGHIJKLMNOPQRSTUVXYZ", digits
" 0123456789", anything else at once; one step per tick, all at once above 128 RPM) only while the
board turns, and draws them on the front face.

**Nixie tubes** (data per tube `BlockData["nixie"]`, tubes in `lv:display:nixies`): a row is the
tubes with the same facing and face, from the left (floor / ceiling: opposite the facing; wall: up;
wall reversed: down); placed on the floor / ceiling the player's facing is turned clockwise, on a
wall the clicked face is used (reversed when the player looks up ⚠). Custom text (clipboard: first
entry of the last viewed page, name tag, display link) goes to the whole row, tube *i* showing
characters 2i and 2i+1; an empty hand clears it. Without text a tube shows the redstone power around
it as "00".."15" (polled every 2 ticks). Dye recolours the whole row (block id changes, data kept).

**Clipboard** (item data `lv:clipboard`, block `BlockData["clipboard"]` = type, pages of {text,
checked}, last opened page, copied settings): right-click (in the air, or on a block without
settings) opens it; pages 1..51, ~15 entries per page ⚠ (Create: height-limited); "#text" entries are
addresses (no checkbox); empty entries and pages are dropped when saved; Erase checked items.
Right-click a block with value boxes or filter slots (U3) copies them ("Copying settings from …";
values per box id, filters per slot id), left-click pastes ("Applied settings to …"; in survival a
filter item is taken from the inventory ⚠); right-click another clipboard merges its entries
(duplicates skipped, pages of 12). Sneaking disables copy / paste (Create).

**Vanilla clients**: board text and nixie characters as name tags of an invisible entity
`levitate:display_text` (one per board / tube; the client mod hides them); forms for the link
(source, line, options; a second form for a newly chosen source's options) and the clipboard (one
page per form: inputs, check toggles, page selector, erase checked).

**Commands** (op, Levitate debug): `/create display text <x y z> <line> <text>` (`_` = space) writes
a board line or a nixie row; `/create display bind <x y z> <tx ty tz> <source>` binds a link and
sends.
#### 3.6 U12 Item logistics (`funnels`)

Code: `src/feature/funnels/` (blocks, ticking, interaction), `src/core/funnels/Logistics.h`
(distribution, tunnel shapes, chute air flow, vault structures; gtest), contract extension
`src/feature/common/FunnelsExt.h` (fan and belt hooks, below). Every block is ticked like a Create
block entity; per-block state lives in `BlockData["funnels"]` (filters in `BlockData["filter"]`).
Config `config/funnels.json`: `defaultExtractionTimer` 8, `vaultCapacity` 20, `brassTunnelTimer`
10, `fanBlockCheckRate` 30 (Create's CLogistics / CKinetics defaults).

- **Funnels** (Create's FunnelBlock / FunnelBlockEntity). Placement: facing towards the player along
  the nearest looking directions (the first that isn't attached to another funnel); placed normally
  it *extracts*, placed while sneaking it *inserts*; the wrench flips it. Redstone power pauses any
  funnel (and resets its cooldown).
  - Extracting: every `defaultExtractionTimer` (8) ticks it pulls from the block behind it (item
    capability, the side facing the funnel) and drops the stack in front (centre + facing × .25,
    0.45 lower for wall funnels; ceiling × .5; floor × .15 with an upward push of 4/16), with the
    default pickup delay; it waits while an item entity is in its block (and the block below,
    unless it faces up). Andesite: 1 item. Brass: the "Extracted Amount" setting (rows *Up to* /
    *Exactly*, 1..64, milestones of 16, default up to 64 = "Any"); *exactly* only extracts when that
    many items of one kind are there. Brass funnels have a filter slot (wall funnel: front, top;
    floor / ceiling funnel: the four sides near the inventory).
  - Inserting: item entities touching its opening (on the front side of the centre − facing × .25)
    go into the inventory behind (filtered); a right-click on the front face with an item inserts
    the held stack. An upward inserting funnel also takes items from belts / chutes / machines
    above it (item capability on the funnel, Create's DirectBeltInputBehaviour).
  - Flaps: horizontal funnels flap (block state `levitate:flap` for 4 ticks, visible on every
    client) with Create's funnel flap sound (item frame turn + wool).
  - On a belt (belt funnels, pushing / pulling by belt direction): pending U11's belt (beltlink).
- **Tunnels** (Create's BeltTunnelBlock / BrassTunnelBlockEntity) sit on a belt segment (beltlink).
  Shape: a T on each side with an output (a belt running across, or a Levitate block taking items
  from that side), cross for both; a straight tunnel between two tunnels gets a window (wrench:
  window ↔ closed). The belt reports items crossing into the segment
  (`beltlink::itemEntering`):
  - andesite tunnel junction, belt along the tunnel's axis, stack > 1: one item split off to every
    side output that takes it (the item waits if one refuses); the rest goes on;
  - brass tunnel with more than one exit (side flaps or a group): it takes the whole stack (the
    belt item waits while the tunnel still holds one) and distributes it after 2 ticks, then every
    `brassTunnelTimer` (10) ticks, among the exits of its group (brass tunnels side by side on
    parallel belts, same axis): own sides first, then forward, then the other tunnels' exits;
    exits with a filter first, then the others. Modes (option box on top, "Distribution Method",
    shared by the group): Split, Forced Split, Round Robin, Forced Round Robin, Prefer Nearest,
    Randomize, Synchronize Inputs (`core::funnels::planDistribution`; synchronize releases items
    only when every tunnel of the group holds one). Filter slots on each open side: inbound filters
    block non-matching belt items, outbound filters sort; a side without its own filter uses the
    filter of the block on that side (e.g. a brass funnel). Exits: the belt / machine on that side
    (item capability), or forward out of the end of the belt as an item entity. Items can be
    inserted into a brass tunnel directly (item capability); empty-hand right-click retrieves the
    group's stacks.
- **Chutes** (Create's ChuteBlockEntity). One stack per chute moving at
  `((push + pull) / 64 clamped to ±20, − 4 when ≤ 0) / 20` blocks per tick (gravity: 4 blocks/s
  down). Encased fans (airflow) above facing down (pull) or below facing up (push) speed items up
  or reverse them; push / pull propagate along connected chutes (shared among branches). Input:
  from the inventory above (below for upward chutes), 16 items at a time; from chutes feeding into
  it; from items dropped on top of an open chute; upward chutes suck in item entities up to 1 / 2 /
  3 blocks below at 32 / 64 / 128 RPM. Output: the inventory / chute below (a smart chute below
  counts as an inventory), the chute it feeds (diagonal chutes: below and behind), or dropped out
  of the bottom (centre − 12/16, falling at .25) unless the block below has a solid top or a
  downward funnel; upward: the inventory above an open chute, the fastest input chute, or out of
  the top. A diagonal chute is made by clicking the side of a block (not sneaking); shapes update
  with the chutes around (intersection when fed diagonally). Wrench: normal ↔ window, encased →
  normal; industrial iron block: encase. Empty-hand right-click takes the stack out.
- **Smart chutes**: a chute with a filter slot on each side (8, 11 px) and the "Extracted
  Amount" setting (default 64; shown while it pulls from an inventory); redstone power stops it.
- **Item vaults** (Create's ItemVaultBlock / ConnectivityHandler): vault blocks of the same
  horizontal axis join into cuboids with a square cross-section up to 3×3 and up to 3× the width
  long (the controller at the lowest corner forming the most blocks wins; ties: the wider one).
  Placement: the axis of the vault clicked on (not sneaking), else the player's horizontal
  facing; wrench on the top / bottom turns the axis. Each block holds `vaultCapacity` (20) stacks;
  the whole structure is one inventory for every item capability user (no manual access).
  Connected textures through block states. Comparators next to any block read the whole vault
  (`floor(fill × 14) + 1` when not empty, Create's `calcRedstoneFromInventory`). Broken blocks
  drop their own stacks.
- **Creative crate**: an endless supply of the item in its filter slot (top): extraction gives up
  to a full stack of it, insertion voids the items (Create's BottomlessItemHandler).
- **Items in chutes** are shown to every client (vanilla and client mod) as client-only display
  item entities that follow the stack (Display.cpp, §8).
- **Contract extension** (`src/feature/common/FunnelsExt.h`, for U11 and U13):
  `airflow::registerFan(blockId, query)` — a fan's facing and speed (Create's
  `EncasedFanBlockEntity.getSpeed()`), read by chutes; `beltlink::registerSegment(blockId,
  query)` — a belt segment's movement facing, speed, slope and casing, read by tunnels; and
  `beltlink::itemEntering(region, beltPos, stack, movementFacing)` — the belt calls it when an
  item crosses into a segment (Continue / Stuck / Taken), so the tunnel above can take or split it.
  Items going *into* belts, depots, funnels and chutes use the item capability with `face` = the
  side they come from.
#### 3.6.1 Building blocks (U10)

### 3.6.31 Equipment (U31)

Checked against Create 6.0.10 (ToolboxBlock / ToolboxBlockEntity / ToolboxInventory /
ToolboxHandler / RadialToolboxMenu, BacktankItem / BacktankBlockEntity / BacktankUtil,
DivingHelmetItem, DivingBootsItem, NetheriteDivingHandler, ExtendoGripItem, PotatoCannonItem /
PotatoProjectileEntity / the built-in projectile types, SymmetryWandItem / SymmetryHandler and the
mirrors, WorldshaperItem / TerrainBrushes / TerrainTools / FlattenTool / PlacementPatterns,
LinkedControllerItem / LinkedControllerServerHandler / LecternControllerBlock, SandPaperItem,
TreeFertilizerItem, CEquipment). Code: `src/core/equipment/` (inventory, mirrors, brushes, air and
projectile numbers; gtest), `src/feature/equipment/`, `src/client/equipment/`. Block data lives in
BlockData `data["equipment"]`; item data in the stack's user data (`lv:toolbox`, `lv:air`,
`lv:symmetry`, `lv:shaper`, `lv:controller`).

**Toolboxes.** 8 compartments × 4 stacks. A compartment locks to the first item put in (its
filter stays when it runs empty; clicking an empty compartment slot with an empty cursor clears it);
items share a compartment when they are the same item with the same data, or the same damageable
unstackable tool. Insertion follows insertItemStacked (merge into same stacks, then empty slots in
order); toolboxes, shulker boxes and bundles don't fit. The block exposes all 32 slots through the
item capability (funnels, arms, chutes, ...). Right-click opens the screen (native: the 8
compartments in an octagon with the inventory below — click a compartment to take a stack, right-click
half, click an inventory slot to put it in; vanilla: a form), with Create's iron door + chest sounds;
sneak passes through so blocks can be placed against it. A dye of another colour recolours it in
place without using the dye. Left-click (attack) picks it up with its contents into the inventory
(unlinking everyone); breaking it any other way drops it with its contents (creative: only when not
empty).
- *Equipping* (Create's ToolboxHandler): a hotbar slot linked to a compartment of a toolbox within
  `toolboxRange` (10; squared distance to the block's bottom centre) is kept at
  `(maxStack + 1) / 2` items every tick (32 / 8 / 1): it refills from the compartment and gives the
  excess back. The link drops when the slot holds another item or the filter is cleared; it persists
  in the world DB. Equip: the slot is unequipped first, its old item goes into the toolbox (matching
  compartments only), then the inventory; Unequip returns the slot's items (Stop tracking keeps them,
  out of range); Return items puts every inventory stack into matching compartments of the nearby
  toolboxes (unlinking their slots).
- *Radial menu* (client mod): hold **Left Alt** (Create's "Access Nearby Toolboxes" key): the 8
  nearest toolboxes in range (SELECT_BOX when there are several) and then their compartments;
  releasing the key (or clicking) confirms the hovered entry; the centre unequips the selected
  slot ("Unequip: …", or "Stop tracking and keep item" out of range) or returns items ("Return items
  to (nearby) Toolbox(es)"). Linked hotbar slots get a marker (green in range, grey out of range).
  Vanilla players use `/create toolbox` (a form with the same entries).

**Backtanks.** Air 0..`airInBacktank` (900) in the item (bar = air). Placed as a block (facing the
player, keeping its air), it is a kinetic block with a shaft end at the bottom (stress impact 4):
while turning it adds `clamp(floor((|rpm| − 100) / 20), 1, 5)` air, then waits
`clamp(int(128 − |rpm| / 5) − 108, 0, 20)` ticks (256 RPM: 5 air per tick); full: a chime and a
ring of particles. Right-click with an empty chest slot (not sneaking, not holding a block item)
puts it on. Air tools cost `max(900 / uses, 1)` air (extendo grip 1000 uses: 1 air; potato cannon
200 uses: 4 air) from a worn backtank with air, else 1 durability (creative: free; 0 uses:
unbreakable). Pressure warnings as a subtitle when the air drops to max / 10 ("⚠ Backtank pressure
low", gold) and to 1 ("⚠ Backtank pressure depleted", red).
- *Diving helmet*: under water with a worn backtank with air: breathing (water breathing, refreshed)
  and 1 air per second; the netherite helmet with a netherite backtank does the same in lava
  (air only). The air timer (mm:ss, red under 60 s) is shown while it is used (client mod: next to the
  hotbar; vanilla: action bar).
- *Netherite set*: netherite diving helmet + netherite backtank with air + netherite leggings +
  netherite (diving) boots: fire immunity (fire resistance, refreshed).
- *Diving boots*: in water (not swimming, not flying): sinking (vy − 0.05 per tick, a jump on the
  ground +0.5) and walking speed ×1.3 below 0.145 blocks per tick (motion sent to the player, §8).

**Extendo grip**: +3 block reach in either hand, +5 with one in each hand (pick range; §8); every
block broken or placed and every entity hit by the holder uses the grip (air / durability, main hand
first); hits get Create's extra knockback.

**Potato cannon.** Ammo: off hand first, then the inventory (creative: not used up). Projectiles
leave 1.5 blocks in front of the eyes at `2 × velocity` blocks per tick and move with
`v = (v − (0, 0.05 · gravity, 0)) · drag` (gravity 1, drag 0.99); split shots spread around the aim.
Cooldown = the type's reload ticks (× 2/3 with a cannon in each hand). Types (damage / reload /
velocity / knockback / split): potato 5/15/1.25/1.5 (plants potatoes on farmland), baked potato
5/15/1.25/0.5 (fire 3 s), carrot 4/12/1.45/0.3 (plants carrots), golden carrot 12/15/1.45/0.5, sweet
berries 3/10/1.05/0.1 ×3, glow berries 2/10/1.05/0.05 ×2, chocolate glazed berries 4/10/1.05/0.2
×3, poisonous potato 5/15/1.25/0.05 (poison 8 s), chorus fruit 3/15/1.2/0.05 (random teleport),
apple 5/10/1.45/0.5, honeyed apple 6/15/1.35/0.1 (slowness II 8 s), golden apple 1/100 (golden
apple effects), enchanted golden apple 1/100, beetroot 2/5/1.6/0.1, melon slice 3/8/1.45/0.1,
glistering melon 5/8/1.45/0.1, melon 8/20/0.95/2 and pumpkin 6/15/0.95/2 (placed where they hit),
pumpkin pie 7/15/1.1/0.05, cake 8/15/1.1/0.1, blaze cake 15/20/1.1/0.3 (fire 12 s), fish (cod,
salmon, cooked, tropical) 4/10/1.3/0.6, pufferfish 4/10/1.1/0.4 (pufferfish effects), suspicious
stew 3/40/0.8/0.2 (drops the bowl). Knockback × 0.6 along the shot; endermen take damage only; a
powered wither is immune. The item is recovered with 12.5 % unless the hit used it up.

**Wand of symmetry.** Right-click a block: the mirror goes to the block in front of the clicked face
and the wand is enabled (5-tick cooldown); a new mirror is "Mirror Once" (players facing north /
south: "Along Z", else "Along X"); moving a plane re-orients it, moving a rectangular mirror picks
diagonal when |yaw mod 90| is in 22..67. Right-click the air disables it (the mirror stays);
sneak-right-click opens the screen (Mirror: Mirror Once / Rectangular / Octagonal, Orientation).
Every enabled wand in the hotbar mirrors blocks placed within `maxSymmetryWandRange` (50) of its
mirror (horizontal reflections about the mirror block; 1 / 3 / 7 copies), turning their states
(Bedrock's structure transform for vanilla blocks, `levitate:facing` for ours); survival copies use
an item each and need a free spot; breaking breaks the mirrored blocks (survival: same block only,
with drops). The mirror shows end-rod particles to the holder; each copy draws a particle line.

**Handheld worldshaper** (Creative Worldshaper; creative tab only). Left-click a block picks the
material ("Using: …"; not unbreakable blocks, block entities, Levitate blocks with data, doors and
beds). Sneak-right-click opens the screen (brush, its parameters, tool, placement, pattern);
right-click shapes at the looked-at block up to 128 blocks away (2-tick cooldown; "Shift-Right-Click
to Select a Shape" / "Left-Click a Block to set Material" otherwise). Brushes: cuboid (w / h / l
1..32, from −(n−1)/2 to n/2), sphere (radius 0..10, centre distance < r + .5), cylinder (radius 0..8,
height 1..8, distance < r + .42), surface and cluster (range 1..10, Follow Diagonals, Ignore Material
Borders: flood fill from the clicked block within distance < range; surface stays in the face's plane
and needs an open face). Placement (shaped brushes): merged / attached / inserted (Create's
offsets). Tools: fill (air only), place, replace (non-air only), clear, overlay (on top of non-air
blocks), flatten (Create's 5×5 Gaussian height smoothing). Patterns: solid, checkerboard, inverted
checkerboard, 25 / 50 / 75 % roll.

**Linked controller.** Six buttons — forward, back, left, right, jump, sneak — each bound to a
frequency (two items, Create's Couple<Frequency>). Right-click toggles it in use; in use, held buttons
transmit power 15 on their frequency from the player's position as transmitters of U23's link network
(`feature/redstone/RedstoneLink.h`: `link::add` / `move` / `remove`, Create's range rules); binding
reads a link block's two slots from BlockData `data["redstone"]` (U23's documented layout). A
pressed frequency stays alive 30 ticks without a refresh. Client mod: W / S / A
/ D / Space / Shift are captured while in use (Esc stops), sent on change and every 5 ticks;
vanilla: the player's movement input is polled. Right-click a redstone link: bind mode ("Bind mode
active"); the next pressed button takes the link's frequency ("Frequency bound to …").
Sneak-right-click in the air: the binding screen (12 ghost slots; vanilla: a form of dropdowns).
Lectern: right-click a lectern without a book with the controller places it (lectern controller,
keeping the controller in its data); right-click it to use it (one user at a time, within 4 blocks),
sneak + controller swaps, sneak with anything else takes it off (back to a lectern, the controller
drops on top).

**Sand paper / red sand paper.** Use (32 ticks, "eat" animation) to polish the item in the other
hand (or an item entity at the looked-at point within 3 blocks) by the `sandpaper_polishing` recipes:
the result goes to the inventory, the paper loses 1 durability; releasing early cancels. On copper
blocks it scrapes one oxidation stage or the wax like an axe (1 durability).

**Tree fertilizer.** On a sapling (not a hanging mangrove propagule) the tree grows at once
(Bedrock's own growth, repeated until it grows, so 2×2 trees work); one item is used (creative: none).

**Messages / logs**: one log line per user-visible action (`equipment: ...`), e2e scenarios wait
for them.

Code: `src/feature/building/`, data generated by `tools/gen/building.ps1`, textures by
`tools/textures/building.ps1`. State conventions: `levitate:facing` 0..5 (Java Direction order; the
horizontal blocks use 2..5, 0/1 read as north), connected-texture states `levitate:c_<dir>`, pane arms
`levitate:north|south|west|east`. Bedrock stores list-valued states by index, so every integer state
of the unit is a 0..n range. Any world change (players, commands, pistons, contraptions) is seen through
the block-changed event: the position and its neighbours are re-evaluated at the next tick (Create's
`updateShape`).

- **Connected textures**: casings and framed glass connect on all four edges of every face to the same
  block (16 textures per block), horizontal framed glass left/right, vertical framed glass and windows
  top/bottom (their top and bottom faces use an end texture). States: one per neighbour direction used.
- **Panes and bars** (Java `IronBarsBlock.attachsTo`): an arm towards a sturdy face, another pane / bars
  (ours or vanilla glass panes and bars) or a wall. Collision: 2-px post plus arms.
- **Stairs**: facing (player's direction), `upside` (clicked the bottom face or the upper half of a side),
  `shape` 0 straight, 1 inner_left, 2 inner_right, 3 outer_left, 4 outer_right computed as Java
  `StairBlock.getStairsShape`; model and collision from 8 octants. **Slabs**: `top`, `double`; a slab of
  the same kind used on a single slab merges as Java `SlabBlock.canBeReplaced`; a double slab drops 2.
- **Copper weathering** (Java `WeatheringCopper` / `ChangeOverTimeBlock`): each random tick (the game's
  `randomTickSpeed` per 4096 blocks per tick) has 64/1125 chance to try; any less weathered copper block
  (ours or vanilla) within Manhattan distance 4 stops it, otherwise it ages with chance
  ((higher+1)/(higher+lower+1))² × (0.75 at the first stage). States are kept. An axe removes the wax
  or scrapes one stage (1 durability), honeycomb waxes (one item); sounds `copper.wax.on` / `.off`,
  `scrape`. Waxed and fully oxidized blocks don't tick.
- **Scaffolding** (Create's `MetalScaffoldingBlock`, never falls): `bottom` when the block below is
  neither the same scaffolding nor sturdy (`distance` 0 on a sturdy block, else 7). Using scaffolding on
  scaffolding (not sneaking) places at the top of the column, or up to 7 blocks sideways when clicking
  the top face. Collision: half block for bottom pieces; else the vanilla shape (top and legs while
  standing on it, nothing when inside or sneaking). Climbable.
- **Ladders** (Create's `MetalLadderBlock`): placed on the clicked side or the nearest look direction
  with a sturdy wall; survive with a wall or the same ladder above (else pop off as an item); using a
  ladder on a ladder extends the line up (looking up) or down by up to `placementAssistRange` 12.
  Climbable.
- **Sliding doors** (Create's `SlidingDoorBlock`): placed facing the player's direction with the hinge of
  Java `DoorBlock.getHinge`, the upper half above (no placement if it doesn't fit). Clicking (any item
  unless sneaking with one) toggles both halves and the double door next to it (opposite hinge, same
  facing and half); breaking one half removes the other; only the lower half drops (none in creative).
  Redstone on either half, or on either half of the double door partner, opens it (`powered`).
  Andesite and copper doors fold, the others slide; sounds `open.iron_door` / `close.iron_door`,
  pitch 0.9–1.0. Open shapes from Create's `SlidingDoorShapes`, clipped to the block.
- **Trapdoors** (Create's `TrainTrapdoorBlock`): Java trapdoor placement (side: facing the clicked face,
  upper half when clicked high; else the player's opposite direction, top when clicking a bottom face),
  click toggles, redstone opens.
- **Girders** (Create's `GirderBlock`): `axis` from the clicked face, beams `x` / `z` when on that axis,
  next to a girder beam on that axis, an encased girder across, or a placard facing it; an existing beam
  stays while either end is held. `top` / `bottom` attach to girders, walls, chains, lanterns and
  placards (and stay on solid blocks). A shaft used on a girder makes `metal_girder_encased_shaft`
  (shaft axis z if the girder had an x beam or the clicked face is on z, else x), a kinetic shaft
  (`specs::shaft`, rotor model `geometry.levitate.girder_shaft_rotor`); it drops the girder and the
  shaft.
- **Brackets**: a metal / wooden bracket used on a shaft, cogwheel, large cogwheel or fluid pipe is
  stored in its `BlockData["building"]["bracket"]` and dropped when that block goes; the item is never
  placed as a block.
- **Table cloths**: decoration (1-px cloth with a skirt over the block below); U26 adds the shop.
- **Placard** (Create's `PlacardBlock`): hangs on the side it was looked at (a block with a collision
  shape). Use with an item while empty: it holds one (sound item frame); use while holding a matching
  item (same item and data, or matching the held filter): 15 power on every side and strong power into
  the block it hangs on for 19 ticks (`minecraft:redstone_producer`), a non-matching item plays the deny
  sound. Punching gives the item back; breaking drops it.
- **Copycats**: panel (3 px on the side looked at, `facing`) and step (half block, `facing`, `top`).
  Using a full, solid, non-container block item applies it as the material (one item used, kept for
  dropping); a different material is refused while one is applied; the wrench resets it.
- Selftests `building.*` (dx 1000..1099), e2e `tools/e2e/{server,client}/scenarios/building.ps1`.

### 3.6.6 Transmission (U6)

Checked against Create 6.0.10 (RotatedPillarKineticBlock, CogWheelBlock, CogwheelBlockItem,
ShaftBlock, EncasedCogwheelBlock, GearboxBlock, VerticalGearboxItem, GearshiftBlock, ClutchBlock,
ChainDriveBlock, ChainGearshiftBlockEntity, SequencedGearshiftBlock(Entity), Instruction,
SpeedControllerBlock(Entity), GaugeBlock, Speed/StressGaugeBlockEntity). Pure rules in
`src/core/transmission/` (gtest), game side in `src/feature/transmission/`, client in
`src/client/transmission/`.

- **Placement** (Create's getStateForPlacement; the state is computed from the world before the
  block is placed and applied in the same call): shafts, encased shafts, clutch, gearshift take the
  axis of the neighbours with a shaft end facing them (none or disagreeing: the nearest look axis;
  sneaking with a preferred axis: the clicked face's axis). Cogwheels: sneaking → clicked face
  axis; a large cog above a speed controller → across the controller's shaft; placed against a
  small cog → its axis; else the preferred axis, else the clicked face axis. A cog is refused where
  Create's `isValidCogwheelPosition` fails (a large cog beside it, or any cog beside a large one,
  unless that neighbour's axis points at it). Encased cogwheels: pillar rule, the axis of a small
  cog they are placed against. Gearbox: axis y; the vertical gearbox item: the axis across the
  horizontal shaft neighbours (none / disagreeing: across the player's facing). Chain drives: the
  preferred axis (sneaking: the look axis), then their part from the neighbours. Sequenced
  gearshift: preferred axis unless sneaking, else the look axis. Speed controller: a large cog
  above decides; else the horizontal shaft neighbours, else across the player's facing. Gauges:
  placed on a shaft end → facing across it (Create's rule from the look direction), else facing =
  clicked face with the shaft axis from the neighbours.
- **Placement assist** (Create's placement helpers): a shaft used on a shaft extends its line along
  the axis on the side nearer the hit (up to `placementAssistRange` 12); a cogwheel used on a cog
  (not on its shaft) puts the new cog beside it (small on small: same axis, the side nearer the hit;
  large on large: diagonal, perpendicular axis; mixed sizes: diagonal in the plane, same axis); a
  large cogwheel used on a speed controller goes on top across its shaft. One log line per assisted
  placement.
- **Encasing**: right-clicking a shaft / cogwheel / large cogwheel with `andesite_casing` or
  `brass_casing` (U10) turns it into the encased block without using the casing; encased cogwheels
  get a shaft end on each axis side with a neighbouring shaft facing them.
- **Kinetics** (specs in `Specs.cpp`): gearbox halves (Create's GearboxBlockEntity), clutch
  (powered: far half disconnected) and gearshift (powered: far half reversed); chain drives pass
  their speed ×1 along the connection axis (Create's areBlocksConnected), the adjustable chain
  gearshift multiplies by `1 + (signal + 1) / 16` (1 unpowered, 2 at signal 15) as source and divides
  as receiver; the sequenced gearshift's far half turns at the running instruction's modifier
  (idle, delay, await, end: disconnected); the speed controller drives a large cog on top
  (axis across its shaft) at its target speed with Create's getConveyedSpeed rules; gauges are
  shafts along their rotation axis. Stress impact 0 for all.
- **Redstone**: clutch, gearshift, adjustable chain gearshift (analog strength) and sequenced
  gearshift read the strongest neighbour signal from the dimension's circuit system every tick
  (redstone dust beside or above, producers, powered blocks, repeaters / comparators / observers
  facing them, torches not standing on them).
- **Sequenced gearshift**: right-click (any item but the wrench, and but a kinetic block on a shaft
  face) opens the instruction screen (native for the client mod; a form with five rows otherwise).
  Up to 5 instructions: Turn by angle (1..360°, Shift step 45, default 90), Turn to move
  Piston/Pulley/Gantry (1..128 m, step 5, default 5), Timed Delay (1..600 ticks, step 20, default 10),
  Await new Redstone Pulse, End; Turn instructions have a speed modifier (×2, ×1, ×−1, ×−2).
  Default list: turn 90° forward, end. A rising redstone edge while idle starts instruction 1
  (when it turns); durations: turn `ceil(angle / (speed·mod·0.3)) + 2` ticks, piston
  `ceil(m / (speed·mod/512)) + 2`, delay as set; Await continues on the next rising edge; End or
  the last instruction stop (the state stays until the power goes). A speed change re-times the
  running instruction, a stop ends the sequence. Changing the instructions resets the sequence.
  Comparator output = `levitate:state`. One log line per instruction step.
- **Rotation speed controller**: value box (board ⟳ / ⟲, 1..`maxRotationSpeed`, milestones 32) on
  the two side faces at 8 / 11 px; default 16 RPM; label "Targeted Speed in RPM". Changing it
  re-attaches the network. `levitate:bracket` shows the cog bracket under a large cogwheel.
- **Gauges**: speedometer dial = Create's getDialTarget (0..0.45 below `mediumSpeed`, ..0.75 below
  `fastSpeed`, ..1.125 at `maxRotationSpeed`, from the effective speed); stressometer dial = stress /
  capacity of the network (1.125 when overstressed, 0 when not turning). Comparator output
  `ceil(clamp(dial · 14, 0, 15))`. The client mod draws the needle on each head face (Create's
  shouldRenderHeadOnFace) easing 1/8 per tick towards the target (shaking past 1); vanilla clients
  see the needle at the nearest 22.5° step. Goggle lines (U8's overlay) use the gauge strings
  defined here (`create.gui.speedometer.title`, `create.gui.stressometer.*`, `create.gui.gauge.info_header`).
### 3.6.8 Wrench, goggles and the goggle overlay (U8 `goggles`)

Checked against Create 6.0.10 (WrenchItem, IWrenchable, GogglesItem, GoggleOverlayRenderer,
KineticBlockEntity / GeneratingKineticBlockEntity tooltips, IHaveGoggleInformation, KineticStats,
ItemDescription, TooltipHelper, OverlayConfigCommand, GoggleConfigScreen, CClient). Code:
`src/core/goggles/` (rotation, text helpers; gtest), `src/feature/goggles/` (wrench, overlay,
selftests), `src/client/goggles/` (overlay panel, position screen, item tooltips).

**Wrench** (`levitate:wrench`). Right-click on a block (players who may build; value boxes and
filter slots keep their plain clicks, U3):
- a *wrenchable* block runs its `onWrenched`; default (Create's IWrenchable): the state turned by
  `getRotatedBlockState` about the clicked face, set in the world (the kinetic network re-attaches
  it), the rotate sound (`levitate.wrench_rotate`: wooden trapdoor close, volume .25 × 1, pitch
  1.25 × (0.5..1.5)) when it changed. Rotation (Levitate state conventions, `core/goggles/Rotation.h`):
  on the top/bottom face `levitate:horizontal_axis` (0 x / 2 z) or `levitate:horizontal_facing`
  (0 S, 1 W, 2 N, 3 E) turn a quarter clockwise; else `levitate:axis` (0..2) becomes the axis of
  its positive direction turned clockwise about the clicked axis (unchanged when it is that axis);
  else `levitate:facing` / `minecraft:facing_direction` (0..5, Create's Direction order) turns
  clockwise about the clicked axis (unchanged when along it), `levitate:axis_along_first` toggles
  when the facing is along the clicked axis or the click was on top/bottom (Create's
  DirectionalAxisKineticBlock).
- Sneak + right-click runs `onSneakWrenched`; default: survival players get the block's item
  (inventory, the rest dropped at their feet; creative: nothing), a contraption anchored at the block
  is disassembled first, the block is destroyed without drops (break particles), remove sound
  (`levitate.wrench_remove` pop .25/.75 + `levitate.wrench_remove_hit` netherite step 1/.75, pitch
  × (0.5..1)).
- Not wrenchable but in Create's `#create:wrench_pickup` (rails, redstone wire, redstone torches,
  repeaters, comparators, levers, buttons, pressure plates, observers, pistons, tripwire, tripwire
  hooks, daylight detectors, targets, hoppers): sneak + right-click picks it up the same way.
- One action per click: repeated use packets on the same block within 4 ticks are ignored
  (Java's item use delay).
- Every kinetic block is wrenchable (Create's IRotate extends IWrenchable); U8 also opts in the
  wave-1 blocks Create makes wrenchable: chassis (3), sticker (defaults), track (right-click does
  nothing; sneak in survival gives the curves' tracks / girders with `tracks::dismantleCurves`
  before the pick-up, as Create's TrackBlock).
- Attacking a minecart (any `*minecart`) with the wrench in survival breaks it in one hit (Create's
  `wrenchInstaKillsMinecarts`, 100 damage).
- **Contract** `src/feature/goggles/Wrenchable.h`: `LV_REGISTER_WRENCHABLE(id[, Handler{onWrenched,
  onSneakWrenched, rotate}])`, `defaultOnWrenched` / `defaultOnSneakWrenched` / `rotatedBlock` for
  "super" calls (e.g. U21's bearings disassemble after the default rotation, U20's pistons return
  `Pass` unless retracted), `registerEntityHandler` for right-clicks on entities with the wrench
  (U20/U27: minecart contraption pickup, train relocation — Create's "When used on Contraptions").

**Engineer's goggles** (`levitate:goggles`): worn in the head slot (Bedrock's wearable equips it
on use and from dispensers). `isWearingGoggles` = goggles in the head slot or any predicate added
with `addWearingPredicate` (Create's GogglesItem.addIsWearingPredicate).

**Goggle overlay** (Create's GoggleOverlayRenderer), built on the server for the block the player
looks at (`overlayFor(player, pos)`):
1. A provider's `proxy` may redirect to another position (IProxyHoveringInformation).
2. With goggles: the *goggle* lines. Levitate blocks get the defaults unless a provider sets
   `replaceDefaults`: kinetic stats (only when the impact is not 0: "Kinetic Stats:", gray "Kinetic
   Stress Impact:", aqua `impact × |theoretical speed|` + "su", dark gray "at current speed" — indent 1),
   generator stats (capacity not 0: "Generator Stats:", gray "Kinetic Stress Capacity:", aqua
   `|capacity × speed|`, corrected by generated/actual speed when driven faster), then — each
   separated by a blank line — the fluid capability (Create's containedFluidTooltip: "Fluid Container
   Info:", per non-empty tank gray name and gold "amount mB" gray " / " dark gray "capacity mB"; a
   single empty tank shows gray "Capacity: " gold "N mB"; fluid names from `fluid.<id>` keys), the
   item capability (Levitate: "Contents:", gray name green " xN", at most 8 kinds + "...and N more")
   and the heat capability (Levitate: "Heat: <level>"). Then every provider's `goggle`.
3. The *hover* lines (with or without goggles), after a blank line when there are goggle lines:
   kinetic blocks in an overstressed network show gold "Overstressed" and the wrapped hint
   `create.gui.contraptions.network_overstressed` (gray, _highlights_ white, 200 px per line; client
   config `enableOverstressedTooltip`); a block turning slower than its `setSpeedRequirement` level
   (slow 1, medium `mediumSpeed`, fast `fastSpeed`) shows gold "Speed Requirement:" and
   `not_fast_enough` with the block's name; then every provider's `hover`. The blank line goes when
   the goggles added lines and the hover added none.
4. Nothing when both kinds exist and neither added lines, or when there are no lines. Every line is
   indented by 4 + indent spaces (Create's forGoggles); numbers use Create's format (grouping, ≤ 2
   decimals). The icon is `levitate:goggles` unless a provider's `icon` names another item.

Client mod: every `overlayInterval` (config, 4) ticks and on every target change the client sends
`goggles.query {pos}`; the server answers `goggles.info {pos, lines, icon}` (targets within 8
blocks, at most one answer per tick and player). The panel is drawn on the HUD (not while a screen is open, not for spectators, not while
looking at a value box or filter slot) at `centre + (overlayOffsetX, overlayOffsetY)` (defaults 20,
0), kept 20 px inside the screen, as Minecraft's hovering text (12 px right / up of that point,
background `0xF0100010` at 75 % alpha, border gradient `0x505000FF` → `0x5028007F`, or the custom
colours with `customColorsOverlay`), the first line 2 px apart, the goggles icon at (+10, −16); it
fades in over 24 frames sliding 8 px from the offset side. Vanilla clients: every `overlayInterval`
ticks the server traces the player's look (5.5 blocks) and shows the same lines in the action bar
(indentation trimmed), resent every second while unchanged (never blanked: it fades, so other units' action-bar messages stay)
(config `vanillaOverlay`).

`/create overlay` (anyone, client mod) opens Create's position screen: the sample tooltip
(`create.gui.config.overlay1..8`) follows clicks / drags (clamped as Create), Esc saves the offset
to `config/goggles.json`. `/create overlay reset` sets both offsets to 0 (Create does the same) and
answers "Create Goggle Overlay has been reset to default position".

**Overlay contract** `src/feature/goggles/GoggleInfo.h` (wave-2 blocks add lines without knowing
U8's code): `LV_REGISTER_GOGGLE_INFO(id, InfoProvider{goggle, hover, icon, proxy,
replaceDefaults})` — `goggle` / `hover` get an `InfoContext {player, region, pos, block, sneaking,
wearingGoggles, tr(...)}` and append lines; helpers `line(text, indent)`, `number`, `addHint`,
`formattedSpeed` / `formattedStress` (Create's IRotate texts for U6's gauges), `kineticGoggleTooltip`,
`kineticHoverTooltip`, `fluidTooltip` (for providers replacing the defaults); kinetic block
properties `setSpeedRequirement` (IRotate.getMinimumRequiredSpeedLevel), `setHideStressImpact`,
`setGeneratorSpeed(id, rpm, mayGenerateLess)` (BlockStressValues; preset: hand crank and valve
handles 32, water wheel 8, large water wheel 4, creative motor up to 256).

**Item tooltips** (client mod, appended to the game's hover text of `levitate:` items):
- Create's ItemDescription: when `item.create.<path>.tooltip.summary` (or `block.create...`)
  exists: dark gray "Hold [Shift] for Summary" (key gray, white while held) and, with Ctrl
  actions, "Hold [Ctrl] for Controls"; Shift shows the summary (wrapped at 200 px, Create's palette:
  material gold with yellow highlights), a blank line and every condition (gray) with its behaviour
  (indented); Ctrl the controls with their actions. Keys: `<key>.summary`, `.condition<i>`,
  `.behaviour<i>`, `.control<i>`, `.action<i>`; each unit adds them for its items. Config
  `enableTooltips`.
- Create's KineticStats for kinetic blocks: a blank line, gray "Kinetic Stress Impact:" and a
  3-segment bar (low < `mediumStressImpact` ≤ medium < `highStressImpact` ≤ high; yellow / gold /
  red) with "Low / Moderate / High" — with goggles on "Nx RPM" instead; gray "Kinetic Stress
  Capacity:" (thresholds `mediumCapacity` / `highCapacity`, colours reversed) with "Small / Medium /
  Large" or "Nx RPM" and dark gray " -> N su" ("Up to N su") from the generator speed.
- Logged actions: `wrench: <player> rotated <id> at <pos> (face f)`, `wrench: <player> dismantled
  <id> at <pos>`, `wrench: <player> broke a <minecart>`, `goggles: overlay for <player> at <pos>
  (<n> lines, client mod|action bar)` (once per new target), `goggles: overlay screen opened for`,
  `goggles: overlay reset for`, client `goggles: overlay offset set to x, y` / `reset`.

### 3.6.9 Materials, foods, world generation, decorative stone (U9)

Data: `tools/gen/materials.ps1` generates every pack / recipe file of the unit (and
`src/feature/materials/MaterialsBlocks.inc`, the block table the C++ side reads) from its tables;
names come from `tools/gen/materials.names.tsv` (Create's en_US / zh_CN wording); textures from
`tools/textures/materials.ps1`. Rerun both after a change and commit the output.

- **Blocks** (properties copied from the vanilla block Create starts from; hardness / blast
  resistance): zinc ore 3 / 3, deepslate zinc ore 4.5 / 3 (both need an **iron** pickaxe; drop
  raw zinc with Fortune's ore bonus `count·(max(0, rand(fortune+2)−1)+1)`, silk touch: the ore),
  raw zinc / zinc / brass blocks 5 / 6 (iron pickaxe), andesite alloy block 1.5 / 6, industrial and
  weathered iron 3 / 6 (column), rose quartz block (pillar) 1.5 / 1.5, rose quartz tiles / small tiles
  3 / 6, block of experience 3 / 6 (light 15), cardboard / bound cardboard 0.2 (horizontal axis of
  the player's facing, axe; bound: silk touch itself, else cardboard block + string). Every other
  block needs a pickaxe to drop (Create's requiresCorrectToolForDrops); the rule is applied when a
  player mines (`BlockType::spawnResources` hook), explosions use the loot tables.
- **Decorative stone**: per stone the raw stone (Create's 7; asurine, crimsite, ochrum, veridium
  with 4 random natural texture variants), cut / polished cut / cut bricks / small bricks (each +
  stairs, slab, wall), layered (column, cap top), pillar (axis from the clicked face). Create's
  stones: hardness 1.25 (scoria / scorchia 1.5); stairs / slabs / walls have their pattern's
  properties. Stairs: `minecraft:corner_and_cardinal_direction` + `minecraft:vertical_half`
  (Bedrock computes Create's inner / outer corners, corners with any of the unit's stairs), world
  aligned quarter boxes for model and collision. Slabs: `minecraft:vertical_half` + `levitate:double`;
  using a slab on the empty half of the same single slab (clicked face, or the slab beside the
  clicked face by click height) makes it double (one item used, 2 drops); the polished slab's sides
  use Create's slab texture. Walls: `minecraft:connection` (format 1.26.0), post unless exactly
  straight, low sides.
- **Recipes**: every Create 6.0.10 crafting / smelting / blasting / smoking / campfire /
  stonecutting recipe producing these items (c: tags resolved to their single Bedrock item);
  stonecutting accepts every member of Create's `stone_types/<stone>` tag (full blocks, stairs,
  walls, layered, pillar, raw stone) into every palette block (slabs ×2). Processing recipes
  (Create's format, `assets/recipes/<type>/`): mixing (andesite alloy ×2, brass ingot heated, pulp,
  dough, chocolate melting), compacting (blaze cake base, chocolate bar), pressing (sheets,
  cardboard), sandpaper polishing (rose quartz), sequenced
  assembly (precision mechanism: golden sheet, 5 loops of cogwheel / large cogwheel / iron nugget,
  weighted results 120 / 8 / 8 / 5 / 3 / 2 / 2 / 1 / 1; sturdy sheet: powdered obsidian, lava 500 mB,
  press, press), filling of the potion fluid into cinder flour; the other crushing / milling /
  splashing / filling / emptying / item application recipes of these items ship with U13 / U15 /
  U19. Tags: `assets/recipes/tags/materials.json` (c: metal tags, `levitate:stone_types/*`,
  `levitate:pulpifiable`, ...). U3's attribute filter recipe (brass nugget, wool, brass nugget) is in.
- **Foods** (nutrition, saturation modifier): bar of chocolate 6 / 0.3, sweet roll 6 / 0.8,
  chocolate glazed berries 7 / 0.8, honeyed apple 8 / 0.8, builder's tea 1 / 0.6 (always edible,
  drink, 42 ticks, stacks to 16, gives the bottle back, Haste I for 3 min without particles / icon).
  Blaze cake burns 320 s in a furnace, cardboard 50 s, creative blaze cake forever.
- **Experience nugget**: use = the whole stack (one when sneaking) turns into 3 xp per nugget, in
  one orb (one nugget) or up to 5 orbs of `1 + total/5`. Log `<player> used N experience nugget(s)`.
- **Chromatic compound** (dropped, Create's legacy item; config §5): falling into the void
  (y < min, falling > 10 blocks per tick) turns the stack into shadow steel (thrown up by
  `(fall distance + 3)/50`); half of the ticks it picks a random block within 3: a light source in
  line of sight (not unbreakable, not a beacon) is removed and one compound gains a charge (item data
  `lv:chromatic_compound.light`); at `lightSourceCountForRefinedRadiance` (10) charges it becomes refined
  radiance (+0.25 up); over an active beacon the whole stack becomes refined radiance. Shadow steel
  and refined radiance float (gravity cancelled) and shed end rod particles.
- **World generation** (`src/core/materials/WorldGen.*`, gtest): run natively when a chunk is
  decorated (overworld multinoise and nether generators): zinc = vanilla OreFeature (size 12,
  stone → zinc ore, deepslate / tuff → deepslate zinc ore, origin must be below the ocean floor
  height of the area), striated = Create's LayeredOreFeature (pattern picked at random, layers
  rolled by weight never repeating, thickness min..max, ramp along a random direction biased to
  horizontal, simplex displacement 1.75/size at 0.125, radial threshold 0.5..1 × noise 0.75..1,
  targets `stone_ore_replaceables` / `deepslate_ore_replaceables` / `base_stone_nether`). Features
  write into the 8 neighbour chunks when present. Flat worlds get nothing (as Java). Log once per
  world: `materials: world generation active ...`.
## 4. Screens

Each screen: native (client mod) + form fallback (vanilla). Owner unit in brackets.

| Screen | Opened by | Owner |
|---|---|---|
| Filter / attribute filter | right-click filter item | U3 |
| Value box form (vanilla) | wrench right-click on a value box | U3 |
| Sequenced gearshift instructions (native: 5 rows of instruction / value / speed, sent on close; form: 5 rows of dropdown / slider / dropdown) | right-click | U6 |
| Toolbox | right-click / radial select (client mod) | U31 |
| Linked controller binding | right-click | U31 |
| Wand of symmetry / worldshaper | right-click | U31 |
| Schematic table, schematicannon, schematic & quill naming | right-click | U29 |
| Clipboard | right-click | U24 |
| Display link configuration | right-click | U24 |
| Redstone requester / factory gauge / stock ticker / shopping list | right-click | U26 |
| Package port (frogport, postbox), package filter address (packagers take their address from signs, as Create) | right-click | U25 |
| Train schedule editor | right-click with schedule | U27 |
| Station (name, assemble/disassemble, door control) | right-click station | U27 |
| Crafting blueprint | right-click | U29 |
| Ponder | W over item (client mod) | U30 |
| Threshold switch (`redstone.threshold_switch`; the smart observer has only a filter slot) | right-click | U23 |

## 5. Configuration

One section per unit (`config/<unit>.json`, `LV_REGISTER_CONFIG`). Keys mirror Create's server
config (CKinetics, CStress, CLogistics, CFluids, CTrains, CSchematics, CEquipment, CRecipes, CWorldGen)
with the same defaults; each unit lists its keys here in its PR. The stress table (impact /
capacity per RPM, Create defaults ⚠) belongs to `config/kinetics.json` (U2) so all units read one
place:

| Block | Impact | Capacity | Generated RPM |
|---|---|---|---|
| hand_crank / valve handles | | 8 | 32 |
| water_wheel | | 32 | 8 |
| large_water_wheel | | 128 | 4 |
| windmill_bearing | | 512 ⚠ (per sail-count rules) | sails / 8 ⚠ |
| steam_engine | | 1024 (per engine, on its powered shaft, §3.6.1) | 16..64 (boiler driven) |
| creative_motor | | 16384 | value box (−256..256) |
| mechanical_press | 8 | | |
| mechanical_mixer | 4 | | |
| millstone | 4 | | |
| crushing_wheel | 8 | | |
| encased_fan | 2 | | |
| mechanical_drill / saw | 4 | | |
| deployer | 4 | | |
| mechanical_arm | 2 | | |
| mechanical_crafter | 2 | | |
| mechanical_bearing / clockwork / piston / pulleys / gantry | 4 | | |
| turntable | 4 | | |
| belt / depot / gearbox / shaft / cogs | 0 | | |
| speed controller / sequenced gearshift / chain drives | 0 / 0 / 0 ⚠ | | |
| mechanical_pump | 4 | | |
| hose_pulley | 4 | | |
| chain_conveyor | 1 ⚠ | | |

`config/packages.json` (U25): `packagePortRange` 5, `chainConveyorCapacity` 20 (Create's CLogistics),
`maxChainConveyorLength` 32, `maxChainConveyorConnections` 4 (Create's CKinetics).

`config/pistons.json` (U20): `maxPistonPoles` 64, `maxRopeLength` 384 (Create's CKinetics keys and
defaults; the push line uses U4's `maxChassisRange`). Stress (checked): pistons, rope and elevator
pulleys 4; gantry shaft and carriage 0 (`gantry_shaft` 4 in the table above is wrong).
`config/display.json` (U24): `displayLinkRange` 64 (Create's CLogistics).

`config/contraption.json` (U4): `maxBlocksMoved` 2048, `maxChassisRange` 16 (Create's CKinetics
keys and defaults), `maxSuperGlueRange` 24, `glueOutlines` true, `unmovableBlocks` (extra unmovable ids).

`config/actors.json` (U22): `harvestPartiallyGrown` false, `harvesterReplants` true,
`moveItemsToStorage` true, `rollerFillDepth` 12 (Create's CKinetics), `psiTimeout` 60,
`seatHostileMobs` true (Create's CLogistics).
`config/redstone.json` (U23): `linkRange` 256 (Create's CLogistics `linkRange`).
`config/crafter.json` (U16, Create's CRecipes / CKinetics keys and defaults):
`allowRegularCraftingInCrafter` (true), `maxFireworkIngredientsInCrafter` (9),
`allowStonecuttingOnSaw` (true), `bulkCutting` (false), `moveItemsToStorage` (true).
`config/goggles.json` (U8): Create's CClient keys `enableTooltips` (true),
`enableOverstressedTooltip` (true), `overlayOffsetX` (20), `overlayOffsetY` (0),
`customColorsOverlay` (false), `customBackgroundOverlay` (0xf0100010), `customBorderTopOverlay`
(0x505000ff), `customBorderBotOverlay` (0x5028007f) — read by the client mod — and Levitate's
`vanillaOverlay` (true: action-bar overlay for vanilla clients) and `overlayInterval` (4 ticks).
`config/pipes.json` (U17): `mechanicalPumpRange` (16), `pipesPlaceFluidSourceBlocks` (true) (Create's
CFluids keys and defaults), `waterSourceConversion` (true) and `lavaSourceConversion` (false) (Java's game
rules of the same names, which decide whether a drained source refills).
`config/basin.json` (U14): Create's CRecipes `bulkPressing` (false), `allowShapelessInMixer` (true),
`allowShapedSquareInPress` (true), `allowBrewingInMixer` (true, no effect until U19's potion fluids);
Levitate: `basinIntakeInterval` (1 tick), `burnerParticles` (true).
`config/belts.json` (U11): `maxBeltLength` 20, `maxEjectorDistance` 32 (Create's CKinetics keys and
defaults), `displayItems` true (show items on belts / depots / in flight as item entities, §8).
`config/fluids.json` (U19, Create's CFluids and CLogistics defaults): `hosePulleyBlockThreshold`
10000 (-1: never infinite), `fillInfinite` false, `hosePulleyRange` 128, `bottomlessFluidMode`
`ALLOW_BY_TAG` (`ALLOW_ALL`, `DENY_ALL`, `ALLOW_BY_TAG`, `DENY_BY_TAG`), `bottomlessAllow`
[water, lava] (Create's `create:bottomless/allow` tag), `bottomlessDeny` [], `fluidFillPlaceFluidSourceBlocks`
true, `psiTimeout` 60.
`config/bulk.json` (U13, Create's CKinetics keys): `crushingDamage` 4, `fanPushDistance` 20,
`fanPullDistance` 20, `fanBlockCheckRate` 30, `fanRotationArgmax` 256, `fanProcessingTime` 150, and
`fanParticleDensity` 0.5 (Create's client option).
`config/steam.json` (U18): `fluidTankCapacity` (8, buckets per tank block) and `fluidTankMaxHeight`
(32), Create's CFluids keys and defaults.
`config/assembly.json` (U15): `mechanicalArmRange` 5 (Create's CLogistics key and default).
`config/funnels.json` (U12): `defaultExtractionTimer` 8, `vaultCapacity` 20, `brassTunnelTimer` 10
(Create's CLogistics), `fanBlockCheckRate` 30 (Create's CKinetics; how often chutes re-check their air flow).
`config/equipment.json` (U31, Create's CEquipment keys and defaults): `toolboxRange` 10, `airInBacktank`
900, `maxExtendoGripActions` 1000, `maxPotatoCannonShots` 200, `maxSymmetryWandRange` 50 (the
controller's link range is U23's `redstone.linkRange`).

`config/kinetics.json` (U2): `disableStress` (false), `maxRotationSpeed` (256), `kineticValidationFrequency`
(60), `mediumSpeed` (30), `fastSpeed` (100), `mediumStressImpact` (4), `highStressImpact` (8),
`mediumCapacity` (256), `highCapacity` (1024), `impact` {block path: SU per RPM} and `capacity`
{block path: SU per RPM} (the table above, plus `cuckoo_clock` 1, `weighted_ejector` 2, backtanks 4,
`elevator_pulley` 4 ⚠, `gantry_shaft` 4 ⚠ and 0 for the transmission blocks), `syncToClients` (true).
A block without a table entry uses its spec's `impact` / `capacity`. The generated RPM column is the
generators' own behaviour (U7, U18, U21), not a config value.

`config/generators.json` (U7): `crankHungerMultiplier` (0.01, exhaustion per hand crank use per RPM).

`config/materials.json` (U9): `disableWorldGen` (false, CWorldGen), `lightSourceCountForRefinedRadiance`
(10), `enableRefinedRadianceRecipe` (true), `enableShadowSteelRecipe` (true) (CRecipes).

## 6. Commands

| Command | Who | Owner |
|---|---|---|
| `/create reload`, `/create selftest ["glob"]` | op | foundation |
| `/create kinetics <pos>` (dump network: speed, stress, sources) | op | U2 |
| `/create killtrain <name>`, `/create trains` | op | U27 |
| `/create glue clear [radius]` (Create's `/create clearglue`; default 16, around the sender) | op | U4 |
| `/create contraption assemble <x y z> [axis]` / `move <id> <dx dy dz> [ticks]` / `rotate <id> <axis> <degrees> [ticks]` / `stop <id>` / `disassemble <id>` / `list` / `demo` / `chassis <x y z> <range>` (test anchor) | op | U4 |
| `/create ponder <id>` (client mod) | anyone | U30 |
| `/create highlight <pos>` | op | U3 |
| `/create overlay` (overlay position screen, client mod) / `/create overlay reset` | anyone | U8 |
| `/create pipes <pos>` (pressures, flows and transfers of a pipe) | op | U17 |
| `/create steam <pos>` (tank / boiler goggle lines) / `steam <pos> fill <fluid> <mB>` (Levitate debug: fills, or drains when negative, through the fluid capability) | op | U18 |
| `/create display text <x y z> <line> <text>` / `bind <x y z> <tx ty tz> <source>` (Levitate debug) | op | U24 |
| `/create toolbox` (the toolbox radial menu as a form, for players without the client mod) | anyone | U31 |

## 7. Strings

Create's English wording and key names (`create.*`) for every string that exists in Create, e.g.
`create.gui.goggles.kinetic_stats`, `create.gui.stressometer.overstressed`, `create.tooltip.stressImpact`.
Levitate-only strings use `lv.<unit>.*`. Each unit owns the keys of its features (en_US + zh_CN; zh_CN
matches Create's official Chinese translation wording).

## 8. Not ported

| Create feature | Why | What Levitate does instead |
|---|---|---|
| Animated parts / contraptions for **vanilla** clients | Bedrock has no client-side rendering hooks without a client mod | static block models; moving contraptions shown as their blocks only when stopped plus a placeholder entity while moving; the client mod shows everything |
| Flowing honey/chocolate in the world (U19) | Bedrock has no data-driven custom fluids: a custom block can't flow, be swum in or use the fluid renderer | they exist everywhere fluids are handled (pipes, tanks, basins, spouts, drains, buckets); in the world a bucket or a hose pulley places a translucent, non-colliding, non-flowing source block (`levitate:honey` / `levitate:chocolate`) that buckets and hose pulleys pick up again; entities in it are slowed (Slowness II) and take no fall damage instead of Create's viscous fluid motion; lava touching it turns into obsidian / limestone / scoria; no fog colour under it |
| Hose drawn for **vanilla** clients (U19) | Create draws the hose with a block entity renderer; a block in its place would block the fluid it hangs in | the client mod draws the hose and its end piece; vanilla clients see the pulley only (draining and filling work the same) |
| Spout nozzle squeeze and splash particles (U19) | client-side animation and fluid particles | the client mod draws Create's fluid stream (its width follows Create's curve over the 20 ticks); the nozzle model is static; vanilla clients hear the sounds only |
| Spout over a moving belt item (U19 + U11) | wave-2 units can't call each other: the spout sees the item through the item capability (slot 0 from the top of a Levitate block) and can't stop a belt item itself | works on depots (and any block that exposes the item at its centre as slot 0); belt items are filled when the belt unit exposes the item under the spout that way |
| JEI/EMI recipe viewers, Create's config GUI | no such mods on Bedrock | recipes visible in Ponder/tooltips; JSON configs + `/create reload` |
| Create's mod-compat crushed ores for absent mods | no such mods | items exist (as Create registers them), with the vanilla-only recipes |
| Contraption blocks drawn as one rotating model on **vanilla** clients (U4) | a vanilla client draws a falling-block entity axis-aligned and can't be told to turn it | each block is its own falling-block entity at its exact rotated position, showing the block turned by the nearest quarter turn; the client mod turns each block by the remaining angle so the structure looks exact |
| Contraption collision computed by the client (U4) | Bedrock clients only collide with real blocks and a few entity types | the server pushes players out of the contraption and carries them (teleports); with lag this is less smooth than Create's client-side collision |
| Glue region outlines (U4) | no client-side line rendering without a client renderer for it | particles along the edges of nearby regions for players holding super glue (client mod and vanilla) |
| Redstone input of the sticker (U4) | custom blocks get no redstone neighbour update | the sticker polls the redstone power of its neighbours every 4 ticks (redstone blocks, wire, torches, levers, buttons, repeaters, comparators, observers, anything with `redstone_signal`) |
| Curved tracks drawn for **vanilla** clients (U5) | Bedrock block models are fixed per block state; a curve is not a block | the client mod draws the curve; vanilla clients see the straight end tracks and a track bed (`levitate:fake_track`) on every block the curve passes through |
| Track placement preview while hovering (U5) | Create draws the planned curve and its message client-side every frame | the message (`create.track.*`) is shown in the action bar when clicking; the selection is kept per player on the server (the item is not enchanted-looking) |
| "Hold Ctrl for maximized turn" (U5) | the server can't see a held key without the client mod | the player's sprint state is used (sprinting = maximized) |
| Tilted tracks between two sloped curves (U5) | Create re-shapes the curves and tilts the block model at runtime | curves keep their original handles; the middle track stays flat |
| Value box editing by *holding* right-click (Create opens the board after a short hold) | the server only sees discrete use packets, no release | a click opens the board; a click on the board confirms (U3) |
| Filter slot items rendered on the block face | no item rendering hooks for vanilla clients; the client mod has no world-space item renderer yet | the hover label (client mod) names the filter's item; vanilla players see it in the form / action messages (U3) |
| Create's in-world value box outline and label | same | client mod: label next to the crosshair while looking at a box (U3) |
| PSI comparator output (U22) | a data-driven block can't give a comparator signal (the block type's comparator virtuals are folded game functions that can't be hooked per block) | the `levitate:extended` state shows the connection |
| Actors dropping off when the block behind them goes (U22) | data-driven blocks get no neighbour updates and placement filters can't follow a facing state | they stay; they still move with the block behind them |
| Seats halving fall damage, mobs bouncing on seats (U22) | no fall-damage or bounce hook for data-driven blocks | normal landing; mobs that land on a free seat sit down |
| Right-clicking contraption blocks with an empty hand into the air on **vanilla** clients (U22) | a vanilla client sends nothing for an empty-hand click into the air, and display entities can't be clicked | with an item in hand, or with a world block behind, the server casts the look itself; the client mod sends the click |
| Seat dyeing recipe (seat + dye) (U22) | Bedrock recipes can't use an item tag of our seats | dye a placed seat with a right-click (as Create) |
| Riders sitting on moving contraption seats (U22) | the engine (U4) carries riders by teleporting them | they stand on the seat while it moves and sit down again on the placed seat |
| Redstone *input* callbacks for custom blocks (U23) | data-driven blocks can't be circuit capacitors (one circuit component per position: a block that outputs can't also consume) and get no neighbour-signal callback natively | outputs are native `minecraft:redstone_producer` permutations (no polling); inputs are read from the game's circuit components around each loaded block once per tick, which is also Create's block-entity tick, so latency matches Create's scheduled ticks |
| Redstone link frequency items rendered on the block (U23) | no world-space item rendering (see filter slots above) | client mod: hovering a link shows its mode and both frequency items (icons + names) next to the crosshair; vanilla: action-bar message when a slot is set |
| Diode value box "short click toggles, hold opens the board" (U23) | see value box row above | clicking the box opens the board; clicking elsewhere on the diode (or with any item but the wrench) toggles inverted |
| Vanilla repeater / comparator facing in the input reader (U23) | a repeater set by a command keeps an unpowered circuit, so the selftests can't build one; the circuit direction is the game's legacy 2D direction | read from the circuit component (`getDirection`, 2D → face); covered by vanilla play, not by a selftest |
| Item / block icons of the redstone link: Create draws the upward link | block item icons use the default permutation (facing 0 = down) | `minecraft:item_visual` with the upward geometry |
| Redstone input of the mechanical crafter (U16) | custom blocks get no redstone neighbour update | the crafter polls the power of its neighbours every tick (same sources as the sticker) |
| Crafter items on the crafter's face (U16) | Create renders the slot / grid items client-side; vanilla clients have no hook for it, and the client mod's attempt (the GUI item renderer in the world's block entity pass) draws nothing visible yet | every client sees the blocks, the crit particles, the sounds and the result popping out; the client mod already tracks the animation state, a world-space item renderer is still to be done |
| Saw output only into blocks with Create's DirectBeltInputBehaviour (U16) | the item capability doesn't say whether a block takes "direct belt input" | the saw (and its belt neighbours) hands its items to Levitate blocks with an item handler except vaults; vanilla containers next to a saw are skipped as in Create |
| Sequenced assembly steps on the saw (U16) | the sequenced assembly state belongs to U15 | the saw cuts plain `cutting` recipes; U15 adds the sequenced cutting step |
| Block break sounds of the drill / saw per block sound type (U16) | the server can't name a block's hit sound for a PlaySound packet | `hit.wood` / `hit.grass` / `hit.gravel` / `hit.sand` / `hit.stone` by the block id |
| Saw / drill damage source names (U16) | Bedrock has no custom damage causes | contact damage (death message of a cactus-like cause) |
| Comparator output of packagers (0 / 15), frogports and postboxes (U25) | custom blocks without a block entity can't give comparators a signal | none; read the port / packager state with other means (display links, U24) |
| Packages as physical entities (thrown, put down, on the ground) (U25) | Create's PackageEntity is a custom entity with its own model and physics | thrown / put-down packages are item entities (same velocity); breaking or opening is done with the item |
| Typing an address into a native screen (frogport, postbox, package filter) (U25) | the native screen framework has no text field | the address is typed into a form (all clients); the rest of the port screen is native |
| Chains, hanging packages, frogport tongue and packager tray for **vanilla** clients (U25) | no client-side rendering without the client mod | the client mod draws them; vanilla clients see the static conveyor, frogport and packager models and the postbox flag / lid states |
| The chain conveyor wheel's size (U25) | Bedrock block geometry is limited to 30 px per axis | the wheel is 28 px across instead of Create's 2.5 blocks; the chains still start 1.25 blocks from the centre |
| Riding chains is client-side in Create (U25) | the server can't move a vanilla client smoothly | the server moves the rider every tick (teleport towards the chain point, as Create's 0.25 pull) |
| Create's own sounds (packager, package pop, frogport) (U25) | no original audio assets | levitate.packages.* sound events mapped to vanilla sounds |
| Package filters in U3's filter slots (U25) | `filter::matches` (U3) only knows list / attribute filters | `packages::filterMatches` implements Create's rule; U3's matcher must delegate `levitate:package_filter` to it (follow-up, called out in the U25 PR) |
| Spawn eggs inside opened packages spawn their mob (U25) | the server would need the egg's spawn logic | the egg is given to the player like any item |
| Adding a port's address to a held clipboard (U25) | the clipboard and its data are U24's | no-op until U24 exposes clipboard pages |
| Items floating in the basin (U14) | the client mod has no world-space item renderer yet | the fluid surface is drawn; the items are taken out with an empty hand or seen by funnels; a later U14 PR draws them once an item renderer exists |
| Basin / blaze burner comparator output (U14) | custom blocks can't provide a comparator signal | none yet (Create: basin inputs fill level; burner heat − 1; lit burner 1 / 2) |
| Hollow basin collision (U14) | a custom block has a single collision box | full-block collision; item entities resting on top are collected as if they fell in |
| Press on a moving belt (U14) | no belt contract to hold an item under the press | items on a depot or belt below are pressed through the item capability (one per cycle) |
| Sequenced-assembly pressing steps, potion brewing in the mixer (U14) | they need U15's sequenced assembly and U19's potion fluids | plain pressing / mixing; added with those units |
| Basin spout checks of the funnel facing and the belt direction (U14) | funnel and belt states are U12's / U11's, without a contract | a funnel beside the basin never blocks a spout; a belt below takes outputs whatever its direction |
| Valve handle turning its network by an exact angle (Create's sequenced `TURN_ANGLE` context makes bearings / pistons stop after exactly that angle) (U7) | the kinetics contract has no sequence context yet | the valve generates 32 RPM for `ceil(angle / 9.6) + 2` ticks as in Create; receivers move for that time |
| Valve handle: short click turns, holding opens the angle board (U7) | the server only sees discrete clicks (U3's value box row) | a click turns the handle; the wrench on the handle's front opens the board (form for vanilla players) |
| Re-dyeing a dyed valve handle in the crafting grid (Create's `#create:valve_handles` + dye) (U7) | custom block items have no item tags for recipe ingredients | copper valve handle + dye in the crafting grid; any handle is re-dyed in the world by right-clicking it with a dye (as Create) |
| Large water wheel as one 3×3 model for **vanilla** clients (U7) | a Bedrock block model may be at most 30 px wide | the wheel and its 8 structure blocks each show their own cell of the wheel; the client mod spins the whole wheel |
| Red outline of the missing room when a large water wheel can't be placed (U7) | client-side outline rendering | the action bar message only |
| Waterlogged hand cranks / valve handles (Create's ProperWaterloggedBlock) (U7) | data-driven Bedrock blocks are not waterloggable | they replace water like other custom blocks |
| Hand crank / water wheel / lava wheel advancements (U7) | Bedrock has no custom advancements | none |
| Cart assembler and controller rail as real rails (U21) | Bedrock minecarts only ride vanilla rail blocks; a data-driven block can't be a rail | they are rail-shaped blocks without collision; a minecart on one is guided by the server: held on the rail line at rail height and moved along it at the speed the rail gives it (no off-rail friction), then the next vanilla rail takes it over. A minecart item used on them places a cart there |
| Redstone input of the cart assembler / controller rail (U21) | custom blocks get no neighbour updates and the game's signal levels aren't exposed | power polled every 4 ticks from the neighbours' states (wire level, redstone block, torches, levers, buttons, repeaters / comparators, observers, `redstone_signal`, solid blocks on a lit torch) |
| "Pause actors while rotating" (U21) | the contraption engine has no API to pause a contraption's actors | the mode is kept and turns the structure like "Always face toward motion" |
| Furnace minecart contraptions and the chest minecart contraption item (U21) | Bedrock has no furnace minecart; a chest minecart's own inventory can't travel in the item | contraptions ride rideable and chest minecarts; only rideable carts are picked up (`minecart_contraption`) |
| Picking up a minecart contraption (U21) | the engine has no "discard" | the structure is placed and its blocks removed again silently (refused when something is in the way: `lv.bearings.pickup_blocked`); the item carries the engine's own record of the structure |
| Coupling chains drawn between carts (U21) | no client-side line rendering for entities | couplings work but aren't drawn |
| Sequenced gearshift angle limits on bearings (U21) | the kinetic contract has no sequence context | bearings turn at their speed; the gearshift's own timing still applies |
| Crushing wheel controller letting only the crushed entity through (U13) | Bedrock block collision can't depend on the entity | the controller keeps Create's 13 px slab for everyone; the item being crushed is moved through it by the server; mobs are hurt where they stand on it |
| Create's crushing death messages, looting 2 for crushed mobs, fan damage types (U13) | Bedrock has no data-driven damage types or per-kill looting | crushing uses the contact cause, fans fire / lava / drowning causes; drops are the mob's plain loot; the strings exist in the lang files |
| Fan processing state kept on the item entity (U13) | Bedrock item entities can't change their item for clients and carry no free data | a processed item entity is replaced by a new one with the same motion; the processing state lives in the server's memory (restarts after a reload) |
| Haunted horses keep their data (U13) | no generic entity NBT copy between types | a skeleton horse is spawned in place; saddle, armour and taming are not carried over |
| Placing a crushing wheel or nozzle where Create refuses it (U13) | the placement can't be vetoed per position for custom blocks | the block pops off right after placement |
| Millstone / crusher ambient sounds, air current wind sound (U13) | Create's soundscapes are client-side sound assets | not played (U2's note on kinetic soundscapes) |
| Pulling nozzle explosion with more than 256 entities (U13) | rare edge case | not ported |
| Air currents through blocks made of several boxes (U13) | the server only gets one collision box per block | a partial block stops the current by its bounding box (stairs, walls and hoppers block it like a full block); fan-transparent blocks pass as in Create |
| Comparator output of fluid tanks (U18) | Bedrock comparators only read vanilla containers; a custom block's comparator signal can't be supplied without hooking the shared (folded) `hasComparatorSignal` of every block | `steam::comparatorLevel(pos)` gives Create's level for units that read it (threshold switch, display links); vanilla comparators read nothing |
| Fluid inside tanks and the moving engine piston for **vanilla** clients (U18) | no client-side rendering without the client mod; a fluid level per block would need a block state per level and fluid | the client mod draws them; vanilla clients see the tank (windows show an empty inside) and a static engine |
| Whistle redstone input (U18) | custom blocks get no redstone neighbour update | the whistle polls its neighbours' redstone power every 2 ticks (as the sticker, U4) |
| Whistle tone as one looping sound with a changing pitch (U18) | Bedrock's server can't hold a looping sound instance on clients | the tone sample (0.6 s) is restarted every 10 ticks at the whistle's pitch, so it sounds continuous; the chiff plays when it starts |
| Boiler gauges drawn on the tank sides (U18) | Create draws them with its block entity renderer | the goggle lines (`/create steam`, U8's overlay) show the same status |
| Steam particles of whistles / engines as Create's custom steam jet particle (U18) | no custom particle shipped yet | vanilla's water evaporation particle at the whistle's mouth / the engine's front, for every client |
| Deployer fake player (U15) | Bedrock has no fake players | one simulated player per dimension ("Deployer", invisible, invulnerable, survival) acts for every deployer: it is moved into the hand for each action and waits above the build limit in between; it shows in the player list |
| Redstone input of the deployer and the arm (U15) | custom blocks get no neighbour updates | power is polled every tick (same sources as the sticker) |
| Item held by a deployer drawn in its hand, item held by the arm (U15) | no world-space item renderer in the client mod yet | the hand pose shows whether it holds something; the item is in the goggles / log; vanilla clients see static models |
| Transitional item progress bar and tooltip (U15) | item bars come only from durability and tooltips are client-side | the tooltip lines are the item's lore (server language); no bar |
| Deployer kills dropping no experience (U15) | Bedrock has no hook between a kill and its orbs | orbs drop as for a player kill; item drops go into the deployer |
| Deployer "fist bump" easter egg (U15) | cosmetic advancement only | not reproduced |

| Goggle overlay next to the crosshair for **vanilla** clients (U8) | no client-side drawing without a client mod | the same lines in the action bar (indentation trimmed), refreshed every `overlayInterval` ticks |
| Kinetic stats and "Hold [Shift] for Summary" in item tooltips for **vanilla** clients (U8) | Bedrock builds item hover text on the client from the item alone | the client mod appends them to the hover text; vanilla clients see the plain name. On a server the client mod reads its own `config/kinetics.json` stress table (Create syncs the server's) |
| The wrench's animated cog in the hand (U8) | Bedrock items have no custom item renderer | a static item sprite |
| Strikethrough in Create's speed text of an overstressed network (U8) | Bedrock text has no strikethrough format | the text is only dark gray |
| Drops of sneak-wrenched blocks from their loot tables (U8) | the server has no loot roll for a block that isn't broken by a tool | the block's pick item (its own item; redstone for wire, the repeater for a repeater, ...), which is what Create's `#wrench_pickup` blocks and Levitate's blocks drop |
| Any glass (`#c:glass_blocks`) in the goggles recipe (U8) | a Bedrock recipe key is one item or an item tag, and there is no glass tag | plain glass |

| Fluid moving through glass pipes drawn by a block entity renderer, with the flow front advancing (U17) | custom blocks have no block entity renderer on Bedrock | the glass pipe's `levitate:fluid` state shows the fluid filling the pipe as soon as a flow starts (all clients, with or without the mod); the front's progress is not drawn |
| Waterlogged pipes, pumps and valves (U17) | Bedrock custom blocks have no waterlogged state the server can set | open ends don't drain or fill waterlogged blocks; pipes in water are not waterlogged |
| Pipe particles (drips at rims, splashes, the pouring stream of open ends) (U17) | Create's fluid particles need fluid-tinted particle types | not shown yet |
| Instant potion effects from open ends at half strength (U17) | Bedrock applies instantaneous effects through `addEffect` at full strength | instant potion effects apply at full strength |
| Ropes / elevator belts of *moving* pulleys for **vanilla** clients (U20) | the rope length changes every tick; it can't be a rigid contraption block, and placing rope blocks while moving would collide with riders | the client mod draws rope, belt and magnet; vanilla players see the moving structure, and the ropes and magnet as blocks once it stops |
| Redstone input of gantry shafts and elevator contacts (U20) | custom blocks get no neighbour updates | polled every tick (shafts) / every 2 ticks (contacts), like the sticker |
| Climbable ropes and magnets (U20) | data-driven blocks can't be climbable | they are thin solid blocks |
| Native elevator contact screen (U20) | the native screen framework (U3) has no text field | the contact's form (floor identifier, description, door control) is used for every player |
| Elevator door control acting on doors (U20) | the sliding / train doors are U10's | the mode is stored per contact (`data["pistons"]["door"]`) for U10 to apply |
| Sequenced gearshift distance limits on pistons / gantries (U20) | Create passes the sequence context through its kinetic block entities; the U2 contract has no such channel | pistons and gantries run until their end / collision |

| Items riding belts / held on depots / thrown by ejectors drawn by the belt renderer (U11) | Bedrock has no item renderer for vanilla clients, and the client framework has no world-space item renderer yet | the server shows each stack as an item entity (no pickup, no despawning, they don't merge by themselves, teleported along the belt every tick; for every client, the client mod included); with `displayItems` off nothing is shown |
| Sloped belt collision shape (U11) | a custom block has one collision box | slopes collide as a half slab; entities are carried up / down by the belt code |
| Casing drawn on sloped / vertical belts (U11) | the sloped casing would leave the block geometry bounds | the casing state is kept (and removable with the wrench) but only horizontal belts draw it |
| Belt drops when exploded or removed by commands (U11) | the server can't tell an explosion from a command removing the block | the connector / shaft drops only for survival players breaking the belt; items on it always drop |
| Comparator output of depots / ejectors (U11) | custom blocks can't give a comparator signal | none |
| Redstone input of the ejector (U11) | custom blocks get no redstone neighbour update | polled every 4 ticks (as U4's sticker) |
| Belt connector selection preview line (U11) | Create draws it client-side every frame | server particles along the planned belt every 5 ticks while the connector is held |
| Display board text for **vanilla** clients (U24) | no client-side text rendering on blocks without the client mod | the lines as a name tag in front of a turning board (invisible entity, colours as § codes, no flap animation); the client mod draws the split flaps |
| Nixie tube glyphs for **vanilla** clients (U24) | same | the two characters as a coloured name tag above each tube |
| Display link / nixie tube redstone input (U24) | custom blocks get no redstone neighbour updates | polled every 2 ticks from the neighbours' states (wire, torches, levers, buttons, plates, repeaters, comparators, observers, redstone blocks); strong power through solid blocks is not seen |
| Clipboard text editing in the native screen (U24) | the client mod's screen framework has no text field | the native screen shows pages, checkboxes, erase and page buttons; its edit button opens the server form for the text |
| "Clipboard Actions" hover tip and block outline while holding a clipboard (U24) | per-frame client logic for every block's settings | the action bar tells what was copied / pasted |
| Deaths statistic of the respawn anchor source (U24) | Bedrock has no deaths statistic | Levitate counts player deaths per world (`lv:display:deaths`) |
| Display link placement by a SimulatedPlayer (U24 selftests) | `simulateUseItemInSlotOnBlock` does not place this block item | the selftest places the block itself after the item logic; the client e2e places it for real |

| Items drawn inside chutes by a block entity renderer (U12) | custom blocks have no block entity renderers | client-only display item entities (packets only, never a server entity, so nobody can pick them up) follow each moving stack; vanilla clients and the client mod see the same |
| Smooth (lerped) funnel flaps (U12) | no per-frame block animation for custom blocks without a renderer | the flap is a block state shown for 4 ticks after a transfer (both kinds of clients) |
| Editing a filter's amount by *holding* the filter slot (U12) | the server sees no held click (see the value box row) | brass funnels and smart chutes have a separate "Extracted Amount" value box next to the filter slot |
| Funnel / smart chute redstone input by neighbour updates (U12) | custom blocks get no redstone neighbour update | polled every 2 ticks like the sticker (same sources) |
| Comparators reading a vault (U12) | Bedrock only asks container blocks for analog signals | the vault's signal is handed to the comparator's circuit component before it evaluates (ComparatorCapacitor hook) |
| Belt funnels, tunnels on belts, chutes pulling from belts (U12) | waiting for U11's belt | the hooks exist (`beltlink`, FunnelsExt.h); tunnels already work with any registered segment |

| Create's connected-texture corners (U10) | a Bedrock face picks its texture from block states; inner corners would need the 12 diagonal neighbours as states | faces join along their four edges (6 neighbour states, 16 textures); the small inner-corner notch of Create's CTM is not drawn; glass panes don't connect their textures |
| Random ticks of copper roofing (U10) | data-driven blocks get no random ticks without scripting | the unit keeps the positions of its weathering blocks (saved with the world) and ticks them at the game's `randomTickSpeed` rate |
| Climbable ladders / scaffolding, multi-box collision (U10) | no JSON component for either | the block types get the Climbable property and a collision hook (server and client mod); vanilla clients are corrected by the server's movement authority, so climbing and stairs feel slightly rubbery for them |
| Sliding door animation, open panel beyond the block (U10) | Bedrock models are static per state and must fit 30 px | the open state shows the slid / folded panel clipped to the model bounds, no sliding animation |
| Door / trapdoor redstone input (U10) | custom blocks get no neighbour updates | polled every 2 ticks from adjacent power sources (wire, levers, buttons, torches, repeaters, comparators, observers, plates, placards, blocks strongly powered by them); the direction of wire / repeaters is not checked |
| Item on the placard, copycat material, brackets drawn on vanilla clients (U10) | a block model can't show another item / block | the data and every behaviour exist on both; vanilla clients see the placard frame, the copycat base and the plain shaft; drawing them is the client mod's job (not yet) |
| Copycat material state cycling (U10) | Bedrock block items always place their default state | materials are applied in their default state; clicking again with the same block does nothing |
| Placement helpers of girders / table cloths (U10) | — (not done yet) | girders and table cloths are placed like normal blocks |
| Redstone neighbour updates of the clutch, gearshifts and sequenced gearshift (U6) | custom blocks get no redstone neighbour update | they read their neighbours' signal from the circuit system every tick (same reaction time as Create's scheduled tick); the sequenced gearshift reacts to power *changes* only, as Create's neighbour updates |
| Comparator reading a gauge / sequenced gearshift (U6) | `getComparatorSignal` of custom blocks can't be overridden (a folded function) | a hook on the comparator's evaluation feeds it the block's output when its rear (directly or through a solid block) is one of ours |
| Smooth gauge needle for **vanilla** clients (U6) | block models are static per state; cube rotations are limited to 22.5° steps | the needle state snaps to the nearest 22.5°; the client mod draws the eased needle |
| Gauge dial particles (U6) | Create's coloured dust particles need a client particle with a colour parameter | not shown yet |
| Sequenced gearshift stopping a bearing / piston exactly (U6) | Create passes a "sequence context" through the kinetic network to the driven machine; the wave-1 kinetics contract has no such channel | the instructions run for Create's durations (which overshoot by two ticks); exact stops need a kinetics contract extension |
| Wrench on transmission blocks (encased cog shaft toggling, removing casings) (U6) | the wrench (U8) has no per-block hook in the wave-1 contracts | left to U8's wrench (rotate / pick up); the shaft ends of encased cogs are set when encasing |
| Integrated-cog placement helpers (cogs placed against mixers / crafters) (U6) | those blocks are other units' | plain placement rules apply |

| Holding the toolbox key on a **vanilla** client (U31) | the server can't see a held key without the client mod | `/create toolbox` opens the same entries as a form |
| Dyeing a toolbox in the crafting grid keeps its contents (U31) | Bedrock crafting results carry no data of the ingredients | the crafted toolbox is empty; right-clicking a placed toolbox with a dye recolours it with its contents (as Create does too) |
| Tree fertilizer as one recipe with "any small flower" (U31) | Bedrock recipes take one item or tag per ingredient and vanilla flowers have no common item tag | one recipe per flower × coral (two of the same flower) |
| Toolbox lid / drawer animation, toolbox comparator output (U31) | custom blocks have no block entity renderer; comparators only read containers | static model (closed lid); no comparator output yet |
| Enchantments Capacity (backtank air) and Potato Recovery (U31) | Bedrock has no data-driven enchantments | air is always `airInBacktank`; projectiles are recovered with the base 12.5 % |
| Armour toughness and knockback resistance of the netherite diving gear (U31) | `minecraft:wearable` only has `protection` | protection as Create; no toughness / knockback resistance |
| Netherite helmet lava vision and the diving helmet's clearer underwater fog (U31) | fog is client-side | no fog change |
| Diving boots movement (U31) | players move themselves (client-authoritative input); the server can only push them | sinking and the ×1.3 walking speed are sent as motion packets while in water (less smooth than Create) |
| Extendo grip reach for **vanilla** clients (U31) | the client limits its own block picking range | the pick range is raised in GameMode (server validation and the client mod's local player); vanilla clients keep their normal reach |
| Potato projectiles as entities (U31) | no custom projectile entities without a client-side renderer | the server simulates the flight (Create's drag / gravity, block and entity hits) and shows the ammo as an item entity that can't be picked up; sticky projectiles don't stick; glow berries / glistering melon have no glowing (Bedrock has no glowing effect); golden apples don't cure zombie villagers; custom sounds use vanilla ones |
| Tree fertilizer growing without room (U31) | Create grows the tree in a virtual world and copies it; Bedrock's tree features aren't callable into another world | Bedrock's own sapling growth, repeated until it grows (needs the room Bedrock trees need) |
| Linked controller input for **vanilla** clients (U31) | movement keys are only visible through Bedrock's movement input | the movement input the server receives is polled (the player still moves); the client mod captures the keys as Create |
| Worldshaper preview outline (U31) | no world-space line renderer yet | no preview; the changed blocks are logged and appear at once |
| Lectern controller comparator output 15 (U31) | comparators only read containers | none |


| Create's data-driven ore / layered ore features (U9) | Bedrock feature JSON has no layered-ore feature and no config filter | both run natively in the chunk decoration hook with Create's numbers and patterns (own RNG, so positions differ from Java for the same seed) |
| Connected textures of layered stone and pillars (U9) | no connected textures on Bedrock | the plain layered / pillar / cap textures |
| Tall wall sides and the post forced by a block above (U9) | the connection trait only gives the four connections | low sides, post unless straight; wall collision is 1 block high (Bedrock's limit), Java's 1.5 |
| Mining speed per tool tier on custom blocks (U9) | not data-driven in this format | Create's hardness as destroy time; the drop rules (pickaxe / iron tier) are exact |
| Chromatic compound charge bar, stack size 1 when charged, absorbing light from belts / depots (U9) | durability bars and per-stack max sizes aren't data-driven; belts are another unit with no transported-item contract | the charge is kept in the item data (stacks of equal charge merge up to 16); placed light sources only |
| Furnace cook times and xp of the unit's smelting recipes (U9) | Bedrock furnace recipes have fixed times / xp | Bedrock's 10 s / 5 s / 30 s, which equal Create's 200 / 100 / 600 ticks |
| End rod particles of the block of experience (U9) | no data-driven block particles | none |

Units add rows here in their PRs when they find something else impossible.

## 9. Work plan

Unit U<n> owns selftest `testPos` dx range `100n..100n+99`, selftest prefix `<unit>.*`, config
section `<unit>`, lang files `<unit>.json`, e2e scenarios `<unit>.ps1`, texture script `<unit>.ps1`.

### Wave 1 (contracts; all of wave 2 depends on them)

| Unit | Name | Owns | Depends on |
|---|---|---|---|
| U1 | spec | this file | — |
| U2 | `kinetics` | §3.1, `src/{feature,client,core}/kinetics/`, `config/kinetics.json` (stress table) | — |
| U3 | `common` | §3.2–3.3, `src/feature/common/`, `src/feature/net/`, `src/client/screen/`, filters (§2.26) | — |
| U4 | `contraption` | §3.4, §2.17, `src/{feature,client}/contraption/` | — |
| U5 | `tracks` | §3.5, `track`, `src/{feature,client,core}/tracks/` | — |

### Wave 2 (features; depend only on wave 1)

| Unit | Name | Owns |
|---|---|---|
| U6 | `transmission` | §2.1 |
| U7 | `generators` | §2.2 |
| U8 | `goggles` | §2.3, goggles overlay, kinetic tooltips |
| U9 | `materials` | §2.4, world gen, decorative stone |
| U10 | `building` | §2.5 |
| U11 | `belts` | §2.6 |
| U12 | `funnels` | §2.7 |
| U13 | `bulk` | §2.8 |
| U14 | `basin` | §2.9, heat sources |
| U15 | `assembly` | §2.10, sequenced assembly |
| U16 | `crafter` | §2.11 |
| U17 | `pipes` | §2.12 |
| U18 | `steam` | §2.13 |
| U19 | `fluids` | §2.14 |
| U20 | `pistons` | §2.15 |
| U21 | `bearings` | §2.16 |
| U22 | `actors` | §2.18 |
| U23 | `redstone` | §2.19 |
| U24 | `display` | §2.20 |
| U25 | `packages` | §2.21 |
| U26 | `stock` | §2.22 |
| U27 | `trains` | §2.23 (station, bogeys, controls, schedules) |
| U28 | `signals` | §2.23 (signals, observers) |
| U29 | `schematics` | §2.24 |
| U30 | `ponder` | Ponder (client) |
| U31 | `equipment` | §2.25 |

Cross-unit integration (e.g. a funnel on a belt, a spout over a depot, a deployer on a contraption)
goes through wave-1 contracts (capabilities, actors, BlockData); a unit never includes another
wave-2 unit's headers. When both sides exist, the integration works without either unit naming the
other.
