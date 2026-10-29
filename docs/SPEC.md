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

`belt_connector` (item), `belt` (block, states `part` start/middle/end/pulley, `slope`, `facing`,
`casing`), `depot`, `weighted_ejector`.

### 2.7 Item logistics (U12)

`andesite_funnel`, `brass_funnel`, `andesite_tunnel`, `brass_tunnel`, `chute`, `smart_chute`,
`item_vault`, `creative_crate`.

### 2.8 Bulk processing (U13)

`millstone`, `crushing_wheel` (+ `crushing_wheel_controller`, invisible), `encased_fan`, `nozzle`.

### 2.9 Basin processing (U14)

`basin`, `mechanical_press`, `mechanical_mixer`, `blaze_burner` (states `blaze` none/smouldering/
fading/kindled/seething), `empty_blaze_burner`, `lit_blaze_burner`.

### 2.10 Assembly (U15)

`deployer`, `mechanical_arm`, sequenced assembly intermediates (`incomplete_*`).

### 2.11 Crafting tools (U16)

`mechanical_crafter`, `mechanical_saw`, `mechanical_drill` (behaviour in §3.6 "Crafting tools").

- `mechanical_crafter`: states `levitate:facing` 2..5 (the front, horizontal), `levitate:pointing`
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

`fluid_pipe`, `glass_fluid_pipe`, `encased_fluid_pipe`, `mechanical_pump`, `fluid_valve`, `smart_fluid_pipe`.

### 2.13 Tanks and steam (U18)

`fluid_tank`, `creative_fluid_tank`, `steam_engine`, `steam_whistle` (+ `steam_whistle_extension`), `powered_shaft` (internal).

### 2.14 Fluid machines (U19)

`spout`, `item_drain`, `hose_pulley`, `portable_fluid_interface`, fluids `honey`, `chocolate`,
`tea` (builder's tea), potion fluid (by potion type), `bucket` variants `honey_bucket`, `chocolate_bucket`.

### 2.15 Linear contraptions (U20)

`mechanical_piston`, `sticky_mechanical_piston`, `piston_extension_pole`, `mechanical_piston_head`,
`rope_pulley`, `rope`, `pulley_magnet`, `elevator_pulley`, `elevator_contact`, `gantry_carriage`,
`gantry_shaft`.

### 2.16 Rotating contraptions (U21)

`mechanical_bearing`, `windmill_bearing`, `clockwork_bearing`, `sail_frame`, `white_sail` +
`<colour>_sail`, `cart_assembler`, `minecart_coupling` (item), `controller_rail`, `turntable`.

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

- **Belts (U11)**: 2–20 ⚠ blocks long, connect two shafts in a line or slope (45°), items move at
  speed proportional to RPM, belt carries entities, funnels/tunnels/depots/processing work on belt
  items, belt casing, dye, wrench to adjust, breaking returns belts.
- **Fans (U13)**: push/pull entities and items along up to `fanPushDistance` 20 ⚠ blocks; items in the
  air stream through water → washing, fire/campfire/lava → smoking or blasting, soul fire → haunting.
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

#### 3.6.1 Crafting tools (U16)

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
- Client mod: the slot item on the front (half a block), the grouped grid spread over the front
  half a block apart, sliding one block along the arrow while exporting, the result growing while
  crafting. Vanilla clients see the blocks and the particles only (§8).

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
| steam_engine | | 1024 ⚠ | boiler driven |
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

`config/contraption.json` (U4): `maxBlocksMoved` 2048, `maxChassisRange` 16 (Create's CKinetics
keys and defaults), `maxSuperGlueRange` 24, `glueOutlines` true, `unmovableBlocks` (extra unmovable ids).

`config/crafter.json` (U16, Create's CRecipes / CKinetics keys and defaults):
`allowRegularCraftingInCrafter` (true), `maxFireworkIngredientsInCrafter` (9),
`allowStonecuttingOnSaw` (true), `bulkCutting` (false), `moveItemsToStorage` (true).

`config/kinetics.json` (U2): `disableStress` (false), `maxRotationSpeed` (256), `kineticValidationFrequency`
(60), `mediumSpeed` (30), `fastSpeed` (100), `mediumStressImpact` (4), `highStressImpact` (8),
`mediumCapacity` (256), `highCapacity` (1024), `impact` {block path: SU per RPM} and `capacity`
{block path: SU per RPM} (the table above, plus `cuckoo_clock` 1, `weighted_ejector` 2, backtanks 4,
`elevator_pulley` 4 ⚠, `gantry_shaft` 4 ⚠ and 0 for the transmission blocks), `syncToClients` (true).
A block without a table entry uses its spec's `impact` / `capacity`. The generated RPM column is the
generators' own behaviour (U7, U18, U21), not a config value.

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
| Placeable flowing honey/chocolate in the world | Bedrock has no data-driven custom fluids | they exist in pipes, tanks, basins, buckets (as items); emptying a bucket in the world places a custom non-flowing source block ⚠ U19 decides |
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
| Redstone input of the mechanical crafter (U16) | custom blocks get no redstone neighbour update | the crafter polls the power of its neighbours every tick (same sources as the sticker) |
| Crafter items for **vanilla** clients (U16) | Create renders the slot / grid items on the crafter's face client-side | the client mod draws them; vanilla clients see the blocks, the crit particles and the result popping out |
| Saw output only into blocks with Create's DirectBeltInputBehaviour (U16) | the item capability doesn't say whether a block takes "direct belt input" | the saw (and its belt neighbours) hands its items to Levitate blocks with an item handler except vaults; vanilla containers next to a saw are skipped as in Create |
| Sequenced assembly steps on the saw (U16) | the sequenced assembly state belongs to U15 | the saw cuts plain `cutting` recipes; U15 adds the sequenced cutting step |
| Block break sounds of the drill / saw per block sound type (U16) | the server can't name a block's hit sound for a PlaySound packet | `hit.wood` / `hit.grass` / `hit.gravel` / `hit.sand` / `hit.stone` by the block id |
| Saw / drill damage source names (U16) | Bedrock has no custom damage causes | contact damage (death message of a cactus-like cause) |

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
