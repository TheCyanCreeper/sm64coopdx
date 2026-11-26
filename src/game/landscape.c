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

static Vtx vertices[3][PATCHES * 4];
static Gfx lists[3][PATCHES * 3 + 32];
static s16 heights[ROW * ROW];
static u8 snow[ROW * ROW];
// The original collision, warp nodes, moat and castle actors stay resident.
// Only triangles owned by the wilderness streamer may be retired.
struct GroundCell {
    s64 x, z;
    s16 localX, localZ;
    struct Surface *tri[2];
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
struct Climate {
    double woods, sand, ice, height;
};
static struct Climate sample(s64 x, s64 z) {
    double distance = fmax(fabs((double) x), fabs((double) z));
    double outskirts = smooth((distance - 16) / 18);
    double weather = noise(x, z, 80, 32421);
    double ice = outskirts * smooth((weather - .53) / .22);
    double sand = outskirts * smooth((.52 - weather) / .20);
    double woods = (1 - ice - sand) * smooth((noise(x, z, 48, 419) - .35) / .35) * outskirts;
    double hills = noise(x, z, 9, 77), detail = noise(x, z, 3, 782);
    struct Climate c = {
        woods, sand, ice,
        outskirts * ((hills - .45) * (1050 + ice * 1800 + sand * 530) + (detail - .5) * 180)
    };
    c.height += 360;
    if (distance <= HOME + 4) {
        int best = 26;
        for (int j = -5; j <= 5; j++)
            for (int i = -5; i <= 5; i++) {
                int d = i * i + j * j;
                if (d < best && authored(x + i, z + j))
                    best = d;
            }
        if (best < 26) {
            int sum = 0, count = 0;
            for (int j = -5; j <= 5; j++)
                for (int i = -5; i <= 5; i++) {
                    if (i * i + j * j == best && authored(x + i, z + j)) {
                        sum += homeHeight[z + j + HOME][x + i + HOME];
                        count++;
                    }
                }
            c.height = lerp((double) sum / count - 8, c.height, smooth(sqrt(best) / 5));
        }
        // A grass saddle climbs over the original steep south bank.
        if (z >= 8 && z <= 20 && x >= -6 && x <= 0) {
            double across = smooth((3 - fabs(x + 3.)) / 2.);
            double ridge = 260 + 1430 * smooth((z - 9.) / 5.0);
            double crest = fmax(c.height, ridge + 5);
            // Tuck the foot below the stock lawn so the spawn point keeps its authored floor.
            if (z <= 9)
                crest = c.height - 26;
            c.height = lerp(c.height, crest, across * (1 - smooth((z - 14.) / 6.)));
        }
    }
    return c;
}
static void build(void) {
    struct Climate climate[ROW * ROW];
    for (int z = 0; z < ROW; z++)
        for (int x = 0; x < ROW; x++) {
            int k = z * ROW + x;
            climate[k] = sample(centerX + x - SIDE / 2, centerZ + z - SIDE / 2);
            heights[k] = lround(climate[k].height);
            snow[k] = climate[k].ice > .65;
        }

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
                s64 wx = centerX + x - SIDE / 2, wz = centerZ + z - SIDE / 2;
                int ringX = (wx % SIDE + SIDE) % SIDE, ringZ = (wz % SIDE + SIDE) % SIDE;
                struct GroundCell *g = &ground[ringZ * SIDE + ringX];
                int localX = (wx - originX) * CELL, localZ = (wz - originZ) * CELL;
                if (!pass && g->tri[0]
                    && (g->x != wx || g->z != wz || g->localX != localX || g->localZ != localZ)) {
                    delete_surface(g->tri[0]);
                    delete_surface(g->tri[1]);
                    g->tri[0] = g->tri[1] = NULL;
                }
                if (authored(wx, wz) && authored(wx + 1, wz) && authored(wx, wz + 1)
                    && authored(wx + 1, wz + 1) && !(wz >= 8 && wz < 18 && wx >= -6 && wx < 0))
                    continue;
                Vec3s p[4];
                int visible = 0;
                for (int corner = 0; corner < 4; corner++) {
                    int dx = corner & 1, dz = corner >> 1, n = k + dx + dz * ROW;
                    struct Climate c = climate[n];
                    p[corner][0] = (wx + dx - originX) * CELL;
                    p[corner][1] = heights[n];
                    p[corner][2] = (wz + dz - originZ) * CELL;
                    double shade = .89 + .11 * noise(wx + dx, wz + dz, 3, 144);
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
                if (!pass && !g->tri[0]) {
                    g->x = wx;
                    g->z = wz;
                    g->localX = localX;
                    g->localZ = localZ;
                    s16 surface = snow[k] ? SURFACE_NOISE_SLIPPERY : SURFACE_NOT_SLIPPERY;
                    g->tri[0] = smlua_collision_add_surface(false, surface, p[0], p[2], p[1]);
                    g->tri[1] = smlua_collision_add_surface(false, surface, p[1], p[2], p[3]);
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
    if (authored(x, z))
        return;
    int lx = (x - originX) * CELL, lz = (z - originZ) * CELL;
    struct Object *o = spawn_object_abs_with_rot(gMarioObject, 0, model, bhv, lx,
                                                 lround(sample(x, z).height), lz, 0, hash(x, z, 5), 0);
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
            if ((h & 255) < 85 + 150 * c.woods - 50 * c.sand)
                place(s, 0,
                      c.ice > .55    ? MODEL_CCM_SNOW_TREE
                      : c.sand > .55 ? MODEL_SSL_PALM_TREE
                                     : MODEL_CASTLE_GROUNDS_BUBBLY_TREE,
                      bhvTree, x, z, 1);
            s64 rx = bx * 4 + ((h >> 17) & 3), rz = bz * 4 + ((h >> 20) & 3);
            if ((h >> 24) < 85 + 70 * (c.sand + c.ice)) {
                if (c.woods > .35 && (h & 1))
                    place(s, 1, MODEL_BUTTERFLY, bhvButterfly, rx, rz, 1);
                else if (c.ice > .65 && (h & 1))
                    place(s, 1, MODEL_CCM_SNOWMAN_BASE, bhvStaticObject, rx, rz, .48);
                else if (c.sand > .65 && (h & 3) == 0)
                    place(s, 1, MODEL_SSL_PYRAMID_TOP, bhvStaticObject, rx, rz, .32);
                else
                    place(s, 1, MODEL_HMC_ROLLING_ROCK, bhvStaticObject, rx, rz,
                          .18 + .5 * ((h >> 6) & 15) / 15.);
            }
            // One possible encounter in each group of four sites, giving at most nine
            // in the entire active window. The broad home meadow stays safe.
            if (((bx & 1) == 0) && ((bz & 1) == 0) && (h >> 24) < 180
                && fmax(llabs(x), llabs(z)) > 22) {
                int model = MODEL_GOOMBA;
                const BehaviorScript *behavior = bhvGoomba;
                if (c.ice > .5) {
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

void landscape_load(s32 area) {
    if (gCurrLevelNum != LEVEL_CASTLE_GROUNDS || area != 1)
        return;
    originX = originZ = centerX = centerZ = 0;
    memset(ground, 0, sizeof(ground));
    memset(sites, 0, sizeof(sites));
    for (int z = -HOME; z <= HOME; z++)
        for (int x = -HOME; x <= HOME; x++) {
            struct Surface *floor = NULL;
            float y = find_floor(x * CELL, 12000, z * CELL, &floor);
            homeHeight[z + HOME][x + HOME] =
                floor && floor->type != SURFACE_DEATH_PLANE ? (s16) y : -11000;
        }
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
        ((struct GraphNodeSwitchCase *) node)->selectedCase = (originX || originZ) ? 1 : 0;
    return NULL;
}

Gfx *geo_landscape(s32 context, struct GraphNode *node, UNUSED Mat4 matrix) {
    if (context != GEO_CONTEXT_RENDER)
        return NULL;
    int pass = ((struct GraphNodeGenerated *) node)->parameter;
    node->flags = (node->flags & 0xff) | ((pass ? LAYER_TRANSPARENT_DECAL : LAYER_OPAQUE) << 8);
    return lists[pass];
}
