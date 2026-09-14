# Map Generation Passes (`coopa::maps`)

Each header here holds one stage of map generation. A pass is a stateless class with a
single method:

```cpp
void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const;
```

`MapGenerator::execute_passes_()` runs them in the fixed order below, each gated by its
`MapConfig::enable_*` toggle. The order is a dependency chain, not a preference — valleys and moisture
need rivers, biomes need moisture and temperature, regions need biomes, towns need regions
to be named in the right dialect, and landmarks need towns to know which land is already
settled. Turning a pass off leaves the fields it would have written at their defaults
rather than reordering anything.

The graph's geometry is built before any pass runs and none of them change it; a pass only
annotates cells, corners and edges.

That single fixed signature is also why `generate_async()` checks for cancellation
*between* passes rather than inside one: threading a token through thirteen `execute()`
methods would change the contract every pass is written to, to shave at most one pass off
the latency — and the longest pass is roughly 50 ms. Nothing in here runs in parallel
either. Generation is about 140 ms against some 10 s of export, so there is nothing to win,
and the road pass could not be parallelised regardless: each route is deliberately routed
over ground the earlier ones already claimed. Every pass that draws randomness seeds a generator from
`MapConfig::seed`, offset per pass so two passes never share a stream.

---

## Pass Order

| # | File | Writes | Reads |
|---|------|--------|-------|
| 1 | [`pass_water.h`](./pass_water.h) | `water`, `ocean` on corners and cells | the island noise field, the `border` flag |
| 2 | [`pass_coast.h`](./pass_coast.h) | `coast`; refines corner `ocean`/`water` | pass 1 |
| 3 | [`pass_elevation.h`](./pass_elevation.h) | `elevation`, `downslope`, `water_level` | pass 2 |
| 4 | [`pass_temperature.h`](./pass_temperature.h) | `temperature` | pass 3 |
| 5 | [`pass_rivers.h`](./pass_rivers.h) | `river` on corners and edges; `MapGraph::rivers` | pass 3 |
| 6 | [`pass_valleys.h`](./pass_valleys.h) | rewrites `elevation` and `downslope` | pass 5 |
| 7 | [`pass_moisture.h`](./pass_moisture.h) | `moisture` | pass 5 |
| 8 | [`pass_biomes.h`](./pass_biomes.h) | `MapCenter::biome` | passes 4, 6 and 7 |
| 9 | [`pass_roads.h`](./pass_roads.h) | `MapEdge::road`, `road_class`, `traffic`, `bridge`; `MapGraph::roads` | passes 5, 6 and 8 |
| 10 | [`pass_regions.h`](./pass_regions.h) | `MapGraph::regions`, `countries`; cell `region`/`country` | passes 6 and 8 |
| 11 | [`pass_towns.h`](./pass_towns.h) | `MapGraph::towns` | passes 8, 9 and 10 |
| 12 | [`pass_landmarks.h`](./pass_landmarks.h) | `MapGraph::landmarks` | passes 8, 10 and 11 |
| 13 | [`pass_noisy_edges.h`](./pass_noisy_edges.h) | `noisy_points0`, `noisy_points1` | pass 8 |

---

## Notes Per Pass

### 1. Water ([`pass_water.h`](./pass_water.h))
The `border` flag it reads comes from `MapGenerator::border_check_()`, which is now driven
by `MapConfig::shape` — a canvas-spanning rectangle by default, or a circle, triangle,
continent or archipelago inscribed in the canvas. That one predicate (`ShapeField::inset()`)
is where the shape of the world comes from; nothing in here knows shapes exist.

Threshold the island noise field per corner, promote cells whose corner count crosses
`threshold_water_count`, then flood-fill from the border to mark `ocean`. That last step
is the only thing separating the sea from an inland lake — both are `water`, only the sea
reaches the edge of the map.

### 2. Coast ([`pass_coast.h`](./pass_coast.h))
A land cell adjacent to ocean is coast. Corner state then follows from the cells it
touches.

### 3. Elevation ([`pass_elevation.h`](./pass_elevation.h))
Height is breadth-first graph distance from the border, not a second noise field — which
is what makes coastlines land at sea level and pushes mountains into the interior of a
landmass. Crossing between two land corners costs a full unit, every other step 0.01, so
an inland sea does not raise the terrain around it. Heights are then rank-remapped through
`sqrt(k) - sqrt(k(1-y))` so a continent reads as broad plains with a few peaks.

Between those two steps it blends in fractal relief (`terrain_relief`, default 0.65). The
distance field models *how high* land gets well and *where* badly — the maximum of a
distance field is its medial axis, so untouched it puts every summit on a thin ridge
equidistant between the bays either side, which renders as foam. Mixing toward noise
replaces that ordering; scaling by noise cannot, because distances span tens of units while
a noise factor spans one. A coastal mask keeps the shore the lowest land, and land is lifted
clear of water so drainage still runs seaward. Zero restores the pure distance field.

The rank remap is **split**: the sea takes `[0, sea_level)` and everything else
`[sea_level, 1]`. Ranked together — which they were — the two interleave, because a corner
far out to sea accumulates enough hundredths of a step to outrank a coastal one. The sea had
no consistent depth, the shoreline no consistent height, and `elevation` under water meant
nothing at all. Split, depth becomes real bathymetry and the waterline becomes a definite
height. Lakes rank with the *land*: a tarn sits at altitude, and the only thing below sea
level is the sea. Smoothing is likewise confined to each side of the waterline, since
relaxing across it drags the sea onto the shore and the shore under it.

Then it **fills the pits**, which is what makes the drainage a drainage. The distance field
is monotone, but nothing after it is: relief noise, the rank remap and both smoothing passes
move corners independently of their neighbours, and any of them can leave a corner lower
than everything around it. A downhill walk that reaches one stops on dry land — 80 of 11 438
land corners were such pits, and 23 of 55 rivers used to end in the middle of a field. The
fix is the priority-flood fill (Barnes, Lehman & Mulla 2014), what DEM processing uses for
exactly this: seed a min-heap with every corner already at or in water, then pop the lowest
and raise each dry neighbour to just above it. That walks outward from the sea in ascending
order of the height water would need to reach a corner, so every corner is resolved *from* a
strictly lower one and inherits its descending path to water. Lake corners seed the heap
alongside the sea, so an inflow that reaches a lake has arrived and no lake bed is filled in
to force the water over its rim — which is what keeps an endorheic basin a lake. The epsilon
is 1e-7 and chains run to 39 corners, so the worst-case rise is 4e-6 of the range: about two
millimetres at the default 600 m, and not a visible change to the terrain.

`assign_downslopes()` compares strictly (`<`). An equal-height neighbour is not downhill, and
accepting one let two corners at the same elevation name each other as their downslope — a
two-cycle a flow walk follows until its step guard. Strictness is also what makes "points at
itself" mean "has nowhere lower to go" rather than "happened to be scanned last".

Finally it levels the water. `elevation` is the height of the *ground*, which under a water
cell is the bed; `MapCenter::water_level` states the surface, which unlike a bed is flat.
`sea_level` across the whole sea, and one height per lake, found by flooding the
`water && !ocean` cells and taking the highest bed in each body so no basin pokes through
its own surface. It has to happen here rather than in the water pass, which is where sea and
lake are told apart but which runs first and has no heights to level against.

### 4. Temperature ([`pass_temperature.h`](./pass_temperature.h))
Latitude band, minus an altitude lapse rate, plus a noise field. Without it biomes are
classified on elevation and moisture alone, which leaves a third of the table unreachable
and puts deserts at the pole. Latitude runs along **y**, so a map reads as a north-south
slice of a globe.

### 5. Rivers ([`pass_rivers.h`](./pass_rivers.h))
Sample sources uniformly, reject any outside the source elevation band, and walk
`downslope` until the water. The attempt count is bounded: the original retried by
decrementing its loop counter, which hangs outright on a map with no qualifying land.

**Every river ends in a water body**, and that is a guarantee rather than a tendency. The
walk stops on `water || coast` — the union, not `coast` alone, because `PassCoast`
deliberately excludes the shoreline from `water` so the two can be told apart: a sea mouth
is a `coast` corner and a lake mouth a `water` one. Stopping on `coast` alone also ran a
river on down a lake *bed* to its lowest corner, drawing a channel across the surface,
since lake corners sit below their shore. A walk that somehow still ends dry has its river
discarded and logged. With the pits filled that rejection never fires, and it stays anyway:
"always ends in water" is a property callers rely on, and one enforced only by an invariant
two passes away is one a future change to elevation can quietly break.

The walk gathers the corner chain *before* raising any volume, and only commits a
watercourse at least `river_min_length` corners long. Raising volumes as it walked — which
is what this did — makes a short river impossible to reject, because by the time you can
measure it you have already carved it. Most land is near a coast, so without the rejection
the map fills with two-cell trickles. The source floor was raised from 0.3 to 0.45 for the
same reason: below that a "source" can appear on the plain it was meant to run down to.

Each kept watercourse is then traced into a `MapRiver` — the corner chain, and a copy of it
corner-cut by `chaikin_smooth()` ([`../map_data.h`](../map_data.h), shared with the road
pass). Drawn straight between Voronoi corners a river is visibly angular at every corner,
which is the one shape moving water never has; corner-cutting rounds the joints without
straightening the course, because it never moves a point more than a quarter of a segment.
The meander is the downslope chain itself and survives intact.

### 6. Valleys ([`pass_valleys.h`](./pass_valleys.h))
The only pass that rewrites `elevation` after pass 3, and it has to be: rivers erode, but
elevation is computed *before* rivers are routed — it has to be, the routing follows
`downslope` — so without this the height field has no idea a river pass ever ran. The
elevation layer showed no trace of the rivers the water layer is full of.

It cuts a **valley, not a channel**, and the geometry forces that. `elevation_at()`
interpolates *cell-site* heights over Delaunay triangles while rivers run along Voronoi
edges — cell *boundaries* — so lowering corner heights alone would change nothing anyone
can see. The cells have to move, and the narrowest thing they can express is about a cell
across. Which is the right answer anyway: a river sits in a valley far wider than itself,
and the channel within it is what the water layer draws.

Depth per corner from `river_incision_m` plus `river_incision_per_volume_m` × volume,
tapered over the first few corners so a river does not begin with its valley already cut.
Graded outward over `river_valley_width` rings of `adjacent` at `river_valley_falloff`
each — `max`, not `+`, so a confluence is one valley rather than two stacked. Corners are
then lowered, clamped at the waterline, and the descent along each course is restored;
cells take the *mean* of their corners' depths, which applies the valley on top of
`smooth_center_elevations_()` instead of throwing that relaxation away.

It finishes by calling `restore_drainage()` ([`drainage.h`](./drainage.h), shared with
pass 3). Moving the ground owes the map its drainage back: carving digs pits where a
valley wall grades into ground with no outlet, the waterline clamp flattens river mouths
into ties, and `downslope` was read off a field that no longer exists.

Setting `river_incision_m` to 0 reproduces the uncarved height field byte for byte, which
is what the layer test asserts.

### 7. Moisture ([`pass_moisture.h`](./pass_moisture.h))
Lakes and rivers seed the field; the ocean deliberately does not, so a desert can sit
behind a coastal range. Diffuse outward losing a tenth per hop, pin ocean and coast wet,
then rank-normalise so every map spans the full range.

### 8. Biomes ([`pass_biomes.h`](./pass_biomes.h))
Delegates to `classify_biome()` in [`../biome.h`](../biome.h), which branches on
temperature first, then elevation, then moisture — about 33 biomes rather than the
original 18.

**A water cell always gets a water biome** — `Ocean`, `Lake` or `Ice`, and nothing else.
This is not cosmetic. The biome and composite layers are coloured by *biome*, not by
`MapCenter::water`, so the two disagreeing is a defect the reader sees however sound the
data underneath is: `classify_biome()` used to hand a shallow lake `Marsh` (33, 94, 33 —
a dark green) and 18% of rivers ended in what looks exactly like forest. A lake you cannot
see is indistinguishable from no lake, and rivers must visibly end in water.

`Marsh`, `Swamp` and `BorealWetland` are land biomes now, which is what the words mean:
waterlogged basin floor beside the water rather than the water itself. Moisture is seeded
from lakes and rivers, so they land where they belong.

Freezing is on temperature alone. The elevation test that used to also freeze a high lake
double-counted altitude, because `PassTemperature` already applies an altitude lapse rate —
a high lake was frozen twice over and a cold low one not at all.

The thresholds are fractions of the **land** range, because the pass passes
`land_height()`. That rescale is what broke the marsh rule in the first place: `0.1` meant
"below 0.1 absolute" when the waterline sat at 0, and became "the bottom tenth of the land"
once it moved to `sea_level` — which is where lakes sit, since a lake is a basin. A table
test on `classify_biome()` stayed green throughout, because the arguments changed and not
the function.

### 9. Roads ([`pass_roads.h`](./pass_roads.h))
Roads are *routed*, not drawn. Every Delaunay edge gets a travel cost from the ground
either side of it — distance scaled by slope, height and how rough the biome is, plus a
volume-scaled charge to ford or bridge a river and a high but finite one to step into
water. Anchors ("hubs") are picked with `biome_habitability()`, and a least-cost path is
run between **every pair** of them.

Routes are laid one at a time, longest link first, and each sees ground an earlier route
already built on as `RoadConfig::reuse_discount` times as dear. That one rule is what makes
this a network rather than a fan of independent optimal paths: a later route bends to join
an existing road instead of paralleling it a cell away, trunks consolidate, and traffic
concentrates. An edge's `traffic` is the number of routes that chose it, and `RoadClass`
is a pair of thresholds on that count taken as a *share of the routes laid* — a dead-end
spur always carries exactly `hubs - 1` routes, so an absolute cutoff would mean something
different on every map.

Switchbacks survive from the old implementation, but for a reason rather than by
construction: `slope_cost` makes climbing straight up dear and traversing a slope cheap, so
a mountain route crosses the contour at a shallow angle and doubles back. Bridges need no
geometry — a road follows the Delaunay edge `d0`–`d1` and a river the dual Voronoi edge
`v0`–`v1`, and those are the same `MapEdge`, so an edge carrying both *is* the crossing.
Finally each maximal same-class chain is traced into a `MapRoad` and corner-cut with
Chaikin, ends pinned, because a path drawn straight between cell sites is visibly faceted
and no road is.

Water is crossable but bounded: a causeway may span up to `max_water_span` consecutive
water cells, so nearby islands link up while the open ocean stays impassable and a remote
island keeps its own self-contained network. The search state is therefore
`(cell, consecutive water crossed)` rather than just the cell — the same lake cell is
reachable one hop from shore and unreachable three hops out.

**What this replaces.** The pass used to flood four elevation bands outward from the coast
and flag any edge whose two corners fell in different bands. That traces contour lines, and
contour lines connect nothing: a road could run half the map without passing a settlement,
every road was the same width, and `TownConfig::road_bonus` was rewarding proximity to a
contour rather than to a trade route.

**Why it runs before towns.** Towns want roads to score sites by and good roads want towns
to connect, which looks circular. It is not: the pass picks its own hubs with the same
`biome_habitability()` table `PassTowns` ranks sites with, so it depends only on biomes and
rivers, and the settlements placed two passes later land on the network because both
passes read the same ground. The pass draws no randomness at all — terrain decides
everything, so there is no stream to seed.

### 10. Regions ([`pass_regions.h`](./pass_regions.h))
Scatter country seeds across the land, then claim territory by multi-source Dijkstra where
climbing is expensive and crossing water more so — which is why the borders that emerge
follow ridgelines and coasts instead of cutting across them. Each country is subdivided the
same way, and any land the fill could not reach is adopted by its nearest claimant so no
cell is left stateless. Countries draw a synthetic language; regions get a dialect of it.

### 11. Towns ([`pass_towns.h`](./pass_towns.h))
Score land cells on biome habitability (`biome_habitability()` in
[`../biome.h`](../biome.h), the same table the road pass ranks hubs with), low ground, and
access to sea, river or road;
accept the best greedily subject to a minimum separation so settlements spread across a
continent rather than clustering on one river mouth; then rank them into capitals, towns
and villages.

A settlement is **not one cell**. At 60 m to the grid unit a cell is about 3,600 m², which
holds roughly ten 10 m buildings at a believable density — so a capital confined to its own
cell is a hamlet and the three tiers stop being distinguishable. Each settlement claims
cells breadth-first over `MapCenter::neighbors`, taking dry habitable land no other
settlement holds: seven for a capital, three for a town, one for a village. Breadth-first
and not a radius, so the claimed patch is contiguous and a coastal town grows along its
shore instead of reaching across the water. The packer then runs per claimed cell, carrying
the growing building list forward so `buildings_overlap()` still rejects a footprint that
would cross a boundary into ground already built on.

Each accepted cell is then laid out. The roads and rivers bordering it become *streets*
running from the cell's site out to those edges — a road is drawn along the Delaunay edge,
so a road-flagged border means one enters the cell and heads for its centre. Buildings are
placed in pairs flanking each street, walking outward, jittered in position and yaw so the
rows read as built over time rather than surveyed. Whatever budget remains is spent on
rejection sampling across the cell, filling the interior without falling back into a grid.
A cell with no road or river gets fallback lanes toward its farthest corners.

Every candidate must have all four of its **rotated** corners inside the cell polygon and
must clear every building already placed, by separating-axis test. That is what makes
`MapBuilding::rotation` a pose a consumer can trust.

Names are drawn from the region's dialect and then *claimed*: a name already taken is
redrawn, up to a bound, after which an ordinal is appended. Two settlements of one region
draw from the same small phoneme table, so a collision is a matter of how many towns that
region got — and two places sharing a name silently conflates them for anything keying on
one.

This pass was a stub in the original — it logged its own name and returned, with a
commented-out sketch of the packing step referencing types that never existed. The scoring,
placement and layout are new; the containment test follows that sketch's ray-cast approach.

### 12. Landmarks ([`pass_landmarks.h`](./pass_landmarks.h))
Natural features are *read off* the terrain rather than sprinkled onto it — a peak is a cell
higher than all its neighbours, a waterfall a river edge with a real drop across it — so they
always agree with the map they sit on. Ruins go where people could live but do not, giving
the world a past. Every kind is checked against `landmark_suits_biome()`, and no single kind
may take more than a fifth of the budget: ranking alone lets whichever signature happens to
be commonest swallow every slot, and the result is forty hot springs and no coastline.

### 13. Noisy Edges ([`pass_noisy_edges.h`](./pass_noisy_edges.h))
Redraw each cell boundary as a path that wanders inside the quadrilateral formed by the
Voronoi edge and the two cell sites, recursively, to a depth of three. Both neighbouring
cells read the same edge, so they can never disagree about where their shared border runs.
Cost is paid only where it shows: open ocean is never subdivided, same-biome interiors only
if long, coastlines and river banks always.

Controlled by `MapConfig::subdivide_noisy_edges` — off reproduces the original's
straight-edged output, which shipped with the subdivision call commented out.
