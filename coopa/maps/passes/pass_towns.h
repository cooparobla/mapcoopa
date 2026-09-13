/**
 * @file pass_towns.h
 * @brief Eighth pass: scores every land cell for habitability, places
 *        settlements on the best sites, and packs buildings inside each one.
 */

#ifndef COOPA_MAPS_PASSES_PASS_TOWNS_H
#define COOPA_MAPS_PASSES_PASS_TOWNS_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/maps/biome.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/name_generator.h>

namespace coopa {
namespace maps {

/**
 * @class PassTowns
 * @brief Fills `MapGraph::towns`.
 *
 * Sites are scored on where people actually settle: a habitable biome, low
 * ground, and access to the sea, a river or a road. The best-scoring cells are
 * then accepted greedily subject to a minimum separation, so settlements
 * spread across a continent instead of clustering on the single best river
 * mouth. Buildings are packed into the accepted cell by scanning a grid and
 * keeping any footprint that lies wholly inside the polygon.
 *
 * The original left this pass as a stub that logged its own name, with a
 * commented-out sketch of the packing step referencing types that never
 * existed. The scoring and placement here are new; the packing follows that
 * sketch's ray-cast containment test.
 */
class PassTowns {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to populate; requires `PassBiomes` and `PassRoads` to have run.
     * @param config Supplies the seed and the `TownConfig` block.
     * @param logger Receives progress and the settlement count placed.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: towns");

        graph.towns.clear();
        dialects_.clear();
        taken_names_.clear();
        const TownConfig& towns = config.towns;
        if (towns.town_count <= 0 || graph.centers.empty()) {
            return;
        }

        // Offset from the master seed so town placement varies independently
        // of the river sources, which draw from the same value.
        seed_ = config.seed;
        std::mt19937 rng(static_cast<std::mt19937::result_type>(config.seed) + k_seed_offset);
        std::uniform_real_distribution<double> jitter(-towns.score_jitter, towns.score_jitter);

        const double mean_area = mean_land_area_(graph);
        std::vector<CenterId> candidates;
        std::vector<double> scores(graph.centers.size(), 0.0);
        for (const MapCenter& center : graph.centers) {
            if (center.water || center.ocean || center.border) {
                continue;
            }
            const double score = score_site_(graph, center, towns, mean_area) + jitter(rng);
            if (score <= 0.0) {
                continue;
            }
            scores[static_cast<std::size_t>(center.index)] = score;
            candidates.push_back(center.index);
        }

        std::sort(candidates.begin(), candidates.end(), [&scores](CenterId a, CenterId b) {
            return scores[static_cast<std::size_t>(a)] > scores[static_cast<std::size_t>(b)];
        });

        const double min_spacing_squared = towns.min_spacing * towns.min_spacing;
        for (const CenterId candidate : candidates) {
            if (static_cast<int>(graph.towns.size()) >= towns.town_count) {
                break;
            }
            const MapCenter& center = graph.centers[static_cast<std::size_t>(candidate)];
            if (!is_clear_of_existing_(graph, center.point, min_spacing_squared)) {
                continue;
            }

            MapTown town;
            town.center = center.index;
            town.point = center.point;
            town.region = center.region;
            town.score = scores[static_cast<std::size_t>(candidate)];
            town.tier = tier_for_rank_(static_cast<int>(graph.towns.size()), towns);
            town.buildings = pack_buildings_(graph, center, config, town.tier, rng);
            assign_statistics_(town, center, towns, rng);
            town.name = name_for_(graph, town, rng);
            graph.towns.push_back(std::move(town));
        }

        roll_up_populations_(graph);

        int total_population = 0;
        for (const MapTown& town : graph.towns) {
            total_population += town.population;
        }
        logger.info("map pass: towns placed " + std::to_string(graph.towns.size())
                    + " of " + std::to_string(towns.town_count) + " requested, population "
                    + std::to_string(total_population));
    }

private:
    /** @brief Added to `MapConfig::seed` so towns and rivers do not share a stream. */
    static constexpr unsigned int k_seed_offset = 7919u;

    /** @brief Scores one cell on biome, elevation and its access to water and roads. */
    double score_site_(const MapGraph& graph, const MapCenter& center, const TownConfig& towns,
                       double mean_area) const {
        double score = biome_habitability(center.biome);
        if (score <= 0.0) {
            return 0.0;
        }

        if (center.coast) {
            score += towns.coast_bonus;
        }

        bool has_river = false;
        bool has_road = false;
        for (const EdgeId edge_id : center.borders) {
            const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
            if (edge.river > 0) has_river = true;
            if (edge.road) has_road = true;
        }
        if (has_river) score += towns.river_bonus;
        if (has_road) score += towns.road_bonus;

        if (center.elevation > towns.elevation_penalty_start) {
            score -= (center.elevation - towns.elevation_penalty_start) * towns.elevation_penalty_scale;
        }

        // Room to grow. A settlement can hold no more buildings than its cell
        // has space for, so ranking sites without regard to area produces
        // capitals smaller than the towns below them.
        if (mean_area > 0.0) {
            const double relative = std::clamp(graph.cell_area(center) / mean_area, 0.0, 2.0);
            score += towns.area_bonus * (relative - 1.0);
        }
        return score;
    }

    /** @brief Mean cell area across dry land, used to normalise the area bonus. */
    static double mean_land_area_(const MapGraph& graph) {
        double total = 0.0;
        std::size_t count = 0;
        for (const MapCenter& center : graph.centers) {
            if (center.water || center.border) {
                continue;
            }
            total += graph.cell_area(center);
            ++count;
        }
        return count == 0 ? 0.0 : total / static_cast<double>(count);
    }

    /** @brief True when `point` is at least the minimum spacing from every placed town. */
    bool is_clear_of_existing_(const MapGraph& graph, const MapPoint& point,
                               double min_spacing_squared) const {
        for (const MapTown& town : graph.towns) {
            const double dx = town.point.x - point.x;
            const double dy = town.point.y - point.y;
            if (dx * dx + dy * dy < min_spacing_squared) {
                return false;
            }
        }
        return true;
    }

    /** @brief Awards a size class by placement rank -- capitals first, villages last. */
    TownTier tier_for_rank_(int rank, const TownConfig& towns) const {
        if (rank < towns.capital_count) {
            return TownTier::Capital;
        }
        if (rank < towns.capital_count + towns.town_tier_count) {
            return TownTier::Town;
        }
        return TownTier::Village;
    }

    /**
     * @struct Street
     * @brief A line a settlement's buildings front onto, in grid units.
     */
    struct Street {
        MapPoint from;      /**< @brief Inner end, at the cell's site. */
        MapPoint to;        /**< @brief Outer end, where the street leaves the cell. */
        double bearing = 0.0; /**< @brief Direction from `from` to `to`, in radians. */
        double length = 0.0;  /**< @brief Distance between the ends. */
        /**
         * @brief Extra setback before the first plot, in grid units.
         *
         * A river's half-width, so plots line the bank rather than the channel.
         * Zero for a road, which buildings may front directly.
         */
        double clearance = 0.0;
    };

    /**
     * @brief Derives the streets running through a settlement's cell.
     *
     * A road is drawn along the Delaunay edge -- from one cell's site to its
     * neighbour's -- so an edge flagged `road` means a road enters this cell and
     * heads for its centre. The same goes for a river bank. Those approaches are
     * the streets, and buildings line them.
     *
     * A cell with neither still needs structure, or its layout degenerates into
     * scattered noise, so it falls back to lanes running from the site toward
     * its farthest corners.
     *
     * @param graph The map being generated.
     * @param center The settlement's cell.
     * @return The streets, innermost end first.
     */
    std::vector<Street> derive_streets_(const MapGraph& graph, const MapCenter& center,
                                        const MapConfig& config) const {
        std::vector<Street> streets;

        for (const EdgeId edge_id : center.borders) {
            const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
            if (!edge.road && edge.river <= 0) {
                continue;
            }
            Street street = make_street_(center.point, edge.midpoint);
            if (edge.river > 0) {
                // Set the frontage back past the water's edge. Without this the
                // packer aims plots at the channel and every one is rejected,
                // losing the riverside frontage that made the site desirable.
                street.clearance = river_width(config, edge.river) * 0.5
                                 + config.towns.water_clearance;
            }
            streets.push_back(street);
        }

        if (streets.empty()) {
            std::vector<CornerId> by_distance = center.corners;
            std::sort(by_distance.begin(), by_distance.end(),
                      [&graph, &center](CornerId a, CornerId b) {
                          return center.point.distance_to(graph.corners[static_cast<std::size_t>(a)].point)
                               > center.point.distance_to(graph.corners[static_cast<std::size_t>(b)].point);
                      });
            const std::size_t wanted = std::min<std::size_t>(k_fallback_streets, by_distance.size());
            for (std::size_t i = 0; i < wanted; ++i) {
                streets.push_back(make_street_(
                    center.point, graph.corners[static_cast<std::size_t>(by_distance[i])].point));
            }
        }

        return streets;
    }

    /** @brief Builds a street between two points, precomputing its bearing and length. */
    static Street make_street_(const MapPoint& from, const MapPoint& to) {
        Street street;
        street.from = from;
        street.to = to;
        street.bearing = std::atan2(to.y - from.y, to.x - from.x);
        street.length = from.distance_to(to);
        return street;
    }

    /**
     * @brief Lays a settlement out: buildings fronting its streets, then infill.
     *
     * Plots are placed in pairs flanking each street, walking outward from the
     * cell's site, each jittered in position and yaw so the rows read as built
     * over time rather than surveyed. Whatever budget survives that is spent on
     * rejection sampling across the cell, which fills the interior without
     * falling back into a grid.
     *
     * Every candidate is accepted only if all four of its *rotated* corners lie
     * inside the cell and it clears every building already placed. Testing the
     * rotated footprint is what makes `MapBuilding::rotation` a pose a consumer
     * can trust; the previous implementation tested an axis-aligned box and then
     * assigned a random yaw afterwards, so a building drawn at its stated angle
     * could poke outside the cell or into its neighbour.
     *
     * The polygon comes from `MapCenter::corners` rather than the edges' noisy
     * paths: this pass runs before the noisy-edge pass, so those paths do not
     * exist yet. A footprint that fits the straight-edged cell also fits the
     * wobbled one, since subdivision only pushes boundaries outward from the
     * midpoint by a bounded fraction.
     *
     * @param graph The map being generated.
     * @param center The settlement's cell.
     * @param towns Layout parameters.
     * @param rng The pass's seeded generator; the sole source of randomness here.
     * @return The placed buildings.
     */
    std::vector<MapBuilding> pack_buildings_(const MapGraph& graph, const MapCenter& center,
                                             const MapConfig& config, TownTier tier,
                                             std::mt19937& rng) const {
        const TownConfig& towns = config.towns;
        std::vector<MapBuilding> buildings;
        if (center.corners.size() < 3 || towns.buildings_per_town <= 0) {
            return buildings;
        }

        std::vector<MapPoint> polygon;
        polygon.reserve(center.corners.size());
        for (const CornerId corner_id : center.corners) {
            polygon.push_back(graph.corners[static_cast<std::size_t>(corner_id)].point);
        }

        const WaterKeepOut keep_out = collect_water_(graph, center, config);
        const int budget = building_budget_(tier, towns);

        place_street_frontage_(polygon, derive_streets_(graph, center, config), towns, keep_out,
                               budget, rng, buildings);
        place_infill_(polygon, towns, keep_out, budget, rng, buildings);
        return buildings;
    }

    /**
     * @brief Names a settlement in the dialect of the region it stands in.
     *
     * Each region's language is rebuilt here from the same seed the region pass
     * used, rather than carried on `MapRegion`: a language is generator state,
     * not map data, and serialising one would put phoneme tables in every saved
     * world for no gain -- the names themselves are what a consumer needs.
     *
     * @param graph The map, for the region list.
     * @param town The settlement being named.
     * @param rng The pass's seeded generator.
     * @return A generated place name, or a fallback for an unclaimed site.
     */
    std::string name_for_(const MapGraph& graph, const MapTown& town, std::mt19937& rng) const {
        if (town.region == k_invalid_id
            || static_cast<std::size_t>(town.region) >= graph.regions.size()) {
            // Land outside any nation still gets a name, just not a local one.
            Language stateless = make_language(rng);
            return claim_name_(generate_name(stateless, rng), stateless, rng);
        }

        auto found = dialects_.find(town.region);
        if (found == dialects_.end()) {
            const MapRegion& region = graph.regions[static_cast<std::size_t>(town.region)];
            found = dialects_.emplace(
                town.region,
                dialect_for(country_language_seed(seed_, region.country),
                            region_dialect_seed(seed_, region.index))).first;
        }
        return claim_name_(generate_name(found->second, rng), found->second, rng);
    }

    /**
     * @brief Takes a drawn name if it is free, otherwise redraws until one is.
     *
     * Two settlements of one region draw from the same small phoneme table, so a
     * collision is not rare -- it is a matter of how many towns that region got.
     * Nothing here used to check, and two places on the same map could share a
     * name, which is worse than a slightly duller name: a consumer keying on a
     * place name would silently conflate them.
     *
     * The last resort is a numeric suffix rather than another draw, because only
     * that terminates: a dialect has a finite vocabulary and a region crowded
     * enough can exhaust it, and a loop that draws until it gets lucky would not
     * come back.
     *
     * @param drawn The first candidate, already generated.
     * @param dialect The language to redraw from.
     * @param rng The pass's seeded generator.
     * @return A name no other settlement on this map holds.
     */
    std::string claim_name_(std::string drawn, const Language& dialect, std::mt19937& rng) const {
        for (int attempt = 0; attempt < k_name_attempts; ++attempt) {
            if (taken_names_.insert(drawn).second) {
                return drawn;
            }
            drawn = generate_name(dialect, rng);
        }
        for (int ordinal = 2;; ++ordinal) {
            std::string suffixed = drawn + " " + std::to_string(ordinal);
            if (taken_names_.insert(suffixed).second) {
                return suffixed;
            }
        }
    }

    /** @brief Sums settlement populations into their regions and countries. */
    void roll_up_populations_(MapGraph& graph) const {
        for (MapRegion& region : graph.regions) {
            region.population = 0;
            region.capital = k_invalid_id;
        }
        for (MapCountry& country : graph.countries) {
            country.population = 0;
            country.capital = k_invalid_id;
        }

        for (const MapTown& town : graph.towns) {
            if (town.region == k_invalid_id
                || static_cast<std::size_t>(town.region) >= graph.regions.size()) {
                continue;
            }
            MapRegion& region = graph.regions[static_cast<std::size_t>(town.region)];
            region.population += town.population;

            // The region's seat is its largest settlement, not its first.
            if (region.capital == k_invalid_id
                || town.population > population_at_(graph, region.capital)) {
                region.capital = town.center;
            }
        }

        for (const MapRegion& region : graph.regions) {
            if (region.country == k_invalid_id
                || static_cast<std::size_t>(region.country) >= graph.countries.size()) {
                continue;
            }
            MapCountry& country = graph.countries[static_cast<std::size_t>(region.country)];
            country.population += region.population;
            if (region.capital != k_invalid_id
                && (country.capital == k_invalid_id
                    || population_at_(graph, region.capital)
                           > population_at_(graph, country.capital))) {
                country.capital = region.capital;
            }
        }
    }

    /** @brief Population of the settlement occupying a cell, or 0 if none does. */
    static int population_at_(const MapGraph& graph, CenterId cell) {
        for (const MapTown& town : graph.towns) {
            if (town.center == cell) {
                return town.population;
            }
        }
        return 0;
    }

    /** @brief Redraws allowed before a name is disambiguated by ordinal instead. */
    static constexpr int k_name_attempts = 64;

    /** @brief Cached dialect per region, built on first use. */
    mutable std::unordered_map<RegionId, Language> dialects_;
    /** @brief Every settlement name already handed out, so no two places share one. */
    mutable std::unordered_set<std::string> taken_names_;
    /** @brief The map seed, so a region's dialect can be rebuilt from its id. */
    mutable int seed_ = 0;

    /**
     * @brief Derives a settlement's population and prosperity from what it is made of.
     *
     * Population is counted, not invented: every dwelling houses a drawn number
     * of occupants, scaled by how densely the tier builds and by how well the
     * surrounding land feeds them. A settlement's headcount and its footprint
     * therefore cannot drift apart -- add buildings and the population follows.
     *
     * @param town The settlement to annotate; its buildings must already be packed.
     * @param center The cell it occupies, for biome habitability.
     * @param towns Household size and density parameters.
     * @param rng The pass's seeded generator.
     */
    void assign_statistics_(MapTown& town, const MapCenter& center, const TownConfig& towns,
                            std::mt19937& rng) const {
        town.households = static_cast<int>(town.buildings.size());
        if (town.households == 0) {
            town.population = 0;
            town.prosperity = 0.0;
            return;
        }

        const double density = tier_density_(town.tier, towns);
        // Fertile ground supports fuller houses; the same hamlet on scorched
        // rock holds fewer people than one on grassland.
        const double land = 0.7 + 0.3 * biome_habitability(center.biome);

        std::uniform_int_distribution<int> occupants(towns.household_size_min,
                                                     towns.household_size_max);
        double total = 0.0;
        for (int household = 0; household < town.households; ++household) {
            total += occupants(rng) * density * land;
        }
        town.population = std::max(1, static_cast<int>(total));
        town.prosperity = std::clamp(town.score * 0.5 * density, 0.0, 1.0);
    }

    /** @brief Population density multiplier for a settlement's size class. */
    static double tier_density_(TownTier tier, const TownConfig& towns) {
        switch (tier) {
            case TownTier::Capital: return towns.capital_density;
            case TownTier::Town:    return towns.town_density;
            case TownTier::Village: return 1.0;
        }
        return 1.0;
    }

    /**
     * @struct WaterKeepOut
     * @brief The water a settlement's buildings must stay clear of.
     *
     * Segments are the cell's own bounding edges: a river runs *along* a cell
     * boundary, and a lake or sea sits on the far side of one, so both hazards
     * are edges of the polygon the buildings sit inside. Containment alone
     * therefore cannot catch them.
     */
    struct WaterKeepOut {
        /** @brief Segment endpoints paired with the clearance required from each. */
        std::vector<std::array<MapPoint, 2>> segments;
        std::vector<double> clearances;
    };

    /** @brief Gathers the river and shoreline edges bounding a cell, with their clearances. */
    WaterKeepOut collect_water_(const MapGraph& graph, const MapCenter& center,
                                const MapConfig& config) const {
        WaterKeepOut keep_out;
        for (const EdgeId edge_id : center.borders) {
            const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
            if (edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
                continue;
            }

            double clearance = 0.0;
            if (edge.river > 0) {
                clearance = river_width(config, edge.river) * 0.5 + config.towns.water_clearance;
            } else {
                const CenterId other = edge.d0 == center.index ? edge.d1 : edge.d0;
                if (other != k_invalid_id
                    && graph.centers[static_cast<std::size_t>(other)].water) {
                    clearance = config.towns.water_clearance;
                }
            }
            if (clearance <= 0.0) {
                continue;
            }

            keep_out.segments.push_back({graph.corners[static_cast<std::size_t>(edge.v0)].point,
                                         graph.corners[static_cast<std::size_t>(edge.v1)].point});
            keep_out.clearances.push_back(clearance);
        }
        return keep_out;
    }

    /** @brief How many buildings a settlement of this tier may hold. */
    static int building_budget_(TownTier tier, const TownConfig& towns) {
        switch (tier) {
            case TownTier::Capital: return towns.buildings_per_town;
            case TownTier::Town:
                return static_cast<int>(towns.buildings_per_town * towns.town_building_scale);
            case TownTier::Village:
                return static_cast<int>(towns.buildings_per_town * towns.village_building_scale);
        }
        return towns.buildings_per_town;
    }

    /** @brief Places plots in pairs flanking each street, walking outward from the site. */
    void place_street_frontage_(const std::vector<MapPoint>& polygon,
                                const std::vector<Street>& streets, const TownConfig& towns,
                                const WaterKeepOut& keep_out, int budget, std::mt19937& rng,
                                std::vector<MapBuilding>& buildings) const {
        if (streets.empty() || towns.street_spacing <= 0.0) {
            return;
        }

        std::uniform_real_distribution<double> offset_jitter(-towns.position_jitter,
                                                             towns.position_jitter);
        std::uniform_real_distribution<double> yaw_jitter(-towns.rotation_jitter,
                                                          towns.rotation_jitter);
        std::uniform_real_distribution<double> pick_size(towns.building_size_min,
                                                         towns.building_size_max);

        for (const Street& street : streets) {
            const double along_x = std::cos(street.bearing);
            const double along_y = std::sin(street.bearing);
            const double across_x = -along_y;
            const double across_y = along_x;
            const double offset = towns.street_offset + street.clearance;

            for (double t = towns.street_spacing; t < street.length; t += towns.street_spacing) {
                for (const double side : {-1.0, 1.0}) {
                    if (static_cast<int>(buildings.size()) >= budget) {
                        return;
                    }
                    const double size = pick_size(rng);
                    MapBuilding candidate;
                    candidate.width = size;
                    candidate.height = size;
                    candidate.rotation = street.bearing + yaw_jitter(rng);
                    candidate.point = {
                        street.from.x + along_x * t + across_x * side * offset
                            + offset_jitter(rng),
                        street.from.y + along_y * t + across_y * side * offset
                            + offset_jitter(rng)};

                    if (can_place_(polygon, buildings, keep_out, candidate)) {
                        buildings.push_back(candidate);
                    }
                }
            }
        }
    }

    /** @brief Spends the remaining budget on jittered rejection sampling inside the cell. */
    void place_infill_(const std::vector<MapPoint>& polygon, const TownConfig& towns,
                       const WaterKeepOut& keep_out, int budget, std::mt19937& rng,
                       std::vector<MapBuilding>& buildings) const {
        if (static_cast<int>(buildings.size()) >= budget) {
            return;
        }

        double min_x = polygon.front().x, max_x = polygon.front().x;
        double min_y = polygon.front().y, max_y = polygon.front().y;
        for (const MapPoint& point : polygon) {
            min_x = std::min(min_x, point.x);
            max_x = std::max(max_x, point.x);
            min_y = std::min(min_y, point.y);
            max_y = std::max(max_y, point.y);
        }

        std::uniform_real_distribution<double> pick_x(min_x, max_x);
        std::uniform_real_distribution<double> pick_y(min_y, max_y);
        std::uniform_real_distribution<double> pick_yaw(0.0, k_two_pi);
        std::uniform_real_distribution<double> pick_size(towns.building_size_min,
                                                         towns.building_size_max);

        // Bounded: a cell too cramped to hold another building would otherwise
        // sample forever.
        for (int attempt = 0; attempt < towns.infill_attempts; ++attempt) {
            if (static_cast<int>(buildings.size()) >= budget) {
                return;
            }
            const double size = pick_size(rng);
            MapBuilding candidate;
            candidate.width = size;
            candidate.height = size;
            candidate.rotation = pick_yaw(rng);
            candidate.point = {pick_x(rng), pick_y(rng)};

            if (can_place_(polygon, buildings, keep_out, candidate)) {
                buildings.push_back(candidate);
            }
        }
    }

    /**
     * @brief True when a footprint fits: inside the cell, clear of water, clear of neighbours.
     * @param polygon The cell outline.
     * @param buildings Already-placed footprints.
     * @param keep_out River and shoreline segments with their required clearances.
     * @param candidate The footprint to test.
     */
    bool can_place_(const std::vector<MapPoint>& polygon,
                    const std::vector<MapBuilding>& buildings,
                    const WaterKeepOut& keep_out,
                    const MapBuilding& candidate) const {
        const std::array<MapPoint, 4> corners = building_corners(candidate);

        for (const MapPoint& corner : corners) {
            if (!point_in_polygon(polygon, corner)) {
                return false;
            }
        }
        for (std::size_t i = 0; i < keep_out.segments.size(); ++i) {
            const double clearance = keep_out.clearances[i];
            for (const MapPoint& corner : corners) {
                if (distance_to_segment_(corner, keep_out.segments[i][0],
                                         keep_out.segments[i][1]) < clearance) {
                    return false;
                }
            }
        }
        for (const MapBuilding& placed : buildings) {
            if (buildings_overlap(placed, candidate)) {
                return false;
            }
        }
        return true;
    }

    /** @brief Shortest distance from a point to a line segment. */
    static double distance_to_segment_(const MapPoint& point, const MapPoint& a,
                                       const MapPoint& b) {
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double length_squared = dx * dx + dy * dy;
        if (length_squared == 0.0) {
            return point.distance_to(a);
        }
        double t = ((point.x - a.x) * dx + (point.y - a.y) * dy) / length_squared;
        t = std::clamp(t, 0.0, 1.0);
        return std::hypot(point.x - (a.x + t * dx), point.y - (a.y + t * dy));
    }

    /** @brief Lanes given to a cell with no road or river to build along. */
    static constexpr std::size_t k_fallback_streets = 2;

    /** @brief Full turn in radians, for the building yaw distribution. */
    static constexpr double k_two_pi = 6.283185307179586;
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_TOWNS_H
