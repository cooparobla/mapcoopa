# Map Generation Passes (`coopa::maps`)

Each header here holds one stage of map generation. A pass is a stateless class with a
single method:

```cpp
void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const;
```

`MapGenerator::execute_passes_()` runs them in the fixed order below, each gated by its
`MapConfig::enable_*` toggle. The order is a dependency chain, not a preference — moisture
needs rivers, biomes need moisture and temperature, regions need biomes, towns need regions
to be named in the right dialect, and landmarks need towns to know which land is already
settled. Turning a pass off leaves the fields it would have written at their defaults
rather than reordering anything.

The graph's geometry is built before any pass runs and none of them change it; a pass only
annotates cells, corners and edges. Every pass that draws randomness seeds a generator from
`MapConfig::seed`, offset per pass so two passes never share a stream.

---

## Pass Order

| # | File | Writes | Reads |
|---|------|--------|-------|
| 1 | [`pass_water.h`](./pass_water.h) | `water`, `ocean` on corners and cells | the island noise field |
| 2 | [`pass_coast.h`](./pass_coast.h) | `coast`; refines corner `ocean`/`water` | pass 1 |
| 3 | [`pass_elevation.h`](./pass_elevation.h) | `elevation`, `downslope` | pass 2 |
| 4 | [`pass_temperature.h`](./pass_temperature.h) | `temperature` | pass 3 |
| 5 | [`pass_rivers.h`](./pass_rivers.h) | `river` on corners and edges | pass 3 |
| 6 | [`pass_moisture.h`](./pass_moisture.h) | `moisture` | pass 5 |
| 7 | [`pass_biomes.h`](./pass_biomes.h) | `MapCenter::biome` | passes 3, 4 and 6 |
| 8 | [`pass_roads.h`](./pass_roads.h) | `MapEdge::road` | pass 3 |
| 9 | [`pass_regions.h`](./pass_regions.h) | `MapGraph::regions`, `countries`; cell `region`/`country` | passes 3 and 7 |
| 10 | [`pass_towns.h`](./pass_towns.h) | `MapGraph::towns` | passes 7, 8 and 9 |
| 11 | [`pass_landmarks.h`](./pass_landmarks.h) | `MapGraph::landmarks` | passes 7, 9 and 10 |
| 12 | [`pass_noisy_edges.h`](./pass_noisy_edges.h) | `noisy_points0`, `noisy_points1` | pass 7 |

---

## Notes Per Pass

### 1. Water ([`pass_water.h`](./pass_water.h))
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

### 4. Temperature ([`pass_temperature.h`](./pass_temperature.h))
Latitude band, minus an altitude lapse rate, plus a noise field. Without it biomes are
classified on elevation and moisture alone, which leaves a third of the table unreachable
and puts deserts at the pole. Latitude runs along **y**, so a map reads as a north-south
slice of a globe.

### 5. Rivers ([`pass_rivers.h`](./pass_rivers.h))
Sample sources uniformly, reject any outside a middling elevation band, and walk
`downslope` to the coast. The attempt count is bounded: the original retried by
decrementing its loop counter, which hangs outright on a map with no qualifying land.

### 6. Moisture ([`pass_moisture.h`](./pass_moisture.h))
Lakes and rivers seed the field; the ocean deliberately does not, so a desert can sit
behind a coastal range. Diffuse outward losing a tenth per hop, pin ocean and coast wet,
then rank-normalise so every map spans the full range.

### 7. Biomes ([`pass_biomes.h`](./pass_biomes.h))
Delegates to `classify_biome()` in [`../biome.h`](../biome.h), which branches on
temperature first, then elevation, then moisture — about 33 biomes rather than the
original 18.

### 8. Roads ([`pass_roads.h`](./pass_roads.h))
Flood elevation-band contour levels outward from the coast; a road runs along any edge
whose two corners fall in different bands. Roads therefore trace band boundaries, which is
why they read as switchbacks climbing a slope.

### 9. Regions ([`pass_regions.h`](./pass_regions.h))
Scatter country seeds across the land, then claim territory by multi-source Dijkstra where
climbing is expensive and crossing water more so — which is why the borders that emerge
follow ridgelines and coasts instead of cutting across them. Each country is subdivided the
same way, and any land the fill could not reach is adopted by its nearest claimant so no
cell is left stateless. Countries draw a synthetic language; regions get a dialect of it.

### 10. Towns ([`pass_towns.h`](./pass_towns.h))
Score land cells on biome habitability, low ground, and access to sea, river or road;
accept the best greedily subject to a minimum separation so settlements spread across a
continent rather than clustering on one river mouth; then rank them into capitals, towns
and villages.

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

This pass was a stub in the original — it logged its own name and returned, with a
commented-out sketch of the packing step referencing types that never existed. The scoring,
placement and layout are new; the containment test follows that sketch's ray-cast approach.

### 11. Landmarks ([`pass_landmarks.h`](./pass_landmarks.h))
Natural features are *read off* the terrain rather than sprinkled onto it — a peak is a cell
higher than all its neighbours, a waterfall a river edge with a real drop across it — so they
always agree with the map they sit on. Ruins go where people could live but do not, giving
the world a past. Every kind is checked against `landmark_suits_biome()`, and no single kind
may take more than a fifth of the budget: ranking alone lets whichever signature happens to
be commonest swallow every slot, and the result is forty hot springs and no coastline.

### 12. Noisy Edges ([`pass_noisy_edges.h`](./pass_noisy_edges.h))
Redraw each cell boundary as a path that wanders inside the quadrilateral formed by the
Voronoi edge and the two cell sites, recursively, to a depth of three. Both neighbouring
cells read the same edge, so they can never disagree about where their shared border runs.
Cost is paid only where it shows: open ocean is never subdivided, same-biome interiors only
if long, coastlines and river banks always.

Controlled by `MapConfig::subdivide_noisy_edges` — off reproduces the original's
straight-edged output, which shipped with the subdivision call commented out.
