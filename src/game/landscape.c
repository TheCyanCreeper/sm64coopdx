#include <math.h>
#include <stdlib.h>
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

// Keep native s16 vertices and the collision partition near Mario. Global cell
// coordinates survive each origin shift and drive both vegetation and terrain.
enum { SIDE = 24, ROW = SIDE + 1, CELL = 512, SHIFT = 2048, PATCHES = SIDE * SIDE };
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

static Vtx vertices[3][PATCHES * 4];
static Gfx lists[3][PATCHES * 3 + 32];
static s16 heights[ROW * ROW];
static u8 snow[ROW * ROW];
static struct Object *trees[38];
static int plantTrees;
static const s16 emptyFloor[] = { COL_INIT(), COL_VERTEX_INIT(0), COL_TRI_STOP(), COL_END() };

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
struct Climate {
    double woods, sand, ice, height;
};
static struct Climate sample(s64 x, s64 z) {
    double distance = fmax(fabs((double) x), fabs((double) z));
    double outskirts = smooth((distance - 2) / 8);
    double weather = noise(x, z, 18, 32421);
    double ice = outskirts * smooth((weather - .53) / .22);
    double sand = outskirts * smooth((.52 - weather) / .20);
    double woods = (1 - ice - sand) * smooth((noise(x, z, 10, 419) - .35) / .35) * outskirts;
    double hills = noise(x, z, 9, 77), detail = noise(x, z, 3, 782);
    struct Climate c = {
        woods, sand, ice,
        outskirts * ((hills - .45) * (1050 + ice * 1800 + sand * 530) + (detail - .5) * 180)
    };
    return c;
}
static void build(void) {
    struct Climate climate[ROW * ROW];
    for (int z = 0; z < ROW; z++)
        for (int x = 0; x < ROW; x++) {
            int k = z * ROW + x;
            climate[k] = sample(originX + x - SIDE / 2, originZ + z - SIDE / 2);
            heights[k] = lround(climate[k].height);
            snow[k] = climate[k].ice > .65;
        }
    load_area_terrain(2, (s16 *) emptyFloor, NULL, NULL);
    for (int pass = 0; pass < 3; pass++) {
        Gfx *dl = lists[pass];
        gDPPipeSync(dl++);
        gSPClearGeometryMode(dl++, G_LIGHTING);
        gDPSetCombineMode(dl++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
        gSPTexture(dl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
        const u8 *texture = pass == 2 ? outside_09003800 : pass == 1 ? snow_09008800 : generic_09002800;
        gDPLoadTextureBlock(dl++, texture, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, G_TX_WRAP, G_TX_WRAP,
                            5, 5, G_TX_NOLOD, G_TX_NOLOD);
        for (int z = 0; z < SIDE; z++)
            for (int x = 0; x < SIDE; x++) {
                int cell = z * SIDE + x, k = z * ROW + x;
                Vec3s p[4];
                int visible = 0;
                for (int corner = 0; corner < 4; corner++) {
                    int dx = corner & 1, dz = corner >> 1, n = k + dx + dz * ROW;
                    struct Climate c = climate[n];
                    p[corner][0] = (x + dx - SIDE / 2) * CELL;
                    p[corner][1] = heights[n];
                    p[corner][2] = (z + dz - SIDE / 2) * CELL;
                    double shade = .89 + .11 * noise(originX + x + dx, originZ + z + dz, 3, 144);
                    int r = pass == 2 ? 255 - 87 * c.woods : 255;
                    int g = pass == 2 ? 255 - 47 * c.woods : 255;
                    int b = pass == 2 ? 255 - 65 * c.woods : 255;
                    int alpha = pass == 2   ? 255 * (1 - c.ice - c.sand)
                                : pass == 1 ? 255 * smooth(c.ice * 8)
                                            : 255;
                    visible |= alpha;
                    make_vertex(vertices[pass], cell * 4 + corner, p[corner][0], p[corner][1],
                                p[corner][2], dx * 2048, dz * 2048, r * shade, g * shade, b * shade,
                                alpha);
                }
                if (!pass) {
                    s64 wx = originX + x - SIDE / 2, wz = originZ + z - SIDE / 2;
                    s16 surface = (wx == -1 || wx == 0) && wz == -2 ? SURFACE_INSTANT_WARP_1B
                                  : snow[k]                         ? SURFACE_NOISE_SLIPPERY
                                                                    : SURFACE_DEFAULT;
                    smlua_collision_add_surface(false, surface, p[0], p[2], p[1]);
                    smlua_collision_add_surface(false, surface, p[1], p[2], p[3]);
                }
                if (!visible)
                    continue;
                gSPVertex(dl++, &vertices[pass][cell * 4], 4, 0);
                gSP2Triangles(dl++, 0, 2, 1, 0, 1, 2, 3, 0);
            }
        gSPTexture(dl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);
        gSPSetGeometryMode(dl++, G_LIGHTING);
        gDPSetCombineMode(dl++, G_CC_SHADE, G_CC_SHADE);
        gSPEndDisplayList(dl++);
    }
    plantTrees = 1;
}

void landscape_load(s32 area) {
    if (gCurrLevelNum != LEVEL_CASTLE_GROUNDS)
        return;
    if (area == 1) {
        Vec3s a = { -1560, 263, 5150 }, b = { -1096, 263, 5150 }, c = { -1560, 263, 5400 },
              d = { -1096, 263, 5400 };
        smlua_collision_add_surface(false, SURFACE_INSTANT_WARP_1B, a, c, b);
        smlua_collision_add_surface(false, SURFACE_INSTANT_WARP_1B, b, c, d);
    } else if (area == 2) {
        originX = originZ = 0;
        for (int i = 0; i < 38; i++)
            trees[i] = NULL;
        build();
    }
}

static void populate(void) {
    // One stable candidate per four-cell square, with fewer trees on dunes.
    for (int j = 0; j < 6; j++)
        for (int i = 0; i < 6; i++) {
            s64 gx = originX + (i - 3) * 4, gz = originZ + (j - 3) * 4;
            u32 h = hash(gx, gz, 9147);
            int dx = (h >> 8) % 4, dz = (h >> 12) % 4;
            int x = i * 4 + dx, z = j * 4 + dz;
            struct Climate c = sample(gx + dx, gz + dz);
            int id = j * 6 + i;
            struct Object *tree = trees[id];
            if (tree)
                obj_mark_for_deletion(tree);
            trees[id] = NULL;
            if ((h & 255) > 82 + 155 * c.woods - 50 * c.sand
                || (llabs(gx + dx) < 2 && llabs(gz + dz) < 3))
                continue;
            int model = c.ice > .55   ? MODEL_CCM_SNOW_TREE
                        : c.sand > .6 ? MODEL_SSL_PALM_TREE
                                      : MODEL_CASTLE_GROUNDS_BUBBLY_TREE;
            trees[id] = spawn_object_abs_with_rot(gMarioObject, 0, model, bhvTree, (x - 12) * CELL,
                                                  heights[z * ROW + x], (z - 12) * CELL, 0, 0, 0);
        }
    for (int side = 0; side < 2; side++) {
        struct Object **marker = &trees[36 + side];
        if (*marker)
            obj_mark_for_deletion(*marker);
        *marker = NULL;
        if (llabs(originX) > 8 || llabs(originZ) > 8)
            continue;
        int x = (side ? 548 : -548) - originX * CELL;
        *marker = spawn_object_abs_with_rot(gMarioObject, 0, MODEL_CASTLE_GROUNDS_BUBBLY_TREE, bhvTree,
                                            x, 0, -512 - originZ * CELL, 0, 0, 0);
    }
    plantTrees = 0;
}

void landscape_update(void) {
    if (gCurrLevelNum != LEVEL_CASTLE_GROUNDS || gCurrAreaIndex != 2 || !gMarioObject)
        return;
    struct MarioState *m = gMarioState;
    int dx = m->pos[0] > SHIFT ? SHIFT : m->pos[0] < -SHIFT ? -SHIFT : 0;
    int dz = m->pos[2] > SHIFT ? SHIFT : m->pos[2] < -SHIFT ? -SHIFT : 0;
    if (dx || dz) {
        originX += dx / CELL;
        originZ += dz / CELL;
        m->pos[0] -= dx;
        m->pos[2] -= dz;
        m->marioObj->oPosX = m->pos[0];
        m->marioObj->oPosZ = m->pos[2];
        m->nonInstantWarpPos[0] = m->pos[0];
        m->nonInstantWarpPos[2] = m->pos[2];
        build();
        for (int i = 1; i < MAX_PLAYERS; i++) {
            struct MarioState *remote = &gMarioStates[i];
            if (!remote->marioObj)
                continue;
            remote->pos[0] -= dx;
            remote->pos[2] -= dz;
            remote->marioObj->oPosX = remote->pos[0];
            remote->marioObj->oPosZ = remote->pos[2];
            remote->wall = remote->ceil = NULL;
            remote->floorHeight =
                find_floor(remote->pos[0], remote->pos[1] + 100, remote->pos[2], &remote->floor);
        }
        warp_camera(-dx, 0, -dz);
        skip_camera_interpolation();
        m->wall = m->ceil = NULL;
        m->floorHeight = find_floor(m->pos[0], m->pos[1] + 100, m->pos[2], &m->floor);
    }
    if (plantTrees)
        populate();
}

Gfx *geo_landscape(s32 context, struct GraphNode *node, UNUSED Mat4 matrix) {
    if (context != GEO_CONTEXT_RENDER)
        return NULL;
    int pass = ((struct GraphNodeGenerated *) node)->parameter;
    node->flags = (node->flags & 0xff) | ((pass ? LAYER_TRANSPARENT_DECAL : LAYER_OPAQUE) << 8);
    return lists[pass];
}
