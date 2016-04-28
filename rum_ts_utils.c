/*-------------------------------------------------------------------------
 *
 * rum_ts_utils.c
 *		various support functions
 *
 * Portions Copyright (c) 2015-2016, Postgres Professional
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "tsearch/ts_utils.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "rum.h"

#include <math.h>

PG_FUNCTION_INFO_V1(rum_extract_tsvector);
PG_FUNCTION_INFO_V1(rum_extract_tsquery);
PG_FUNCTION_INFO_V1(rum_tsvector_config);
PG_FUNCTION_INFO_V1(rum_tsquery_pre_consistent);
PG_FUNCTION_INFO_V1(rum_tsquery_distance);
PG_FUNCTION_INFO_V1(rum_ts_distance);

#define RANK_NO_NORM			0x00
#define RANK_NORM_LOGLENGTH		0x01
#define RANK_NORM_LENGTH		0x02
#define RANK_NORM_EXTDIST		0x04
#define RANK_NORM_UNIQ			0x08
#define RANK_NORM_LOGUNIQ		0x10
#define RANK_NORM_RDIVRPLUS1	0x20
#define DEF_NORM_METHOD			RANK_NO_NORM

static float calc_rank_pos_and(float *w, Datum *addInfo, bool *addInfoIsNull,
						   int size);
static float calc_rank_pos_or(float *w, Datum *addInfo, bool *addInfoIsNull,
						  int size);

static float calc_rank_or(const float *w, TSVector t, TSQuery q);
static float calc_rank_and(const float *w, TSVector t, TSQuery q);

typedef struct
{
	QueryItem  *first_item;
	bool	   *check;
	int		   *map_item_operand;
	bool	   *need_recheck;
} RumChkVal;

static bool
checkcondition_rum(void *checkval, QueryOperand *val, ExecPhraseData *data)
{
	RumChkVal  *gcv = (RumChkVal *) checkval;
	int			j;

	/* if any val requiring a weight is used, set recheck flag */
	if (val->weight != 0)
		*(gcv->need_recheck) = true;

	/* convert item's number to corresponding entry's (operand's) number */
	j = gcv->map_item_operand[((QueryItem *) val) - gcv->first_item];

	/* return presence of current entry in indexed value */
	return gcv->check[j];
}

Datum
rum_tsquery_pre_consistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);

	TSQuery		query = PG_GETARG_TSQUERY(2);

	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	bool	    recheck;
	bool		res = FALSE;

	if (query->size > 0)
	{
		QueryItem  *item;
		RumChkVal	gcv;

		/*
		 * check-parameter array has one entry for each value (operand) in the
		 * query.
		 */
		gcv.first_item = item = GETQUERY(query);
		gcv.check = check;
		gcv.map_item_operand = (int *) (extra_data[0]);
		gcv.need_recheck = &recheck;

		res = TS_execute(GETQUERY(query),
						 &gcv,
						 false,
						 checkcondition_rum);
	}

	PG_RETURN_BOOL(res);
}

static float weights[] = {0.1f, 0.2f, 0.4f, 1.0f};

#define wpos(wep)	( w[ WEP_GETWEIGHT(wep) ] )
/* A dummy WordEntryPos array to use when haspos is false */
static WordEntryPosVector POSNULL = {
	1,							/* Number of elements that follow */
	{0}
};

#define SIXTHBIT 0x20
#define LOWERMASK 0x1F

/*
 * Returns a weight of a word collocation
 */
static float4
word_distance(int32 w)
{
	if (w > 100)
		return 1e-30f;

	return 1.0 / (1.005 + 0.05 * exp(((float4) w) / 1.5 - 2));
}

static int
cnt_length(TSVector t)
{
	WordEntry  *ptr = ARRPTR(t),
			   *end = (WordEntry *) STRPTR(t);
	int			len = 0;

	while (ptr < end)
	{
		int			clen = POSDATALEN(t, ptr);

		if (clen == 0)
			len += 1;
		else
			len += clen;

		ptr++;
	}

	return len;
}

#define WordECompareQueryItem(e,q,p,i,m) \
	tsCompareString((q) + (i)->distance, (i)->length,	\
					(e) + (p)->pos, (p)->len, (m))

/*
 * Returns a pointer to a WordEntry's array corresponding to 'item' from
 * tsvector 't'. 'q' is the TSQuery containing 'item'.
 * Returns NULL if not found.
 */
static WordEntry *
find_wordentry(TSVector t, TSQuery q, QueryOperand *item, int32 *nitem)
{
	WordEntry  *StopLow = ARRPTR(t);
	WordEntry  *StopHigh = (WordEntry *) STRPTR(t);
	WordEntry  *StopMiddle = StopHigh;
	int			difference;

	*nitem = 0;

	/* Loop invariant: StopLow <= item < StopHigh */
	while (StopLow < StopHigh)
	{
		StopMiddle = StopLow + (StopHigh - StopLow) / 2;
		difference = WordECompareQueryItem(STRPTR(t), GETOPERAND(q), StopMiddle, item, false);
		if (difference == 0)
		{
			StopHigh = StopMiddle;
			*nitem = 1;
			break;
		}
		else if (difference > 0)
			StopLow = StopMiddle + 1;
		else
			StopHigh = StopMiddle;
	}

	if (item->prefix)
	{
		if (StopLow >= StopHigh)
			StopMiddle = StopHigh;

		*nitem = 0;

		while (StopMiddle < (WordEntry *) STRPTR(t) &&
			   WordECompareQueryItem(STRPTR(t), GETOPERAND(q), StopMiddle, item, true) == 0)
		{
			(*nitem)++;
			StopMiddle++;
		}
	}

	return (*nitem > 0) ? StopHigh : NULL;
}

static int
compress_pos(char *target, uint16 *pos, int npos)
{
	int i;
	uint16 prev = 0, delta;
	char *ptr;

	ptr = target;
	for (i = 0; i < npos; i++)
	{
		delta = WEP_GETPOS(pos[i]) - WEP_GETPOS(prev);

		while (true)
		{
			if (delta >= SIXTHBIT)
			{
				*ptr = (delta & (~HIGHBIT)) | HIGHBIT;
				ptr++;
				delta >>= 7;
			}
			else
			{
				*ptr = delta | (WEP_GETWEIGHT(pos[i]) << 5);
				ptr++;
				break;
			}
		}
		prev = pos[i];
	}
	return ptr - target;
}

static char *
decompress_pos(char *ptr, uint16 *pos)
{
	int i;
	uint8 v;
	uint16 delta = 0;

	i = 0;
	while (true)
	{
		v = *ptr;
		ptr++;
		if (v & HIGHBIT)
		{
			delta |= (v & (~HIGHBIT)) << i;
		}
		else
		{
			delta |= (v & LOWERMASK) << i;
			*pos += delta;
			WEP_SETWEIGHT(*pos, v >> 5);
			return ptr;
		}
		i += 7;
	}
}

static int
count_pos(char *ptr, int len)
{
	int count = 0, i;
	for (i = 0; i < len; i++)
	{
		if (!(ptr[i] & HIGHBIT))
			count++;
	}
	return count;
}

static float
calc_rank_pos_and(float *w, Datum *addInfo, bool *addInfoIsNull, int size)
{
	int			i,
				k,
				l,
				p;
	WordEntryPos post,
			   ct;
	int32		dimt,
				lenct,
				dist;
	float		res = -1.0;
	char		*ptrt, *ptrc;

	if (size < 2)
	{
		return calc_rank_pos_or(w, addInfo, addInfoIsNull, size);
	}
	WEP_SETPOS(POSNULL.pos[0], MAXENTRYPOS - 1);

	for (i = 0; i < size; i++)
	{
		if (!addInfoIsNull[i])
		{
			dimt = count_pos(VARDATA_ANY(addInfo[i]), VARSIZE_ANY_EXHDR(addInfo[i]));
			ptrt = (char *)VARDATA_ANY(addInfo[i]);
		}
		else
		{
			dimt = POSNULL.npos;
			ptrt = (char *)POSNULL.pos;
		}
		for (k = 0; k < i; k++)
		{
			if (!addInfoIsNull[k])
				lenct = count_pos(VARDATA_ANY(addInfo[k]), VARSIZE_ANY_EXHDR(addInfo[k]));
			else
				lenct = POSNULL.npos;
			post = 0;
			for (l = 0; l < dimt; l++)
			{
				ptrt = decompress_pos(ptrt, &post);
				ct = 0;
				if (!addInfoIsNull[k])
					ptrc = (char *)VARDATA_ANY(addInfo[k]);
				else
					ptrc = (char *)POSNULL.pos;
				for (p = 0; p < lenct; p++)
				{
					ptrc = decompress_pos(ptrc, &ct);
					dist = Abs((int) WEP_GETPOS(post) - (int) WEP_GETPOS(ct));
					if (dist || (dist == 0 && (ptrt == (char *)POSNULL.pos || ptrc == (char *)POSNULL.pos)))
					{
						float		curw;

						if (!dist)
							dist = MAXENTRYPOS;
						curw = sqrt(wpos(post) * wpos(ct) * word_distance(dist));
						res = (res < 0) ? curw : 1.0 - (1.0 - res) * (1.0 - curw);
					}
				}
			}
		}

	}
	return res;
}

static float
calc_rank_pos_or(float *w, Datum *addInfo, bool *addInfoIsNull, int size)
{
	WordEntryPos post;
	int32		dimt,
				j,
				i;
	float		res = 0.0;
	char *ptrt;

	for (i = 0; i < size; i++)
	{
		float		resj,
					wjm;
		int32		jm;

		if (!addInfoIsNull[i])
		{
			dimt = count_pos(VARDATA_ANY(addInfo[i]), VARSIZE_ANY_EXHDR(addInfo[i]));
			ptrt = (char *)VARDATA_ANY(addInfo[i]);
		}
		else
		{
			dimt = POSNULL.npos;
			ptrt = (char *)POSNULL.pos;
		}

		resj = 0.0;
		wjm = -1.0;
		jm = 0;
		post = 0;
		for (j = 0; j < dimt; j++)
		{
			ptrt = decompress_pos(ptrt, &post);
			resj = resj + wpos(post) / ((j + 1) * (j + 1));
			if (wpos(post) > wjm)
			{
				wjm = wpos(post);
				jm = j;
			}
		}
/*
		limit (sum(i/i^2),i->inf) = pi^2/6
		resj = sum(wi/i^2),i=1,noccurence,
		wi - should be sorted desc,
		don't sort for now, just choose maximum weight. This should be corrected
		Oleg Bartunov
*/
		res = res + (wjm + resj - wjm / ((jm + 1) * (jm + 1))) / 1.64493406685;

	}
	if (size > 0)
		res = res / size;
	return res;
}

static float
calc_rank_pos(float *w, TSQuery q, Datum *addInfo, bool *addInfoIsNull, int size)
{
	QueryItem  *item = GETQUERY(q);
	float		res = 0.0;

	if (!size || !q->size)
		return 0.0;

	/* XXX: What about NOT? */
	res = (item->type == QI_OPR && item->qoperator.oper == OP_AND) ?
		calc_rank_pos_and(w, addInfo, addInfoIsNull, size) :
		calc_rank_pos_or(w, addInfo, addInfoIsNull, size);

	if (res < 0)
		res = 1e-20f;

	return res;
}

/*
 * sort QueryOperands by (length, word)
 */
static int
compareQueryOperand(const void *a, const void *b, void *arg)
{
	char	   *operand = (char *) arg;
	QueryOperand *qa = (*(QueryOperand *const *) a);
	QueryOperand *qb = (*(QueryOperand *const *) b);

	return tsCompareString(operand + qa->distance, qa->length,
						   operand + qb->distance, qb->length,
						   false);
}

/*
 * Returns a sorted, de-duplicated array of QueryOperands in a query.
 * The returned QueryOperands are pointers to the original QueryOperands
 * in the query.
 *
 * Length of the returned array is stored in *size
 */
static QueryOperand **
SortAndUniqItems(TSQuery q, int *size)
{
	char	   *operand = GETOPERAND(q);
	QueryItem  *item = GETQUERY(q);
	QueryOperand **res,
			  **ptr,
			  **prevptr;

	ptr = res = (QueryOperand **) palloc(sizeof(QueryOperand *) * *size);

	/* Collect all operands from the tree to res */
	while ((*size)--)
	{
		if (item->type == QI_VAL)
		{
			*ptr = (QueryOperand *) item;
			ptr++;
		}
		item++;
	}

	*size = ptr - res;
	if (*size < 2)
		return res;

	qsort_arg(res, *size, sizeof(QueryOperand *), compareQueryOperand, (void *) operand);

	ptr = res + 1;
	prevptr = res;

	/* remove duplicates */
	while (ptr - res < *size)
	{
		if (compareQueryOperand((void *) ptr, (void *) prevptr, (void *) operand) != 0)
		{
			prevptr++;
			*prevptr = *ptr;
		}
		ptr++;
	}

	*size = prevptr + 1 - res;
	return res;
}

static float
calc_rank_and(const float *w, TSVector t, TSQuery q)
{
	WordEntryPosVector **pos;
	WordEntryPosVector1 posnull;
	WordEntryPosVector *POSNULL;
	int			i,
				k,
				l,
				p;
	WordEntry  *entry,
			   *firstentry;
	WordEntryPos *post,
			   *ct;
	int32		dimt,
				lenct,
				dist,
				nitem;
	float		res = -1.0;
	QueryOperand **item;
	int			size = q->size;

	item = SortAndUniqItems(q, &size);
	if (size < 2)
	{
		pfree(item);
		return calc_rank_or(w, t, q);
	}
	pos = (WordEntryPosVector **) palloc0(sizeof(WordEntryPosVector *) * q->size);

	/* A dummy WordEntryPos array to use when haspos is false */
	posnull.npos = 1;
	posnull.pos[0] = 0;
	WEP_SETPOS(posnull.pos[0], MAXENTRYPOS - 1);
	POSNULL = (WordEntryPosVector *) &posnull;

	for (i = 0; i < size; i++)
	{
		firstentry = entry = find_wordentry(t, q, item[i], &nitem);
		if (!entry)
			continue;

		while (entry - firstentry < nitem)
		{
			if (entry->haspos)
				pos[i] = _POSVECPTR(t, entry);
			else
				pos[i] = POSNULL;

			dimt = pos[i]->npos;
			post = pos[i]->pos;
			for (k = 0; k < i; k++)
			{
				if (!pos[k])
					continue;
				lenct = pos[k]->npos;
				ct = pos[k]->pos;
				for (l = 0; l < dimt; l++)
				{
					for (p = 0; p < lenct; p++)
					{
						dist = Abs((int) WEP_GETPOS(post[l]) - (int) WEP_GETPOS(ct[p]));
						if (dist || (dist == 0 && (pos[i] == POSNULL || pos[k] == POSNULL)))
						{
							float		curw;

							if (!dist)
								dist = MAXENTRYPOS;
							curw = sqrt(wpos(post[l]) * wpos(ct[p]) * word_distance(dist));
							res = (res < 0) ? curw : 1.0 - (1.0 - res) * (1.0 - curw);
						}
					}
				}
			}

			entry++;
		}
	}
	pfree(pos);
	pfree(item);
	return res;
}

static float
calc_rank_or(const float *w, TSVector t, TSQuery q)
{
	WordEntry  *entry,
			   *firstentry;
	WordEntryPosVector1 posnull;
	WordEntryPos *post;
	int32		dimt,
				j,
				i,
				nitem;
	float		res = 0.0;
	QueryOperand **item;
	int			size = q->size;

	/* A dummy WordEntryPos array to use when haspos is false */
	posnull.npos = 1;
	posnull.pos[0] = 0;

	item = SortAndUniqItems(q, &size);

	for (i = 0; i < size; i++)
	{
		float		resj,
					wjm;
		int32		jm;

		firstentry = entry = find_wordentry(t, q, item[i], &nitem);
		if (!entry)
			continue;

		while (entry - firstentry < nitem)
		{
			if (entry->haspos)
			{
				dimt = POSDATALEN(t, entry);
				post = POSDATAPTR(t, entry);
			}
			else
			{
				dimt = posnull.npos;
				post = posnull.pos;
			}

			resj = 0.0;
			wjm = -1.0;
			jm = 0;
			for (j = 0; j < dimt; j++)
			{
				resj = resj + wpos(post[j]) / ((j + 1) * (j + 1));
				if (wpos(post[j]) > wjm)
				{
					wjm = wpos(post[j]);
					jm = j;
				}
			}
/*
			limit (sum(i/i^2),i->inf) = pi^2/6
			resj = sum(wi/i^2),i=1,noccurence,
			wi - should be sorted desc,
			don't sort for now, just choose maximum weight. This should be corrected
			Oleg Bartunov
*/
			res = res + (wjm + resj - wjm / ((jm + 1) * (jm + 1))) / 1.64493406685;

			entry++;
		}
	}
	if (size > 0)
		res = res / size;
	pfree(item);
	return res;
}

static float
calc_rank(const float *w, TSVector t, TSQuery q, int32 method)
{
	QueryItem  *item = GETQUERY(q);
	float		res = 0.0;
	int			len;

	if (!t->size || !q->size)
		return 0.0;

	/* XXX: What about NOT? */
	res = (item->type == QI_OPR && (item->qoperator.oper == OP_AND ||
									item->qoperator.oper == OP_PHRASE)) ?
			calc_rank_and(w, t, q) :
			calc_rank_or(w, t, q);

	if (res < 0)
		res = 1e-20f;

	if ((method & RANK_NORM_LOGLENGTH) && t->size > 0)
		res /= log((double) (cnt_length(t) + 1)) / log(2.0);

	if (method & RANK_NORM_LENGTH)
	{
		len = cnt_length(t);
		if (len > 0)
			res /= (float) len;
	}

	/* RANK_NORM_EXTDIST not applicable */

	if ((method & RANK_NORM_UNIQ) && t->size > 0)
		res /= (float) (t->size);

	if ((method & RANK_NORM_LOGUNIQ) && t->size > 0)
		res /= log((double) (t->size + 1)) / log(2.0);

	if (method & RANK_NORM_RDIVRPLUS1)
		res /= (res + 1);

	return res;
}

Datum
rum_extract_tsvector(PG_FUNCTION_ARGS)
{
	TSVector	vector = PG_GETARG_TSVECTOR(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	Datum	   **addInfo = (Datum **) PG_GETARG_POINTER(3);
	bool	   **addInfoIsNull = (bool **) PG_GETARG_POINTER(4);
	Datum	   *entries = NULL;

	*nentries = vector->size;
	if (vector->size > 0)
	{
		int			i;
		WordEntry  *we = ARRPTR(vector);
		WordEntryPosVector *posVec;

		entries = (Datum *) palloc(sizeof(Datum) * vector->size);
		*addInfo = (Datum *) palloc(sizeof(Datum) * vector->size);
		*addInfoIsNull = (bool *) palloc(sizeof(bool) * vector->size);

		for (i = 0; i < vector->size; i++)
		{
			text	   *txt;
			bytea	   *posData;
			int			posDataSize;

			txt = cstring_to_text_with_len(STRPTR(vector) + we->pos, we->len);
			entries[i] = PointerGetDatum(txt);

			if (we->haspos)
			{
				posVec = _POSVECPTR(vector, we);
				posDataSize = VARHDRSZ + 2 * posVec->npos * sizeof(WordEntryPos);
				posData = (bytea *)palloc(posDataSize);
				posDataSize = compress_pos(posData->vl_dat, posVec->pos, posVec->npos) + VARHDRSZ;
				SET_VARSIZE(posData, posDataSize);

				(*addInfo)[i] = PointerGetDatum(posData);
				(*addInfoIsNull)[i] = false;
			}
			else
			{
				(*addInfo)[i] = (Datum)0;
				(*addInfoIsNull)[i] = true;
			}
			we++;
		}
	}

	PG_FREE_IF_COPY(vector, 0);
	PG_RETURN_POINTER(entries);
}

static const float *
getWeights(ArrayType *win)
{
	static float ws[lengthof(weights)];
	int			i;
	float4	   *arrdata;

	if (win == NULL)
		return weights;

	if (ARR_NDIM(win) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array of weight must be one-dimensional")));

	if (ArrayGetNItems(ARR_NDIM(win), ARR_DIMS(win)) < lengthof(weights))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array of weight is too short")));

	if (array_contains_nulls(win))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("array of weight must not contain nulls")));

	arrdata = (float4 *) ARR_DATA_PTR(win);
	for (i = 0; i < lengthof(weights); i++)
	{
		ws[i] = (arrdata[i] >= 0) ? arrdata[i] : weights[i];
		if (ws[i] > 1.0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("weight out of range")));
	}

	return ws;
}

Datum
rum_extract_tsquery(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);

	/* StrategyNumber strategy = PG_GETARG_UINT16(2); */
	bool	  **ptr_partialmatch = (bool **) PG_GETARG_POINTER(3);
	Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);

	/* bool   **nullFlags = (bool **) PG_GETARG_POINTER(5); */
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *entries = NULL;

	*nentries = 0;

	if (query->size > 0)
	{
		QueryItem  *item = GETQUERY(query);
		int32		i,
					j;
		bool	   *partialmatch;
		int		   *map_item_operand;
		char       *operand = GETOPERAND(query);
		QueryOperand **operands;

		/*
		 * If the query doesn't have any required positive matches (for
		 * instance, it's something like '! foo'), we have to do a full index
		 * scan.
		 */
		if (tsquery_requires_match(item))
			*searchMode = GIN_SEARCH_MODE_DEFAULT;
		else
			*searchMode = GIN_SEARCH_MODE_ALL;

		*nentries = query->size;
		operands = SortAndUniqItems(query, nentries);

		entries = (Datum *) palloc(sizeof(Datum) * (*nentries));
		partialmatch = *ptr_partialmatch = (bool *) palloc(sizeof(bool) * (*nentries));

		/*
		 * Make map to convert item's number to corresponding operand's (the
		 * same, entry's) number. Entry's number is used in check array in
		 * consistent method. We use the same map for each entry.
		 */
		*extra_data = (Pointer *) palloc(sizeof(Pointer) * (*nentries));
		map_item_operand = (int *) palloc0(sizeof(int) * query->size);

		for (i = 0; i < (*nentries); i++)
		{
			text	   *txt;

			txt = cstring_to_text_with_len(GETOPERAND(query) + operands[i]->distance,
											operands[i]->length);
			entries[i] = PointerGetDatum(txt);
			partialmatch[i] = operands[i]->prefix;
			(*extra_data)[i] = (Pointer) map_item_operand;
		}

		/* Now rescan the VAL items and fill in the arrays */
		for (j = 0; j < query->size; j++)
		{
			if (item[j].type == QI_VAL)
			{
				QueryOperand *val = &item[j].qoperand;
				bool found = false;

				for (i = 0; i < (*nentries); i++)
				{
					if (!tsCompareString(operand + operands[i]->distance, operands[i]->length,
							operand + val->distance, val->length,
							false))
					{
						map_item_operand[j] = i;
						found = true;
						break;
					}
				}

				if (!found)
					elog(ERROR, "Operand not found!");
			}
		}
	}

	PG_FREE_IF_COPY(query, 0);

	PG_RETURN_POINTER(entries);
}

Datum
rum_tsquery_distance(PG_FUNCTION_ARGS)
{
	/* bool	   *check = (bool *) PG_GETARG_POINTER(0); */

	/* StrategyNumber strategy = PG_GETARG_UINT16(1); */
	TSQuery		query = PG_GETARG_TSQUERY(2);

	int32	nkeys = PG_GETARG_INT32(3);
	/* Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	Datum	   *addInfo = (Datum *) PG_GETARG_POINTER(8);
	bool	   *addInfoIsNull = (bool *) PG_GETARG_POINTER(9);
	float8 res;

	res = 1.0 / (float8)calc_rank_pos(weights, query,
									  addInfo, addInfoIsNull, nkeys);

	PG_RETURN_FLOAT8(res);
}

Datum
rum_ts_distance(PG_FUNCTION_ARGS)
{
	TSVector	txt = PG_GETARG_TSVECTOR(0);
	TSQuery		query = PG_GETARG_TSQUERY(1);
	float		res;

	res = 1.0 / calc_rank(getWeights(NULL), txt, query, DEF_NORM_METHOD);

	PG_FREE_IF_COPY(txt, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_FLOAT4(res);
}

Datum
rum_tsvector_config(PG_FUNCTION_ARGS)
{
	RumConfig *config = (RumConfig *)PG_GETARG_POINTER(0);
	config->addInfoTypeOid = BYTEAOID;
	PG_RETURN_VOID();
}