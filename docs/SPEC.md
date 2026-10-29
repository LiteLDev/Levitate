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
States: `axis` (x/y/z) for shafts and cogs, `facing` for gauges, `powered` for clutch/gearshift,
`part` for chain drives, `encased` variants are separate ids (as Create).

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

`wrench`, `goggles` (engineer's goggles, head slot).

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
World generation: zinc ore veins (overworld, y −63..70 ⚠ Create 6 vein config), striated stone layers
(asurine, crimsite, ochrum, veridium, limestone, scorchia, scoria in their biomes/depths ⚠).

Decorative stone (U9): for each of `granite, diorite, andesite, calcite, dripstone, deepslate, tuff,
asurine, crimsite, limestone, ochrum, scoria, scorchia, veridium`: the raw stone (for Create's own
stones) and `cut_<s>`, `cut_<s>_stairs/_slab/_wall`, `polished_cut_<s>` (+ stairs/slab/wall),
`cut_<s>_bricks` (+ stairs/slab/wall), `small_<s>_bricks` (+ stairs/slab/wall), `layered_<s>`,
`<s>_pillar`. Stonecutter recipes as Create.

### 2.5 Building blocks (U10)

`andesite_casing`, `brass_casing`, `copper_casing`, `shadow_steel_casing`, `refined_radiance_casing`,
`railway_casing`; framed/tiled/horizontal/vertical framed glass + panes; windows for every wood type +
`ornate_iron_window` + panes; `copper_shingles`/`copper_tiles` (all oxidation + waxed stages, stairs,
slabs); `andesite_scaffolding`, `brass_scaffolding`, `copper_scaffolding`; `andesite_ladder`,
`brass_ladder`, `copper_ladder`; `andesite_bars`, `brass_bars`, `copper_bars`; doors `andesite_door`,
`brass_door`, `copper_door`, `train_door`, `framed_glass_door`, trapdoors `train_trapdoor`,
`framed_glass_trapdoor`; `metal_girder` (+ encased shaft variant), `metal_bracket`, `wooden_bracket`;
`<colour>_table_cloth` + `andesite/brass/copper_table_cloth` (deco; shop behaviour is U26);
`placard`; `<colour>_toolbox` is U31; `<colour>_seat` is U22; `<colour>_valve_handle` is U7;
`copycat_panel`, `copycat_step` (take the look of an applied block).

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
`item_vault`, `creative_crate`.

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

`mechanical_crafter`, `mechanical_saw`, `mechanical_drill`.

### 2.12 Fluid transport (U17)

`fluid_pipe`, `glass_fluid_pipe`, `encased_fluid_pipe`, `mechanical_pump`, `fluid_valve`, `smart_fluid_pipe`.

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
Mechanics in §3.6.1.

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
`contraption_controls`, `<colour>_seat` (16).

### 2.19 Redstone (U23)

`redstone_link`, `analog_lever`, `pulse_repeater`, `pulse_extender`, `pulse_timer`, `powered_latch`,
`powered_toggle_latch`, `smart_observer`, `threshold_switch`, `redstone_contact`.

### 2.20 Display (U24)

`display_link`, `display_board`, `nixie_tube` + `<colour>_nixie_tube`, `clipboard`.

### 2.21 Packages (U25)

`packager`, `repackager`, `cardboard_package_*` (box styles), `package_frogport`, `chain_conveyor`,
`<wood>_postbox`, `package_filter`, `filter` + `attribute_filter` are U3.

### 2.22 Stock network (U26)

`stock_link`, `stock_ticker`, `factory_gauge`, `redstone_requester`, `blaze_burner` shopkeeper role on
`<colour>_table_cloth`, `shopping_list` (item).

### 2.23 Trains (U5 tracks, U27 trains, U28 signals)

U5: `track`, curved track (no separate id: stored in BlockData + graph), `track_station` is U27.
U27: `track_station`, `small_bogey`, `large_bogey`, `controls`, `schedule` (item), `railway_casing` is
U10, `train_door` is U10. U28: `track_signal`, `track_observer`.

U28 (details in §3.6.28): `track_signal` ("Train Signal", states `levitate:signal_type` 0 entry /
1 cross, `levitate:signal_state` 0 off / 1 red / 2 yellow / 3 green, `levitate:powered`; post
with a lamp head, metal sound, 1.5 s to mine, drops itself) and `track_observer` ("Train
Observer", `levitate:powered`, full block with a lens on top that lights up, emits redstone 15
on every side while powered). Both are placed by first right-clicking the track they should
watch (Create's track targeting items). Recipes (pending until U9 / U10 land, in
`assets/pending_recipes/signals/`): `track_signal` ×4 = railway casing + electron tube
(shapeless), `track_observer` ×2 = railway casing + any pressure plate (shapeless) ⚠.

### 2.24 Schematics (U29)

`schematic_table`, `schematicannon`, `empty_schematic`, `schematic`, `schematic_and_quill`,
`crafting_blueprint`.

### 2.25 Equipment (U31)

`<colour>_toolbox` (16), `copper_backtank`, `netherite_backtank`, `copper_diving_helmet`,
`copper_diving_boots`, `netherite_diving_helmet`, `netherite_diving_boots`, `extendo_grip`,
`potato_cannon`, `wand_of_symmetry`, `handheld_worldshaper`, `linked_controller`, `sand_paper`,
`red_sand_paper`, `tree_fertilizer`, `minecart_contraption` (item).

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
- **Trains (U27/U28)**: assembly at a station from bogeys + casing, controls drive manually,
  schedules automate (instructions/conditions as Create), signals split the graph into sections,
  trains brake for red signals, derailment/collision behaviour as Create.
- **Schematics (U29)**: Bedrock `.mcstructure` and Create `.nbt` schematics in `schematics/`;
  placement tool overlays the ghost (client mod), schematicannon prints with gunpowder and materials
  from attached inventories.
- **Ponder (U30)**: hold W over an item to open Ponder (client mod only), index of all scenes,
  animated scenes built from our blocks.

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
  (`create.track_target.clear`). Selections are per player and per item (§8).
- The block keeps its target. When the track graph around it is rebuilt or removed, it binds
  again; while its track is gone the signal is dark (`Invalid`) and the boundary is gone.

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
- A boundary side without a signal block doesn't stop trains (it still splits sections).
- A redstone-powered signal is red for trains (forced red).

**Signal state** (Create's tickState, every tick): red when forced by redstone or when the section
is occupied by any train or reserved through another signal; for a cross signal, yellow when it
would be green but a signal leading out of its section (on the way forward, no turns sharper
than Create's 7/8 rule) is red; green otherwise; off when unbound. The block shows it with its
lamp (`levitate:signal_state`, vanilla clients and the client mod alike). Comparators reading a
signal output 15 while it is red, 0 otherwise. Redstone input sets `levitate:powered` (polled
every 4 ticks, §8).

**Track observers** (edge point type `levitate:observer`, never shared, can't be within 0.75 of
another point): powered while a train covers its position, and for 8 ticks after (Create's
`keepAlive`), with redstone 15 to all sides (`minecraft:redstone_producer` on the powered state;
the circuit component's strength is set with the state change). The filter slot on
top (U3 filter slot, "filter") limits it to trains carrying a matching item (in any carriage
inventory) or fluid (any tank), asked through the trains' cargo provider; an empty filter
matches every train.

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

Persistence: the blocks (target, bound point, side, cached type and power) in `lv:signals:blocks`;
edge points are stored with the track graph (U5); observer filters in BlockData; sections,
occupancy and reservations are rebuilt (trains report again after a load).

## 4. Screens

Each screen: native (client mod) + form fallback (vanilla). Owner unit in brackets.

| Screen | Opened by | Owner |
|---|---|---|
| Filter / attribute filter | right-click filter item | U3 |
| Value box form (vanilla) | wrench right-click on a value box | U3 |
| Sequenced gearshift instructions | right-click | U6 |
| Toolbox | right-click / radial select (client mod) | U31 |
| Linked controller binding | right-click | U31 |
| Wand of symmetry / worldshaper | right-click | U31 |
| Schematic table, schematicannon, schematic & quill naming | right-click | U29 |
| Clipboard | right-click | U24 |
| Display link configuration | right-click | U24 |
| Redstone requester / factory gauge / stock ticker / shopping list | right-click | U26 |
| Packager address, frogport address, postbox | right-click | U25 |
| Train schedule editor | right-click with schedule | U27 |
| Station (name, assemble/disassemble, door control) | right-click station | U27 |
| Crafting blueprint | right-click | U29 |
| Ponder | W over item (client mod) | U30 |
| Threshold switch / smart observer bounds | right-click | U23 |

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

`config/pistons.json` (U20): `maxPistonPoles` 64, `maxRopeLength` 384 (Create's CKinetics keys and
defaults; the push line uses U4's `maxChassisRange`). Stress (checked): pistons, rope and elevator
pulleys 4; gantry shaft and carriage 0 (`gantry_shaft` 4 in the table above is wrong).

`config/contraption.json` (U4): `maxBlocksMoved` 2048, `maxChassisRange` 16 (Create's CKinetics
keys and defaults), `maxSuperGlueRange` 24, `glueOutlines` true, `unmovableBlocks` (extra unmovable ids).

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

`config/kinetics.json` (U2): `disableStress` (false), `maxRotationSpeed` (256), `kineticValidationFrequency`
(60), `mediumSpeed` (30), `fastSpeed` (100), `mediumStressImpact` (4), `highStressImpact` (8),
`mediumCapacity` (256), `highCapacity` (1024), `impact` {block path: SU per RPM} and `capacity`
{block path: SU per RPM} (the table above, plus `cuckoo_clock` 1, `weighted_ejector` 2, backtanks 4,
`elevator_pulley` 4 ⚠, `gantry_shaft` 4 ⚠ and 0 for the transmission blocks), `syncToClients` (true).
A block without a table entry uses its spec's `impact` / `capacity`. The generated RPM column is the
generators' own behaviour (U7, U18, U21), not a config value.

`config/generators.json` (U7): `crankHungerMultiplier` (0.01, exhaustion per hand crank use per RPM).

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
| `/create steam <pos>` (tank / boiler goggle lines) / `steam <pos> fill <fluid> <mB>` (Levitate debug: fills, or drains when negative, through the fluid capability) | op | U18 |
| `/create overlay reset` | anyone | U8 |

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
| Signal / observer track selection stored on the item (U28) | changing the held item's data from the server re-sends the slot and breaks the click flow; the glint isn't controllable | the selection is kept per player and per item id on the server (lost on relog); no glint, no outline of the selected track |
| Signal lamp rendered toward the track, track overlay for vanilla clients (U28) | Bedrock block models are fixed per state and can't face an edge point | the lamp shows on all four sides of the signal head (block state); the client mod draws Create's arrows / squares on the track |
| Redstone input of signals (U28) | custom blocks get no redstone neighbour update | polled every 4 ticks (as U4's sticker; diodes count only when facing the signal) |
| Comparator output of signals (U28) | Bedrock asks only containers for analog signals | the signal's 15 / 0 is handed to the comparator's circuit component before it evaluates (ComparatorCapacitor hook, as U12's vault) |
| Train occupancy by front/back signal listeners (U28) | not a limitation: trains report the track they cover | sections and observers follow the covered track (same results as Create's listeners, robust against skipped ticks) |
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
