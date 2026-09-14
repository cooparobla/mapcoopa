# mapcoopa

`mapcoopa` is a header-only C++ world generator: a seed and a `MapConfig` in, a whole
`MapGraph` out — Voronoi cells carrying an elevation, a climate and one of 33 biomes,
threaded with rivers that run downhill to the sea and a road network routed between the
places worth travelling between — graded highway, road and trail by the traffic each
stretch carries, bridging the rivers it crosses; nations divided into provinces whose
borders settle on ridgelines and coasts; settlements
placed where people would actually live, each with a population counted from its buildings
and a name in its region's own invented language; and landmarks read off the terrain
itself. It is a port of Amit Patel's *Polygonal Map Generation*
([Red Blob Games](https://www.redblobgames.com/maps/mapgen2/)).

The world has a **scale**: 60 metres to a grid cell, so the default map is 4.8 km square,
and every physical size — a 6 m road, a 10 m cottage, a 5 m stream — is configured in
metres. Rendered at the default one pixel per metre, a width measured off a PNG is a
measurement of the ground.

It produces **data**, not pixels: nothing here touches Vulkan, GLFW or any sibling repo
other than [`libcoopa`](../libcoopa), which it uses for exactly two headers
(`coopa/collections/yaml_map.h` and `coopa/debug/logger.h`). The two PNG renderers are a
software-only debugging convenience; the deliverable is the `MapGraph`, and
`coopa/maps/map_yaml.h` writes one to disk so a game can load a world it did not generate.

Everything is reproducible: a `MapConfig` plus its `seed` fully determines the output,
byte for byte, including the settlements and the wobble on every coastline.

Full module documentation, the data-flow diagram and a usage example live in
[**`coopa/maps/README.md`**](./coopa/maps/README.md); the twelve generation stages are
documented in [`coopa/maps/passes/README.md`](./coopa/maps/passes/README.md).

---

## Generating a map

`cbuild` configures and builds; `cplay` runs `./build/mapcoopa` — the generator — and
forwards its arguments verbatim.

```bash
cd libs/mapcoopa
cbuild
cplay --seed=42 --out=world
```

That writes `world.yaml` and seven PNG layers into the current directory. Settings come from [`assets/config.yaml`](./assets/config.yaml); flags override
it for one run. With no `--seed` on either, the generator draws one from system entropy and
prints it, so every run differs but stays reproducible afterwards.

`cbuild` wipes `build/` and reconfigures every time. For a fast iteration loop after the
first build:

```bash
cmake --build build && ./build/mapcoopa --out=world
```

### Options

| Flag | Default | Effect |
|---|---|---|
| `--seed=N` | random, printed | Master seed; reproduces a map exactly |
| `--grid-size=N` | 80 | Cells per axis. The island noise frequency scales with this, so a bigger map resolves more finely instead of fragmenting |
| `--image-size=N` | 4800 | Render size in pixels, square. Back-computes `meters_per_pixel`, so the two cannot disagree — see [Scale](#scale) |
| `--rivers=N` | 55 | River sources to attempt |
| `--towns=N` | 28 | Settlements to place |
| `--road-hubs=N` | 32 | Places the road network is routed between |
| `--countries=N` | 5 | Nations to carve out |
| `--regions=N` | 3 | Provinces per nation |
| `--no-regions` | — | Skip political geography entirely |
| `--no-landmarks` | — | Skip notable places |
| `--no-roads` | — | Skip the road network |
| `--no-subdivide` | — | Straight cell boundaries instead of wobbled ones |
| `--out=PATH` | `map_out` | Output prefix |
| `--config=PATH` | `assets/config.yaml` | Settings file to read before applying flags |
| `--shading=MODE` | `elevation` | Composite lighting: `elevation` or `hillshade` |
| `--threads=N` | 0 (all cores) | Worker threads; `1` runs everything serially |
| `--png-level=N` | 8 | PNG deflate effort, 1–9; lower is faster and larger |

Every default above comes from `assets/config.yaml`, not from the binary — see
**Configuration**. At the default `--grid-size=80` the YAML lands around 15 MB.

## Configuration

[`assets/config.yaml`](./assets/config.yaml) holds every knob the generator has: the world
scale, the sampling grid, both noise fields, the terrain and climate curves, rivers, the
road network, settlements, political geography, landmarks, and the twelve pass toggles.

### Scale

Two numbers give the world its size, and make every other measurement in the file mean
something:

```
world extent = grid_size x meters_per_grid_unit = 80 x 60 m = 4.8 km square
render size  = world extent / meters_per_pixel  = 4800 / 1  = 4800 px square
```

`meters_per_grid_unit` is how much ground a cell covers — 60 m, about enough to hold the
ten or so buildings that make a village.

#### Render resolution

`meters_per_pixel` and `image_size` are **one knob with two ends**, and either may be the
one you set:

| Set | In | Effect |
|---|---|---|
| `meters_per_pixel: 1` | `config.yaml` | The default, always. 4.8 km ÷ 1 m → **4800 px** |
| `image_size: 2048` | `config.yaml` | 4.8 km ÷ 2048 px → `meters_per_pixel` becomes 2.34 |
| `--image-size=2048` | CLI | Same, and overrides the file |

Naming either computes the other, so they can never disagree about how much ground a pixel
covers. `image_size` wins if you set both, a pixel count being the more concrete statement
of intent. The invariant is `image_size × meters_per_pixel == grid_size × meters_per_grid_unit`,
and `MapConfig` satisfies it from construction.

**`meters_per_pixel` defaults to 1 and that is deliberate**: a PNG is then a literal
one-pixel-per-metre map, so a pixel count read off a render *is* a measurement — a 6 m road
is 6 px wide because it is 6 m wide. Fix the resolution instead and you fix the pixel count
while the scale floats: at `image_size: 2048`, changing `grid_size` re-scales the ground a
pixel covers rather than resizing the PNG. Which you want depends on whether you are
measuring the map or fitting it somewhere.

Below roughly 4 m/px, thin features stop being reliable — river strokes fragment at 8 m/px
simply because a 7 m river is narrower than a pixel.

Every physical size is therefore in **metres** and carries an `_m` suffix — `road_width_m: 6`,
`building_size_max_m: 14`, `river_width_base_m: 5`. Counts, costs, scores and ratios are
dimensionless and have no suffix. Widths were previously fractions of a grid unit, which
meant nothing on its own: at 60 m to the cell, the old `road_width: 0.20` was a 12-metre
carriageway and the highway an 18-metre one.
It is heavily commented — each key carries its units and what it actually does — and it is
the file to edit rather than a header.

Settings are resolved in four layers, each overriding the one before it:

| | Source | Where |
|---|---|---|
| 1 | `MapConfig`'s in-struct defaults | [`coopa/maps/map_config.h`](./coopa/maps/map_config.h) |
| 2 | the generator's own scene defaults | [`examples/map_generator.cpp`](./examples/map_generator.cpp) |
| 3 | `assets/config.yaml`, or `--config=PATH` | |
| 4 | command-line flags | |

Layer 2 exists so that deleting or moving the config file still produces the 80-cell,
4800-pixel world this README documents instead of silently dropping to the library's much
smaller defaults. It sets no `image_size`: `meters_per_pixel` is 1, and 80 cells of 60 m at
one pixel to the metre *is* 4800 px. (It used to say `image_size = 2048` there, which was
dead — the derivation overwrote it before anything read it, and the tool has always
rendered at 4800.) Layer 4 means trying something never requires editing a version-controlled
file.

Every key is optional and falls back to the layer beneath it, so deleting a line is always
safe, and unknown keys are ignored — an older binary still reads a newer file.

**A missing config.yaml is a warning; a malformed one is fatal.** No file, and the
generator says so on stderr and carries on with layer 2. A file that exists but does not
parse stops the run with the parser's own message and exit code 1 — generating a map that
quietly ignored the settings it was handed is worse than generating none.

Three keys do not behave like the rest, and the file says so inline:

- **`seed`** — `--seed` wins over it. With neither, a seed is drawn from system entropy
  and printed.
- **`noise_island.seed` / `noise_temperature.seed`** — not settable. Both are always
  derived from the master seed, so one `--seed` reproduces the whole map rather than just
  the pass ordering. They appear in the file commented out, with that note.
- **`noise_island.frequency`** — a *reference* value at `grid_size` 40, scaled by
  `40 / grid_size` at startup so `--grid-size` controls detail rather than the size of the
  world. Held fixed, a bigger map comes out as an archipelago of the same small islands.

To load the same settings from C++, `coopa::maps::load_config()` in
[`coopa/maps/map_yaml.h`](./coopa/maps/map_yaml.h) applies a file on top of an existing
`MapConfig` and reports whether it was found, parsed and whether it named a seed.

## Layers

Each run writes one PNG per layer rather than a single composited image, so a consumer can
take the height field without the roads drawn over it, or the road network without the
terrain under it. All eight register pixel for pixel.

| File | Format | Contents |
|---|---|---|
| `_elevation.png` | RGB | Ground height, sea bed included: black at the deepest water, `sea_level` at the shore, white at the summit. Height and nothing else — no rivers cut into it. |
| `_water.png` | RGB | Water-surface height, **on the same scale as `_elevation.png`**. Flat per body: the sea at `sea_level`, each lake at one height across all its cells, rivers at the ground height plus a depth. Dry land is black. |
| `_biomes.png` | RGB | Flat terrain colour, no overlays. |
| `_roads.png` | RGBA | The road network by class, transparent elsewhere. |
| `_structures.png` | RGBA | Building footprints as rotated quads, transparent elsewhere. |
| `_landmarks.png` | RGBA | Settlement and landmark markers, transparent elsewhere. |
| `_regions.png` | RGB | Provinces in flat colour, and nothing over them — the political counterpart of the biome layer, registering with it pixel for pixel. Every pixel is exactly one region's colour or exactly the background, so a consumer can recover which region covers a pixel; countries are not drawn. |
| `_composite.png` | RGB | All of it: biome colour lit from the elevation field, then water, roads, buildings and markers. See **Shading** below. |

The three overlay layers carry real transparency, so they stack over the terrain in any
image editor and reproduce the composite's arrangement.

### Shading

The composite lights its biome colours two ways, chosen by `composite_shading` or
`--shading=MODE`. Both read the elevation field the map already carries, so neither can
disagree with the heightmap layer, and **neither touches the height data** — the elevation
layer is the raw field under either.

| Mode | Brightness follows | Shows |
|---|---|---|
| `elevation` *(default)* | **height** — high ground is pale | *Where* the high ground is. A function of altitude alone, so the same height reads the same everywhere on the map. |
| `hillshade` | **slope**, lit from the north-west | The *shape* of the land — ridgelines, valley walls, which way a face turns. Blind to altitude: a slope at sea level and the same slope on a summit look identical. |

Hillshading costs a second full-resolution pass plus a blur, so the default mode is also
the faster one — about 43 s for the full set against 51 s.

The blur is not optional in hillshade mode. Elevation inside a cell is interpolated from
that cell's own corners, so the surface is continuous across a shared edge but its *slope*
is not — and shading off the raw field draws every cell as its own little dome rather than
drawing terrain.

At the default 4.8 km world and 1 m/px each layer is 4800 × 4800 — about 23 megapixels, and
some 90 MB in memory while it is being written. They are rendered and written one at a
time for that reason; the full set takes under a minute.

## Asynchronous generation

Generation and export both run off the calling thread, on a
`coopa::job::JobEngine` the caller injects. Nothing is owned: mapcoopa *takes* an
engine, so a host that already runs a thread pool shares it rather than competing
with a second one. **With no engine injected everything runs inline** — that is the
default, and it is what the determinism tests compare against.

```cpp
generator.set_job_engine(&engine);          // nullptr, the default, is serial
MapTask task = generator.generate_async();

while (!task.done()) {                      // from the frame loop, never blocking
    draw_loading_bar(task.progress());      // 0..1
}
use(generator.graph());
```

`MapTask` is move-only and **its destructor cancels and waits**. The work writes
into storage the caller owns — the generator's graph, the exporter's images — so a
task outliving what it is filling in would be a use-after-free; blocking in the
destructor makes that unrepresentable, the same bargain `std::jthread` strikes.

`cancel()` is cooperative. Generation checks between passes; export checks before
each layer and between row bands. The one thing neither can interrupt is a PNG
encode already in progress, because stb's deflate is a single opaque call.

Exporting has the same shape:

```cpp
MapExporter exporter;
exporter.set_job_engine(&engine);
MapTask task = exporter.export_layers_async(graph, config, "world");
```

### What it costs, and what it buys

Measured on 20 cores at the default 4.8 km world, 4800 × 4800, seven layers:

| | generate | export | yaml | total |
|---|---|---|---|---|
| Before any of this (`-O0`, serial) | 580 ms | 40.3 s | 1.9 s | **42.8 s** |
| Optimised build, serial | 137 ms | 9.5 s | 563 ms | **10.2 s** |
| Optimised build, 20 threads | 140 ms | 1.3 s | 576 ms | **2.0 s** |

Two findings worth recording, because neither was the one expected:

- **`CMAKE_BUILD_TYPE` was unset**, so every binary this project had ever produced
  was `-O0`. Defaulting it to `Release` for a standalone build is one line and
  worth **4.3×** — more than all the threading put together.
- **Generation was never the problem.** It is 140 ms; the export is 98% of the
  run. Nothing inside generation is parallelised, deliberately: threading 140 ms
  would buy nothing and cost the byte-for-byte guarantee its simplest proof.

Parallel output is **byte-for-byte identical** to serial output — all seven PNGs
and the YAML. Work is split only by disjoint output: across layers, and across
row bands within a layer. Splitting by *cell* would race, because adjacent cells
deliberately share their boundary pixels. For the same reason the road pass stays
serial: each route is routed over ground earlier routes already claimed.

To check it yourself:

```bash
./build/mapcoopa --seed=42 --threads=1 --out=/tmp/serial
./build/mapcoopa --seed=42 --threads=0 --out=/tmp/parallel
cmp /tmp/serial.yaml /tmp/parallel.yaml
for l in elevation water biomes roads structures landmarks composite; do
    cmp /tmp/serial_$l.png /tmp/parallel_$l.png || echo "DIFFERS: $l"
done
```

## Water

`sea_level` (default **0.25**) is a **real height** in the normalised field, not a
convention. The sea bed occupies everything below it and land everything above, so
"this ground is under water" is an honest comparison — and the sea's surface comes
out a visible mid-grey on the same scale as everything else.

It was not always so. With the whole field starting at zero and the sea pinned to the
bottom of it, nothing was ever below sea level, the sea's surface rendered as the same
black as dry land, and a river running into the ocean appeared to run into nothing.

| | |
|---|---|
| `_elevation.png` | the **ground**, sea bed included — genuine bathymetry |
| `_water.png` | the **surface** of whatever water covers it, same scale |

`MapCenter::water_level` carries that surface as data. It is flat per *body*, which
`elevation` is not: the sea at `sea_level` everywhere, each lake at one height across
all of its cells. A consumer floods a terrain mesh to it directly.

Land therefore spans `[sea_level, 1]`, so every threshold that describes *land* — the
Whittaker biome rows, `elevation_penalty_start`, `peak_elevation`, `canyon_elevation` —
is taken through `land_height()` first. Moving the waterline does not silently shift
them.

### Vertical scale

`elevation_range_m` (default **600**) is how many metres the `[0, 1]` field spans — the
vertical counterpart of `meters_per_grid_unit`, which the world previously had no
equivalent of. Sea floor at 0 m, waterline at `sea_level × elevation_range_m`, summit at
the full range. Without it "a river one metre deep" had nowhere to land.

### Rivers and shorelines

- **Every river ends in a water body** — a lake or the sea, never in the middle of a field.
  This took fixing in two independent places, because the requirement has two halves: the
  river has to *arrive* at water, and the map has to *show* it.
  - *Arriving.* A river is a walk down the flow field, so where it ends is decided by the
    height field and not by the river pass: relief noise, the rank remap and the smoothing
    passes each move corners independently, and any of them can leave a corner lower than
    all its neighbours. A walk that reaches such a pit stops on dry land, and 42% of rivers
    used to. The elevation pass now fills the pits (priority flood — see
    [`passes/README.md`](./coopa/maps/passes/README.md)), which leaves *every* land corner
    with a strictly descending path to water; the river pass discards anything that still
    ends dry, so the guarantee holds even if the terrain changes under it.
  - *Showing it.* These layers are coloured by **biome**, and `classify_biome()` used to
    hand a shallow lake `Marsh` — a dark green. 18% of rivers therefore ended in a cell
    that reads as forest, on a map whose data said "lake". Water cells now only ever get
    `Ocean`, `Lake` or `Ice`.
- **`river_depth_m`** (+ `river_depth_per_volume_m`) lifts a river's surface *above* the
  ground it runs over, because a river is water standing in a channel rather than a line
  painted on the terrain. Interpolated along the smoothed centreline, so the fall
  downstream is continuous instead of terracing at every corner.
- **Bodies are drawn after rivers**, so a lake's flat surface wins inside its own
  outline. The other way round, a river stroked at ground height gouged a channel across
  every lake it flowed into.
- **`water_edge_overlap_m`** (default **1**) extends every water surface past its own
  edge so it clips *into* the terrain. Two surfaces sharing an edge exactly will show a
  seam wherever their meshes disagree by a rounding error, and along a coastline they
  always do.

## Landmass shape

Land is confined to a shape centred on the canvas; everything outside it is sea. The
**canvas itself is always `grid_size` square** — the shape is inscribed in it, and the
margin left over becomes open ocean. A smaller shape is a smaller world on the same size
of map, not a smaller image.

| Shape | Dimensions | Flag |
|---|---|---|
| `rectangle` *(default)* | `width_m`, `height_m` | `--shape=rect --shape-size=M --shape-height=M` |
| `circle` | `diameter_m` | `--shape=circle --shape-size=M` |
| `triangle` | `edge_length_m`, equilateral and apex-up | `--shape=triangle --shape-size=M --shape-rot=DEG` |

**Every dimension defaults to 0, meaning "fill the canvas"**, which makes the default a
canvas-spanning rectangle — byte for byte what the generator produced before shapes
existed. `rotation` turns the shape about the centre; only the triangle is asymmetric
enough for it to show.

```bash
cplay --shape=circle   --shape-size=3600
cplay --shape=triangle --shape-size=4000 --shape-rot=30
cplay --shape=rect     --shape-size=3000 --shape-height=1800
```

It is one predicate: `shape_inset()` in
[`coopa/maps/map_config.h`](./coopa/maps/map_config.h) returns how far inside the shape a
point lies, and `border_check_()` flags anything within `border_length` of the edge. The
water pass floods those cells and the elevation pass measures height outward from them, so
coastlines, mountains, regions and roads all follow the shape without any of them knowing
shapes exist.

## Terrain

Height is breadth-first **distance from the coast**, which is what keeps coastlines at sea
level and puts mountains inland. Taken alone, though, it makes the high ground the literal
*medial axis* of the landmass — every summit on a thin ridge running equidistant between the
bays either side, which renders as bright closed loops around dark basins. That reads as
foam, not terrain.

Three knobs, at three scales:

- **`terrain_relief`** (0 to 1, default **0.65**) blends the distance field toward fractal
  noise, so the interior becomes massifs and valleys instead of a skeleton. `--relief=F`.
  A coastal mask keeps the shore the lowest land there is, and land is lifted clear of
  water, so rivers still run off the land into the sea. **0 restores the pure distance
  field** — what Amit Patel's original produced.
- **`elevation_smoothing_iterations`** relaxes the height field — how smooth the *landform*
  is.
- **`terrain_roughness`** (0 to 1, default 0) displaces the *sampled surface* with a much
  finer detail field — how rough the skin over it is. `--roughness=F`.

Blended rather than multiplied, incidentally, because scaling the distance field by noise
cannot reorder it: distances span tens of units and a noise factor spans one, so the ridge
survives however hard it is attenuated.

The roughness lives in the sampler, not in the graph. Cell and corner heights stay exactly
as the passes computed them, so biomes, rivers and roads are still classified on the smooth
control field; only what you get from `MapGraph::elevation_at()` — and therefore the
elevation layer and the composite — roughens. The displacement is scaled by the local
height, so a coastline stays exactly at sea level however high the knob goes.

Separately, `elevation_at()` interpolates barycentrically over the **Delaunay triangle**
containing the sample, blending the three cell-site heights at its vertices — the natural
piecewise-linear surface through samples taken at the sites, and what a terrain mesh built
from this data would be. Two earlier interpolations were worse:

- **Inverse-distance weighting** over a cell's corners read as a plateau: every corner is
  roughly equidistant from the middle of a cell, so the interior came out near the mean of
  the corners.
- **Barycentric over the cell's own corner fan** creased at each of the six-odd internal fan
  edges *and* put a tent pole at every site, so the surface came out visibly crumpled — and
  the fan covers only the *straight* corner polygon while the renderer draws the
  *subdivided* outline, so 1.78% of pixels missed every triangle and fell through to the
  inverse-distance formula, speckling every cell boundary.

The Delaunay triangulation tiles the hull, so there is nothing to fall through, and it
creases once per edge rather than six times per cell.

## Legend

### Biomes

The colours `MapLayers` fills a cell with, from `BiomePalette::biome_colors` in
[`coopa/maps/map_config.h`](./coopa/maps/map_config.h). The **name** column is the
`snake_case` identifier written into the `.yaml` and accepted back by `biome_from_name()`
— that string is a biome's stable on-disk identity, so it is what to key on rather than
the enum's position. The swatches are SVGs under [`assets/svg/`](./assets/svg), one per
palette entry.

Both tables and every swatch are generated — run
[`python3 tools/gen_legend_svg.py`](./tools/gen_legend_svg.py) after changing a palette
colour, and `test_readme_legend_matches_the_palette` fails the build if they ever drift
from `BiomePalette`.

| Colour | Biome | YAML name | Hex | RGB |
|---|---|---|---|---|
| ![](assets/svg/ocean.svg) | Ocean | `ocean` | `#5EB6DF` | 94, 182, 223 |
| ![](assets/svg/lake.svg) | Lake | `lake` | `#5EB6DF` | 94, 182, 223 |
| ![](assets/svg/marsh.svg) | Marsh | `marsh` | `#215E21` | 33, 94, 33 |
| ![](assets/svg/ice.svg) | Ice | `ice` | `#92CEE7` | 146, 206, 231 |
| ![](assets/svg/beach.svg) | Beach | `beach` | `#F5DEB3` | 245, 222, 179 |
| ![](assets/svg/snow.svg) | Snow | `snow` | `#FFFAFA` | 255, 250, 250 |
| ![](assets/svg/tundra.svg) | Tundra | `tundra` | `#A9A9A9` | 169, 169, 169 |
| ![](assets/svg/bare.svg) | Bare | `bare` | `#C9B49B` | 201, 180, 155 |
| ![](assets/svg/scorched.svg) | Scorched | `scorched` | `#99826D` | 153, 130, 109 |
| ![](assets/svg/taiga.svg) | Taiga | `taiga` | `#336600` | 51, 102, 0 |
| ![](assets/svg/shrubland.svg) | Shrubland | `shrubland` | `#808000` | 128, 128, 0 |
| ![](assets/svg/temperate_desert.svg) | Temperate desert | `temperate_desert` | `#EED6AF` | 238, 214, 175 |
| ![](assets/svg/temperate_rain_forest.svg) | Temperate rain forest | `temperate_rain_forest` | `#556B2F` | 85, 107, 47 |
| ![](assets/svg/temperate_deciduous_forest.svg) | Temperate deciduous forest | `temperate_deciduous_forest` | `#228B22` | 34, 139, 34 |
| ![](assets/svg/grassland.svg) | Grassland | `grassland` | `#7CFC00` | 124, 252, 0 |
| ![](assets/svg/tropical_rain_forest.svg) | Tropical rain forest | `tropical_rain_forest` | `#006400` | 0, 100, 0 |
| ![](assets/svg/tropical_seasonal_forest.svg) | Tropical seasonal forest | `tropical_seasonal_forest` | `#6B8E23` | 107, 142, 35 |
| ![](assets/svg/subtropical_desert.svg) | Subtropical desert | `subtropical_desert` | `#FAFAD2` | 250, 250, 210 |
| ![](assets/svg/alpine_meadow.svg) | Alpine meadow | `alpine_meadow` | `#8EBA7C` | 142, 186, 124 |
| ![](assets/svg/glacier.svg) | Glacier | `glacier` | `#DEF1F7` | 222, 241, 247 |
| ![](assets/svg/cold_desert.svg) | Cold desert | `cold_desert` | `#BAB8A0` | 186, 184, 160 |
| ![](assets/svg/steppe.svg) | Steppe | `steppe` | `#B2B66C` | 178, 182, 108 |
| ![](assets/svg/savanna.svg) | Savanna | `savanna` | `#C4BE5A` | 196, 190, 90 |
| ![](assets/svg/chaparral.svg) | Chaparral | `chaparral` | `#96A05C` | 150, 160, 92 |
| ![](assets/svg/moorland.svg) | Moorland | `moorland` | `#7E7460` | 126, 116, 96 |
| ![](assets/svg/boreal_wetland.svg) | Boreal wetland | `boreal_wetland` | `#486E60` | 72, 110, 96 |
| ![](assets/svg/swamp.svg) | Swamp | `swamp` | `#3A5C3E` | 58, 92, 62 |
| ![](assets/svg/mangrove.svg) | Mangrove | `mangrove` | `#2E785A` | 46, 120, 90 |
| ![](assets/svg/cloud_forest.svg) | Cloud forest | `cloud_forest` | `#609676` | 96, 150, 118 |
| ![](assets/svg/badlands.svg) | Badlands | `badlands` | `#B28058` | 178, 128, 88 |
| ![](assets/svg/salt_flat.svg) | Salt flat | `salt_flat` | `#EEEEE6` | 238, 238, 230 |
| ![](assets/svg/dunes.svg) | Dunes | `dunes` | `#E8CE94` | 232, 206, 148 |
| ![](assets/svg/volcanic_field.svg) | Volcanic field | `volcanic_field` | `#5C4A46` | 92, 74, 70 |

`ocean` and `lake` share a colour deliberately — they are the same water to look at, and
what separates them is whether the body reaches the edge of the map, which a reader can
see from the shape rather than the hue. `ice` is a *frozen* body and is coloured as a pale
version of that same blue rather than as a fourth near-white beside `snow`, `glacier` and
`salt_flat`: a river has to visibly end in water, and ending in something the eye files
with snowfields does not count.

Only those three ever colour a water cell. `marsh`, `swamp` and `boreal_wetland` are dry
land — waterlogged basin floor beside the water, not the water itself.

> **These are the untinted, unshaded colours.** Two things move a rendered pixel off the
> table value. At the default `show_regions = true` and `region_tint = 0.13`, every claimed
> land cell is mixed 13% toward its region's colour so that provinces are visible — generate
> with `--no-regions`, or set `MapConfig::show_regions = false`, for exact matches. And the
> **composite** additionally lights every land pixel by its elevation or its slope, so match
> against `_biomes.png` rather than `_composite.png`.

### Overlays

Drawn over the filled cells, in this order — each layer covers the one beneath it.

| Colour | Overlay | Hex | RGB | Shape |
|---|---|---|---|---|
| ![](assets/svg/river.svg) | River | `#5EB6DF` | 94, 182, 223 | 5 m plus 2 m per unit of volume, along a smoothed centreline |
| ![](assets/svg/trail.svg) | Trail | `#887A64` | 136, 122, 100 | 3 m wide; a spur off the network |
| ![](assets/svg/road.svg) | Road | `#7A6552` | 122, 101, 82 | 6 m wide |
| ![](assets/svg/highway.svg) | Highway | `#926C3E` | 146, 108, 62 | 10 m wide; the busiest stretches |
| ![](assets/svg/bridge.svg) | Bridge | `#706C66` | 112, 108, 102 | Stone parapet drawn square across the road |
| ![](assets/svg/building.svg) | Building | `#463228` | 70, 50, 40 | Rotated quad, 7-14 m per side, one per dwelling |
| ![](assets/svg/settlement.svg) | Settlement | `#782828` | 120, 40, 40 | Square marker; 25 m capital, 17 m town, 11 m village |
| ![](assets/svg/landmark-natural.svg) | Natural landmark | `#283C82` | 40, 60, 130 | Diamond; 21 m for a region's wonder, 13 m otherwise |
| ![](assets/svg/landmark-built.svg) | Built landmark | `#5A3C82` | 90, 60, 130 | Square, 13 m |
| ![](assets/svg/background.svg) | Background | `#FFFFFF` | 255, 255, 255 | Whatever no cell covers |

The three road tiers are told apart by **width** first and colour second, which is how a
paper map does it. `RoadClass` is not a label anything chooses: it is read off
`MapEdge::traffic`, the number of routes the road pass sent along that edge, so a highway
is a highway because everything goes that way.

The elevation render has no legend — it is a greyscale heightmap, black at sea level and
white at the highest point, with rivers dimmed 10 values per channel over the terrain
beneath rather than painted over it.

## Running the tests

The suite is a separate target, since the bare project name belongs to the generator:

```bash
cbuild
./build/mapcoopa_tests      # or: ctest --test-dir build
```

69 cases covering determinism, the graph invariants, every pass, both renderers, the
configuration loader, the world scale, the asynchronous API and the YAML round
trip — including that every parallel path reproduces its serial one exactly.

## Consuming it from another repo

`add_subdirectory()` this repo and link `coopa::maps`; the standalone guard means you get
the interface target and nothing else — no extra binaries, no ctest entries.

```cmake
set(MAPCOOPA_DIR "${ROOT_DIR_PARENT}/mapcoopa")
if(NOT TARGET coopa::maps)
    add_subdirectory(${MAPCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/mapcoopa-build)
endif()
target_link_libraries(your_target PRIVATE coopa::maps)
```

`coopa::maps` pulls in `coopa::lib` transitively, so glm and fkYAML come with it. The
delaunator, FastNoiseLite and stb_image_write headers this repo vendors under `includes/`
are on its interface include path too.
