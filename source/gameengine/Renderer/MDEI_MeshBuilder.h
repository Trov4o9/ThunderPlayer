/*
 * MDEI_MeshBuilder.h — Extract raw geometry from a Blender DerivedMesh
 * into MDEI_Mesh (pos + normal + uv0, no tangents, no multi-UV).
 */

#ifndef __MDEI_MESH_BUILDER_H__
#define __MDEI_MESH_BUILDER_H__

class MDEI_Mesh;
struct Object;
struct Scene;

class MDEI_MeshBuilder {
public:
	/** Build and upload an MDEI_Mesh from a Blender Object.
	 *  The caller owns the returned pointer.
	 *  Returns nullptr if the object has no valid mesh data. */
	static MDEI_Mesh *Build(Object *ob, Scene *blenderScene);
};

#endif /* __MDEI_MESH_BUILDER_H__ */
