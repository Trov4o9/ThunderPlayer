#include "BLI_utildefines.h"
#include "BKE_context.h"
#include "DNA_space_types.h"
#include "ED_space_api.h"
#include "ED_screen.h"
#include "WM_types.h"
#include "UI_resources.h"
#include "UI_interface.h"
#include "MEM_guardedalloc.h"
#include "BLF_api.h"
#include <string.h>

/* -------------------------------------------------------------------- */
/* Space Type Init / Free / Duplicate */

static SpaceLink *shader_editor_create(const ScrArea *UNUSED(area), const Scene *UNUSED(scene))
{
  SpaceLink *sl = MEM_callocN(sizeof(SpaceLink), "init shader editor");
  sl->spacetype = SPACE_SHADER_EDITOR;
  return sl;
}

static void shader_editor_free(SpaceLink *UNUSED(sl)) {}

static SpaceLink *shader_editor_duplicate(SpaceLink *sl)
{
  return MEM_dupallocN(sl);
}

/* -------------------------------------------------------------------- */
/* Region Init / Draw */

static void shader_editor_main_region_init(wmWindowManager *UNUSED(wm), ARegion *region)
{
  region->v2d.scroll = V2D_SCROLL_RIGHT | V2D_SCROLL_BOTTOM;
  region->v2d.keepzoom = V2D_KEEPZOOM | V2D_KEEPASPECT;
  region->v2d.keeptot = V2D_KEEPTOT_STRICT;
  UI_view2d_region_reinit(&region->v2d, V2D_COMMONVIEW_STANDARD, region->winx, region->winy);
}

static void shader_editor_main_region_draw(const bContext *UNUSED(C), ARegion *region)
{
  UI_ThemeClearColor(TH_BACK);
  UI_draw_roundbox_corner_set(UI_CNR_ALL);
  UI_draw_roundbox(0, region->winrct.xmin + 10, region->winrct.ymin + 10,
                   region->winrct.xmax - 10, region->winrct.ymax - 10,
                   10.0f, NULL);

  /* Placeholder de texto centralizado */
  const char *info = "Editor de Shaders GLSL - Em construção";
  int width = BLF_width(BLF_default(), info, strlen(info));
  int x = (region->winrct.xmax - region->winrct.xmin - width) / 2;
  int y = (region->winrct.ymax - region->winrct.ymin) / 2;

  BLF_position(BLF_default(), x, y, 0.0f);
  BLF_draw(BLF_default(), info, strlen(info));
}

/* -------------------------------------------------------------------- */
/* Space Type Definition */

void ED_spacetype_shader_editor(void)
{
  SpaceType *st = MEM_callocN(sizeof(SpaceType), "spacetype shader editor");
  ARegionType *art;

  st->spaceid = SPACE_SHADER_EDITOR;
  STRNCPY(st->name, "Shader Editor");

  st->create = shader_editor_create;
  st->free = shader_editor_free;
  st->duplicate = shader_editor_duplicate;

  /* Região principal */
  art = MEM_callocN(sizeof(ARegionType), "main region for shader editor");
  art->regionid = RGN_TYPE_WINDOW;
  art->init = shader_editor_main_region_init;
  art->draw = shader_editor_main_region_draw;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D;

  BLI_addhead(&st->regiontypes, art);
  BKE_spacetype_register(st);
}
