#include "RAS_DisplayArrayPatch.h"
#include "GPU_glew.h"

#define CM_Log(msg) fprintf(stdout, "[CM_LOG] %s\n", msg)
#define CM_Logf(fmt, ...) fprintf(stdout, "[CM_LOG] " fmt "\n", __VA_ARGS__)
#define CM_Error(msg) fprintf(stderr, "[CM_ERROR] %s\n", msg)
#define CM_Errorf(fmt, ...) fprintf(stderr, "[CM_ERROR] " fmt "\n", __VA_ARGS__)

RAS_DisplayArrayPatch::RAS_DisplayArrayPatch(const Format& format)
    : RAS_DisplayArray(TRIANGLES, format) 
{
}

RAS_DisplayArrayPatch::RAS_DisplayArrayPatch(const RAS_DisplayArrayPatch& other)
    : RAS_DisplayArray(other)
{
}

RAS_DisplayArrayPatch::~RAS_DisplayArrayPatch()
{
}

int RAS_DisplayArrayPatch::GetOpenGLPrimitiveType() const
{
    return GL_PATCHES;
    CM_Log("Return GL_PATCHES");
}

RAS_DisplayArray::Type RAS_DisplayArrayPatch::GetType() const
{
    return NORMAL;
}
