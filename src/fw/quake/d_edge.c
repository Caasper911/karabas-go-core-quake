/* karabas: copy of PocketQuake src/firmware/quake/d_edge.c (5792fe2) with D_DrawPoly for KG_POLY (Ф7) */
/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// d_edge.c

#include "quakedef.h"
#include "d_local.h"
#include "libc.h"
#include "scanline_accel.h"

extern int	pq_combined_z_active;

/* Sub-profiling for D_DrawSurfaces breakdown */
unsigned int pq_prof_ds_calcgrad_cycles;
unsigned int pq_prof_ds_cachesurf_cycles;
unsigned int pq_prof_ds_sky_cycles;
extern cvar_t pq_cycleprof;

static int	miplevel;

float		scale_for_mip;
int			screenwidth;
int			ubasestep, errorterm, erroradjustup, erroradjustdown;
int			vstartscan;

// FIXME: should go away
extern void			R_RotateBmodel (void);
extern void			R_TransformFrustum (void);

vec3_t		transformed_modelorg;

/*
==============
D_DrawPoly

==============
*/
#ifndef KG_POLY
void D_DrawPoly (void)
{
// this driver takes spans, not polygons
}
#endif


/*
=============
D_MipLevelForScale
=============
*/
PQ_FASTTEXT int D_MipLevelForScale (float scale)
{
	int		lmiplevel;

	if (scale >= d_scalemip[0] )
		lmiplevel = 0;
	else if (scale >= d_scalemip[1] )
		lmiplevel = 1;
	else if (scale >= d_scalemip[2] )
		lmiplevel = 2;
	else
		lmiplevel = 3;

	if (lmiplevel < d_minmip)
		lmiplevel = d_minmip;

	return lmiplevel;
}


/*
==============
D_DrawSolidSurface
==============
*/

// FIXME: clean this up

void D_DrawSolidSurface (surf_t *surf, int color)
{
	espan_t	*span;
	byte	*pdest;
	int		u, u2, pix;
	
	pix = (color<<24) | (color<<16) | (color<<8) | color;
	for (span=surf->spans ; span ; span=span->pnext)
	{
		pdest = (byte *)d_viewbuffer + screenwidth*span->v;
		u = span->u;
		u2 = span->u + span->count - 1;
		((byte *)pdest)[u] = pix;

		if (u2 - u < 8)
		{
			for (u++ ; u <= u2 ; u++)
				((byte *)pdest)[u] = pix;
		}
		else
		{
			for (u++ ; u & 3 ; u++)
				((byte *)pdest)[u] = pix;

			u2 -= 4;
			for ( ; u <= u2 ; u+=4)
				*(int *)((byte *)pdest + u) = pix;
			u2 += 4;
			for ( ; u <= u2 ; u++)
				((byte *)pdest)[u] = pix;
		}
	}
}


/*
==============
D_CalcGradients
==============
*/
PQ_FASTTEXT void D_CalcGradients (msurface_t *pface)
{
	mplane_t	*pplane;
	float		mipscale;
	vec3_t		p_temp1;
	vec3_t		p_saxis, p_taxis;
	float		t;

	pplane = pface->plane;

	mipscale = 1.0f / (float)(1 << miplevel);

	TransformVector (pface->texinfo->vecs[0], p_saxis);
	TransformVector (pface->texinfo->vecs[1], p_taxis);

	t = xscaleinv * mipscale;
	d_sdivzstepu = p_saxis[0] * t;
	d_tdivzstepu = p_taxis[0] * t;

	t = yscaleinv * mipscale;
	d_sdivzstepv = -p_saxis[1] * t;
	d_tdivzstepv = -p_taxis[1] * t;

	d_sdivzorigin = p_saxis[2] * mipscale - xcenter * d_sdivzstepu -
			ycenter * d_sdivzstepv;
	d_tdivzorigin = p_taxis[2] * mipscale - xcenter * d_tdivzstepu -
			ycenter * d_tdivzstepv;

	VectorScale (transformed_modelorg, mipscale, p_temp1);

	t = 0x10000*mipscale;
	sadjust = ((fixed16_t)(DotProduct (p_temp1, p_saxis) * 0x10000 + 0.5f)) -
			((pface->texturemins[0] << 16) >> miplevel)
			+ pface->texinfo->vecs[0][3]*t;
	tadjust = ((fixed16_t)(DotProduct (p_temp1, p_taxis) * 0x10000 + 0.5f)) -
			((pface->texturemins[1] << 16) >> miplevel)
			+ pface->texinfo->vecs[1][3]*t;

//
// -1 (-epsilon) so we never wander off the edge of the texture
//
	bbextents = ((pface->extents[0] << 16) >> miplevel) - 1;
	bbextentt = ((pface->extents[1] << 16) >> miplevel) - 1;
}


/*
==============
D_DrawSurfaces
==============
*/
PQ_FASTTEXT void D_DrawSurfaces (void)
{
	surf_t			*s;
	msurface_t		*pface;
	surfcache_t		*pcurrentcache;
	vec3_t			world_transformed_modelorg;
	vec3_t			local_modelorg;
	int profiling = (int)pq_cycleprof.value;
	unsigned int prof_t;

	currententity = &cl_entities[0];
	TransformVector (modelorg, transformed_modelorg);
	VectorCopy (transformed_modelorg, world_transformed_modelorg);


// TODO: could preset a lot of this at mode set time
	if (r_drawflat.value)
	{
		for (s = &surfaces[1] ; s<surface_p ; s++)
		{
			if (!s->spans)
				continue;

			d_zistepu = s->d_zistepu;
			d_zistepv = s->d_zistepv;
			d_ziorigin = s->d_ziorigin;

			D_DrawSolidSurface (s, (int)s->data & 0xFF);
			D_DrawZSpans (s->spans);
		}
	}
	else
	{
		entity_t *last_bmodel_entity = NULL;

		for (s = &surfaces[1] ; s<surface_p ; s++)
		{
			if (!s->spans)
				continue;

			r_drawnpolycount++;

			d_zistepu = s->d_zistepu;
			d_zistepv = s->d_zistepv;
			d_ziorigin = s->d_ziorigin;

			if (s->flags & SURF_DRAWSKY)
			{
				if (!r_skymade)
				{
					R_MakeSky ();
				}

				if (profiling) prof_t = SYS_CYCLE_LO;
				D_DrawSkyScans8 (s->spans);
				if (profiling) pq_prof_ds_sky_cycles += SYS_CYCLE_LO - prof_t;
			}
			else if (s->flags & SURF_DRAWBACKGROUND)
			{
				d_zistepu = 0;
				d_zistepv = 0;
				d_ziorigin = -0.9;

				D_DrawSolidSurface (s, (int)r_clearcolor.value & 0xFF);
				D_DrawZSpans (s->spans);
			}
			else if (s->flags & SURF_DRAWTURB)
			{
				pface = s->data;
				miplevel = 0;
				cacheblock = (pixel_t *)
						((byte *)pface->texinfo->texture +
						pface->texinfo->texture->offsets[0]);
				cachewidth = 64;

				if (s->insubmodel)
				{
					if (s->entity != last_bmodel_entity) {
						if (last_bmodel_entity) {
							currententity = &cl_entities[0];
							VectorCopy(world_transformed_modelorg, transformed_modelorg);
							VectorCopy(base_vpn, vpn);
							VectorCopy(base_vup, vup);
							VectorCopy(base_vright, vright);
							VectorCopy(base_modelorg, modelorg);
							R_TransformFrustum();
						}
						currententity = s->entity;
						VectorSubtract(r_origin, currententity->origin, local_modelorg);
						TransformVector(local_modelorg, transformed_modelorg);
						R_RotateBmodel();
						last_bmodel_entity = s->entity;
					}
				}
				else if (last_bmodel_entity)
				{
					currententity = &cl_entities[0];
					VectorCopy(world_transformed_modelorg, transformed_modelorg);
					VectorCopy(base_vpn, vpn);
					VectorCopy(base_vup, vup);
					VectorCopy(base_vright, vright);
					VectorCopy(base_modelorg, modelorg);
					R_TransformFrustum();
					last_bmodel_entity = NULL;
				}

				if (profiling) prof_t = SYS_CYCLE_LO;
				D_CalcGradients (pface);
				if (profiling) pq_prof_ds_calcgrad_cycles += SYS_CYCLE_LO - prof_t;
				Turbulent8 (s->spans);
				if (!pq_combined_z_active)
					D_DrawZSpans (s->spans);
				pq_combined_z_active = 0;
			}
			else
			{
				if (s->insubmodel)
				{
					if (s->entity != last_bmodel_entity) {
						if (last_bmodel_entity) {
							currententity = &cl_entities[0];
							VectorCopy(world_transformed_modelorg, transformed_modelorg);
							VectorCopy(base_vpn, vpn);
							VectorCopy(base_vup, vup);
							VectorCopy(base_vright, vright);
							VectorCopy(base_modelorg, modelorg);
							R_TransformFrustum();
						}
						currententity = s->entity;
						VectorSubtract(r_origin, currententity->origin, local_modelorg);
						TransformVector(local_modelorg, transformed_modelorg);
						R_RotateBmodel();
						last_bmodel_entity = s->entity;
					}
				}
				else if (last_bmodel_entity)
				{
					currententity = &cl_entities[0];
					VectorCopy(world_transformed_modelorg, transformed_modelorg);
					VectorCopy(base_vpn, vpn);
					VectorCopy(base_vup, vup);
					VectorCopy(base_vright, vright);
					VectorCopy(base_modelorg, modelorg);
					R_TransformFrustum();
					last_bmodel_entity = NULL;
				}

				pface = s->data;
				miplevel = D_MipLevelForScale (s->nearzi * scale_for_mip
				* pface->texinfo->mipadjust);

				if (profiling) prof_t = SYS_CYCLE_LO;
				pcurrentcache = D_CacheSurface (pface, miplevel);
				if (profiling) pq_prof_ds_cachesurf_cycles += SYS_CYCLE_LO - prof_t;

				cacheblock = (pixel_t *)pcurrentcache->data;
				cachewidth = pcurrentcache->width;

				if (profiling) prof_t = SYS_CYCLE_LO;
				D_CalcGradients (pface);
				if (profiling) pq_prof_ds_calcgrad_cycles += SYS_CYCLE_LO - prof_t;

				(*d_drawspans) (s->spans);

				if (!pq_combined_z_active)
					D_DrawZSpans (s->spans);
				pq_combined_z_active = 0;
			}
		}

		// Restore world state if loop ended while in bmodel state
		if (last_bmodel_entity)
		{
			currententity = &cl_entities[0];
			VectorCopy(world_transformed_modelorg, transformed_modelorg);
			VectorCopy(base_vpn, vpn);
			VectorCopy(base_vup, vup);
			VectorCopy(base_vright, vright);
			VectorCopy(base_modelorg, modelorg);
			R_TransformFrustum();
		}
	}
}



#ifdef KG_POLY
/*
==============
karabas Ф7: the polygon driver (r_drawpolys): R_RenderPoly (r_draw.c) clips and projects a face, D_DrawPoly
sends it to the rasterizer of the core (quake_span: PVTX / PDRAW, the surface as D_DrawSpans8 of Ф3е) that
draws a world pixel only where nothing is drawn yet (the world comes front to back from the BSP: no z compare,
which the 16-bit z, coarse far away, gets wrong) and z-tests the bmodels; the coverage mask of the rasterizer is cleared
per frame (kg_poly_begin).
Sky and water: the same rows / pixels in software (kg_poly_spans, as src/sim/qspan_model.h poly()), the pixels
the rasterizer would not draw dropped, then D_DrawSkyScans8 / Turbulent8 and their z by the drawer.
==============
*/
#include "kg_hwspan.h"
extern void kg_span_idle (void);

#define KG_PMAXV 64			/* the vertex buffer of the rasterizer */
#define KG_MAXSPANS 1024

static espan_t kg_spans[KG_MAXSPANS], kg_vspans[KG_MAXSPANS];

extern int kg_pu[], kg_pv[];

/* the frame start: the coverage mask of the rasterizer cleared */
void kg_poly_begin (void)
{
	MMIO_QSP_ZCLR = 0;
}

/* the x of an edge at row y in 2^-16 pixels (qspan_model.h edge_x) */
static int kg_edge_x (int Xa, int Ya, int Xb, int Yb, int y)
{
	int step = ((Xb - Xa) * 65536) / (Yb - Ya);	/* |Xb - Xa| < 2^13: no overflow, as the 64-bit model */
	int yf = (Ya + 15) >> 4;
	int x0 = Xa * 4096 + (int)(((long long)(16 * yf - Ya) * step) >> 4);
	return x0 + (y - yf) * step;
}

/* the spans of the polygon as the rasterizer draws them (into kg_spans; returns the count) */
static int kg_poly_spans (const int *pu, const int *pv, int n)
{
	int i, y, y0, y1, vmin = 0x7fffffff, vmax = -0x7fffffff, ns = 0;

	for (i = 0 ; i < n ; i++)
	{
		if (pv[i] < vmin) vmin = pv[i];
		if (pv[i] > vmax) vmax = pv[i];
	}
	y0 = (vmin + 15) >> 4;
	y1 = (vmax + 15) >> 4;
	if (y0 < 0) y0 = 0;
	if (y1 > 200) y1 = 200;
	for (y = y0 ; y < y1 && ns < KG_MAXSPANS ; y++)
	{
		int xl = 0x7fffffff, xr = -0x7fffffff - 1, found = 0, xs, xe;
		for (i = 0 ; i < n ; i++)
		{
			int j = i + 1 == n ? 0 : i + 1;
			int Xa = pu[i], Ya = pv[i], Xb = pu[j], Yb = pv[j], x;
			if (Ya == Yb)
				continue;
			if (Ya > Yb) { int t = Xa; Xa = Xb; Xb = t; t = Ya; Ya = Yb; Yb = t; }
			if (16 * y < Ya || 16 * y >= Yb)
				continue;
			x = kg_edge_x (Xa, Ya, Xb, Yb, y);
			if (x < xl) xl = x;
			if (x > xr) xr = x;
			found++;
		}
		if (found < 2)
			continue;
		xs = (int)(((long long)xl + 0xffff) >> 16);
		xe = (int)(((long long)xr + 0xffff) >> 16);
		if (xs < 0) xs = 0;
		if (xe > 320) xe = 320;
		if (xs >= xe)
			continue;
		kg_spans[ns].u = xs;
		kg_spans[ns].v = y;
		kg_spans[ns].count = xe - xs;
		ns++;
	}
	return ns;
}

/* the parts of the spans the rasterizer would draw (ztest 0: not covered yet (the mask), 1: z >= stored),
   linked; NULL if none */
static espan_t *kg_visible_spans (int ns, int ztest)
{
	int i, nv = 0;
	int izistep = (int)(d_zistepu * 0x8000 * 0x10000);
	espan_t *head = NULL, **link = &head;

	kg_span_idle ();			/* the z-buffer / mask are the drawer's until it is idle */
	for (i = 0 ; i < ns ; i++)
	{
		espan_t *sp = &kg_spans[i];
		float zi = d_ziorigin + (float)sp->v * d_zistepv + (float)sp->u * d_zistepu;
		int izi = (int)(zi * 0x8000 * 0x10000), u, run = -1;
		short *pz = d_pzbuffer + d_zwidth * sp->v;
		unsigned off = sp->v * 320 + sp->u, w = 0;

		if (!ztest)
		{
			MMIO_QSP_MADDR = off >> 5;
			w = MMIO_QSP_MDATA >> (off & 31);
		}
		for (u = sp->u ; u <= sp->u + sp->count ; u++, izi += izistep, off++)
		{
			int vis;
			if (ztest)
				vis = u < sp->u + sp->count && (short)(izi >> 16) >= pz[u];
			else
			{
				if (!(off & 31) && u < sp->u + sp->count)
					w = MMIO_QSP_MDATA;
				vis = u < sp->u + sp->count && !(w & 1);
				w >>= 1;
			}
			if (vis && run < 0)
				run = u;
			else if (!vis && run >= 0)
			{
				if (nv < KG_MAXSPANS)
				{
					espan_t *o = &kg_vspans[nv++];
					o->u = run;
					o->v = sp->v;
					o->count = u - run;
					*link = o;
					link = &o->pnext;
				}
				run = -1;
			}
		}
	}
	*link = NULL;
	return head;
}

void D_DrawPoly (void)
{
	msurface_t	*pface = r_polydesc.pcurrentface;
	int			n = r_polydesc.numverts, i;
	int			*pu = kg_pu, *pvv = kg_pv;	/* 12.4, by R_RenderPoly (r_draw.c) */
	mplane_t	*pplane = pface->plane;
	vec3_t		p_normal;
	float		distinv;

	if (n < 3)
		return;

	/* 1/z over the screen as R_RenderFace / R_RenderBmodelFace (modelorg: of the entity, rotated) */
	TransformVector (pplane->normal, p_normal);
	distinv = 1.0f / (pplane->dist - DotProduct (modelorg, pplane->normal));
	d_zistepu = p_normal[0] * xscaleinv * distinv;
	d_zistepv = -p_normal[1] * yscaleinv * distinv;
	d_ziorigin = p_normal[2] * distinv - xcenter * d_zistepu - ycenter * d_zistepv;
	if (currententity != &cl_entities[0])
	{
		float bias = distinv * 0.002f;	/* bmodels a little closer (R_RenderBmodelFace) */
		d_ziorigin += (bias > 0) ? bias : -bias;
	}
	TransformVector (modelorg, transformed_modelorg);

	if (pface->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
	{
		espan_t *vis;
		int ns = kg_poly_spans (pu, pvv, n);
		if (!ns)
			return;
		vis = kg_visible_spans (ns, currententity != &cl_entities[0]);
		if (!vis)
			return;
		if (pface->flags & SURF_DRAWSKY)
		{
			if (!r_skymade)
				R_MakeSky ();
			D_DrawSkyScans8 (vis);
		}
		else
		{
			miplevel = 0;
			cacheblock = (pixel_t *)
					((byte *)pface->texinfo->texture +
					pface->texinfo->texture->offsets[0]);
			cachewidth = 64;
			D_CalcGradients (pface);
			Turbulent8 (vis);
		}
		D_DrawZSpans (vis);
		return;
	}

	{
		surfcache_t *pcurrentcache;
		miplevel = D_MipLevelForScale (r_polydesc.nearzi * scale_for_mip * pface->texinfo->mipadjust);
		pcurrentcache = D_CacheSurface (pface, miplevel);
		cacheblock = (pixel_t *)pcurrentcache->data;
		cachewidth = pcurrentcache->width;
		D_CalcGradients (pface);
	}
	kg_span_tex ((unsigned char *)cacheblock, cachewidth);
	kg_persp_surface (d_sdivzorigin, d_sdivzstepu, d_sdivzstepv, d_tdivzorigin, d_tdivzstepu, d_tdivzstepv,
	                  d_ziorigin, d_zistepu, d_zistepv, sadjust, tadjust, bbextents, bbextentt);
	for (i = 0 ; i < n ; i++)
		kg_poly_vtx (pu[i], pvv[i]);
	kg_poly_draw (currententity != &cl_entities[0]);
	r_drawnpolycount++;
}
#endif
