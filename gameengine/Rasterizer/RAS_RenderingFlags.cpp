/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "RAS_RenderingFlags.h"

/* ============================================================================
 * PERSISTENT SSBO RENDERING SYSTEM FLAG
 * ============================================================================
 * Set to 1 to enable the new SSBO-based persistent rendering with indirect draw
 * Set to 0 to use the old instancing system (default/legacy behavior)
 */
int USE_PERSISTENT_SSBO_RENDERING = 0;  /* Change to 1 to enable */
