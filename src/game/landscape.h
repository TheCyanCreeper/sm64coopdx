#pragma once
#include "types.h"

void landscape_load(s32 area);
void landscape_update(void);
Gfx *geo_landscape(s32 context, struct GraphNode *node, Mat4 matrix);
void landscape_get_origin(s64 origin[2]);
void landscape_relative_position(Vec3f pos, const s64 origin[2]);
