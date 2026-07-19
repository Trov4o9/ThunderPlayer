#ifndef __RAS_IDISPLAY_ARRAY_PATCH_H__
#define __RAS_IDISPLAY_ARRAY_PATCH_H__

#include "RAS_DisplayArray.h"

class RAS_DisplayArrayPatch : public RAS_DisplayArray
{
public:
    RAS_DisplayArrayPatch(const Format& format);
    RAS_DisplayArrayPatch(const RAS_DisplayArrayPatch& other);
    virtual ~RAS_DisplayArrayPatch();

    int GetOpenGLPrimitiveType() const override;

    Type GetType() const override;
};

#endif // __RAS_IDISPLAY_ARRAY_PATCH_H__
