/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2005 by the Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/space_view3d/drawarmature.c
 *  \ingroup spview3d
 */


#include <stdlib.h>
#include <string.h>
#include <math.h>


#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_constraint_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"
#include "DNA_object_types.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_dlrbTree.h"
#include "BLI_utildefines.h"

#include "BKE_animsys.h"
#include "BKE_action.h"
#include "BKE_armature.h"
#include "BKE_global.h"
#include "BKE_modifier.h"
#include "BKE_nla.h"

#include "GPU_colors.h"
#include "GPU_primitives.h"

#include "BIF_glutil.h"

#include "ED_armature.h"
#include "ED_keyframes_draw.h"

#include "BLF_api.h"

#include "UI_resources.h"

#include "view3d_intern.h"


/* *************** Armature Drawing - Coloring API ***************************** */

/* global here is reset before drawing each bone */
static ThemeWireColor *bcolor = NULL;

/* values of colCode for set_pchan_gpuCurrentColor */
enum {
	PCHAN_COLOR_NORMAL  = 0,        /* normal drawing */
	PCHAN_COLOR_SOLID,              /* specific case where "solid" color is needed */
	PCHAN_COLOR_CONSTS,             /* "constraint" colors (which may/may-not be suppressed) */

	PCHAN_COLOR_SPHEREBONE_BASE,    /* for the 'stick' of sphere (envelope) bones */
	PCHAN_COLOR_SPHEREBONE_END,     /* for the ends of sphere (envelope) bones */
	PCHAN_COLOR_LINEBONE            /* for the middle of line-bones */
};

/* This function sets the color-set for coloring a certain bone */
static void set_pchan_colorset(Object *ob, bPoseChannel *pchan)
{
	bPose *pose = (ob) ? ob->pose : NULL;
	bArmature *arm = (ob) ? ob->data : NULL;
	bActionGroup *grp = NULL;
	short color_index = 0;
	
	/* sanity check */
	if (ELEM4(NULL, ob, arm, pose, pchan)) {
		bcolor = NULL;
		return;
	}
	
	/* only try to set custom color if enabled for armature */
	if (arm->flag & ARM_COL_CUSTOM) {
		/* currently, a bone can only use a custom color set if it's group (if it has one),
		 * has been set to use one
		 */
		if (pchan->agrp_index) {
			grp = (bActionGroup *)BLI_findlink(&pose->agroups, (pchan->agrp_index - 1));
			if (grp)
				color_index = grp->customCol;
		}
	}
	
	/* bcolor is a pointer to the color set to use. If NULL, then the default
	 * color set (based on the theme colors for 3d-view) is used. 
	 */
	if (color_index > 0) {
		bTheme *btheme = UI_GetTheme();
		bcolor = &btheme->tarm[(color_index - 1)];
	}
	else if (color_index == -1) {
		/* use the group's own custom color set */
		bcolor = (grp) ? &grp->cs : NULL;
	}
	else 
		bcolor = NULL;
}

/* This function is for brightening/darkening a given color (like UI_ThemeColorShade()) */
static void cp_shade_color3ub(unsigned char cp[3], const int offset)
{
	int r, g, b;
	
	r = offset + (int) cp[0];
	CLAMP(r, 0, 255);
	g = offset + (int) cp[1];
	CLAMP(g, 0, 255);
	b = offset + (int) cp[2];
	CLAMP(b, 0, 255);
	
	cp[0] = r;
	cp[1] = g;
	cp[2] = b;
}

/* This function sets the gl-color for coloring a certain bone (based on bcolor) */
static bool set_pchan_gpuCurrentColor(short colCode, int boneflag, short constflag)
{
	switch (colCode) {
		case PCHAN_COLOR_NORMAL:
		{
			if (bcolor) {
				unsigned char cp[3];
			
				if (boneflag & BONE_DRAW_ACTIVE) {
					copy_v3_v3_char((char *)cp, bcolor->active);
					if (!(boneflag & BONE_SELECTED)) {
						cp_shade_color3ub(cp, -80);
					}
				}
				else if (boneflag & BONE_SELECTED) {
					copy_v3_v3_char((char *)cp, bcolor->select);
				}
				else {
					/* a bit darker than solid */
					copy_v3_v3_char((char *)cp, bcolor->solid);
					cp_shade_color3ub(cp, -50);
				}
			
				gpuCurrentColor3ubv(cp);
			}
			else {
				if ((boneflag & BONE_DRAW_ACTIVE) && (boneflag & BONE_SELECTED)) {
					UI_ThemeColor(TH_BONE_POSE_ACTIVE);
				}
				else if (boneflag & BONE_DRAW_ACTIVE) {
					UI_ThemeColorBlend(TH_WIRE, TH_BONE_POSE, 0.15f); /* unselected active */
				}
				else if (boneflag & BONE_SELECTED) {
					UI_ThemeColor(TH_BONE_POSE);
				}
				else {
					UI_ThemeColor(TH_WIRE);
				}
			}
	
			return true;
		}
		break;
		
		case PCHAN_COLOR_SOLID:
		{
			if (bcolor) {
				gpuCurrentColor3ubv((unsigned char *)bcolor->solid);
			}
			else
				UI_ThemeColor(TH_BONE_SOLID);
			
			return true;
		}
		break;
		
		case PCHAN_COLOR_CONSTS:
		{
			if ((bcolor == NULL) || (bcolor->flag & TH_WIRECOLOR_CONSTCOLS)) {
				if (constflag & PCHAN_HAS_TARGET) gpuCurrentColor4ub(255, 150, 0, 80);
				else if (constflag & PCHAN_HAS_IK) gpuCurrentColor4ub(255, 255, 0, 80);
				else if (constflag & PCHAN_HAS_SPLINEIK) gpuCurrentColor4ub(200, 255, 0, 80);
				else if (constflag & PCHAN_HAS_CONST) gpuCurrentColor4ub(0, 255, 120, 80);
			
				return true;
			}
			else
				return 0;
		}
		break;

		case PCHAN_COLOR_SPHEREBONE_BASE:
		{
			if (bcolor) {
				unsigned char cp[3];

				if (boneflag & BONE_DRAW_ACTIVE) {
					copy_v3_v3_char((char *)cp, bcolor->active);
				}
				else if (boneflag & BONE_SELECTED) {
					copy_v3_v3_char((char *)cp, bcolor->select);
				}
				else {
					copy_v3_v3_char((char *)cp, bcolor->solid);
				}

				gpuCurrentColor3ubv(cp);
			}
			else {
				if (boneflag & BONE_DRAW_ACTIVE) UI_ThemeColorShade(TH_BONE_POSE, 40);
				else if (boneflag & BONE_SELECTED) UI_ThemeColor(TH_BONE_POSE);
				else UI_ThemeColor(TH_BONE_SOLID);
			}
			
			return true;
		}
		break;
		case PCHAN_COLOR_SPHEREBONE_END:
		{
			if (bcolor) {
				unsigned char cp[3];

				if (boneflag & BONE_DRAW_ACTIVE) {
					copy_v3_v3_char((char *)cp, bcolor->active);
					cp_shade_color3ub(cp, 10);
				}
				else if (boneflag & BONE_SELECTED) {
					copy_v3_v3_char((char *)cp, bcolor->select);
					cp_shade_color3ub(cp, -30);
				}
				else {
					copy_v3_v3_char((char *)cp, bcolor->solid);
					cp_shade_color3ub(cp, -30);
				}
			
				gpuCurrentColor3ubv(cp);
			}
			else {
				if (boneflag & BONE_DRAW_ACTIVE) UI_ThemeColorShade(TH_BONE_POSE, 10);
				else if (boneflag & BONE_SELECTED) UI_ThemeColorShade(TH_BONE_POSE, -30);
				else UI_ThemeColorShade(TH_BONE_SOLID, -30);
			}
		}
		break;
		
		case PCHAN_COLOR_LINEBONE:
		{
			/* inner part in background color or constraint */
			if ((constflag) && ((bcolor == NULL) || (bcolor->flag & TH_WIRECOLOR_CONSTCOLS))) {
				if (constflag & PCHAN_HAS_TARGET) gpuCurrentColor3ub(255, 150, 0);
				else if (constflag & PCHAN_HAS_IK) gpuCurrentColor3ub(255, 255, 0);
				else if (constflag & PCHAN_HAS_SPLINEIK) gpuCurrentColor3ub(200, 255, 0);
				else if (constflag & PCHAN_HAS_CONST) gpuCurrentColor3ub(0, 255, 120);
				else if (constflag) UI_ThemeColor(TH_BONE_POSE);  /* PCHAN_HAS_ACTION */
			}
			else {
				if (bcolor) {
					char *cp = bcolor->solid;
					gpuCurrentColor4ub(cp[0], cp[1], cp[2], 204);
				}
				else
					UI_ThemeColorShade(TH_BACK, -30);
			}
		
			return true;
		}
		break;
	}
	
	return false;
}

static void set_ebone_gpuCurrentColor(const unsigned int boneflag)
{
	if ((boneflag & BONE_DRAW_ACTIVE) && (boneflag & BONE_SELECTED)) {
		UI_ThemeColor(TH_EDGE_SELECT);
	}
	else if (boneflag & BONE_DRAW_ACTIVE) {
		UI_ThemeColorBlend(TH_WIRE_EDIT, TH_EDGE_SELECT, 0.15f); /* unselected active */
	}
	else if (boneflag & BONE_SELECTED) {
		UI_ThemeColorShade(TH_EDGE_SELECT, -20);
	}
	else {
		UI_ThemeColor(TH_WIRE_EDIT);
	}
}

/* *************** Armature drawing, helper calls for parts ******************* */

static void draw_bonevert(void)
{
	gpuPushMatrix();

	gpuImmediateFormat_V2(); // DOODLE: bonevert, 3 orthogonal circles

	gpuDrawCircle(0, 0, 0.052, 16);

	gpuRotateRight('Y');
	gpuRepeat();

	gpuRotateRight('X');
	gpuRepeat();

	gpuImmediateUnformat();

	gpuPopMatrix();
}

static void draw_bonevert_solid(void)
{
	static GPUimmediate *displist = NULL;
	static GPUindex* index = NULL;

	gpuShadeModel(GL_SMOOTH);

	if (!displist) {
		GPUprim3 prim = GPU_PRIM_MIDFI_SOLID;
		prim.usegs = 8;
		prim.vsegs = 5;

		gpuPushImmediate();
		gpuImmediateMaxVertexCount(48);

		index = gpuNewIndex();
		gpuImmediateIndex(index);
		gpuImmediateMaxIndexCount(240, GL_UNSIGNED_SHORT);

		gpuSingleSphere(&prim, 0.05f);

		displist = gpuPopImmediate();
	}
	else {
		gpuImmediateSingleRepeatElements(displist);
	}

	gpuShadeModel(GL_FLAT);
}

static GLfloat bone_octahedral_verts[8][3] = {
	{ 0.0f, 0.0f,  0.0f}, /* 0 */
	{ 0.1f, 0.1f,  0.1f}, /* 1 */
	{ 0.1f, 0.1f, -0.1f}, /* 2 */
	{-0.1f, 0.1f, -0.1f}, /* 3 */
	{-0.1f, 0.1f,  0.1f}, /* 4 */
	{ 0.0f, 1.0f,  0.0f}, /* 5 */

	/* there are more faces (8) than verts (6),
	   so make more (2) so the counts match,
	   otherwise we cannot flat shade */
	{ 0.1f, 0.1f,  0.1f}, /* dup of #1 */
	{-0.1f, 0.1f, -0.1f}, /* dup of #3 */
};

/* Eulerian path over octohedron */
static GLuint bone_octahedral_wire[] =
	{0, 1, 4, 5, 2, 3, 0, 2, 1, 5, 3, 4};

static GLfloat bone_octahedral_solid_normals[8][3] = {
	{+0.70710683f, -0.70710683f,  0.00000000f},
	{ 0.00000000f, -0.70710683f, +0.70710683f},
	{ 0.00000000f, -0.70710683f, -0.70710683f},
	{-0.70710683f, -0.70710683f,  0.00000000f},

	{-0.99388373f, +0.11043154f,  0.00000000f},
	{+0.99388373f, +0.11043154f,  0.00000000f},
	{ 0.00000000f, +0.11043154f, +0.99388373f},
	{ 0.00000000f, +0.11043154f, -0.99388373f},
};

static GLuint bone_octahedral_solid_tris[8][3] = {
	/* notice that provoking vertex is last in each tri,
	   the normal of that vertex is what is used for lighting */

	{2, 1, 0}, /* bottom */
	{4, 0, 1},
	{0, 3, 2},
	{0, 4, 3},

	{5, 3, 4}, /* top */
	{1, 2, 5},
	{5, 4, 6}, /* #6 pos is same as #1 */
	{5, 2, 7}, /* #7 pos is same as #3 */
};

static void draw_bone_octahedral(void)
{
	static GPUimmediate *displist = NULL;
	static GPUindex *index = NULL;

	if (!displist) {
		const GLsizei vertex_count = 6;
		const GLsizei index_count  = 12;

		gpuPushImmediate();
		gpuImmediateMaxVertexCount(vertex_count);

		index = gpuNewIndex();
		gpuImmediateIndex(index);
		gpuImmediateMaxIndexCount(index_count, GL_UNSIGNED_INT); // XXX jwilkins: telling the type here is too early

		gpuSingleClientRangeElements_V3F(
			GL_LINE_LOOP,
			bone_octahedral_verts,
			0,
			0,
			5,
			index_count,
			bone_octahedral_wire);

		displist = gpuPopImmediate();
	}
	else {
		gpuImmediateSingleRepeatRangeElements(displist);
	}
}

static void draw_bone_solid_octahedral(void)
{
	static GPUimmediate *displist = NULL;
	static GPUindex* index = NULL;

	gpuShadeModel(GL_FLAT);

	if (!displist) {
		const GLsizei index_count  = 24;
		const GLsizei vertex_count = 8; /* 2 extra duplicate verts because or normals */

		gpuPushImmediate();
		gpuImmediateMaxVertexCount(vertex_count);

		index = gpuNewIndex();
		gpuImmediateIndex(index);
		gpuImmediateMaxIndexCount(index_count, GL_UNSIGNED_INT);// XXX jwilkins: telling the type here is too early

		gpuSingleClientRangeElements_N3F_V3F(
			GL_TRIANGLES,
			bone_octahedral_solid_normals,
			0,
			bone_octahedral_verts,
			0,
			0,
			7,
			index_count,
			bone_octahedral_solid_tris[0]);

		displist = gpuPopImmediate();
	}
	else {
		gpuImmediateSingleRepeatRangeElements(displist);
	}

	gpuShadeModel(GL_SMOOTH);
}

/* *************** Armature drawing, bones ******************* */


static void draw_bone_points(const short dt, int armflag, unsigned int boneflag, int id)
{
	/*	Draw root point if we are not connected */
	if ((boneflag & BONE_CONNECTED) == 0) {
		if (id != -1)
			gpuSelectLoad(id | BONESEL_ROOT);
		
		if (dt <= OB_WIRE) {
			if (armflag & ARM_EDITMODE) {
				if (boneflag & BONE_ROOTSEL) UI_ThemeColor(TH_VERTEX_SELECT);
				else UI_ThemeColor(TH_VERTEX);
			}
		}
		else {
			if (armflag & ARM_POSEMODE) 
				set_pchan_gpuCurrentColor(PCHAN_COLOR_SOLID, boneflag, 0);
			else
				UI_ThemeColor(TH_BONE_SOLID);
		}
		
		if (dt > OB_WIRE) 
			draw_bonevert_solid();
		else 
			draw_bonevert();
	}
	
	/*	Draw tip point */
	if (id != -1)
		gpuSelectLoad(id | BONESEL_TIP);
	
	if (dt <= OB_WIRE) {
		if (armflag & ARM_EDITMODE) {
			if (boneflag & BONE_TIPSEL) UI_ThemeColor(TH_VERTEX_SELECT);
			else UI_ThemeColor(TH_VERTEX);
		}
	}
	else {
		if (armflag & ARM_POSEMODE) 
			set_pchan_gpuCurrentColor(PCHAN_COLOR_SOLID, boneflag, 0);
		else
			UI_ThemeColor(TH_BONE_SOLID);
	}
	
	gpuTranslate(0.0f, 1.0f, 0.0f);
	if (dt > OB_WIRE) 
		draw_bonevert_solid();
	else 
		draw_bonevert();
	gpuTranslate(0.0f, -1.0f, 0.0f);
	
}

/* 16 values of sin function (still same result!) */
static float si[16] = {
	0.00000000f,
	0.20129852f, 0.39435585f,
	0.57126821f, 0.72479278f,
	0.84864425f, 0.93775213f,
	0.98846832f, 0.99871650f,
	0.96807711f, 0.89780453f,
	0.79077573f, 0.65137248f,
	0.48530196f, 0.29936312f,
	0.10116832f
};
/* 16 values of cos function (still same result!) */
static float co[16] = {
	1.00000000f,
	0.97952994f, 0.91895781f,
	0.82076344f, 0.68896691f,
	0.52896401f, 0.34730525f,
	0.15142777f, -0.05064916f,
	-0.25065253f, -0.44039415f,
	-0.61210598f, -0.75875812f,
	-0.87434661f, -0.95413925f,
	-0.99486932f
};



/* smat, imat = mat & imat to draw screenaligned */
static void draw_sphere_bone_dist(float smat[4][4], float imat[4][4], bPoseChannel *pchan, EditBone *ebone)
{
	float head, tail, dist /*, length*/;
	float *headvec, *tailvec, dirvec[3];
	
	/* figure out the sizes of spheres */
	if (ebone) {
		/* this routine doesn't call get_matrix_editbone() that calculates it */
		ebone->length = len_v3v3(ebone->head, ebone->tail);

		/*length = ebone->length;*/ /*UNUSED*/
		tail = ebone->rad_tail;
		dist = ebone->dist;
		if (ebone->parent && (ebone->flag & BONE_CONNECTED))
			head = ebone->parent->rad_tail;
		else
			head = ebone->rad_head;
		headvec = ebone->head;
		tailvec = ebone->tail;
	}
	else {
		/*length = pchan->bone->length;*/ /*UNUSED*/
		tail = pchan->bone->rad_tail;
		dist = pchan->bone->dist;
		if (pchan->parent && (pchan->bone->flag & BONE_CONNECTED))
			head = pchan->parent->bone->rad_tail;
		else
			head = pchan->bone->rad_head;
		headvec = pchan->pose_head;
		tailvec = pchan->pose_tail;
	}
	
	/* ***** draw it ***** */
	
	/* move vector to viewspace */
	sub_v3_v3v3(dirvec, tailvec, headvec);
	mul_mat3_m4_v3(smat, dirvec);
	/* clear zcomp */
	dirvec[2] = 0.0f;

	if (head != tail) {
		/* correction when viewing along the bones axis
		 * it pops in and out but better then artifacts, [#23841] */
		float view_dist = len_v2(dirvec);

		if (head - view_dist > tail) {
			tailvec = headvec;
			tail = head;
			zero_v3(dirvec);
			dirvec[0] = 0.00001;  /* XXX. weak but ok */
		}
		else if (tail - view_dist > head) {
			headvec = tailvec;
			head = tail;
			zero_v3(dirvec);
			dirvec[0] = 0.00001;  /* XXX. weak but ok */
		}
	}

	/* move vector back */
	mul_mat3_m4_v3(imat, dirvec);
	
	if (0.0f != normalize_v3(dirvec)) {
		float norvec[3], vec1[3], vec2[3], vec[3];
		int a;
		
		//mul_v3_fl(dirvec, head);
		cross_v3_v3v3(norvec, dirvec, imat[2]);
		
		gpuBegin(GL_TRIANGLE_STRIP);
		
		for (a = 0; a < 16; a++) {
			vec[0] = -si[a] * dirvec[0] + co[a] * norvec[0];
			vec[1] = -si[a] * dirvec[1] + co[a] * norvec[1];
			vec[2] = -si[a] * dirvec[2] + co[a] * norvec[2];

			madd_v3_v3v3fl(vec1, headvec, vec, head);
			madd_v3_v3v3fl(vec2, headvec, vec, head + dist);
			
			gpuColor4x(CPACK_WHITE, 0.196f);
			gpuVertex3fv(vec1);
			//gpuColor4x(CPACK_WHITE, 0);
			gpuVertex3fv(vec2);
		}
		
		for (a = 15; a >= 0; a--) {
			vec[0] = si[a] * dirvec[0] + co[a] * norvec[0];
			vec[1] = si[a] * dirvec[1] + co[a] * norvec[1];
			vec[2] = si[a] * dirvec[2] + co[a] * norvec[2];

			madd_v3_v3v3fl(vec1, tailvec, vec, tail);
			madd_v3_v3v3fl(vec2, tailvec, vec, tail + dist);
			
			//gpuColor4x(CPACK_WHITE, 0.196f);
			gpuVertex3fv(vec1);
			//gpuColor4x(CPACK_WHITE, 0);
			gpuVertex3fv(vec2);
		}
		/* make it cyclic... */
		
		vec[0] = -si[0] * dirvec[0] + co[0] * norvec[0];
		vec[1] = -si[0] * dirvec[1] + co[0] * norvec[1];
		vec[2] = -si[0] * dirvec[2] + co[0] * norvec[2];

		madd_v3_v3v3fl(vec1, headvec, vec, head);
		madd_v3_v3v3fl(vec2, headvec, vec, head + dist);

		//gpuColor4x(CPACK_WHITE, 0.196f);
		gpuVertex3fv(vec1);
		//gpuColor4x(CPACK_WHITE, 0);
		gpuVertex3fv(vec2);
		
		gpuEnd();
	}
}


/* smat, imat = mat & imat to draw screenaligned */
static void draw_sphere_bone_wire(float smat[4][4], float imat[4][4],
                                  int armflag, int boneflag, short constflag, unsigned int id,
                                  bPoseChannel *pchan, EditBone *ebone)
{
	float head, tail /*, length*/;
	float *headvec, *tailvec, dirvec[3];

	gpuImmediateFormat_V3(); // DOODLE: sphere bone wire

	/* figure out the sizes of spheres */
	if (ebone) {
		/* this routine doesn't call get_matrix_editbone() that calculates it */
		ebone->length = len_v3v3(ebone->head, ebone->tail);

		/*length = ebone->length;*/ /*UNUSED*/
		tail = ebone->rad_tail;
		if (ebone->parent && (boneflag & BONE_CONNECTED))
			head = ebone->parent->rad_tail;
		else
			head = ebone->rad_head;
		headvec = ebone->head;
		tailvec = ebone->tail;
	}
	else {
		/*length = pchan->bone->length;*/ /*UNUSED*/
		tail = pchan->bone->rad_tail;
		if ((pchan->parent) && (boneflag & BONE_CONNECTED))
			head = pchan->parent->bone->rad_tail;
		else
			head = pchan->bone->rad_head;
		headvec = pchan->pose_head;
		tailvec = pchan->pose_tail;
	}

	/* sphere root color */
	if (armflag & ARM_EDITMODE) {
		if (boneflag & BONE_ROOTSEL) UI_ThemeColor(TH_VERTEX_SELECT);
		else UI_ThemeColor(TH_VERTEX);
	}
	else if (armflag & ARM_POSEMODE)
		set_pchan_gpuCurrentColor(PCHAN_COLOR_NORMAL, boneflag, constflag);

	/*	Draw root point if we are not connected */
	if ((boneflag & BONE_CONNECTED) == 0) {
		if (id != -1)
			gpuSelectLoad(id | BONESEL_ROOT);
		
		gpuDrawFastBall(GL_LINE_LOOP, headvec, head, imat);
	}

	/*	Draw tip point */
	if (armflag & ARM_EDITMODE) {
		if (boneflag & BONE_TIPSEL) UI_ThemeColor(TH_VERTEX_SELECT);
		else UI_ThemeColor(TH_VERTEX);
	}

	if (id != -1)
		gpuSelectLoad(id | BONESEL_TIP);

	gpuDrawFastBall(GL_LINE_LOOP, tailvec, tail, imat);

	/* base */
	if (armflag & ARM_EDITMODE) {
		if (boneflag & BONE_SELECTED) UI_ThemeColor(TH_SELECT);
		else UI_ThemeColor(TH_WIRE_EDIT);
	}

	sub_v3_v3v3(dirvec, tailvec, headvec);

	/* move vector to viewspace */
	mul_mat3_m4_v3(smat, dirvec);
	/* clear zcomp */
	dirvec[2] = 0.0f;
	/* move vector back */
	mul_mat3_m4_v3(imat, dirvec);

	if (0.0f != normalize_v3(dirvec)) {
		float norvech[3], norvect[3], vec[3];

		copy_v3_v3(vec, dirvec);

		mul_v3_fl(dirvec, head);
		cross_v3_v3v3(norvech, dirvec, imat[2]);

		mul_v3_fl(vec, tail);
		cross_v3_v3v3(norvect, vec, imat[2]);

		if (id != -1)
			gpuSelectLoad(id | BONESEL_BONE);

		gpuBegin(GL_LINES);

		add_v3_v3v3(vec, headvec, norvech);
		gpuVertex3fv(vec);

		add_v3_v3v3(vec, tailvec, norvect);
		gpuVertex3fv(vec);

		sub_v3_v3v3(vec, headvec, norvech);
		gpuVertex3fv(vec);

		sub_v3_v3v3(vec, tailvec, norvect);
		gpuVertex3fv(vec);

		gpuEnd();
	}

	gpuImmediateUnformat();
}

/* does wire only for outline selecting */
static void draw_sphere_bone(const short dt, int armflag, int boneflag, short constflag, unsigned int id,
                             bPoseChannel *pchan, EditBone *ebone)
{
	GPUprim3 prim;
	float head, tail, length;
	float fac1, fac2;

	gpuImmediateFormat_V3();

	gpuPushMatrix();

	/* figure out the sizes of spheres */
	if (ebone) {
		length = ebone->length;
		tail = ebone->rad_tail;
		if (ebone->parent && (boneflag & BONE_CONNECTED))
			head = ebone->parent->rad_tail;
		else
			head = ebone->rad_head;
	}
	else {
		length = pchan->bone->length;
		tail = pchan->bone->rad_tail;
		if (pchan->parent && (boneflag & BONE_CONNECTED))
			head = pchan->parent->bone->rad_tail;
		else
			head = pchan->bone->rad_head;
	}

	/* move to z-axis space */
	gpuRotateRight(-'X');

	if (dt == OB_SOLID) {
		/* set up solid drawing */
		gpuEnableColorMaterial();
		gpuEnableLighting();

		gpuShadeModel(GL_SMOOTH);

		prim = GPU_PRIM_MIDFI_SOLID;
	}
	else {
		prim = GPU_PRIM_MIDFI_WIRE;
	}

	/* sphere root color */
	if (armflag & ARM_EDITMODE) {
		if (boneflag & BONE_ROOTSEL) {
			UI_ThemeColor(TH_VERTEX_SELECT);
		}
		else {
			UI_ThemeColorShade(TH_BONE_SOLID, -30);
		}
	}
	else if (armflag & ARM_POSEMODE) {
		set_pchan_gpuCurrentColor(PCHAN_COLOR_SPHEREBONE_END, boneflag, constflag);
	}
	else if (dt == OB_SOLID) {
		UI_ThemeColorShade(TH_BONE_SOLID, -30);
	}

	/*	Draw root point if we are not connected */
	if ((boneflag & BONE_CONNECTED) == 0) {
		if (id != -1) {
			gpuSelectLoad(id | BONESEL_ROOT);
		}

		gpuDrawSphere(&prim, head);
		//GLU Sphere(qobj, head, 16, 10);
	}

	/*	Draw tip point */
	if (armflag & ARM_EDITMODE) {
		if (boneflag & BONE_TIPSEL) {
			UI_ThemeColor(TH_VERTEX_SELECT);
		}
		else {
			UI_ThemeColorShade(TH_BONE_SOLID, -30);
		}
	}

	if (id != -1) {
		gpuSelectLoad(id | BONESEL_TIP);
	}

	gpuTranslate(0.0f, 0.0f, length);
	gpuDrawSphere(&prim, tail);
	//GLU Sphere(qobj, tail, 16, 10);
	gpuTranslate(0.0f, 0.0f, -length);

	/* base */
	if (armflag & ARM_EDITMODE) {
		if (boneflag & BONE_SELECTED) {
			UI_ThemeColor(TH_SELECT);
		}
		else {
			UI_ThemeColor(TH_BONE_SOLID);
		}
	}
	else if (armflag & ARM_POSEMODE) {
		set_pchan_gpuCurrentColor(PCHAN_COLOR_SPHEREBONE_BASE, boneflag, constflag);
	}
	else if (dt == OB_SOLID) {
		UI_ThemeColor(TH_BONE_SOLID);
	}

	fac1 = (length - head) / length;
	fac2 = (length - tail) / length;

	if (length > (head + tail)) {
		if (id != -1) {
			gpuSelectLoad(id | BONESEL_BONE);
		}

		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(-1.0f, -1.0f);

		gpuTranslate(0.0f, 0.0f, head);
		gpuDrawCylinder(
			&prim,
			fac1 * head + (1.0f - fac1) * tail,
			fac2 * tail + (1.0f - fac2) * head,
			length - head - tail);
		//GLU Cylinder(qobj, fac1 * head + (1.0f - fac1) * tail, fac2 * tail + (1.0f - fac2) * head, length - head - tail, 16, 1);
		gpuTranslate(0.0f, 0.0f, -head);

		glDisable(GL_POLYGON_OFFSET_FILL);

		/* draw sphere on extrema */
		gpuTranslate(0.0f, 0.0f, length - tail);
		gpuDrawSphere(&prim, fac2 * tail + (1.0f - fac2) * head);
		//GLU Sphere(qobj, fac2 * tail + (1.0f - fac2) * head, 16, 10);
		gpuTranslate(0.0f, 0.0f, -length + tail);

		gpuTranslate(0.0f, 0.0f, head);
		gpuDrawSphere(&prim,  fac1 * head + (1.0f - fac1) * tail);
		//GLU Sphere(qobj, fac1 * head + (1.0f - fac1) * tail, 16, 10);
	}
	else {
		/* 1 sphere in center */
		gpuTranslate(0.0f, 0.0f, (head + length - tail) / 2.0f);
		gpuDrawSphere(&prim,  fac1 * head + (1.0f - fac1) * tail);
		//GLU Sphere(qobj, fac1 * head + (1.0f - fac1) * tail, 16, 10);
	}

	/* restore */
	if (dt == OB_SOLID) {
		gpuShadeModel(GL_FLAT);
		gpuDisableLighting();
		gpuDisableColorMaterial();
	}

	gpuPopMatrix();

	gpuImmediateFormat_V3();
}



static GLubyte   bm_dot6_data[] = {0x00, 0x18, 0x3C, 0x7E, 0x7E, 0x3C, 0x18, 0x00};
static GPUbitmap bm_dot6        = {8, 8, 4, 4, bm_dot6_data};

static GLubyte   bm_dot8_data[] = {0x3C, 0x7E, 0xFF, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C};
static GPUbitmap bm_dot8        = {8, 8, 4, 4, bm_dot8_data};

static GLubyte   bm_dot5_data[] = {0x00, 0x00, 0x10, 0x38, 0x7c, 0x38, 0x10, 0x00};
static GPUbitmap bm_dot5        = {8, 8, 4, 4, bm_dot5_data};

static GLubyte   bm_dot7_data[] = {0x00, 0x38, 0x7C, 0xFE, 0xFE, 0xFE, 0x7C, 0x38};
static GPUbitmap bm_dot7        = {8, 8, 4, 4, bm_dot7_data};



static void draw_line_bone(int armflag, int boneflag, short constflag, unsigned int id,
                           bPoseChannel *pchan, EditBone *ebone)
{
	float length;

	gpuPixelFormat(GL_UNPACK_ALIGNMENT, 1);
	gpuPixelsBegin();

	if (pchan) 
		length = pchan->bone->length;
	else 
		length = ebone->length;

	if (G.f & G_PICKSEL) {
		gpuAspectBegin(GPU_ASPECT_TEXTURE, NULL);

		gpuImmediateFormat_T2_V3();
	}
	else {
		gpuImmediateFormat_V3();
	}

	gpuPushMatrix();
	gpuScale(length, length, length);

	/* this chunk not in object mode */
	if (armflag & (ARM_EDITMODE | ARM_POSEMODE)) {
		gpuLineWidth(4.0f);
		if (armflag & ARM_POSEMODE)
			set_pchan_gpuCurrentColor(PCHAN_COLOR_NORMAL, boneflag, constflag);
		else if (armflag & ARM_EDITMODE) {
			UI_ThemeColor(TH_WIRE_EDIT);
		}

		/*	Draw root point if we are not connected */
		if ((boneflag & BONE_CONNECTED) == 0) {
			if (G.f & G_PICKSEL) {  /* no bitmap in selection mode, crashes 3d cards... */
				gpuSelectLoad(id | BONESEL_ROOT);
				gpuBegin(GL_POINTS);
				gpuVertex3f(0.0f, 0.0f, 0.0f);
				gpuEnd();
			}
			else {
				gpuPixelPos3f(0.0f, 0.0f, 0.0f);
				gpuCacheBitmap(&bm_dot8);
				gpuBitmap(&bm_dot8);
			}
		}

		if (id != -1)
			gpuSelectLoad((GLuint) id | BONESEL_BONE);
		
		gpuBegin(GL_LINES);
		gpuVertex3f(0.0f, 0.0f, 0.0f);
		gpuVertex3f(0.0f, 1.0f, 0.0f);
		gpuEnd();

		/* tip */
		if (G.f & G_PICKSEL) {
			/* no bitmap in selection mode, crashes 3d cards... */
			gpuSelectLoad(id | BONESEL_TIP);
			gpuBegin(GL_POINTS);
			gpuVertex3f(0.0f, 1.0f, 0.0f);
			gpuEnd();
		}
		else {
			gpuPixelPos3f(0.0f, 1.0f, 0.0f);
			gpuCacheBitmap(&bm_dot7);
			gpuBitmap(&bm_dot7);
		}

		/* further we send no names */
		if (id != -1)
			gpuSelectLoad(id & 0xFFFF);  /* object tag, for bordersel optim */

		if (armflag & ARM_POSEMODE)
			set_pchan_gpuCurrentColor(PCHAN_COLOR_LINEBONE, boneflag, constflag);
	}

	gpuLineWidth(2.0);

	/*Draw root point if we are not connected */
	if ((boneflag & BONE_CONNECTED) == 0) {
		if ((G.f & G_PICKSEL) == 0) {
			/* no bitmap in selection mode, crashes 3d cards... */
			if (armflag & ARM_EDITMODE) {
				if (boneflag & BONE_ROOTSEL) UI_ThemeColor(TH_VERTEX_SELECT);
				else UI_ThemeColor(TH_VERTEX);
			}

			gpuPixelPos3f(0.0f, 0.0f, 0.0f);
			gpuCacheBitmap(&bm_dot6);
			gpuBitmap(&bm_dot6);
		}
	}

	if (armflag & ARM_EDITMODE) {
		if (boneflag & BONE_SELECTED) UI_ThemeColor(TH_EDGE_SELECT);
		else UI_ThemeColorShade(TH_BACK, -30);
	}

	gpuBegin(GL_LINES);
	gpuVertex3f(0.0f, 0.0f, 0.0f);
	gpuVertex3f(0.0f, 1.0f, 0.0f);
	gpuEnd();

	/* tip */
	if ((G.f & G_PICKSEL) == 0) {
		/* no bitmap in selection mode, crashes 3d cards... */
		if (armflag & ARM_EDITMODE) {
			if (boneflag & BONE_TIPSEL) UI_ThemeColor(TH_VERTEX_SELECT);
			else UI_ThemeColor(TH_VERTEX);
		}

		gpuPixelPos3f(0.0f, 1.0f, 0.0f);
		gpuCacheBitmap(&bm_dot5);
		gpuBitmap(&bm_dot5);
	}

	gpuLineWidth(1.0);

	gpuPopMatrix();

	gpuImmediateUnformat();

	if (G.f & G_PICKSEL) {
		gpuAspectEnd(GPU_ASPECT_TEXTURE, NULL);
	}

	gpuPixelsEnd();
	gpuPixelFormat(GL_UNPACK_ALIGNMENT, 4);  /* restore default value */
}

static void draw_b_bone_boxes(const short dt, bPoseChannel *pchan, float xwidth, float length, float zwidth)
{
	int segments = 0;
	
	if (pchan) 
		segments = pchan->bone->segments;
	
	if ((segments > 1) && (pchan)) {
		float dlen = length / (float)segments;
		Mat4 *bbone = b_bone_spline_setup(pchan, 0);
		int a;
		
		for (a = 0; a < segments; a++, bbone++) {
			gpuPushMatrix();
			gpuMultMatrix(bbone->mat);
			gpuScale(xwidth, dlen, zwidth);

			if (dt == OB_SOLID) {
				gpuDrawSolidHalfCube();
			}
			else {
				gpuDrawWireHalfCube();
			}

			gpuPopMatrix();

		}
	}
	else {
		gpuPushMatrix();
		gpuScale(xwidth, length, zwidth);

		if (dt == OB_SOLID) {
			gpuDrawSolidHalfCube();
		}
		else {
			gpuDrawWireHalfCube();
		}

		gpuPopMatrix();

	}
}

static void draw_b_bone(const short dt, int armflag, int boneflag, short constflag, unsigned int id,
                        bPoseChannel *pchan, EditBone *ebone)
{
	float xwidth, length, zwidth;
	
	if (pchan) {
		xwidth = pchan->bone->xwidth;
		length = pchan->bone->length;
		zwidth = pchan->bone->zwidth;
	}
	else {
		xwidth = ebone->xwidth;
		length = ebone->length;
		zwidth = ebone->zwidth;
	}
	
	/* draw points only if... */
	if (armflag & ARM_EDITMODE) {
		/* move to unitspace */
		gpuPushMatrix();
		gpuScale(length, length, length);
		draw_bone_points(dt, armflag, boneflag, id);
		gpuPopMatrix();
		length *= 0.95f;  /* make vertices visible */
	}

	/* colors for modes */
	if (armflag & ARM_POSEMODE) {
		if (dt <= OB_WIRE)
			set_pchan_gpuCurrentColor(PCHAN_COLOR_NORMAL, boneflag, constflag);
		else 
			set_pchan_gpuCurrentColor(PCHAN_COLOR_SOLID, boneflag, constflag);
	}
	else if (armflag & ARM_EDITMODE) {
		if (dt == OB_WIRE) {
			set_ebone_gpuCurrentColor(boneflag);
		}
		else 
			UI_ThemeColor(TH_BONE_SOLID);
	}
	
	if (id != -1) {
		gpuSelectLoad((GLuint) id | BONESEL_BONE);
	}
	
	/* set up solid drawing */
	if (dt > OB_WIRE) {
		gpuEnableColorMaterial();
		gpuEnableLighting();
		
		if (armflag & ARM_POSEMODE)
			set_pchan_gpuCurrentColor(PCHAN_COLOR_SOLID, boneflag, constflag);
		else
			UI_ThemeColor(TH_BONE_SOLID);
		
		draw_b_bone_boxes(OB_SOLID, pchan, xwidth, length, zwidth);
		
		/* disable solid drawing */
		gpuDisableColorMaterial();
		gpuDisableLighting();
	}
	else {
		/* wire */
		if (armflag & ARM_POSEMODE) {
			if (constflag) {
				/* set constraint colors */
				if (set_pchan_gpuCurrentColor(PCHAN_COLOR_CONSTS, boneflag, constflag)) {
					glEnable(GL_BLEND);
					
					draw_b_bone_boxes(OB_SOLID, pchan, xwidth, length, zwidth);
					
					glDisable(GL_BLEND);
				}
				
				/* restore colors */
				set_pchan_gpuCurrentColor(PCHAN_COLOR_NORMAL, boneflag, constflag);
			}
		}
		
		draw_b_bone_boxes(OB_WIRE, pchan, xwidth, length, zwidth);
	}
}

static void draw_wire_bone_segments(bPoseChannel *pchan, Mat4 *bbones, float length, int segments)
{
	if ((segments > 1) && (pchan)) {
		float dlen = length / (float)segments;
		Mat4 *bbone = bbones;
		int a;
		
		for (a = 0; a < segments; a++, bbone++) {
			gpuPushMatrix();
			gpuMultMatrix(bbone->mat);
			
			gpuBegin(GL_LINES);
			gpuVertex3f(0.0f, 0.0f, 0.0f);
			gpuVertex3f(0.0f, dlen, 0.0f);
			gpuEnd(); /* GL_LINES */
			
			gpuPopMatrix();
		}
	}
	else {
		gpuPushMatrix();
		
		gpuBegin(GL_LINES);
		gpuVertex3f(0.0f, 0.0f, 0.0f);
		gpuVertex3f(0.0f, length, 0.0f);
		gpuEnd();
		
		gpuPopMatrix();
	}
}

static void draw_wire_bone(const short dt, int armflag, int boneflag, short constflag, unsigned int id,
                           bPoseChannel *pchan, EditBone *ebone)
{
	Mat4 *bbones = NULL;
	int segments = 0;
	float length;
	
	if (pchan) {
		segments = pchan->bone->segments;
		length = pchan->bone->length;
		
		if (segments > 1)
			bbones = b_bone_spline_setup(pchan, 0);
	}
	else 
		length = ebone->length;
	
	/* draw points only if... */
	if (armflag & ARM_EDITMODE) {
		/* move to unitspace */
		gpuPushMatrix();
		gpuScale(length, length, length);
		draw_bone_points(dt, armflag, boneflag, id);
		gpuPopMatrix();
		length *= 0.95f;  /* make vertices visible */
	}
	
	/* this chunk not in object mode */
	if (armflag & (ARM_EDITMODE | ARM_POSEMODE)) {
		if (id != -1)
			gpuSelectLoad((GLuint) id | BONESEL_BONE);
		
		draw_wire_bone_segments(pchan, bbones, length, segments);
		
		/* further we send no names */
		if (id != -1)
			gpuSelectLoad(id & 0xFFFF);    /* object tag, for bordersel optim */
	}
	
	/* colors for modes */
	if (armflag & ARM_POSEMODE) {
		set_pchan_gpuCurrentColor(PCHAN_COLOR_NORMAL, boneflag, constflag);
	}
	else if (armflag & ARM_EDITMODE) {
		set_ebone_gpuCurrentColor(boneflag);
	}
	
	/* draw normal */
	draw_wire_bone_segments(pchan, bbones, length, segments);
}

static void draw_bone(const short dt, int armflag, int boneflag, short constflag, unsigned int id, float length)
{
	
	/* Draw a 3d octahedral bone, we use normalized space based on length,
	 * for display-lists */
	
	gpuScale(length, length, length);

	/* set up solid drawing */
	if (dt > OB_WIRE) {
		gpuEnableColorMaterial();
		gpuEnableLighting();
		UI_ThemeColor(TH_BONE_SOLID);
	}
	
	/* colors for posemode */
	if (armflag & ARM_POSEMODE) {
		if (dt <= OB_WIRE)
			set_pchan_gpuCurrentColor(PCHAN_COLOR_NORMAL, boneflag, constflag);
		else 
			set_pchan_gpuCurrentColor(PCHAN_COLOR_SOLID, boneflag, constflag);
	}
	
	
	draw_bone_points(dt, armflag, boneflag, id);
	
	/* now draw the bone itself */
	if (id != -1) {
		gpuSelectLoad((GLuint) id | BONESEL_BONE);
	}
	
	/* wire? */
	if (dt <= OB_WIRE) {
		/* colors */
		if (armflag & ARM_EDITMODE) {
			set_ebone_gpuCurrentColor(boneflag);
		}
		else if (armflag & ARM_POSEMODE) {
			if (constflag) {
				/* draw constraint colors */
				if (set_pchan_gpuCurrentColor(PCHAN_COLOR_CONSTS, boneflag, constflag)) {
					glEnable(GL_BLEND);
					
					draw_bone_solid_octahedral();
					
					glDisable(GL_BLEND);
				}
				
				/* restore colors */
				set_pchan_gpuCurrentColor(PCHAN_COLOR_NORMAL, boneflag, constflag);
			}
		}
		draw_bone_octahedral();
	}
	else {
		/* solid */
		if (armflag & ARM_POSEMODE)
			set_pchan_gpuCurrentColor(PCHAN_COLOR_SOLID, boneflag, constflag);
		else
			UI_ThemeColor(TH_BONE_SOLID);

		draw_bone_solid_octahedral();
	}

	/* disable solid drawing */
	if (dt > OB_WIRE) {
		gpuDisableColorMaterial();
		gpuDisableLighting();
	}
}

static void draw_custom_bone(Scene *scene, View3D *v3d, RegionView3D *rv3d, Object *ob,
                             const short dt, int armflag, int boneflag, unsigned int id, float length)
{
	if (ob == NULL) return;
	
	gpuScale(length, length, length);
	
	/* colors for posemode */
	if (armflag & ARM_POSEMODE) {
		set_pchan_gpuCurrentColor(PCHAN_COLOR_NORMAL, boneflag, 0);
	}
	
	if (id != -1) {
		gpuSelectLoad((GLuint) id | BONESEL_BONE);
	}
	
	draw_object_instance(scene, v3d, rv3d, ob, dt, armflag & ARM_POSEMODE);
}


static void pchan_draw_IK_root_lines(bPoseChannel *pchan, short only_temp)
{
	bConstraint *con;
	bPoseChannel *parchan;
	
	for (con = pchan->constraints.first; con; con = con->next) {
		if (con->enforce == 0.0f)
			continue;
		
		switch (con->type) {
			case CONSTRAINT_TYPE_KINEMATIC:
			{
				bKinematicConstraint *data = (bKinematicConstraint *)con->data;
				int segcount = 0;
				
				/* if only_temp, only draw if it is a temporary ik-chain */
				if ((only_temp) && !(data->flag & CONSTRAINT_IK_TEMP))
					continue;
				
				setlinestyle(3);
				gpuBegin(GL_LINES);
				
				/* exclude tip from chain? */
				if ((data->flag & CONSTRAINT_IK_TIP) == 0)
					parchan = pchan->parent;
				else
					parchan = pchan;
				
				gpuVertex3fv(parchan->pose_tail);
				
				/* Find the chain's root */
				while (parchan->parent) {
					segcount++;
					if (segcount == data->rootbone || segcount > 255) {
						break;  /* 255 is weak */
					}
					parchan = parchan->parent;
				}
				if (parchan)
					gpuVertex3fv(parchan->pose_head);
				
				gpuEnd();
				setlinestyle(0);
			}
			break;
			case CONSTRAINT_TYPE_SPLINEIK: 
			{
				bSplineIKConstraint *data = (bSplineIKConstraint *)con->data;
				int segcount = 0;
				
				setlinestyle(3);
				gpuBegin(GL_LINES);
				
				parchan = pchan;
				gpuVertex3fv(parchan->pose_tail);
				
				/* Find the chain's root */
				while (parchan->parent) {
					segcount++;
					/* FIXME: revise the breaking conditions */
					if (segcount == data->chainlen || segcount > 255) break;  /* 255 is weak */
					parchan = parchan->parent;
				}
				if (parchan)  /* XXX revise the breaking conditions to only stop at the tail? */
					gpuVertex3fv(parchan->pose_head);

				gpuEnd();
				setlinestyle(0);
			}
			break;
		}
	}
}

static void bgl_sphere_project(float ax, float az)
{
	float dir[3], sine, q3;

	sine = 1.0f - ax * ax - az * az;
	q3 = (sine < 0.0f) ? 0.0f : (float)(2.0 * sqrt(sine));

	dir[0] = -az * q3;
	dir[1] = 1.0f - 2.0f * sine;
	dir[2] = ax * q3;

	gpuVertex3fv(dir);
}

static void draw_dof_ellipse(float ax, float az)
{
	static float staticSine[16] = {
		0.0f, 0.104528463268f, 0.207911690818f, 0.309016994375f,
		0.406736643076f, 0.5f, 0.587785252292f, 0.669130606359f,
		0.743144825477f, 0.809016994375f, 0.866025403784f,
		0.913545457643f, 0.951056516295f, 0.978147600734f,
		0.994521895368f, 1.0f
	};

	int i, j, n = 16;
	float x, z, px, pz;

	glEnable(GL_BLEND);
	gpuDepthMask(GL_FALSE);

	gpuCurrentGray4f(0.276f, 0.196f);

	gpuBegin(GL_QUADS);
	pz = 0.0f;
	for (i = 1; i < n; i++) {
		z = staticSine[i];
		
		px = 0.0f;
		for (j = 1; j < n - i + 1; j++) {
			x = staticSine[j];
			
			if (j == n - i) {
				gpuEnd();
				gpuBegin(GL_TRIANGLES);
				bgl_sphere_project(ax * px, az * z);
				bgl_sphere_project(ax * px, az * pz);
				bgl_sphere_project(ax * x, az * pz);
				gpuEnd();
				gpuBegin(GL_QUADS);
			}
			else {
				bgl_sphere_project(ax * x, az * z);
				bgl_sphere_project(ax * x, az * pz);
				bgl_sphere_project(ax * px, az * pz);
				bgl_sphere_project(ax * px, az * z);
			}
			
			px = x;
		}
		pz = z;
	}
	gpuEnd();

	glDisable(GL_BLEND);
	gpuDepthMask(GL_TRUE);

	gpuCurrentColor3x(CPACK_BLACK);

	gpuBegin(GL_LINE_STRIP);
	for (i = 0; i < n; i++)
		bgl_sphere_project(staticSine[n - i - 1] * ax, staticSine[i] * az);
	gpuEnd();
}

static void draw_pose_dofs(Object *ob)
{
	bArmature *arm = ob->data;
	bPoseChannel *pchan;
	Bone *bone;
	
	for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
		bone = pchan->bone;
		
		if ((bone != NULL) && !(bone->flag & (BONE_HIDDEN_P | BONE_HIDDEN_PG))) {
			if (bone->flag & BONE_SELECTED) {
				if (bone->layer & arm->layer) {
					if (pchan->ikflag & (BONE_IK_XLIMIT | BONE_IK_ZLIMIT)) {
						if (ED_pose_channel_in_IK_chain(ob, pchan)) {
							float corner[4][3], posetrans[3], mat[4][4];
							float phi = 0.0f, theta = 0.0f, scale;
							int a, i;
							
							/* in parent-bone pose, but own restspace */
							gpuPushMatrix();
							
							copy_v3_v3(posetrans, pchan->pose_mat[3]);
							gpuTranslate(posetrans[0], posetrans[1], posetrans[2]);
							
							if (pchan->parent) {
								copy_m4_m4(mat, pchan->parent->pose_mat);
								mat[3][0] = mat[3][1] = mat[3][2] = 0.0f;
								gpuMultMatrix(mat);
							}
							
							copy_m4_m3(mat, pchan->bone->bone_mat);
							gpuMultMatrix(mat);
							
							scale = bone->length * pchan->size[1];
							gpuScale(scale, scale, scale);
							
							if (pchan->ikflag & BONE_IK_XLIMIT) {
								if (pchan->ikflag & BONE_IK_ZLIMIT) {
									float amin[3], amax[3];
									
									for (i = 0; i < 3; i++) {
										/* *0.5f here comes from M_PI/360.0f when rotations were still in degrees */
										amin[i] = sinf(pchan->limitmin[i] * 0.5f);
										amax[i] = sinf(pchan->limitmax[i] * 0.5f);
									}
									
									gpuScale(1.0f, -1.0f, 1.0f);
									if ((amin[0] != 0.0f) && (amin[2] != 0.0f))
										draw_dof_ellipse(amin[0], amin[2]);
									if ((amin[0] != 0.0f) && (amax[2] != 0.0f))
										draw_dof_ellipse(amin[0], amax[2]);
									if ((amax[0] != 0.0f) && (amin[2] != 0.0f))
										draw_dof_ellipse(amax[0], amin[2]);
									if ((amax[0] != 0.0f) && (amax[2] != 0.0f))
										draw_dof_ellipse(amax[0], amax[2]);
									gpuScale(1.0f, -1.0f, 1.0f);
								}
							}
							
							/* arcs */
							if (pchan->ikflag & BONE_IK_ZLIMIT) {
								/* OpenGL requires rotations in degrees; so we're taking the average angle here */
								theta = 0.5f * (pchan->limitmin[2] + pchan->limitmax[2]);
								gpuRotateAxis(theta, 'Z');
								
								gpuCurrentColor3ub(50, 50, 255);    /* blue, Z axis limit */
								gpuBegin(GL_LINE_STRIP);
								for (a = -16; a <= 16; a++) {
									/* *0.5f here comes from M_PI/360.0f when rotations were still in degrees */
									float fac = ((float)a) / 16.0f * 0.5f;
									
									phi = fac * (pchan->limitmax[2] - pchan->limitmin[2]);
									
									i = (a == -16) ? 0 : 1;
									corner[i][0] = sinf(phi);
									corner[i][1] = cosf(phi);
									corner[i][2] = 0.0f;
									gpuVertex3fv(corner[i]);
								}
								gpuEnd();
								
								gpuRotateAxis(-theta, 'Z');
							}
							
							if (pchan->ikflag & BONE_IK_XLIMIT) {
								/* OpenGL requires rotations in degrees; so we're taking the average angle here */
								theta = 0.5f * (pchan->limitmin[0] + pchan->limitmax[0]);
								gpuRotateAxis(theta, 'X');
								
								gpuCurrentColor3ub(255, 50, 50);    /* Red, X axis limit */
								gpuBegin(GL_LINE_STRIP);
								for (a = -16; a <= 16; a++) {
									/* *0.5f here comes from M_PI/360.0f when rotations were still in degrees */
									float fac = ((float)a) / 16.0f * 0.5f;
									phi = (float)(0.5 * M_PI) + fac * (pchan->limitmax[0] - pchan->limitmin[0]);
									
									i = (a == -16) ? 2 : 3;
									corner[i][0] = 0.0f;
									corner[i][1] = sinf(phi);
									corner[i][2] = cosf(phi);
									gpuVertex3fv(corner[i]);
								}
								gpuEnd();
								
								gpuRotateAxis(-theta, 'X');
							}
							
							/* out of cone, out of bone */
							gpuPopMatrix(); 
						}
					}
				}
			}
		}
	}
}

static void bone_matrix_translate_y(float mat[4][4], float y)
{
	float trans[3];

	copy_v3_v3(trans, mat[1]);
	mul_v3_fl(trans, y);
	add_v3_v3(mat[3], trans);
}

/* assumes object is Armature with pose */
static void draw_pose_bones(Scene *scene, View3D *v3d, ARegion *ar, Base *base,
                            const short dt, const unsigned char ob_wire_col[4],
                            const bool do_const_color, const bool is_outline)
{
	RegionView3D *rv3d = ar->regiondata;
	Object *ob = base->object;
	bArmature *arm = ob->data;
	bPoseChannel *pchan;
	Bone *bone;
	GLfloat tmp;
	float smat[4][4], imat[4][4], bmat[4][4];
	int index = -1;
	short do_dashed = 3;
	bool draw_wire = false;
	int flag;
	
	/* being set below */
	arm->layer_used = 0;
	
	/* hacky... prevent outline select from drawing dashed helplines */
	tmp = gpuGetLineWidth();
	if (tmp > 1.1f) do_dashed &= ~1;
	if (v3d->flag & V3D_HIDE_HELPLINES) do_dashed &= ~2;
	
	/* precalc inverse matrix for drawing screen aligned */
	if (arm->drawtype == ARM_ENVELOPE) {
		/* precalc inverse matrix for drawing screen aligned */
		copy_m4_m4(smat, rv3d->viewmatob);
		mul_mat3_m4_fl(smat, 1.0f / len_v3(ob->obmat[0]));
		invert_m4_m4(imat, smat);
		
		/* and draw blended distances */
		if (arm->flag & ARM_POSEMODE) {
			glEnable(GL_BLEND);
			//gpuShadeModel(GL_SMOOTH);
			
			if (v3d->zbuf) glDisable(GL_DEPTH_TEST);
			
			for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
				bone = pchan->bone;
				if (bone) {
					/* 1) bone must be visible, 2) for OpenGL select-drawing cannot have unselectable [#27194] 
					 * NOTE: this is the only case with (NO_DEFORM == 0) flag, as this is for envelope influence drawing
					 */
					if (((bone->flag & (BONE_HIDDEN_P | BONE_NO_DEFORM | BONE_HIDDEN_PG)) == 0) &&
					    ((G.f & G_PICKSEL) == 0 || (bone->flag & BONE_UNSELECTABLE) == 0))
					{
						if (bone->flag & (BONE_SELECTED)) {
							if (bone->layer & arm->layer)
								draw_sphere_bone_dist(smat, imat, pchan, NULL);
						}
					}
				}
			}
			
			if (v3d->zbuf) glEnable(GL_DEPTH_TEST);
			glDisable(GL_BLEND);
			//gpuShadeModel(GL_FLAT);
		}
	}
	
	/* little speedup, also make sure transparent only draws once */
	glCullFace(GL_BACK); 
	glEnable(GL_CULL_FACE);
	
	/* if solid we draw that first, with selection codes, but without names, axes etc */
	if (dt > OB_WIRE) {
		if (arm->flag & ARM_POSEMODE) 
			index = base->selcol;
		
		for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
			bone = pchan->bone;
			arm->layer_used |= bone->layer;
			
			/* 1) bone must be visible, 2) for OpenGL select-drawing cannot have unselectable [#27194] */
			if (((bone->flag & (BONE_HIDDEN_P | BONE_HIDDEN_PG)) == 0) &&
			    ((G.f & G_PICKSEL) == 0 || (bone->flag & BONE_UNSELECTABLE) == 0))
			{
				if (bone->layer & arm->layer) {
					const bool use_custom = (pchan->custom) && !(arm->flag & ARM_NO_CUSTOM);
					gpuPushMatrix();
					
					if (use_custom && pchan->custom_tx) {
						gpuMultMatrix(pchan->custom_tx->pose_mat);
					}
					else {
						gpuMultMatrix(pchan->pose_mat);
					}
					
					/* catch exception for bone with hidden parent */
					flag = bone->flag;
					if ((bone->parent) && (bone->parent->flag & (BONE_HIDDEN_P | BONE_HIDDEN_PG))) {
						flag &= ~BONE_CONNECTED;
					}
					
					/* set temporary flag for drawing bone as active, but only if selected */
					if (bone == arm->act_bone)
						flag |= BONE_DRAW_ACTIVE;
					
					if (do_const_color) {
						/* keep color */
					}
					else {
						/* set color-set to use */
						set_pchan_colorset(ob, pchan);
					}
					
					if (use_custom) {
						/* if drawwire, don't try to draw in solid */
						if (pchan->bone->flag & BONE_DRAWWIRE) {
							draw_wire = true;
						}
						else {
							draw_custom_bone(scene, v3d, rv3d, pchan->custom,
							                 OB_SOLID, arm->flag, flag, index, bone->length);
						}
					}
					else if (arm->drawtype == ARM_LINE) {
						/* nothing in solid */
					}
					else if (arm->drawtype == ARM_WIRE) {
						/* nothing in solid */
					}
					else if (arm->drawtype == ARM_ENVELOPE) {
						draw_sphere_bone(OB_SOLID, arm->flag, flag, 0, index, pchan, NULL);
					}
					else if (arm->drawtype == ARM_B_BONE) {
						draw_b_bone(OB_SOLID, arm->flag, flag, 0, index, pchan, NULL);
					}
					else {
						draw_bone(OB_SOLID, arm->flag, flag, 0, index, bone->length);
					}
						
					gpuPopMatrix();
				}
			}
			
			if (index != -1)
				index += 0x10000;  /* pose bones count in higher 2 bytes only */
		}
		
		/* very very confusing... but in object mode, solid draw, we cannot do gpuSelectLoad yet,
		 * stick bones and/or wire custom-shapes are drawn in next loop 
		 */
		if (ELEM(arm->drawtype, ARM_LINE, ARM_WIRE) == 0 && (draw_wire == false)) {
			/* object tag, for bordersel optim */
			gpuSelectLoad(index & 0xFFFF);
			index = -1;
		}
	}
	
	/* draw custom bone shapes as wireframes */
	if (!(arm->flag & ARM_NO_CUSTOM) &&
	    (draw_wire || (dt <= OB_WIRE)) )
	{
		if (arm->flag & ARM_POSEMODE)
			index = base->selcol;
			
		/* only draw custom bone shapes that need to be drawn as wires */
		for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
			bone = pchan->bone;
			
			/* 1) bone must be visible, 2) for OpenGL select-drawing cannot have unselectable [#27194] */
			if (((bone->flag & (BONE_HIDDEN_P | BONE_HIDDEN_PG)) == 0) &&
			    ((G.f & G_PICKSEL) == 0 || (bone->flag & BONE_UNSELECTABLE) == 0) )
			{
				if (bone->layer & arm->layer) {
					if (pchan->custom) {
						if ((dt < OB_SOLID) || (bone->flag & BONE_DRAWWIRE)) {
							gpuPushMatrix();
							
							if (pchan->custom_tx) {
								gpuMultMatrix(pchan->custom_tx->pose_mat);
							}
							else {
								gpuMultMatrix(pchan->pose_mat);
							}
							
							/* prepare colors */
							if (do_const_color) {
								/* 13 October 2009, Disabled this to make ghosting show the right colors (Aligorith) */
							}
							else if (arm->flag & ARM_POSEMODE)
								set_pchan_colorset(ob, pchan);
							else {
								gpuCurrentColor3ubv(ob_wire_col);
							}
								
							/* catch exception for bone with hidden parent */
							flag = bone->flag;
							if ((bone->parent) && (bone->parent->flag & (BONE_HIDDEN_P | BONE_HIDDEN_PG)))
								flag &= ~BONE_CONNECTED;
								
							/* set temporary flag for drawing bone as active, but only if selected */
							if (bone == arm->act_bone)
								flag |= BONE_DRAW_ACTIVE;
							
							draw_custom_bone(scene, v3d, rv3d, pchan->custom,
							                 OB_WIRE, arm->flag, flag, index, bone->length);
							
							gpuPopMatrix();
						}
					}
				}
			}
			
			if (index != -1) 
				index += 0x10000;  /* pose bones count in higher 2 bytes only */
		}
		/* stick or wire bones have not been drawn yet so don't clear object selection in this case */
		if (ELEM(arm->drawtype, ARM_LINE, ARM_WIRE) == 0 && draw_wire) {
			/* object tag, for bordersel optim */
			gpuSelectLoad(index & 0xFFFF);
			index = -1;
		}
	}
	
	/* wire draw over solid only in posemode */
	if ((dt <= OB_WIRE) || (arm->flag & ARM_POSEMODE) || ELEM(arm->drawtype, ARM_LINE, ARM_WIRE)) {
		/* draw line check first. we do selection indices */
		if (ELEM(arm->drawtype, ARM_LINE, ARM_WIRE)) {
			if (arm->flag & ARM_POSEMODE) 
				index = base->selcol;
		}
		/* if solid && posemode, we draw again with polygonoffset */
		else if ((dt > OB_WIRE) && (arm->flag & ARM_POSEMODE)) {
			bglPolygonOffset(rv3d->dist, 1.0);
		}
		else {
			/* and we use selection indices if not done yet */
			if (arm->flag & ARM_POSEMODE) 
				index = base->selcol;
		}
		
		for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
			bone = pchan->bone;
			arm->layer_used |= bone->layer;
			
			/* 1) bone must be visible, 2) for OpenGL select-drawing cannot have unselectable [#27194] */
			if (((bone->flag & (BONE_HIDDEN_P | BONE_HIDDEN_PG)) == 0) &&
			    ((G.f & G_PICKSEL) == 0 || (bone->flag & BONE_UNSELECTABLE) == 0))
			{
				if (bone->layer & arm->layer) {
					const short constflag = pchan->constflag;
					if ((do_dashed & 1) && (pchan->parent)) {
						/* Draw a line from our root to the parent's tip 
						 * - only if V3D_HIDE_HELPLINES is enabled...
						 */
						if ((do_dashed & 2) && ((bone->flag & BONE_CONNECTED) == 0)) {
							if (arm->flag & ARM_POSEMODE) {
								gpuSelectLoad(index & 0xFFFF);  /* object tag, for bordersel optim */
								UI_ThemeColor(TH_WIRE);
							}
							setlinestyle(3);
							gpuImmediateFormat_V3();
							gpuBegin(GL_LINES);
							gpuVertex3fv(pchan->pose_head);
							gpuVertex3fv(pchan->parent->pose_tail);
							gpuEnd();
							gpuImmediateUnformat();
							setlinestyle(0);
						}
						
						/* Draw a line to IK root bone 
						 *  - only if temporary chain (i.e. "autoik")
						 */
						if (arm->flag & ARM_POSEMODE) {
							if (constflag & PCHAN_HAS_IK) {
								if (bone->flag & BONE_SELECTED) {
									if (constflag & PCHAN_HAS_TARGET) gpuCurrentColor3ub(200, 120, 0);
									else gpuCurrentColor3ub(200, 200, 50);  /* add theme! */

									gpuSelectLoad(index & 0xFFFF);
									pchan_draw_IK_root_lines(pchan, !(do_dashed & 2));
								}
							}
							else if (constflag & PCHAN_HAS_SPLINEIK) {
								if (bone->flag & BONE_SELECTED) {
									gpuCurrentColor3ub(150, 200, 50);   /* add theme! */
									
									gpuSelectLoad(index & 0xFFFF);
									pchan_draw_IK_root_lines(pchan, !(do_dashed & 2));
								}
							}
						}
					}
					
					gpuPushMatrix();
					if (arm->drawtype != ARM_ENVELOPE)
						gpuMultMatrix(pchan->pose_mat);
					
					/* catch exception for bone with hidden parent */
					flag = bone->flag;
					if ((bone->parent) && (bone->parent->flag & (BONE_HIDDEN_P | BONE_HIDDEN_PG)))
						flag &= ~BONE_CONNECTED;
					
					/* set temporary flag for drawing bone as active, but only if selected */
					if (bone == arm->act_bone)
						flag |= BONE_DRAW_ACTIVE;
					
					/* extra draw service for pose mode */

					/* set color-set to use */
					if (do_const_color) {
						/* keep color */
					}
					else {
						set_pchan_colorset(ob, pchan);
					}
					
					if ((pchan->custom) && !(arm->flag & ARM_NO_CUSTOM)) {
						/* custom bone shapes should not be drawn here! */
					}
					else if (arm->drawtype == ARM_ENVELOPE) {
						if (dt < OB_SOLID)
							draw_sphere_bone_wire(smat, imat, arm->flag, flag, constflag, index, pchan, NULL);
					}
					else if (arm->drawtype == ARM_LINE)
						draw_line_bone(arm->flag, flag, constflag, index, pchan, NULL);
					else if (arm->drawtype == ARM_WIRE)
						draw_wire_bone(dt, arm->flag, flag, constflag, index, pchan, NULL);
					else if (arm->drawtype == ARM_B_BONE)
						draw_b_bone(OB_WIRE, arm->flag, flag, constflag, index, pchan, NULL);
					else
						draw_bone(OB_WIRE, arm->flag, flag, constflag, index, bone->length);
					
					gpuPopMatrix();
				}
			}
			
			/* pose bones count in higher 2 bytes only */
			if (index != -1) 
				index += 0x10000;
		}
		/* restore things */
		if (!ELEM(arm->drawtype, ARM_WIRE, ARM_LINE) && (dt > OB_WIRE) && (arm->flag & ARM_POSEMODE))
			bglPolygonOffset(rv3d->dist, 0.0);
	}
	
	/* restore */
	glDisable(GL_CULL_FACE);
	
	/* draw DoFs */
	if (arm->flag & ARM_POSEMODE) {
		if (((base->flag & OB_FROMDUPLI) == 0)) {
			draw_pose_dofs(ob);
		}
	}

	/* finally names and axes */
	if ((arm->flag & (ARM_DRAWNAMES | ARM_DRAWAXES)) &&
	    (is_outline == 0) &&
	    ((base->flag & OB_FROMDUPLI) == 0))
	{
		/* patch for several 3d cards (IBM mostly) that crash on GL_SELECT with text drawing */
		if ((G.f & G_PICKSEL) == 0) {
			float vec[3];
			unsigned char col[4];
			if (do_const_color) {
				/* so we can draw bone names in current const color */
				gpuGetCurrentColor4ubv(col);
				col[3] = 255;
			}
			else {
				col[0] = ob_wire_col[0];
				col[1] = ob_wire_col[1];
				col[2] = ob_wire_col[2];
				col[3] = 255;
			}

			if (v3d->zbuf) {
				glDisable(GL_DEPTH_TEST);
			}

			for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
				if ((pchan->bone->flag & (BONE_HIDDEN_P | BONE_HIDDEN_PG)) == 0 &&
					 pchan->bone->layer & arm->layer)
				{
					if (arm->flag & (ARM_EDITMODE | ARM_POSEMODE)) {
						bone = pchan->bone;
						UI_GetThemeColor3ubv((bone->flag & BONE_SELECTED) ? TH_TEXT_HI : TH_TEXT, col);
					}
					else if (dt > OB_WIRE) {
						UI_GetThemeColor3ubv(TH_TEXT, col);
					}

					/*  Draw names of bone  */
					if (arm->flag & ARM_DRAWNAMES) {
						mid_v3_v3v3(vec, pchan->pose_head, pchan->pose_tail);
						view3d_cached_text_draw_add(vec, pchan->name, 10, 0, col);
					}

					/*	Draw additional axes on the bone tail  */
					if ((arm->flag & ARM_DRAWAXES) && (arm->flag & ARM_POSEMODE)) {
						gpuPushMatrix();
						copy_m4_m4(bmat, pchan->pose_mat);
						bone_matrix_translate_y(bmat, pchan->bone->length);
						gpuMultMatrix(bmat);

						gpuCurrentColor3ubv(col);
						drawaxes(pchan->bone->length * 0.25f, OB_ARROWS);

						gpuPopMatrix();
					}
				}
			}

			if (v3d->zbuf) {
				glEnable(GL_DEPTH_TEST);
			}
		}
	}
}

/* in editmode, we don't store the bone matrix... */
static void get_matrix_editbone(EditBone *eBone, float bmat[4][4])
{
	float delta[3];
	float mat[3][3];
	
	/* Compose the parent transforms (i.e. their translations) */
	sub_v3_v3v3(delta, eBone->tail, eBone->head);
	
	eBone->length = (float)sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
	
	vec_roll_to_mat3(delta, eBone->roll, mat);
	copy_m4_m3(bmat, mat);

	add_v3_v3(bmat[3], eBone->head);
}

static void draw_ebones(View3D *v3d, ARegion *ar, Object *ob, const short dt)
{
	RegionView3D *rv3d = ar->regiondata;
	EditBone *eBone;
	bArmature *arm = ob->data;
	float smat[4][4], imat[4][4], bmat[4][4];
	unsigned int index;
	int flag;
	
	/* being set in code below */
	arm->layer_used = 0;

	ED_view3d_check_mats_rv3d(rv3d);

	/* envelope (deform distance) */
	if (arm->drawtype == ARM_ENVELOPE) {
		/* precalc inverse matrix for drawing screen aligned */
		copy_m4_m4(smat, rv3d->viewmatob);
		mul_mat3_m4_fl(smat, 1.0f / len_v3(ob->obmat[0]));
		invert_m4_m4(imat, smat);
		
		/* and draw blended distances */
		glEnable(GL_BLEND);
		//gpuShadeModel(GL_SMOOTH);
		
		if (v3d->zbuf) glDisable(GL_DEPTH_TEST);

		for (eBone = arm->edbo->first; eBone; eBone = eBone->next) {
			if (eBone->layer & arm->layer) {
				if ((eBone->flag & (BONE_HIDDEN_A | BONE_NO_DEFORM)) == 0) {
					if (eBone->flag & (BONE_SELECTED | BONE_TIPSEL | BONE_ROOTSEL))
						draw_sphere_bone_dist(smat, imat, NULL, eBone);
				}
			}
		}
		
		if (v3d->zbuf) glEnable(GL_DEPTH_TEST);
		glDisable(GL_BLEND);
		//gpuShadeModel(GL_FLAT);
	}
	
	/* if solid we draw it first */
	if ((dt > OB_WIRE) && (arm->drawtype != ARM_LINE)) {
		for (eBone = arm->edbo->first, index = 0; eBone; eBone = eBone->next, index++) {
			if (eBone->layer & arm->layer) {
				if ((eBone->flag & BONE_HIDDEN_A) == 0) {
					gpuPushMatrix();
					get_matrix_editbone(eBone, bmat);
					gpuMultMatrix(bmat);
					
					/* catch exception for bone with hidden parent */
					flag = eBone->flag;
					if ((eBone->parent) && !EBONE_VISIBLE(arm, eBone->parent)) {
						flag &= ~BONE_CONNECTED;
					}
						
					/* set temporary flag for drawing bone as active, but only if selected */
					if (eBone == arm->act_edbone)
						flag |= BONE_DRAW_ACTIVE;
					
					if (arm->drawtype == ARM_ENVELOPE)
						draw_sphere_bone(OB_SOLID, arm->flag, flag, 0, index, NULL, eBone);
					else if (arm->drawtype == ARM_B_BONE)
						draw_b_bone(OB_SOLID, arm->flag, flag, 0, index, NULL, eBone);
					else if (arm->drawtype == ARM_WIRE)
						draw_wire_bone(OB_SOLID, arm->flag, flag, 0, index, NULL, eBone);
					else {
						draw_bone(OB_SOLID, arm->flag, flag, 0, index, eBone->length);
					}
					
					gpuPopMatrix();
				}
			}
		}
	}
	
	/* if wire over solid, set offset */
	index = -1;
	gpuSelectLoad(-1);
	if (ELEM(arm->drawtype, ARM_LINE, ARM_WIRE)) {
		if (G.f & G_PICKSEL)
			index = 0;
	}
	else if (dt > OB_WIRE) 
		bglPolygonOffset(rv3d->dist, 1.0f);
	else if (arm->flag & ARM_EDITMODE) 
		index = 0;  /* do selection codes */
	
	for (eBone = arm->edbo->first; eBone; eBone = eBone->next) {
		arm->layer_used |= eBone->layer;
		if (eBone->layer & arm->layer) {
			if ((eBone->flag & BONE_HIDDEN_A) == 0) {
				
				/* catch exception for bone with hidden parent */
				flag = eBone->flag;
				if ((eBone->parent) && !EBONE_VISIBLE(arm, eBone->parent)) {
					flag &= ~BONE_CONNECTED;
				}
					
				/* set temporary flag for drawing bone as active, but only if selected */
				if (eBone == arm->act_edbone)
					flag |= BONE_DRAW_ACTIVE;
				
				if (arm->drawtype == ARM_ENVELOPE) {
					if (dt < OB_SOLID)
						draw_sphere_bone_wire(smat, imat, arm->flag, flag, 0, index, NULL, eBone);
				}
				else {
					gpuPushMatrix();
					get_matrix_editbone(eBone, bmat);
					gpuMultMatrix(bmat);
					
					if (arm->drawtype == ARM_LINE) 
						draw_line_bone(arm->flag, flag, 0, index, NULL, eBone);
					else if (arm->drawtype == ARM_WIRE)
						draw_wire_bone(OB_WIRE, arm->flag, flag, 0, index, NULL, eBone);
					else if (arm->drawtype == ARM_B_BONE)
						draw_b_bone(OB_WIRE, arm->flag, flag, 0, index, NULL, eBone);
					else
						draw_bone(OB_WIRE, arm->flag, flag, 0, index, eBone->length);
					
					gpuPopMatrix();
				}
				
				/* offset to parent */
				if (eBone->parent) {
					UI_ThemeColor(TH_WIRE_EDIT);
					gpuSelectLoad(-1);  /* -1 here is OK! */
					setlinestyle(3);
					
					gpuBegin(GL_LINES);
					gpuVertex3fv(eBone->parent->tail);
					gpuVertex3fv(eBone->head);
					gpuEnd();
					
					setlinestyle(0);
				}
			}
		}
		if (index != -1) index++;
	}
	
	/* restore */
	if (index != -1) {
		gpuSelectLoad(-1);
	}

	if (ELEM(arm->drawtype, ARM_LINE, ARM_WIRE)) {
		/* pass */
	}
	else if (dt > OB_WIRE) {
		bglPolygonOffset(rv3d->dist, 0.0f);
	}
	
	/* finally names and axes */
	if (arm->flag & (ARM_DRAWNAMES | ARM_DRAWAXES)) {
		/* patch for several 3d cards (IBM mostly) that crash on GL_SELECT with text drawing */
		if ((G.f & G_PICKSEL) == 0) {
			float vec[3];
			unsigned char col[4];
			col[3] = 255;
			
			if (v3d->zbuf) glDisable(GL_DEPTH_TEST);
			
			for (eBone = arm->edbo->first; eBone; eBone = eBone->next) {
				if (eBone->layer & arm->layer) {
					if ((eBone->flag & BONE_HIDDEN_A) == 0) {

						UI_GetThemeColor3ubv((eBone->flag & BONE_SELECTED) ? TH_TEXT_HI : TH_TEXT, col);

						/*	Draw name */
						if (arm->flag & ARM_DRAWNAMES) {
							mid_v3_v3v3(vec, eBone->head, eBone->tail);
							//glRasterPos3fv(vec); // XXX jwilkins: is this needed?  the next function doesn't seem to actually draw anything anymore
							view3d_cached_text_draw_add(vec, eBone->name, 10, 0, col);
						}
						/*	Draw additional axes */
						if (arm->flag & ARM_DRAWAXES) {
							gpuPushMatrix();
							get_matrix_editbone(eBone, bmat);
							bone_matrix_translate_y(bmat, eBone->length);
							gpuMultMatrix(bmat);

							gpuCurrentColor3ubv(col);
							drawaxes(eBone->length * 0.25f, OB_ARROWS);
							
							gpuPopMatrix();
						}
						
					}
				}
			}
			
			if (v3d->zbuf) glEnable(GL_DEPTH_TEST);
		}
	}
}

/* ****************************** Armature Visualization ******************************** */

/* ---------- Paths --------- */

/* draw bone paths
 *	- in view space 
 */
static void draw_pose_paths(Scene *scene, View3D *v3d, ARegion *ar, Object *ob)
{
	bAnimVizSettings *avs = &ob->pose->avs;
	bArmature *arm = ob->data;
	bPoseChannel *pchan;
	
	/* setup drawing environment for paths */
	draw_motion_paths_init(v3d, ar);
	
	/* draw paths where they exist and they releated bone is visible */
	for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
		if ((pchan->bone->layer & arm->layer) && (pchan->mpath))
			draw_motion_path_instance(scene, ob, pchan, avs, pchan->mpath);
	}
	
	/* cleanup after drawing */
	draw_motion_paths_cleanup(v3d);
}


/* ---------- Ghosts --------- */

/* helper function for ghost drawing - sets/removes flags for temporarily 
 * hiding unselected bones while drawing ghosts
 */
static void ghost_poses_tag_unselected(Object *ob, short unset)
{
	bArmature *arm = ob->data;
	bPose *pose = ob->pose;
	bPoseChannel *pchan;
	
	/* don't do anything if no hiding any bones */
	if ((arm->flag & ARM_GHOST_ONLYSEL) == 0)
		return;
		
	/* loop over all pchans, adding/removing tags as appropriate */
	for (pchan = pose->chanbase.first; pchan; pchan = pchan->next) {
		if ((pchan->bone) && (arm->layer & pchan->bone->layer)) {
			if (unset) {
				/* remove tags from all pchans if cleaning up */
				pchan->bone->flag &= ~BONE_HIDDEN_PG;
			}
			else {
				/* set tags on unselected pchans only */
				if ((pchan->bone->flag & BONE_SELECTED) == 0)
					pchan->bone->flag |= BONE_HIDDEN_PG;
			}
		}
	}
}

/* draw ghosts that occur within a frame range 
 *  note: object should be in posemode
 */
static void draw_ghost_poses_range(Scene *scene, View3D *v3d, ARegion *ar, Base *base)
{
	Object *ob = base->object;
	AnimData *adt = BKE_animdata_from_id(&ob->id);
	bArmature *arm = ob->data;
	bPose *posen, *poseo;
	float start, end, stepsize, range, colfac;
	int cfrao, flago, ipoflago;
	
	start = (float)arm->ghostsf;
	end = (float)arm->ghostef;
	if (end <= start)
		return;
	
	stepsize = (float)(arm->ghostsize);
	range = (float)(end - start);
	
	/* store values */
	ob->mode &= ~OB_MODE_POSE;
	cfrao = CFRA;
	flago = arm->flag;
	arm->flag &= ~(ARM_DRAWNAMES | ARM_DRAWAXES);
	ipoflago = ob->ipoflag;
	ob->ipoflag |= OB_DISABLE_PATH;
	
	/* copy the pose */
	poseo = ob->pose;
	BKE_pose_copy_data(&posen, ob->pose, 1);
	ob->pose = posen;
	BKE_pose_rebuild(ob, ob->data);    /* child pointers for IK */
	ghost_poses_tag_unselected(ob, 0);      /* hide unselected bones if need be */
	
	glEnable(GL_BLEND);
	if (v3d->zbuf) glDisable(GL_DEPTH_TEST);
	
	/* draw from first frame of range to last */
	for (CFRA = (int)start; CFRA < end; CFRA += (int)stepsize) {
		colfac = (end - (float)CFRA) / range;
		UI_ThemeColorShadeAlpha(TH_WIRE, 0, -128 - (int)(120.0 * sqrt(colfac)));
		
		BKE_animsys_evaluate_animdata(scene, &ob->id, adt, (float)CFRA, ADT_RECALC_ALL);
		BKE_pose_where_is(scene, ob);
		draw_pose_bones(scene, v3d, ar, base, OB_WIRE, NULL, true, false);
	}
	glDisable(GL_BLEND);
	if (v3d->zbuf) glEnable(GL_DEPTH_TEST);
	
	/* before disposing of temp pose, use it to restore object to a sane state */
	BKE_animsys_evaluate_animdata(scene, &ob->id, adt, (float)cfrao, ADT_RECALC_ALL);
	
	/* clean up temporary pose */
	ghost_poses_tag_unselected(ob, 1);      /* unhide unselected bones if need be */
	BKE_pose_free(posen);
	
	/* restore */
	CFRA = cfrao;
	ob->pose = poseo;
	arm->flag = flago;
	ob->mode |= OB_MODE_POSE;
	ob->ipoflag = ipoflago;
}

/* draw ghosts on keyframes in action within range 
 *	- object should be in posemode 
 */
static void draw_ghost_poses_keys(Scene *scene, View3D *v3d, ARegion *ar, Base *base)
{
	Object *ob = base->object;
	AnimData *adt = BKE_animdata_from_id(&ob->id);
	bAction *act = (adt) ? adt->action : NULL;
	bArmature *arm = ob->data;
	bPose *posen, *poseo;
	DLRBT_Tree keys;
	ActKeyColumn *ak, *akn;
	float start, end, range, colfac, i;
	int cfrao, flago;
	
	start = (float)arm->ghostsf;
	end = (float)arm->ghostef;
	if (end <= start)
		return;
	
	/* get keyframes - then clip to only within range */
	BLI_dlrbTree_init(&keys);
	action_to_keylist(adt, act, &keys, NULL);
	BLI_dlrbTree_linkedlist_sync(&keys);
	
	range = 0;
	for (ak = keys.first; ak; ak = akn) {
		akn = ak->next;
		
		if ((ak->cfra < start) || (ak->cfra > end))
			BLI_freelinkN((ListBase *)&keys, ak);
		else
			range++;
	}
	if (range == 0) return;
	
	/* store values */
	ob->mode &= ~OB_MODE_POSE;
	cfrao = CFRA;
	flago = arm->flag;
	arm->flag &= ~(ARM_DRAWNAMES | ARM_DRAWAXES);
	ob->ipoflag |= OB_DISABLE_PATH;
	
	/* copy the pose */
	poseo = ob->pose;
	BKE_pose_copy_data(&posen, ob->pose, 1);
	ob->pose = posen;
	BKE_pose_rebuild(ob, ob->data);  /* child pointers for IK */
	ghost_poses_tag_unselected(ob, 0);    /* hide unselected bones if need be */
	
	glEnable(GL_BLEND);
	if (v3d->zbuf) glDisable(GL_DEPTH_TEST);
	
	/* draw from first frame of range to last */
	for (ak = keys.first, i = 0; ak; ak = ak->next, i++) {
		colfac = i / range;
		UI_ThemeColorShadeAlpha(TH_WIRE, 0, -128 - (int)(120.0 * sqrt(colfac)));
		
		CFRA = (int)ak->cfra;
		
		BKE_animsys_evaluate_animdata(scene, &ob->id, adt, (float)CFRA, ADT_RECALC_ALL);
		BKE_pose_where_is(scene, ob);
		draw_pose_bones(scene, v3d, ar, base, OB_WIRE, NULL, true, false);
	}
	glDisable(GL_BLEND);
	if (v3d->zbuf) glEnable(GL_DEPTH_TEST);
	
	/* before disposing of temp pose, use it to restore object to a sane state */
	BKE_animsys_evaluate_animdata(scene, &ob->id, adt, (float)cfrao, ADT_RECALC_ALL);
	
	/* clean up temporary pose */
	ghost_poses_tag_unselected(ob, 1);  /* unhide unselected bones if need be */
	BLI_dlrbTree_free(&keys);
	BKE_pose_free(posen);
	
	/* restore */
	CFRA = cfrao;
	ob->pose = poseo;
	arm->flag = flago;
	ob->mode |= OB_MODE_POSE;
}

/* draw ghosts around current frame
 *  - object is supposed to be armature in posemode
 */
static void draw_ghost_poses(Scene *scene, View3D *v3d, ARegion *ar, Base *base)
{
	Object *ob = base->object;
	AnimData *adt = BKE_animdata_from_id(&ob->id);
	bArmature *arm = ob->data;
	bPose *posen, *poseo;
	float cur, start, end, stepsize, range, colfac, actframe, ctime;
	int cfrao, flago;
	
	/* pre conditions, get an action with sufficient frames */
	if (ELEM(NULL, adt, adt->action))
		return;

	calc_action_range(adt->action, &start, &end, 0);
	if (start == end)
		return;

	stepsize = (float)(arm->ghostsize);
	range = (float)(arm->ghostep) * stepsize + 0.5f;   /* plus half to make the for loop end correct */
	
	/* store values */
	ob->mode &= ~OB_MODE_POSE;
	cfrao = CFRA;
	actframe = BKE_nla_tweakedit_remap(adt, (float)CFRA, 0);
	flago = arm->flag;
	arm->flag &= ~(ARM_DRAWNAMES | ARM_DRAWAXES);
	
	/* copy the pose */
	poseo = ob->pose;
	BKE_pose_copy_data(&posen, ob->pose, 1);
	ob->pose = posen;
	BKE_pose_rebuild(ob, ob->data);    /* child pointers for IK */
	ghost_poses_tag_unselected(ob, 0);      /* hide unselected bones if need be */
	
	glEnable(GL_BLEND);
	if (v3d->zbuf) glDisable(GL_DEPTH_TEST);
	
	/* draw from darkest blend to lowest */
	for (cur = stepsize; cur < range; cur += stepsize) {
		ctime = cur - (float)fmod(cfrao, stepsize);  /* ensures consistent stepping */
		colfac = ctime / range;
		UI_ThemeColorShadeAlpha(TH_WIRE, 0, -128 - (int)(120.0 * sqrt(colfac)));
		
		/* only within action range */
		if (actframe + ctime >= start && actframe + ctime <= end) {
			CFRA = (int)BKE_nla_tweakedit_remap(adt, actframe + ctime, NLATIME_CONVERT_MAP);
			
			if (CFRA != cfrao) {
				BKE_animsys_evaluate_animdata(scene, &ob->id, adt, (float)CFRA, ADT_RECALC_ALL);
				BKE_pose_where_is(scene, ob);
				draw_pose_bones(scene, v3d, ar, base, OB_WIRE, NULL, true, false);
			}
		}
		
		ctime = cur + (float)fmod((float)cfrao, stepsize) - stepsize + 1.0f;   /* ensures consistent stepping */
		colfac = ctime / range;
		UI_ThemeColorShadeAlpha(TH_WIRE, 0, -128 - (int)(120.0 * sqrt(colfac)));
		
		/* only within action range */
		if ((actframe - ctime >= start) && (actframe - ctime <= end)) {
			CFRA = (int)BKE_nla_tweakedit_remap(adt, actframe - ctime, NLATIME_CONVERT_MAP);
			
			if (CFRA != cfrao) {
				BKE_animsys_evaluate_animdata(scene, &ob->id, adt, (float)CFRA, ADT_RECALC_ALL);
				BKE_pose_where_is(scene, ob);
				draw_pose_bones(scene, v3d, ar, base, OB_WIRE, NULL, true, false);
			}
		}
	}
	glDisable(GL_BLEND);
	if (v3d->zbuf) glEnable(GL_DEPTH_TEST);
	
	/* before disposing of temp pose, use it to restore object to a sane state */
	BKE_animsys_evaluate_animdata(scene, &ob->id, adt, (float)cfrao, ADT_RECALC_ALL);
	
	/* clean up temporary pose */
	ghost_poses_tag_unselected(ob, 1);      /* unhide unselected bones if need be */
	BKE_pose_free(posen);
	
	/* restore */
	CFRA = cfrao;
	ob->pose = poseo;
	arm->flag = flago;
	ob->mode |= OB_MODE_POSE;
}

/* ********************************** Armature Drawing - Main ************************* */

/* called from drawobject.c, return 1 if nothing was drawn
 * (ob_wire_col == NULL) when drawing ghost */
bool draw_armature(Scene *scene, View3D *v3d, ARegion *ar, Base *base,
                   const short dt, const short dflag, const unsigned char ob_wire_col[4],
                   const bool is_outline)
{
	Object *ob = base->object;
	bArmature *arm = ob->data;
	bool retval = false;

	if (v3d->flag2 & V3D_RENDER_OVERRIDE)
		return true;

	if (dt > OB_WIRE && !ELEM(arm->drawtype, ARM_LINE, ARM_WIRE)) {
		static const GLfloat white[4] = { 1, 1, 1, 1 };

		/* we use color for solid lighting */
		gpuMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, white);

		gpuColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);

		glFrontFace((ob->transflag & OB_NEG_SCALE) ? GL_CW : GL_CCW);  /* only for lighting... */
	}
	
	/* arm->flag is being used to detect mode... */
	/* editmode? */
	if (arm->edbo) {
		arm->flag |= ARM_EDITMODE;
		draw_ebones(v3d, ar, ob, dt);
		arm->flag &= ~ARM_EDITMODE;
	}
	else {
		/*	Draw Pose */
		if (ob->pose && ob->pose->chanbase.first) {
			/* drawing posemode selection indices or colors only in these cases */
			if (!(base->flag & OB_FROMDUPLI)) {
				if (G.f & G_PICKSEL) {
#if 0
					/* nifty but actually confusing to allow bone selection out of posemode */
					if (OBACT && (OBACT->mode & OB_MODE_WEIGHT_PAINT)) {
						if (ob == modifiers_isDeformedByArmature(OBACT))
							arm->flag |= ARM_POSEMODE;
					}
					else
#endif
					if (ob->mode & OB_MODE_POSE) {
						arm->flag |= ARM_POSEMODE;
					}
				}
				else if (ob->mode & OB_MODE_POSE) {
					if (arm->ghosttype == ARM_GHOST_RANGE) {
						draw_ghost_poses_range(scene, v3d, ar, base);
					}
					else if (arm->ghosttype == ARM_GHOST_KEYS) {
						draw_ghost_poses_keys(scene, v3d, ar, base);
					}
					else if (arm->ghosttype == ARM_GHOST_CUR) {
						if (arm->ghostep)
							draw_ghost_poses(scene, v3d, ar, base);
					}
					if ((dflag & DRAW_SCENESET) == 0) {
						if (ob == OBACT)
							arm->flag |= ARM_POSEMODE;
						else if (OBACT && (OBACT->mode & OB_MODE_WEIGHT_PAINT)) {
							if (ob == modifiers_isDeformedByArmature(OBACT))
								arm->flag |= ARM_POSEMODE;
						}
						draw_pose_paths(scene, v3d, ar, ob);
					}
				}
			}
			draw_pose_bones(scene, v3d, ar, base, dt, ob_wire_col, (dflag & DRAW_CONSTCOLOR), is_outline);
			arm->flag &= ~ARM_POSEMODE; 
			
			if (ob->mode & OB_MODE_POSE)
				UI_ThemeColor(TH_WIRE);  /* restore, for extra draw stuff */
		}
		else {
			retval = true;
		}
	}
	/* restore */
	glFrontFace(GL_CCW);

	return retval;
}
