#include "RAS_AttributeArrayStorage.h"
#include "RAS_StorageVao.h"
#include "RAS_BucketManager.h"

RAS_AttributeArrayStorage::RAS_AttributeArrayStorage(const RAS_DisplayArrayLayout& layout, RAS_DisplayArrayStorage *arrayStorage,
                                                     const RAS_AttributeArray::AttribList& attribList)
	:m_vao(new RAS_StorageVao(layout, arrayStorage, attribList))
{
}

RAS_AttributeArrayStorage::~RAS_AttributeArrayStorage()
{
}

void RAS_AttributeArrayStorage::BindPrimitives()
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_ATTRIBUTEARRAYSTORAGE_BINDPRIMITIVES);
	m_vao->BindPrimitives();
}

void RAS_AttributeArrayStorage::UnbindPrimitives()
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_ATTRIBUTEARRAYSTORAGE_UNBINDPRIMITIVES);
	m_vao->UnbindPrimitives();
}


