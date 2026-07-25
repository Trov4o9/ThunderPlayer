#ifndef __ED_SPACE_SHADER_EDITOR_H__
#define __ED_SPACE_SHADER_EDITOR_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Registra o tipo de espaço personalizado do editor de shader GLSL.
 * Deve ser chamado em `spacetypes.c`.
 */
void ED_spacetype_shader_editor(void);

#ifdef __cplusplus
}
#endif

#endif /* __ED_SPACE_SHADER_EDITOR_H__ */
