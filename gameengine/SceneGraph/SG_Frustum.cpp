#include "SG_Frustum.h"

SG_Frustum::SG_Frustum(const mt::mat4& matrix)
	:m_matrix(matrix)
{
	const mt::vec4 row0 = m_matrix.GetRow(0);
	const mt::vec4 row1 = m_matrix.GetRow(1);
	const mt::vec4 row2 = m_matrix.GetRow(2);
	const mt::vec4 row3 = m_matrix.GetRow(3);

	m_planes[0] = row3 + row2;
	m_planes[1] = row3 - row2;
	m_planes[2] = row3 + row0;
	m_planes[3] = row3 - row0;
	m_planes[4] = row3 - row1;
	m_planes[5] = row3 + row1;

	for (mt::vec4& plane : m_planes) {
		const float factor = plane.x * plane.x + plane.y * plane.y + plane.z * plane.z;
		if (factor > 0.0f) {
			const float inv = 1.0f / sqrtf(factor);
			plane *= inv;
		}
	}
}

const std::array<mt::vec4, 6>& SG_Frustum::GetPlanes() const
{
	return m_planes;
}

const mt::mat4& SG_Frustum::GetMatrix() const
{
	return m_matrix;
}

inline float planeSide(const mt::vec4& plane, const mt::vec3& point)
{
	return (plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w);
}

SG_Frustum::TestType SG_Frustum::PointInsideFrustum(const mt::vec3& point) const
{
	for (const mt::vec4& plane : m_planes) {
		if (planeSide(plane, point) < 0.0f) {
			return OUTSIDE;
		}
	}
	return INSIDE;
}


SG_Frustum::TestType SG_Frustum::SphereInsideFrustum(const mt::vec3& center, float radius) const
{
	for (const mt::vec4& plane : m_planes) {
		const float distance = planeSide(plane, center);
		if (distance < -radius) {
			return OUTSIDE;
		}
		else if (fabs(distance) <= radius) {
			return INTERSECT;
		}
	}

	return INSIDE;
}

SG_Frustum::TestType SG_Frustum::BoxInsideFrustum(const std::array<mt::vec3, 8>& box) const
{
	unsigned short insidePlane = 0;
	for (const mt::vec4& plane : m_planes) {
		unsigned short insidePoint = 0;
		for (const mt::vec3& point : box) {
			insidePoint += (planeSide(plane, point) >= 0.0f);
		}

		if (insidePoint == 0) {
			return OUTSIDE;
		}
		insidePlane += (insidePoint == 8);
	}

	return (insidePlane == 6) ? INSIDE : INTERSECT;
}

static void getNearFarAabbPoint(const mt::vec4& plane, const mt::vec3& min, const mt::vec3& max, mt::vec3& near, mt::vec3& far)
{
	for (unsigned short axis = 0; axis < 3; ++axis) {
		if (plane[axis] < 0.0f) {
			near[axis] = max[axis];
			far[axis] = min[axis];
		}
		else {
			near[axis] = min[axis];
			far[axis] = max[axis];
		}
	}
}

static bool aabbIntersect(const mt::vec3& min1, const mt::vec3& max1, const mt::vec3& min2, const mt::vec3& max2)
{
	for (unsigned short axis = 0; axis < 3; ++axis) {
		if (max1[axis] < min2[axis] || min1[axis] > max2[axis]) {
			return false;
		}
	}

	return true;
}

SG_Frustum::TestType SG_Frustum::AabbInsideFrustum(const mt::vec3& min, const mt::vec3& max, const mt::mat4& mat) const
{
	TestType result = INSIDE;

	// Center and extents do AABB para SAT-style culling
	const mt::vec3 center = (min + max) * 0.5f;
	const mt::vec3 extents = (max - min) * 0.5f;

	for (const mt::vec4& wplane : m_planes) {
		// Plano do frustum transformado para o object space
		const mt::vec4 oplane = wplane * mat;

		// Distância do centro do AABB ao plano
		const float distance = (oplane.x * center.x + oplane.y * center.y + oplane.z * center.z + oplane.w);
		
		// Raio projetado do AABB na normal do plano (SAT)
		// fabs() pode ser substituído por branchless bitwise se necessário, mas o compilador já otimiza bem.
		const float radius = extents.x * fabsf(oplane.x) + 
		                     extents.y * fabsf(oplane.y) + 
		                     extents.z * fabsf(oplane.z);

		// Se o AABB está totalmente atrás do plano, está fora
		if (distance + radius < 0.0f) {
			return OUTSIDE;
		}
		
		// Se o AABB cruza o plano, marcamos como possível interseção
		if (distance - radius < 0.0f) {
			result = INTERSECT;
		}
	}

	/* Big object can intersect two "orthogonal" planes without be inside the frustum.
	 * In this case the object is outside the AABB of the frustum. */
	if (result == INTERSECT) {
		mt::vec3 fmin;
		mt::vec3 fmax;
		mt::FrustumAabb((m_matrix * mat).Inverse(), fmin, fmax);

		if (!aabbIntersect(min, max, fmin, fmax)) {
			return OUTSIDE;
		}
	}

	return result;
}

SG_Frustum::TestType SG_Frustum::AabbInsideFrustumFast(const mt::vec3& min, const mt::vec3& max) const
{
	TestType result = INSIDE;

	// Componentes individuais para evitar mt::vec3 temporário e overhead de construtores
	const float cx = (min.x + max.x) * 0.5f;
	const float cy = (min.y + max.y) * 0.5f;
	const float cz = (min.z + max.z) * 0.5f;

	const float ex = (max.x - min.x) * 0.5f;
	const float ey = (max.y - min.y) * 0.5f;
	const float ez = (max.z - min.z) * 0.5f;

	// Loop fixo para melhor unrolling e otimização do compilador
	for (int i = 0; i < 6; ++i) {
		const mt::vec4& p = m_planes[i];

		// Distância do centro do AABB ao plano (sem multiplicação por matriz)
		const float distance = (p.x * cx + p.y * cy + p.z * cz + p.w);
		
		// Raio projetado do AABB na normal do plano (SAT)
		const float radius = ex * fabsf(p.x) + 
		                     ey * fabsf(p.y) + 
		                     ez * fabsf(p.z);

		// Se o AABB está totalmente atrás do plano, está fora
		if (distance + radius < 0.0f) {
			return OUTSIDE;
		}
		
		// Se o AABB cruza o plano, marcamos como possível interseção
		if (distance - radius < 0.0f) {
			result = INTERSECT;
		}
	}

	return result;
}

static int whichSide(const std::array<mt::vec3, 8>& box, const mt::vec4& plane)
{
	unsigned short positive = 0;
	unsigned short negative = 0;

	for (const mt::vec3& point : box) {
		const float t = planeSide(plane, point);
		if (mt::FuzzyZero(t)) {
			return 0;
		}

		negative += (t < 0.0f); // point outside
		positive += (t > 0.0f); // point inside

		if (positive > 0 && negative > 0) {
			return 0;
		}
	}

	return (positive > 0) ? 1 : -1;
}

static int whichSide(const std::array<mt::vec3, 8>& box, const mt::vec3& normal, const mt::vec3& vert)
{
	unsigned short positive = 0;
	unsigned short negative = 0;

	for (const mt::vec3& point : box) {
		const float t = mt::dot(normal, point - vert);
		if (mt::FuzzyZero(t)) {
			return 0;
		}

		negative += (t < 0.0f); // point outside
		positive += (t > 0.0f); // point inside

		if (positive > 0 && negative > 0) {
			return 0;
		}
	}

	return (positive > 0) ? 1 : -1;
}

SG_Frustum::TestType SG_Frustum::FrustumInsideFrustum(const SG_Frustum& frustum) const
{
	// Based on https://booksite.elsevier.com/9781558605930/revisionnotes/MethodOfSeperatingAxes.pdf

	/* First test if the vertices of the second frustum box are not fully oustide the
	 * planes of the first frustum.
	 */
	std::array<mt::vec3, 8> fbox2;
	mt::FrustumBox(frustum.m_matrix.Inverse(), fbox2);

	for (const mt::vec4& plane : m_planes) {
		if (whichSide(fbox2, plane) < 0) {
			return OUTSIDE;
		}
	}

	// Test with first frustum box and second frustum planes.
	std::array<mt::vec3, 8> fbox1;
	mt::FrustumBox(m_matrix.Inverse(), fbox1);

	for (const mt::vec4& plane : frustum.m_planes) {
		if (whichSide(fbox1, plane) < 0) {
			return OUTSIDE;
		}
	}

	/* Test edge separation axis, they are produced by the cross product of
	 * edge from the both frustums.
	 */
	std::array<mt::vec3, 12> fedges1;
	std::array<mt::vec3, 12> fedges2;

	mt::FrustumEdges(fbox1, fedges1);
	mt::FrustumEdges(fbox2, fedges2);

	for (unsigned short i = 0; i < 12; ++i) {
		const mt::vec3& edge1 = fedges1[i];
		// Origin of the separation axis.
		const mt::vec3& vert = fbox1[mt::FrustumEdgeVertex(i)];
		for (unsigned short j = 0; j < 12; ++j) {
			const mt::vec3& edge2 = fedges2[j];
			// Normal of the separation axis.
			const mt::vec3 normal = mt::cross(edge2, edge1);

			const int side1 = whichSide(fbox1, normal, vert);

			// Intersect ?
			if (side1 == 0) {
				continue;
			}

			const int side2 = whichSide(fbox2, normal, vert);

			// Intersect ?
			if (side2 == 0) {
				continue;
			}

			// Frustum on opposite side of the separation axis.
			if ((side1 * side2) < 0) {
				return OUTSIDE;
			}
		}
	}

	return INSIDE;
}
