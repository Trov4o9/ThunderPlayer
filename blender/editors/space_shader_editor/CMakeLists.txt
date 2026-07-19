set(INC
  .
  ../include
  ../../blenkernel
  ../../blenlib
  ../../blenfont
  ../../blenloader
  ../../makesdna
  ../../makesrna
  ../../windowmanager
  ../../../../intern/guardedalloc
)

set(SRC
  space_shader_editor.c
  ../include/ED_space_shader_editor.h
)

set(LIB
  bf_blenkernel
  bf_blenlib
)

blender_add_lib(bf_editor_space_shader_editor "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")