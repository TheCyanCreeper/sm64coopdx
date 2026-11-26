#pragma once
#include "types.h"

void landscape_load(s32 area);
void landscape_update(void);
Gfx *geo_landscape(s32 context, struct GraphNode *node, Mat4 matrix);
void landscape_get_origin(s64 origin[2]);
void landscape_relative_position(Vec3f pos, const s64 origin[2]);
Gfx *geo_landscape_home(s32 context, struct GraphNode *node, Mat4 matrix);

// Area-local model slot; the stock snow-level ID is used by the castle birds.
#define MODEL_LANDSCAPE_SPINDRIFT 0x3C
#define MODEL_LANDSCAPE_PIRANHA 0x3D

#define LANDSCAPE_HOME_ACTOR 0xffffffffu
void landscape_anchor_actor(Mat4 matrix, Mat4 parent);
#define COURT_WATER_ID 41
#define COURT_Y 1024
#define COURT_Z (-8320)
#define MODEL_GROUNDS_BOO 0x3E
Gfx *geo_landscape_home_offset(s32 context, struct GraphNode *node, Mat4 matrix);
Gfx *geo_landscape_court_path(s32 context, struct GraphNode *node, Mat4 matrix);
#define LANDSCAPE_HILL (1 << 4)
Gfx *geo_landscape_backdrop(s32 context, struct GraphNode *node, Mat4 matrix);
Gfx *geo_landscape_rim(s32 context, struct GraphNode *node, Mat4 matrix);
