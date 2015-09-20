#include "mapdata.h"
#include "map.h"
#include "map_iterator.h"
#include "game.h"
#include "lightmap.h"
#include "options.h"
#include "npc.h"
#include "monster.h"
#include "veh_type.h"
#include "vehicle.h"
#include "submap.h"
#include "mtype.h"
#include "weather.h"

#include <cmath>
#include <cstring>

#define INBOUNDS(x, y) \
    (x >= 0 && x < SEEX * MAPSIZE && y >= 0 && y < SEEY * MAPSIZE)
#define LIGHTMAP_CACHE_X SEEX * MAPSIZE
#define LIGHTMAP_CACHE_Y SEEY * MAPSIZE

constexpr double PI     = 3.14159265358979323846;
constexpr double HALFPI = 1.57079632679489661923;
constexpr double SQRT_2 = 1.41421356237309504880;

void map::add_light_from_items( const tripoint &p, std::list<item>::iterator begin,
                                std::list<item>::iterator end )
{
    for( auto itm_it = begin; itm_it != end; ++itm_it ) {
        float ilum = 0.0; // brightness
        int iwidth = 0; // 0-360 degrees. 0 is a circular light_source
        int idir = 0;   // otherwise, it's a light_arc pointed in this direction
        if( itm_it->getlight( ilum, iwidth, idir ) ) {
            if( iwidth > 0 ) {
                apply_light_arc( p, idir, ilum, iwidth );
            } else {
                add_light_source( p, ilum );
            }
        }
    }
}

// TODO Consider making this just clear the cache and dynamically fill it in as trans() is called
void map::build_transparency_cache( const int zlev )
{
    auto &map_cache = get_cache( zlev );
    auto &transparency_cache = map_cache.transparency_cache;
    auto &outside_cache = map_cache.outside_cache;

    if( !map_cache.transparency_cache_dirty ) {
        return;
    }

    // Default to just barely not transparent.
    std::uninitialized_fill_n(
        &transparency_cache[0][0], MAPSIZE*SEEX * MAPSIZE*SEEY, LIGHT_TRANSPARENCY_OPEN_AIR);

    // Traverse the submaps in order
    for( int smx = 0; smx < my_MAPSIZE; ++smx ) {
        for( int smy = 0; smy < my_MAPSIZE; ++smy ) {
            auto const cur_submap = get_submap_at_grid( smx, smy, zlev );

            for( int sx = 0; sx < SEEX; ++sx ) {
                for( int sy = 0; sy < SEEY; ++sy ) {
                    const int x = sx + smx * SEEX;
                    const int y = sy + smy * SEEY;

                    auto &value = transparency_cache[x][y];

                    if( !(cur_submap->ter[sx][sy].obj().transparent &&
                          cur_submap->frn[sx][sy].obj().transparent) ) {
                        value = LIGHT_TRANSPARENCY_SOLID;
                        continue;
                    }

                    if( outside_cache[x][y] ) {
                        value *= weather_data(g->weather).sight_penalty;
                    }

                    for( auto const &fld : cur_submap->fld[sx][sy] ) {
                        const field_entry &cur = fld.second;
                        const field_id type = cur.getFieldType();
                        const int density = cur.getFieldDensity();

                        if( fieldlist[type].transparent[density - 1] ) {
                            continue;
                        }

                        // Fields are either transparent or not, however we want some to be translucent
                        switch (type) {
                        case fd_cigsmoke:
                        case fd_weedsmoke:
                        case fd_cracksmoke:
                        case fd_methsmoke:
                        case fd_relax_gas:
                            value *= 5;
                            break;
                        case fd_smoke:
                        case fd_incendiary:
                        case fd_toxic_gas:
                        case fd_tear_gas:
                            if (density == 3) {
                                value = LIGHT_TRANSPARENCY_SOLID;
                            } else if (density == 2) {
                                value *= 10;
                            }
                            break;
                        case fd_nuke_gas:
                            value *= 10;
                            break;
                        case fd_fire:
                            value *= 1.0 - ( density * 0.3 );
                            break;
                        default:
                            value = LIGHT_TRANSPARENCY_SOLID;
                            break;
                        }
                        // TODO: [lightmap] Have glass reduce light as well
                    }
                }
            }
        }
    }
    map_cache.transparency_cache_dirty = false;
}

void map::apply_character_light( const player &p )
{
    float const held_luminance = p.active_light();
    if( held_luminance > LIGHT_AMBIENT_LOW ) {
        apply_light_source( p.pos(), held_luminance );
    }
}

void map::generate_lightmap( const int zlev )
{
    auto &map_cache = get_cache( zlev );
    auto &lm = map_cache.lm;
    auto &sm = map_cache.sm;
    auto &outside_cache = map_cache.outside_cache;
    std::memset(lm, 0, sizeof(lm));
    std::memset(sm, 0, sizeof(sm));

    /* Bulk light sources wastefully cast rays into neighbors; a burning hospital can produce
         significant slowdown, so for stuff like fire and lava:
     * Step 1: Store the position and luminance in buffer via add_light_source, for efficient
         checking of neighbors.
     * Step 2: After everything else, iterate buffer and apply_light_source only in non-redundant
         directions
     * Step 3: Profit!
     */
    auto &light_source_buffer = map_cache.light_source_buffer;
    std::memset(light_source_buffer, 0, sizeof(light_source_buffer));

    constexpr int dir_x[] = {  0, -1 , 1, 0 };   //    [0]
    constexpr int dir_y[] = { -1,  0 , 0, 1 };   // [1][X][2]
    constexpr int dir_d[] = { 90, 0, 180, 270 }; //    [3]

    const float natural_light  = g->natural_light_level( zlev );
    const float inside_light = (natural_light > LIGHT_SOURCE_BRIGHT) ?
        LIGHT_AMBIENT_LOW + 1.0 : LIGHT_AMBIENT_MINIMAL;
    // Apply sunlight, first light source so just assign
    for( int sx = 0; sx < LIGHTMAP_CACHE_X; ++sx ) {
        for( int sy = 0; sy < LIGHTMAP_CACHE_Y; ++sy ) {
            // In bright light indoor light exists to some degree
            if( !outside_cache[sx][sy] ) {
                lm[sx][sy] = inside_light;
            } else {
                lm[sx][sy] = natural_light;
            }
        }
    }

    apply_character_light( g->u );
    for( auto &n : g->active_npc ) {
        apply_character_light( *n );
    }

    // Traverse the submaps in order
    for (int smx = 0; smx < my_MAPSIZE; ++smx) {
        for (int smy = 0; smy < my_MAPSIZE; ++smy) {
            auto const cur_submap = get_submap_at_grid( smx, smy, zlev );

            for (int sx = 0; sx < SEEX; ++sx) {
                for (int sy = 0; sy < SEEY; ++sy) {
                    const int x = sx + smx * SEEX;
                    const int y = sy + smy * SEEY;
                    const tripoint p( x, y, zlev );
                    // Project light into any openings into buildings.
                    if (natural_light > LIGHT_SOURCE_BRIGHT && !outside_cache[p.x][p.y]) {
                        // Apply light sources for external/internal divide
                        for(int i = 0; i < 4; ++i) {
                            if (INBOUNDS(p.x + dir_x[i], p.y + dir_y[i]) &&
                                outside_cache[p.x + dir_x[i]][p.y + dir_y[i]]) {
                                lm[p.x][p.y] = natural_light;

                                if (light_transparency( p ) > LIGHT_TRANSPARENCY_SOLID) {
                                    apply_directional_light( p, dir_d[i], natural_light );
                                }
                            }
                        }
                    }

                    if( cur_submap->lum[sx][sy] && has_items( p ) ) {
                        auto items = i_at( p );
                        add_light_from_items( p, items.begin(), items.end() );
                    }

                    const ter_id terrain = cur_submap->ter[sx][sy];
                    if (terrain == t_lava) {
                        add_light_source( p, 50 );
                    } else if (terrain == t_console) {
                        add_light_source( p, 10 );
                    } else if (terrain == t_utility_light) {
                        add_light_source( p, 240 );
                    }

                    for( auto &fld : cur_submap->fld[sx][sy] ) {
                        const field_entry *cur = &fld.second;
                        // TODO: [lightmap] Attach light brightness to fields
                        switch(cur->getFieldType()) {
                        case fd_fire:
                            if (3 == cur->getFieldDensity()) {
                                add_light_source( p, 160 );
                            } else if (2 == cur->getFieldDensity()) {
                                add_light_source( p, 60 );
                            } else {
                                add_light_source( p, 20 );
                            }
                            break;
                        case fd_fire_vent:
                        case fd_flame_burst:
                            add_light_source( p, 20 );
                            break;
                        case fd_electricity:
                        case fd_plasma:
                            if (3 == cur->getFieldDensity()) {
                                add_light_source( p, 20 );
                            } else if (2 == cur->getFieldDensity()) {
                                add_light_source( p, 4 );
                            } else {
                                // Kinda a hack as the square will still get marked.
                                apply_light_source( p, LIGHT_SOURCE_LOCAL );
                            }
                            break;
                        case fd_incendiary:
                            if (3 == cur->getFieldDensity()) {
                                add_light_source( p, 160 );
                            } else if (2 == cur->getFieldDensity()) {
                                add_light_source( p, 60 );
                            } else {
                                add_light_source( p, 20 );
                            }
                            break;
                        case fd_laser:
                            apply_light_source( p, 4 );
                            break;
                        case fd_spotlight:
                            add_light_source( p, 80 );
                            break;
                        case fd_dazzling:
                            add_light_source( p, 5 );
                            break;
                        default:
                            //Suppress warnings
                            break;
                        }
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < g->num_zombies(); ++i) {
        auto &critter = g->zombie(i);
        if(critter.is_hallucination()) {
            continue;
        }
        const tripoint &mp = critter.pos();
        if( inbounds( mp ) ) {
            if (critter.has_effect("onfire")) {
                apply_light_source( mp, 8 );
            }
            // TODO: [lightmap] Attach natural light brightness to creatures
            // TODO: [lightmap] Allow creatures to have light attacks (ie: eyebot)
            // TODO: [lightmap] Allow creatures to have facing and arc lights
            if (critter.type->luminance > 0) {
                apply_light_source( mp, critter.type->luminance );
            }
        }
    }

    // Apply any vehicle light sources
    VehicleList vehs = get_vehicles();
    for( auto &vv : vehs ) {
        vehicle *v = vv.v;
        if(v->lights_on) {
            int dir = v->face.dir();
            float veh_luminance = 0.0;
            float iteration = 1.0;
            std::vector<int> light_indices = v->all_parts_with_feature(VPFLAG_CONE_LIGHT);
            for( auto &light_indice : light_indices ) {
                veh_luminance += ( v->part_info( light_indice ).bonus / iteration );
                iteration = iteration * 1.1;
            }
            if (veh_luminance > LL_LIT) {
                for( auto &light_indice : light_indices ) {
                    tripoint pp = tripoint( vv.x, vv.y, vv.z ) +
                                  v->parts[light_indice].precalc[0];
                    if( inbounds( pp ) ) {
                        add_light_source( pp, SQRT_2 ); // Add a little surrounding light
                        apply_light_arc( pp, dir + v->parts[light_indice].direction,
                                         veh_luminance, 45 );
                    }
                }
            }
        }
        if(v->overhead_lights_on) {
            std::vector<int> light_indices = v->all_parts_with_feature(VPFLAG_CIRCLE_LIGHT);
            for( auto &light_indice : light_indices ) {
                if( ( calendar::turn % 2 &&
                      v->part_info( light_indice ).has_flag( VPFLAG_ODDTURN ) ) ||
                    ( !( calendar::turn % 2 ) &&
                      v->part_info( light_indice ).has_flag( VPFLAG_EVENTURN ) ) ||
                    ( !v->part_info( light_indice ).has_flag( VPFLAG_EVENTURN ) &&
                      !v->part_info( light_indice ).has_flag( VPFLAG_ODDTURN ) ) ) {
                    tripoint pp = tripoint( vv.x, vv.y, vv.z ) +
                                  v->parts[light_indice].precalc[0];
                    if(inbounds( pp )) {
                        add_light_source( pp, v->part_info( light_indice ).bonus );
                    }
                }
            }
        }
        // why reinvent the [lightmap] wheel
        if(v->dome_lights_on) {
            std::vector<int> light_indices = v->all_parts_with_feature(VPFLAG_DOME_LIGHT);
            for( auto &light_indice : light_indices ) {
                tripoint pp = tripoint( vv.x, vv.y, vv.z ) +
                              v->parts[light_indice].precalc[0];
                if( inbounds( pp )) {
                    add_light_source( pp, v->part_info( light_indice ).bonus );
                }
            }
        }
        if(v->aisle_lights_on) {
            std::vector<int> light_indices = v->all_parts_with_feature(VPFLAG_AISLE_LIGHT);
            for( auto &light_indice : light_indices ) {
                tripoint pp = tripoint( vv.x, vv.y, vv.z ) +
                              v->parts[light_indice].precalc[0];
                if( inbounds( pp )) {
                    add_light_source( pp, v->part_info( light_indice ).bonus );
                }
            }
        }
        if(v->has_atomic_lights) {
            // atomic light is always on
            std::vector<int> light_indices = v->all_parts_with_feature(VPFLAG_ATOMIC_LIGHT);
            for( auto &light_indice : light_indices ) {
                tripoint pp = tripoint( vv.x, vv.y, vv.z ) +
                              v->parts[light_indice].precalc[0];
                if(inbounds( pp )) {
                    add_light_source( pp, v->part_info( light_indice ).bonus );
                }
            }
        }
        for( size_t p = 0; p < v->parts.size(); ++p ) {
            tripoint pp = tripoint( vv.x, vv.y, vv.z ) +
                          v->parts[p].precalc[0];
            if( !inbounds( pp ) ) {
                continue;
            }
            if( v->part_flag( p, VPFLAG_CARGO ) && !v->part_flag( p, "COVERED" ) ) {
                add_light_from_items( pp, v->get_items(p).begin(), v->get_items(p).end() );
            }
        }
    }

    /* Now that we have position and intensity of all bulk light sources, apply_ them
      This may seem like extra work, but take a 12x12 raging inferno:
        unbuffered: (12^2)*(160*4) = apply_light_ray x 92160
        buffered:   (12*4)*(160)   = apply_light_ray x 7680
    */
    const tripoint cache_start( 0, 0, zlev );
    const tripoint cache_end( LIGHTMAP_CACHE_X, LIGHTMAP_CACHE_Y, zlev );
    for( const tripoint &p : points_in_rectangle( cache_start, cache_end ) ) {
        if( light_source_buffer[p.x][p.y] > 0.0 ) {
            apply_light_source( p, light_source_buffer[p.x][p.y] );
        }
    }


    if (g->u.has_active_bionic("bio_night") ) {
        for( const tripoint &p : points_in_rectangle( cache_start, cache_end ) ) {
            if( rl_dist( p, g->u.pos() ) < 15 ) {
                lm[p.x][p.y] = LIGHT_AMBIENT_MINIMAL;
            }
        }
    }
}

void map::add_light_source( const tripoint &p, float luminance )
{
    auto &light_source_buffer = get_cache( p.z ).light_source_buffer;
    light_source_buffer[p.x][p.y] = std::max(luminance, light_source_buffer[p.x][p.y]);
}

// Tile light/transparency: 3D

lit_level map::light_at( const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        return LL_DARK;    // Out of bounds
    }

    const auto &map_cache = get_cache_ref( p.z );
    const auto &lm = map_cache.lm;
    const auto &sm = map_cache.sm;
    if (sm[p.x][p.y] >= LIGHT_SOURCE_BRIGHT) {
        return LL_BRIGHT;
    }

    if (lm[p.x][p.y] >= LIGHT_AMBIENT_LIT) {
        return LL_LIT;
    }

    if (lm[p.x][p.y] >= LIGHT_AMBIENT_LOW) {
        return LL_LOW;
    }

    return LL_DARK;
}

float map::ambient_light_at( const tripoint &p ) const
{
    if( !inbounds( p ) ) {
        return 0.0f;
    }

    return get_cache_ref( p.z ).lm[p.x][p.y];
}

bool map::trans( const tripoint &p ) const
{
    return light_transparency( p ) > LIGHT_TRANSPARENCY_SOLID;
}

float map::light_transparency( const tripoint &p ) const
{
    return get_cache_ref( p.z ).transparency_cache[p.x][p.y];
}

// End of tile light/transparency

bool map::pl_sees( const tripoint &t, const int max_range ) const
{
    if( !inbounds( t ) ) {
        return false;
    }

    if( max_range >= 0 && square_dist( t, g->u.pos3() ) > max_range ) {
        return false;    // Out of range!
    }

    const auto &map_cache = get_cache_ref( t.z );
    return map_cache.seen_cache[t.x][t.y] > LIGHT_TRANSPARENCY_SOLID + 0.1 &&
        ( map_cache.seen_cache[t.x][t.y] * map_cache.lm[t.x][t.y] >
          g->u.get_vision_threshold( map_cache.lm[g->u.posx()][g->u.posy()] ) ||
          map_cache.sm[t.x][t.y] > 0.0 );
}

template<float(*calc)(const float &, const float &, const int &),
         bool(*check)(const float &, const float &)>
void cast_ray( float (&output_cache)[MAPSIZE*SEEX][MAPSIZE*SEEY],
               const std::array<const float (*)[MAPSIZE*SEEX][MAPSIZE*SEEY], OVERMAP_LAYERS> &input_arrays,
               const std::function<bool(const tripoint &)> &floor_check,
               const tripoint &f, const tripoint &t,
               const int offset_distance, const float numerator )
{
    tripoint last_point = f;
    float cumulative_transparency = 0.0f;
    int distance = 1;
    const auto ray = [&]( const tripoint &cur ) {
        float current_transparency;
        if( last_point.z == cur.z ) {
            current_transparency = (*input_arrays[cur.z + OVERMAP_DEPTH])[cur.x][cur.y];
        } else {
            const int max_z = std::max( cur.z, last_point.z );
            current_transparency = LIGHT_TRANSPARENCY_SOLID;
            if( floor_check({cur.x, cur.y, max_z}) ) {
                current_transparency = (*input_arrays[last_point.z + OVERMAP_DEPTH])[cur.x][cur.y];
            }

            if( floor_check({last_point.x, last_point.y, max_z}) ) {
                const float here = (*input_arrays[cur.z + OVERMAP_DEPTH])[last_point.x][last_point.y];
                // Need this weird split because lower is more transparent
                // except at 0.0 when it becomes totally opaque
                if( current_transparency == LIGHT_TRANSPARENCY_SOLID ) {
                    current_transparency = here;
                } else if( here != LIGHT_TRANSPARENCY_SOLID ) {
                    current_transparency = std::min( current_transparency, here );
                }
            }

            if( current_transparency == LIGHT_TRANSPARENCY_SOLID ) {
                return false;
            }
        }

        last_point = cur;

        const int dist = rl_dist( f, cur ) + offset_distance;
        float intensity = calc( numerator, cumulative_transparency, dist );
        if( cur.z == t.z ) {
            output_cache[cur.x][cur.y] = std::max( output_cache[cur.x][cur.y], intensity );
        }

        if( !check( current_transparency, intensity ) ) {
            return false;
        }
        // Cumulative average of the transparency values encountered.
        cumulative_transparency =
            ((distance - 1) * cumulative_transparency + current_transparency) / distance;
        distance++;
        return true;
    };

    bresenham( f, t, 0, 0, ray );
}

/**
 * Calculates the Field Of View for the provided map from the given x, y
 * coordinates. Returns a lightmap for a result where the values represent a
 * percentage of fully lit.
 *
 * A value equal to or below 0 means that cell is not in the
 * field of view, whereas a value equal to or above 1 means that cell is
 * in the field of view.
 *
 * @param startx the horizontal component of the starting location
 * @param starty the vertical component of the starting location
 * @param radius the maximum distance to draw the FOV
 */
void map::build_seen_cache( const tripoint &origin, const int target_z )
{
    auto &map_cache = get_cache( target_z );
    float (&transparency_cache)[MAPSIZE*SEEX][MAPSIZE*SEEY] = map_cache.transparency_cache;
    float (&seen_cache)[MAPSIZE*SEEX][MAPSIZE*SEEY] = map_cache.seen_cache;

    std::uninitialized_fill_n(
        &seen_cache[0][0], MAPSIZE*SEEX * MAPSIZE*SEEY, LIGHT_TRANSPARENCY_SOLID);

    if( !fov_3d ) {
        seen_cache[origin.x][origin.y] = LIGHT_TRANSPARENCY_CLEAR;

        castLight<0, 1, 1, 0, sight_calc, sight_check>(
            seen_cache, transparency_cache, origin.x, origin.y, 0 );
        castLight<1, 0, 0, 1, sight_calc, sight_check>(
            seen_cache, transparency_cache, origin.x, origin.y, 0 );

        castLight<0, -1, 1, 0, sight_calc, sight_check>(
            seen_cache, transparency_cache, origin.x, origin.y, 0 );
        castLight<-1, 0, 0, 1, sight_calc, sight_check>(
            seen_cache, transparency_cache, origin.x, origin.y, 0 );

        castLight<0, 1, -1, 0, sight_calc, sight_check>(
            seen_cache, transparency_cache, origin.x, origin.y, 0 );
        castLight<1, 0, 0, -1, sight_calc, sight_check>(
            seen_cache, transparency_cache, origin.x, origin.y, 0 );

        castLight<0, -1, -1, 0, sight_calc, sight_check>(
            seen_cache, transparency_cache, origin.x, origin.y, 0 );
        castLight<-1, 0, 0, -1, sight_calc, sight_check>(
            seen_cache, transparency_cache, origin.x, origin.y, 0 );
    } else {
        if( origin.z == target_z ) {
            seen_cache[origin.x][origin.y] = LIGHT_TRANSPARENCY_CLEAR;
        }

        // Cache the caches (pointers to them)
        std::array<const float (*)[MAPSIZE*SEEX][MAPSIZE*SEEY], OVERMAP_LAYERS> transparency_caches;
        const int minz = std::min( origin.z, target_z );
        const int maxz = std::max( origin.z, target_z );
        for( int z = minz; z <= maxz; z++ ) {
            const auto &cur_cache = get_cache( z );
            transparency_caches[z + OVERMAP_DEPTH] = &cur_cache.transparency_cache;
        }

        const auto floor_check = [this]( const tripoint &p ) {
            const tripoint below( p.x, p.y, p.z - 1 );
            return valid_move( p, below, false, true );
        };

        // Raycasting, as opposed to shadowcasting for 2D (just for now)
        const int range = g->u.sight_range( ambient_light_at( g->u.pos() ) );
        const tripoint start( g->u.posx() - range, g->u.posy() - range, target_z );
        const tripoint end  ( g->u.posx() + range, g->u.posy() + range, target_z );
        for( const tripoint &pt : points_in_rectangle( start, end ) ) {
            cast_ray<sight_calc, sight_check>(
                seen_cache, transparency_caches, floor_check, origin, pt, 0, 1.0f );
        }

        // No vehicles yet
        return;
    }

    int part;
    vehicle *veh = veh_at( origin, part );
    if( veh == nullptr ) {
        return;
    }

    // We're inside a vehicle. Do mirror calcs.
    std::vector<int> mirrors = veh->all_parts_with_feature(VPFLAG_EXTENDS_VISION, true);
    // Do all the sight checks first to prevent fake multiple reflection
    // from happening due to mirrors becoming visible due to processing order.
    // Cameras are also handled here, so that we only need to get through all veh parts once
    int cam_control = -1;
    for (std::vector<int>::iterator m_it = mirrors.begin(); m_it != mirrors.end(); /* noop */) {
        const auto mirror_pos = veh->global_pos() + veh->parts[*m_it].precalc[0];
        // We can utilize the current state of the seen cache to determine
        // if the player can see the mirror from their position.
        if( !veh->part_info( *m_it ).has_flag( "CAMERA" ) &&
            seen_cache[mirror_pos.x][mirror_pos.y] < LIGHT_TRANSPARENCY_SOLID + 0.1 ) {
            m_it = mirrors.erase(m_it);
        } else if( !veh->part_info( *m_it ).has_flag( "CAMERA_CONTROL" ) ) {
            ++m_it;
        } else {
            if( origin.x == mirror_pos.x && origin.y == mirror_pos.y && veh->camera_on ) {
                cam_control = *m_it;
            }
            m_it = mirrors.erase( m_it );
        }
    }

    for( size_t i = 0; i < mirrors.size(); i++ ) {
        const int &mirror = mirrors[i];
        bool is_camera = veh->part_info( mirror ).has_flag( "CAMERA" );
        if( is_camera && cam_control < 0 ) {
            continue; // Player not at camera control, so cameras don't work
        }

        const auto mirror_pos = veh->global_pos() + veh->parts[mirror].precalc[0];

        // Determine how far the light has already traveled so mirrors
        // don't cheat the light distance falloff.
        int offsetDistance;
        if( !is_camera ) {
            offsetDistance = rl_dist(origin.x, origin.y, mirror_pos.x, mirror_pos.y);
        } else {
            offsetDistance = 60 - veh->part_info( mirror ).bonus *
                                  veh->parts[mirror].hp / veh->part_info( mirror ).durability;
            seen_cache[mirror_pos.x][mirror_pos.y] = LIGHT_TRANSPARENCY_OPEN_AIR;
        }

        // @todo: Factor in the mirror facing and only cast in the
        // directions the player's line of sight reflects to.
        //
        // The naive solution of making the mirrors act like a second player
        // at an offset appears to give reasonable results though.

        castLight<0, 1, 1, 0, sight_calc, sight_check>(
            seen_cache, transparency_cache, mirror_pos.x, mirror_pos.y, offsetDistance );
        castLight<1, 0, 0, 1, sight_calc, sight_check>(
            seen_cache, transparency_cache, mirror_pos.x, mirror_pos.y, offsetDistance );

        castLight<0, -1, 1, 0, sight_calc, sight_check>(
            seen_cache, transparency_cache, mirror_pos.x, mirror_pos.y, offsetDistance );
        castLight<-1, 0, 0, 1, sight_calc, sight_check>(
            seen_cache, transparency_cache, mirror_pos.x, mirror_pos.y, offsetDistance );

        castLight<0, 1, -1, 0, sight_calc, sight_check>(
            seen_cache, transparency_cache, mirror_pos.x, mirror_pos.y, offsetDistance );
        castLight<1, 0, 0, -1, sight_calc, sight_check>(
            seen_cache, transparency_cache, mirror_pos.x, mirror_pos.y, offsetDistance );

        castLight<0, -1, -1, 0, sight_calc, sight_check>(
            seen_cache, transparency_cache, mirror_pos.x, mirror_pos.y, offsetDistance );
        castLight<-1, 0, 0, -1, sight_calc, sight_check>(
            seen_cache, transparency_cache, mirror_pos.x, mirror_pos.y, offsetDistance );
    }
}

template<int xx, int xy, int yx, int yy, float(*calc)(const float &, const float &, const int &),
         bool(*check)(const float &, const float &)>
void castLight( float (&output_cache)[MAPSIZE*SEEX][MAPSIZE*SEEY],
                const float (&input_array)[MAPSIZE*SEEX][MAPSIZE*SEEY],
                const int offsetX, const int offsetY, const int offsetDistance, const float numerator,
                const int row, float start, const float end, double cumulative_transparency )
{
    float newStart = 0.0f;
    float radius = 60.0f - offsetDistance;
    if( start < end ) {
        return;
    }
    float last_intensity = 0.0;
    // Making this static prevents it from being needlessly constructed/destructed all the time.
    static const tripoint origin(0, 0, 0);
    // But each instance of the method needs one of these.
    tripoint delta(0, 0, 0);
    for( int distance = row; distance <= radius; distance++ ) {
        delta.y = -distance;
        bool started_row = false;
        float current_transparency = 0.0;
        for( delta.x = -distance; delta.x <= 0; delta.x++ ) {
            int currentX = offsetX + delta.x * xx + delta.y * xy;
            int currentY = offsetY + delta.x * yx + delta.y * yy;
            float trailingEdge = (delta.x - 0.5f) / (delta.y + 0.5f);
            float leadingEdge = (delta.x + 0.5f) / (delta.y - 0.5f);

            if( !(currentX >= 0 && currentY >= 0 && currentX < SEEX * MAPSIZE &&
                  currentY < SEEY * MAPSIZE) || start < leadingEdge ) {
                continue;
            } else if( end > trailingEdge ) {
                break;
            }
            if( !started_row ) {
                started_row = true;
                current_transparency = input_array[ currentX ][ currentY ];
            }

            const int dist = rl_dist( origin, delta ) + offsetDistance;
            last_intensity = calc( numerator, cumulative_transparency, dist );
            output_cache[currentX][currentY] =
                std::max( output_cache[currentX][currentY], last_intensity );

            float new_transparency = input_array[ currentX ][ currentY ];

            if( new_transparency != current_transparency ) {
                // Only cast recursively if previous span was not opaque.
                if( check( current_transparency, last_intensity ) ) {
                    castLight<xx, xy, yx, yy, calc, check>(
                        output_cache, input_array, offsetX, offsetY, offsetDistance,
                        numerator, distance + 1, start, trailingEdge,
                        ((distance - 1) * cumulative_transparency + current_transparency) / distance );
                }
                // The new span starts at the leading edge of the previous square if it is opaque,
                // and at the trailing edge of the current square if it is transparent.
                if( current_transparency == LIGHT_TRANSPARENCY_SOLID ) {
                    start = newStart;
                } else {
                    // Note this is the same slope as the recursive call we just made.
                    start = trailingEdge;
                }
                current_transparency = new_transparency;
            }
            newStart = leadingEdge;
        }
        if( !check(current_transparency, last_intensity) ) {
            // If we reach the end of the span with terrain being opaque, we don't iterate further.
            break;
        }
        // Cumulative average of the transparency values encountered.
        cumulative_transparency =
            ((distance - 1) * cumulative_transparency + current_transparency) / distance;
    }
}

static float light_calc( const float &numerator, const float &transparency, const int &distance ) {
    // Light needs inverse square falloff in addition to attenuation.
    return numerator / (float)(exp( transparency * distance ) * distance);
}
static bool light_check( const float &transparency, const float &intensity ) {
    return transparency > LIGHT_TRANSPARENCY_SOLID && intensity > LIGHT_AMBIENT_LOW;
}

void map::apply_light_source( const tripoint &p, float luminance )
{
    auto &cache = get_cache( p.z );
    float (&lm)[MAPSIZE*SEEX][MAPSIZE*SEEY] = cache.lm;
    float (&sm)[MAPSIZE*SEEX][MAPSIZE*SEEY] = cache.sm;
    float (&transparency_cache)[MAPSIZE*SEEX][MAPSIZE*SEEY] = cache.transparency_cache;
    float (&light_source_buffer)[MAPSIZE*SEEX][MAPSIZE*SEEY] = cache.light_source_buffer;

    const int x = p.x;
    const int y = p.y;

    if( inbounds( p ) ) {
        lm[x][y] = std::max(lm[x][y], static_cast<float>(LL_LOW));
        lm[x][y] = std::max(lm[x][y], luminance);
        sm[x][y] = std::max(sm[x][y], luminance);
    }
    if ( luminance <= 1 ) {
        return;
    } else if ( luminance <= 2 ) {
        luminance = 1.49f;
    } else if (luminance <= LIGHT_SOURCE_LOCAL) {
        return;
    }

    /* If we're a 5 luminance fire , we skip casting rays into ey && sx if we have
         neighboring fires to the north and west that were applied via light_source_buffer
       If there's a 1 luminance candle east in buffer, we still cast rays into ex since it's smaller
       If there's a 100 luminance magnesium flare south added via apply_light_source instead od
         add_light_source, it's unbuffered so we'll still cast rays into sy.

          ey
        nnnNnnn
        w     e
        w  5 +e
     sx W 5*1+E ex
        w ++++e
        w+++++e
        sssSsss
           sy
    */
    const int peer_inbounds = LIGHTMAP_CACHE_X - 1;
    bool north = (y != 0 && light_source_buffer[x][y - 1] < luminance );
    bool south = (y != peer_inbounds && light_source_buffer[x][y + 1] < luminance );
    bool east = (x != peer_inbounds && light_source_buffer[x + 1][y] < luminance );
    bool west = (x != 0 && light_source_buffer[x - 1][y] < luminance );

    if( north ) {
        castLight<1, 0, 0, -1, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
        castLight<-1, 0, 0, -1, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
    }

    if( east ) {
        castLight<0, -1, 1, 0, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
        castLight<0, -1, -1, 0, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
    }


    if( south ) {
        castLight<1, 0, 0, 1, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
        castLight<-1, 0, 0, 1, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
    }

    if( west ) {
        castLight<0, 1, 1, 0, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
        castLight<0, 1, -1, 0, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
    }
}

void map::apply_directional_light( const tripoint &p, int direction, float luminance )
{
    const int x = p.x;
    const int y = p.y;

    auto &cache = get_cache( p.z );
    float (&lm)[MAPSIZE*SEEX][MAPSIZE*SEEY] = cache.lm;
    float (&transparency_cache)[MAPSIZE*SEEX][MAPSIZE*SEEY] = cache.transparency_cache;

    if( direction == 90 ) {
        castLight<1, 0, 0, -1, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
        castLight<-1, 0, 0, -1, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
    } else if( direction == 0 ) {
        castLight<0, -1, 1, 0, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
        castLight<0, -1, -1, 0, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
    } else if( direction == 270 ) {
        castLight<1, 0, 0, 1, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
        castLight<-1, 0, 0, 1, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
    } else if( direction == 180 ) {
        castLight<0, 1, 1, 0, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
        castLight<0, 1, -1, 0, light_calc, light_check>( lm, transparency_cache, x, y, 0, luminance );
    }
}

void map::apply_light_arc( const tripoint &p, int angle, float luminance, int wideangle )
{
    if (luminance <= LIGHT_SOURCE_LOCAL) {
        return;
    }

    bool lit[LIGHTMAP_CACHE_X][LIGHTMAP_CACHE_Y] {};

    apply_light_source( p, LIGHT_SOURCE_LOCAL );

    // Normalise (should work with negative values too)
    const double wangle = wideangle / 2.0;

    int nangle = angle % 360;

    tripoint end;
    double rad = PI * (double)nangle / 180;
    int range = LIGHT_RANGE(luminance);
    calc_ray_end( nangle, range, p, end );
    apply_light_ray(lit, p, end , luminance);

    tripoint test;
    calc_ray_end(wangle + nangle, range, p, test );

    const float wdist = hypot( end.x - test.x, end.y - test.y );
    if (wdist <= 0.5) {
        return;
    }

    // attempt to determine beam density required to cover all squares
    const double wstep = ( wangle / ( wdist * SQRT_2 ) );

    for( double ao = wstep; ao <= wangle; ao += wstep ) {
        if( trigdist ) {
            double fdist = (ao * HALFPI) / wangle;
            double orad = ( PI * ao / 180.0 );
            end.x = int( p.x + ( (double)range - fdist * 2.0) * cos(rad + orad) );
            end.y = int( p.y + ( (double)range - fdist * 2.0) * sin(rad + orad) );
            apply_light_ray( lit, p, end, luminance );

            end.x = int( p.x + ( (double)range - fdist * 2.0) * cos(rad - orad) );
            end.y = int( p.y + ( (double)range - fdist * 2.0) * sin(rad - orad) );
            apply_light_ray( lit, p, end, luminance );
        } else {
            calc_ray_end( nangle + ao, range, p, end );
            apply_light_ray( lit, p, end, luminance );
            calc_ray_end( nangle - ao, range, p, end );
            apply_light_ray( lit, p, end, luminance );
        }
    }
}

void map::calc_ray_end(int angle, int range, const tripoint &p, tripoint &out ) const
{
    double rad = (PI * angle) / 180;
    if (trigdist) {
        out.x = p.x + range * cos(rad);
        out.y = p.y + range * sin(rad);
    } else {
        int mult = 0;
        if (angle >= 135 && angle <= 315) {
            mult = -1;
        } else {
            mult = 1;
        }

        if (angle <= 45 || (135 <= angle && angle <= 215) || 315 < angle) {
            out.x = p.x + range * mult;
            out.y = p.y + range * tan(rad) * mult;
        } else {
            out.x = p.x + range * 1 / tan(rad) * mult;
            out.y = p.y + range * mult;
        }
    }
}

void map::apply_light_ray(bool lit[LIGHTMAP_CACHE_X][LIGHTMAP_CACHE_Y],
                          const tripoint &s, const tripoint &e, float luminance)
{
    int ax = abs(e.x - s.x) * 2;
    int ay = abs(e.y - s.y) * 2;
    int dx = (s.x < e.x) ? 1 : -1;
    int dy = (s.y < e.y) ? 1 : -1;
    int x = s.x;
    int y = s.y;

    // TODO: Invert that z comparison when it's sane
    if( s.z != e.z || (s.x == e.x && s.y == e.y) ) {
        return;
    }

    auto &lm = get_cache( s.z ).lm;
    auto &transparency_cache = get_cache( s.z ).transparency_cache;

    float distance = 1.0;
    float transparency = LIGHT_TRANSPARENCY_OPEN_AIR;
    const float scaling_factor = (float)rl_dist( s, e ) /
        (float)square_dist( s, e );
    // TODO: [lightmap] Pull out the common code here rather than duplication
    if (ax > ay) {
        int t = ay - (ax / 2);
        do {
            if(t >= 0) {
                y += dy;
                t -= ax;
            }

            x += dx;
            t += ay;

            // TODO: clamp coordinates to map bounds before this method is called.
            if (INBOUNDS(x, y)) {
                if (!lit[x][y]) {
                    // Multiple rays will pass through the same squares so we need to record that
                    lit[x][y] = true;
                    lm[x][y] = std::max( lm[x][y],
                                         luminance / ((float)exp( transparency * distance ) * distance) );
                }
                float current_transparency = transparency_cache[x][y];
                if(current_transparency == LIGHT_TRANSPARENCY_SOLID) {
                    break;
                }
                // Cumulative average of the transparency values encountered.
                transparency = ((distance - 1.0) * transparency + current_transparency) / distance;
            } else {
                break;
            }

            distance += scaling_factor;
        } while(!(x == e.x && y == e.y));
    } else {
        int t = ax - (ay / 2);
        do {
            if(t >= 0) {
                x += dx;
                t -= ay;
            }

            y += dy;
            t += ax;

            if (INBOUNDS(x, y)) {
                if(!lit[x][y]) {
                    // Multiple rays will pass through the same squares so we need to record that
                    lit[x][y] = true;
                    lm[x][y] = std::max(lm[x][y],
                                        luminance / ((float)exp( transparency * distance ) * distance) );
                }
                float current_transparency = transparency_cache[x][y];
                if(current_transparency == LIGHT_TRANSPARENCY_SOLID) {
                    break;
                }
                // Cumulative average of the transparency values encountered.
                transparency = ((distance - 1.0) * transparency + current_transparency) / distance;
            } else {
                break;
            }

            distance += scaling_factor;
        } while(!(x == e.x && y == e.y));
    }
}
