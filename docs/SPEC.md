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
`rotation_speed_controller`, `speedometer`, `stressometer`, `cuckoo_clock`, `mysterious_cuckoo_clock`,
`flywheel` (U49), `belt` connector item is U11.
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
| cuckoo_clock, mysterious_cuckoo_clock (U49) | `levitate:horizontal_facing` (0 south, 1 west, 2 north, 3 east: the dial's side), `levitate:hour` / `levitate:minute` 0..11 (static hands for vanilla clients, minutes in 5-minute steps), `levitate:cuckoo` (0 doors shut, 1 pig out, 2 creeper out) |
| flywheel (U49) | `levitate:axis` |

Drops: encased shafts drop a shaft, encased cogwheels their cogwheel / large cogwheel (as Create);
a gearbox with a horizontal axis drops a vertical gearbox. Recipes (Create 6.0.10, `crafting/kinetics`):
shaft ×8 = andesite alloy / andesite alloy; cogwheel = shaft + planks; large cogwheel = shaft + 2 planks
or cogwheel + planks; gearbox = cogwheel ×4 around an andesite casing (` C / CBC / C `); vertical
gearbox = `C C / B / C C`; gearbox ↔ vertical gearbox and speedometer ↔ stressometer convert
shapeless; clutch = andesite casing + shaft + redstone; gearshift = andesite casing + cogwheel +
redstone; encased chain drive = andesite casing + 3 iron (or zinc) nuggets; adjustable chain
gearshift = encased chain drive + electron tube; sequenced gearshift = brass casing + cogwheel +
electron tube; rotation speed controller = precision mechanism / brass casing; speedometer = compass
/ andesite casing; cuckoo clock = planks / andesite casing / clock; mysterious cuckoo clock = cuckoo clock with
gunpowder on its four sides (` C / CBC / C `); flywheel = shaft surrounded by 8 brass ingots (U49).
Deploying: shaft + planks → cogwheel, cogwheel + planks → large cogwheel. Recipes
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
- Drops: itself; the large wheel drops itself, the structure drops nothing (§3.6.7).
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
and two brass lenses on the head; a small copy in the hand). The wrench in the hand is an
attachable too (U48, `tools/models/wrench.ps1`): the item sprite extruded into pixels with a small
gear on the shaft that turns about the shaft while the wrench is drawn (Create's
WrenchItemRenderer: ScrollValueHandler's passive 0.5° a tick). First and third person have their own
geometry and render controller (conditional render controller entries on `c.is_first_person`):
in third person it hangs from the player model's `rightItem` bone, turning about the hand; the
first-person arm has no such bone, so there the root bone is bound to the slot's hand
(`q.item_slot_to_bone_name`, as the vanilla spear); an animation controller switches the two poses.
It comes with the resource pack, so vanilla clients see it too. Mechanics in §3.6.8 (U8).

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

132 blocks (Create 6.0.10's ids), mechanics in §3.6.10:

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
`item_vault`, `creative_crate`. Behaviour in §3.6.12 (U12).

| Block | States | Drops / recipe (Create 6.0.10) |
|---|---|---|
| `andesite_funnel`, `brass_funnel` | `levitate:facing` 0..5 (the side the opening faces, away from the attached block), `levitate:extracting`, `levitate:powered`, `levitate:flap` | itself; 2 from andesite alloy over dried kelp / 2 from electron tube, brass ingot, dried kelp (column) |
| `andesite_tunnel`, `brass_tunnel` | `levitate:axis` 0 x / 2 z (the belt's axis), `levitate:shape` 0 straight, 1 window, 2 closed, 3 T left, 4 T right, 5 cross | itself; 2 from 2 andesite alloy over 2 dried kelp / 2 from electron tube, 2 brass ingots, 2 dried kelp |
| `chute` | `levitate:facing` 0 down or 1..4 diagonal (north, south, west, east), `levitate:shape` 0 normal, 1 window, 2 intersection, 3 encased | itself; 4 from iron sheet, iron ingot, iron sheet (column) |
| `smart_chute` | `levitate:powered` | itself; brass sheet, chute, electron tube (column) |
| `item_vault` | `levitate:axis` 0 x / 2 z, connected textures `levitate:len` / `levitate:u` / `levitate:v` / `levitate:large` | itself; iron sheet, barrel, iron sheet (column) |
| `creative_crate` | — | itself; creative only (no recipe) |
| `item_hatch` (U49) | `levitate:facing` 2..5 (FacingID towards the container it sits on), `levitate:open` | itself; andesite alloy + iron trapdoor (shapeless) |

The recipes use U9's materials (andesite alloy, brass, sheets, electron tube): they wait in
`assets/pending_recipes/funnels/` until U9 is merged. Belt funnels (`andesite_belt_funnel`,
`brass_belt_funnel`: a funnel placed on the side of a belt) come with the belt API (§3.6.12).

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
  Shaft input from below (above on the ceiling) and a small cog on its base that meshes with
  cogwheels beside it (Create's ArmBlock is an ICogWheel, U50). Breaking drops the held item.
- Recipes (Create 6.0.10): deployer = electron tube / andesite casing / brass hand (column);
  mechanical arm = `LLI` / `L  ` / `RC ` (L brass sheet, I electron tube, R precision mechanism,
  C brass casing). Their ingredients are U9 / U10 items: the files wait in
  `assets/pending_recipes/assembly/`.

### 2.11 Crafting tools (U16)

`mechanical_crafter`, `mechanical_saw`, `mechanical_drill` (behaviour in §3.6.16).

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
(details in §3.6.17).

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
  drop themselves; the fluid inside is lost when its share no longer fits, §3.6.18). States
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
`redstone_contact`, `desk_bell`, `rose_quartz_lamp` (U49). Create 6.0.10 registers the smart observer and the threshold switch under their
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
comparator. U49: desk bell = andesite casing + golden sheet; rose quartz lamp = polished rose quartz +
redstone + zinc ingot (shapeless, in the BP).

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

- `stock_link` (Create's packager link): face-attached like a lever (`levitate:face` 0 floor /
  1 wall / 2 ceiling, `levitate:facing` Java 2D facing, `levitate:powered`); it belongs to the
  packager it is attached to. `stock_ticker`: `levitate:facing` (faces the player, sneak reverses).
  `factory_gauge`: face-attached, `levitate:panels` = bitmask of its 4 panels (each panel is a
  quadrant of the model; bone visibility follows the mask). `redstone_requester`: `levitate:axis`
  (0 x / 1 z, the player's horizontal axis), `levitate:powered`. `shopping_list`: stacks to 1, no
  recipe (handed out by shops).
- Recipes (Create 6.0.10): stock link = transmitter over item vault; factory gauge (×2) = stock
  link + precision mechanism (shapeless) — both in `assets/pending_recipes/stock/` (U9 / U12
  items); stock ticker = glass, stock link, gold ingot (column); redstone requester = redstone,
  stock link, iron ingot (column); each linked block alone in the crafting grid clears its tuning
  (`*_clear` recipes).

### 2.23 Trains (U5 tracks, U27 trains, U28 signals)

U5: `track`, curved track (no separate id: stored in BlockData + graph), `track_station` is U27.
U27: `track_station`, `small_bogey`, `large_bogey`, `controls`, `schedule` (item), `railway_casing` is
U10, `train_door` is U10. U28: `track_signal`, `track_observer`.

U28 (details in §3.6.28): `track_signal` ("Train Signal", states `levitate:signal_type` 0 entry /
1 cross, `levitate:signal_state` 0 off / 1 red / 2 yellow / 3 green, `levitate:powered`; post
with a lamp head, metal sound, 1.5 s to mine, drops itself) and `track_observer` ("Train
Observer", `levitate:powered`, full block with a lens on top that lights up, emits redstone 15
on every side while powered). Both are placed by first right-clicking the track they should
watch (Create's track targeting items). Recipes: `track_signal` ×4 = railway casing + electron tube
(shapeless), `track_observer` ×2 = railway casing + any pressure plate (shapeless) ⚠.
- `track_station` ("Train Station"): state `levitate:assembling` 0/1 (the flag shows assembly mode),
  drops itself. Recipe (Create 6.0.10): shapeless railway casing + compass → 2.
- `small_bogey` / `large_bogey`: state `levitate:bogey_axis` 0 x / 1 z (along the track); no item,
  created with train casing on an assembly track (§3.6.27), drop train casing. Wheel radius 6.5 / 12.5
  px, wheel points ±1 block from the bogey (both sizes). The bogey stands in the block above the
  track: its model reaches from the rails (wheels down to 12 px below the block) to the block's top
  (a full-width top plate; collision and selection a full block), so the carriage block above sits
  directly on it (the model's wheels are 13 / 14 px across: Bedrock models are at most 30 px per axis). Wrench: swaps small ↔ large (Create has one
  bogey style), sneak-wrench dismantles (gives the casing).
- `controls` ("Train Controls"): state `levitate:facing` 0..5 (horizontal values used; Create's
  FACING points from the controls towards the driver: placed facing the player, sneaking reversed).
  Recipe: lever / railway casing / precision mechanism (shaped column).
- `schedule` ("Train Schedule", stack 16): its schedule lives in the item's user data
  (`lv:schedule`, Create's NBT layout: `Entries` [{`Instruction` {Id, Data}, `Conditions` [[{Id,
  Data}]]}], `Cyclic`, `Progress`). Recipe: sturdy sheet (Create's `c:plates/obsidian`) + paper → 4.
- The three crafting recipes use U10's railway casing and U9's precision mechanism / sturdy sheet.

### 2.24 Schematics (U29)

`schematic_table`, `schematicannon`, `empty_schematic`, `schematic`, `schematic_and_quill`,
`crafting_blueprint` (item), `placed_crafting_blueprint` (the placed blueprint's sections, §8).
Empty schematics, schematics and the schematic and quill stack to 1, blueprints to 64. Recipes
(Create 6.0.10): empty schematic = paper + light blue dye (shapeless); schematic and quill = empty
schematic + feather; schematic table = `WWW / S / S` (wooden slabs, smooth stone); schematicannon =
` I / LIL / SDS` (iron block, logs, smooth stone, dispenser); crafting blueprint = painting +
crafting table. Details in §3.6.29.

### 2.25 Equipment (U31)

`<colour>_toolbox` (16), `copper_backtank`, `netherite_backtank`, `copper_diving_helmet`,
`copper_diving_boots`, `netherite_diving_helmet`, `netherite_diving_boots`, `extendo_grip`,
`potato_cannon`, `wand_of_symmetry`, `handheld_worldshaper`, `linked_controller`, `sand_paper`,
`red_sand_paper`, `tree_fertilizer`, `lectern_controller` (block, no item), and since U49
`peculiar_bell`, `haunted_bell`, `cardboard_helmet`, `cardboard_chestplate`, `cardboard_leggings`,
`cardboard_boots`, `cardboard_sword`. `minecart_contraption`
is U21's (it is made by its cart assembler). Mechanics in §3.6.31.

- Bells (U49): `levitate:facing` (FacingID 2..5, Java's FACING), `levitate:attachment` (0 floor,
  1 ceiling, 2 single wall, 3 double wall), `levitate:powered`, `levitate:part` (1: the frame only,
  the client mod's mesh; never in the world). Brittle, pickaxe, hardness 3 / resistance 6 (Create's
  soft metal), anvil sounds; drop themselves. Peculiar bell = block of brass over a brass sheet;
  the haunted bell is haunted from it (`assets/recipes/haunting/haunted_bell.json`, U13's fans) or
  converted over soul fire. Models: `tools/models/curiosities.ps1`.
- Cardboard armour (U49, Create's cardboard armour material): protection 1 each, durability 44 /
  64 / 60 / 52, enchantability 4, repaired with cardboard, 50 s of furnace fuel, tags
  `minecraft:is_armor` and `minecraft:trimmable_armors` (smithing-table trims). Recipes: the
  vanilla armour shapes of cardboard. Cardboard sword: unbreakable (Create's tier has 0 uses),
  +5 attack damage, enchantable (value 1) with Knockback only, 50 s of fuel, stacks to 1; recipe
  2 cardboard over a stick.

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
  "eat" use); tree fertilizer stacks 64. The netherite helmet, backtank and boots are Create's
  netherite material: +3 armour toughness (a Mob::getToughnessValue hook) and +0.1 knockback
  resistance (an attribute modifier) per piece. Toolboxes also have `levitate:toolbox_lid` 0..2
  (0 closed, 1 open, 2 body only for the client mod).
- Recipes (Create 6.0.10): toolbox dyeing (any toolbox + a dye, 16 shapeless recipes; the result
  keeps the contents and name of the dyed toolbox),
  copper diving helmet (copper ingots around glass), sand paper (paper + sand), red sand paper
  (paper + red sand), tree fertilizer ×2 (2 small flowers + a coral + bone meal: a recipe per
  unordered pair of the 14 small flowers × 5 corals, from `tools/gen/equipment.ps1`), netherite helmet / boots / backtank (smithing: upgrade template + the copper
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
- **Persistence**: the network state of each block (block, states, speed, source, network,
  generated speed, overrides) is its `NodeRecord` in U3's BlockData (`data["kinetics"]`), written
  back from the solver before every save and before a contraption takes the block
  (`blockdata::onWriteBack`), restored when the world loads, then re-attached as Create's block
  entities do on load. Worlds saved by wave 1 (`lv:kinetics:c:<dim>:<cx>:<cz>`) are migrated once on
  load (log `kinetics: moved N block records from M chunks into block data`). A block a contraption
  puts back keeps its generated speed and stress overrides (`blockdata::onPut`). A slow world
  check re-reads blocks that changed without an event.
- **API for machines**: `speedAt`, `theoreticalSpeedAt`, `nodeAt`, `networkAt`, `networkMembers`,
  `isOverstressed`, `stressImpactOf` / `stressCapacityOf`, `onSpeedChanged(blockId, fn)` (batched
  once per tick), `onBreak(fn)`.
- **Angle-limited rotation** (Create's `SequenceContext`, U32): the sequenced gearshift (U6) calls
  `setSequenceContext(pos, {TurnAngle | MoveDistance, value})` while it runs an instruction;
  `sequenceContextAt(pos)` gives a machine the instruction of the nearest block up its source chain,
  with the value scaled by `|speed here| / |speed there|` (a bearing driven at half the gearshift's
  speed turns half the angle). Machines read it when their speed changes and stop after turning (bearings,
  turntables: degrees) or moving (pistons, pulleys, gantries: blocks) that far, as Create's
  `sequencedAngleLimit` / `sequencedOffsetLimit`. Not persisted (the gearshift sets it again).
  Every instruction carries a `serial` (U37; stamped by `setSequenceContext` when 0, starting at
  the wall clock so numbers never repeat across restarts): a machine also takes a new instruction
  that arrives at an unchanged speed (two equal steps in a row), and reads each one once (a speed
  change reported after it already read the instruction doesn't restart the limit). The gearshift
  saves the serial with its run and declares the same one again after a load, so machines keep the
  limit they had counted down (saved with them).
- **Command**: `/create kinetics <pos>` dumps the block and its network; `/create kinetics <pos>
  speed <rpm>` (Levitate debug) sets a generator's speed.
- **Client** (client mod only): a kinetic block with a `rotorState` (int state, e.g.
  `levitate:rotor`) has three models: `0` the full static model (what the world holds and vanilla
  clients see), `1` its non-rotating parts (the client mod tessellates this in the chunk mesh), `2..`
  its rotating parts (`spec.parts`, default one part around the rotation axis). The client mod draws
  each rotating part every frame at Create's angle `(ticks + partialTicks)·speed·0.3 + offset`
  degrees, where offset is 22.5° when the sum of the block's coordinates off the axis is even, else
  0 (11.25 for large cogs), plus the spec's `angleOffset` — so meshing cogs interlock.
- **Speed sync** (client mod only): U3's companion protocol, channel `kinetics.sync`
  `{"batches": [...]}` (one `core/kinetics/SyncCodec.h` batch per chunk, ≤ 48 KiB of batches per
  message). After the handshake the server sends a full sync of the player's dimension (again after
  each dimension change; the first batch resets the client's copy), then once per tick the changed
  speeds. Vanilla clients get nothing. Client units read the speeds with
  `client/kinetics/KineticsClient.h` (`syncedSpeed`, `onSync`).
- **Kinetic feedback** (U37, `src/feature/kinetics/Feedback.cpp`, rules in
  `core/kinetics/Feedback.h`). Sent by the server to every client: the particle
  (`levitate:rotation_indicator`) and the sounds (`levitate.kinetics.*`) are in Levitate's resource
  pack, so vanilla clients and the client mod see / hear the same thing (one path, never doubled).
  - *Rotation indicators* (Create's GeneratingKineticBlockEntity.updateGeneratedRotation,
    KineticEffectHandler, RotationIndicatorParticle): a generator **without a source** whose speed
    level (none / slow / medium / fast, `speedLevelOf`) changes queues them (placing, starting,
    stopping, a value box / sail / boiler change; a direction change alone doesn't); 2 ticks later
    (`queueRotationIndicators` countdown) the block, if it still turns (effective speed ≠ 0, so not
    while overstressed), sends one burst of 20 particles from its centre to every player within 32
    blocks **wearing Engineer's Goggles** (`goggles::isWearingGoggles`; Create's particle is
    invisible to everyone else). The particles circle the rotation axis: colour of the speed level
    (slow `#22FF22`, medium `#0084FF`, fast `#FF55FF`), angular speed 10 / 20 / 30° per tick signed
    by the direction, angle `(time·speed) mod 360 − speed/2·age²/lifetime` (+180° for a negative
    speed on the Y axis), radius from `.75` easing to `.65` (large cogs `1` → `1.125`) by 10 % of the
    gap per tick, life 10 + 0..21 ticks, fading out (alpha `1 − (age − life/2)/life`) and towards the
    colour mixed half with white over the second half of their life; an 8-frame glint texture
    (`tools/textures/gaps.ps1`). One log line per burst that reached a player:
    `kinetics: rotation indicators at <x y z> (<rpm> RPM) for <n> players`.
  - *Ambient soundscapes* (Create's KineticBlockEntity.tickAudio, SoundScapes, SoundScape;
    `config/gaps.json` `enableAmbientSounds`): every rotating block (effective speed ≠ 0) within 16
    blocks of a player (block positions, squared distance < 256) adds itself to the *kinetic* group,
    cogwheels / large cogwheels / encased cogs (spec `cog`) and gearboxes also to the *cog* group,
    at pitch `clamp(|speed|/256 + .45, .85, 1)`. Sources are grouped per player by (group, pitch
    group: < .7, < .9, < 1.1, < 1.3, above); a group plays at the pitch of its first source (kept
    while the group has sources) from the mean of its block centres, with volume
    `clamp(count / 150, .025, ambientVolumeCap .1) · lerp(distance / 16, 2, 0)` times the group's
    relative volume (kinetic .025, cogs 1.5). Create keeps a looping sound per group and regroups
    every 5 ticks; the server can't hold a looping sound on a client, so Levitate regroups and
    restarts its 0.5 s samples every 10 ticks (`levitate.kinetics.cogs`: wooden gear teeth,
    `levitate.kinetics.rumble`: a low hum; both synthesised by `tools/sounds/gaps.ps1`, seamless
    when repeated), played one block from the player towards the mean position (inside the sounds'
    2-block min distance, so the game adds no falloff of its own). Create's milling / crushing
    soundscapes are not part of this (§8).
  - Overstress keeps Create's smoke / cloud puffs above; Create plays no sound for it.
### 3.2 Block data, capabilities, filters, value boxes (U3)

Headers in `src/feature/common/` (pure logic in `src/core/common/`, unit-tested).

- **`BlockData`** (`BlockData.h`): one JSON object per (dim, pos), shared by units through
  top-level keys (`data["kinetics"]`, `data["filter"]`, ...). `get/edit/set/erase/has`,
  `forEachInChunk`, `forEach`. Saved per chunk in the world DB (`lv:common:bd:<dim>:<cx>:<cz>`,
  JSON keyed `"x,y,z"`), loaded lazily, written on every world save. Erased automatically when the
  block at the position changes to another block *type* (break, replace, explosion, vanilla
  piston); state changes keep it. `onErase(fn)` lets owners drop contents first. Contraptions (U4)
  move every document with `take(pos)` (before removing the block) and `put(pos, doc)` (the
  `common:blockdata` data carrier, so units need no carrier for what they keep here). Owners that cache
  their part elsewhere write it back in `onWriteBack(fn)` (before every save and before `take`) and
  adopt moved data in `onPut(fn)` (U2's kinetic records do both).
- **Interactions** (`Interactions.h`, U32, U36): one hook per GameMode action (`useItemOn`,
  `baseUseItem`, `destroyBlock`, `startDestroyBlock`, `interact`, `attack`) dispatches to handlers
  registered per block / item (entity) id with a priority (`onUseOn`, `onUse`, `onDestroy`,
  `onWrench`, `onStartDestroy`, `onInteract`, `onAttack`); the first handler that returns `Consume` /
  `Deny` wins (vanilla is skipped and the player's inventory resent), `Pass` goes on. Value boxes and
  filter slots use `priority::Highest`, units' own blocks `Normal` (their wrench handling, Create's
  `IWrenchable#onWrenched`, comes before the generic one), the wrench's generic rotate / pick up (U8)
  `priority::Fallback`. After vanilla ran: `onPlace` (a block vanilla placed from a use-on nobody
  took: Create's `getStateForPlacement` / `setPlacedBy`), `onUnusedUseOn` (vanilla's use-on failed,
  e.g. a bucket in the air), `onDestroyed` / `onAttacked` (with vanilla's result). `nextTick` defers
  a world edit to the next server tick. Since U36 every unit goes through this dispatch; the only
  remaining player hooks are not click dispatch (pick range, item release, eating, tool damage).
- **Comparator output** (`Redstone.h`, U36): `registerAnalog(blockId, fn)` = Create's
  `getAnalogOutputSignal`; one `ComparatorCapacitor::evaluate` hook gives a vanilla comparator the
  output of the registered block behind it, or behind the solid block behind it (Java's
  `ComparatorBlock.getInputSignal`), and releases it when it stops reading one. Registered: fluid
  tank and creative tank (`fractionToRedstoneLevel` of the structure's fill), basin (inventory),
  blaze burner (heat - 1) and lit burner (1 / soul 2), depot and weighted ejector (stack / max
  stack), item vault (inventory), toolboxes (inventory), lectern controller (15), portable storage
  and fluid interfaces (15 while connected), packager (15 while busy), frogport / postboxes
  (inventory), redstone requester (15 after a successful request), train station (15 while a train is
  present), train signal (15 while red), speedometer / stressometer (dial) and sequenced gearshift
  (its step). `registerEmitter(blockId, fn)` lets `power` see custom signal sources (redstone links,
  latches, placards).
- **Redstone input** (`Redstone.h`, U32): `power(region, pos)` = the strongest signal (0..15) of the
  six neighbours read from the game's circuit graph, as a vanilla mechanism sees it (conducting
  blocks strongly or weakly powered, wire, levers, buttons, plates, redstone blocks, observers;
  torches except into their support; repeaters and comparators only into the block they face);
  `watch(pos, fn)` reports changes, polled every 2 ticks while the chunk is loaded (custom blocks
  get no neighbour updates, §8).
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
  signed and option boards; `requiresWrench` as Create's: only the wrench opens it). A box is hit
  when the click is within `scale / 2` (default 0.25)
  blocks of its centre (Create's ValueBoxTransform.testHit). Client mod (U42, Create's
  ValueSettingsClient): a right-click with the wrench or an empty hand on a box starts a *hold*:
  the server asks the client mod to watch the use button (`valuebox.hold {token, pos}`), the client
  answers `valuebox.release {token, state}`: held for more than 3 ticks → the board opens (pointer
  picks row and value, wheel ±1, Shift+wheel jumps milestones, click confirms, right-click / Esc
  cancels), released earlier → the box's `onShortClick` (Create's onShortInteract; the valve
  handle turns, a brass diode toggles), looked away / sneaked → nothing. Repeats of the held button
  are consumed while the hold waits (`holdPending`). The client mod watches the mouse's use button;
  without one (gamepad, touch) a click on a box answers "hold" at once (the board opens). Filter slots with `onHold` (filters with an
  editable amount) run it on a hold, a short click sets / clears the filter. Other units use the
  same pair through `awaitHold(player, pos, onShort, onHold)` (gauge panels, price tags). Looking
  at a block with boxes outlines the boxes of the looked-at face (`client/render/Outline.h`,
  the hovered one white, the others grey; boxes needing the wrench only while holding it) and
  shows the hovered box's label, value and "Click and hold to edit" next to the crosshair.
  Vanilla: the same right-click opens a form
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
  `net::client::on` / `net::client::send` on the client; `net::onCompanionJoined` sends a player
  the full state after the handshake. Typed channels so far: `screen.open/update/close/action`,
  `valuebox.open/set/hold/release`, `common.hover`, `kinetics.sync` (§3.1), `kinetics.stress` (U42: the
  server's stress tables for item tooltips, on join and after `/create reload`), `tracks.curves` (§3.5),
  `itemrender.batch/query/reset`.
- **Display items** (U33, server `src/feature/itemrender/DisplayItems.h`, client
  `src/client/render/WorldItems.h`): items the client mod draws in the world for blocks (Create's
  block entity renderers draw them). A unit publishes items per (owner block, slot): live items
  with `set` / `remove` / `clear` every time they change (`lerpTicks` = ticks the client takes to
  move there, 1 for items moved every tick), provided items through a provider per block id that
  builds them from block data when a client mod first sees such a block (`itemrender.query`) or the
  unit calls `refresh(owner)`; provided items go by themselves when the block changes. Companions
  within 64 blocks get `itemrender.batch {dim, set, move, del}` (core/itemrender/DisplayItem.h:
  item, position, yaw / pitch / roll, size, style Facing (item frame look) or Lying (belt look),
  spin, pile, fallback entity to hide). The client interpolates and draws them with the game's own
  item models (`ItemInHandRenderer::renderItem` at a world matrix: block items as cubes, sprites
  as flat items), lit like held items; client code that already animates something draws items
  with `render::drawItem`. Vanilla clients get nothing: each unit keeps its fallback (§8).
  `/create itemrender demo x y z` (op) shows a test pattern, `/create itemrender clear` removes it.
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
  keyboard input, and closes with Esc or the inventory key (a server screen then sends
  `screen.closed {type, context}`; `ScreenType::closed` runs, log `screen <type> closed by <player>`).
  `registerServerScreen(type, factory)` binds server-opened types. Clicks are queued from the
  input thread and handled on the client thread; inventory stacks are only read, the ghost cursor
  is item JSON (`Screen::cursorItem`) with a render-only icon. `TextField` (U32): a single-line
  text box edited in place (focus on click, characters from the window's WM_CHAR so layouts and
  IME text work, Backspace/Delete/arrows/Home/End, Ctrl+V pastes, Enter submits, Esc cancels,
  `maxLength`, `filter`); `/create screentest` opens a test screen with one.
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
  on it are carried; entities collide with its blocks through the game's own collision (the server's,
  and the client mod's for the local player); seats can be ridden, the riders stay seated.
- **Actors**: blocks on a contraption may act while moving (`contraption::registerActor`): drills
  break, saws cut, harvesters harvest, ploughs plough, rollers pave, deployers use, PSIs transfer,
  controls toggle actors.
- **Storage**: inventories and tanks on a contraption are merged into contraption storage, usable by
  actors and the PSI.
- **Disassembly**: placing back aligned to the grid (rounding as Create), blocks that can't be placed
  drop as items.
- **Super glue**: drag-select a box (two clicks) with super glue to glue a region (max 24 ⚠ blocks
  in each axis); while the player holds super glue the client mod outlines the regions (passive green
  0x4D9162, the one under the crosshair 0x68C586) and the selection from the first click to the
  looked-at block (green when it can be glued, yellow 0xC5B548 when too big or not connected); the
  server sends the regions within 48 blocks on `contraption.glue`; vanilla players see particles along
  the edges. Removed by hitting with glue / breaking.
- **Chassis**: linear (sticky faces via slime ball, range by scroll value), secondary linear, radial
  (range radius); stickers stick the block in front when powered (their redstone input through the
  shared `redstone::watch`, Create's neighborChanged). The range is a U3 value box on
  every face of the chassis, opened with the wrench only (Create's `requiresWrench`), 1..
  `maxChassisRange` with milestones every 10, labelled "Range of Sticky Sides" (radial: "Radius when
  Sticky"); a value set on one chassis goes to its connected group. Stored in BlockData
  (`data["chassis"]["range"]`; wave-1 worlds' `lv:contraption:chassis` list is migrated once).
- **Client**: the client mod renders contraption blocks at their interpolated transform, including
  rotating kinetic parts and actors' animations.

Implementation (U4, `src/feature/contraption/Contraption.h` is the contract):

- **API for anchors**: `assemble(AssembleOptions{anchor, start, forced, axis, pivot, kind,
  includeAnchor}) -> AssembleResult{id | errorKey, errorText}`; `find(id)` → `Contraption&` with
  `setTransform(t)` (anchors that compute their position each tick: bearings, pistons),
  `setMotion({velocity, angularVelocity, follow, ticks})` (velocity per tick, follow an actor),
  `stop()`, `storage()`, `anchorData()`, `seat()` / `seated()`, `setActorsPaused(bool)` (Create's
  isActorActive of a cart turning in "Pause actors while rotating"); `disassemble(id)`; `discard(id)`
  (Create's entity discard: gone without placing or dropping anything); `setBlockStates(id, local,
  states)` (a carried block changes, its collision and display follow: doors); `bounds(id)`; `all()`. Ids are persisted:
  keep the id in your BlockData, never the pointer. Transform: local block coordinates relative to
  the anchor; `world = origin + offset + pivot + R(local - pivot)`, R = rotation by `angle` degrees
  about `axis` (right-hand rule) after an optional `tilt` about the local `tiltAxis` (a train carriage
  pitched on a slope; disassembly ignores it). `/create contraption assemble|move|rotate|stop|disassemble|list|
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
  `registerDataCarrier({name, capture, restore, rotate})` moves other units' per-block data; the
  `common:blockdata` carrier moves every BlockData document (restore merges with what other carriers put
  back first), chassis ranges included; keys a unit carries itself are declared with
  `blockdata::ownCarrier`, direction-dependent ones turn through `blockdata::onRotate`. While
  moving, `Contraption::blockData(local)` / `actorData(local)` are the writable carried data
  (persisted with the contraption). Client mod: companions get `contraption.blocks` (display entity
  → contraption, local position, block id, quarter turns) whenever displays are spawned,
  `contraption.visual` (actor animation state from `setActorVisual`) and `contraption.remove`;
  `client/contraption/ContraptionClient.h` (`forEachBlock`) gives client units each block's
  interpolated world centre and rotation per frame for actor animations. Merged storage: vanilla chests, trapped chests,
  barrels, shulker boxes, hoppers, dispensers and droppers (`Storage::insertItem/extractItem/slot`);
  tanks through `registerFluidStorage(blockId, {read, write})` on the carried data.
- **Movement each tick**: the anchor's transform or the motion; actors (`registerActor(blockId,
  MovementBehaviour{startMoving, tick, visitNewPosition, stopMoving, isActive})`, Create's
  MovementBehaviour; an actor may `stall` the contraption; `tickWhileStill` actors also tick while it
  stands still, as Create ticks every actor each tick); entities standing on it are carried
  (the point under their feet follows the transform, rotation included); entities collide with its
  blocks: a hook of `BlockSource::fetchCollisionShapes` / `fetchCollisionShapesAndBlocks` adds each
  carried block's collision box (the bounding box of the turned box, Create's oriented colliders
  approximated) on the server, and from the synced transforms on the client mod's side (the local
  player's prediction; `contraption.blocks` carries each block's box); players and mobs are still
  pushed out of its blocks for vanilla clients (also while it stands still: the blocks are gone from
  the world, the contraption is the floor); seated entities (`registerSeat(blockId, height)`, entities
  in a seat block at assembly or `seat()`) follow their seat: one riding a seat entity stays mounted
  and the engine moves the seat entity; players leave by sneaking. Unloaded anchor chunk: frozen.
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
  turn; its own rotation carries the axis (pitch) and the remaining angle (yaw). Bedrock clients
  never apply the movement the server sends for a falling block that isn't falling (it stays where
  it was spawned), so (U34): companions get each changed transform once per tick
  (`contraption.motion` `{"m": [[id, axis, angle, tx, ty, tz, px, py, pz], ...]}`, world =
  t + p + R(local − p); `contraption.blocks` carries the transform too); the client mod takes the newest
  at each client tick (`core/contraption/Motion.h`), moves every display entity there with the previous
  place as its old position, and draws each block at the frame's partial tick, turned by the exact
  interpolated angle about its center (Create's approach: the client interpolates the
  contraption's motion; without vanilla viewers the displays keep the quarter turn they were
  spawned with and the client mod turns them by the rest, so they aren't spawned again every 90
  degrees). While a player without the client mod is in the dimension, a display that
  moved away from where it was spawned is spawned again at its place (from half a block away, at
  most every 4 ticks per block, and once more where it comes to rest), so vanilla clients see the
  structure move in steps. A client draws a new falling block only a few frames (~125 ms) after
  it appears, so the replaced display is removed 3 ticks (150 ms) later (`contraption.blocks` lists it under
  `retiring`; the client mod keeps it on its block meanwhile): no gap in the structure. Display
  entities are never written to the world
  (`Actor::save` refuses them): contraptions live in our DB and spawn their displays again after a
  load, so a world saved mid-move (autosave, or the last save before a crash) holds no falling
  blocks that could land when it is opened without Levitate (§8). Persistence:
  `lv:contraption:c:<id>` (blocks, transform, motion, glue, seats, anchor data),
  `lv:contraption:glue`, `lv:contraption:stickers`.

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
  (3 from 4 blocks up, at least 6); curves leave/enter slopes only straight. Without Ctrl (Create's
  sprint key mapping) the curve is standardised (smallest turn, straight track fills the rest); with
  Ctrl held it takes the whole space (Create's "maximized turn"). The client mod reports the Ctrl key
  (`tracks.ctrl`); for vanilla clients sprinting stands for it.
- Placement preview (client mod, Create's TrackPlacement.clientTick): while the player holds the
  track item with a selection (sent to the client mod on `tracks.selection`) and looks at a block,
  the same rules run every frame on the client: both rails of the planned straight runs and of the
  curve are drawn (1/8 block lines, 4 px above the bed) in a colour fading from Create's red
  `#EA5C2B` to green `#95CD41` as the connection becomes valid, and the action bar shows "Can
  Connect ✔" (green), "Place track or select a second point" (white) or the refusal (red),
  including `not_enough_tracks` from the client's inventory.
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

**Tilted tracks** (Create's TrackBlockEntityTilt, U48; maths in `core/tracks/Tilt.h`): a flat
straight track (not bound to a portal) with exactly two curves leaving it on opposite sides, one
ending lower and one higher (two sloped curves meeting at it), tilts when the second curve is
built: m = (height of the high far end − height of the low far end) / (sum of both curve lengths);
the low curve keeps its start and leaves along the ramp (its axis gets −m), the high curve starts
at the ramp's top (low start + the axis direction raised by m, normalised, times the axis length)
with axis +m; the graph node at the raised end is lifted by `smoothing` = ⌊m · axis length · 16⌋
(0..15) pixels (Create's TrackNodeLocation.yOffsetPixels), so trains climb over the block; the
smoothing angle (atan m, negative when the ramp rises towards −x / −z) is kept in
`BlockData["tracks"]["tilt"]` (not carried by contraptions: a moved track loses its curves). A
connection onto a tilted track is refused (`create.track.turn_start`); once the track no longer has
exactly its two curves, the remaining ones get their original shape back and the track is flat
again. The client mod leaves the tilted block out of the chunk mesh and draws its model turned
onto the ramp (Create's TrackModel transform); vanilla clients see the flat block (§8).

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

**Client**: curves are drawn by the client mod (ties and rails along the curve segments) from the
companion protocol's `tracks.curves` messages: every curve of every dimension after the handshake
(`reset` first), then each change, and the tilt of every tilted track (`{"op":"tilt"}`, see §8).
Vanilla clients get nothing.

### 3.6 Feature mechanics

Each wave-2 unit implements the full Create 6.0.10 behaviour of its blocks (§2), including: what
right-click/shift-right-click/wrench do, redstone input and output, comparator output, interaction
with belts/funnels/arms/deployers/pipes where Create has it, processing recipes and timings, sounds and
particles, goggles info, and the client rendering. The unit's PR adds a subsection §3.6.<n> (unit U<n>, in unit order) here with
the details and numbers it implemented. Short reminders of the less obvious ones:

- **Belts (U11)**: 2–20 blocks long (§3.6.11), connect two shafts in a line or slope (45°), items move at
  speed proportional to RPM, belt carries entities, funnels/tunnels/depots/processing work on belt
  items, belt casing, dye, wrench to adjust, breaking returns belts.
- **Fans (U13)**: push/pull entities and items along up to `fanPushDistance` / `fanPullDistance` 20
  blocks; items in the air stream through water → washing, fire/campfire → smoking, lava → blasting,
  soul fire → haunting (details in §3.6.13).
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

#### 3.6.6 Transmission (U6)

Checked against Create 6.0.10 (RotatedPillarKineticBlock, CogWheelBlock, CogwheelBlockItem,
ShaftBlock, EncasedCogwheelBlock, GearboxBlock, VerticalGearboxItem, GearshiftBlock, ClutchBlock,
ChainDriveBlock, ChainGearshiftBlockEntity, SequencedGearshiftBlock(Entity), Instruction,
SpeedControllerBlock(Entity), GaugeBlock, Speed/StressGaugeBlockEntity; U49: CuckooClockBlock(Entity),
CuckooClockRenderer, FlywheelBlock(Entity)). Pure rules in
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
- **Cogs placed against integrated cogs** (U50, Create's CogwheelBlockItem.onItemUseFirst): the
  cog helpers above take every ICogWheel, not only the cogwheels: the encased cogwheels and the
  blocks with an integrated small cog (mechanical mixer and millstone around y, mechanical arm
  around y, mechanical crafter and display board around their facing axis, mechanical pump
  around its facing axis), with the block's rotation axis and its hit-on-shaft test (the centre
  of the faces on its axis). They run before the clicked block's own use (registered above the
  blocks' handlers, below value boxes and filter slots): a cog on a crafter's front goes beside
  the crafter unless the click is on the front's centre, where the crafter takes it into its
  slot. When no spot fits, or the player sneaks, the block's use and the plain placement follow
  (the axis of the small cog it is placed against; the plain placement's state and canSurvive
  run after every block's own use, as BlockItem.place does). One helper placement per 5 ticks: the
  repeats Bedrock sends while the button is held are swallowed in between. Create's IntegratedLargeCogHelper /
  IntegratedSmallCogHelper match a subset of the states the diagonal helpers already match, and
  `onItemUseFirst` returns the first matching helper's result, so in 6.0.10 they never decide a
  click (they only feed the client ghost). `isValidCogwheelPosition` skips a neighbour only when
  it has the AXIS property along the facing (cogwheels, encased cogwheels): a large cog is refused
  beside any block with an integrated cog, whatever its axis. The mechanical arm is a small cog
  for the kinetic network too (Create's ArmBlock implements ICogWheel).
- **Encasing**: right-clicking a shaft / cogwheel / large cogwheel with `andesite_casing` or
  `brass_casing` (U10) turns it into the encased block without using the casing; encased cogwheels
  get a shaft end on each axis side with a neighbouring shaft facing them.
- **Wrench** (U42, Create's EncasedCogwheelBlock / EncasedShaftBlock IWrenchable): the wrench on an
  encased cogwheel's face along its axis toggles the shaft end on that side (`top_shaft` /
  `bottom_shaft`, rotate sound); other faces rotate it as any kinetic block. Sneak + wrench on an
  encased shaft / cogwheel takes the casing off: the block turns back into the shaft / cogwheel /
  large cogwheel on the same axis with the casing's break particles (nothing is given, encasing
  uses no casing).
- **Kinetics** (specs in `Specs.cpp`): gearbox halves (Create's GearboxBlockEntity), clutch
  (powered: far half disconnected) and gearshift (powered: far half reversed); chain drives pass
  their speed ×1 along the connection axis (Create's areBlocksConnected), the adjustable chain
  gearshift multiplies by `1 + (signal + 1) / 16` (1 unpowered, 2 at signal 15) as source and divides
  as receiver; the sequenced gearshift's far half turns at the running instruction's modifier
  (idle, delay, await, end: disconnected); the speed controller drives a large cog on top
  (axis across its shaft) at its target speed with Create's getConveyedSpeed rules; gauges are
  shafts along their rotation axis. Stress impact 0 for all but the cuckoo clocks (1).
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
  While a turn instruction runs, the gearshift declares Create's `SequenceContext`
  (`SequenceContext.fromGearshift`): Turn by angle → `TurnAngle`, Turn to move → `MoveDistance`,
  the value divided by |modifier| (kinetics scales by the speed ratio to the gearshift's *input*,
  Create's relative value is `value / (speed · modifier)`); delays, awaits and the end clear it.
  The network is re-read in the same tick, so the driven machines see the new speed and the
  instruction together. The machines stop exactly at the value (bearings, pistons, pulleys,
  gantries below) while the instruction's time (2 ticks longer) runs out.
  Comparator output = `levitate:state`. One log line per instruction step.
- **Rotation speed controller**: value box (board ⟳ / ⟲, 1..`maxRotationSpeed`, milestones 32) on
  the two side faces at 8 / 11 px; default 16 RPM; label "Targeted Speed in RPM". Changing it
  re-attaches the network. `levitate:bracket` shows the cog bracket under a large cogwheel.
- **Gauges**: speedometer dial = Create's getDialTarget (0..0.45 below `mediumSpeed`, ..0.75 below
  `fastSpeed`, ..1.125 at `maxRotationSpeed`, from the effective speed); stressometer dial = stress /
  capacity of the network (1.125 when overstressed, 0 when not turning). Comparator output
  `ceil(clamp(dial · 14, 0, 15))`. The client mod draws the needle on each head face (Create's
  shouldRenderHeadOnFace) easing 1/8 per tick towards the target (shaking past 1); vanilla clients
  see the needle at the nearest 22.5° step. Goggle lines (U42, Create's Speed/StressGaugeBlockEntity
  addToGoggleTooltip, replacing the kinetic defaults): "Gauge Information:", then the speedometer's
  "Rotation Speed" + Create's formatted speed text (struck through while overstressed), or the
  stressometer's "Network Stress" + "No Rotation" / the stress level text, "Remaining Capacity" and
  remaining / total su coloured by the stress level. Dust (U42, Create's GaugeBlock.animateTick):
  while the dial is not 0, each player within 32 blocks gets, with the chance the client's random
  display ticks reach the block (667/32³ within 16 blocks, plus 667/64³ within 32), coloured
  `levitate:gauge_dust` particles off each head face (4 when the dial is past 1, else one with a
  chance of 1/4), 0.55 out and 0.3 to the side, coloured as Create's gauge colour (speed level
  colour a quarter towards white; stress green → yellow → red).
- **Cuckoo clocks** (U49, Create's CuckooClockBlock / CuckooClockBlockEntity / CuckooClockRenderer;
  pure rules in `core/transmission/CuckooClock.h`): a HorizontalKineticBlock with half a shaft at
  its back (rotation axis = facing axis, shaft end only on the back face), stress impact 1 SU/RPM
  (`kinetics.json` keys `cuckoo_clock`, `mysterious_cuckoo_clock`). Placed with the back to a
  horizontal shaft end facing it (two on different axes: none), else facing the player. Both clocks
  look the same and are both named "Cuckoo Clock" (the mysterious one only differs in its recipe:
  the clock with gunpowder on four sides). Nothing happens while it stands still (overstressed:
  still). While it turns: time = day time (outside the overworld ×24), hours `(t/1000 + 6) % 24`,
  minutes `(t % 1000)·60/1000`; the hands show it (hour hand jumps 30° per hour, minute hand 6° per
  minute; the client mod chases both by a fifth per tick, vanilla clients see `levitate:hour` /
  `levitate:minute`); note block hat ticks (1/16 volume) every 16 ticks, pitch 2 on multiples of 32,
  else 1.5 (outside the overworld every 3 ticks, pitch 2 on multiples of 6). Overworld only: with
  no animation running, 12:00..12:04 starts the pig, 18:32..18:35 the creeper (a mysterious clock
  starts the surprise instead); the animation counts up each tick and ends after 100. Sounds by
  progress: chime (volume 2, pitch .5) at 1 and (pitch .7937) at 21; for each phase 30, 45, 60
  (surprise: every 3 from 30 to 60) chest open 5 (1) ticks before, the pig's oink (¼, 1) or
  creeper hurt (¼, pitch 3) at it, chest close 5 (1) after; the surprise smokes from 31 and lights
  the fuse at 40. The doors swing 0 → 135° and back around each phase (Create's renderer, including
  its overshoot during the surprise), the figure slides half a block out with them (client mod;
  vanilla clients see `levitate:cuckoo` with the doors open past 67.5°). When the creeper comes out
  the owner within 32 blocks gets "Is it Time?" (`cuckoo_clock`). At progress 50 the surprise
  removes the clock (no drops) and explodes at its centre (power 3, no fire, breaks blocks); deaths
  by that explosion read Create's "%1$s was blown up by tampered cuckoo clock". Sounds play on the
  server for every client (Create plays them on each client). Display sources: time of day and
  stopwatch (U24). Item tooltips: Create's cuckoo clock description on both.
- **Flywheel** (U49, Create's FlywheelBlock): a RotatedPillarKineticBlock (shaft ends on both axis
  faces, the pillar placement rule), no stress impact, a brass wheel 28 px across around the shaft;
  the client mod spins it with the network (Create eases the wheel's visual speed by 1/64 a tick:
  here it turns at the network speed at once), vanilla clients see it still. Collision / selection:
  its own block (Bedrock boxes are at most 16 px).

#### 3.6.7 Generators (U7)

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
  `create.large_water_wheel.not_enough_space` ("Clear Blocks for Placement") in the action bar,
  and the client mod draws Create's red (0xff5d6c) outline of the 3x3 room it needs for a second
  (`generators.room {lo, hi}`, U42).
- Hand crank and valve handles keep the water they are placed in (Create's ProperWaterloggedBlock,
  `minecraft:liquid_detection` can_contain_liquid, U42).

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
or for 4 ticks after a turn. While it turns it declares a `TurnAngle` sequence context of |angle|
(Create's ValveHandleBlockEntity): bearings it drives stop after exactly that angle (scaled by the
speed ratio). A dye of another colour dyes the handle (the block becomes
`<colour>_valve_handle`, facing kept, the dye is not used; Create's ValveHandleBlock.clicked); its
angle resets to 45° as Create's new block entity. Players who may not build (adventure) can't use
it. The value box sits on the handle's front (4.5 px out from the mounting face). Client mod
(U42): a click on it with an empty hand or the wrench starts Create's click and hold: released
early it turns the handle (the box's short click), held it opens the board; vanilla: an empty hand
turns the handle, the wrench there opens the form (U3). Re-dyeing works in the crafting grid as
Create's `#create:valve_handles` + dye: any handle (copper or dyed) with a dye of another colour
(one generated recipe per pair, `tools/models/generators.ps1`). The handle model turns from
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

#### 3.6.8 Wrench, goggles and the goggle overlay (U8)

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
- Sneak + right-click runs `onSneakWrenched`; default: survival players get the block's drops,
  its loot table rolled with the wrench as the tool (`BlockType::getResourceDrops`, Create's
  `Block.getDrops(..., player, wrench)`; U42) into the inventory (the rest dropped at their feet;
  creative: nothing), a contraption anchored at the block
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
Players wearing them also see the rotation indicator particles of generators changing their
speed level (Create's RotationIndicatorParticle; §3.1 Kinetic feedback, U37); the wrench alone
doesn't show them, as in Create.

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
  Large" or "Nx RPM" and dark gray " -> N su" ("Up to N su") from the generator speed. The impacts
  and capacities are the server's (Create syncs its stress config): the server sends its tables
  as `kinetics.stress` on join and after every config reload; the client mod uses them until it
  leaves the level (U42).
- Overstressed speed text (Create's strikethrough): the server wraps it in `§~` markers for the
  client mod, which draws a line through that span in the overlay (U42).
- Recipe (U42): Create's " S " / "GPG" with any glass (`#c:glass_blocks`: glass, tinted glass, the
  16 stained glasses, the lenses may differ): one generated recipe per pair (`tools/gen/goggles.ps1`,
  Bedrock mirrors shaped recipes).
- Logged actions: `wrench: <player> rotated <id> at <pos> (face f)`, `wrench: <player> dismantled
  <id> at <pos>`, `wrench: <player> broke a <minecart>`, `goggles: overlay for <player> at <pos>
  (<n> lines, client mod|action bar)` (once per new target), `goggles: overlay screen opened for`,
  `goggles: overlay reset for`, client `goggles: overlay offset set to x, y` / `reset`.

#### 3.6.9 Materials, foods, world generation, decorative stone (U9)

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
  properties. Connected textures (Create's CT, kept by the server in `levitate:c_<dir>`, recomputed
  at the next tick after any change): layered stone sides HORIZONTAL_KRYPPERS (same block left /
  right), its top / bottom the cap sheet (OMNIDIRECTIONAL over the 4 horizontal neighbours); pillar
  sides RECTANGLE, the faces on its axis the cap sheet; pillars join along the axis with the same
  pillar on the same axis, sideways only when both columns end at the same places (Create's
  ConnectedPillarBlock, columns walked up to 1000 blocks). Every block of the unit carries
  Create's mineable tags (`minecraft:is_pickaxe|axe_item_destructible`, `minecraft:iron_tier_destructible`
  for needs_iron_tool), so the game's tool speeds and tiers apply. Stairs: `minecraft:corner_and_cardinal_direction` + `minecraft:vertical_half`
  (Bedrock computes Create's inner / outer corners, corners with any of the unit's stairs), world
  aligned quarter boxes for model and collision. Slabs: `minecraft:vertical_half` + `levitate:double`;
  using a slab on the empty half of the same single slab (clicked face, or the slab beside the
  clicked face by click height) makes it double (one item used, 2 drops); the polished slab's sides
  use Create's slab texture. Walls: `minecraft:connection` (format 1.26.0) for the four connections,
  plus Java's WallBlock rules kept by the server: a side is tall (`levitate:tall_<dir>`, 16 px) when
  the bottom face of the block above covers it, else low (14 px); the post (`levitate:up`) shows
  unless the wall is exactly straight, a tall straight line drops it, a wall post above or a torch /
  sign / banner / pressure plate (WALL_POST_OVERRIDE) or a block covering the post above forces it;
  collision 24 px (post 4..12, sides 5..11) from the collision hook.
- **Furnace xp**: Create's recipe xp (crushed ores 0.1, raw zinc 0.7, zinc ore 1.0, dough / soul
  sand 0) instead of the output item's Bedrock multiplier: the furnace's slots are compared between
  ticks (`FurnaceBlockActor::canBurn`) and when the experience is taken; the total is rounded as
  Java (whole points, the fraction as one more with its probability). Cook times are Bedrock's
  10 s / 5 s / 30 s = Create's 200 / 100 / 600 ticks.
- **Block of experience**: Create's animateTick (1 in 5, end rod within ±0.55 of the centre,
  speed gaussian × 0.005) with Java's ClientLevel.animateTick sampling (667 × ±16 and ±32 per
  tick): the client mod around its player, the server for vanilla players.
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
  `lv:chromatic_compound.light`; a charged compound stacks to 1, the client mod draws Create's charge
  bar: width round(13 · light / N), colour 0x413c69 → white); a belt segment or depot picked instead
  gives up one item of the first carried stack whose block emits light; at `lightSourceCountForRefinedRadiance` (10) charges it becomes refined
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

#### 3.6.10 Building blocks (U10)

Code: `src/feature/building/`, data generated by `tools/gen/building.ps1`, textures by
`tools/textures/building.ps1`. State conventions: `levitate:facing` 0..5 (Java Direction order; the
horizontal blocks use 2..5, 0/1 read as north), connected-texture states `levitate:c_<dir>`, pane arms
`levitate:north|south|west|east`. Bedrock stores list-valued states by index, so every integer state
of the unit is a 0..n range. Any world change (players, commands, pistons, contraptions) is seen through
the block-changed event: the position and its neighbours are re-evaluated at the next tick (Create's
`updateShape`).

- **Connected textures**: casings and framed glass connect on all four edges of every face to the same
  block (16 textures per block), horizontal framed glass left/right (its top and bottom use the framed
  glass sheet), vertical framed glass and wooden / ornate iron windows top/bottom, industrial and
  weathered iron windows on all four edges of their sides (Create's RECTANGLE); window top / bottom
  faces use an end texture. Panes (framed glass, horizontal / vertical framed glass and every window
  pane) use their full block's sheet on every side face, joining the same pane around them (Create's
  GlassPaneCTBehaviour). States: one per neighbour direction used. The client mod draws the inner
  corners of the omnidirectional sheets (casings, framed glass and its panes, layered / pillar caps):
  the frame's corner where two edges join but the block across the corner differs.
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
  pitch 0.9–1.0. Open shapes from Create's `SlidingDoorShapes`, clipped to the block. A closing door
  keeps its open collision (`levitate:closing`) for the 7 ticks of Create's animation, then plays
  `close.iron_door` at volume 0.5. The client mod draws the doors moving (Create's
  SlidingDoorRenderer: a value chasing open / closed at 0.15 per tick, drawn as its square; sliding
  panels move 13 px sideways and 0.5 px out, folding doors turn their leaves 91° / −181°).
- **Trapdoors** (Create's `TrainTrapdoorBlock`): Java trapdoor placement (side: facing the clicked face,
  upper half when clicked high; else the player's opposite direction, top when clicking a bottom face),
  click toggles, redstone opens.
- **Girders** (Create's `GirderBlock`): `axis` from the clicked face, beams `x` / `z` when on that axis,
  next to a girder beam on that axis, an encased girder across, or a placard facing it; an existing beam
  stays while either end is held. `top` / `bottom` attach to girders, walls, chains, lanterns and
  placards (and stay on solid blocks). A shaft used on a girder makes `metal_girder_encased_shaft`
  (shaft axis z if the girder had an x beam or the clicked face is on z, else x), a kinetic shaft
  (`specs::shaft`, rotor model `geometry.levitate.girder_shaft_rotor`); it drops the girder and the
  shaft. Placement helper (Create's GirderPlacementHelper): a girder used (not sneaking) on a girder
  or encased girder extends the line at its end in the direction nearest to the clicked point that
  the clicked block extends along (a vertical girder up / down, a junction any way, an encased one
  across its shaft), up to `placementAssistRange` 12 (+4 with an extendo grip in the off hand).
- **Brackets**: a metal / wooden bracket used on a shaft, cogwheel or large cogwheel is stored in its
  `BlockData["building"]["bracket"]` ({id, facing, axis, cog}: Create's BracketBlockItem facing, the
  clicked face or, on an end of the shaft, the face towards the player) and dropped when that block
  goes; another bracket swaps it, the wrench takes it off (Create's IWrenchableWithBracket); fluid
  pipes keep their own (U17). The item is never placed as a block. The client mod draws the bracket
  (Create's ground / wall bracket models, the mounted states of the bracket block).
- **Table cloths**: decoration (1-px cloth with a skirt over the block below); U26 adds the shop.
- **Placard** (Create's `PlacardBlock`): hangs on the side it was looked at (a block with a collision
  shape). Use with an item while empty: it holds one (sound item frame); use while holding a matching
  item (same item and data, or matching the held filter): 15 power on every side and strong power into
  the block it hangs on for 19 ticks (`minecraft:redstone_producer`), a non-matching item plays the deny
  sound. Punching gives the item back; breaking drops it.
- **Copycats**: panel (3 px on the side looked at, `facing`) and step (half block, `facing`, `top`).
  Using a full, solid, non-container block item applies it as the material (one item used, kept for
  dropping) in its default state turned to the clicked face (FACING / HORIZONTAL_FACING / AXIS), or
  in the exact state of a neighbouring identical copycat with the same material; using the same
  block again turns it (Create's cycleMaterial: FACING, HORIZONTAL_FACING clockwise, AXIS, LIT; item
  frame sound); a different material is refused while one is applied; the wrench resets it. The
  material is stored with its states (`BlockData["building"]["state"]`, turned with contraptions).
  The client mod draws the copycat in its material (Create's CopycatPanelModel / CopycatStepModel:
  the material's quads cropped and moved; faces against a solid neighbour culled); materials and
  brackets reach it over `building.query` / `building.visuals`.
  U48: the panel also takes **bars** (iron bars, the copper bars, Create's bars; not glass panes)
  and **trapdoors** (vanilla and Create's), although they aren't full blocks (Create's
  CopycatSpecialCases / isAcceptedRegardless; the step doesn't). A trapdoor is prepared as Create's
  prepareMaterial: in a floor / ceiling panel it lies closed, facing the way the player looks (upside
  down in a ceiling panel); against a wall it stands open, facing the panel's way, in the half the
  click was in. Using the same trapdoor again flips an open one's half, turns a closed one clockwise
  (Create's cycleMaterial). The client mod draws a trapdoor material as the trapdoor itself and bars
  as Create's bars panel (a sheet of the bars' texture 1 px in from the back, 2 px edge strips;
  CopycatBarsModel). A block in the off hand becomes the material of a copycat when it is placed
  (Create's setPlacedBy).
  **Placement helpers** (Create's CopycatPanelBlock / CopycatStepBlock PlacementHelper,
  `feature/building/CopycatHelpers.cpp`): a copycat panel used on a copycat panel (not sneaking)
  places one on the free side nearest the clicked point across its facing, with its facing (the
  click is used up even when nothing fits); a copycat step used on a copycat step continues the line
  across its facing (PoleHelper on `facing.getClockWise()`'s axis, past up to
  `placementAssistRange` = 12 steps, +4 with the extendo grip), with its facing and half. While a
  player of the client mod holds one and looks at one of its kind, the copycat the click would add
  is drawn as a breathing ghost there (Create's PlacementClient ghost).
- Selftests `building.*` (dx 1000..1099; U43 additions 4350..4399; U48 4870..4899), e2e
  `tools/e2e/{server,client}/scenarios/building.ps1`; client parts in `src/client/building/`.

#### 3.6.11 Belts, depots and weighted ejectors (U11)

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
  is selected and the connector held, dust shows the planned belt every tick (Create's
  BeltConnectorHandler: from the selected shaft towards the shaft looked at, or the block in front of
  the face looked at; the component along the axis dropped and the direction snapped to straight /
  45 degrees; one particle per 1/16 block with a chance of 1/10; green when the shafts can be
  connected, red when not; looking at nothing, the shaft sparkles now and then). The client mod
  draws it itself (`belts.connector` tells it the selection); other players get the particles from
  the server every 5 ticks, sent to them only.
- **Kinetics**: a belt is a kinetic block (stress 0): pulley segments (start, end, pulley) have shaft
  ends on both faces of the rotation axis (the axis 90 degrees from the facing; sideways: y); all
  segments of one belt turn together, sloped ones through their diagonal neighbours.
- **Items** (BeltInventory): speed `rpm / 480` blocks per tick, START -> END when Create's
  direction-aware speed is positive (`rpm * step(facing) * (x axis ? -1 : 1) > 0`); items keep one
  block apart, stop 0.45 before a blocked end (solid block), 0.25 before a block that takes items
  (another belt, depot, ejector, other units' direct inputs) and fall off an open end (thrown along the
  belt at max(speed, 1/8) plus 1/8 up). Vertical and sideways belts carry nothing (items fall off).
  Belt-to-belt: the end inserts into the block after it (any direct input: belts, depots, ejectors,
  chutes, upward inserting funnels; for slopes also the block level with the
  last segment) with the movement direction as side; a belt refuses insertion against its movement,
  side insertions start 0.35 off-centre and slide to the middle (a distributing brass tunnel above
  the segment takes them instead). Items crossing into another segment are reported to the block
  above it (tunnels, `beltlink::itemEntering`; a stuck item waits at .99 / .01 of its segment);
  belt funnels above the segments take the items crossing their entry (Create's checkForFunnels).
  Dropped item entities landing on a
  belt are taken whole at their segment (Create's item handler: one stack per segment until it moved
  on), except under a tunnel. Right-click with an empty hand takes the stacks within 0.55 of that
  segment's centre. Sloped segments collide as eight 2 px steps following the band (server and client
  mod; vanilla clients are corrected by the server).
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
  pulley; each segment blown up by an explosion drops that loot with a chance of 1 / radius each.
- **Depot** (DepotBehaviour): holds one stack (max stack size) and an 8-slot output buffer.
  Comparator output (depot and ejector, U36): f = (held + output buffer counts) / max stack size
  (64, the ejector's stack size setting when set); floor(14·f) + (f > 0).
  Right-click gives back the held stack and the outputs, then puts the held hand stack on it (slides from
  0.25 to the centre). Items arrive from belts / funnels / ejectors (DirectBeltInput) only when it is
  empty, slide to the centre (1/4 of the distance per tick), are processed by the block 2 above (Create's
  BeltProcessingBehaviour, unless the block between has a collision box), outputs go to the buffer.
  Item capability: 9 slots (0 held, 1..8 outputs), insert only into an empty depot, extract outputs
  first, the held stack when it isn't being processed. Sounds: Create's depot slide / plop (sand break
  0.125 / 1.5, item frame add 0.25 / 1.25). A pulling belt funnel on top takes the outputs, then the
  centred held stack (one transfer per tick; on an ejector not against its launch direction).
- **Weighted ejector**: a depot with a launch plate; kinetic (impact 2 SU/RPM, shaft on the sides).
  Target: sneak + right-click a block with the ejector item ("Target Selected"), then place it: it
  faces the target and throws there if the target is in line with it, not straight above / below and
  within `maxEjectorDistance` (32) ("Ejecting to [x,y,z]"), else ("Target was not Valid") the block in
  front; placed without a selection it faces away from the player. Launches when charged, turning, not
  powered by redstone (the shared redstone watch), and an entity stands on the plate or the held stack is
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
  the band texture scrolls at the surface speed (1 px steps), pulleys turn at Create's kinetic angle,
  the casing is drawn on horizontal, sloped and vertical segments (the static model shows it too);
  speeds over `belts.query` / `belts.speed`. The ejector's shaft turns through U2's rotor rendering.

#### 3.6.12 Item logistics (U12)

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
  - On a belt (Create's BeltFunnelBlock): a horizontal funnel on a belt carrying items, a depot or
    an ejector is a belt funnel, `levitate:shape` 1 retracted / 2 extended (along the belt), 3
    pushing / 4 pulling (across it; placed normally: pushing, sneaking: pulling). It becomes one
    when placed there or when the block below changes, and turns back into a plain funnel
    (pushing -> extracting) when the belt goes. Wrench: pulling <-> pushing, retracted <->
    extended (sloped belts: retracted only). Pushing (and a retracted / extended funnel the belt
    moves away from) extracts every `defaultExtractionTimer` ticks what the block below takes from
    the funnel's side (andesite 1, brass the amount setting); pulling (or one the belt moves
    towards) takes belt items crossing its entry (the segment centre; extended: the segment edge)
    into its inventory: the whole stack (*up to*) or exactly the amount; one facing against the
    movement holds back what it can't take (powered, filtered out, full). Belt funnels don't
    collect item entities and aren't used by hand; the back plate follows the mode. Filter slot
    and amount box: on the front (extended: on top).
  - Flaps on the client mod: Create's four strips swinging after each transfer ("funnels.flap"
    {dim, pos, inward}; damped as Create's LerpedFloat, .05 per tick); horizontal funnels are left
    out of the chunk mesh and drawn every frame.
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
  normal; industrial iron block: encase. Empty-hand right-click takes the stack out. Belts work
  through the item capability (a chute below a belt pulls the items on that segment; a chute above
  drops onto it) and belt ends / ejectors / tunnels insert into an open chute from any side while
  it can pick items up (Create's DirectBeltInputBehaviour of the chute).
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
- **Item hatch** (U49, Create's ItemHatchBlock / ItemHatchBlockEntity / ItemHatchHandler /
  HatchFilterSlot; `ItemHatch.cpp`): placed only against the side of a block (a click on a top or
  bottom face places nothing), facing into it; waterloggable, netherite sounds, pickaxe, hardness 3.
  A right-click — also while sneaking (ItemHatchHandler) — on anything but its filter slot: with
  no item capability behind it nothing happens (Create's FAIL); sneaking with the wrench is the
  wrench's own; else the held stack (not sneaking: the selected hotbar slot only; sneaking: every
  main inventory slot outside the hotbar) goes into the container behind it as far as it fits
  (insertItemStacked), skipping items that can't go into containers (shulker boxes) and items the
  filter rejects (an empty filter passes everything). When anything went in: Create's hatch sound
  (barrel open at .25 / 1.4 + netherite place at .75 / 1.15), `levitate:open` for 10 ticks and the
  action bar "Held item deposited" / "Inventory items deposited". Deployers (the fake player) do
  nothing. The filter slot is the front of the slanted door (U3 filter slot, scale 0.48); a filter
  item in it drops with the block.
- **Contract extension** (`src/feature/common/FunnelsExt.h`, for U11 and U13):
  `airflow::registerFan(blockId, query)` — a fan's facing and speed (Create's
  `EncasedFanBlockEntity.getSpeed()`), read by chutes; `beltlink::registerSegment(blockId,
  query)` — a belt segment's movement facing, speed, slope and casing, read by tunnels; and
  `beltlink::itemEntering(region, beltPos, stack, movementFacing)` — the belt calls it when an
  item crosses into a segment (Continue / Stuck / Taken), so the tunnel above can take or split it;
  `beltlink::hasAbove` (dropped items don't land under a tunnel) and
  `beltlink::registerAboveSideInput` / `sideInputAbove` (a distributing brass tunnel takes items
  inserted into its segment from a side), added by U41. The belt registers `levitate:belt` as a
  segment; funnels register as the belt's above-interactors and, like chutes, as direct inputs
  (`belts::registerDirectInput`; `belts::canInsertDirect` added by U41).
  Items going *into* belts, depots, funnels and chutes use the item capability with `face` = the
  side they come from.

#### 3.6.13 Bulk processing (U13)

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
extracts only from the outputs. Item crack particles of the input while milling. While it holds an
input it plays Create's MILLING soundscape (below).

**Crushing wheels** (impact 8 SU/RPM, pillar with shaft ends along its axis): two wheels with the
same axis two blocks apart get a `crushing_wheel_controller` between them (when the block is
replaceable); it is valid when both turn and their speeds have opposite signs, facing
`nearest(axis × side · sign(speed) · step(side))` (Create's updateControllers) — the way items move
through the pair. Checked on placement, on speed changes and every 20 ticks; a removed wheel takes
its controllers with it. A wheel can't stand next to a controller on a non-axis side or next to a
wheel that isn't stacked along their common axis: a player can't place it there (Create's
canSurvive, checked before the placement; other placements pop it). Crushing speed = |wheel RPM| / 50.
Controller (valid and turning): the first entity entering it is taken (not creative / spectator
players, no players on peaceful, not items it ejected itself); items are pulled through and taken in
once 0.25 past the centre; the controller has no collision for the entity it crushes (it falls
through) nor, unless it faces up, for the items it ejected (they land on it); the inventory (32 slots) counts `processing_time` (crushing recipe, else
milling, else 100) down by `clamp(4 · crushingSpeed / log2(count), 0.25, 20)` per tick (20 for one
item), applies the recipe once per item below 20 (no recipe: the items are destroyed, as Create)
and at 0 outputs: into the block below / in front with Create's DirectBeltInputBehaviour (the belts
unit's direct inputs: belts, depots, ejectors, basins, saws, crushers, which may refuse the side —
the items wait —; chutes and item drains through their item capability), else as item entities 0.55
past the centre
with Create's out-speed (0.25 sideways, 0.5 up, 0 down). Other entities get `crushingDamage` (4) per
hit (Bedrock's hurt cooldown applies; Create's `create.crush` death message, "%1$s was processed by
Crushing Wheels") and are moved out below the wheels before a lethal hit; their drops roll with
looting II (Create's crushingIsFortunate) and keep no motion. Item capability (and the belts unit's
direct input, Create's DirectBeltInputBehaviour: items travelling the way the wheels pull, any way
when they pull down): insert a whole stack into an empty, unoccupied controller. While something is
inside it plays Create's CRUSHING soundscape.

**Encased fan** (impact 2 SU/RPM, shaft at the back, `facing` = the side the air leaves through).
Placement (Create's getStateForPlacement): against a chute it faces the chute's way; else the side
whose neighbour has a shaft pointing at it (unless ambiguous) or the look direction, turned to face
away (sneaking: towards). Air current, rebuilt on speed changes, neighbour changes and every
`fanBlockCheckRate` (30) ticks:
- pushing when the speed converted to the facing's direction (negated for down/north/west) is
  positive, else pulling; reach `lerp(min(|RPM| / fanRotationArgmax, 1), 3, fanPushDistance |
  fanPullDistance)` (256, 20, 20);
- the reach stops at the first full collision box, unloaded chunk or partial block whose collision
  boxes (all of them: stairs, walls, ...) cover the 5 test points (then `i + depth + 1/32`, the depth
  from the boxes at each test point); air, liquids, non-colliding blocks and
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
  again by that air; the rest spawn next to it); the processing state (type, time) is kept on the
  item entity (tag `lv_fan:<type>:<time>`, Create's persistent data), so it survives saving: splashing / haunting = their recipes rolled per
  item; smoking = the smoker recipe × count; blasting = the furnace (else blast furnace) recipe ×
  count unless the smoker gives the same item (food burns up); anything else that isn't fire
  resistant burns up in blasting air;
- other entities: splashing hurts endermen, snow golems and blazes (2, drowning) and puts out fire;
  smoking sets on fire 2 s and hurts 2 (`create.fan_fire`); blasting sets on fire 10 s and hurts 4
  (`create.fan_lava`; not fire immune); haunting gives blindness (30 ticks) and slowness II (20
  ticks), and turns a horse into a skeleton horse after 100 ticks that keeps its name, health / speed
  / jump attributes, taming, owner and saddle (its armour drops);
- a player in a current hears wind (Create's air current sound): Levitate's own sample
  (`levitate.fan_wind`, `tools/sounds/bulk.ps1`) every 10 ticks, volume `clamp(|RPM| / 128 · .4,
  .01, .4)`, pitch `clamp(|motion| · .5, .5, 2)`;
- items on `levitate:belt`, `levitate:depot` and `levitate:weighted_ejector` in the current (or one
  below a horizontal one) are processed the same way through their item capability;
- airflow particles (`levitate:fan_airflow`, density `fanParticleDensity` 0.5 per tick) flow along the
  current at `(reach + 1 − distance) / 16` blocks per tick, tinted per air type (Create's colours);
  processing particles at items (smoke, soul flames, poof, splash).
Other units use `feature/common/BulkExt.h` (`airflow::sourceAt`, `typeAt`, `canProcess`, `process`,
`processingTime`), e.g. chutes for fans blowing up into them.

**Nozzle**: placed on the clicked face; survives only in front of a fan facing the same way (a player
can't place it elsewhere; other placements pop). Range = the fan's reach (0 when stopped), pushing like the fan. Every 5 ticks it collects
entities within the range it can see; each tick it pushes them `normalize(diff) · (range −
distance) · (1/32, items 1/128)` away (pulling: towards it, except closer than 1.5); sneaking and
creative-flying entities are left alone; poof particles around it. A pulling nozzle holding more than
256 entities explodes (power 2, no block damage) and the entities are gone. The nozzle blocks the
fan's own current (its shape stops it at once).

**Soundscapes** (Create's SoundScapes MILLING / CRUSHING, sent by the server to every player within
16 blocks): sources are grouped per player by type and pitch group (pitch `clamp(|RPM| / 256 + .45,
.85, 1)`), volume `clamp(count / 150, .025, .1) · lerp(distance / 16, 2, 0)` (Create's ambient
volume cap .1), played one block from the player towards their mean position: netherrack hit
(×1.545, pitch .75) every tick, gravel place (×.425, pitch .75) every 2 ticks, and for crushing
netherite break (×2, pitch 1.75) every 2 ticks.

**Client**: rotating parts through the kinetic renderer (millstone stone; crushing wheel; fan shaft
at the network speed and propeller at 5×); every block has a static model for vanilla clients.

#### 3.6.14 Basin processing (U14)

Checked against Create 6.0.10 (BasinBlock, BasinBlockEntity, BasinInventory, BasinRecipe,
BasinOperatingBlockEntity, MechanicalPressBlockEntity, PressingBehaviour, MechanicalMixerBlockEntity,
BlazeBurnerBlock(Entity), LitBlazeBurnerBlock, BlazeBurnerBlockItem, CRecipes). Pure numbers in
`src/core/basin/` (gtest), game side in `src/feature/basin/`, client animation in `src/client/basin/`.
Contents live in BlockData (`data["basin"]`, `["press"]`, `["mixer"]`, `["burner"]`); the unit's
blocks are listed in `lv:basin:machines` and ticked while loaded.

**Basin.** 9 input slots and 9 output slots (64 per slot, one stack per item kind, the first free
slot otherwise), 2 input and 2 output tanks of 1000 mB with enforced variety (one tank per fluid).
Comparator output (U36): Create's calcRedstoneFromInventory over the 18 item slots; blaze burner:
heat ordinal − 1 (at least 0).
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
- Collision: Create's basin shape (a 12 px foot and 2 px walls, hollow above 2 px: entities can stand
  in it). Item entities falling into the basin are taken into the inputs (every tick by default,
  `basinIntakeInterval`; up to half a block above the rim, since Bedrock pushes an item entity out of
  the basin's cell). Belt ends, saws, chutes, ... hand items in through the belts unit's direct
  input (Create's DirectBeltInputBehaviour).
- Spout (Create's updateSpoutput, every 10 ticks): a side can be output to when the neighbour has no
  collision (or is a funnel whose opening doesn't face away from the basin), no belt below it moves
  towards the basin, and the block below it accepts items from that side and is one of
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
  ending in `_manual_only` are ignored. Potion brewing (Create's PotionMixingRecipes,
  `allowBrewingInMixer`): a reagent in the basin and a tank with 1000 mB of a potion fluid (or water)
  brew 1000 mB of the potion the brewing stand would make (Bedrock's PotionBrewing mixes: potion
  mixes for each bottle type, container mixes such as gunpowder → splash), heated.
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
- What it presses (Create's getRecipe): the next step of a sequenced assembly when it is pressing
  (U15), else the first `pressing` recipe; per item the results (sequenced: the advanced
  transitional item) are rolled and merged (RecipeApplier).
- *World*: every 10 ticks (moving) it looks for item entities on the ground in the block below
  (deflated by 1/8) that it can press; at the half cycle it presses the first one (Bedrock
  lists entities in another order than Java, so items without a recipe are skipped): a single
  item (or the whole stack with `bulkPressing`) becomes the results, otherwise one item of the
  stack is pressed and the results drop beside it.
- *Belt / depot* (Create's BeltProcessingBehaviour, the belts unit's processor): a pressable item
  reaching the centre of the belt segment / depot two blocks below is held there and starts a cycle
  (on a moving belt too); at the half cycle the held stack (`bulkPressing`) or one of its items is
  pressed (results stay held under the press until it lifts, the rest of a stack stays for the next
  cycle), then the press lets go.
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
  9, comparator 1 / 2 (soul flame 2).
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

#### 3.6.15 Assembly (U15)

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
"<n> selected interaction point(s) removed due to range limitations." or "Mechanical Arm has <i>
input(s) and <o> output(s)." Points are stored relative to the arm (they turn with it on
contraptions). Interaction point types (Create's registry): basin, belt, blaze burner, chute /
smart chute, mechanical crafter, crushing wheel controller, deployer, depot / weighted ejector /
train station, andesite / brass funnel, millstone, mechanical saw facing up — all through their
item capability — and the vanilla jukebox, composter (in from the top, bone meal from the bottom),
campfire (cookable food) and respawn anchor (glowstone charges it).
Behaviour (Create's ArmBlockEntity): progress towards the chased point grows by
`min(256, |speed|)/1024` per tick; at a point it collects (the amount the outputs can take,
simulated) or deposits; searching for an input (not while powered) and for an output (every 10
ticks) follows the value box *Distribution Method*: Round Robin (after the last used
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
"Spout <fluid>") in the server's language; the client mod shows the same lines in the game's
language instead. Machines running steps: deployers (deploying), presses (pressing: world, belts,
depots), spouts (filling), saws (cutting).

**Item application** (Create's ManualApplicationRecipe, `assets/recipes/item_application/`):
right-clicking a block with the applied item (players and deployers, not sneaking) turns it into
the result block (further results pop out), plays the copper break sound (pitch 1.45) and uses up
the item unless creative or `keep_held_item`. Shipped: andesite / copper / brass casing from
stripped logs and wood, railway casing, bound cardboard; deploying: cogwheel, large cogwheel,
waxing Create's copper shingles / tiles.

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
  (`allowStonecuttingOnSaw`), cutting first; a sequenced assembly's cutting step (U15) comes before
  all of them and alone when the filter allows its transitional item; the recipe filter slot (on top, 3 px beside the
  blade; U3 filter slot) keeps the recipes whose first result matches; several recipes cycle per
  item. Time = recipe time (50 for stonecutting) × max(1, count / 5), counted down by
  clamp(|RPM| / 24, 1, 128) per tick; below 5 the results are rolled (once per input item, merged),
  then 20 more; no recipe: 10 ticks, the item passes through. Output: along the item movement
  (±x when not along first, ∓z otherwise; sign of the speed) into the block with Create's
  DirectBeltInputBehaviour there (the belts unit's direct inputs: belts, depots, ejectors, basins,
  saws, crushing wheels; chutes and item drains through their item capability), which may refuse
  the side (the items wait), else ejected (centre + move × .5 + (0, .5, 0),
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
- Entities inside the block (but items) are hurt like by the drill (Create's `create.mechanical_saw`
  death message, "%1$s got cut in half by Mechanical Saw"). The saw takes items from belt ends and
  other saws (the belts unit's direct input).
- Contraption actor: breaks sawable blocks in its path (stalling the contraption), fells their
  trees (not for leaves), drops into the contraption's storage first (`moveItemsToStorage`).

**Mechanical drill.**
- *Placing* (DirectionalKineticBlock): away from a single neighbour with a shaft towards it,
  else facing the player (sneaking: away).
- *Breaking* (both machines): every hit adds clamp((int)(speed / hardness), 1, 10 − progress)
  (speed = |RPM| / 100), then waits (int)(hardness / speed) ticks; at 10 the block breaks with
  its normal drops (no tool) at its centre, then the drill resumes on its next lazy tick
  (10 ticks). Never: air, liquids, unbreakable blocks, Create's non-breakable list. Every hit plays
  the block's own hit sound (LevelSoundEvent `Hit` with the block's runtime id, as Create's
  SoundType.getHitSound), cracks shown while breaking.
- Entities (not items) inside its block lose min(s/16, 2) + min(s/32, 4) + min(s/64, 4) health
  per hit (s = |RPM|; Create's `create.mechanical_drill` death message).
- Contraption actor (BlockBreakingMovementBehaviour): visits the block at its centre + facing ×
  .65, hurts entities there (clamp(6·|motion|^0.4 + 1, 2, 10)) and throws them, stalls while
  breaking blocks with collision (no tracks) at speed clamp(|animation speed| / 500, 1/128, 16),
  falling blocks above are broken top-down and re-visited after 10 ticks; inactive while moving
  backwards; drops go to the contraption's storage first.
- Client mod: the blade and the drill head turn with the kinetic renderer (`rotor` bones; the
  drill uses a mirrored bone for negative facings). Vanilla clients see the static blade / head.

#### 3.6.17 Fluid transport (U17)

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
with `lavaSourceConversion`), a full cauldron (1000 mB), a full beehive (250 mB of honey) or the water
of a waterlogged block (1000 mB; the block stays, dry). A waterloggable block (Java's `WATERLOGGED`
property: on Bedrock the block's water detection rule, vanilla blocks and custom blocks with
`minecraft:liquid_detection`) without a sturdy face towards the pipe is an open end too. Pushing
fills the buffer and places the fluid's source block once it holds 1000 mB (not on a source, not when
`pipesPlaceFluidSourceBlocks` is off; water evaporates in the Nether with the fizz sound); in front of
a waterloggable block only water goes out, waterlogging it (not when it already is); fluids
without a world block (milk, potions, tea, and honey / chocolate until U19 gives them blocks) are used
up 1 mB per transfer while their effect applies. Pouring into another fluid reacts (water on a lava
source: obsidian, on flowing lava: cobblestone; lava on a water source: stone, on flowing water:
cobblestone). Effects on the block in front and one below (two when the pipe faces down): water every
5 ticks puts out burning entities, fire, campfires and candles; lava every 5 ticks sets non-immune
entities on fire for 3 s; milk every 5 ticks clears the effects of living entities; tea every 5 ticks
gives Haste I for 21 ticks (hidden); potions apply the potion's effects on every transfer, instant ones
(healing, harming) at half strength (`MobEffect::applyInstantaneousEffect` with scale 0.5, Create's
PotionEffectHandler). Fluid pipes, glass pipes, pumps, valves and smart pipes are waterloggable
(`minecraft:liquid_detection`): placed in water they keep it. Logs: `pipes: drained the water of the
waterlogged <block> at ...`, `pipes: waterlogged the <block> at ...`.

**Particles** (Create's PipeConnection / FluidFX, for every client: the server spawns the resource
pack particles `levitate:fluid_particle` and `levitate:fluid_drip`, tinted with the fluid's colour,
for the players within 20 blocks): every tick each pipe face with a flow pours one particle out of an
open end (sucked in towards the face when pulling), or drips one at its rim with a chance of 1/1000;
a flow that just started splashes once (a rim drip, at open ends the stream).

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
(vanilla clients); the client mod draws Create's flow fronts instead (TransparentStraightPipeRenderer:
per face with a flow, a 6 px wide column of the fluid growing `progress / 2` blocks from the face
(inbound) or from the centre (outbound), advanced every tick at the flow's speed; the server sends the
flows of glass pipes over `pipes.glass` when they change, chunk meshes leave the static fluid out);
pumps' cogs and valves' shafts rotate through the kinetic renderer (U2).
**Command** `/create pipes <pos>` (op): pressures, flows and transfers of a pipe.

#### 3.6.18 Fluid tanks, boilers, steam engines and whistles (U18)

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
`fractionToRedstoneLevel(amount / capacity)` (floor(14·f + (f > 0))) of the whole structure (also
creative tanks), read by vanilla comparators through `common/Redstone.h` (U36). Right-click (not
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
engine's four side faces. Puffs of steam (sound `levitate.steam.engine`, a `levitate:steam_jet`
particle out of the cylinder's front) twice per shaft turn while it runs.

**Whistle** (WhistleBlock / BlockEntity). Pitch = 2 semitones per full extension above it, 1 for a
half (single) one at the top (`updatePitch`, up to 25). Using a whistle item on a whistle or an
extension grows the column (Create's `incrementSize`: up to 6 blocks: a half extension becomes full,
else a half extension is added on the next free block; the item is not used up; place sound pitched
2^(−2i/12)). Wrench: the next size (small → medium → large → small) for the whole column; sneaking
with the wrench on the top extension shortens it by half a block. Extensions without a whistle or
extension below vanish. Powered by redstone (polled, §8) on a boiler with heat (passive or active) it
sounds: the `levitate.steam.whistle_chiff` hiss when it starts (per player at volume
`.175 · (64 − distance) / 64`, pitch `f + .75` for small whistles), then the tone as one looping
sound instance per whistle (Create's WhistleSoundInstance): one original sample played at
`f = octave · 2^(−pitch/12)` (sounds `whistle_high` / `whistle_medium` / `whistle_low` are the same
sample at pitch 2 / 1 / .5). The server gives the instance a sound handle and plays it looping
(PlaySoundPacket loop count −1 + server sound handle) on the clients of the players within 64 blocks;
a player coming into range gets it, one leaving it (68 blocks) gets Stop; a pitch change (a longer
or shorter column) is sent to the playing clients as SetPitch (ClientboundUpdateSoundDataPacket), a
size change starts a new instance, and a whistle that stops (unpowered, no steam, removed, chunk
unloaded) fades out over 4 ticks (Create: −.25 volume per tick) and then stops. Every client plays it
with the game's own sound engine (no client mod needed). A `levitate:steam_jet` particle (Create's
SteamJetParticle: a grey-white puff that grows, rises and fades) leaves the mouth when it starts and
every 8 ticks, up and out of the mouth. One log line when it starts / stops and one when the looping
instance starts.

**Client** (client mod): the server sends tank contents (`steam.tanks`: controller, size, fluid,
amount, capacity, windows, boiler, and for boilers the gauge target `g` and the hidden sides `occ`)
and engines (`steam.engines`: position, facing, shaft speed and axis) to companions; the client draws
the fluid inside windowed tanks as Create's renderer (1/4 caps, a 1/16 puddle, inset by the hull, the
level easing towards the real one) in the fluid's colour, and each running engine's piston rod,
crosshead and connecting rod following Create's `(1 − sin a) / 4 · 24/16` stroke on a crank pin
turning with the shaft. **Boiler gauges** (Create's FluidTankRenderer.renderAsBoiler): a boiler draws
no fluid but a brass gauge in the middle of every side of its bottom layer, the needle pivot 6/16 up,
the needle turning `145° · gauge` from its rest position; the gauge chases (Create's `chase(target,
.125, EXP)`) the target `1/8` for a passive boiler, else `level / size limit` (Create's
BoilerData.read); a side whose gauge position has a block with a collision box in front of it
(Create's updateOcclusion) shows none. Vanilla clients see the tank only (§8).

**Persistence**: tank parts in BlockData (`data["steam"]`: controller, width, height, window,
fluid, amount), engines' value box there too; the engine / whistle / powered shaft index in
`lv:steam:index`, synced tanks in `lv:steam:tanks`. Boiler water supply samples are not saved
(they refill within 50 ticks).

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
  BeltProcessingBehaviour). Belts and depots (U11) hold the item through their processing behaviour
  (`belts::registerProcessor`, Create's onItemReceived / whenItemHeld): an item that can be filled
  stops at the centre of the segment under the spout (also with an empty tank), is filled one at a
  time (the filled one moves on, the rest of the stack stays held) and an item the fluid can't fill
  passes. Other **Levitate** blocks exposing the item as slot 0 from the top through the item
  capability are filled through it (the result goes back, leftovers drop; vanilla containers have no
  processing behaviour). Filling takes 20 ticks (FILLING_TIME); the item is filled 15 ticks in, one
  item per cycle. With too little fluid it waits. A cauldron two blocks below is filled with a whole
  bucket (water, lava, powder snow). Sounds: start / fill. Particles for every client (U17's
  `levitate:fluid_particle`): a drop leaves the nozzle every tick from 20 to 8 ticks left, 20 drops
  splash off the item when it is filled. Client mod: the stream (its width follows Create's
  `((2p − 1)² − 1) / 32`) and the nozzle drawn in three bits (top, middle, bottom) that squeeze
  together (Create's squeeze: the stream's curve, −1 from 10 to 2 ticks left, then back; each bit
  `3/32 · squeeze` above the one before), the chunk mesh leaving them out (`levitate:nozzle` 1).
  Logs: `spout at x y z filled <in> into <out>`, `... filled a cauldron with <fluid>`.
- **Item drain**: 1500 mB tank, pipes extract on every side, nothing is inserted from outside.
  Items enter through the item capability (one at a time when they hold fluid, otherwise the whole
  stack), as a direct belt input (U11's `registerDirectInput`, Create's DirectBeltInputBehaviour:
  belt ends, saws, chutes hand items in; not travelling upwards), from item entities landing on top, and travel at 1/8 block per tick away from the side
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

#### 3.6.20 Linear contraptions (U20)

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
- Sequenced gearshift (U6) `MoveDistance` context: pistons and rope pulleys (not mirrored pulleys,
  not elevators) take its value as `sequencedOffsetLimit` on a speed change or a new instruction;
  each step is clamped to what is left and the limit counts down, so the structure stops at exactly
  that distance and waits until the speed changes (Create's getMovementSpeed / tick).
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
pulley breaks the rope below. Ropes and magnets are brittle and unmovable, and climbable like ladders (Create's `minecraft:climbable` tag: their block types get the Climbable property on the server and in the client mod). Stress impact 4.

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
the wrench) to open Create's contact screen (native for the client mod, `pistons.elevator_contact`, saved when confirmed or closed; a form for vanilla clients) with the floor identifier (4 chars), description (30 chars) and door control (stored for
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
hits the world; then it is placed (grid aligned). A `MoveDistance` sequence context reaching the
shaft when the carriage assembles limits the gantry to that distance (Create's limitMovement): it
then holds until its shaft stops; a new instruction reaching the shaft at an unchanged speed sets a
new limit (Create places and re-assembles the gantry between two steps). A powered shaft drives the carriage as a gear instead
(Create's pinion ratio). Nested gantries work through the world (an inner carriage starts when its
shaft turns after the outer one stops). Stress impact 0 (Create: setNoImpact; the kinetics table in
`config/kinetics.json` still lists `gantry_shaft` 4 and must become 0).

**Client mod**: pistons, pulleys and gantries rotate their shaft / pinion with the kinetic renderer
(`levitate:rotor`); running pulleys draw their rope (elevators: belt) and magnet from the pulley to the
structure (`pistons.rope` messages, every 3 ticks, extrapolated in between); contraption blocks are
drawn by U4's display entities.

#### 3.6.21 Rotating contraptions (U21)

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
  it). A sequenced gearshift's / valve handle's `TurnAngle` context reaching the bearing (read on a
  speed change or a new instruction; windmills never) limits the turn: each tick's step is clamped
  to the degrees left (`sequencedAngleLimit`, saved in the BlockData), so it stops at exactly the
  angle. When the speed changes sign the angle is rounded to a whole degree. When the speed drops to 0
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
  for bearings facing a negative direction (clockwise seen from the front). **Clock Hand Arrangement** box:
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
  `|motion| · 12 / max(1, width / 6)`° per tick) / Pause actors while rotating (the same, its actors
  don't act while it turns) / Lock rotation. The assembler's and the controller rail's redstone input
  comes from the shared `redstone::watch`.
  If the cart breaks, the structure is placed where it is.
- **Controller rail**: a cart on it is driven at `0.4 · power / 15` b/t in the rail's direction when
  powered and not moving against it; otherwise it is braked to the rail's centre.
- **Minecart coupling**: use on a cart, then on another: coupled at their distance (whole blocks,
  at least 2) if ≤ `maxCartCouplingLength` (32), each cart has at most two couplings and they don't
  form a loop (`create.minecart_coupling.*` messages); one coupling item used (not in creative).
  Coupled carts keep their distance (Create's soft collision step: each cart corrected along the
  coupling by the predicted stretch, clamped to 1.75, projected on its rail, speeds ≤ 0.4). Wrench on
  a coupled cart removes its couplings (items dropped, "Removed all couplings from minecart"); a
  broken cart drops its couplings. The client mod draws each coupling between the carts (Create's
  CouplingRenderer: a ring on each cart's end and the connector between them, following both carts
  every frame; pairs sent on `bearings.couplings`).
- **Minecart contraption** (item, stack 1): wrench right-click on a cart carrying a contraption
  (survival only when `survivalContraptionPickup`) picks it up as `minecart_contraption` with the
  structure (a chest minecart: `chest_minecart_contraption`, carrying the cart's inventory too); the
  contraption is discarded (nothing is placed); refused for spawners ("A mystical force…") and for structures over 512 KB of data
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
  The roller also hurts them: `clamp(6 · |motion|^0.4 + 1, 2, 10)`, with its own death message
  (`death.attack.create.roller`, "%1$s was flattened by a Mechanical Roller"). Items, display
  entities and seat entities are left alone. A block being broken plays its own hit sound (the game's
  `Hit` sound event with the block's runtime id, Create's getSoundType().getHitSound()).
- **Support**: harvesters, ploughs and rollers hang on the block behind them (Create's
  AttachedActorBlock): placing one where that block has no collision shape is refused (the item
  comes back), and one whose block behind goes drops as an item (block changes watched centrally).
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
- **Portable storage interface**: comparator output 15 while connected, else 0 (also the portable
  fluid interface; U36). A carried PSI's area is 1.85 ahead of its front; a stationary
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
  recolours it (the dye stays); in the crafting grid any seat + a dye gives the seat of that colour
  (one recipe per pair, `tools/gen/seats.ps1`). An entity landing on a seat takes half the fall
  distance; one that doesn't sit down (players, non-pickable mobs, a taken seat) bounces
  (`vy = −vy · 0.66`, 0.8 of that for non-living; sneaking players don't). A living mob (not a shulker / armor stand; hostile ones only with
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
CLogistics; U49: DeskBellBlock(Entity), DeskBellRenderer, BellMovementBehaviour, RoseQuartzLampBlock).
Code: `src/core/redstone/` (state machines, link networks; gtest),
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
  a click in the overlap sets both, as Create; item-frame sound (.25, .1). The client mod draws
  both items in their slots (display items, Create's LinkRenderer; U42). Sneak + empty hand or
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
  not the wrench) toggles `inverted` (lever click .3, pitch .6 / .5). On the delay box (client
  mod, U42) a short click toggles `inverted` too and holding opens the board (vanilla: the click
  opens the board).
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
- **Desk bell** (U49, Create's DeskBellBlock / DeskBellBlockEntity / DeskBellRenderer /
  BellMovementBehaviour; `facing` 0..5 = the clicked face, `powered`, `rotor`): a click (empty hand
  or any item but the wrench, not sneaking with an item) plays `levitate.desk_bell` (our own
  synthesised ding, `tools/sounds/redstone.ps1`; Create's "Reception bell dings") and powers it:
  15 weakly on every side and strongly into the block it stands on, released 20 ticks after the
  last ring. The wrench turns it (WrenchableDirectionalBlock), sneak + wrench picks it up. Placed in
  water it keeps the water. The client mod draws the plunger dipping and the bell wobbling (the
  animation falls 1 → 0 by 0.05 a tick; tilt `p^1.25 · 8°` along a random phase); vanilla clients
  see the plunger pressed while it is powered. On a contraption it rings when it starts, stops and
  turns around (an elevator: when it arrives at a floor), never on a train.
- **Rose quartz lamp** (U49, Create's RoseQuartzLampBlock; `powered`, `powering`, `activate`):
  placed with POWERED = its input (it does not light up). A rising input lights it (POWERED,
  POWERING, ACTIVATE) and darkens every other lamp of its cluster (lamps connected through lamps
  within 16 blocks, Manhattan); one tick later every lamp of the cluster takes POWERING = ACTIVATE
  (so lamps activated in the same tick stay lit). A falling input only clears POWERED: the lamp
  stays lit. A lit lamp emits 15 to everything but other lamps (not a conductor); a comparator
  reading it gets 15 for a lit lamp, one less for every lamp further along the column behind it up
  to the first lit one (0 when the column ends first). The wrench toggles a lamp (Create's
  getRotatedBlockState cycles POWERING), sneak + wrench picks it up. Light level 15 while lit.
  Ponder scene `rose_quartz_lamp` (U30).

Config `config/redstone.json`: `linkRange` (256). Log lines: one per player action (placement,
mode toggle, frequency change, lever change, inversion, latch click, threshold settings, a
contact powered by a contraption); circuit-driven state changes are not logged.

#### 3.6.24 Display (U24)

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
`accumulate_items`), `formatValue` (Create's ValueListDisplaySource.shorten, integer division:
1234 → "1k", 3456789 → "3m"), `progressBar` (filled length truncated). A registration
with the same (block, source id) replaces an earlier one, so the owner of a block can refine a
built-in source. `Context` gives the region, the link, source and target positions, the source's
persisted `config` (Create's sourceConfig) and the game time.

**Display link** (data `BlockData["display_link"]` = target offset, line, source id, config;
positions in `lv:display:links`): every tick its timer grows; at the source's
`getPassiveRefreshTicks` (default 100; stopwatch 20, accumulated items 200, observed train name 400,
station summary / status 20) it resets and, unless the link is powered, reads the source and writes
the target (`updateGatheredData`). A newly opened link screen starts at the block's first source
(Create's screen does, and applies it when closed). Redstone: powered
links send nothing; when the power goes the timer resets, the source's `onSignalReset` runs and the
data is sent at once. The source is the block behind the link; a selected source the block no
longer offers is deselected. Lines of a display board are *reserved* by the link that wrote them
(another link skips them while that one still targets the board). As Create's
DisplayBoardTarget.acceptFlapText a board takes link text only while it turns fast enough; the first
line is taken, the following ones stop at a line another link or a player wrote, and a multi-line
source clears the lines below its text.

**Sources** (ids, names and options as Create): speedometer `kinetic_speed` (Ignore / Include
Direction, "64 RPM" with a space everywhere), stressometer `kinetic_stress` (Progress Bar / Percentage /
Stress in SU / Total Capacity / Remaining SU, "su", a space on display boards), cuckoo clocks
`time_of_day` (12 / 24-hour, hours padded with a space, minutes rounded down to 5, "--:--" when the
clock stands still, nonsense outside the overworld) and `stopwatch` (h:mm:ss since the last signal
reset), smart observer (block `content_observer`) `count_items` / `list_items` / `count_fluids` /
`list_fluids` / `read_package_address` (contents of the observed block through U3's capabilities and
the observer's filter slot; the address of the first matching package, else of the first matching
package looping around a chain conveyor there, U25's `loopingPackages`), threshold switch (block
`stockpile_switch`) `fill_level` (Percent / Progress Bar), fluid tanks `boiler` (Create's
BoilerDisplaySource: "Boiler:  <level>" and the size / water / heat bars of U18's goggle lines; "Not
enough space for Boiler Status" below 4 rows), elevator contacts `current_floor` (the floor the
contact last reported, U20's BlockData), belt / depot / weighted ejector `item_names`, tunnels
`accumulate_items` / `item_throughput` (per second / minute / hour; Create's pool of the last 10
batch rates, 0 after twice the usual time without items), seats `entity_name`, nixie tubes
`nixie_tube`, and the vanilla `target` → `redstone_power` (Number / Progress Bar),
`enchanting_table` → `enchant_power` (Max Enchanting Cost: twice the bookshelves, at least 1 +
shelves / 2 ⚠ Create rolls Java's bottom-slot cost), command blocks → `scoreboard` (objective id,
value list, "name value"), `respawn_anchor` → `death_count` (Player Deaths, counted by Levitate:
"name value", full numbers, no options). Every single-line source has Create's "Attached Label"
option where Create has it. Counts are plain numbers; other numbers use Create's formatting (up to 2
decimals, grouped thousands); percentages are truncated; fluids as Create's FluidFormatter ("1000
mB"; lists "1.5B" with Shortened). Progress bars fill the target width minus the label (a sign: 6/9
of it, a display board: wide flaps 7/9). Value lists put the value first except scoreboards and
deaths; a lectern gets 8 entries per page. Station summary / train status (U27), observed train name
(U28) and factory gauge (U26) are registered by those units.

**Targets**: display board (2 lines per block row, `⌊(16·width − 2) / 3.5⌋` characters per line: 4,
8, 13, 17, …, 145 at 32 wide), nixie tube row (1 line, 2 characters per tube), sign (4 lines,
⚠ 15 characters, both sides as Create's SignDisplayTarget), lectern (one page per text, ⚠ 256
characters; a book and quill becomes a written book "Printed Book" by "Data Gatherer" ⚠).

**Display board** (data at the controller `BlockData["display_board"]`, controllers in
`lv:display:boards`): the controller is the top block at the left end as seen from the front
(Create: no UP flag, no identical neighbour clockwise); width up to 32 counter-clockwise, height
down the DOWN flags; rows connect up / down only when the whole row matches. A size change keeps
line colours and glow and clears the text. Right-click a line (hit point moved ⅛ block into the
block; not sneaking): empty hand clears it (needs rotation), dye colours it (also on a stopped
board; white resets to Create's default `#D3C6BA`), glow ink makes it glow, a named name tag writes its
name and a clipboard the entries of its last viewed page (split at line breaks) from that line down
(both need rotation, Create's FlapDisplayBlock.useItemOn). The flaps show upper case (Create's
FlapDisplaySection.refresh; § colour codes kept).
Every 10 ticks (Create's lazy tick) the state goes to companions (`display.board`); the client mod
spins every character through Create's flap cycles (letters " ABCDEFGHIJKLMNOPQRSTUVXYZ", digits
" 0123456789", anything else at once; one step per tick, all at once above 128 RPM) only while the
board turns, and draws them on the front face.

**Nixie tubes** (data per tube `BlockData["nixie"]`, tubes in `lv:display:nixies`): a row is the
tubes with the same facing and face, from the left (floor / ceiling: opposite the facing; wall: up;
wall reversed: down); placed on the floor / ceiling the player's facing is turned clockwise, on a
wall the clicked face is used (reversed when the look points against the clockwise side of the
direction the player looks at the wall, Create's DoubleFaceAttachedBlock). Custom text (clipboard:
first entry of the last viewed page, name tag, display link) goes to the whole row, tube *i* showing
characters 2i and 2i+1; an empty hand clears the clicked tube (Create's NixieTubeBlock). Without text a tube shows the redstone power around
it as "00".."15" (polled every 2 ticks). Dye recolours the whole row (block id changes, data kept).

**Clipboard** (item data `lv:clipboard`, block `BlockData["clipboard"]` = type, pages of {text,
checked}, last opened page, copied settings): right-click (in the air, or on a block without
settings) opens it; pages 1..51, ~15 entries per page ⚠ (Create: height-limited); "#text" entries are
addresses (no checkbox); empty entries and pages are dropped when saved; Erase checked items.
Right-click a block with value boxes or filter slots (U3) copies them ("Copying settings from …";
values per box id, filters per slot id), left-click pastes ("Applied settings to …"; in survival a
filter item is taken from the inventory ⚠); right-click another clipboard merges its entries
(duplicates skipped, pages of 12, check marks kept). Redstone links copy / paste their two
frequency items ("Frequencies") and pulse repeaters / extenders / timers whether they are inverted
("Block": "Inverted"), as Create's ClipboardCloneables. A left-click on a placed clipboard (or a
sneak-right-click) picks it up with its pages (Create's breakAndCollect). Sneaking disables copy /
paste (Create). Item copying (Create's ItemCopyingRecipe): a written schedule or filter (attribute,
package filter) crafted with blank ones of the same kind gives that many + 1 copies (recipes
`<item>_copying_<n>` and a craft hook that copies the data, enchantments dropped; two written ones
are refused). Clipboards are not copied: on Bedrock they stack to 1, so no recipe can give several.
Entries may carry an item icon and an amount (Create's `ClipboardEntry.displayItem`) and a
clipboard may be read-only (Create's `Readonly`: entries can be checked off, not edited or
erased). Other units write onto clipboards through `src/feature/display/Clipboard.h`:
`addAddress` (U25 package ports: "#address" on the first page with at most 11 entries, "Added '…'
to clipboard") and `makeWritten` (U29's material checklist).
**Clipboard Actions** (client mod, Create's ClipboardValueSettingsHandler): while a clipboard is
held and the crosshair is on a block, the client asks the server what the clipboard can do there
(`display.clipboard_query`: copy = the block has settings, paste = the copied settings would
apply, simulated); then the block gets a white outline and the tip "Clipboard Actions" /
"Right-Click to Copy settings" / "Left-Click to Paste settings" (a placed clipboard: "Right-Click to
Copy entries from this Clipboard") is drawn next to the crosshair.

**Vanilla clients**: board text and nixie characters as name tags of an invisible entity
`levitate:display_text` (one per board / tube; the client mod hides them); forms for the link
(source, line, options; a second form for a newly chosen source's options) and the clipboard (one
page per form: inputs, check toggles, page selector, erase checked).

**Client mod**: the clipboard screen edits the page in place: one native text field per entry plus a
blank one that adds an entry (512 characters each), check boxes, page buttons and Erase checked
(read-only clipboards: labels and check boxes only; item entries show the icon and amount); a
field's text is sent with Enter or when it is left (`{op: "set", page, entries}`; blank entries are
dropped). Text options of the display link's source (e.g. a label) are text fields too
(`{op: "option", key, value}`, 100 characters). The screens wait with rebuilding on new data while
the player types.

**Commands** (op, Levitate debug): `/create display text <x y z> <line> <text>` (`_` = space) writes
a board line or a nixie row; `/create display bind <x y z> <tx ty tz> <source>` binds a link and
sends.

#### 3.6.25 Packages (U25)

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
  right-click opens it into the inventory ("Package breaks" sound; a spawn egg inside spawns its mob
  in the block one step ahead of the player, using up one egg of the stack, as Create's
  `PackageItem.open`); holding right-click and
  releasing throws it with the bow's power curve (`(t² + 2t) / 3`, t = seconds held, capped at 1,
  thrown only from 0.1; speed 2 × power, snowball sound at volume 0.5, pitch 0.5); right-click on a
  block puts it down there. Packages, shulker boxes and bundles don't fit into packages.
- **Package entities** (U46, Create's PackageEntity): thrown, put-down and dropped packages are
  `levitate:package` entities (a box model per style from the resource pack, so vanilla clients
  see them too; size = the style's width × height / 16, rare styles 12 × 10; a random yaw; health
  5; gravity and drag of a mob). The package item (contents, address, order data) is kept in the
  world DB (`lv:packages:entity:<unique id>`). Put down: at the click point (from below: lowered by
  its height + 0.25; on a side: out by half its width), nothing when a package already stands
  there. Thrown: from the player's mid-height plus the motion, keeping the motion. Every package
  item entity joining the world (drops with Q, broken containers, ...) becomes a package entity
  on the next tick with 1.5 × its motion. Right-click with an empty hand: the package goes into
  the hand (pick-up sound at volume 0.2, pitch 0.75–1.75). A hit (not in adventure mode), an
  explosion, a projectile, water, or fire / lava burning through its health break it open: the
  contents drop at the entity (spawn eggs spawn their mob there), "Package breaks" sound, item
  particles of the box; the package item itself is not dropped. The void and `/kill` remove it with
  its contents. No fall damage; landing after a fall plays the chiseled bookshelf fall sound. Create's centerPackage: on a belt (not rising, no tunnel on the
  segment), landed on a depot / ejector or on a chute that takes items, the package slides to the
  input point (motion × 0.75, y × 0.25, 20% of the way per tick; insertion delay 30, +1 per tick,
  −3 per centering tick) and is inserted; inside a funnel it is handed to the funnel as an item
  entity at the funnel's centre (taken back as an entity after 20 ticks if the funnel refused it,
  and not offered again for 100 ticks).
- Comparator output (U36): packager / re-packager 15 while holding or animating a package, else 0;
  frogport / postbox calcRedstoneFromInventory over the port's 18 slots.
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
  loop the player leaves towards the chain they look along; Create's riding handler: every tick the
  rider's motion becomes motion · 0.75 + (chain point − player top − 0.5) · 0.25, sent to the
  rider's client like knockback, no fall damage; off when more than 3 blocks away or 1 below after
  the first 20 ticks), sneak-right-click a chain to remove it
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
- **Ports and clipboards** (Create's `addAddressToClipboard`): right-clicking a frogport or postbox
  with a clipboard adds "#address" to it instead of opening the screen (not for a blank address or one
  already on the clipboard; "Added '…' to clipboard").
- **Postbox**: the same port inventory and address; it targets a train station (right-click the
  station with the postbox, then place it within 5 blocks, below is allowed). `levitate:flag` is up
  while it holds packages, `levitate:open` while its screen is open (barrel sounds). U27's trains use
  `postboxesForStation` / `insertIntoPort` / `extractFromPort`.
- **Package filter**: right-click sets its address (native screen `packages.package_filter` with one
  address box for companions, a form for vanilla players); in every filter slot (U3's
  `filter::registerMatcher`, Create's PackageFilterItemStack) it lets packages whose address matches
  through (an empty one behaves like a plain item). Lore `-> address`.
- **Screens**: the port screen (address, send-and-receive / send-only, the 18 package slots: click
  one to take it, click a package in the inventory to put it in) is native for companions; the
  address is typed into the screen's address box (native text field, at most 50 characters, sent
  with Enter or when the box is left: action `{op: "address", text}`); vanilla clients get one form
  (address, mode, take a package, put my packages in).
- **Client mod**: draws both strands of every chain, the boxes hanging on chains (interpolated
  between the 4-tick updates), the frogport's tongue with the box it catches or sends, and the
  packager's tray sliding in and out; the wheel turns as a kinetic rotor at Create's full size
  (`geometry.levitate.chain_conveyor_wheel`, 2.5 blocks; the block model keeps a 28 px wheel for
  vanilla clients, block geometry stops at 30 px).
- **Sounds**: the packager, package pop, depot plop and the frogport's open / catch / deposit are
  original samples (`tools/sounds/packages.ps1`, `levitate.packages.*`).
- **Log lines** (e2e): `packages: packager at <pos> activated by redstone`, `... packed a package
  for '<address>' (<n> stacks)`, `... unpacked a package ...`, `chain conveyors at A and B
  connected`, `frogport at <pos> caught a package for '<address>'`, `... sent a package ...`,
  `<player> opened a package (<n> stacks)`, `<player> threw a package ...`, `<player> put down a
  package at ...`, `<player> picked up a package entity`, `a package entity broke open (<n>
  stacks)`, `a package entity went into the <block> at ...`, `a package entity was removed with
  its contents`.

#### 3.6.26 Stock network (U26)

Pure logic in `core/stock/Logistics.h` (gtests); the unit's contract-style API is
`feature/stock/Stock.h` (networks, stock summaries, orders, packager seam, gauge panels).

- **Networks** (Create's GlobalLogisticsManager): a network is a frequency (UUID). Linked block
  items (stock link, ticker, requester, gauge) right-clicked (not sneaking) on a stock link,
  ticker or requester take its network ("Tuned to this network"); a gauge panel with an item
  tunes held linked items too. Right-click in the air clears a tuned item ("Cleared connection to
  network"); tuned items show "Frequency configured" / "Place in crafting grid to reset" in their
  lore. Placing an untuned link / ticker / requester starts a new network ("New link network
  started"), a tuned one joins ("Connected to existing network successfully"); gauges must be
  tuned ("Tune to a stock link before placing"). The player who placed the first link owns the
  network; the owner may lock it (request screen): then only the owner interacts with its blocks
  ("Logistics Network is Protected"; at a keeper: "Interact using a Shopping list, direct access has
  been locked"). Networks (owner, lock, every link position, promises) are saved in the world DB
  (`lv:stock:networks`); block positions in `lv:stock:blocks`; block settings in BlockData
  (`data["stock"]`).
- **Stock** (Create's LogisticsManager): the network's stock = the target inventories of the
  packagers its loaded stock links sit on; a link powered with 15 leaves the network (the power is
  read every tick, U42: as Create's neighbour updates, custom blocks get none); links on the
  same inventory count once. Summaries are cached 20 ticks (accurate: 1 tick). Orders are split
  over the links sorted by redstone power (lowest first), one random link per shared inventory,
  each entry taken from the links in turn; requests carry the address, order id, link index, the
  final-link flag and (first request only) the order context. A packager with too many queued
  packages refuses: player orders 50, redstone 20, restock 10. Items that newly appear in a
  linked packager's inventory fulfil the network's promises (oldest first).
- **Packagers** plug in through `stock::registerPackagerProvider` (is-packager, available items,
  inventory id, busy check, send requests). The unit registers U25's packagers
  (`feature/packages/Packages.h`): a link uses the packager it sits on (not a repackager), the
  packager's target inventory (opposite its facing) is counted slot by slot, requests go to
  `packages::attemptToSend` (up to 100 packages at once, Create's performPackageRequests), and
  U25 asks the unit whether a packager has a stock link (linked packagers ignore redstone).
  Without a packager a link contributes nothing ("No Packagers Linked").
- **Stock ticker**: right-click the block hands out the payments its shops collected (27 slots);
  otherwise it opens the category editor if a keeper is present ("Seat a mob or blaze burner next
  to this block to use it"). Keepers: a mob riding something (a seat) beside the ticker at its
  level or one below, or a blaze burner (with a blaze) beside it at its level. Right-clicking the
  keeper (the mob, or the blaze burner block) opens the stock keeper request screen; with a
  shopping list in hand it checks out. Category editor: categories are list / attribute filters
  (taken from the inventory, dropped back when deleted or when the ticker breaks) or empty
  dividers, named, moved up / down (Shift: to top / bottom). Request screen: the network's stock
  by count in the ticker's categories (+ "Unsorted"; categories can be hidden per player), search,
  up to 9 ordered item kinds (click +1, Shift +1 stack, right-click -1), the package address
  (remembered per ticker), "Send" (sound, "Request Sent!") or, holding a redstone requester or a
  table cloth, "Program Requester" (the order, address and ticker position go into the held item;
  a requester item is tuned to the network).
- **Redstone requester**: comparator output 15 when the last request succeeded (U36). A rising redstone edge (polled every tick) sends its order: with
  "Must send all items" every entry must be in stock (else deny sound, nothing sent); with
  "Allow partial orders" it sends what is there. Placed from a programmed item: "Connected to
  Stock keeper successfully" if the ticker is within 128 blocks in the same dimension, else
  "Requester too far away from Stock keeper". Screen: the 9 order entries with amounts 1..256
  (scroll, Shift ×10), partial / full, address.
- **Factory gauge**: up to 4 panels per block (placing a tuned gauge on a free quadrant adds a
  panel; breaking removes the targeted panel only while others remain). A panel watches one item
  (click with the item, or with an empty hand pick it from the inventory screen; filter items are
  refused) in its network — or, when the gauge sits on a packager (restocker), in that packager's
  inventory. Target amount 0..100 items or stacks on the value board (client mod: Create's click
  and hold with an empty hand, a short click opens the panel screen; vanilla: Sneak + empty-hand
  click; 0 = inactive). "Use Mechanical Crafting" (U42, Create's availableCraftingRecipe): when
  the panel's inputs are exactly the ingredients of a crafting-table recipe for its item, the
  screen / form offers the toggle; on, the inputs' amounts and the output follow the recipe and
  every request carries the recipe's 3x3 arrangement (`crafts`, Create's
  PackageOrderWithCrafts.singleRecipe), so the packager boxes one craft per package in the grid's
  slots for mechanical crafters. Connections: "Add new connection" then click another panel: same face and facing,
  same plane, closer than 16 blocks, at most 9 inputs, both with an item, the input not a
  restocker (Create's messages). Every tick a panel checks storage (satisfied / promised-satisfied /
  waiting for unloaded links); when not satisfied, not paused and its timer ran out
  (`factoryGaugeTimer`, 100 ticks; a new target waits half of it) it requests: a recipe panel
  orders every input's amount (1..64 each, summed per item and network; all must be in stock) to
  its address and promises its output (1..64); a restocker orders `clamp(demand - promised -
  inStorage, 0, 9 stacks)` (capped by the network stock outside its own inventory) to its address
  and promises it. Promises expire after never / 30 s / 1..31 min. The wrench on a panel with
  outputs cycles the arrow path mode. Redstone links (U23) pause panels through
  `stock::setPanelPaused`. A display link (U24) on a gauge offers Create's "Factory gauge status"
  source (`gauge_status`): one line per panel, `<level in storage> ✔|↑|▪ <item>` (shortened /
  full numbers); other units read panels with `stock::panelAt`.
- **Table cloth shops**: a table cloth (U10) programmed at a stock ticker becomes a shop when
  placed (owner = placer, facing = towards the placer). The owner sets the price with an item on
  the price tag (front of the top face; Sneak + empty hand clears it, empty hand opens the amount
  board 1..100, "Price per Order"; client mod: Create's click and hold, a short empty-hand click
  clears it, holding opens the board). A table cloth in hand on a table cloth (not sneaking)
  extends it: Create's placement helper puts the held cloth into the free horizontal neighbour
  nearest to the clicked point (U42). Others click the cloth with an empty hand or a shopping list:
  each click adds one purchase to the list (stock limited: "Out of Stock", "Stock limit reached";
  "Shop owner must set a price first", "Stock keeper missing"); the list's lore shows the items and
  the total cost. At the keeper the list checks out: wrong network, stock too low, cash register
  full and too broke are refused; otherwise the price moves from the inventory into the ticker's
  payments and the order is sent to the shop's address ("ka-ching"). Sneak-use discards a list.
  A broken shop drops its programmed cloth.
- **Client mod**: native screens (request, category editor, requester, gauge, gauge item picker);
  text is typed into native text fields: the request screen's search box filters as the player
  types (`{op: "search", text}`), its address box keeps the ticker's last used address
  (`{op: "address", text}`, 50 characters); category names are edited in place in the category
  editor (`{op: "rename", index, name}`); the requester's and the gauge's address boxes send
  `{op: "set", address}`. Vanilla players get the same values as forms. Looking at a
  gauge panel shows its label, "in storage / target" (Create's value box text) and "Click and hold
  for target amount"; looking at a ticker with payments shows them (owner). On the gauges (U42,
  Create's FactoryPanelRenderer) the client mod draws each panel's item (display items), its
  count in storage / target coloured as the label (5x7 pixel font) and the arrows from its inputs
  along the panel plane (bent as the wrench's arrow mode, coloured satisfied green / promised
  gold / missing red / grey without a target); the server sends `stock.gauge` when a gauge changes.
- **Log lines**: `stock: stock link placed at x y z on network …`, `stock: order of N item(s) in M
  stack(s) sent to 'A' through K packager(s)`, `stock: redstone requester at x y z sent its order to
  'A'`, `stock: factory gauge at x y z restocks <id> xN to 'A'`, `stock: factory gauge at x y z panel S
  requested its ingredients to 'A'`, `stock: … checked out a shopping list …`.

#### 3.6.27 Trains (U27)

Checked against Create 6.0.10 (`Train`, `Carriage`, `Navigation`, `ScheduleRuntime`, the schedule
instructions / conditions, `StationBlockEntity`, `ControlsHandler`, `ContraptionCollider`, `CTrains`).
Pure logic in `src/core/trains/` (gtest: physics, schedules, assembly rules, graph motion), game side
in `src/feature/trains/`, native screens in `src/client/trains/`. Other units integrate through
`src/feature/tracks/TrainsExt.h` (U27's addition to the track contract).

**Units.** Config speeds are blocks per second (acceleration blocks/s²); a train works per tick:
top speed `trainTopSpeed / 20` (28 → 1.4 b/t), turning `trainTurningTopSpeed / 20` (14 → 0.7), acceleration
`trainAcceleration / 400` (3 → 0.0075 b/t²). A train with fuel burning ("powered") uses the
`poweredTrain*` values (40 / 20 / 3).

**Stations.** Comparator output (U36): 15 while a train stands at the station. Placing: right-click a track with the station item selects it ("Targeted track
selected"; junctions refused: `create.track_target.no_junctions`); the direction the player looks
along the track is the station's direction (trains arrive facing it; the assembly area lies behind).
Sneak-use in the air clears the selection. As Create's TrackTargetingBlockItem the selection is
kept on the item stack (item data `lv:track_target`; placing clears it from the held stack), and a
spot within 0.75 blocks of another edge point is refused (`occupied`). The station block must be
placed within 16 blocks of the track (`too_far`); without a selection: `missing`; bound: `success`. A
station is an edge point (`levitate:station`, id `station:<dim>:<x>:<y>:<z>`) at the track block's
centre; it is re-added when the graph is rebuilt. Breaking the block removes it (trains heading there
re-route; its auto-schedule drops). Right-click: an auto-schedule held → it is given back; otherwise
the station screen (also with a schedule in hand, as Create). The *auto-schedule* (Create's depot
behaviour that only accepts schedules) is a one-slot inventory (U3 item capability: funnels,
hoppers, arms) that also takes a schedule thrown onto the station; the client mod draws it lying on
top (U33 display items). Every train that newly arrives at the station, and a train standing there
when one is put in, follows it from then on (replacing its own schedule; the conductor advancement,
a confirm sound and particles). Default name "Track Station" (max 25 chars); train names max 35;
the station screen sets the train's icon type (traditional / electric / modern, Create's
AssemblyScreen selector).

**Assembly** (station screen "Create new Train" on straight orthogonal track → assembly mode; the
station's flag rises, no train may pass or approach it; the client mod draws Create's flag on the
station, raised while a train stands there or in assembly mode, waving while it settles, sent on
`trains.flags`). The assembly track starts one block past
the station's track, against the station's direction, and runs while the same straight track
continues (up to `maxAssemblyLength` 128). Train casing on a track of the area creates a small bogey
on top (consumed unless creative; Create's isValidBogeyOffset: the first bogey only at index 0,
later ones at ≥ 3 and not within 2 of another bogey; the block above breaks unless unbreakable):
"Bogey created. Click again to cycle type"; a click where no bogey may go cycles a bogey within one
block of it small ↔ large (no casing needed); without casing (empty hand, survival):
`requires_casing`. "Assemble Train"
checks, with Create's messages: at least one bogey (`no_bogeys`), the first at the station marker
(`frontmost_bogey_at_station`), consecutive bogeys ≥ 3 apart (`bogeys_too_close`), at most
`maxBogeyCount` 20; every carriage starts at its first bogey and takes what the contraption rules
attach (super glue, chassis, attached blocks; the block on top of a bogey always joins, a bogey joins
the block above it): more than one block (`nothing_attached`), at most two bogeys (`too_many_bogeys`),
the second one the next on the track (`not_connected_in_order`), no tracks or stations
(`unmovableBlock`), Train Controls only along the track (`sideways_controls`) and at least one
forward-facing (`no_controls`, "forward" = Create's FACING equal to the assembly direction).
Each carriage becomes one contraption (kind `train`, pivot = bottom centre of its first bogey).
The train takes the name the station remembers from its last disassembled train ("Unnamed Train"
otherwise), is *double ended* when backward controls exist, and waits at the station. Errors stay on
the screen ("Assembly Failed", "Carriage N:", message, "Resolve this and retry").

**Trains on the graph.** A train is a list of points on directed edges (the front point at the
station marker, each bogey centre 1 block behind the previous index and its wheel points ±1); the
points follow the junction choices of the leading point (both directions), so carriages keep their
spacing exactly along the track. Each carriage's contraption is placed every tick: first bogey at its
point (+1 block up), yaw from the first to the last bogey (one-bogey carriages: from its wheel
points); on a slope the carriage is pitched from its first to its last bogey (the transform's tilt,
  Create's carriage pitch); the client mod turns the bogeys' wheels with the distance travelled
  (Create's BogeyRenderer). Entities standing on carriages are carried, entities collide with them (U4).
  Carried sliding / train doors open while the train stands at a station whose door control allows
  their side (and close as it leaves), as at an elevator floor (contact door control): Create's
  SlidingDoorMovementBehaviour (`src/feature/building/DoorMovement.cpp`).

**Speed** (per tick): navigation (Create's Navigation.tick): final approach snaps onto the target
when `target + 0.25 − |speed| < 1/32`; within 10 blocks the speed is pulled towards `top · d / 10`;
otherwise the target speed is top · throttle while the remaining distance exceeds the braking
distance `v² / 2a`, else 0; a curve ahead (within braking + 20) limits it to the turning speed once
`d > braking − turn² / 2a` no longer holds; speed approaches the target by `a` per tick. Unguided
trains coast to a stop at `a`; derailed trains lose 2/3 of their speed per tick.

**Schedules** (Create's ScheduleRuntime): entries run in order; instructions: *Travel to Station*
(name filter, `*` = any text, everything else literal, whole name; stations of the train's graph in
any dimension; the lowest A* cost wins: distance + penalties: passing a station 50, a station with
another train 300, the edges at both ends of every other train half its penalty (manual 200, idle 700,
waiting at a signal 50 + waiting ticks / 20, arriving 50, any other 25), an occupied signal
`clamp(2 · ticks waiting at a signal, 25, 200)` halved after each one met in the search, a
redstone-forced one 400; a signal boundary without a signal for the travel direction can't be passed
(Create's canNavigateVia: signals are one-way for routing); stations in assembly mode can't be
passed; far from the station the way is looked for again every 100 blocks, and a train waiting at a
cross signal now and then), *Update Schedule Title*, *Limit Max Speed* (5..100 % in 5 % steps),
*Deliver Package* (every station with a postbox for any package in cargo is a candidate; no packages:
the entry is skipped with its conditions; none accepted: `no_package_target`), *Retrieve Package* (a
station whose postbox holds an outgoing package addressed to the filter; none: the entry is skipped
after 40 ticks). At a station with postboxes (U25's `postboxesForStation`)
every 20 ticks packages addressed to a postbox leave the train and outgoing packages board it. Conditions (columns OR, conditions of a column AND, in order): Scheduled
Delay (`Value` 0..120, unit ticks / seconds / minutes, default 5 s), Time of Day (hour, minute,
rotation every day … every 0:15; met within 40 ticks after the time), Item / Fluid Cargo (more / less /
exactly, threshold, items or stacks, filter = item or bucket), Redstone Link (U23's link networks: any
transmitter on the frequency pair; the form takes the pair from the player's hands), Players Seated (exactly / or above, 0..20), Cargo Inactivity (no cargo change
for the duration, checked every 20 ticks), Chunk Unloaded, Station Powered. A failed instruction
waits 40 ticks; status messages go to the train's owner (`create.train.status.*`, each failure once
until it changes); non-cyclic schedules pause when done. New entries: travel + wait 5 s.

**Conductors.** A scheduled train needs a conductor facing its travel direction (Create's
checkConductors): a non-player passenger seated (U4 contraption seats) on the seat in the cell behind
a Train Controls block that faces it (`missing_driver` / `opposite_driver`), or a blaze burner there
(`levitate:blaze_burner`, U14). Only an entity seated on such a seat takes or gives schedules; a
schedule on another seated mob: `non_controlling_seat`. Right-click a conductor with a schedule: applied
("Train is now following this Schedule", item used); with a schedule already running:
`remove_with_empty_hand`; empty hand: resumes a paused schedule (`continued`) or takes the schedule
back (`removed_from_train`, auto-schedules are discarded); empty schedule: `no_stops`.

**Manual driving.** Right-click a train's controls (also through any block behind them, or in the
air holding an item) starts controlling ("Now controlling: <train>", the schedule pauses:
`paused_for_manual`); again, sneaking, leaving 8 blocks or the dimension stops ("Stopped controlling
contraption"). Movement keys from the player's input (vanilla clients: PlayerAuthInputPacket; the client mod:
Create's controls packets, below; held keys time out after 30 ticks): W / S throttle (target `top ·
manualTrainSpeedModifier 0.75 · throttle`, the turning speed on curves, a quarter backwards relative
to the controls, double-ended trains too (Create's `slow = inverted ^ targetSpeed < 0`); braking
against the motion at 2a), A / D pick the left / right branch at junctions, Space at a station shows
"Arrived at <station>"; elsewhere, while a free station lies between 0.75 and max(32, 1.5) braking
distances ahead (a double-ended train: in the direction driven), the action bar says "Hold [Space] to
approach <station>", Space approaches it with a 30-bar progress prompt (releasing Space cancels). Leaving a station:
"Departing from <station>". Fuel: any burnable item from the carriages' chests / barrels (from the
back carriage first when reversing; the item's crafting remainder, e.g. a lava bucket's bucket, goes
back) while driving.

**Client mod controls** (Create's ControlsHandler / TrainHUD, U37). Starting / stopping to control
sends the companion `trains.controls` {active, throttle, name}. While active, the client mod takes
W / S / A / D / Space / Shift from the player (they don't move the player, as Create sets the keys
up) and sends `trains.interact` {pressed: [forward, back, left, right, jump, sneak]} on every change
and every 5 ticks while one is held; Esc sends {stop: true}. The mouse wheel changes the throttle by
1/18 (1/18 .. 1, Create's editedThrottle) and sends `trains.throttle` {throttle} at most every
5 ticks (one log line per change on the server). The sprint key (Ctrl) is the horn: {honk: true}
every 5 ticks while held, {honk: false} when released; the server follows Create's HonkPacket
(`honkTicks` = 20 for a new honk, 13 while held, 6 or 0 on release) and, while it counts down,
plays the train horn (`levitate.trains.horn`, the low horn for a large whistle; our own synthesised
samples, `tools/sounds/trains.ps1`) at the train's first whistle with Create's pitch
2^(((small ? 12 : 0) − semitones) / 12), restarted every 18 ticks, and its end sound at 19 and 5
ticks left; trains without a whistle have no horn (Create's lowHonk null). Vanilla clients honk with
their sprint input (PlayerAuthInputPacket's sprint bits, the same messages every 5 ticks). An empty-hand right-click into the air sends `trains.interact` {} (vanilla
clients send nothing for it): it toggles the controls looked at, as the item-use path. Once a
companion's client mod sent key input, its PlayerAuthInputPacket is ignored for driving until it
stops controlling. Keys are only taken while no screen (ours, chat, inventory, pause) is over the
HUD, and a key's release is only taken when its press was. The server sends the controlling companion
`trains.hud` {speed, target, top, throttle, name, approach} every 5 ticks; the HUD draws a bar above
the hotbar filled to speed / top speed with a gold marker at the throttle and, beside it, "Throttle: n%" (or
"Approaching station..." while holding Space towards a station).

**Arrival / departure.** A train arrives when its leading point passes the station marker in the
station's direction (reversing double-ended trains: their back point, against it); speed 0, the
runtime's conditions start, the station's auto-schedule replaces the train's schedule. Create's
ArrivalSoundQueue: after a trip of more than 100 blocks, within 60 blocks of the station, the train's
bells, note blocks and whistles play from front to back (one block every 2 ticks). Sounds and
particles (Create's CarriageSounds / CarriageParticles, as world sounds so vanilla clients get them):
the steam rhythm of the leading carriage, the wheels faster and higher with the speed, a release hiss
when it stops; sparks at the bogeys while braking fast, steam while it depressurises. **Stalls**
(Create's Train.tick): an actor that stalls its carriage (a drill breaking a block) stops the whole
train; it goes on at its previous speed (at most 1 block per tick) when the stall ends. **Portals**
(Create's DimensionalCarriageEntity): a carriage whose first bogey passes a portal track moves into
the other dimension with its riders; the train counts as being where its front is. Disassembly ("Disassemble Train", station
screen) needs the train standing at the station with its front at the marker and every carriage
yaw a multiple of 90°: the carriages are aligned to the marker and placed back as blocks; the station
remembers the train's name; a schedule goes back to the player.

**Derailment and collisions.** Track removed under a train: it detaches (`track_missing`, derailed)
and tries to re-attach to the track at its last position every 40 ticks (`back_on_track`).
Wrench relocation (Create's TrainRelocator): right-clicking a standing train with the wrench selects
it ("Click a Track to Relocate {0} to. Sneak-Click to abort"; a moving one: "Cannot relocate a
moving Train"); while the wrench is held the looked-at track is checked (the train must fit there,
facing the look direction, within 24 blocks: valid / invalid / too far in the action bar, the client
mod draws the carriages at the target; refused where it would overlap another train or span a
portal); a click puts the train there stopped ("Relocation successful"), a sneak-click aborts. A dead end
stops it (`end_of_track`). The leading point of a moving train touching another train's segments
(within 0.5 block, height within 3): both trains bounce back (speed `−clamp(v, −0.5, 0.5)`) and derail
(`collision`, "In <dim> near [x,y,z]"); combined speed > 0.2 b/t makes an explosion of power
`min(3 · combined, 5)` that breaks no blocks. Entities in the way (not on top) of a carriage moving
faster than 0.35 b/t relative to them take `⌊16 · relative speed⌋` damage (×2 for monsters) and are
knocked away (motion clamped to 3), once per 10 ticks; `trainsCauseDamage` turns it off; creative /
spectator players are never hit.

**Signals (U28's SignalsExt.h).** Trains report the track they cover (`setTrainSpans`, ids = decimal
train ids) whenever they move or are placed, `removeTrain` when they are gone; while navigating they
ask `checkAhead` (look-ahead = braking distance + 4, along their path) every tick and brake to stop at
the returned distance, waiting in front of the signal (0.75 block before it); cancelled navigation and
arrival release their reservations; path costs add `pathPenalty`. Trains provide their cargo
(`setCargoProvider`: carriage inventories and tanks) and names (`setTrainNameProvider`) to observers.

**TrainsExt.h** (for U24 displays and others): `all()` trains (name, speed, graph, occupied edge
points, station, destination, schedule title), `stations()` (name, present train, predicted arrivals
in ticks), `onPointEntered` / `onPointLeft` (an edge point lies between a train's ends). Display
sources on the station (U24): "Train Station Summary" (display boards only, Create's
StationSummaryDisplaySource over submitPredictions: every upcoming destination of every running
schedule whose name or filter matches the filter glob, soonest first, with ETAs accumulated from the
learned transit times and the delays of the stops, a train standing there as "now"; options: filter
(default: the station's name), train column size 0..73 %, platform column 0..16 characters) and
"Train Schedule Status" (the waiting status of the current condition of the first column of the
train standing there: "Departs in …", "Cargo Idle for …", "Passengers: n/m", …; nothing on the last
stop of a non-looping schedule).

**Persistence.** `lv:trains:stations` (stations), `lv:trains:trains` (trains: points, route,
carriages with their contraption ids, speed, runtime, navigation); carriages persist with U4.

**Commands.** `/create trains` (train summary: networks, stations, every train's status line),
`/create killtrain <name|id>` and `/create train remove <name|id>` (Create 6.0.10's `/create train
remove`: the carriages vanish, nothing is placed), `/create train tp <name|id>` (Create's train
teleport: creative / spectator players only, 5 blocks above its first carriage, in its dimension).
The schedule item lists its destinations as lore (Create's tooltip: "> station", the stop it goes on
from "-> station"); the schedule editor's station field offers the station names nearest first
(Create's DestinationSuggestions).

**Screens.** Station (native for companions, forms for vanilla; the station name is the native
screen's title text field, 25 characters, the present train's name a text field, 35): idle (create new train / why not),
assembly (bogey count, how-to, errors, assemble / cancel), train present (name, disassemble or
"Cannot disassemble, not all carriages aligned", retrieve schedule / discard auto-schedule), door
control (Open All / North / East / South / West / None), arriving trains with Create's ETA texts
(now, `N min`, `Ns`). Schedule editor: entries with their summaries, edit / type / move / remove,
conditions with edit / remove, add condition (AND) / alternative condition (OR), add action, Loop
Forever, Reset Progress, Skip current Stop. Its instruction and condition editors are native
sub-screens (Create's editor of ScheduleScreen): a type selector, text fields for station names,
schedule titles (25 characters) and package addresses (50), scroll inputs for throttle, durations,
time of day, operators and player counts, a digits-only threshold field, and ghost item slots for
the reference item and the redstone link frequency, filled from the inventory. Confirming, or
closing the screen with an editor open, sends the part (`set_instruction` / `set_condition`); the
server checks it against the same limits. The vanilla forms edit everything field by field.

#### 3.6.28 Signals and observers (U28)

Checked against Create 6.0.10 (SignalBlock, SignalBlockEntity, SignalBoundary, SignalEdgeGroup,
SignalPropagator, Navigation's signal scan, TrackTargetingBlockItem, TrackTargetingBehaviour,
TrackObserver). Pure logic in `src/core/signals/` (gtest), game side in `src/feature/signals/`,
track markers in `src/client/signals/`, contract `src/feature/tracks/SignalsExt.h`.

**Placing** (Create's TrackTargetingBlockItem, same for both blocks):
- Right-click a track with the item: selects it with the direction along the track closest to
  the look ("Targeted track selected", click sound). Curves are selected by clicking their track
  bed (`levitate:fake_track`). Refused (deny sound, red): crossings
  (`create.track_target.no_junctions`), a spot within 0.75 blocks of another edge point
  (`occupied`; another signal within 0.25 blocks is joined instead: both form one boundary), no
  graph there (`invalid`).
- Right-click elsewhere: places the block when it is closer than 16 blocks to the selected track
  (else `too_far`); without a selection `missing`. Success: "Successfully bound to targeted track",
  the selection is used up. Sneak-right-click a block with a selection clears it
  (`create.track_target.clear`). The selection (track, direction, curve point) is stored on the
  item stack (item data `lv:track_target`) and travels with it; placing clears it from the held
  stack. While the held stack carries one, the client mod outlines the selected track (Create's
  TrackTargetingClient); the glint of Create's isFoil isn't drawn (§8).
- The block keeps its target. When the track graph around it is rebuilt or removed, it binds
  again; while its track is gone the signal is dark (`Invalid`) and the boundary is gone. The
  target is kept relative to the block in its BlockData (`signals`), so a contraption moving the
  block (and its track) binds it again at the same offset (turned with the block).
- A curve selection reaches `maxTrackPlacementLength` + 16 blocks.

**Signal boundaries and sections.** A signal block is bound to a boundary edge point (type
`levitate:signal`) on one of its two sides: side 1 controls trains travelling along the point's
edge (node1 → node2), side 0 the other way. Several signal blocks may share a side (one type per
side; a joining block takes the side's type). Boundaries split the graph into sections (Create's
signal edge groups): the track reachable from a boundary side without passing another boundary,
across every connection at nodes. Networks without boundaries have no sections and ignore
signals. Sections whose tracks cross without a node (a curve over another track, checked on the
xz plane with 0.5 blocks of height tolerance) are linked: a train in one occupies all of them
(Create's intersecting groups).

**Occupancy and reservations.** A section is occupied while any part of a train covers it
(trains report the stretch they cover). A train that is let past a signal *reserves* the section
until it enters it (or asks again / stops navigating). For a train T, a section is occupied when
another train is in it or in a linked section, or another train reserved it (Create's
`isOccupiedUnless(train)`).

**Signal types** (wrench right-click cycles the side's type: "-> Allow passage if section
unoccupied" / "-> Allow passage if section fully traversable"; unbound: "Unable to switch mode of
this Signal"):
- *Entry signal*: the train may enter when the section is free; it reserves it.
- *Cross signal*: the train may enter only when every section on its path up to and including
  the one behind the next entry signal is free (chain signalling: consecutive cross signals
  chain); all of them are reserved at once. Otherwise it waits at the first cross signal of the
  chain, never inside the junction. A path that ends inside the chain, or leaves it through a
  boundary without a signal for its direction, ends the chain.
- A boundary side without a signal block doesn't stop trains driving through it (it still splits
  sections), but scheduled trains never route through it (Create's canNavigateVia: signals are
  one-way for path finding).
- A redstone-powered signal is red for trains (forced red).

**Signal state** (Create's tickState, every tick): invalid when both sides of the boundary are one
section (a loop); red when forced by redstone or when the section is occupied by any train or
reserved through another signal; a free cross signal resolves its chain (Create's
resolveSignalChain over the signals leading out of its section on the way forward, no turns
sharper than Create's 7/8 rule): green when all of them are green (or invalid), red when all are
red, yellow when mixed; off when unbound. The block (Create's SignalBlockEntity.enterState) turns
red only 15 ticks after showing green or yellow; an invalid signal flashes its red lamp (3 of every
40 ticks); losing the redstone input applies 4 ticks later (SignalBlock.neighborChanged). The block shows it with its
lamp (`levitate:signal_state`, vanilla clients and the client mod alike). Comparators reading a
signal output 15 while it is red, 0 otherwise. Redstone input sets `levitate:powered` (the shared
`redstone::watch`; powered track observers are signal sources there).

**Track observers** (edge point type `levitate:observer`, never shared, can't be within 0.75 of
another point): powered while a train covers its position, and for 8 ticks after (Create's
`keepAlive`), with redstone 15 to all sides (`minecraft:redstone_producer` on the powered state;
the circuit component's strength is set with the state change). The filter slot on
top (U3 filter slot, "filter") limits it to trains carrying a matching item (in any carriage
inventory) or fluid (any tank), asked through the trains' cargo provider; an empty filter
matches every train. A display link on an observer offers "Detected Train Name" (Create's
`observed_train_name` source, U24 contract): the name of the train it sees (with the Attached Label
option, refreshed every 400 ticks), empty otherwise; a change of the observer's power refreshes the
links reading it at once.

**Client mod** (Create's track overlay): an arrow is drawn on the track at every bound signal side
(pointing the way of the trains it controls, in the signal's colour) and a square at every
observer (lit while powered), from `signals.overlay` messages (U3 protocol, companions only, sent
when the markers change and on join). Vanilla clients see the block lamps only.

**Contract** (`feature/tracks/SignalsExt.h`, namespace `lv::feature::tracks::signals`), for U27
trains and other units. Train ids are stable strings (Create: the train UUID).
- `setTrainSpans(id, spans)` / `setTrainExtent(id, trailing, leading)`: the track the train
  covers (spans: edge between two node locations, from/to blocks from the first node; extent:
  found along the graph from the trailing to the leading point, both travelling the same way).
  Call every tick the train moved, after assembly and after `tracks::onGraphChanged`. This
  replaces Create's front/back signal listeners: a train enters a section when its covered track
  first touches it and leaves it when it no longer does; observers see it while it covers them.
- `removeTrain(id)`: disassembled / destroyed.
- `checkAhead(id, front, pathEdges, lookahead)` → `Authority {clear, stopDistance, waitingFor,
  waitingSide, reserved}`: Create's Navigation scan from the leading point along the path
  (`PathResult::edges`); signals farther than `lookahead` are not checked yet (a started cross
  chain is always followed to its end). The train's previous reservations are replaced by what
  this call grants, so call it every tick while driving and stop within `stopDistance` when not
  clear. `releaseReservations(id)` when the train stops following the path.
- `signalsAhead(front, path, maxDistance)`: boundaries on the way (id, side entered, distance).
- `pathPenalty(id, graph, edge)` for `PathQuery::edgePenalty`: Create's `RED_SIGNAL` 25 for a
  signal into an occupied section, `REDSTONE_RED_SIGNAL` 400 for a redstone-powered one.
- Queries: `sectionAt(location)`, `trainsIn(section)`, `signalState(pos)`, `observerPowered(pos)`.
- Provided by trains: `setCargoProvider(fn(id, itemTest, fluidTest) -> bool)` (any carried item /
  fluid passes the test); without it filtered observers never trigger.
  `setTrainNameProvider(fn(id) -> name)` for the display source; `observedTrain(pos)`.

Persistence: the blocks (target, bound point, side, cached type and power) in `lv:signals:blocks`;
edge points are stored with the track graph (U5); observer filters in BlockData; sections,
occupancy and reservations are rebuilt (trains report again after a load).

#### 3.6.29 Schematics (U29)

Checked against Create 6.0.10 (SchematicAndQuillHandler, SchematicExport, ServerSchematicLoader,
ClientSchematicLoader, SchematicTableBlockEntity, SchematicItem, SchematicHandler and its tools,
SchematicannonBlockEntity, SchematicPrinter, LaunchedItem, MaterialChecklist, BlueprintEntity,
BlueprintItem, BlueprintMenu, CSchematics). Code: `src/core/schematics/` (NBT both byte orders,
DEFLATE/GZIP, `.mcstructure` reader/writer, Create `.nbt` reader, Java→Bedrock state mapping,
placement transforms, print order, replace modes, checklist, blueprint layout; gtest),
`src/feature/schematics/`, `src/client/schematics/`.

- **Files.** The server keeps each player's schematics in `<mod>/schematics/<player>/` (Create's
  `schematics/uploaded/<player>/`); the client mod's local folder is `<mod>/schematics/` (Create's
  `schematics/`). Files are `.mcstructure` (what the quill saves, Bedrock block states, Levitate
  BlockData kept per block, the block entities' NBT and the entities in the area except players, as
  Create's `fillFromWorld` with entities) or Create/Java `.nbt` (GZIP or raw NBT, block states mapped to Bedrock:
  vanilla ids with Bedrock's renames and flattened families, state names converted; `create:<path>`
  → `levitate:<path>` with `levitate:axis` / `levitate:facing` / `levitate:<bool>`; states a block
  doesn't have are dropped, alternatives tried). Block types without a Bedrock block are skipped and
  listed to the player once per file (`lv.schematics.unmapped`).
- **Schematic and quill.** Right-click a block: first, then second corner (clicking the top or
  bottom face of a non-replaceable block selects the block beyond it); with Ctrl (vanilla:
  sprinting) the corner is the block `range` (10, Ctrl-scroll 1..100) blocks along the view. With
  both corners: Ctrl-scroll pushes/pulls the face looked at (inverted from inside, never below one
  block; size shown in the action bar), right-click opens the name prompt (title "Schematic Name:",
  a native text field taking any text but file name separators `\ / : * ? " < > |`, Enter saves;
  Save / "Save and Upload Immediately" / abort; empty name = "My Schematic"). Sneak-right-click
  discards ("Removed selection."). Saving writes `<name>.mcstructure` (a free `<name>_2...` unless
  converting), clears the selection, sends the file to the client mod's local folder; the oldest
  file of the player's server folder is deleted when `maxSchematics` is reached; converting replaces
  the quill with a written schematic (`maxTotalSchematicSize` checked). Areas above 512 blocks per
  axis or 8M blocks are refused ("Failed to save schematic"). Client mod: Create's blue (0x6886c5) box outline following the
  cursor, the face being resized highlighted; vanilla: particle edges.
- **Schematic table.** Input slot (empty schematic), output slot (written schematic). Client mod:
  native screen listing the local folder (selection input, refresh, upload); an upload sends
  `schematics.upload.begin {pos, name, size}` (refused above `maxTotalSchematicSize` KB with
  Create's two messages; `.nbt` must be GZIP), then one chunk (≤ `maxSchematicPacketSize` bytes,
  base64) per tick and `schematics.upload.end`; the empty schematic is used up at the start, the
  progress bar follows the server, an upload idle for `schematicIdleTimeout` ticks is cancelled, a
  finished file replaces a same-named one (oldest deleted at `maxSchematics`) and becomes the written
  schematic in the output. Vanilla: a form lists the player's files on the server and writes one.
- **Written schematic.** Item data `lv:schematic` {file, owner, deployed, anchor, rotation 0..3
  (clockwise), mirror (none, left-right = z flip, front-back = x flip), size}; lore shows the file
  (gold) or Create's invalid-item text. Placement = Create's StructurePlaceSettings: mirror, then
  rotation about the anchor. Client mod: the held schematic's ghost (every block with its own model,
  tessellated as Ponder draws blocks: Levitate geometry from the pack, other blocks through the
  game's block tessellator; blocks hidden on all six sides are left out) and bounds,
  Alt+scroll picks a tool (Position, Move XZ, Move Y, Rotate, Mirror, Print in creative; only
  Position until deployed), Ctrl+scroll uses it (distance / push along the look axis / up-down /
  90° turns and mirroring about the footprint's centre), right-click with Position deploys with the
  target block as the bottom centre (Ctrl: at the fixed distance), Print places it instantly
  (creative, `creativePrintIncludesAir`) with its block entity NBT and its entities (positions and
  facings turned and mirrored like the blocks, new entity ids). Sneak-right-click: coordinates screen (x, y, z as native number fields
  taking digits and `-`, Enter confirms; rotation, mirror). Vanilla: right-click a block deploys there, sneak-right-click opens the same values as a
  form (with Print for creative players), particle outline of the bounds.
- **Schematicannon.** Slots: schematic, used schematics out, book (book / book and quill /
  clipboard), checklist out, gunpowder (hoppers, funnels and arms insert into the matching slot and
  extract outputs). States STOPPED / PAUSED / RUNNING with Create's status keys
  (`create.schematicannon.status.*`). A schematic loads as soon as it is inserted ("Ready", paused):
  not anchored → "Schematic not Positioned", no file / empty → "Schematic File Expired" and broken
  file → "Incompatible Blocks" (the schematic comes back empty), anchor ≥ 256 blocks away →
  "Target too far away". Play prints layer by layer (y, then z, then x), brittle blocks (torches,
  rails, levers, buttons, doors, signs, carpets, ...) deferred to the end; one shot every
  `schematicannonDelay` (10) ticks, each using one shot of fuel (one gunpowder = 400 shots, from
  its slot or the attached inventories, "Out of Gunpowder" pauses). Targets within 2 blocks of the
  cannon, unbreakable blocks, blocks already right and (Protect Block Entities, on by default)
  data-holding blocks are skipped; the replace mode decides the rest (Don't Replace Solid /
  Solid with Solid / Solid with Any (default) / Solid with Empty). The item of each block comes
  from the six neighbouring inventories (any item capability; a creative crate makes everything
  free); missing items pause with "Missing Item:" or, with Skip missing Blocks (default on), skip
  the block. A launched block flies `max(10, 4·dist^½)` ticks and is placed with its second half
  (doors, beds, tall plants); of its block entity data only a sign's text is printed (Create's
  NBTProcessors in survival). After the blocks the printer launches the schematic's entities that
  have an item requirement (Create's ItemRequirement.of(entity): armor stands with what they wear
  and hold; item frames are blocks on Bedrock), one per shot. Sounds as Create (explode 0.1/1.1 per
  shot, note bell 1/0.7 at the end). The checklist printer turns a book into a written "Material
  Checklist" (5 entries per page, missing ones first, `xN | stacks▤ +rest`) and a clipboard into a
  read-only "Material Checklist" clipboard (7 entries per page, each with its item icon and the
  missing amount, completed ones checked off; Create's createWrittenClipboard) in 20 ticks. Client
  mod: launched blocks drawn along their arc; the connector turns and the barrel pitches towards the
  printer's current target (`schematics.cannon`; Create's angles: yaw = atan2(dx, dz), pitch =
  atan2(distance, dy·3 + distance·2) + 10° from vertical, eased over a tick; resting: south, 40°)
  and recoils up to 0.1 block in the 10 ticks after a shot (the block's `levitate:barrel` = 1
  permutation hides both bones for the client mod's chunk mesh); everyone: smoke at the barrel,
  particles at the target.
- **Crafting blueprint.** Placed against a face (walls, floor, ceiling) as the largest of 3x3, 2x2,
  1x1 that fits (free blocks with a solid block behind each). Each section: 9 recipe slots (filters
  allowed), the display item (inferred from the crafting recipe, or set by hand) and a secondary
  display item. Right-click a configured section crafts once from the inventory (sneaking: until
  64 items or the ingredients run out); an empty section or the wrench opens the recipe screen
  (native ghost slots / form); hitting a configured section clears it; breaking any section
  removes the blueprint and drops one item (none in creative). Client mod (Create's
  BlueprintRenderer): each section shows its display item (half a block) in the middle of the
  canvas and the secondary one (a quarter) at its lower right, facing out (U33 display items).

#### 3.6.30 Ponder (U30)

Checked against Create 6.0.10 (`AllCreatePonderScenes`, `AllCreatePonderTags`) and the Ponder
library it ships with (PonderUI, PonderTagScreen, PonderTagIndexScreen, PonderTooltipHandler).
Client mod only (§8); the server side is `/create ponder`.

- **Entries and scenes**: `assets/ponder/index.json` lists every component with Ponder scenes in
  Create's registration order (156 entries: Create's components with our ids, e.g. the 16 dyed
  valve handles, the 11 postboxes, the 19 table cloths) and each component's scenes in Create's
  order; the 167 scenes are Create's scene ids (`create.ponder.<id>.header` / `.text_<n>`, e.g.
  `shaft`, `cog_speedup`, `funnel_intro`, `train_assembly`, `factory_gauge_links`). Each scene is
  our own script (`assets/ponder/scenes/<id>.json`, format in `src/core/ponder/Scene.h`) teaching
  the same steps as Create's lesson; titles keep Create's wording, the bubble texts are our own
  (en_US + zh_CN). Blocks use the SPEC ids; an id that is not registered (yet) is drawn as a
  placeholder outline and the scene plays on.
- **Tags** (categories, Create's order and membership, with Create's titles / descriptions):
  kinetic_relays, kinetic_sources, kinetic_appliances, fluids, logistics, high_logistics,
  redstone, decoration, creative, movement_anchor, contraption_actor, contraption_assembly
  (all on the index), windmill_sails, arm_targets, train_related (index), display_sources,
  display_targets, threshold_switch_targets.
- **Opening**: hovering an item with scenes in any container screen (inventory, creative
  inventory, chests) adds "Hold [W] to Ponder" (dark gray, the key gray) below the item's name in its
  tooltip; while the key is held Create's progress bar of `|` characters as wide as the hint (done
  part gray) is drawn in a tooltip box at the top of the screen (Bedrock builds an item's hover text
  once per hover, so the tooltip line itself can't change while it is shown). Holding the Ponder key (W, Create's default; rebound with the "[W]"
  button of the welcome screen: click, press a key, Esc cancels; saved as `ponderKey` in
  `config/ponder.json`) fills it with Create's curve (`p = min(1, p + max(0.25, p) · 0.25)` per tick, about 11
  ticks; released: −0.05 per tick) and opens the item's first scene. `/create ponder` opens the
  index, `/create ponder <id>` an entry (`shaft`, `levitate:shaft`) or a tag (`kinetic_relays`).
- **Scene player** (PonderUI): "Pondering about..." with the scene title and the subject item
  (tooltip "Subject of this scene"); the scene drawn in 3D with the game's block renderer from
  the south-west, 30° from above (drag with the left button to turn it); scene dots; the timeline
  with key frames (click: jump, snapping to key frames); text bubbles pointing at their target
  (Create's palette colours); outlines, hand / mouse-button / item / Sneak + / Ctrl + indicators,
  redstone sparks, item entities, entities (minecarts, mobs) and particles (U46: drawn by the game's
  own actor renderer from puppet actors the client level's actor factory builds but never adds to
  the level; particles as billboards of our own sprites: flame, soul flame, smoke, large smoke,
  cloud, splash, bubble, snowflake, heart, crit), sections that slide and turn (contraptions), kinetic blocks
  turning at their RPM (0.3° per tick per RPM, rotor parts as the kinetics renderer); blocks fade in
  and out translucent (blend layer, alpha in 16 steps) while sliding from a direction; water and lava
  with their levels (`liquid_depth`: 8/9 of a block for a source, one ninth less per level, falling
  fluid full) and Minecraft's corner averaging, the flowing texture on sloped surfaces. Buttons and keys as Create: previous / next scene (A / D, mouse
  wheel), Close (Esc), Identify (Q: pauses, the hovered block's name; "Identify mode active.
  Unpause with [Q]"), Comfy Reading (half speed while a text is shown), Replay (S), Think Back
  (back to the previous Ponder screen). When a scene ends the next button lights up with
  "Up Next: <title>". The subject's tags are listed on the left (click: the tag screen).
- **Index** ("Welcome to Ponder", "Available Categories for Create"): the index tags; a tag opens
  its screen (title, description, "Associated Entries"; entries without scenes are greyed out);
  "Ponder Index" lists every entry.
- **Command**: `/create ponder [id]` (anyone) resolves the id on the server (index + tags) and
  sends `ponder.open {id}` to a companion; the console gets "Only players can ponder.", vanilla
  clients "Ponder needs the Levitate client mod.", unknown ids "There is nothing to ponder about
  <id>.".
- **Log lines** (e2e): `ponder: loaded N scenes for M entries`, `ponder: hovering <id>`,
  `ponder: held <key> over <id>`, `ponder: Ponder key set to <key>`, `ponder: opened <id> (N scenes)`, `ponder: scene <id> (i/n) of
  <component>`, `ponder: scene <id> drawn (N blocks, M placeholders)`, `ponder: scene <id> shows N
  entities, M item entities, K particles`, `ponder: identify mode
  on|off`, `ponder: replay <id>`, `ponder: closed <id>`, `ponder: tag <id> opened`,
  `ponder: index of N categories opened`.

#### 3.6.31 Equipment (U31)

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

**Toolboxes.** Comparator output (U36): calcRedstoneFromInventory over the 32 slots; the lectern
controller gives 15. 8 compartments × 4 stacks. A compartment locks to the first item put in (its
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
- *Lid and drawers*: while any player has the toolbox screen open (native or form; closed by Esc,
  the form's close, leaving, or moving more than 8 blocks away) the block state
  `levitate:toolbox_lid` is 1: vanilla clients show the open model (lid 135° back, drawers out). The
  client mod meshes the body only (state 2) and animates lid and drawers like Create's
  ToolboxRenderer (lid linear 0.2 per tick to 135°, drawers exponential 0.2, lower 0.35 / upper
  0.175 blocks). Sounds: iron door + chest open on the first opener, chest close when the last
  closes, iron door close 5 ticks later.
- *Dyeing in the crafting grid* (one toolbox + one dye, Create's ToolboxDyeingRecipe): the crafted
  toolbox keeps the contents and name (the craft results of the item stack request get the
  toolbox's user data).

**Backtanks.** Air 0..`airInBacktank` (900) + `enchantedBacktankCapacity` (300) × Capacity level in
the item (bar = air). Placed as a block (facing the player, keeping its air and Capacity level), it is a kinetic block with a shaft end at the bottom (stress impact 4):
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
  netherite (diving) boots: fire immunity (fire resistance, refreshed). Each netherite diving piece
  adds 3 armour toughness and 0.1 knockback resistance (Create: netherite armour material).
- *Fog* (Create's ClientEvents#getFogDensity): a worn diving helmet pushes the fog layer
  `levitate:diving_helmet` (water fog 6.25 times as far: 0..375 blocks), the netherite one
  `levitate:netherite_diving_helmet` (also lava fog 0..20 blocks); fog layers live in
  `assets/pack/rp/fogs/` and reach vanilla clients through PlayerFogPacket.
- *Diving boots*: in water (not swimming, not flying): sinking (vy − 0.05 per tick, a jump on the
  ground +0.5) and walking speed ×1.3 below 0.145 blocks per tick. The client mod applies this to its
  own player every client tick; vanilla players get the vertical motion as packets and +87.5 %
  underwater movement speed (the speed Create's rule settles at, §8).
- *Capacity* (backtanks) and *Potato Recovery* (potato cannon), levels I–III, weight 10, cost
  15 + 15·(L−1) .. 45 + 15·(L−1), are kept in the item's user data (`lv:enchants`) and shown as lore
  lines (`enchantment.create.*`). The enchanting table rolls them per option with Java's algorithm
  (the player's enchantment seed + option; books too); anvils merge them (equal +1, else the higher,
  +1 cost per level); grindstones remove them (§8).

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
powered wither is immune. Each projectile is a `levitate:potato_projectile` entity drawn from the
resource pack (also by vanilla clients) with the ammo's texture and Create's render mode (tumbling
billboard; along the motion with spin 1 for carrot, fish, pufferfish and suspicious stew, spin 2 for
golden carrot and beetroot; melon and pumpkin as small blocks); it is not saved with the world.
Sticky types (pumpkin pie, cake, blaze cake, fish, pufferfish) stick to the living entity they hit,
slide down (fall speed +0.007 per tick) and pop below its feet. Projectiles of different types pop
each other (once one is at least 10 ticks old), awarding `potato_cannon_collide` to both owners; a
kill of a monster, or a hit on another player, awards `potato_cannon`. Golden apples cure a zombie
villager with weakness, else give the golden apple effects. A hit that hurts nothing drops nothing;
otherwise, unless the hit used the item up, the item comes back with the recovery chance (0, or
12.5 % + 12.5 % per Potato Recovery level; first projectile of a split only), else the type's drop
(a bowl for suspicious stew). Crops plant only from the top face; melons and pumpkins that hit a
side fall as a falling block. Sounds: `levitate.potato_cannon.fwoomp` (original), the hit = item
frame break + roots break. Glow berries / glistering melon give no glowing (§8).

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
checkerboard, 25 / 50 / 75 % roll. Preview (client mod, Create's WorldshaperRenderHandler): while a
configured worldshaper is held (main hand first), the blocks its brush would change at the
looked-at block (128 blocks) are outlined as one cluster (0xbfbfbf, 1/32 wide; rebuilt when the aim
or the settings change).

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

**Tree fertilizer.** On a sapling, azalea or mangrove propagule (not a hanging propagule, which
does nothing and uses no item) the tree grows at once, also without room (Create's
TreesDreamWorld). The sapling's own Bedrock tree feature (its single or 2×2 feature; the mangrove:
the tall mangrove with a chance of 0.85, else the mangrove; azaleas: the azalea tree) is placed
into a world that is air except the soil below (dirt-like soil counts as dirt); podzol is ignored.
The result is copied: a solid block replaces anything but unbreakable blocks, any other block only
replaces blocks without collision, and replaced blocks drop as broken (the grown saplings drop
nothing). A single dark oak / pale oak sapling grows nothing. One item is used (creative: none). If
a feature can't be resolved, Bedrock's growth repeats until it grows (needs room).

**Peculiar and haunted bells** (U49, Create's AbstractBellBlock(Entity), PeculiarBellBlock,
HauntedBellBlock(Entity), HauntedBellPulser, SoulPulseEffect(Handler), BellMovementBehaviour,
HauntedBellMovementBehaviour; `Bells.cpp`, `core/equipment/Bells.h`, gtest). Vanilla bell rules:
placed on a floor / under a ceiling (facing the player's direction) or against a wall (facing into
it; between two sturdy walls a double wall bell, else standing / hanging when the wall can't carry
it); it breaks (dropping itself) when its support goes; a double wall bell losing one wall hangs on
the other, a single wall bell gaining the opposite wall spans both. A right-click (not sneaking with
an item) on a side of the bell below 0.8124 of its height rings it — floor / ceiling bells from the
two sides along their facing, wall bells from the two sides across it; otherwise the click goes on
(placing the held block). A rising redstone signal rings it along its facing (wall bells: facing
turned clockwise). Ringing lasts 74 ticks; the client mod draws the bell swinging
(`equipment.bell.ring`, BellRenderer: `1.2 sin(t / 1.5 / π) / (2.5 + t / 4.5)` rad about the axis
across the hit; the chunk mesh has the frame only), vanilla clients see the bell still (§8).
Sounds: the peculiar bell = the vanilla bell at volume 2, pitch 0.94; the haunted bell its own toll
(`levitate.haunted_bell.use`, original, volume 4). A peculiar bell above soul fire or a soul campfire
(placed there, or when the block below changes) becomes a haunted bell with the same states, with
15..24 soul fire flames and `levitate.haunted_bell.convert` (original). A haunted bell placed (also
by conversion) stirs for 20 ticks: now and then a soul particle rises with a soul-escape sound.
- *Haunted bell*: ringing it (not again while it rang less than 65 ticks ago) sends a soul pulse of
  10 blocks and awards `haunted_bell` to the player. Holding it in either hand (not spectating)
  sends pulses of 3 blocks around the player's feet after a 10-tick warmup (first pulse at once,
  then on every game tick not divisible by 8; the warmup resets after 5 ticks without it). On a
  contraption (not a train) a haunted bell pulses 3 blocks wherever it enters a new block, then
  waits 65 ticks, ringing each time.
- *Soul pulses* (run on the server, seen by every client): one layer of offsets per 6 ticks (layer
  n: rounded distance n, up to 11), skipping empty layers at once, then 100 ticks of keeping its
  marks. A spot is a candidate where a zombie could spawn on the ground (sturdy top below that is
  no glass / leaves / ice / barrier / bedrock / magma and gives less than 14 light; the spot and the
  block above without full collision, signal, liquid, rail or harmful block; a 0.2 × 0.75 dummy at
  its centre touches no collision box); pulses that don't overlap never mark a spot another running
  pulse marked. A marked spot with block light 0 gets the soul particle (`levitate:soul`, 24
  frames: forming, flickering, rising away, ~6 s) and the flat base ring (`levitate:soul_base`,
  4.5–7 s). A pulse of the same origin and size as one still expanding is not started again (it
  would mark nothing new: a held bell asks nearly every tick).
- *Contraptions*: brittle, hanging on their attachment side; the bell sounds when the contraption
  starts, turns around and stops (elevators: when they arrive); not on trains.

**Cardboard armour** (U49, Create's CardboardArmorItem / CardboardArmorHandler(Client) /
CardboardArmorStealthOverlay; `Cardboard.cpp`). A player in all four pieces who sneaks and isn't
flying hides as a box: the server makes the player invisible (no body, armour, held item or name on
any client) and keeps a `levitate:cardboard_disguise` entity at the player's feet, turned with the
player — one of the four plain package boxes (U25's models; random each time hiding starts), bobbing
while the player walks, not drawn for a camera closer than 1.8 blocks (Create: not in first person);
it takes no damage, has no collision and is removed when the player stops hiding, leaves or changes
dimension (orphans are swept every 5 s). Vanilla clients see the same. Mobs can't choose a hiding
player as their target (Create: visibility 0), except one it just hurt (vanilla's hurt-by
targeting ignores visibility); every 16 ticks of a mob a hiding target or last attacker is
forgotten. Hiding awards `cardboard_armor`. The client mod fades a cardboard vignette over the HUD
while its player hides (opacity chasing 1 / 0 by a quarter per tick). Taking a trimmed cardboard
armour piece out of the smithing table awards `cardboard_armor_trim` (Create's SmithingMenuMixin;
the advancement is secret; Bedrock's smithing craft request carries the result with its `Trim`
item data). There is no cardboard trim template item in Create (trims use the vanilla templates);
the pieces carry `minecraft:trimmable_armors` and the vanilla armour render controller draws the
trim on the attachables (checked in the client e2e). The item description (`item.create.cardboard_armor.tooltip`) is shared by the
four pieces.

**Cardboard sword** (U49, Create's CardboardSwordItem / KnockbackPacket). Hitting a living entity
that is not an arthropod does no damage: the "Resonant bonk" (`levitate.cardboard_sword.bonk`,
original, .75 / 1.85) and a knockback of `(2 + Knockback + 1 when sprinting) / 2` along the
attacker's look (minus knockback resistance; upward `min(.4, vy / 2 + strength)` on the ground;
the target dismounts); creatures (not monsters, water mobs, ambient mobs or players) get slowness
X for 3 s (hidden); the attacker's horizontal motion × 0.6 and sprinting stops. Arthropods take a
normal hit. A left-click on a block plays the bonk (.5 / 1.85). Only Knockback enchants it (table,
anvil and books).

**Messages / logs**: one log line per user-visible action (`equipment: ...`), e2e scenarios wait
for them.

#### 3.6.45 Advancements (U45)

Checked against Create 6.0.10 (AllAdvancements, CreateAdvancement, AdvancementBehaviour,
SimpleCreateTrigger) and vanilla's advancement toast, chat line and AdvancementVisibilityEvaluator.
Code: `src/core/advancements/` (the 102 entries in Create's order with parent, icon, task type and
inventory criteria; visibility; tree layout; gtest), `src/feature/advancements/` (contract
`Advancements.h`), `src/client/advancements/`.

- **Entries and types**: Create's ids (`lava_wheel_00000`, `track_0`, ...), titles and
  descriptions as `advancement.create.<id>` / `.desc` (Create's en_US and zh_CN text; secret ones end
  with "§7(Hidden Advancement)"). Types: silent (no toast), normal (toast), noisy (toast + chat),
  expert (goal frame, toast + chat), secret (goal frame, toast + chat, hidden until done). The root
  is awarded for free (silent).
- **Progress**: per player (uuid) in the world DB, `lv:advancements:player:<uuid>` `{done: [ids]}`.
  On unlock: client mod: Create's toast at the top right (5 s, "Advancement Made!" / "Goal
  Reached!", icon, title); vanilla: the game's toast (ToastRequestPacket); announce types: the chat
  line "<player> has made the advancement [<title>]" / "has reached the goal [...]" to every player
  in their language; log `advancements: <player> made <id>`.
- **Screen**: `/create advancements`: client mod: the tree (columns by depth, a parent centred on
  its children, Create's task / goal frames gold when done, lines to the parent, wheel / Shift-wheel
  scrolling, title and description tooltip); vanilla: a form listing the visible entries (done ones
  green) with a detail form. Visibility is vanilla's: done, or a done descendant, or the nearest of
  itself / parent / grandparent that is done or hidden is done. `/create advancements grant|revoke
  <id>` (op) changes the caller's progress.
- **Owners** (Create's AdvancementBehaviour): the player who placed a block with block advancements
  (Create's registerAwardables list per block entity) is remembered (`lv:advancements:owners`) until
  they have all of them; placements are seen through the interaction dispatch two ticks after the
  click (also units' own placements); units placing blocks otherwise call `setOwner`. Units award
  with `award(player, id)`, `awardOwner(pos, id)` and `awardOwnerIfNear(pos, id, distance)`.
- **Triggers this unit fires** (observing the world and the contracts): root; inventory criteria
  every second (andesite_alloy, burner, wrench_goggles (wrench + goggles), cardboard, copper, brass,
  rose_quartz, precision_mechanism, sturdy_sheet, track_0, chocolate_bucket); extendo_grip /
  extendo_grip_dual (held); item application turning a block into an andesite / copper / brass /
  railway casing (andesite_casing, copper_casing, brass_casing, train_casing_00); belt (a belt
  connector making a belt); glass_pipe (a wrench turning a pipe with fluid into a glass pipe);
  track_signal (a signal placed); shifting_gears (a small and a large cogwheel meshing diagonally
  with the same axis); hand_crank_000 (a crank used with an empty hunger bar); owner + kinetic speed:
  encased_fan, mechanical_pump_0 (speed magnitude changed, not 0), mechanical_crafter /
  crafter_lazy_000 (< 5 RPM), speed_controller (a large cogwheel on top), water_wheel /
  lava_wheel_00000 (the wheel generates; lava next to it), crushing_wheel / crusher_maxed_0000 (next
  to a crushing wheel controller; > 255 RPM), deployer (turning), backtank (turning); owner, polled
  every second: steam_whistle (powered), stressometer / stressometer_maxed (the owner looks at it
  wearing goggles; stress = capacity), windmill / windmill_maxed (assembled; ≥ 128 sails),
  clockwork_bearing (assembled), steam_engine (its powered shaft turns); contraption_actors (a new
  contraption with drills, saws, ploughs, rollers or harvesters, to its anchor's owner);
  super_glue (a new glue region, to the nearest player holding super glue); equipment: diving_suit,
  diving_suit_lava, potato_cannon, potato_cannon_collide.
- **Triggers fired in their unit** (U49; owner = the block's placer through `awardOwner`, the
  owner table also has packager / repackager, package frogport and factory gauge): processing:
  mechanical_press (each pressed result, `basin/Processing.cpp` via `onItemPressed`) and
  track_crafting_factory (1000 tracks pressed by one press, counter in its BlockData), compacting /
  mechanical_mixer (a basin recipe applied), fan_processing (an item / stack processed by the air
  current, `bulk/Fan.cpp`), saw_processing (`crafter/Machines.cpp` applyRecipe), millstone
  (`bulk/Millstone.cpp` process), spout (an item filled, `fluids/Spout.cpp`) and foods (the same spout
  made chocolate glazed berries, a honeyed apple and a sweet roll while its owner is known), drain
  (an item emptied) / chained_drain (an item passed into another drain, `fluids/ItemDrain.cpp`),
  hose_pulley (drained or filled a block, also from an infinite body or voided) / hose_pulley_lava
  (infinite lava, `fluids/HosePulley.cpp`), water_supply (an open pipe drains a water source) /
  honey_drain (honey from a beehive) / cross_streams (two fluids meet in a pipe, `pipes/OpenEnds.cpp`),
  steam_engine_maxed (a sampled boiler at level 18) / pipe_organ (12 different whistle pitches + sizes
  on one boiler, `steam/SteamBoilers.cpp`), fist_bump (two deployers punch, `assembly/Deployer.cpp`),
  mechanical_arm (every deposit) / musical_arm (a disc fully inserted into a jukebox) /
  arm_many_targets (≥ 10 outputs) / arm_blaze_burner (a blaze burner point, `assembly/Arm.cpp`);
  logistics: funnel (a funnel transfer, `funnels/Funnel.cpp`), belt_funnel_kiss (the wrench extends a
  belt funnel facing another extended one, to the player), chute (an item set into a chute,
  `funnels/Chute.cpp`), package_chute_throw (a thrown package enters a chute, to the thrower,
  `packages/PackageEntity.cpp`), ejector_maxed (a player launched ≥ 25 blocks, `belts/Depots.cpp`),
  packager (a package made and held, `packages/Packager.cpp`), frogport (a frogport catches a package,
  `packages/Ports.cpp`), stock_ticker (a non-empty order, to the player, `stock/Ticker.cpp`),
  table_cloth_shop (a table cloth placed from a programmed item, `stock/Shop.cpp`), factory_gauge (a
  gauge with connected inputs requests them, `stock/Gauge.cpp`), display_link (a link sent data,
  `display/Link.cpp` update), display_board_0 (the station summary lists a train on a board,
  `trains/Display.cpp`), linked_controller (a pressed controller button whose frequency a link listens
  to in the dimension: `RedstoneLink.h` `hasListeningLinks`, `equipment/Controller.cpp`);
  contraptions: portable_storage_interface (a transfer, `actors/Psi.cpp`), cart_pickup (a minecart
  contraption of more than 200 blocks picked up, `bearings/Carts.cpp`), anvil_plough (a plough throws a
  falling anvil at ≥ 0.25 blocks per tick: every player within 32 blocks, `actors/Plough.cpp`),
  pulley_maxed (a rope pulley reaches 200 blocks, `pistons/Actuators.cpp`), self_deploying (a deployer
  on a minecart contraption or train places a rail / track: the owner carried in the deployer's
  BlockData, `assembly/Deployer.cpp`); trains (`trains/`): train / long_train (assembly at a station,
  ≥ 6 carriages, station owner), conductor (a station's auto-schedule applied to an arriving train:
  station owner; a schedule handed to a train: the player), train_whistle (a controlling player honks
  a train with a whistle), train_roadkill (a train kills an entity: its driver), red_signal (a train
  without a running schedule enters a signal section another train occupies: its driver),
  train_crash (a collision: every seated player but the driver) / train_crash_backwards (the player
  who drove it backwards that tick), long_travel (a player leaves a train seat ≥ 5000 blocks from
  where they sat down).
- **Not fired**: train_portal (Levitate's trains don't cross dimensions through portal tracks yet).
- **Fired with their content (U49)**: cuckoo_clock (the creeper animation of a turning cuckoo
  clock starts: its owner within 32 blocks, transmission), haunted_bell (a player rings a haunted
  bell, equipment), cardboard_armor (a sneaking player in full cardboard armour starts hiding,
  equipment), cardboard_armor_trim (a player takes trimmed cardboard armour out of a smithing table,
  equipment; Create's SmithingMenuMixin).

#### 3.6.35 Consistency (U35)

Rules the build and the review keep for every unit (U35):

- **Integer block states run 0..n.** Bedrock reads a list-valued integer state by its position in the
  list, so every integer state list in `assets/pack/bp/blocks/` must be `[0, 1, ..., n]`;
  `tools/xmake/assemble.lua` fails the build otherwise (no exceptions). Chutes (U12):
  `levitate:facing` 0 down, 1..4 north / south / west / east (diagonal), `levitate:shape` 0 normal,
  1 window, 2 intersection, 3 encased; smart chutes `levitate:powered` [0, 1]. A unit whose state
  values are not its natural numbers converts at its edges (placement, rotation handler,
  schematic mapping).
- **One definition per string.** A Create string uses Create's key, Create's en_US text and its official
  zh_CN text (placeholders as `{0}`); Levitate-only copies of Create strings are not added.
  Shared generic keys (`create.generic.unit.*`, `create.action.*`, `create.schedule.condition.threshold.*`
  units, `create.gui.schedule.move_*`, `create.logistics.filter.*`, `lv.common.filter.empty`, vanilla fluid
  names `fluid_type.minecraft.*`) are defined in `common.json` (U3); other units reference them with
  `tr(key, "English fallback")`, the fallback being the en_US text. Ponder step texts (U30) are the
  scenes' own.
- **One fluid registry**: `feature/common/FluidRegistry.h` (fluid types, names, world blocks,
  bucket / bottle items and amounts, both potion id forms, open-pipe effects). U19 registers Create's
  fluids on it; every unit names fluids with `fluidtypes::displayName`.
- Selftests `consistency.*` (dx 3500..3599).

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
| Ponder scene player, tag screen, index (§3.6.30; client mod only, no form: §8) | hold W over an item, `/create ponder [id]` | U30 |
| Threshold switch (`redstone.threshold_switch`; the smart observer has only a filter slot) | right-click | U23 |

## 5. Configuration

One section per unit (`config/<unit>.json`, `LV_REGISTER_CONFIG`). Keys mirror Create's server
config (CKinetics, CStress, CLogistics, CFluids, CTrains, CSchematics, CEquipment, CRecipes, CWorldGen)
with the same defaults; each unit lists its keys here in its PR. Create's config GUI is
`/create config [section]` (U42): the sections, then a scrolled list of the section's booleans,
numbers and texts (nested maps such as the stress table as `impact > mechanical_press`), with the
default in the tooltip and a reset button; operators change values (the file is rewritten with the
change merged, then every config reloads as with `/create reload`; log `config <section>.<path>
set to <value> by <player>`), everybody else may look. `config::ConfigEntry` gained `values`,
`defaults` and `write` for it. The stress table (impact /
capacity per RPM, Create defaults ⚠) belongs to `config/kinetics.json` (U2) so all units read one
place:

| Block | Impact | Capacity | Generated RPM |
|---|---|---|---|
| hand_crank / valve handles | | 8 | 32 |
| water_wheel | | 32 | 8 |
| large_water_wheel | | 128 | 4 |
| windmill_bearing | | 512 ⚠ (per sail-count rules) | sails / 8 ⚠ |
| steam_engine | | 1024 (per engine, on its powered shaft, §3.6.18) | 16..64 (boiler driven) |
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
| mechanical_bearing / clockwork / piston / rope and elevator pulleys | 4 | | |
| gantry_shaft | 0 | | |
| turntable | 4 | | |
| belt / depot / gearbox / shaft / cogs | 0 | | |
| speed controller / sequenced gearshift / chain drives | 0 / 0 / 0 | | |
| mechanical_pump | 4 | | |
| hose_pulley | 4 | | |
| chain_conveyor | 1 | | |

`config/packages.json` (U25): `packagePortRange` 5, `chainConveyorCapacity` 20 (Create's CLogistics),
`maxChainConveyorLength` 32, `maxChainConveyorConnections` 4 (Create's CKinetics).

`config/pistons.json` (U20): `maxPistonPoles` 64, `maxRopeLength` 384 (Create's CKinetics keys and
defaults; the push line uses U4's `maxChassisRange`). Stress (checked): pistons, rope and elevator
pulleys 4; gantry shaft and carriage 0.
`config/display.json` (U24): `displayLinkRange` 64 (Create's CLogistics).

`config/stock.json` (U26): `factoryGaugeTimer` 100 (ticks between a gauge's requests, Create's
CLogistics default, at least 5).
`config/schematics.json` (U29, Create's CSchematics keys and defaults): `creativePrintIncludesAir`
false, `maxSchematics` 10, `maxTotalSchematicSize` 256 (KB), `maxSchematicPacketSize` 1024 (bytes),
`schematicIdleTimeout` 600 (ticks), `schematicannonDelay` 10 (ticks), `schematicannonShotsPerGunpowder` 400.

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
900, `enchantedBacktankCapacity` 300, `maxExtendoGripActions` 1000, `maxPotatoCannonShots` 200, `maxSymmetryWandRange` 50 (the
controller's link range is U23's `redstone.linkRange`).

`config/kinetics.json` (U2): `disableStress` (false), `maxRotationSpeed` (256), `kineticValidationFrequency`
(60), `mediumSpeed` (30), `fastSpeed` (100), `mediumStressImpact` (4), `highStressImpact` (8),
`mediumCapacity` (256), `highCapacity` (1024), `impact` {block path: SU per RPM} and `capacity`
{block path: SU per RPM} (the table above, plus `cuckoo_clock` 1, `weighted_ejector` 2, backtanks 4,
`elevator_pulley` 4, `gantry_shaft` 0 and 0 for the transmission blocks), `syncToClients` (true).
A block without a table entry uses its spec's `impact` / `capacity`. The generated RPM column is the
generators' own behaviour (U7, U18, U21), not a config value.

`config/trains.json` (U27, Create's CTrains): `trainsCauseDamage` (true), `maxAssemblyLength` (128),
`maxBogeyCount` (20), `manualTrainSpeedModifier` (0.75), `trainTopSpeed` (28), `trainTurningTopSpeed`
(14), `trainAcceleration` (3), `poweredTrainTopSpeed` (40), `poweredTrainTurningTopSpeed` (20),
`poweredTrainAcceleration` (3). (`maxTrackPlacementLength` is in `config/tracks.json`, U5.)

`config/gaps.json` (U37): Create's CClient `enableAmbientSounds` (true) and `ambientVolumeCap` (0.1),
applied by the server for everyone; `rotationIndicators` (true, Levitate switch for the goggles
particles). See §3.1 Kinetic feedback.
`config/generators.json` (U7): `crankHungerMultiplier` (0.01, exhaustion per hand crank use per RPM).

`config/ponder.json` (U30, client mod): `comfyReading` (false, Create's client option),
`holdSpeed` (1, scales the hold-to-ponder time), `depthScale` (0.1, depth of scene blocks in the
UI pass), `placeholderColour` (outline of unregistered blocks).

`config/materials.json` (U9): `disableWorldGen` (false, CWorldGen), `lightSourceCountForRefinedRadiance`
(10), `enableRefinedRadianceRecipe` (true), `enableShadowSteelRecipe` (true) (CRecipes).

## 6. Commands

| Command | Who | Owner |
|---|---|---|
| `/create reload`, `/create selftest ["glob"]` | op | foundation |
| `/create kinetics <pos>` (dump network: speed, stress, sources) | op | U2 |
| `/create killtrain <name\|id>` (Create's `/create train remove`), `/create trains` (train summary), `/create train tp <name\|id>` (creative / spectator) | op | U27 |
| `/create glue clear [radius]` (Create's `/create clearglue`; default 16, around the sender) | op | U4 |
| `/create contraption assemble <x y z> [axis]` / `move <id> <dx dy dz> [ticks]` / `rotate <id> <axis> <degrees> [ticks]` / `stop <id>` / `disassemble <id>` / `list` / `demo` / `chassis <x y z> <range>` (test anchor) | op | U4 |
| `/create ponder [id]` (client mod; no id: the index; an entry or a tag id) | anyone | U30 |
| `/create highlight <pos>` | op | U3 |
| `/create schematic list` (the caller's stored schematics), `/create schematic give <file>` (op: a written schematic of one of them) | anyone / op | U29 |
| `/create overlay` (overlay position screen, client mod) / `/create overlay reset` | anyone | U8 |
| `/create pipes <pos>` (pressures, flows and transfers of a pipe) | op | U17 |
| `/create steam <pos>` (tank / boiler goggle lines) / `steam <pos> fill <fluid> <mB>` (Levitate debug: fills, or drains when negative, through the fluid capability) | op | U18 |
| `/create display text <x y z> <line> <text>` / `bind <x y z> <tx ty tz> <source>` (Levitate debug) | op | U24 |
| `/create toolbox` (the toolbox radial menu as a form, for players without the client mod) | anyone | U31 |
| `/create toolbox lid <x y z> <ticks>` (Levitate debug: holds a toolbox lid open; 0 releases) | op | U45 |
| `/create advancements` (Create's advancement tree: native for the client mod, a form otherwise) / `advancements grant\|revoke <id>` (the caller's progress) | anyone / op | U45 |
| `/create config [section]` (Create's config screen: sections, then their entries; native for the client mod, forms otherwise; changing a value needs op) | anyone / op | U42 |

## 7. Strings

Create's English wording and key names (`create.*`) for every string that exists in Create, e.g.
`create.gui.goggles.kinetic_stats`, `create.gui.stressometer.overstressed`, `create.tooltip.stressImpact`.
Levitate-only strings use `lv.<unit>.*`. Each unit owns the keys of its features (en_US + zh_CN; zh_CN
matches Create's official Chinese translation wording).

## 8. Not ported

| Create feature | Why | What Levitate does instead |
|---|---|---|
| Animated parts / contraptions for **vanilla** clients | Bedrock has no client-side rendering hooks without a client mod | static block models; a moving contraption is drawn as one falling-block display entity per block, moved by the server (respawned when it drifts half a block); the client mod draws everything as Create does |
| JEI/EMI recipe viewers | third-party Java mods with no Bedrock counterpart | recipes visible in Ponder/tooltips; the configs are edited with `/create config` (U42) or as JSON + `/create reload` |
| Value box editing by *holding* right-click for **vanilla** clients | the server only sees discrete use packets, no release; the client mod reports it (`valuebox.hold` / `valuebox.release`, U42) | vanilla: a click opens the form |
| Filter slot items rendered on the block face for **vanilla** clients | no item rendering hooks for vanilla clients | the client mod draws the filter item on every face its slot is on, facing out, a quarter block big (display items, U33), and the hover label names it; vanilla players see it in the form / action messages (U3) |
| Value box outlines and labels for **vanilla** clients | no client-side rendering without the client mod | the client mod outlines the boxes and shows the label next to the crosshair (U3, U42) |
| Contraption blocks drawn as one rotating model on **vanilla** clients (U4) | a vanilla client draws a falling-block entity axis-aligned and can't be told to turn it | each block is its own falling-block entity at its exact rotated position, showing the block turned by the nearest quarter turn; the client mod turns each block by the remaining angle so the structure looks exact. Displays carry no gravity (actor flag HasGravity off, NoAI on; set again after the entity is added), so a vanilla client, which simulates falling blocks itself, doesn't drop a block that has no real block under it (a train carriage over its bogey) |
| Smooth contraption motion on **vanilla** clients (U34) | a vanilla client never applies the movement packets of a falling block that isn't falling: it stays where it was spawned | while a vanilla player is in the dimension, each display entity is spawned again at its place once it is half a block away (at most every 4 ticks per block, and where it comes to rest), so vanilla players see the structure move in half-block steps; the client mod moves the displays smoothly from the contraption's motion |
| Curved tracks drawn for **vanilla** clients (U5) | Bedrock block models are fixed per block state; a curve is not a block | the client mod draws the curve; vanilla clients see the straight end tracks and a track bed (`levitate:fake_track`) on every block the curve passes through |
| Tilted track block drawn for **vanilla** clients (U5, U48) | Bedrock block models are fixed per state; the ramp's angle is a real number per block | the curves, the graph and the trains follow Create's tilt for every client (§3.5); the client mod draws the block turned onto the ramp; vanilla clients see the flat block between the two curves' track beds |
| Smooth gauge needle for **vanilla** clients (U6) | block models are static per state; cube rotations are limited to 22.5° steps | the needle state snaps to the nearest 22.5°; the client mod draws the eased needle |
| Moving cuckoo clock hands, doors and figure for **vanilla** clients (U6, U49) | block models are static per state and turn cubes only in 22.5° steps | the hands are pixel steps in 12 positions each (`levitate:hour`, `levitate:minute` in 5-minute steps), the doors stand open with the figure out while Create's doors are past half open (`levitate:cuckoo`); the client mod draws Create's chasing hands, swinging doors and sliding figure |
| Flywheel collision and selection beyond its block (U49) | JSON collision / selection boxes are at most 16 px and inside the block | the 28 px wheel collides and is selected as its own block |

| Valve handle "holding opens the angle board" for **vanilla** clients (U7) | the server sees no held button (value box row) | client mod: as Create; vanilla: a click turns the handle, the wrench on its front opens the form |
| Large water wheel as one 3×3 model for **vanilla** clients (U7) | a Bedrock block model may be at most 30 px wide | the wheel and its 8 structure blocks each show their own cell of the wheel; the client mod spins the whole wheel |
| Red outline of the missing room when a large water wheel can't be placed, for **vanilla** clients (U7) | no client-side line rendering without the client mod | the action bar message only (the client mod draws the outline, U42) |
| Goggle overlay next to the crosshair for **vanilla** clients (U8) | no client-side drawing without a client mod | the same lines in the action bar (indentation trimmed), refreshed every `overlayInterval` ticks |
| Kinetic stats and "Hold [Shift] for Summary" in item tooltips for **vanilla** clients (U8) | Bedrock builds item hover text on the client from the item alone | the client mod appends them to the hover text, with the server's stress table (`kinetics.stress`, U42); vanilla clients see the plain name |
| Strikethrough in Create's speed text of an overstressed network for **vanilla** clients (U8) | Bedrock text has no strikethrough format | the client mod draws the line (`§~` spans, U42); vanilla: only dark gray |
| Wall collision predicted by **vanilla** clients (U9) | JSON collision boxes are at most 16 px high | the server and the client mod use Java's 24 px (collision hook); vanilla clients predict 16 px and are corrected by the server's movement authority |
| Chromatic compound charge bar and stack size 1 for **vanilla** clients (U9) | durability bars and per-stack max sizes aren't data-driven | the server and the client mod use Create's stack size (1 while charged); the client mod draws the bar; vanilla clients see no bar and predict stacks of 16 (the server corrects them) |
| Inner corners of Create's connected textures for **vanilla** clients (U9 / U10) | a face's texture comes from block states; the inner corners would need the 12 diagonal neighbours as states (2^18 states per block) | faces join along their four edges for every client; the client mod draws the frame's inner corners over the faces (casings, framed glass and panes, layered / pillar caps) |
| Climbable ladders / scaffolding, multi-box collision (U10) | no JSON component for either | the block types get the Climbable property and a collision hook (server and client mod); vanilla clients are corrected by the server's movement authority, so climbing and stairs feel slightly rubbery for them |
| Sliding door animation for **vanilla** clients (U10) | Bedrock models are static per state and must fit 30 px | the client mod draws the panel sliding / folding (Create's 0.15 per tick, eased); vanilla clients see the open state clipped to the model bounds, no animation |
| Item on the placard, copycat material, brackets drawn on **vanilla** clients (U10) | a block model can't show another item / block | the data and every behaviour exist on both; the client mod draws the placard's item (display item, U33), the copycat in its material (Create's cropped panel / step models) and the bracket on its shaft or cogwheel; vanilla clients see the placard frame, the copycat base and the plain shaft |
| Items riding belts / held on depots / thrown by ejectors drawn by the belt renderer, for **vanilla** clients (U11) | Bedrock has no item renderer for vanilla clients | the client mod draws each stack (display items, U33: lying, half size, a pile for bigger stacks, interpolated between ticks) and does not draw the fallback; vanilla players see each stack as an item entity (no pickup, no despawning, they don't merge by themselves, teleported along the belt every tick); with `displayItems` off they see nothing |
| Belt drops when removed by `/setblock ... destroy` (U11) | the block change the server sees carries no cause: a command's destroy mode looks like a replace | survival players breaking a segment and explosions (each segment with Create's 1 / radius survival chance) drop the connector / shaft; command removals drop nothing but the items on the belt |
| Items drawn inside chutes by a block entity renderer, for **vanilla** clients (U12) | custom blocks have no block entity renderers | the client mod draws the moving stack (display item, U33, interpolated); vanilla clients get client-only item entities (packets only, never a server entity, so nobody can pick them up) that follow each moving stack |
| Smooth (lerped) funnel flaps for **vanilla** clients (U12) | no per-frame block animation for custom blocks without a renderer | the flap is a block state shown for 4 ticks after a transfer; the client mod draws Create's four swinging strips |
| Editing a filter's amount by *holding* the filter slot, for **vanilla** clients (U12) | the server sees no held click from a vanilla client (the client mod reports it: `valuebox.release`) | companions hold the slot to open the "Extracted Amount" board (`SlotSpec::onHold`); vanilla players use a separate "Extracted Amount" value box next to the filter slot |
| Items floating in the basin for **vanilla** clients (U14) | no item rendering hooks for vanilla clients | the client mod draws the stacks inside (Create's ring around the centre, tilted, slowly turning, bobbing on the fluid, one more copy per 8 items) with the world item renderer (U33) and the fluid surface; vanilla clients see the basin; the items are taken out with an empty hand or seen by funnels |
| Deployer fake player (U15) | Bedrock has no fake players: a player that acts in the world must be a real (simulated) server player | one simulated player per dimension ("Deployer", invisible, invulnerable, survival) acts for every deployer: it is moved into the hand for each action and waits above the build limit in between. Like Create's fake player it is no player for the others: it is taken out of every client's player list, command selectors don't find it, it doesn't count for sleeping through the night and its kills drop no experience |
| Item held by a deployer drawn in its hand, item held by the arm, for **vanilla** clients (U15) | no item rendering hooks for vanilla clients | the client mod draws the held item in the moving hand and between the arm's claws (world item renderer, U33; the server sends the stack with the animation state); vanilla clients see static models, the hand pose shows whether it holds something |
| Transitional item progress bar (U15) | item bars come only from item durability, which the game also uses for item behaviour (breaking, repairing); custom items have none | no bar. The tooltip is Create's: the item's lore in the server's language for vanilla clients, rebuilt in the game's language by the client mod |
| Crafter items on the crafter's face for **vanilla** clients (U16) | Create renders the slot / grid items client-side; vanilla clients have no hook for it | the client mod draws the slot item, the grouped grid, the exported grid sliding to the next crafter and the growing result with the world item renderer (U33); every client sees the blocks, the crit particles, the sounds and the result popping out |
| Glass pipe flow fronts for **vanilla** clients (U17) | no client-side rendering without the client mod; a block state per progress step, face and fluid would not fit | the client mod draws Create's advancing fronts; vanilla clients see the glass pipe's `levitate:fluid` state fill the whole pipe as soon as a flow starts |
| Fluid inside tanks and the moving engine piston for **vanilla** clients (U18) | no client-side rendering without the client mod; a fluid level per block would need a block state per level and fluid | the client mod draws them; vanilla clients see the tank (windows show an empty inside) and a static engine |
| Boiler gauges for **vanilla** clients (U18) | the needle moves with the boiler level: client-side drawing (the client mod draws the gauges); a block state per needle position and side would need hundreds of tank states | vanilla clients see the boiler without gauges; the goggle lines (`/create steam`, U8's overlay) show the same status |
| Flowing honey/chocolate in the world (U19) | Bedrock has no data-driven custom fluids: a custom block can't flow, be swum in or use the fluid renderer | they exist everywhere fluids are handled (pipes, tanks, basins, spouts, drains, buckets); in the world a bucket or a hose pulley places a translucent, non-colliding, non-flowing source block (`levitate:honey` / `levitate:chocolate`) that buckets and hose pulleys pick up again; entities in it are slowed (Slowness II) and take no fall damage instead of Create's viscous fluid motion; lava touching it turns into obsidian / limestone / scoria; no fog colour under it |
| Hose drawn for **vanilla** clients (U19) | Create draws the hose with a block entity renderer; a block in its place would block the fluid it hangs in | the client mod draws the hose and its end piece; vanilla clients see the pulley only (draining and filling work the same) |
| Spout stream and nozzle squeeze for **vanilla** clients (U19) | no client-side animation without the client mod | the client mod draws Create's stream and squeezes the nozzle; vanilla clients see the static nozzle and the drops and splash (particles, every client) and hear the sounds |
| Ropes / elevator belts of *moving* pulleys for **vanilla** clients (U20) | the rope length changes every tick; it can't be a rigid contraption block, and placing rope blocks while moving would collide with riders | the client mod draws rope, belt and magnet; vanilla players see the moving structure, and the ropes and magnet as blocks once it stops |
| Cart assembler and controller rail as real rails (U21) | Bedrock minecarts only ride vanilla rail blocks; a data-driven block can't be a rail | they are rail-shaped blocks without collision; a minecart on one is guided by the server: held on the rail line at rail height and moved along it at the speed the rail gives it (no off-rail friction), then the next vanilla rail takes it over. A minecart item used on them places a cart there |
| Furnace minecart contraptions (U21) | Bedrock has no furnace minecart entity | contraptions ride rideable and chest minecarts; the wrench picks them up as `minecart_contraption` / `chest_minecart_contraption` (the chest cart's own inventory travels in the item) |
| Right-clicking contraption blocks with an empty hand into the air on **vanilla** clients (U22) | a vanilla client sends nothing for an empty-hand click into the air, and display entities can't be clicked | with an item in hand, or with a world block behind, the server casts the look itself; the client mod sends the click |
| Redstone link frequency items on the block for **vanilla** clients (U23) | no world-space item rendering without the client mod | the client mod draws both items in their slots (U42); vanilla: action-bar message when a slot is set |
| Diode value box "short click toggles, hold opens the board" for **vanilla** clients (U23) | see the value box row above | client mod: as Create (U42); vanilla: clicking the box opens the board, clicking elsewhere on the diode (or with any item but the wrench) toggles inverted |
| Desk bell wobble for **vanilla** clients (U23, U49) | no per-frame block animation without the client mod | the plunger is pressed while the bell is powered; the client mod draws Create's dipping plunger and wobbling bell |
| Display board text for **vanilla** clients (U24) | no client-side text rendering on blocks without the client mod | the lines as a name tag in front of a turning board (invisible entity, colours as § codes, no flap animation); the client mod draws the split flaps |
| Nixie tube glyphs for **vanilla** clients (U24) | same | the two characters as a coloured name tag above each tube |

| Deaths statistic of the respawn anchor source (U24) | Bedrock has no deaths statistic | Levitate counts player deaths per world (`lv:display:deaths`) |
| Package entities piling on each other (U25 / U46) | Create's PackageEntity collides only with packages below it; Bedrock entity collision is per type (`minecraft:is_stackable` would let players stand on packages too, which Create's packages don't allow) | package entities (§3.6.25) pass through each other; everything else of the entity is ported |
| Chains, hanging packages, frogport tongue and packager tray for **vanilla** clients (U25) | no client-side rendering without the client mod | the client mod draws them; vanilla clients see the static conveyor, frogport and packager models and the postbox flag / lid states |
| Factory gauge item icons, counts and connection arrows for **vanilla** clients (U26) | no world-space item / text rendering without the client mod | the client mod draws them (U42); vanilla: the bulb is the block model |
| "Click and hold" on a gauge panel / price tag for **vanilla** clients (U26) | the server only sees discrete clicks | client mod: as Create (U42); vanilla: sneak + empty-hand click opens the board (price tag: a click opens it, sneaking clears) |
| JEI recipe sync of factory gauges (U26) | no recipe viewer on Bedrock | "Use Mechanical Crafting" works (U42); recipes can't be dragged in from a viewer |

| Train carriages tilting on slopes for **vanilla** clients (U27) | a falling-block entity is always drawn level | the blocks stand at their tilted places, each drawn level; the client mod draws the carriage pitched (Create's pitch from its bogeys) |
| Wheels turning on bogeys for **vanilla** clients (U27) | a display entity shows a fixed block model | still wheels; the client mod turns them with the distance travelled (Create's BogeyRenderer) |
| Station flag animation for **vanilla** clients (U27) | the flag would need a per-frame model | the station's texture shows the flag (green in assembly mode); the client mod raises and waves Create's flag |
| Train HUD and mouse-wheel throttle for **vanilla** clients (U27) | the server can't see the mouse wheel nor draw a HUD | action-bar prompts; throttle from schedules; the horn works for vanilla clients through their sprint input (U57); the client mod has Create's HUD and throttle (§3.6.27) |
| Train map colours / map markers (U27) | no map mods on Bedrock | none |
| Train relocation preview for **vanilla** clients (U27) | the server can't draw the train's outline on a vanilla client | relocation works with action-bar prompts (valid / invalid / too far); the client mod draws the carriages at the target |
| Glint of a signal / track observer item carrying a track selection (Create's isFoil) (U28) | Bedrock draws no enchantment glint on items shown with their block's model (verified: a per-item vtable override of `isGlint` returns true and nothing is drawn) | the selection is on the item stack and travels with it; the client mod outlines the selected track while the item is held |
| Signal lamp rendered toward the track, track overlay for vanilla clients (U28) | Bedrock block models are fixed per state and can't face an edge point | the lamp shows on all four sides of the signal head (block state); the client mod draws Create's arrows / squares on the track |
| Crafting blueprint as a hanging entity (U29) | Create's blueprint is a hanging entity; a block group behaves the same for clicks, crafting and breaking | each section is a thin, non-colliding `levitate:placed_crafting_blueprint` block; the 2x2 / 3x3 group is placed and removed together; the client mod draws the display items on the canvas (Create's BlueprintRenderer), vanilla clients see the empty canvas |
| Schematic files saved as Create `.nbt` (U29) | Bedrock block states have no lossless Java form | the quill saves Bedrock `.mcstructure`; Create `.nbt` files are read (uploads, `schematics/`) with best-effort Java→Bedrock state mapping, unmapped block types are reported and skipped |
| Schematic tools, ghost and quill outline for **vanilla** clients (U29) | no client-side input (Ctrl / Alt / scroll) or world rendering without the client mod | sprinting = Ctrl for mid-air corners; no face resizing; right-click a block deploys a schematic there, sneak-right-click opens a form for exact coordinates / rotation / mirror / Print; particle outlines of the selection and of the deployed bounds |
| Picking local files at the schematic table for **vanilla** clients (U29) | the server can't read the player's disk | the form lists the player's schematics stored on the server (quill saves, earlier uploads) |
| Ponder for **vanilla** clients (U30) | a scene player needs client-side 3D rendering of a private scene; forms can't draw blocks | Ponder is client mod only; `/create ponder` tells vanilla players it needs the client mod |
| Ponder scenes as Create's scene code and NBT schematics (U30) | Create's scenes are Java code over its own schematics (and copying them is not allowed) | our own scene scripts as data (`assets/ponder/scenes/*.json`) teaching the same steps; titles keep Create's wording, the texts are our own |
| "Hold [W] to Ponder" progress bar inside the item tooltip (U30) | Bedrock builds an item's hover text once when the slot is hovered (verified: the line doesn't change while the key is held; re-hovering the slot from the mod to rebuild it can outlive the screen controller) | the hint is the tooltip's second line; while the key is held Create's `\|` bar is drawn in a tooltip box at the top of the screen |
| Holding the toolbox key on a **vanilla** client (U31) | the server can't see a held key without the client mod | `/create toolbox` opens the same entries as a form |
| Enchantments Capacity (backtank air) and Potato Recovery as real enchantments (U31) | Bedrock's enchantment list is a fixed set of vanilla ids (0..41) that clients know by number; an unknown id would reach vanilla clients | the levels live in the item's user data and show as lore lines ("Capacity II"); the enchanting table rolls them per option with Java's algorithm, anvils merge them, grindstones remove them (§3.6.31); the table's clue and option tooltips show only vanilla enchantments, and they are not on loot or trade books |
| Diving boots movement for **vanilla** clients (U31) | players move themselves (client-authoritative input); the server can only push them | the client mod applies Create's motion to its own player; vanilla players sink through motion packets and walk faster under water through the underwater movement speed attribute (Create's ×1.3 per tick below 0.145 b/t settles at ~0.15 b/t: +87.5 %) |
| Extendo grip reach for **vanilla** clients (U31) | the client limits its own block picking range | the pick range is raised in GameMode (server validation and the client mod's local player); vanilla clients keep their normal reach |
| Glowing of glow berry / glistering melon potato projectiles (U31) | Bedrock has no glowing effect | the hit has its other effects |
| Linked controller input for **vanilla** clients (U31) | movement keys are only visible through Bedrock's movement input | the movement input the server receives is polled (the player still moves); the client mod captures the keys as Create |
| Worldshaper preview outline for **vanilla** clients (U31) | no world-space line rendering without the client mod | the client mod draws Create's outline; vanilla players see the blocks change at once |
| Swinging peculiar / haunted bells for **vanilla** clients (U49) | custom blocks have no block entity renderer; a state per swing frame and direction would need dozens of models | the client mod draws Create's swing; vanilla clients see the bell still (sounds, souls and particles reach everyone) |
| Bells rung by projectiles (U49) | custom blocks get no projectile-hit callback (`BlockType::onProjectileHit` is a folded no-op shared by many blocks, it can't be hooked for ours alone) | clicks and redstone ring them |
| Soul particles vanishing when the spot gets lit (U49) | resource-pack particles can't read the block light | the particles live their full time; new pulses only mark dark spots |
| The hiding player's smaller hitbox and eye height (U49) | Bedrock clients size and move their own player (sneaking height is the client's); a server-side size is not used for the player's collision | the player becomes the box visually (invisible + a box entity at its feet, which hits pass through to the player); hitbox and camera stay the sneaking ones |
| Cardboard vignette for **vanilla** clients (U49) | no HUD drawing without the client mod | the client mod draws it (in every camera view: the mod can't read the view perspective; Create: first person only) |
| Cardboard sword's 3D in-hand model (U49) | Bedrock items have no custom item renderer, and an attachable's first-person transform is not solved (wrench row above) | the sprite in hand, as for every item |

Units add rows here in their PRs (in unit order) when they find something else impossible.

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

### Wave 3 (integration and polish; after wave 2)

| Unit | Name | Owns |
|---|---|---|
| U32 | `integration` | cross-unit integration: contract extensions (§3.1 sequence context, §3.2 interactions and redstone input, §3.3 text fields), moving contraptions without the falling-block fallback |
| U33 | `itemrender` | display items (§3.3): the client mod's world item renderer and its use by belts, chutes, filters, placards, deployers, crafters and basins |
| U34 | contraption motion | moving contraptions drawn and simulated on the client |
| U35 | `consistency` | §3.6.35: block state ranges, one definition per string, one fluid registry, the layout of this spec |
| U36 | shared hooks | GameMode hooks moved onto the interactions contract, shared redstone / comparator helpers |
| U37 | feature gaps | kinetic sequence context in the machines, train input, text fields in the U24 / U25 / U26 screens, client item stacks in the stock / schematics screens, kinetic particles and sounds |

Cross-unit integration (e.g. a funnel on a belt, a spout over a depot, a deployer on a contraption)
goes through wave-1 contracts (capabilities, actors, BlockData); a unit never includes another
wave-2 unit's headers. When both sides exist, the integration works without either unit naming the
other.
