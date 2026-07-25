/*
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
 * The Original Code is Copyright (C) 2008 Blender Foundation.
 * All rights reserved.
 */

/** \file blender/editors/space_script/space_script.c
 *  \ingroup spscript
 */

 #include <string.h>
 #include <stdio.h>
 
 #include "MEM_guardedalloc.h"
 
 #include "BLI_blenlib.h"
 #include "BLI_utildefines.h"
 
 #include "BKE_context.h"
 #include "BKE_screen.h"
 
 #include "ED_space_api.h"
 #include "ED_screen.h"
 
 #include "BIF_gl.h"
 
 #include "WM_api.h"
 #include "WM_types.h"
 
 #include "UI_resources.h"
 #include "UI_view2d.h"
 #include "UI_interface.h"
 #include "BLF_api.h"
 
 #include "script_intern.h"
 
 /* ******************** Painel para a aba lateral (Shader Editor) ****************** */
 
 static bool shader_editor_panel_poll(const bContext *C, PanelType *UNUSED(pt))
 {
	 /* Pode implementar condição para mostrar o painel, aqui sempre TRUE */
	 return true;
 }
 
 static void shader_editor_panel_draw(const bContext *C, Panel *panel)
 {
	 uiLayout *layout = panel->layout;
 
	 uiItemL(layout, "Painel do Editor de Shader", ICON_NONE);
 
	 /* Exemplo: botão simples */
	 uiItemS(layout);
	 uiItemL(layout, "Texto extra aqui...", ICON_INFO);
 }
 
 /* Registro do painel na aba lateral */
 static void shader_editor_panel_register(ARegionType *art)
 {
	 PanelType *pt = MEM_callocN(sizeof(PanelType), "shader editor panel");
 
	 BLI_strncpy(pt->idname, "SCRIPT_PT_shader_editor", sizeof(pt->idname));
	 BLI_strncpy(pt->label, "Shader Editor", sizeof(pt->label));
	 BLI_strncpy(pt->category, "Shader", sizeof(pt->category));
	 pt->draw = shader_editor_panel_draw;
	 pt->poll = shader_editor_panel_poll;
	 pt->region_type = RGN_TYPE_UI;
 
	 BLI_addtail(&art->paneltypes, pt);
 }
 
 /* ******************** default callbacks for script space ***************** */
 
 static SpaceLink *script_new(const bContext *UNUSED(C))
 {
	 ARegion *ar;
	 SpaceScript *sscript;
 
	 sscript = MEM_callocN(sizeof(SpaceScript), "initscript");
	 sscript->spacetype = SPACE_SCRIPT;
 
	 /* header */
	 ar = MEM_callocN(sizeof(ARegion), "header for script");
	 BLI_addtail(&sscript->regionbase, ar);
	 ar->regiontype = RGN_TYPE_HEADER;
	 ar->alignment = RGN_ALIGN_BOTTOM;
 
	 /* main region */
	 ar = MEM_callocN(sizeof(ARegion), "main region for script");
	 BLI_addtail(&sscript->regionbase, ar);
	 ar->regiontype = RGN_TYPE_WINDOW;
 
	 /* shader editor UI region */
	 ar = MEM_callocN(sizeof(ARegion), "shader editor region");
	 BLI_addtail(&sscript->regionbase, ar);
	 ar->regiontype = RGN_TYPE_UI;
	 ar->alignment = RGN_ALIGN_RIGHT;
	 /* Deixe visível */
	 ar->flag = RGN_FLAG_HIDDEN;
 
	 return (SpaceLink *)sscript;
 }
 
 static void script_free(SpaceLink *sl)
 {
	 SpaceScript *sscript = (SpaceScript *)sl;
 
 #ifdef WITH_PYTHON
	 if (sscript->but_refs) {
		 sscript->but_refs = NULL;
	 }
 #endif
	 sscript->script = NULL;
 }
 
 static void script_init(struct wmWindowManager *UNUSED(wm), ScrArea *UNUSED(sa))
 {
 }
 
 static SpaceLink *script_duplicate(SpaceLink *sl)
 {
	 SpaceScript *sscriptn = MEM_dupallocN(sl);
	 return (SpaceLink *)sscriptn;
 }
 
 static void script_main_region_init(wmWindowManager *wm, ARegion *ar)
 {
	 wmKeyMap *keymap;
 
	 UI_view2d_region_reinit(&ar->v2d, V2D_COMMONVIEW_STANDARD, ar->winx, ar->winy);
 
	 keymap = WM_keymap_ensure(wm->defaultconf, "Script", SPACE_SCRIPT, 0);
	 WM_event_add_keymap_handler_bb(&ar->handlers, keymap, &ar->v2d.mask, &ar->winrct);
 }
 
 static void script_main_region_draw(const bContext *C, ARegion *ar)
 {
	 SpaceScript *sscript = (SpaceScript *)CTX_wm_space_data(C);
	 View2D *v2d = &ar->v2d;
 
	 UI_ThemeClearColor(TH_BACK);
	 glClear(GL_COLOR_BUFFER_BIT);
 
	 UI_view2d_view_ortho(v2d);
 
 #ifdef WITH_PYTHON
	 if (sscript->script) {
		 // BPY_run_script_space_draw(C, sscript);
	 }
 #else
	 (void)sscript;
 #endif
 
	 UI_view2d_view_restore(C);
 }
 
 static void script_main_region_listener(bScreen *UNUSED(sc), ScrArea *UNUSED(sa), ARegion *UNUSED(ar), wmNotifier *UNUSED(wmn))
 {
 }
 
 /* Header */
 static void script_header_region_init(wmWindowManager *UNUSED(wm), ARegion *ar)
 {
	 ED_region_header_init(ar);
 }
 
 static void script_header_region_draw(const bContext *C, ARegion *ar)
 {
	 ED_region_header(C, ar);
 }
 
 /* Shader Editor Region */
 static void shader_editor_region_init(const bContext *C, ARegion *ar)
 {
	 ED_region_init(C, ar);
 }
 
 static void shader_editor_region_draw(const bContext *C, ARegion *ar)
 {
	 UI_ThemeClearColor(TH_BACK);
	 glClear(GL_COLOR_BUFFER_BIT);
	 UI_view2d_view_ortho(&ar->v2d);
 
	 uiStyle *style = UI_style_get();
 
	 UI_ThemeColor(TH_TEXT_HI);
 
	 BLF_position(style->widget.uifont_id, 10, 10, 0);
	 BLF_draw(style->widget.uifont_id, "Editor de Shader (GLSL)", sizeof("Editor de Shader (GLSL)") - 1);
 
	 UI_view2d_view_restore(C);
 }
 
 /* Apenas chamada uma vez, de space/spacetypes.c */
 void ED_spacetype_script(void)
 {
	 SpaceType *st = MEM_callocN(sizeof(SpaceType), "spacetype script");
	 ARegionType *art;
 
	 st->spaceid = SPACE_SCRIPT;
	 BLI_strncpy(st->name, "Script", sizeof(st->name));
 
	 st->new = script_new;
	 st->free = script_free;
	 st->init = script_init;
	 st->duplicate = script_duplicate;
 
	 /* main region */
	 art = MEM_callocN(sizeof(ARegionType), "spacetype script region");
	 art->regionid = RGN_TYPE_WINDOW;
	 art->init = script_main_region_init;
	 art->draw = script_main_region_draw;
	 art->listener = script_main_region_listener;
	 art->keymapflag = ED_KEYMAP_VIEW2D | ED_KEYMAP_UI | ED_KEYMAP_FRAMES;
 
	 BLI_addhead(&st->regiontypes, art);
 
	 /* header region */
	 art = MEM_callocN(sizeof(ARegionType), "spacetype script header");
	 art->regionid = RGN_TYPE_HEADER;
	 art->prefsizey = HEADERY;
	 art->init = script_header_region_init;
	 art->draw = script_header_region_draw;
	 art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_HEADER;
 
	 BLI_addhead(&st->regiontypes, art);
 
	 /* shader editor region (aba lateral) */
	 art = MEM_callocN(sizeof(ARegionType), "spacetype shader editor region");
	 art->regionid = RGN_TYPE_UI;
	 art->prefsizex = 300;
	 art->keymapflag = ED_KEYMAP_UI;
	 art->init = shader_editor_region_init;
	 art->draw = shader_editor_region_draw;
 
	 /* REGISTRA O PAINEL PARA MOSTRAR CONTEÚDO NA ABA LATERAL */
	 shader_editor_panel_register(art);
 
	 BLI_addhead(&st->regiontypes, art);
 
	 BKE_spacetype_register(st);
 }
 