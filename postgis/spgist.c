#include "postgres.h"
#include "access/gist.h"    /* For GiST */
#include "access/itup.h"
#include "access/skey.h"

#include "access/spgist.h"
#include "catalog/pg_type.h"

#include "../postgis_config.h"

#include "liblwgeom.h"         /* For standard geometry types. */
#include "lwgeom_pg.h"       /* For debugging macros. */
#include "gserialized_gist.h"	     /* For utility functions. */
#include "liblwgeom_internal.h"  /* For MAXFLOAT */

Datum
spgist_geom_config(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(spgist_geom_config);

Datum
spgist_geom_leaf_consistent(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(spgist_geom_leaf_consistent);

Datum
spgist_geom_inner_consistent(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(spgist_geom_inner_consistent);

Datum
spgist_geom_choose(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(spgist_geom_choose);

Datum
spgist_geom_picksplit(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(spgist_geom_picksplit);

static int16
getQuadrant(BOX2DF *centroid, BOX2DF *tst);

static bool box2df_contains(const BOX2DF *a, const BOX2DF *b)
{
	if ( ! a || ! b ) return FALSE; /* TODO: might be smarter for EMPTY */

	if ( (a->xmin > b->xmin) || (a->xmax < b->xmax) ||
	     (a->ymin > b->ymin) || (a->ymax < b->ymax) )
	{
		return FALSE;
	}

	return TRUE;
}

static bool box2df_overright(const BOX2DF *a, const BOX2DF *b)
{
	if ( ! a || ! b ) return FALSE; /* TODO: might be smarter for EMPTY */

	/* a.xmin >= b.xmin */
	return a->xmin >= b->xmin;
}

static bool box2df_overbelow(const BOX2DF *a, const BOX2DF *b)
{
	if ( ! a || ! b ) return FALSE; /* TODO: might be smarter for EMPTY */

	/* a.ymax <= b.ymax */
	return a->ymax <= b->ymax;
}

static bool box2df_overleft(const BOX2DF *a, const BOX2DF *b)
{
	if ( ! a || ! b ) return FALSE; /* TODO: might be smarter for EMPTY */

	/* a.xmax <= b.xmax */
	return a->xmax <= b->xmax;
}

static bool box2df_overabove(const BOX2DF *a, const BOX2DF *b)
{
	if ( ! a || ! b ) return FALSE; /* TODO: might be smarter for EMPTY */

	/* a.ymin >= b.ymin */
	return a->ymin >= b->ymin;
}

static bool box2df_equals(const BOX2DF *a, const BOX2DF *b)
{
	if ( a &&  b ) {
		if ( (a->xmin != b->xmin) || (a->xmax != b->xmax) ||
		     (a->ymin != b->ymin) || (a->ymax != b->ymax) )
		{
			return FALSE;
		}
		return TRUE;
	} else if ( a || b ) {
		/* one empty, one not */
		return FALSE;
	} else {
		/* both empty */
		return TRUE;
	}
}

static inline int box2df_from_gbox_p(GBOX *box, BOX2DF *a)
{
	a->xmin = next_float_down(box->xmin);
	a->xmax = next_float_up(box->xmax);
	a->ymin = next_float_down(box->ymin);
	a->ymax = next_float_up(box->ymax);
	return LW_SUCCESS;
}

static bool box2df_below(const BOX2DF *a, const BOX2DF *b)
{
	if ( ! a || ! b ) return FALSE; /* TODO: might be smarter for EMPTY */

	/* a.ymax < b.ymin */
	return a->ymax < b->ymin;
}

static bool box2df_above(const BOX2DF *a, const BOX2DF *b)
{
	if ( ! a || ! b ) return FALSE; /* TODO: might be smarter for EMPTY */

	/* a.ymin > b.ymax */
	return a->ymin > b->ymax;
}

static bool box2df_left(const BOX2DF *a, const BOX2DF *b)
{
	if ( ! a || ! b ) return FALSE; /* TODO: might be smarter for EMPTY */

	/* a.xmax < b.xmin */
	return a->xmax < b->xmin;
}

static bool box2df_right(const BOX2DF *a, const BOX2DF *b)
{
	if ( ! a || ! b ) return FALSE; /* TODO: might be smarter for EMPTY */

	/* a.xmin > b.xmax */
	return a->xmin > b->xmax;
}


static int 
gserialized_datum_get_box2df_p(Datum gsdatum, BOX2DF *box2df)
{
	GSERIALIZED *gpart;
	uint8_t flags;
	int result = LW_SUCCESS;

	POSTGIS_DEBUG(4, "entered function");

	/*
	** The most info we need is the 8 bytes of serialized header plus the 
	** of floats necessary to hold the bounding box.
	*/
	gpart = (GSERIALIZED*)PG_DETOAST_DATUM_SLICE(gsdatum, 0, 8 + sizeof(BOX2DF));
	flags = gpart->flags;

	POSTGIS_DEBUGF(4, "got flags %d", gpart->flags);

	/* Do we even have a serialized bounding box? */
	if ( FLAGS_GET_BBOX(flags) )
	{
		/* Yes! Copy it out into the box! */
		POSTGIS_DEBUG(4, "copying box out of serialization");
		memcpy(box2df, gpart->data, sizeof(BOX2DF));
		result = LW_SUCCESS;
	}
	else
	{
		/* No, we need to calculate it from the full object. */
		GBOX gbox;
		GSERIALIZED *g = (GSERIALIZED*)PG_DETOAST_DATUM(gsdatum);
		LWGEOM *lwgeom = lwgeom_from_gserialized(g);
		if ( lwgeom_calculate_gbox(lwgeom, &gbox) == LW_FAILURE )
		{
			POSTGIS_DEBUG(4, "could not calculate bbox, returning failure");
			lwgeom_free(lwgeom);
			return LW_FAILURE;
		}
		lwgeom_free(lwgeom);
		result = box2df_from_gbox_p(&gbox, box2df);
	}
	
	return result;
}



Datum
spgist_geom_config(PG_FUNCTION_ARGS)
{
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = 17894; /*17863;PostGIS geometry, geography, and raster spatial types and functions */
	cfg->labelType = VOIDOID;	/* we don't need node labels */
	cfg->canReturnData = true;
	cfg->longValuesOK = false;

        POSTGIS_DEBUG(1, "done");
	PG_RETURN_VOID();
}

Datum
spgist_geom_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);

        BOX2DF inputbox;
        BOX2DF *inputbox_p = &inputbox;
        BOX2DF *centroidbox_p;

        gserialized_datum_get_box2df_p(in->datum, inputbox_p);

	if (in->allTheSame)
	{
		out->resultType = spgMatchNode;
		/* nodeN will be set by core */
		out->result.matchNode.levelAdd = 0;
		out->result.matchNode.restDatum = in->datum;

		PG_RETURN_VOID();
	}

	Assert(in->hasPrefix);
	Assert(in->nNodes == 4);

        centroidbox_p = (BOX2DF *)in->prefixDatum;

	out->resultType = spgMatchNode;
	out->result.matchNode.nodeN = getQuadrant(centroidbox_p, inputbox_p) - 1;
	out->result.matchNode.levelAdd = 0;
	out->result.matchNode.restDatum = in->datum;

	PG_RETURN_VOID();
}

Datum
spgist_geom_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	int i;
        double x, y;
        BOX2DF *centroidbox_p;

	/* Use the average values of x and y as the centroid point */
        x = 0;
        y = 0;

	for (i = 0; i < in->nTuples; i++)
	{
          LWPOINT *apt = (LWPOINT *)lwgeom_from_gserialized((GSERIALIZED *)in->datums[i]);

          x += lwpoint_get_x(apt);
          y += lwpoint_get_y(apt);

          lwpoint_free(apt);
	}

	x /= in->nTuples;
	y /= in->nTuples;
        
        centroidbox_p = palloc(sizeof(BOX2DF));

        centroidbox_p->xmin = x;
        centroidbox_p->xmax = x;
        centroidbox_p->ymin = y;
        centroidbox_p->ymax = y;

	out->hasPrefix = true;
	out->prefixDatum = PointerGetDatum(centroidbox_p);

	out->nNodes = 4;
	out->nodeLabels = NULL;		/* we don't need node labels */

	out->mapTuplesToNodes = palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = palloc(sizeof(Datum) * in->nTuples);

	for (i = 0; i < in->nTuples; i++)
	{
                BOX2DF box;
                gserialized_datum_get_box2df_p(in->datums[i], &box);

		int quadrant = getQuadrant(centroidbox_p, &box) - 1;

		out->leafTupleDatums[i] = PointerGetDatum(in->datums[i]);
		out->mapTuplesToNodes[i] = quadrant;
	}

	PG_RETURN_VOID();
}


/*
 * Determine which quadrant a point falls into, relative to the centroid.
 *
 * Quadrants are identified like this:
 *
 *	 4	|  1
 *	----+-----
 *	 3	|  2
 *
 * Points on one of the axes are taken to lie in the lowest-numbered
 * adjacent quadrant.
 */
static int16
getQuadrant(BOX2DF *centroid, BOX2DF *tst) 
{
        double x = centroid->xmin;
        double y = centroid->ymin;

        if (tst->xmax >= x && tst->ymax >= y)
                return 1;

        if (tst->xmax >= x && tst->ymax <= y)
                return 2;

        if (tst->xmax <= x && tst->ymin <= y)
                return 3;

        if (tst->xmax <= x && tst->ymin >= y)
                return 4;

	elog(ERROR, "getQuadrant: impossible case");
	return 0;
}

Datum
spgist_geom_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
        BOX2DF *centroidbox_p;
	int which;
	int i;
   
	if (in->allTheSame)
	{
		/* Report that all nodes should be visited */
		out->nNodes = in->nNodes;
		out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
		for (i = 0; i < in->nNodes; i++)
			out->nodeNumbers[i] = i;
		PG_RETURN_VOID();
	}

	Assert(in->hasPrefix);

        centroidbox_p = (BOX2DF *)in->prefixDatum;

	Assert(in->nNodes == 4);

	/* "which" is a bitmask of quadrants that satisfy all constraints */
	which = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4);

	for (i = 0; i < in->nkeys; i++)
	{
                BOX2DF box;
                BOX2DF *box_p = &box;
                gserialized_datum_get_box2df_p(in->scankeys[i].sk_argument, box_p);

		switch (in->scankeys[i].sk_strategy)
		{
			case RTLeftStrategyNumber:
                                if (box2df_left(box_p, centroidbox_p))
                                        which &= (1 << 3) | (1 << 4);
				break;
			case RTRightStrategyNumber:
                                if (box2df_right(box_p, centroidbox_p))
					which &= (1 << 1) | (1 << 2);
				break;
			/* case RTSameStrategyNumber: */
			/* 	which &= (1 << getQuadrant(centroidbox_p, box_p)); */
			/* 	break; */
			case RTBelowStrategyNumber:
                                if (box2df_below(centroidbox_p, box_p))
					which &= (1 << 2) | (1 << 3);
				break;
			case RTAboveStrategyNumber:
                                if (box2df_above(centroidbox_p, box_p))
					which &= (1 << 1) | (1 << 4);
				break;
			case RTContainedByStrategyNumber:

                                if (box2df_contains(box_p, centroidbox_p))
				{
					/* centroid is in box, so all quadrants are OK */
				}
				else
				{
					/* identify quadrant(s) containing all corners of box */
					POINT2D	p;
                                        BOX2DF b;
                                        BOX2DF *b_p = &b;
					int r = 0;

                                        b_p->xmin = b_p->xmax = box_p->xmin;
                                        b_p->ymin = b_p->ymax = box_p->ymin;

					r |= 1 << getQuadrant(centroidbox_p, b_p);

                                        b_p->xmin = b_p->xmax = box_p->xmax;
                                        b_p->ymin = b_p->ymax = box_p->ymin;

					r |= 1 << getQuadrant(centroidbox_p, b_p);

                                        b_p->xmin = b_p->xmax = box_p->xmax;
                                        b_p->ymin = b_p->ymax = box_p->ymax;

					r |= 1 << getQuadrant(centroidbox_p, b_p);

                                        b_p->xmin = b_p->xmax = box_p->xmin;
                                        b_p->ymin = b_p->ymax = box_p->ymax;

					r |= 1 << getQuadrant(centroidbox_p, b_p);

					which &= r;
				}
				break;
			default:
				elog(ERROR, "unrecognized strategy number: %d",
					 in->scankeys[i].sk_strategy);
				break;
		}

		if (which == 0)
			break;				/* no need to consider remaining conditions */
	}

	/* We must descend into the quadrant(s) identified by which */
	out->nodeNumbers = (int *) palloc(sizeof(int) * 4);
	out->nNodes = 0;
	for (i = 1; i <= 4; i++)
	{
		if (which & (1 << i))
			out->nodeNumbers[out->nNodes++] = i - 1;
	}

	PG_RETURN_VOID();
}


Datum
spgist_geom_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	bool		res;
	int			i;
        BOX2DF datum;
        BOX2DF *datum_p = &datum;
        
        gserialized_datum_get_box2df_p(in->leafDatum, datum_p);

	/* all tests are exact */
	out->recheck = false;

	/* leafDatum is what it is... */
	out->leafValue = in->leafDatum;

	/* Perform the required comparison(s) */
	res = true;
	for (i = 0; i < in->nkeys; i++)
	{
                BOX2DF query;
                BOX2DF *query_p = &query;

                gserialized_datum_get_box2df_p(in->scankeys[i].sk_argument, query_p);

		switch (in->scankeys[i].sk_strategy)
		{
			case RTLeftStrategyNumber:
                                res = (bool) box2df_left(datum_p, query_p);
				break;
			case RTRightStrategyNumber:
                                res = (bool) box2df_right(datum_p, query_p);
				break;
			case RTSameStrategyNumber:
                                res = (bool) box2df_equals(datum_p, query_p);
				break;
			case RTBelowStrategyNumber:
                                res = (bool) box2df_below(datum_p, query_p);
				break;
			case RTAboveStrategyNumber:
                                res = (bool) box2df_above(datum_p, query_p);
				break;
			case RTContainedByStrategyNumber:
                                res = (bool) box2df_contains(query_p, datum_p);
				break;
			default:
				elog(ERROR, "unrecognized strategy number: %d",
					 in->scankeys[i].sk_strategy);
				break;
		}

		if (!res)
			break;
	}

	PG_RETURN_BOOL(res);
}
