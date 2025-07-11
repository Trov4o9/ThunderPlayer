
/* Automatically generated struct definitions for the Data API.
 * Do not edit manually, changes will be overwritten.           */

#define RNA_RUNTIME

#include <float.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>

#include <stddef.h>

#include "MEM_guardedalloc.h"

#include "DNA_ID.h"
#include "DNA_scene_types.h"
#include "BLI_blenlib.h"

#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_library.h"
#include "BKE_main.h"
#include "BKE_report.h"
#include "RNA_define.h"
#include "RNA_types.h"
#include "rna_internal.h"

#include "rna_prototypes_gen.h"

#include "rna_timeline.c"

/* Autogenerated Functions */


CollectionPropertyRNA rna_TimelineMarker_rna_properties;
PointerPropertyRNA rna_TimelineMarker_rna_type;
StringPropertyRNA rna_TimelineMarker_name;
IntPropertyRNA rna_TimelineMarker_frame;
BoolPropertyRNA rna_TimelineMarker_select;
PointerPropertyRNA rna_TimelineMarker_camera;

static PointerRNA TimelineMarker_rna_properties_get(CollectionPropertyIterator *iter)
{
	return rna_builtin_properties_get(iter);
}

void TimelineMarker_rna_properties_begin(CollectionPropertyIterator *iter, PointerRNA *ptr)
{

	memset(iter, 0, sizeof(*iter));
	iter->parent = *ptr;
	iter->prop = (PropertyRNA *)&rna_TimelineMarker_rna_properties;

	rna_builtin_properties_begin(iter, ptr);

	if (iter->valid)
		iter->ptr = TimelineMarker_rna_properties_get(iter);
}

void TimelineMarker_rna_properties_next(CollectionPropertyIterator *iter)
{
	rna_builtin_properties_next(iter);

	if (iter->valid)
		iter->ptr = TimelineMarker_rna_properties_get(iter);
}

void TimelineMarker_rna_properties_end(CollectionPropertyIterator *iter)
{
	rna_iterator_listbase_end(iter);
}

int TimelineMarker_rna_properties_lookup_string(PointerRNA *ptr, const char *key, PointerRNA *r_ptr)
{
	return rna_builtin_properties_lookup_string(ptr, key, r_ptr);
}

PointerRNA TimelineMarker_rna_type_get(PointerRNA *ptr)
{
	return rna_builtin_type_get(ptr);
}

void TimelineMarker_name_get(PointerRNA *ptr, char *value)
{
	TimeMarker *data = (TimeMarker *)(ptr->data);
	BLI_strncpy_utf8(value, data->name, 64);
}

int TimelineMarker_name_length(PointerRNA *ptr)
{
	TimeMarker *data = (TimeMarker *)(ptr->data);
	return strlen(data->name);
}

void TimelineMarker_name_set(PointerRNA *ptr, const char *value)
{
	TimeMarker *data = (TimeMarker *)(ptr->data);
	BLI_strncpy_utf8(data->name, value, 64);
}

int TimelineMarker_frame_get(PointerRNA *ptr)
{
	TimeMarker *data = (TimeMarker *)(ptr->data);
	return (int)(data->frame);
}

void TimelineMarker_frame_set(PointerRNA *ptr, int value)
{
	TimeMarker *data = (TimeMarker *)(ptr->data);
	data->frame = value;
}

bool TimelineMarker_select_get(PointerRNA *ptr)
{
	TimeMarker *data = (TimeMarker *)(ptr->data);
	return (((data->flag) & 1) != 0);
}

void TimelineMarker_select_set(PointerRNA *ptr, bool value)
{
	TimeMarker *data = (TimeMarker *)(ptr->data);
	if (value) data->flag |= 1;
	else data->flag &= ~1;
}

PointerRNA TimelineMarker_camera_get(PointerRNA *ptr)
{
	TimeMarker *data = (TimeMarker *)(ptr->data);
	return rna_pointer_inherit_refine(ptr, &RNA_Object, data->camera);
}

void TimelineMarker_camera_set(PointerRNA *ptr, PointerRNA value)
{
	TimeMarker *data = (TimeMarker *)(ptr->data);
	ID *id = ptr->id.data;
	if (id == value.data) return;

	if (value.data)
		id_lib_extern((ID *)value.data);

	data->camera = value.data;
}


/* Marker */
CollectionPropertyRNA rna_TimelineMarker_rna_properties = {
	{(PropertyRNA *)&rna_TimelineMarker_rna_type, NULL,
	-1, "rna_properties", 0, 0, 1, 0, "Properties",
	"RNA property collection",
	0, "*",
	PROP_COLLECTION, PROP_NONE | PROP_UNIT_NONE, NULL, 0, {0, 0, 0}, 0,
	NULL, 0, NULL, NULL,
	0, -1, NULL},
	TimelineMarker_rna_properties_begin, TimelineMarker_rna_properties_next, TimelineMarker_rna_properties_end, TimelineMarker_rna_properties_get, NULL, NULL, TimelineMarker_rna_properties_lookup_string, NULL, &RNA_Property
};

PointerPropertyRNA rna_TimelineMarker_rna_type = {
	{(PropertyRNA *)&rna_TimelineMarker_name, (PropertyRNA *)&rna_TimelineMarker_rna_properties,
	-1, "rna_type", 8912896, 0, 0, 0, "RNA",
	"RNA type definition",
	0, "*",
	PROP_POINTER, PROP_NONE | PROP_UNIT_NONE, NULL, 0, {0, 0, 0}, 0,
	NULL, 0, NULL, NULL,
	0, -1, NULL},
	TimelineMarker_rna_type_get, NULL, NULL, NULL,&RNA_Struct
};

StringPropertyRNA rna_TimelineMarker_name = {
	{(PropertyRNA *)&rna_TimelineMarker_frame, (PropertyRNA *)&rna_TimelineMarker_rna_type,
	-1, "name", 262145, 0, 0, 0, "Name",
	"",
	0, "*",
	PROP_STRING, PROP_NONE | PROP_UNIT_NONE, NULL, 0, {64, 0, 0}, 0,
	rna_TimelineMarker_update, 0, NULL, NULL,
	0, -1, NULL},
	TimelineMarker_name_get, TimelineMarker_name_length, TimelineMarker_name_set, NULL, NULL, NULL, 64, ""
};

IntPropertyRNA rna_TimelineMarker_frame = {
	{(PropertyRNA *)&rna_TimelineMarker_select, (PropertyRNA *)&rna_TimelineMarker_name,
	-1, "frame", 3, 0, 4, 0, "Frame",
	"The frame on which the timeline marker appears",
	0, "*",
	PROP_INT, PROP_TIME | PROP_UNIT_TIME, NULL, 0, {0, 0, 0}, 0,
	rna_TimelineMarker_update, 0, NULL, NULL,
	offsetof(TimeMarker, frame), 0, NULL},
	TimelineMarker_frame_get, TimelineMarker_frame_set, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	-10000, 10000, INT_MIN, INT_MAX, 1, 0, NULL
};

BoolPropertyRNA rna_TimelineMarker_select = {
	{(PropertyRNA *)&rna_TimelineMarker_camera, (PropertyRNA *)&rna_TimelineMarker_frame,
	-1, "select", 3, 0, 0, 0, "Select",
	"Marker selection state",
	0, "*",
	PROP_BOOLEAN, PROP_NONE | PROP_UNIT_NONE, NULL, 0, {0, 0, 0}, 0,
	rna_TimelineMarker_update, 0, NULL, NULL,
	0, -1, NULL},
	TimelineMarker_select_get, TimelineMarker_select_set, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL
};

PointerPropertyRNA rna_TimelineMarker_camera = {
	{NULL, (PropertyRNA *)&rna_TimelineMarker_select,
	-1, "camera", 9437185, 0, 0, 0, "Camera",
	"Camera this timeline sets to active",
	0, "*",
	PROP_POINTER, PROP_NONE | PROP_UNIT_NONE, NULL, 0, {0, 0, 0}, 0,
	NULL, 0, NULL, NULL,
	0, -1, NULL},
	TimelineMarker_camera_get, TimelineMarker_camera_set, NULL, NULL,&RNA_Object
};

StructRNA RNA_TimelineMarker = {
	{(ContainerRNA *)&RNA_Sound, (ContainerRNA *)&RNA_Text,
	NULL,
	{(PropertyRNA *)&rna_TimelineMarker_rna_properties, (PropertyRNA *)&rna_TimelineMarker_camera}},
	"TimelineMarker", NULL, NULL, 516, NULL, "Marker",
	"Marker for noting points in the timeline",
	"*", 17,
	(PropertyRNA *)&rna_TimelineMarker_name, (PropertyRNA *)&rna_TimelineMarker_rna_properties,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	{NULL, NULL}
};

