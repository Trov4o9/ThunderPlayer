/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 * KX_MouseFocusSensor determines mouse in/out/over events.
 */

/** \file gameengine/Ketsji/KX_RayCast.cpp
 *  \ingroup ketsji
 */


#include "KX_RayCast.h"

#include "PHY_IPhysicsEnvironment.h"
#include "PHY_IPhysicsController.h"
#include "KX_PolyProxy.h"

#include "../../../blender/blenlib/BLI_math_vector.h"
#include "../../Rasterizer/RAS_Mesh.h"
#include "../../Rasterizer/RAS_IMaterial.h"
#include "../../Rasterizer/RAS_Rasterizer.h"
#include "../../Rasterizer/RAS_DisplayArray.h"
#include "KX_GameObject.h"
#include <cfloat>
#include "CM_Message.h"

KX_RayCast::KX_RayCast(PHY_IPhysicsController *ignoreController, bool faceNormal, bool faceUV)
	:PHY_IRayCastFilterCallback(ignoreController, faceNormal, faceUV)
{
}

void KX_RayCast::reportHit(PHY_RayCastResult* result)
{
	m_hitFound = true;
	m_hitPoint = mt::vec3(result->m_hitPoint);
	m_hitNormal = mt::vec3(result->m_hitNormal);
	m_hitUVOK = result->m_hitUVOK;
	m_hitUV = mt::vec2(result->m_hitUV);
	m_hitMesh = result->m_meshObject;
	m_hitPolygon = result->m_polygon;

	m_hitVertexIndex = -1;
	m_hitVertex = mt::vec3(0.0f);
	m_hitVertexNormal = mt::vec3(0.0f);  

	if (m_hitMesh && m_hitPolygon >= 0) {
		const RAS_Mesh::PolygonInfo poly = m_hitMesh->GetPolygon(m_hitPolygon);
		RAS_DisplayArray* array = poly.array;
		const unsigned int* indices = poly.indices;

		float minDistSq = FLT_MAX;
		for (int i = 0; i < 3; ++i) {
			const mt::vec3 vert = mt::vec3(array->GetPosition(indices[i]));
			const mt::vec3 norm = mt::vec3(array->GetNormal(indices[i]));
			float distSq = (vert - m_hitPoint).LengthSquared();
			if (distSq < minDistSq) {
				minDistSq = distSq;
				m_hitVertex = vert;
				m_hitVertexIndex = indices[i];
				m_hitVertexNormal = norm;
			}
		}
	}
}




bool KX_RayCast::RayTest(PHY_IPhysicsEnvironment *physics_environment, const mt::vec3& _frompoint, const mt::vec3& topoint, KX_RayCast& callback)
{
	if (physics_environment == nullptr) {
		return false;                               /* prevents crashing in some cases */

	}
	// Loops over all physics objects between frompoint and topoint,
	// calling callback.RayHit for each one.
	//
	// callback.RayHit should return true to stop looking, or false to continue.
	//
	// returns true if an object was found, false if not.

	mt::vec3 frompoint(_frompoint);
	const mt::vec3 todir((topoint - frompoint).SafeNormalized(mt::axisX3));
	mt::vec3 prevpoint(_frompoint + todir * (-1.f));

	PHY_IPhysicsController *hit_controller;

	while ((hit_controller = physics_environment->RayTest(callback,
	                                                      frompoint.x, frompoint.y, frompoint.z,
	                                                      topoint.x, topoint.y, topoint.z)) != nullptr)
	{
		KX_ClientObjectInfo *info = static_cast<KX_ClientObjectInfo *>(hit_controller->GetNewClientInfo());

		if (!info) {
			CM_Error("no info!");
			BLI_assert(info && "Physics controller with no client object info");
			break;
		}

		// The biggest danger to endless loop, prevent this by checking that the
		// hit point always progresses along the ray direction..
		prevpoint -= callback.m_hitPoint;
		if (mt::FuzzyZero(prevpoint.LengthSquared())) {
			break;
		}

		if (callback.RayHit(info)) {
			// caller may decide to stop the loop and still cancel the hit
			return callback.m_hitFound;
		}

		// Skip past the object and keep tracing.
		// Note that retrieving in a single shot multiple hit points would be possible
		// but it would require some change in Bullet.
		prevpoint = callback.m_hitPoint;
		/* We add 0.001 of fudge, so that if the margin && radius == 0.0, we don't endless loop. */
		float marg = 0.001f + hit_controller->GetMargin();
		marg *= 2.f;
		/* Calculate the other side of this object */
		float h = std::abs(mt::dot(todir, callback.m_hitNormal));
		if (h <= 0.01f) {
			// the normal is almost orthogonal to the ray direction, cannot compute the other side
			break;
		}
		marg /= h;
		frompoint = callback.m_hitPoint + marg * todir;
		// verify that we are not passed the to point
		if (mt::dot((topoint - frompoint), todir) < 0.f) {
			break;
		}
	}
	return false;
}

