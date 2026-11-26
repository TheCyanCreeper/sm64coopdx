#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "engine/surface_collision.h"
#include <ultra64.h>
#include "sm64.h"
#include "textures.h"
#include "surface_terrains.h"
#include "behavior_data.h"
#include "model_ids.h"
#include "level_table.h"
#include "area.h"
#include "camera.h"
#include "object_helpers.h"
#include "object_list_processor.h"
#include "level_update.h"
#include "geo_misc.h"
#include "engine/surface_load.h"
#include "pc/lua/utils/smlua_collision_utils.h"
#include "landscape.h"
#include "levels/castle_courtyard/header.h"
#include "macro_special_objects.h"

// Keep native s16 vertices and the collision partition near Mario. Global cell
// coordinates survive each origin shift and drive both vegetation and terrain.
enum { SIDE = 24, ROW = SIDE + 1, CELL = 512, PATCHES = SIDE * SIDE };
static s64 originX, originZ;
// Player packets carry integer cell origins only in the generated area.
void landscape_get_origin(s64 origin[2]) {
    origin[0] = originX;
    origin[1] = originZ;
}
void landscape_relative_position(Vec3f pos, const s64 origin[2]) {
    for (int i = 0; i < 2; i++) {
        int axis = i * 2;
        double delta = ((double) origin[i] - (i ? originZ : originX)) * CELL;
        pos[axis] = fmax(-24000., fmin(24000., pos[axis] + delta));
    }
}

static Vtx vertices[4][PATCHES * 4];
static Gfx lists[4][PATCHES * 3 + 32];
static s16 heights[ROW * ROW], waterHeights[ROW * ROW];
enum { WATER_Y = 80, ROCK_SIDES = 6, ROCK_FACES = ROCK_SIDES * 3 };
// The original collision, warp nodes, moat and castle actors stay resident.
// Only triangles owned by the wilderness streamer may be retired.
struct GroundCell {
    s64 x, z;
    s16 localX, localZ;
    struct Surface *tri[2], *rock[ROCK_FACES];
};
static struct GroundCell ground[PATCHES];
static s64 centerX, centerZ;
enum { HOME = 20, HOME_ROW = HOME * 2 + 1 };
static s16 homeHeight[HOME_ROW][HOME_ROW];
static bool authored(s64 x, s64 z) {
    return llabs(x) <= HOME && llabs(z) <= HOME && homeHeight[z + HOME][x + HOME] > -10000;
}

static u32 hash(s64 x, s64 z, u32 seed) {
    u32 h = (u32) x * 0x9e3779b1u ^ (u32) z * 0x85ebca77u ^ seed;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    return h ^ (h >> 16);
}
static double smooth(double t) {
    return t <= 0 ? 0 : t >= 1 ? 1 : t * t * (3 - 2 * t);
}
static double lerp(double a, double b, double t) {
    return a + (b - a) * t;
}
static double noise(double x, double z, int scale, u32 seed) {
    x /= scale;
    z /= scale;
    s64 a = floor(x), b = floor(z);
    double u = smooth(x - a), v = smooth(z - b);
    return lerp(lerp(hash(a, b, seed), hash(a + 1, b, seed), u),
                lerp(hash(a, b + 1, seed), hash(a + 1, b + 1, seed), u), v)
           / 4294967295.;
}
static double castleBank[9];
static double bank_shape(double t) {
    double i = smooth(t) * 8;
    int j = (int) i;
    return j == 8 ? castleBank[j] : lerp(castleBank[j], castleBank[j + 1], i - j);
}
struct Climate {
    double woods, sand, ice, height, mountain, waterY;
};
static double river_distance(double x, double z) {
    double warp = 32 * (noise(x, z, 37, 376) - .5) + 8 * (noise(x, z, 11, 87) - .5);
    return fabs(remainder(x + warp - 31, 67));
}
static struct Climate sample(s64 x, s64 z) {
    double distance = fmax(fabs((double) x), fabs((double) z));
    double outskirts = smooth((distance - 16) / 18);
    double weather = noise(x, z, 80, 32421);
    double ice = outskirts * smooth((weather - .53) / .22);
    double sand = outskirts * smooth((.52 - weather) / .20);
    double woods = (1 - ice - sand) * smooth((noise(x, z, 48, 419) - .35) / .35) * outskirts;
    // Wide grass landings sit between low banks. Sharper shoulders and high
    // ridgelines give the snowfields and mesas distinct platforming profiles.
    double mountain = smooth((distance - 22) / 20) * smooth((noise(x, z, 51, 8609) - .54) / .25);
    double broad = noise(x, z, 15, 77);
    double bench = bank_shape((noise(x, z, 7, 782) - .38) / .31);
    double ridge = 1 - fabs(2 * noise(x, z, 19, 124) - 1);
    struct Climate c = { woods, sand, ice,
                         360
                             + outskirts
                                   * ((broad - .45) * (1280 + ice * 1950)
                                      + (bench - .45) * (420 + 510 * sand)
                                      + ridge * ridge * (280 + 820 * ice)) };
    double spine = 1 - fabs(2 * noise(x, z, 12, 2478) - 1);
    c.mountain = mountain;
    c.height += outskirts * (210 * (noise(x, z, 5, 192) - .5) + 260 * spine * spine)
                + mountain * (700 + 3500 * spine * spine + 1800 * broad);
    // A warped family of continuous valleys feeds a common water elevation.
    // Basin radius and frequency follow moisture, with small isolated desert oases.
    double moisture = 1 - .82 * sand;
    double riverDist = river_distance(x, z);
    double width = (1.6 + 1.9 * moisture) * (1 - .3 * mountain);
    double channel = smooth(1 - riverDist / (width + 3.8));
    double basin = 0, uplift = 0;
    s64 bx = (s64) floor((x + 16.) / 32), bz = (s64) floor((z + 16.) / 32);
    u32 lake = hash(bx, bz, 3217);
    double cx = 32. * bx + ((int) ((lake >> 8) & 7) - 3),
           cz = 32. * bz + ((int) ((lake >> 12) & 7) - 3);
    double basinMoisture = 1 - .82 * smooth((.52 - noise(cx, cz, 80, 32421)) / .20);
    if ((lake & 255) < 45 + 150 * basinMoisture) {
        double radius = (2.6 + ((lake >> 16) & 7) * .85) * (.55 + .45 * basinMoisture);
        double ellipse =
            hypot((x - cx) / radius, (z - cz) / (radius * (.68 + .04 * ((lake >> 24) & 7))));
        basin = smooth(1 - ellipse / 1.55);
        if (ellipse < 1.55) {
            double alpine = smooth((noise(cx, cz, 51, 8609) - .55) / .24);
            uplift = 1650 * alpine * smooth((river_distance(cx, cz) - 19) / 7)
                     * smooth((1.55 - ellipse) / .55) * smooth((fmax(fabs(cx), fabs(cz)) - 38) / 12);
        }
    }
    double carve = fmax(channel, basin);
    double dry = WATER_Y + 85 + 150 * log1p(exp((c.height - WATER_Y - 85) / 150));
    // Lower the surrounding valley in stages so a stream through mountains
    // has banks and approach slopes as well as a submerged bed.
    double valley = smooth(1 - riverDist / (width + 10));
    double bank = lerp(dry, fmin(dry, WATER_Y + 460), valley);
    c.height =
        lerp(c.height, lerp(bank, WATER_Y - 530 - 250 * basin, carve), smooth((distance - 25) / 9));
    double snowline = smooth((c.height - 3100) / 1900) * c.mountain;
    c.ice = lerp(c.ice, 1, snowline);
    c.sand *= 1 - snowline;
    c.waterY = WATER_Y + uplift;
    c.height += uplift;
    double pad = smooth((10 - fabs((double) x)) / 3) * smooth((-z - 14.) / 2) * smooth((29. + z) / 4);
    c.height = lerp(c.height, COURT_Y - 232, pad);
    return c;
}
struct TerrainVertex {
    struct Climate climate;
    double shade;
    u8 stone;
};
static struct TerrainVertex terrain_vertex(s64 x, s64 z) {
    double sx = (sample(x + 1, z).height - sample(x - 1, z).height) / (2 * CELL);
    double sz = (sample(x, z + 1).height - sample(x, z - 1).height) / (2 * CELL);
    struct Climate climate = sample(x, z);
    return (struct TerrainVertex) {
        climate, .64 + .36 * fmax(0, (.82 - .48 * sx - .31 * sz) / sqrt(1 + sx * sx + sz * sz)),
        fmax(190 * climate.mountain * smooth((climate.height - 700) / 1700),
             215 * smooth((hypot(sx, sz) - .32) / .38))
            * (1 - .65 * climate.ice)
    };
}
static int paint_vertex(Vtx *v, Vec3s pos, int u, int t, int pass, const struct TerrainVertex *point) {
    struct Climate c = point->climate;
    int alpha = pass == 3   ? point->stone
                : pass == 2 ? 255 * (1 - c.ice - c.sand)
                : pass == 1 ? 255 * smooth(c.ice * 8)
                            : 255;
    double woods = pass == 2 ? c.woods : 0;
    make_vertex(v, 0, pos[0], pos[1], pos[2], u, t, (255 - 87 * woods) * point->shade,
                (255 - 47 * woods) * point->shade, (255 - 65 * woods) * point->shade, alpha);
    return alpha;
}
static const u8 *terrain_texture(int pass) {
    const u8 *textures[] = { generic_09002800, snow_09008800, outside_09003800, generic_09003000 };
    return textures[pass];
}
// Keep a terrain backdrop around the landmark after the collision window
// moves away. Tiles share the streaming grid's boundary vertices and UVs.
enum {
    BACK_RADIUS = 48,
    TILE = 4,
    BACK_SIDE = BACK_RADIUS * 2 / TILE,
    BACK_COUNT = BACK_SIDE * BACK_SIDE
};
static Vtx backVertices[4][BACK_COUNT][40];
static Gfx backTiles[4][BACK_COUNT][21], backList[4][BACK_COUNT + 32];
static u8 backVisible[4][BACK_COUNT];
static Vtx backWaterVertices[BACK_COUNT][TILE * TILE * 8];
static Gfx backWaterTiles[BACK_COUNT][TILE * TILE * 4 + 1], backWaterList[BACK_COUNT + 32];
static bool backWet[BACK_COUNT];
static Gfx *backWaterScroll;
extern u8 texture_waterbox_water[];
static Gfx *water_cell(Gfx *dl, Vtx *verts, int x, int z, const double h[4], const double level[4]);
static Gfx *feature_begin(Gfx *dl, const u8 *texture, bool clamp);
static void feature_end(Gfx *dl);

static void build_backdrop(void) {
    for (int tz = 0; tz < BACK_SIDE; tz++)
        for (int tx = 0; tx < BACK_SIDE; tx++) {
            int tile = tz * BACK_SIDE + tx, x = tx * TILE - BACK_RADIUS, z = tz * TILE - BACK_RADIUS;
            if (x >= -HOME && x < HOME && z >= -HOME && z < HOME)
                continue;
            struct TerrainVertex grid[TILE + 1][TILE + 1];
            for (int dz = 0; dz <= TILE; dz++)
                for (int dx = 0; dx <= TILE; dx++)
                    grid[dz][dx] = terrain_vertex(x + dx, z + dz);
            Gfx *water = backWaterTiles[tile];
            for (int dz = 0; dz < TILE; dz++)
                for (int dx = 0; dx < TILE; dx++) {
                    double h[4], level[4];
                    for (int i = 0; i < 4; i++) {
                        struct Climate c = grid[dz + (i >> 1)][dx + (i & 1)].climate;
                        h[i] = lround(c.height);
                        level[i] = lround(c.waterY);
                    }
                    water = water_cell(water, &backWaterVertices[tile][8 * (TILE * dz + dx)],
                                       (x + dx) * CELL, (z + dz) * CELL, h, level);
                }
            backWet[tile] = water != backWaterTiles[tile];
            gSPEndDisplayList(water++);
            for (int pass = 0; pass < 4; pass++) {
                Gfx *dl = backTiles[pass][tile];
                int visible = 0;
                for (int row = 0; row < TILE; row++) {
                    for (int k = 0; k < 10; k++) {
                        int dx = k / 2, dz = row + (k & 1);
                        struct TerrainVertex *p = &grid[dz][dx];
                        Vec3s pos = { (x + dx) * CELL, lround(p->climate.height), (z + dz) * CELL };
                        visible |= paint_vertex(&backVertices[pass][tile][row * 10 + k], pos, dx * 2048,
                                                dz * 2048, pass, p);
                    }
                    gSPVertex(dl++, &backVertices[pass][tile][row * 10], 10, 0);
                    for (int k = 0; k < 8; k += 2)
                        gSP2Triangles(dl++, k, k + 1, k + 2, 0, k + 2, k + 1, k + 3, 0);
                }
                gSPEndDisplayList(dl++);
                backVisible[pass][tile] = visible;
            }
        }
}
static void select_backdrop(void) {
    Gfx *water = feature_begin(backWaterList, texture_waterbox_water, false);
    backWaterScroll = water++;
    for (int pass = 0; pass < 4; pass++) {
        Gfx *dl = backList[pass];
        gDPPipeSync(dl++);
        gSPClearGeometryMode(dl++, G_LIGHTING | G_CULL_BACK);
        gDPSetCombineMode(dl++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
        gSPTexture(dl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
        const u8 *texture = terrain_texture(pass);
        gDPLoadTextureBlock(dl++, texture, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, G_TX_WRAP, G_TX_WRAP,
                            5, 5, G_TX_NOLOD, G_TX_NOLOD);
        for (int tz = 0; tz < BACK_SIDE; tz++)
            for (int tx = 0; tx < BACK_SIDE; tx++) {
                int x = tx * TILE - BACK_RADIUS, z = tz * TILE - BACK_RADIUS;
                if (x >= centerX - SIDE / 2 && x < centerX + SIDE / 2 && z >= centerZ - SIDE / 2
                    && z < centerZ + SIDE / 2)
                    continue;
                int tile = tz * BACK_SIDE + tx;
                if (!pass && backWet[tile])
                    gSPDisplayList(water++, backWaterTiles[tile]);
                if (backVisible[pass][tile])
                    gSPDisplayList(dl++, backTiles[pass][tile]);
            }
        gSPTexture(dl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);
        gSPSetGeometryMode(dl++, G_LIGHTING | G_CULL_BACK);
        gDPSetCombineMode(dl++, G_CC_SHADE, G_CC_SHADE);
        gSPEndDisplayList(dl++);
    }
    feature_end(water);
}
Gfx *geo_landscape_backdrop(s32 context, struct GraphNode *node, UNUSED Mat4 matrix) {
    if (context != GEO_CONTEXT_RENDER)
        return NULL;
    int pass = ((struct GraphNodeGenerated *) node)->parameter;
    if (pass == 4) {
        node->flags = (node->flags & 255) | (LAYER_TRANSPARENT_INTER << 8);
        if (backWaterScroll) {
            int scroll = gGlobalTimer & 127;
            gDPSetTileSize(backWaterScroll, G_TX_RENDERTILE, scroll, 0, scroll + 124, 124);
        }
        return backWaterList;
    }
    node->flags = (node->flags & 0xff) | ((pass ? LAYER_TRANSPARENT_DECAL : LAYER_OPAQUE) << 8);
    return backList[pass];
}
// Solid scenery is part of the same rolling collision partition as the ground.
enum { FEATURE_SITES = (SIDE / 4) * (SIDE / 4), BLOOMS = 7 };
static Vtx rockVertices[FEATURE_SITES * ROCK_FACES * 3], flowerVertices[FEATURE_SITES * BLOOMS * 8];
static Vtx sprigs[FEATURE_SITES * BLOOMS * 6];
static Gfx stemList[FEATURE_SITES * BLOOMS * 2 + 16];
static Gfx rockList[2 * FEATURE_SITES * ROCK_FACES + 32], flowerList[4 * FEATURE_SITES * BLOOMS + 32];
extern u8 wiggler_seg5_texture_05008230[];
static Vtx waterVertices[PATCHES * 8];
static Gfx waterList[4 * PATCHES + 32];
static Gfx *waterScroll;

static double triangle_height(const double h[4], double u, double v) {
    if (u + v <= 1)
        return h[0] + u * (h[1] - h[0]) + v * (h[2] - h[0]);
    return h[3] + (1 - u) * (h[2] - h[3]) + (1 - v) * (h[1] - h[3]);
}
f32 landscape_water_level(f32 x, f32 z, f32 existing) {
    if (gCurrLevelNum != LEVEL_CASTLE_GROUNDS || gCurrAreaIndex != 1)
        return existing;
    double gx = x / CELL, gz = z / CELL;
    s64 wx = originX + (s64) floor(gx), wz = originZ + (s64) floor(gz);
    if (llabs(wx) <= HOME && llabs(wz) <= HOME)
        return existing;
    s64 ix = wx - centerX + SIDE / 2, iz = wz - centerZ + SIDE / 2;
    double h[4], level[4];
    for (int i = 0; i < 4; i++) {
        int dx = i & 1, dz = i >> 1;
        if (ix >= 0 && ix < SIDE && iz >= 0 && iz < SIDE) {
            int k = (iz + dz) * ROW + ix + dx;
            h[i] = heights[k];
            level[i] = waterHeights[k];
        } else {
            struct Climate c = sample(wx + dx, wz + dz);
            h[i] = lround(c.height);
            level[i] = lround(c.waterY);
        }
    }
    double u = gx - floor(gx), v = gz - floor(gz), water = triangle_height(level, u, v);
    return triangle_height(h, u, v) < water ? water : existing;
}
static Gfx *feature_begin(Gfx *dl, const u8 *texture, bool clamp) {
    gDPPipeSync(dl++);
    gSPClearGeometryMode(dl++, G_LIGHTING | G_CULL_BACK);
    gDPSetCombineMode(dl++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
    gSPTexture(dl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
    int wrap = clamp ? G_TX_CLAMP : G_TX_WRAP;
    gDPLoadTextureBlock(dl++, texture, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, wrap, wrap, 5, 5,
                        G_TX_NOLOD, G_TX_NOLOD);
    return dl;
}
static void feature_end(Gfx *dl) {
    gSPTexture(dl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);
    gSPSetGeometryMode(dl++, G_LIGHTING | G_CULL_BACK);
    gDPSetCombineMode(dl++, G_CC_SHADE, G_CC_SHADE);
    gSPEndDisplayList(dl++);
}
// Clip each water polygon on the very same diagonal used by Mario's floor.
static Gfx *water_cell(Gfx *dl, Vtx *verts, int x, int z, const double h[4], const double level[4]) {
    const int corners[2][3] = { { 0, 2, 1 }, { 1, 2, 3 } };
    for (int t = 0; t < 2; t++) {
        int count = 0;
        for (int k = 0; k < 3; k++) {
            int a = corners[t][k], b = corners[t][(k + 1) % 3];
            double u = a & 1, v = a >> 1;
            if (h[a] < level[a]) {
                make_vertex(verts, count++, x + u * CELL, level[a], z + v * CELL, u * 1024, v * 1024,
                            134, 207, 232, 174);
            }
            if ((h[a] < level[a]) != (h[b] < level[b])) {
                double f = (level[a] - h[a]) / (h[b] - level[b] - h[a] + level[a]);
                u = lerp(u, b & 1, f);
                v = lerp(v, b >> 1, f);
                make_vertex(verts, count++, x + lround(u * CELL), lround(lerp(level[a], level[b], f)),
                            z + lround(v * CELL), lround(u * 1024), lround(v * 1024), 157, 219, 230,
                            174);
            }
        }
        if (count >= 3) {
            gSPVertex(dl++, verts, count, 0);
            if (count == 4) {
                gSP2Triangles(dl++, 0, 1, 2, 0, 0, 2, 3, 0);
            } else {
                gSP1Triangle(dl++, 0, 1, 2, 0);
            }
        }
        verts += 4;
    }
    return dl;
}
static Gfx *rock_face(Gfx *dl, Vtx *v, struct Surface **collision, Vec3s a, Vec3s b, Vec3s c,
                      int shade) {
    if (!*collision)
        *collision = smlua_collision_add_surface(false, SURFACE_NOT_SLIPPERY, a, b, c);
    // Planar mapping keeps the grain continuous across adjacent side triangles.
    s16 *p[3] = { a, b, c };
    double nx = (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]);
    double ny = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
    double nz = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    int u = fabs(nx) > fabs(nz) ? 2 : 0, t = fabs(ny) > fmax(fabs(nx), fabs(nz)) ? (u == 2 ? 0 : 2) : 1;
    for (int i = 0; i < 3; i++) {
        int uv = ((a[u] * 4) & 1023) + (p[i][u] - a[u]) * 4;
        int tv = ((a[t] * 4) & 1023) + (p[i][t] - a[t]) * 4;
        make_vertex(v, i, p[i][0], p[i][1], p[i][2], uv, tv, shade, shade, shade, 255);
    }
    gSPVertex(dl++, v, 3, 0);
    gSP1Triangle(dl++, 0, 1, 2, 0);
    return dl;
}
static Gfx *boulder(Gfx *dl, int site, struct GroundCell *cell, const double groundY[4], u32 seed) {
    Vec3s base[ROCK_SIDES], shoulder[ROCK_SIDES];
    int cx = cell->localX + CELL / 2, cz = cell->localZ + CELL / 2;
    double radius = 65 + (seed & 127), wide = .53 + ((seed >> 8) & 63) / 120.,
           height = radius * (.40 + ((seed >> 16) & 127) / 145.);
    double groundCenter = triangle_height(groundY, .5, .5);
    Vec3s peak = { cx + (int) (radius * .17), lround(groundCenter + height), cz };
    for (int i = 0; i < ROCK_SIDES; i++) {
        u32 r = hash(cell->x, cell->z, seed + i);
        double angle = (i + (seed % 73) / 73. + (r & 31) / 160.) * 6.283185307 / ROCK_SIDES;
        double extent = radius * (.82 + ((r >> 8) & 31) / 140.);
        double dx = cos(angle) * extent, dz = sin(angle) * extent * wide;
        base[i][0] = cx + lround(dx);
        base[i][2] = cz + lround(dz);
        base[i][1] = lround(triangle_height(groundY, .5 + dx / CELL, .5 + dz / CELL)) - 25;
        shoulder[i][0] = cx + lround(dx * .83);
        shoulder[i][2] = cz + lround(dz * .83);
        shoulder[i][1] = lround(groundCenter + height * (.56 + ((r >> 16) & 31) / 155.));
    }
    for (int i = 0; i < ROCK_SIDES; i++) {
        int j = (i + 1) % ROCK_SIDES, f = 3 * i;
        int shade = 161 + 32 * cos((i + .2) * 6.283185307 / ROCK_SIDES);
        Vtx *v = &rockVertices[(site * ROCK_FACES + f) * 3];
        dl = rock_face(dl, v, &cell->rock[f], base[i], shoulder[i], base[j], shade);
        dl = rock_face(dl, v + 3, &cell->rock[f + 1], base[j], shoulder[i], shoulder[j], shade + 5);
        dl = rock_face(dl, v + 6, &cell->rock[f + 2], shoulder[i], peak, shoulder[j], shade + 35);
    }
    return dl;
}
static void build_features(const struct TerrainVertex *points) {
    Gfx *water = feature_begin(waterList, texture_waterbox_water, false);
    waterScroll = water++;
    Gfx *rocks = feature_begin(rockList, generic_09002000, false);
    Gfx *stems = stemList;
    gDPPipeSync(stems++);
    gSPClearGeometryMode(stems++, G_LIGHTING | G_CULL_BACK);
    gDPSetCombineMode(stems++, G_CC_SHADE, G_CC_SHADE);
    gSPTexture(stems++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);
    Gfx *flowers = feature_begin(flowerList, wiggler_seg5_texture_05008230, true);
    for (int z = 0; z < SIDE; z++)
        for (int x = 0; x < SIDE; x++) {
            s64 wx = centerX + x - SIDE / 2, wz = centerZ + z - SIDE / 2;
            if (wx >= -HOME && wx < HOME && wz >= -HOME && wz < HOME)
                continue;
            int k = z * ROW + x, lx = (wx - originX) * CELL, lz = (wz - originZ) * CELL;
            double h[4] = { heights[k], heights[k + 1], heights[k + ROW], heights[k + ROW + 1] };
            double level[4] = { waterHeights[k], waterHeights[k + 1], waterHeights[k + ROW],
                                waterHeights[k + ROW + 1] };
            water = water_cell(water, &waterVertices[(z * SIDE + x) * 8], lx, lz, h, level);
            if (fmin(fmin(h[0] - level[0], h[1] - level[1]), fmin(h[2] - level[2], h[3] - level[3]))
                <= 12)
                continue;
            int site = (z / 4) * (SIDE / 4) + x / 4;
            u32 seed = hash((wx - (x % 4)) / 4, (wz - (z % 4)) / 4, 671);
            if ((x % 4) != (int) ((seed >> 10) & 3) || (z % 4) != (int) ((seed >> 14) & 3))
                continue;
            struct Climate c = points[k].climate;
            if ((seed & 255) < 42 + 125 * c.mountain + 35 * c.ice + 24 * c.sand) {
                int rx = (wx % SIDE + SIDE) % SIDE, rz = (wz % SIDE + SIDE) % SIDE;
                rocks = boulder(rocks, site, &ground[rz * SIDE + rx], h, seed);
            } else if (c.woods < .48 && c.sand < .45 && c.ice < .45 && c.mountain < .42
                       && ((seed >> 19) & 3) != 0) {
                for (int bloom = 0; bloom < BLOOMS; bloom++) {
                    u32 r = hash(wx, wz, seed + bloom);
                    double u = .12 + (r & 255) / 340., v = .12 + ((r >> 8) & 255) / 340.;
                    int px = lx + lround(CELL * u), pz = lz + lround(CELL * v),
                        py = lround(triangle_height(h, u, v));
                    int size = 14 + ((r >> 16) & 15);
                    Vtx *leaf = &sprigs[(site * BLOOMS + bloom) * 6];
                    make_vertex(leaf, 0, px - 3, py - 2, pz, 0, 0, 43, 109, 29, 255);
                    make_vertex(leaf, 1, px + 3, py - 2, pz + 2, 0, 0, 59, 131, 36, 255);
                    make_vertex(leaf, 2, px + 2, py + size + 6, pz, 0, 0, 80, 156, 47, 255);
                    make_vertex(leaf, 3, px, py + 4, pz, 0, 0, 52, 115, 25, 255);
                    make_vertex(leaf, 4, px - size, py + size / 2, pz + 4, 0, 0, 90, 154, 34, 255);
                    make_vertex(leaf, 5, px - 4, py + size / 2, pz + 6, 0, 0, 59, 131, 36, 255);
                    gSPVertex(stems++, leaf, 6, 0);
                    gSP2Triangles(stems++, 0, 1, 2, 0, 3, 4, 5, 0);
                    Vtx *verts = &flowerVertices[(site * BLOOMS + bloom) * 8];
                    for (int axis = 0; axis < 2; axis++) {
                        for (int n = 0; n < 4; n++) {
                            int side = (n & 1) ? size : -size;
                            make_vertex(verts, 4 * axis + n, px + (axis ? 0 : side),
                                        py + size / 2 + (n >> 1) * size * 2, pz + (axis ? side : 0),
                                        (n & 1) * 992, (1 - (n >> 1)) * 992, 255, 245, 219, 255);
                        }
                        gSPVertex(flowers++, &verts[4 * axis], 4, 0);
                        gSP2Triangles(flowers++, 0, 1, 2, 0, 2, 1, 3, 0);
                    }
                }
            }
        }
    feature_end(water);
    feature_end(rocks);
    feature_end(flowers);
    gSPSetGeometryMode(stems++, G_LIGHTING | G_CULL_BACK);
    gSPEndDisplayList(stems++);
}
Gfx *geo_landscape_features(s32 context, struct GraphNode *node, UNUSED Mat4 matrix) {
    if (context != GEO_CONTEXT_RENDER)
        return NULL;
    int pass = ((struct GraphNodeGenerated *) node)->parameter;
    node->flags = (node->flags & 255)
                  | ((pass == 0   ? LAYER_OPAQUE
                      : pass == 1 ? LAYER_ALPHA
                                  : LAYER_TRANSPARENT_INTER)
                     << 8);
    if (pass == 3) {
        node->flags = (node->flags & 255) | (LAYER_OPAQUE << 8);
        return stemList;
    }
    if (pass == 2 && waterScroll) {
        int scroll = gGlobalTimer & 127;
        gDPSetTileSize(waterScroll, G_TX_RENDERTILE, scroll, 0, scroll + 124, 124);
    }
    return pass == 0 ? rockList : pass == 1 ? flowerList : waterList;
}

static void build(void) {
    select_backdrop();
    struct TerrainVertex points[ROW * ROW];
    for (int z = 0; z < ROW; z++)
        for (int x = 0; x < ROW; x++) {
            int k = z * ROW + x;
            points[k] = terrain_vertex(centerX + x - SIDE / 2, centerZ + z - SIDE / 2);
            heights[k] = lround(points[k].climate.height);
            waterHeights[k] = lround(points[k].climate.waterY);
        }
    for (int pass = 0; pass < 4; pass++) {
        Gfx *dl = lists[pass];
        gDPPipeSync(dl++);
        gSPClearGeometryMode(dl++, G_LIGHTING | G_CULL_BACK);
        gDPSetCombineMode(dl++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
        gSPTexture(dl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
        const u8 *texture = terrain_texture(pass);
        gDPLoadTextureBlock(dl++, texture, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, G_TX_WRAP, G_TX_WRAP,
                            5, 5, G_TX_NOLOD, G_TX_NOLOD);
        for (int z = 0; z < SIDE; z++)
            for (int x = 0; x < SIDE; x++) {
                int cell = z * SIDE + x, k = z * ROW + x;
                s64 wx = centerX + x - SIDE / 2, wz = centerZ + z - SIDE / 2;
                int ringX = (wx % SIDE + SIDE) % SIDE, ringZ = (wz % SIDE + SIDE) % SIDE;
                struct GroundCell *g = &ground[ringZ * SIDE + ringX];
                int localX = (wx - originX) * CELL, localZ = (wz - originZ) * CELL;
                if (!pass && g->tri[0]
                    && (g->x != wx || g->z != wz || g->localX != localX || g->localZ != localZ)) {
                    for (int t = 0; t < ROCK_FACES; t++) {
                        delete_surface(g->rock[t]);
                        g->rock[t] = NULL;
                    }
                    delete_surface(g->tri[0]);
                    delete_surface(g->tri[1]);
                    g->tri[0] = g->tri[1] = NULL;
                }
                if (wx >= -HOME && wx < HOME && wz >= -HOME && wz < HOME)
                    continue;
                Vec3s p[4];
                int visible = 0;
                for (int corner = 0; corner < 4; corner++) {
                    int dx = corner & 1, dz = corner >> 1, n = k + dx + dz * ROW;
                    p[corner][0] = (wx + dx - originX) * CELL;
                    p[corner][1] = heights[n];
                    p[corner][2] = (wz + dz - originZ) * CELL;
                    visible |= paint_vertex(&vertices[pass][cell * 4 + corner], p[corner], dx * 2048,
                                            dz * 2048, pass, &points[n]);
                }
                if (!pass && !g->tri[0]) {
                    g->x = wx;
                    g->z = wz;
                    g->localX = localX;
                    g->localZ = localZ;
                    s16 surface = SURFACE_NOT_SLIPPERY;
                    g->tri[0] = smlua_collision_add_surface(false, surface, p[0], p[2], p[1]);
                    g->tri[1] = smlua_collision_add_surface(false, surface, p[1], p[2], p[3]);
                    for (int t = 0; t < 2; t++)
                        if (g->tri[t])
                            g->tri[t]->flags |= LANDSCAPE_WILD_SLOPE;
                }
                if (!visible)
                    continue;
                gSPVertex(dl++, &vertices[pass][cell * 4], 4, 0);
                gSP2Triangles(dl++, 0, 2, 1, 0, 1, 2, 3, 0);
            }
        gSPTexture(dl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);
        gSPSetGeometryMode(dl++, G_LIGHTING | G_CULL_BACK);
        gDPSetCombineMode(dl++, G_CC_SHADE, G_CC_SHADE);
        gSPEndDisplayList(dl++);
    }
    build_features(points);
}

enum { BLOCKS = 6, SLOTS = 3 };
struct Site {
    bool occupied;
    s64 x, z;
    struct Object *actors[SLOTS];
    const BehaviorScript *behavior[SLOTS];
};
static struct Site sites[BLOCKS * BLOCKS];
static bool live(struct Site *s, int i) {
    struct Object *o = s->actors[i];
    return o && o->activeFlags && o->behavior == s->behavior[i]
           && o->unused1 == (u32) (1 + (s - sites)) * SLOTS + i;
}
static void place(struct Site *s, int slot, int model, const BehaviorScript *bhv, s64 x, s64 z,
                  float scale) {
    struct Climate climate = sample(x, z);
    if (climate.height < climate.waterY + 20 || authored(x, z)
        || (llabs(x) < 14 && z >= -25 && z <= -14))
        return;
    int lx = (x - originX) * CELL, lz = (z - originZ) * CELL;
    float y = !originX && !originZ && llabs(x) <= HOME && llabs(z) <= HOME
                  ? find_floor_height(lx, 12000, lz)
                  : sample(x, z).height;
    struct Object *o =
        spawn_object_abs_with_rot(gMarioObject, 0, model, bhv, lx, lround(y), lz, 0, hash(x, z, 5), 0);
    if (o) {
        // Sites stream independently on each client; their local coordinates differ after a rebase.
        o->coopFlags |= COOP_OBJ_FLAG_NON_SYNC;
        o->unused1 = (u32) (1 + (s - sites)) * SLOTS + slot;
        obj_scale(o, scale);
    }
    s->actors[slot] = o;
    s->behavior[slot] = bhv;
}
static void populate(int shiftX, int shiftZ) {
    for (int j = 0; j < BLOCKS; j++)
        for (int i = 0; i < BLOCKS; i++) {
            s64 bx = centerX / 4 + i - 3, bz = centerZ / 4 + j - 3;
            int id = (bx % BLOCKS + BLOCKS) % BLOCKS + BLOCKS * ((bz % BLOCKS + BLOCKS) % BLOCKS);
            struct Site *s = &sites[id];
            if (s->occupied && s->x == bx && s->z == bz) {
                if (shiftX || shiftZ)
                    for (int k = 0; k < SLOTS; k++)
                        if (live(s, k)) {
                            struct Object *o = s->actors[k];
                            o->oPosX -= shiftX;
                            o->oPosZ -= shiftZ;
                            o->oHomeX -= shiftX;
                            o->oHomeZ -= shiftZ;
                            o->header.gfx.pos[0] = o->oPosX;
                            o->header.gfx.pos[2] = o->oPosZ;
                        }
                continue;
            }
            for (int k = 0; k < SLOTS; k++)
                if (live(s, k))
                    obj_mark_for_deletion(s->actors[k]);
            *s = (struct Site) { .occupied = true, .x = bx, .z = bz };
            u32 h = hash(bx, bz, 9147);
            s64 x = bx * 4 + ((h >> 8) & 3), z = bz * 4 + ((h >> 12) & 3);
            struct Climate c = sample(x, z);
            if ((h & 255) < 85 + 150 * c.woods - 50 * c.sand - 65 * c.mountain)
                place(s, 0,
                      c.ice > .55 || c.mountain > .55 ? MODEL_CCM_SNOW_TREE
                      : c.sand > .55                  ? MODEL_SSL_PALM_TREE
                                                      : MODEL_CASTLE_GROUNDS_BUBBLY_TREE,
                      bhvTree, x, z, 1);
            s64 rx = bx * 4 + ((h >> 17) & 3), rz = bz * 4 + ((h >> 20) & 3);
            if ((h >> 24) < 85 + 70 * (c.sand + c.ice)) {
                if (c.ice < .4 && c.sand < .45 && c.mountain < .45)
                    place(s, 1, MODEL_BUTTERFLY, bhvButterfly, rx, rz, 1);
                else if (c.ice > .65 && (h & 1))
                    place(s, 1, MODEL_CCM_SNOWMAN_BASE, bhvStaticObject, rx, rz, .48);
                else if (c.sand > .65 && (h & 3) == 0)
                    place(s, 1, MODEL_SSL_PYRAMID_TOP, bhvStaticObject, rx, rz, .32);
            }
            // One possible encounter in each group of four sites, giving at most nine
            // in the entire active window. The broad home meadow stays safe.
            if (((bx & 1) == 0) && ((bz & 1) == 0) && (h >> 24) < 180
                && fmax(llabs(x), llabs(z)) > 22) {
                int model = MODEL_GOOMBA;
                const BehaviorScript *behavior = bhvGoomba;
                if (c.ice > .5 || (c.mountain > .65 && c.height > 2100)) {
                    model = MODEL_LANDSCAPE_SPINDRIFT;
                    behavior = bhvSpindrift;
                } else if (c.sand > .5) {
                    model = MODEL_FLYGUY;
                    behavior = bhvFlyGuy;
                } else if (c.woods > .5) {
                    model = MODEL_LANDSCAPE_PIRANHA;
                    behavior = bhvPiranhaPlant;
                }
                place(s, 2, model, behavior, bx * 4 + 2, bz * 4 + 2, 1);
            }
        }
}

// Join the irregular authored boundary to the first regular wilderness grid row.
// Each radial strip starts just inside the original floor. Its far edge lies
// exactly on the grid, including the interpolated heights between grid vertices.
enum { EDGE_STEPS = HOME * 4, RIM_POINTS = 4 * EDGE_STEPS, RIM_ROWS = 4 };
static Vec3s rim[RIM_POINTS][RIM_ROWS + 1];
static Vtx rimVertices[RIM_POINTS * RIM_ROWS * 4];
static Gfx rimList[RIM_POINTS * RIM_ROWS * 2 + 32];

static bool stock_floor(float x, float z, float *height) {
    struct Surface *floor = NULL;
    *height = find_floor(x, 12000, z, &floor);
    return floor && floor->type != SURFACE_DEATH_PLANE;
}

static void prepare_home_collision(void) {
    for (int z = 0; z < NUM_CELLS; z++)
        for (int x = 0; x < NUM_CELLS; x++) {
            for (struct SurfaceNode *n = gStaticSurfacePartition[z][x][SPATIAL_PARTITION_FLOORS].next;
                 n; n = n->next) {
                struct Surface *f = n->surface;
                if (f->type == SURFACE_VERY_SLIPPERY || f->type == SURFACE_NOISE_DEFAULT
                    || f->type == SURFACE_NOISE_SLIPPERY) {
                    f->type = SURFACE_NOT_SLIPPERY;
                    f->flags |= LANDSCAPE_HILL;
                }
            }
            for (struct SurfaceNode *n = gStaticSurfacePartition[z][x][SPATIAL_PARTITION_WALLS].next; n;
                 n = n->next) {
                struct Surface *f = n->surface;
                if (f->flags & SURFACE_FLAG_INTANGIBLE)
                    continue;
                float px = (f->vertex1[0] + f->vertex2[0] + f->vertex3[0]) / 3.f;
                float pz = (f->vertex1[2] + f->vertex2[2] + f->vertex3[2]) / 3.f;
                if (fmaxf(fabsf(px), fabsf(pz)) < 5400)
                    continue;
                float y;
                if (!stock_floor(px + 32 * f->normal.x, pz + 32 * f->normal.z, &y)
                    || !stock_floor(px - 32 * f->normal.x, pz - 32 * f->normal.z, &y))
                    f->flags |= SURFACE_FLAG_INTANGIBLE;
            }
        }
}

static void build_home_rim(void) {
    for (int i = 0; i < RIM_POINTS; i++) {
        int along = (i % EDGE_STEPS) * CELL / 2 - HOME * CELL, side = i / EDGE_STEPS;
        int edgeX = side == 0 ? along : side == 1 ? HOME * CELL : side == 2 ? -along : -HOME * CELL;
        int edgeZ = side == 0 ? -HOME * CELL : side == 1 ? along : side == 2 ? HOME * CELL : -along;
        float length = hypotf(edgeX, edgeZ), ux = edgeX / length, uz = edgeZ / length;
        float edge = length, h = 260;
        while (edge > 128 && !stock_floor(edge * ux, edge * uz, &h))
            edge -= 48;
        float lo = edge, hi = edge + 48;
        for (int j = 0; j < 8; j++) {
            float mid = (lo + hi) * .5f;
            if (stock_floor(mid * ux, mid * uz, &h))
                lo = mid;
            else
                hi = mid;
        }
        edge = lo;
        // The northeast bank has a sheer step above the low path. Extend
        // the visible grass upslope from the path to the lip in this sector.
        double approach = uz < 0 ? smooth(1 - fabs(ux - .54) / .14) : 0;
        float inner = edge - 96 - 2600 * approach, innerY = 260, edgeY = 260;
        stock_floor(inner * ux, inner * uz, &innerY);
        stock_floor((edge - 3) * ux, (edge - 3) * uz, &edgeY);
        // The north ray hits the castle roof. Route the terrain below the
        // rear facade, at the garden foundation, instead of across the roof.
        if (edgeZ == -HOME * CELL) {
            double rear = smooth((4550 - fabs(edge * ux)) / 850);
            edgeY = lerp(edgeY, COURT_Y - 245, rear);
            innerY = lerp(innerY, COURT_Y - 245, rear);
        }
        int gx = (int) floor((double) edgeX / CELL), gz = (int) floor((double) edgeZ / CELL);
        double u = (double) edgeX / CELL - gx, v = (double) edgeZ / CELL - gz;
        double farY =
            lerp(lerp(lround(sample(gx, gz).height), lround(sample(gx + 1, gz).height), u),
                 lerp(lround(sample(gx, gz + 1).height), lround(sample(gx + 1, gz + 1).height), u), v);
        for (int j = 0; j <= RIM_ROWS; j++) {
            double t = j ? (j - 1.) / (RIM_ROWS - 1) : 0;
            double r = j ? lerp(edge, length, t) : inner;
            rim[i][j][0] = lround(ux * r);
            rim[i][j][2] = lround(uz * r);
            rim[i][j][1] = lround(j ? lerp(edgeY, farY, t) : innerY - 28);
        }
    }
    Gfx *dl = rimList;
    gDPPipeSync(dl++);
    gSPClearGeometryMode(dl++, G_LIGHTING | G_CULL_BACK);
    gDPSetCombineMode(dl++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
    gSPTexture(dl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
    gDPLoadTextureBlock(dl++, outside_09003800, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, G_TX_WRAP,
                        G_TX_WRAP, 5, 5, G_TX_NOLOD, G_TX_NOLOD);
    for (int i = 0; i < RIM_POINTS; i++)
        for (int j = 0; j < RIM_ROWS; j++) {
            int next = (i + 1) % RIM_POINTS, base = (i * RIM_ROWS + j) * 4;
            Vec3s *p[4] = { &rim[i][j], &rim[next][j], &rim[i][j + 1], &rim[next][j + 1] };
            for (int k = 0; k < 4; k++) {
                s16 *v = *p[k];
                int shade = 215 + 20 * noise(v[0] / (double) CELL, v[2] / (double) CELL, 3, 144);
                make_vertex(rimVertices, base + k, v[0], v[1], v[2],
                            (((*p[0])[0] * 4) & 1023) + (v[0] - (*p[0])[0]) * 4,
                            (((*p[0])[2] * 4) & 1023) + (v[2] - (*p[0])[2]) * 4, shade, shade, shade,
                            255);
            }
            smlua_collision_add_surface(false, SURFACE_NOT_SLIPPERY, *p[0], *p[1], *p[2]);
            smlua_collision_add_surface(false, SURFACE_NOT_SLIPPERY, *p[1], *p[3], *p[2]);
            // Keep the collision overlap for walking. Only draw the inner strip
            // where it forms the northeast ascent, avoiding coplanar grass.
            if (!j && rim[i][0][2] - rim[i][1][2] < 96 && rim[next][0][2] - rim[next][1][2] < 96) {
                // A short skirt closes the sub-cell difference at the stock edge.
                for (int k = 0; k < 2; k++) {
                    Vtx *v = &rimVertices[base + k];
                    Vtx edge = rimVertices[base + k + 2];
                    // Tuck the skirt into the bank to cover bends between samples.
                    for (int axis = 0; axis <= 2; axis += 2)
                        v->v.ob[axis] = (edge.v.ob[axis] + v->v.ob[axis]) / 2;
                    v->v.ob[1] = edge.v.ob[1] - 100;
                }
            }
            gSPVertex(dl++, &rimVertices[base], 4, 0);
            gSP2Triangles(dl++, 0, 1, 2, 0, 1, 3, 2, 0);
        }
    gSPTexture(dl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);
    gSPSetGeometryMode(dl++, G_LIGHTING | G_CULL_BACK);
    gDPSetCombineMode(dl++, G_CC_SHADE, G_CC_SHADE);
    gSPEndDisplayList(dl++);
}
// The native courtyard mesh and its collision share this translation with the
// door, fountain and garden actors. Ground stays resident across origin shifts.
static Vec3s foundation[128][2];
static int foundationCount;
static Vtx foundationVertices[128 * 4];
static Vtx courtPathVertices[2 * 3 * 4];
static Vtx towerVertices[2][8][5];
static Vtx pathSides[2 * 3 * 2 * 4];
static Gfx courtPathList[160 + 2 * 128];
static s16 courtWater[1 + 6 * 20];
static s16 courtSigns[4 * 5 + 1];

static void load_courtyard(void) {
    foundationCount = 0;
    const Collision *vertices = castle_courtyard_seg7_collision + 2;
    const Collision *data = vertices + 3 * castle_courtyard_seg7_collision[1];
    while (*data != TERRAIN_LOAD_CONTINUE) {
        s16 type = *data++, count = *data++;
        for (int i = 0; i < count; i++) {
            Vec3s p[3];
            for (int k = 0; k < 3; k++) {
                const Collision *v = vertices + 3 * *data++;
                p[k][0] = v[0];
                p[k][1] = v[1] + COURT_Y;
                p[k][2] = v[2] + COURT_Z;
            }
            smlua_collision_add_surface(false, type, p[0], p[1], p[2]);
            // Extend the lower edges of the tall courtyard walls to the terrain.
            for (int k = 0; k < 3; k++) {
                s16 *a = p[k], *b = p[(k + 1) % 3], *c = p[(k + 2) % 3];
                if (a[1] <= COURT_Y + 20 && b[1] <= COURT_Y + 20 && c[1] > COURT_Y + 500
                    && foundationCount < 128) {
                    memcpy(foundation[foundationCount][0], a, sizeof(Vec3s));
                    memcpy(foundation[foundationCount++][1], b, sizeof(Vec3s));
                }
            }
            if (surface_has_force(type))
                data++;
        }
    }
    // Give the fountain its own water ID alongside the original moat boxes.
    if (gEnvironmentRegions && gEnvironmentRegions[0] < 20) {
        int n = gEnvironmentRegionsLength;
        memcpy(courtWater, gEnvironmentRegions, n * sizeof(s16));
        s16 box[] = { COURT_WATER_ID, -656, COURT_Z - 2405, 674, COURT_Z - 1074, COURT_Y + 51 };
        memcpy(courtWater + n, box, sizeof(box));
        gEnvironmentLevels[courtWater[0]++] = COURT_Y + 51;
        gEnvironmentRegionsLength += 6;
        gEnvironmentRegions = courtWater;
    }
    memcpy(courtSigns, castle_courtyard_seg7_macro_objs, sizeof(courtSigns));
    for (int i = 0; i < 4; i++) {
        courtSigns[i * 5 + 2] += COURT_Y;
        courtSigns[i * 5 + 3] += COURT_Z;
    }
    spawn_macro_objects(1, courtSigns);
}

static void build_court_paths(void) {
    Gfx *dl = courtPathList;
    gDPPipeSync(dl++);
    gSPClearGeometryMode(dl++, G_LIGHTING | G_CULL_BACK);
    gDPSetCombineMode(dl++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
    gSPTexture(dl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
    gDPLoadTextureBlock(dl++, outside_0900A000, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, G_TX_WRAP,
                        G_TX_WRAP, 5, 5, G_TX_NOLOD, G_TX_NOLOD);
    for (int i = 0; i < foundationCount; i++) {
        for (int k = 0; k < 4; k++) {
            s16 *p = foundation[i][k & 1];
            make_vertex(foundationVertices, 4 * i + k, p[0], p[1] - (k >> 1) * 2300, p[2],
                        (k & 1)
                            * (int) hypot(foundation[i][0][0] - foundation[i][1][0],
                                          foundation[i][0][2] - foundation[i][1][2])
                            * 3,
                        (k >> 1) * 6900, 190, 190, 190, 255);
        }
        gSPVertex(dl++, &foundationVertices[4 * i], 4, 0);
        gSP2Triangles(dl++, 0, 1, 2, 0, 1, 3, 2, 0);
    }
    for (int side = 0; side < 2; side++) {
        int sign = side ? 1 : -1;
        // End the bridge on grid vertices so both ends meet the actual floor.
        int x[] = { 1792, 3392, 3776, 6656 };
        for (int j = 0; j < 3; j++) {
            Vec3s p[4];
            int base = (side * 3 + j) * 4;
            for (int k = 0; k < 4; k++) {
                int n = j + (k >> 1), z = -10752 + 512 * (k & 1);
                int wx = sign * x[n];
                int y = n == 0   ? COURT_Y - 205
                        : n == 3 ? lround(sample(wx / CELL, z / CELL).height) - 2
                                 : COURT_Y + 838;
                p[k][0] = wx;
                p[k][1] = y;
                p[k][2] = z;
                make_vertex(courtPathVertices, base + k, wx, y, z, (k >> 1) * (x[j + 1] - x[j]) * 3,
                            (k & 1) * 1536, 224, 224, 224, 255);
            }
            int a = side ? 1 : 2, b = 3 - a;
            smlua_collision_add_surface(false, SURFACE_NOT_SLIPPERY, p[0], p[a], p[b]);
            smlua_collision_add_surface(false, SURFACE_NOT_SLIPPERY, p[b], p[a], p[3]);
            for (int edge = 0; edge < 2; edge++) {
                int first = (base / 4 * 2 + edge) * 4;
                for (int k = 0; k < 4; k++) {
                    s16 *v = p[edge + (k & 1) * 2];
                    make_vertex(pathSides, first + k, v[0], v[1] - (k >> 1) * 1300, v[2],
                                (k & 1) * (x[j + 1] - x[j]) * 3, (k >> 1) * 3900, 170, 170, 170, 255);
                }
                gSPVertex(dl++, &pathSides[first], 4, 0);
                gSP2Triangles(dl++, 0, 1, 2, 0, 1, 3, 2, 0);
            }
            gSPVertex(dl++, &courtPathVertices[base], 4, 0);
            gSP2Triangles(dl++, 0, a, b, 0, b, a, 3, 0);
        }
    }
    // The enclosed original set has only the inward-facing tower panels.
    // Fit complete octagonal masonry and roof caps around their footprint.
    static const s16 ring[8][2] = { { -215, -532 }, { 215, -532 }, { 532, -215 }, { 532, 215 },
                                    { 215, 532 },   { -215, 532 }, { -532, 215 }, { -532, -215 } };
    for (int roof = 0; roof <= 1; roof++) {
        if (roof) {
            gDPLoadTextureBlock(dl++, outside_09007800, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0,
                                G_TX_WRAP, G_TX_WRAP, 5, 5, G_TX_NOLOD, G_TX_NOLOD);
        } else {
            gDPLoadTextureBlock(dl++, outside_09006800, G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, 32, 0,
                                G_TX_WRAP, G_TX_WRAP, 6, 5, G_TX_NOLOD, G_TX_NOLOD);
        }
        for (int side = 0; side < 2; side++)
            for (int e = 0; e < 8; e++) {
                int cx = side ? 3584 : -3583, cz = COURT_Z - 3583;
                Vtx *v = towerVertices[side][e];
                Vec3s p[5];
                for (int k = 0; k < 5; k++) {
                    int n = (e + (k & 1)) % 8;
                    p[k][0] = cx + (k == 4 ? 0 : ring[n][0]);
                    p[k][2] = cz + (k == 4 ? 0 : ring[n][1]);
                    p[k][1] = COURT_Y + (k == 4 ? 2450 : k >= 2 ? 2052 : -1350);
                    make_vertex(v, k, p[k][0], p[k][1], p[k][2], k == 4 ? 512 : (k & 1) * 1024,
                                k == 4  ? -1024
                                : k < 2 ? 8192
                                        : 0,
                                192 + 5 * (e % 5), 192 + 5 * (e % 5), 192 + 5 * (e % 5), 255);
                }
                gSPVertex(dl++, v, 5, 0);
                if (roof) {
                    gSP1Triangle(dl++, 2, 4, 3, 0);
                    smlua_collision_add_surface(false, SURFACE_NOT_SLIPPERY, p[2], p[4], p[3]);
                } else {
                    gSP2Triangles(dl++, 0, 2, 1, 0, 1, 2, 3, 0);
                    smlua_collision_add_surface(false, SURFACE_DEFAULT, p[0], p[2], p[1]);
                    smlua_collision_add_surface(false, SURFACE_DEFAULT, p[1], p[2], p[3]);
                }
            }
    }
    gSPTexture(dl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);
    gSPSetGeometryMode(dl++, G_LIGHTING | G_CULL_BACK);
    gDPSetCombineMode(dl++, G_CC_SHADE, G_CC_SHADE);
    gSPEndDisplayList(dl++);
}
Gfx *geo_landscape_court_path(s32 context, struct GraphNode *node, UNUSED Mat4 matrix) {
    if (context != GEO_CONTEXT_RENDER)
        return NULL;
    node->flags = (node->flags & 0xff) | (LAYER_OPAQUE << 8);
    return courtPathList;
}

Gfx *geo_landscape_rim(s32 context, struct GraphNode *node, UNUSED Mat4 mtx) {
    if (context != GEO_CONTEXT_RENDER)
        return NULL;
    node->flags = (node->flags & 0xff) | (LAYER_OPAQUE << 8);
    return rimList;
}

void landscape_load(s32 area) {
    if (gCurrLevelNum != LEVEL_CASTLE_GROUNDS || area != 1)
        return;
    originX = originZ = centerX = centerZ = 0;
    memset(ground, 0, sizeof(ground));
    memset(sites, 0, sizeof(sites));
    prepare_home_collision();
    // Sample the stock lawn-to-crest profile before adding the perimeter mesh.
    float bank[9];
    bool valid = true;
    for (int i = 0; i < 9; i++)
        valid &= stock_floor(-1328, 4700 + 250 * i, &bank[i]);
    for (int i = 0; i < 9; i++)
        castleBank[i] = valid && bank[8] > bank[0] + 1
                            ? fmax(0, fmin(1, (bank[i] - bank[0]) / (bank[8] - bank[0])))
                            : i / 8.;

    for (int z = -HOME; z <= HOME; z++)
        for (int x = -HOME; x <= HOME; x++) {
            struct Surface *floor = NULL;
            float y = find_floor(x * CELL, 12000, z * CELL, &floor);
            homeHeight[z + HOME][x + HOME] =
                floor && floor->type != SURFACE_DEATH_PLANE ? (s16) y : -11000;
        }
    build_backdrop();
    build_home_rim();
    load_courtyard();
    build_court_paths();
    build();
}

// Leave enough space for the original castle and its camera at the origin.
// Faraway coordinates wrap in an outer band and never alias the castle.
static int shift_axis(float local, s64 origin) {
    if (local > 24576 || (origin < 0 && local > -14336))
        return 8192;
    if (local < -24576 || (origin > 0 && local < 14336))
        return -8192;
    return 0;
}
void landscape_update(void) {
    if (gCurrLevelNum != LEVEL_CASTLE_GROUNDS || gCurrAreaIndex != 1 || !gMarioObject)
        return;
    struct MarioState *m = gMarioState;
    int dx = shift_axis(m->pos[0], originX), dz = shift_axis(m->pos[2], originZ);
    s64 cx = originX + 4 * (s64) floor(m->pos[0] / (4 * CELL) + .5);
    s64 cz = originZ + 4 * (s64) floor(m->pos[2] / (4 * CELL) + .5);
    if (cx == centerX && cz == centerZ && !dx && !dz && sites[0].occupied)
        return;
    originX += dx / CELL;
    originZ += dz / CELL;
    centerX = cx;
    centerZ = cz;
    build();
    for (int i = 0; i < MAX_PLAYERS; i++) {
        struct MarioState *p = &gMarioStates[i];
        if (!p->marioObj)
            continue;
        p->pos[0] -= dx;
        p->pos[2] -= dz;
        p->marioObj->oPosX = p->pos[0];
        p->marioObj->oPosZ = p->pos[2];
        p->nonInstantWarpPos[0] = p->pos[0];
        p->nonInstantWarpPos[2] = p->pos[2];
        p->wall = p->ceil = NULL;
        p->floorHeight = find_floor(p->pos[0], p->pos[1] + 100, p->pos[2], &p->floor);
    }
    if (dx || dz) {
        warp_camera(-dx, 0, -dz);
        skip_camera_interpolation();
    }
    populate(dx, dz);
}

Gfx *geo_landscape_home(s32 context, struct GraphNode *node, UNUSED Mat4 matrix) {
    if (context == GEO_CONTEXT_RENDER)
        ((struct GraphNodeSwitchCase *) node)->selectedCase =
            (llabs(originX) > 63 || llabs(originZ) > 63) ? 1 : 0;
    return NULL;
}

// Level actors simulate in authored coordinates. Translate their render matrix
// with the landmark while the streaming window uses a shifted origin.
void landscape_anchor_actor(Mat4 matrix, Mat4 parent) {
    if (!originX && !originZ)
        return;
    float x = fmax(-100000., fmin(100000., (double) originX * CELL));
    float z = fmax(-100000., fmin(100000., (double) originZ * CELL));
    for (int i = 0; i < 3; i++)
        matrix[3][i] -= x * parent[0][i] + z * parent[2][i];
}

bool landscape_hide_home_actor(const struct Object *o) {
    if (o->unused1 != LANDSCAPE_HOME_ACTOR || !gMarioState)
        return false;
    double dx = (double) originX * CELL + gMarioState->pos[0] - o->oPosX;
    double dz = (double) originZ * CELL + gMarioState->pos[2] - o->oPosZ;
    return dx * dx + dz * dz > 10500. * 10500.;
}

Gfx *geo_landscape_home_offset(s32 context, struct GraphNode *node, UNUSED Mat4 matrix) {
    if (context == GEO_CONTEXT_RENDER) {
        struct GraphNodeTranslation *translation = (struct GraphNodeTranslation *) node->next;
        translation->translation[0] = -originX * CELL;
        translation->translation[2] = -originZ * CELL;
    }
    return NULL;
}

Gfx *geo_landscape(s32 context, struct GraphNode *node, UNUSED Mat4 matrix) {
    if (context != GEO_CONTEXT_RENDER)
        return NULL;
    int pass = ((struct GraphNodeGenerated *) node)->parameter;
    node->flags = (node->flags & 0xff) | ((pass ? LAYER_TRANSPARENT_DECAL : LAYER_OPAQUE) << 8);
    return lists[pass];
}
