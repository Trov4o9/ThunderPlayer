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

#ifndef __RAS_RENDERING_FLAGS_H__
#define __RAS_RENDERING_FLAGS_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * PERSISTENT SSBO RENDERING SYSTEM FLAG
 * ============================================================================
 * Set to 1 to enable the new SSBO-based persistent rendering with indirect draw
 * Set to 0 to use the old instancing system (default/legacy behavior)
 */
extern int USE_PERSISTENT_SSBO_RENDERING;

#ifdef __cplusplus
}
#endif

#endif  // __RAS_RENDERING_FLAGS_H__
