/** \ingroup header
 * \file lib/header.c
 */

/* RPM - Copyright (C) 1995-2000 Red Hat Software */

/* Data written to file descriptors is in network byte order.    */
/* Data read from file descriptors is expected to be in          */
/* network byte order and is converted on the fly to host order. */

#include "system.h"

#define	__HEADER_PROTOTYPES__

#include <header_internal.h>

#include "debug.h"

/*@-redecl@*/	/* FIX: avoid rpmlib.h, need for debugging. */
/*@observer@*/ const char *const tagName(int tag)	/*@*/;
/*@=redecl@*/

/*@access entryInfo @*/
/*@access indexEntry @*/

/*@access extensionCache @*/
/*@access sprintfTag @*/
/*@access sprintfToken @*/
/*@access HV_t @*/

#define PARSER_BEGIN 	0
#define PARSER_IN_ARRAY 1
#define PARSER_IN_EXPR  2

/** \ingroup header
 */
static unsigned char header_magic[8] = {
	0x8e, 0xad, 0xe8, 0x01, 0x00, 0x00, 0x00, 0x00
};

/** \ingroup header
 * Alignment needs (and sizeof scalars types) for internal rpm data types.
 */
static int typeSizes[] =  { 
	0,	/*!< RPM_NULL_TYPE */
	1,	/*!< RPM_CHAR_TYPE */
	1,	/*!< RPM_INT8_TYPE */
	2,	/*!< RPM_INT16_TYPE */
	4,	/*!< RPM_INT32_TYPE */
	-1,	/*!< RPM_INT64_TYPE */
	-1,	/*!< RPM_STRING_TYPE */
	1,	/*!< RPM_BIN_TYPE */
	-1,	/*!< RPM_STRING_ARRAY_TYPE */
	-1	/*!< RPM_I18NSTRING_TYPE */
};

HV_t hdrVec;	/* forward reference */

/**
 * Wrapper to free(3), hides const compilation noise, permit NULL, return NULL.
 * @param p		memory to free
 * @return		NULL always
 */
/*@unused@*/ static inline /*@null@*/ void *
_free(/*@only@*/ /*@null@*/ const void * p) /*@modifies *p @*/
{
    if (p != NULL)	free((void *)p);
    return NULL;
}

Header headerNew()
{
    Header h = xcalloc(1, sizeof(*h));

    /*@-assignexpose@*/
    h->hv = *hdrVec;		/* structure assignment */
    /*@=assignexpose@*/
    h->indexAlloced = INDEX_MALLOC_SIZE;
    h->indexUsed = 0;
    h->flags = HEADERFLAG_SORTED;
    h->nrefs = 1;

    h->index = (h->indexAlloced
	? xcalloc(h->indexAlloced, sizeof(*h->index))
	: NULL);

    /*@-globstate@*/
    return h;
    /*@=globstate@*/
}

Header headerFree(Header h)
{
    if (h == NULL || --h->nrefs > 0)
	return NULL;	/* XXX return previous header? */

    if (h->index) {
	indexEntry entry = h->index;
	int i;
	for (i = 0; i < h->indexUsed; i++, entry++) {
	    if ((h->flags & HEADERFLAG_ALLOCATED) && ENTRY_IS_REGION(entry)) {
		if (entry->length > 0) {
		    int_32 * ei = entry->data;
		    ei -= 2; /* XXX HACK: adjust to beginning of header. */
		    ei = _free(ei);
		}
	    } else if (!ENTRY_IN_REGION(entry)) {
		entry->data = _free(entry->data);
	    }
	    entry->data = NULL;
	}
	h->index = _free(h->index);
    }

    /*@-refcounttrans@*/ h = _free(h); /*@=refcounttrans@*/
    return h;
}

Header headerLink(Header h)
{
    h->nrefs++;
    /*@-refcounttrans@*/ return h; /*@=refcounttrans@*/
}

/**
 */
static int indexCmp(const void * avp, const void * bvp)	/*@*/
{
    /*@-castexpose@*/
    indexEntry ap = (indexEntry) avp, bp = (indexEntry) bvp;
    /*@=castexpose@*/
    return (ap->info.tag - bp->info.tag);
}

void headerSort(Header h)
{
    if (!(h->flags & HEADERFLAG_SORTED)) {
	qsort(h->index, h->indexUsed, sizeof(*h->index), indexCmp);
	h->flags |= HEADERFLAG_SORTED;
    }
}

/**
 */
static int offsetCmp(const void * avp, const void * bvp) /*@*/
{
    /*@-castexpose@*/
    indexEntry ap = (indexEntry) avp, bp = (indexEntry) bvp;
    /*@=castexpose@*/
    int rc = (ap->info.offset - bp->info.offset);

    if (rc == 0)
	rc = (ap->info.tag - bp->info.tag);
    return rc;
}

void headerUnsort(Header h)
{
    qsort(h->index, h->indexUsed, sizeof(*h->index), offsetCmp);
}

unsigned int headerSizeof(Header h, enum hMagic magicp)
{
    indexEntry entry;
    unsigned int size = 0;
    unsigned int pad = 0;
    int i;

    if (h == NULL)
	return size;

    headerSort(h);

    switch (magicp) {
    case HEADER_MAGIC_YES:
	size += sizeof(header_magic);
	break;
    case HEADER_MAGIC_NO:
	break;
    }

    size += 2 * sizeof(int_32);	/* count of index entries */

    for (i = 0, entry = h->index; i < h->indexUsed; i++, entry++) {
	unsigned diff;
	int_32 type;

	/* Regions go in as is ... */
        if (ENTRY_IS_REGION(entry)) {
	    size += entry->length;
	    /* XXX Legacy regions do not include the region tag and data. */
	    if (i == 0 && (h->flags & HEADERFLAG_LEGACY))
		size += sizeof(struct entryInfo) + entry->info.count;
	    continue;
        }

	/* ... and region elements are skipped. */
	if (entry->info.offset < 0)
	    continue;

	/* Alignment */
	type = entry->info.type;
	if (typeSizes[type] > 1) {
	    diff = typeSizes[type] - (size % typeSizes[type]);
	    if (diff != typeSizes[type]) {
		size += diff;
		pad += diff;
	    }
	}

	size += sizeof(struct entryInfo) + entry->length;
    }

    return size;
}

/**
 * Return length of entry data.
 * @param type		entry data type
 * @param p		entry data
 * @param count		entry item count
 * @param onDisk	data is concatenated strings (with NUL's))?
 * @return		no. bytes in data
 */
/*@mayexit@*/
static int dataLength(int_32 type, hPTR_t p, int_32 count, int onDisk)
	/*@modifies fileSystem @*/
{
    int length = 0;

    switch (type) {
    case RPM_STRING_TYPE:
	if (count == 1) {	/* Special case -- p is just the string */
	    length = strlen(p) + 1;
	    break;
	}
        /* This should not be allowed */
	fprintf(stderr, _("dataLength() RPM_STRING_TYPE count must be 1.\n"));
	exit(EXIT_FAILURE);
	/*@notreached@*/ break;

    case RPM_STRING_ARRAY_TYPE:
    case RPM_I18NSTRING_TYPE:
    {	int i;

	/* This is like RPM_STRING_TYPE, except it's *always* an array */
	/* Compute sum of length of all strings, including null terminators */
	i = count;

	if (onDisk) {
	    const char * chptr = p;
	    int thisLen;

	    while (i--) {
		thisLen = strlen(chptr) + 1;
		length += thisLen;
		chptr += thisLen;
	    }
	} else {
	    const char ** src = (const char **)p;
	    while (i--) {
		/* add one for null termination */
		length += strlen(*src++) + 1;
	    }
	}
    }	break;

    default:
	if (typeSizes[type] != -1) {
	    length = typeSizes[type] * count;
	    break;
	}
	fprintf(stderr, _("Data type %d not supported\n"), (int) type);
	exit(EXIT_FAILURE);
	/*@notreached@*/ break;
    }

    return length;
}

/** \ingroup header
 * Swap int_32 and int_16 arrays within header region.
 *
 * This code is way more twisty than I would like.
 *
 * A bug with RPM_I18NSTRING_TYPE in rpm-2.5.x (fixed in August 1998)
 * causes the offset and length of elements in a header region to disagree
 * regarding the total length of the region data.
 *
 * The "fix" is to compute the size using both offset and length and
 * return the larger of the two numbers as the size of the region.
 * Kinda like computing left and right Riemann sums of the data elements
 * to determine the size of a data structure, go figger :-).
 *
 * There's one other twist if a header region tag is in the set to be swabbed,
 * as the data for a header region is located after all other tag data.
 *
 * @param entry		header entry
 * @param il		no. of entries
 * @param dl		start no. bytes of data
 * @param pe		header physical entry pointer (swapped)
 * @param dataStart	header data
 * @param regionid	region offset
 * @return		no. bytes of data in region, -1 on error
 */
static int regionSwab(/*@null@*/ indexEntry entry, int il, int dl,
		entryInfo pe, char * dataStart, int regionid)
	/*@modifies *entry, *dataStart @*/
{
    char * tprev = NULL;
    char * t = NULL;
    int tdel, tl = dl;

    for (; il > 0; il--, pe++) {
	struct indexEntry ie;
	int_32 type;

	ie.info.tag = ntohl(pe->tag);
	ie.info.type = ntohl(pe->type);
	if (ie.info.type < RPM_MIN_TYPE || ie.info.type > RPM_MAX_TYPE)
	    return -1;
	ie.info.count = ntohl(pe->count);
	ie.info.offset = ntohl(pe->offset);
	ie.data = t = dataStart + ie.info.offset;
	ie.length = dataLength(ie.info.type, ie.data, ie.info.count, 1);
	ie.rdlen = 0;

	if (entry) {
	    ie.info.offset = regionid;
	    *entry = ie;	/* structure assignment */
	    entry++;
	}

	/* Alignment */
	type = ie.info.type;
	if (typeSizes[type] > 1) {
	    unsigned diff;
	    diff = typeSizes[type] - (dl % typeSizes[type]);
	    if (diff != typeSizes[type]) {
		dl += diff;
	    }
	}
	tdel = (tprev ? (t - tprev) : 0);
	dl += ie.length;
	tl += tdel;
	tprev = (ie.info.tag < HEADER_I18NTABLE)
		? dataStart : t;

	/* Perform endian conversions */
	switch (ntohl(pe->type)) {
	case RPM_INT32_TYPE:
	{   int_32 * it = (int_32 *)t;
	    for (; ie.info.count > 0; ie.info.count--, it += 1)
		*it = htonl(*it);
	    t = (char *) it;
	}   break;
	case RPM_INT16_TYPE:
	{   int_16 * it = (int_16 *) t;
	    for (; ie.info.count > 0; ie.info.count--, it += 1)
		*it = htons(*it);
	    t = (char *) it;
	}   break;
	default:
	    t += ie.length;
	    break;
	}
    }
    tdel = (tprev ? (t - tprev) : 0);
    tl += tdel;
    if (tl > dl)
	dl = tl;
    return dl;
}

#if 0
int headerDrips(const Header h)
{
    indexEntry entry; 
    int i;

    for (i = 0, entry = h->index; i < h->indexUsed; i++, entry++) {
	if (ENTRY_IS_REGION(entry)) {
	    int rid = entry->info.offset;

	    for (; i < h->indexUsed && entry->info.offset <= rid+1; i++, entry++) {
		if (entry->info.offset <= rid)
		    continue;
	    }
	    i--;
	    entry--;
	    continue;
	}

	/* Ignore deleted drips. */
	if (entry->data == NULL || entry->length <= 0)
	    continue;
    }
    return 0;
}
#endif

/** \ingroup header
 */
static /*@only@*/ /*@null@*/ void * doHeaderUnload(Header h,
		/*@out@*/ int * lengthPtr)
	/*@modifies h, *lengthPtr @*/
{
    int_32 * ei = NULL;
    entryInfo pe;
    char * dataStart;
    char * te;
    unsigned pad;
    unsigned len;
    int_32 il = 0;
    int_32 dl = 0;
    indexEntry entry; 
    int_32 type;
    int i;
    int drlen, ndribbles;
    int driplen, ndrips;
    int legacy = 0;

    /* Sort entries by (offset,tag). */
    headerUnsort(h);

    /* Compute (il,dl) for all tags, including those deleted in region. */
    pad = 0;
    drlen = ndribbles = driplen = ndrips = 0;
    for (i = 0, entry = h->index; i < h->indexUsed; i++, entry++) {
	if (ENTRY_IS_REGION(entry)) {
	    int_32 rdl = -entry->info.offset;	/* negative offset */
	    int_32 ril = rdl/sizeof(*pe);
	    int rid = entry->info.offset;

	    il += ril;
	    dl += entry->rdlen + entry->info.count;
	    /* XXX Legacy regions do not include the region tag and data. */
	    if (i == 0 && (h->flags & HEADERFLAG_LEGACY))
		il += 1;

	    /* Skip rest of entries in region, but account for dribbles. */
	    for (; i < h->indexUsed && entry->info.offset <= rid+1; i++, entry++) {
		if (entry->info.offset <= rid)
		    continue;

		/* Alignment */
		type = entry->info.type;
		if (typeSizes[type] > 1) {
		    unsigned diff;
		    diff = typeSizes[type] - (dl % typeSizes[type]);
		    if (diff != typeSizes[type]) {
			drlen += diff;
			pad += diff;
			dl += diff;
		    }
		}

		ndribbles++;
		il++;
		drlen += entry->length;
		dl += entry->length;
	    }
	    i--;
	    entry--;
	    continue;
	}

	/* Ignore deleted drips. */
	if (entry->data == NULL || entry->length <= 0)
	    continue;

	/* Alignment */
	type = entry->info.type;
	if (typeSizes[type] > 1) {
	    unsigned diff;
	    diff = typeSizes[type] - (dl % typeSizes[type]);
	    if (diff != typeSizes[type]) {
		driplen += diff;
		pad += diff;
		dl += diff;
	    } else
		diff = 0;
	}

	ndrips++;
	il++;
	driplen += entry->length;
	dl += entry->length;
    }
    len = sizeof(il) + sizeof(dl) + (il * sizeof(*pe)) + dl;

    ei = xmalloc(len);
    ei[0] = htonl(il);
    ei[1] = htonl(dl);

    pe = (entryInfo) &ei[2];
    dataStart = te = (char *) (pe + il);

    pad = 0;
    for (i = 0, entry = h->index; i < h->indexUsed; i++, entry++) {
	const char * src;
char *t;
	int count;
	int rdlen;

	if (entry->data == NULL || entry->length <= 0)
	    continue;

t = te;
	pe->tag = htonl(entry->info.tag);
	pe->type = htonl(entry->info.type);
	pe->count = htonl(entry->info.count);

	if (ENTRY_IS_REGION(entry)) {
	    int_32 rdl = -entry->info.offset;	/* negative offset */
	    int_32 ril = rdl/sizeof(*pe) + ndribbles;
	    int rid = entry->info.offset;

	    src = (char *)entry->data;
	    rdlen = entry->rdlen;

	    /* XXX Legacy regions do not include the region tag and data. */
	    if (i == 0 && (h->flags & HEADERFLAG_LEGACY)) {
		int_32 stei[4];

		legacy = 1;
		memcpy(pe+1, src, rdl);
		memcpy(te, src + rdl, rdlen);
		te += rdlen;

		pe->offset = htonl(te - dataStart);
		stei[0] = pe->tag;
		stei[1] = pe->type;
		stei[2] = htonl(-rdl-entry->info.count);
		stei[3] = pe->count;
		memcpy(te, stei, entry->info.count);
		te += entry->info.count;
		ril++;
		rdlen += entry->info.count;

		count = regionSwab(NULL, ril, 0, pe, t, 0);
		if (count != rdlen) goto errxit;

	    } else {

		memcpy(pe+1, src + sizeof(*pe), ((ril-1) * sizeof(*pe)));
		memcpy(te, src + (ril * sizeof(*pe)), rdlen+entry->info.count+drlen);
		te += rdlen;
		{   /*@-castexpose@*/
		    entryInfo se = (entryInfo)src;
		    /*@=castexpose@*/
		    int off = ntohl(se->offset);
		    pe->offset = (off) ? htonl(te - dataStart) : htonl(off);
		}
		te += entry->info.count + drlen;

		count = regionSwab(NULL, ril, 0, pe, t, 0);
		if (count != (rdlen + entry->info.count + drlen)) goto errxit;
	    }

	    /* Skip rest of entries in region. */
	    while (i < h->indexUsed && entry->info.offset <= rid+1) {
		i++;
		entry++;
	    }
	    i--;
	    entry--;
	    pe += ril;
	    continue;
	}

	/* Ignore deleted drips. */
	if (entry->data == NULL || entry->length <= 0)
	    continue;

	/* Alignment */
	type = entry->info.type;
	if (typeSizes[type] > 1) {
	    unsigned diff;
	    diff = typeSizes[type] - ((te - dataStart) % typeSizes[type]);
	    if (diff != typeSizes[type]) {
		memset(te, 0, diff);
		te += diff;
		pad += diff;
	    }
	}

	pe->offset = htonl(te - dataStart);

	/* copy data w/ endian conversions */
	switch (entry->info.type) {
	case RPM_INT32_TYPE:
	    count = entry->info.count;
	    src = entry->data;
	    while (count--) {
		*((int_32 *)te) = htonl(*((int_32 *)src));
		te += sizeof(int_32);
		src += sizeof(int_32);
	    }
	    break;

	case RPM_INT16_TYPE:
	    count = entry->info.count;
	    src = entry->data;
	    while (count--) {
		*((int_16 *)te) = htons(*((int_16 *)src));
		te += sizeof(int_16);
		src += sizeof(int_16);
	    }
	    break;

	default:
	    memcpy(te, entry->data, entry->length);
	    te += entry->length;
	    break;
	}
	pe++;
    }
   
    /* Insure that there are no memcpy underruns/overruns. */
    if (((char *)pe) != dataStart) goto errxit;
    if ((((char *)ei)+len) != te) goto errxit;

    if (lengthPtr)
	*lengthPtr = len;

    h->flags &= ~HEADERFLAG_SORTED;
    headerSort(h);

    return (void *) ei;

errxit:
    /*@-usereleased@*/
    ei = _free(ei);
    /*@=usereleased@*/
    return (void *) ei;
}

void * headerUnload(Header h)
{
    int length;
    void * uh = doHeaderUnload(h, &length);
    return uh;
}

Header headerReload(Header h, int tag)
{
    Header nh;
    int length;
    /*@-onlytrans@*/
    void * uh = doHeaderUnload(h, &length);

    h = headerFree(h);
    /*@=onlytrans@*/
    if (uh == NULL)
	return NULL;
    nh = headerLoad(uh);
    if (nh == NULL) {
	uh = _free(uh);
	return NULL;
    }
    if (nh->flags & HEADERFLAG_ALLOCATED)
	uh = _free(uh);
    nh->flags |= HEADERFLAG_ALLOCATED;
    if (ENTRY_IS_REGION(nh->index)) {
	if (tag == HEADER_SIGNATURES || tag == HEADER_IMMUTABLE)
	    nh->index[0].info.tag = tag;
    }
    return nh;
}

Header headerCopy(Header h)
{
    Header nh = headerNew();
    HeaderIterator hi;
    int_32 tag, type, count;
    hPTR_t ptr;
   
    for (hi = headerInitIterator(h);
	headerNextIterator(hi, &tag, &type, &ptr, &count);
	ptr = headerFreeData((void *)ptr, type))
    {
	if (ptr) (void) headerAddEntry(nh, tag, type, ptr, count);
    }
    hi = headerFreeIterator(hi);

    return headerReload(nh, HEADER_IMAGE);
}

Header headerLoad(void * uh)
{
    int_32 * ei = (int_32 *) uh;
    int_32 il = ntohl(ei[0]);		/* index length */
    int_32 dl = ntohl(ei[1]);		/* data length */
    int pvlen = sizeof(il) + sizeof(dl) +
               (il * sizeof(struct entryInfo)) + dl;
    void * pv = uh;
    Header h = xcalloc(1, sizeof(*h));
    entryInfo pe;
    char * dataStart;
    indexEntry entry; 
    int rdlen;
    int i;

    ei = (int_32 *) pv;
    /*@-castexpose@*/
    pe = (entryInfo) &ei[2];
    /*@=castexpose@*/
    dataStart = (char *) (pe + il);

    /*@-assignexpose@*/
    h->hv = *hdrVec;		/* structure assignment */
    /*@=assignexpose@*/
    h->indexAlloced = il + 1;
    h->indexUsed = il;
    h->index = xcalloc(h->indexAlloced, sizeof(*h->index));
    h->flags = HEADERFLAG_SORTED;
    h->nrefs = 1;

    /*
     * XXX XFree86-libs, ash, and pdksh from Red Hat 5.2 have bogus
     * %verifyscript tag that needs to be diddled.
     */
    if (ntohl(pe->tag) == 15 &&
	ntohl(pe->type) == RPM_STRING_TYPE &&
	ntohl(pe->count) == 1)
    {
	pe->tag = htonl(1079);
    }

    entry = h->index;
    i = 0;
    if (!(htonl(pe->tag) < HEADER_I18NTABLE)) {
	h->flags |= HEADERFLAG_LEGACY;
	entry->info.type = REGION_TAG_TYPE;
	entry->info.tag = HEADER_IMAGE;
	entry->info.count = REGION_TAG_COUNT;
	entry->info.offset = ((char *)pe - dataStart); /* negative offset */

	/*@-assignexpose@*/
	entry->data = pe;
	/*@=assignexpose@*/
	entry->length = pvlen - sizeof(il) - sizeof(dl);
	rdlen = regionSwab(entry+1, il, 0, pe, dataStart, entry->info.offset);
	if (rdlen != dl) goto errxit;
	entry->rdlen = rdlen;
	entry++;
	h->indexUsed++;
    } else {
	int nb = ntohl(pe->count);
	int_32 rdl;
	int_32 ril;

	h->flags &= ~HEADERFLAG_LEGACY;

	entry->info.type = htonl(pe->type);
	if (entry->info.type < RPM_MIN_TYPE || entry->info.type > RPM_MAX_TYPE)
	    goto errxit;
	entry->info.count = htonl(pe->count);

	{  int off = ntohl(pe->offset);
	   if (off) {
		int_32 * stei = memcpy(alloca(nb), dataStart + off, nb);
		rdl = -ntohl(stei[2]);	/* negative offset */
		ril = rdl/sizeof(*pe);
		entry->info.tag = htonl(pe->tag);
	    } else {
		ril = il;
		rdl = (ril * sizeof(struct entryInfo));
		entry->info.tag = HEADER_IMAGE;
	    }
	}
	entry->info.offset = -rdl;	/* negative offset */

	/*@-assignexpose@*/
	entry->data = pe;
	/*@=assignexpose@*/
	entry->length = pvlen - sizeof(il) - sizeof(dl);
	rdlen = regionSwab(entry+1, ril-1, 0, pe+1, dataStart, entry->info.offset);
	if (rdlen < 0) goto errxit;
	entry->rdlen = rdlen;

	if (ril < h->indexUsed) {
	    indexEntry newEntry = entry + ril;
	    int ne = (h->indexUsed - ril);
	    int rid = entry->info.offset+1;
	    int rc;

	    /* Load dribble entries from region. */
	    rc = regionSwab(newEntry, ne, 0, pe+ril, dataStart, rid);
	    if (rc < 0) goto errxit;
	    rdlen += rc;

	  { indexEntry firstEntry = newEntry;
	    int save = h->indexUsed;
	    int j;

	    /* Dribble entries replace duplicate region entries. */
	    h->indexUsed -= ne;
	    for (j = 0; j < ne; j++, newEntry++) {
		(void) headerRemoveEntry(h, newEntry->info.tag);
		if (newEntry->info.tag == HEADER_BASENAMES)
		    (void) headerRemoveEntry(h, HEADER_OLDFILENAMES);
	    }

	    /* If any duplicate entries were replaced, move new entries down. */
	    if (h->indexUsed < (save - ne)) {
		memmove(h->index + h->indexUsed, firstEntry,
			(ne * sizeof(*entry)));
	    }
	    h->indexUsed += ne;
	  }
	}
    }

    h->flags &= ~HEADERFLAG_SORTED;
    headerSort(h);

    /*@-globstate@*/
    return h;
    /*@=globstate@*/

errxit:
    /*@-usereleased@*/
    if (h) {
	h->index = _free(h->index);
	/*@-refcounttrans@*/
	h = _free(h);
	/*@=refcounttrans@*/
    }
    /*@=usereleased@*/
    /*@-refcounttrans -globstate@*/
    return h;
    /*@=refcounttrans =globstate@*/
}

Header headerCopyLoad(const void * uh)
{
    int_32 * ei = (int_32 *) uh;
    int_32 il = ntohl(ei[0]);		/* index length */
    int_32 dl = ntohl(ei[1]);		/* data length */
    int pvlen = sizeof(il) + sizeof(dl) +
               (il * sizeof(struct entryInfo)) + dl;
    void * nuh = memcpy(xmalloc(pvlen), uh, pvlen);
    Header h;

    h = headerLoad(nuh);
    if (h == NULL) {
	nuh = _free(nuh);
	return h;
    }
    h->flags |= HEADERFLAG_ALLOCATED;
    return h;
}

Header headerRead(FD_t fd, enum hMagic magicp)
{
    int_32 block[4];
    int_32 reserved;
    int_32 * ei = NULL;
    int_32 il;
    int_32 dl;
    int_32 magic;
    Header h = NULL;
    int len;
    int i;

    memset(block, 0, sizeof(block));
    i = 2;
    if (magicp == HEADER_MAGIC_YES)
	i += 2;

    if (timedRead(fd, (char *)block, i*sizeof(*block)) != (i * sizeof(*block)))
	goto exit;

    i = 0;

    if (magicp == HEADER_MAGIC_YES) {
	magic = block[i++];
	if (memcmp(&magic, header_magic, sizeof(magic)))
	    goto exit;
	reserved = block[i++];
    }
    
    il = ntohl(block[i++]);
    dl = ntohl(block[i++]);

    len = sizeof(il) + sizeof(dl) + (il * sizeof(struct entryInfo)) + dl;

    /*
     * XXX Limit total size of header to 32Mb (~16 times largest known size).
     */
    if (len > (32*1024*1024))
	goto exit;

    ei = xmalloc(len);
    ei[0] = htonl(il);
    ei[1] = htonl(dl);
    len -= sizeof(il) + sizeof(dl);

    if (timedRead(fd, (char *)&ei[2], len) != len)
	goto exit;
    
    h = headerLoad(ei);

exit:
    if (h) {
	if (h->flags & HEADERFLAG_ALLOCATED)
	    ei = _free(ei);
	h->flags |= HEADERFLAG_ALLOCATED;
    } else if (ei)
	ei = _free(ei);
    /*@-mustmod@*/	/* FIX: timedRead macro obscures annotation */
    return h;
    /*@-mustmod@*/
}

int headerWrite(FD_t fd, Header h, enum hMagic magicp)
{
    ssize_t nb;
    int length;
    const void * uh;

    if (h == NULL)
	return 1;
    uh = doHeaderUnload(h, &length);
    if (uh == NULL)
	return 1;
    switch (magicp) {
    case HEADER_MAGIC_YES:
	nb = Fwrite(header_magic, sizeof(char), sizeof(header_magic), fd);
	if (nb != sizeof(header_magic))
	    goto exit;
	break;
    case HEADER_MAGIC_NO:
	break;
    }

    nb = Fwrite(uh, sizeof(char), length, fd);

exit:
    uh = _free(uh);
    return (nb == length ? 0 : 1);
}

/**
 * Find matching (tag,type) entry in header.
 * @param h		header
 * @param tag		entry tag
 * @param type		entry type
 * @return 		header entry
 */
static /*@null@*/
indexEntry findEntry(/*@null@*/ Header h, int_32 tag, int_32 type)
	/*@modifies h @*/
{
    indexEntry entry, entry2, last;
    struct indexEntry key;

    if (h == NULL) return NULL;
    if (!(h->flags & HEADERFLAG_SORTED)) headerSort(h);

    key.info.tag = tag;

    entry2 = entry = 
	bsearch(&key, h->index, h->indexUsed, sizeof(*h->index), indexCmp);
    if (entry == NULL)
	return NULL;

    if (type == RPM_NULL_TYPE)
	return entry;

    /* look backwards */
    while (entry->info.tag == tag && entry->info.type != type &&
	   entry > h->index) entry--;

    if (entry->info.tag == tag && entry->info.type == type)
	return entry;

    last = h->index + h->indexUsed;
    while (entry2->info.tag == tag && entry2->info.type != type &&
	   entry2 < last) entry2++;

    if (entry->info.tag == tag && entry->info.type == type)
	return entry;

    return NULL;
}

int headerIsEntry(Header h, int_32 tag)
{
    /*@-mods@*/		/*@ FIX: h modified by sort. */
    return (findEntry(h, tag, RPM_NULL_TYPE) ? 1 : 0);
    /*@=mods@*/	
}

/** \ingroup header
 * Retrieve data from header entry.
 * @todo Permit retrieval of regions other than HEADER_IMUTABLE.
 * @param entry		header entry
 * @retval type		address of type (or NULL)
 * @retval p		address of data (or NULL)
 * @retval c		address of count (or NULL)
 * @param minMem	string pointers refer to header memory?
 * @return		1 on success, otherwise error.
 */
static int copyEntry(const indexEntry entry,
		/*@null@*/ /*@out@*/ hTYP_t type,
		/*@null@*/ /*@out@*/ hPTR_t * p,
		/*@null@*/ /*@out@*/ hCNT_t c,
		int minMem)
	/*@modifies *type, *p, *c @*/
{
    int_32 count = entry->info.count;
    int rc = 1;		/* XXX 1 on success. */

    if (p)
    switch (entry->info.type) {
    case RPM_BIN_TYPE:
	/* XXX this only works for HEADER_IMMUTABLE */
	if (ENTRY_IS_REGION(entry)) {
	    int_32 * ei = ((int_32 *)entry->data) - 2;
	    /*@-castexpose@*/
	    entryInfo pe = (entryInfo) (ei + 2);
	    /*@=castexpose@*/
	    char * dataStart = (char *) (pe + ntohl(ei[0]));
	    int_32 rdl = -entry->info.offset;	/* negative offset */
	    int_32 ril = rdl/sizeof(*pe);

	    count = 2 * sizeof(*ei) + (ril * sizeof(*pe)) +
			entry->rdlen + REGION_TAG_COUNT;
	    *p = xmalloc(count);
	    ei = (int_32 *) *p;
	    ei[0] = htonl(ril);
	    ei[1] = htonl(entry->rdlen + REGION_TAG_COUNT);
	    /*@-castexpose@*/
	    pe = (entryInfo) memcpy(ei + 2, pe, (ril * sizeof(*pe)));
	    /*@=castexpose@*/
	    dataStart = (char *) memcpy(pe + ril, dataStart,
					(entry->rdlen + REGION_TAG_COUNT));

	    rc = regionSwab(NULL, ril, 0, pe, dataStart, 0);
	    /* XXX 1 on success. */
	    rc = (rc < 0) ? 0 : 1;
	} else {
	    count = entry->length;
	    *p = (!minMem
		? memcpy(xmalloc(count), entry->data, count)
		: entry->data);
	}
	break;
    case RPM_STRING_TYPE:
	if (count == 1) {
	    *p = entry->data;
	    break;
	}
	/*@fallthrough@*/
    case RPM_STRING_ARRAY_TYPE:
    case RPM_I18NSTRING_TYPE:
    {	const char ** ptrEntry;
	int tableSize = count * sizeof(char *);
	char * t;
	int i;

	/*@-mods@*/
	if (minMem) {
	    *p = xmalloc(tableSize);
	    ptrEntry = (const char **) *p;
	    t = entry->data;
	} else {
	    t = xmalloc(tableSize + entry->length);
	    *p = (void *)t;
	    ptrEntry = (const char **) *p;
	    t += tableSize;
	    memcpy(t, entry->data, entry->length);
	}
	/*@=mods@*/
	for (i = 0; i < count; i++) {
	    *ptrEntry++ = t;
	    t = strchr(t, 0);
	    t++;
	}
    }	break;

    default:
	*p = entry->data;
	break;
    }
    if (type) *type = entry->info.type;
    if (c) *c = count;
    return rc;
}

/**
 * Does locale match entry in header i18n table?
 * 
 * \verbatim
 * The range [l,le) contains the next locale to match:
 *    ll[_CC][.EEEEE][@dddd]
 * where
 *    ll	ISO language code (in lowercase).
 *    CC	(optional) ISO coutnry code (in uppercase).
 *    EEEEE	(optional) encoding (not really standardized).
 *    dddd	(optional) dialect.
 * \endverbatim
 *
 * @param td		header i18n table data, NUL terminated
 * @param l		start of locale	to match
 * @param le		end of locale to match
 * @return		1 on match, 0 on no match
 */
static int headerMatchLocale(const char *td, const char *l, const char *le)
	/*@*/
{
    const char *fe;


#if 0
  { const char *s, *ll, *CC, *EE, *dd;
    char *lbuf, *t.

    /* Copy the buffer and parse out components on the fly. */
    lbuf = alloca(le - l + 1);
    for (s = l, ll = t = lbuf; *s; s++, t++) {
	switch (*s) {
	case '_':
	    *t = '\0';
	    CC = t + 1;
	    break;
	case '.':
	    *t = '\0';
	    EE = t + 1;
	    break;
	case '@':
	    *t = '\0';
	    dd = t + 1;
	    break;
	default:
	    *t = *s;
	    break;
	}
    }

    if (ll)	/* ISO language should be lower case */
	for (t = ll; *t; t++)	*t = tolower(*t);
    if (CC)	/* ISO country code should be upper case */
	for (t = CC; *t; t++)	*t = toupper(*t);

    /* There are a total of 16 cases to attempt to match. */
  }
#endif

    /* First try a complete match. */
    if (strlen(td) == (le-l) && !strncmp(td, l, (le - l)))
	return 1;

    /* Next, try stripping optional dialect and matching.  */
    for (fe = l; fe < le && *fe != '@'; fe++)
	{};
    if (fe < le && !strncmp(td, l, (fe - l)))
	return 1;

    /* Next, try stripping optional codeset and matching.  */
    for (fe = l; fe < le && *fe != '.'; fe++)
	{};
    if (fe < le && !strncmp(td, l, (fe - l)))
	return 1;

    /* Finally, try stripping optional country code and matching. */
    for (fe = l; fe < le && *fe != '_'; fe++)
	{};
    if (fe < le && !strncmp(td, l, (fe - l)))
	return 1;

    return 0;
}

/**
 * Return i18n string from header that matches locale.
 * @param h		header
 * @param entry		i18n string data
 * @return		matching i18n string (or 1st string if no match)
 */
/*@dependent@*/ static char *
headerFindI18NString(Header h, indexEntry entry)
{
    const char *lang, *l, *le;
    indexEntry table;

    /* XXX Drepper sez' this is the order. */
    if ((lang = getenv("LANGUAGE")) == NULL &&
	(lang = getenv("LC_ALL")) == NULL &&
        (lang = getenv("LC_MESSAGES")) == NULL &&
	(lang = getenv("LANG")) == NULL)
	    /*@-retalias -retexpose@*/
	    return entry->data;
	    /*@=retalias =retexpose@*/
    
    if ((table = findEntry(h, HEADER_I18NTABLE, RPM_STRING_ARRAY_TYPE)) == NULL)
	/*@-retalias -retexpose@*/
	return entry->data;
	/*@=retalias =retexpose@*/

    for (l = lang; *l != '\0'; l = le) {
	const char *td;
	char *ed;
	int langNum;

	while (*l && *l == ':')			/* skip leading colons */
	    l++;
	if (*l == '\0')
	    break;
	for (le = l; *le && *le != ':'; le++)	/* find end of this locale */
	    {};

	/* For each entry in the header ... */
	for (langNum = 0, td = table->data, ed = entry->data;
	     langNum < entry->info.count;
	     langNum++, td += strlen(td) + 1, ed += strlen(ed) + 1) {

		if (headerMatchLocale(td, l, le))
		    return ed;

	}
    }

    /*@-retalias -retexpose@*/
    return entry->data;
    /*@=retalias =retexpose@*/
}

/**
 * Retrieve tag data from header.
 * @param h		header
 * @param tag		tag to retrieve
 * @retval type		address of type (or NULL)
 * @retval p		address of data (or NULL)
 * @retval c		address of count (or NULL)
 * @param minMem	string pointers reference header memory?
 * @return		1 on success, 0 on not found
 */
static int intGetEntry(Header h, int_32 tag,
		/*@null@*/ /*@out@*/ hTAG_t type,
		/*@null@*/ /*@out@*/ hPTR_t * p,
		/*@null@*/ /*@out@*/ hCNT_t c,
		int minMem)
	/*@modifies *type, *p, *c @*/
{
    indexEntry entry;
    int rc;

    /* First find the tag */
    /*@-mods@*/		/*@ FIX: h modified by sort. */
    entry = findEntry(h, tag, RPM_NULL_TYPE);
    /*@mods@*/
    if (entry == NULL) {
	if (type) type = 0;
	if (p) *p = NULL;
	if (c) *c = 0;
	return 0;
    }

    switch (entry->info.type) {
    case RPM_I18NSTRING_TYPE:
	rc = 1;
	if (type) *type = RPM_STRING_TYPE;
	if (c) *c = 1;
	/*@-dependenttrans@*/
	if (p) *p = headerFindI18NString(h, entry);
	/*@=dependenttrans@*/
	break;
    default:
	rc = copyEntry(entry, type, p, c, minMem);
	break;
    }

    /* XXX 1 on success */
    return ((rc == 1) ? 1 : 0);
}

/** \ingroup header
 * Free data allocated when retrieved from header.
 * @param h		header
 * @param data		address of data (or NULL)
 * @param type		type of data (or -1 to force free)
 * @return		NULL always
 */
static /*@null@*/ void * headerFreeTag(/*@unused@*/ Header h,
		/*@only@*/ /*@null@*/ const void * data, rpmTagType type)
	/*@modifies data @*/
{
    if (data) {
	/*@-branchstate@*/
	if (type == -1 ||
	    type == RPM_STRING_ARRAY_TYPE ||
	    type == RPM_I18NSTRING_TYPE ||
	    type == RPM_BIN_TYPE)
		data = _free(data);
	/*@=branchstate@*/
    }
    return NULL;
}

int headerGetEntry(Header h, int_32 tag, hTYP_t type, void **p, hCNT_t c)
{
    return intGetEntry(h, tag, type, (hPTR_t *)p, c, 0);
}

int headerGetEntryMinMemory(Header h, int_32 tag, hTYP_t type, hPTR_t * p, 
			    hCNT_t c)
{
    return intGetEntry(h, tag, type, p, c, 1);
}

int headerGetRawEntry(Header h, int_32 tag, int_32 * type, hPTR_t * p,
		int_32 * c)
{
    indexEntry entry;
    int rc;

    if (p == NULL) return headerIsEntry(h, tag);

    /* First find the tag */
    /*@-mods@*/		/*@ FIX: h modified by sort. */
    entry = findEntry(h, tag, RPM_NULL_TYPE);
    /*@=mods@*/
    if (!entry) {
	if (p) *p = NULL;
	if (c) *c = 0;
	return 0;
    }

    rc = copyEntry(entry, type, p, c, 0);

    /* XXX 1 on success */
    return ((rc == 1) ? 1 : 0);
}

/**
 */
static void copyData(int_32 type, /*@out@*/ void * dstPtr, const void * srcPtr,
		int_32 c, int dataLength)
	/*@modifies *dstPtr @*/
{
    const char ** src;
    char * dst;
    int i;

    switch (type) {
    case RPM_STRING_ARRAY_TYPE:
    case RPM_I18NSTRING_TYPE:
	/* Otherwise, p is char** */
	i = c;
	src = (const char **) srcPtr;
	dst = dstPtr;
	while (i--) {
	    if (*src) {
		int len = strlen(*src) + 1;
		memcpy(dst, *src, len);
		dst += len;
	    }
	    src++;
	}
	break;

    default:
	memmove(dstPtr, srcPtr, dataLength);
	break;
    }
}

/**
 * Return (malloc'ed) copy of entry data.
 * @param type		entry data type
 * @param p		entry data
 * @param c		entry item count
 * @retval lengthPtr	no. bytes in returned data
 * @return 		(malloc'ed) copy of entry data
 */
static void * grabData(int_32 type, hPTR_t p, int_32 c,
		/*@out@*/ int * lengthPtr)
	/*@modifies *lengthPtr @*/
{
    int length = dataLength(type, p, c, 0);
    void * data = xmalloc(length);

    copyData(type, data, p, c, length);

    if (lengthPtr)
	*lengthPtr = length;
    return data;
}

int headerAddEntry(Header h, int_32 tag, int_32 type, hPTR_t p, int_32 c)
{
    indexEntry entry;

    /* Count must always be >= 1 for headerAddEntry. */
    if (c <= 0)
	return 0;

    /* Allocate more index space if necessary */
    if (h->indexUsed == h->indexAlloced) {
	h->indexAlloced += INDEX_MALLOC_SIZE;
	h->index = xrealloc(h->index, h->indexAlloced * sizeof(*h->index));
    }

    /* Fill in the index */
    entry = h->index + h->indexUsed;
    entry->info.tag = tag;
    entry->info.type = type;
    entry->info.count = c;
    entry->info.offset = 0;
    entry->data = grabData(type, p, c, &entry->length);

    if (h->indexUsed > 0 && tag < h->index[h->indexUsed-1].info.tag)
	h->flags &= ~HEADERFLAG_SORTED;
    h->indexUsed++;

    return 1;
}

int headerAppendEntry(Header h, int_32 tag, int_32 type,
			hPTR_t p, int_32 c)
{
    indexEntry entry;
    int length;

    /* First find the tag */
    entry = findEntry(h, tag, type);
    if (!entry)
	return 0;

    if (type == RPM_STRING_TYPE || type == RPM_I18NSTRING_TYPE) {
	/* we can't do this */
	return 0;
    }

    length = dataLength(type, p, c, 0);

    if (ENTRY_IN_REGION(entry)) {
	char * t = xmalloc(entry->length + length);
	memcpy(t, entry->data, entry->length);
	entry->data = t;
	entry->info.offset = 0;
    } else
	entry->data = xrealloc(entry->data, entry->length + length);

    copyData(type, ((char *) entry->data) + entry->length, p, c, length);

    entry->length += length;

    entry->info.count += c;

    return 1;
}

int headerAddOrAppendEntry(Header h, int_32 tag, int_32 type,
			   hPTR_t p, int_32 c)
{
    return (findEntry(h, tag, type)
	? headerAppendEntry(h, tag, type, p, c)
	: headerAddEntry(h, tag, type, p, c));
}

int headerAddI18NString(Header h, int_32 tag, const char * string, const char * lang)
{
    indexEntry table, entry;
    const char ** strArray;
    int length;
    int ghosts;
    int i, langNum;
    char * buf;

    table = findEntry(h, HEADER_I18NTABLE, RPM_STRING_ARRAY_TYPE);
    entry = findEntry(h, tag, RPM_I18NSTRING_TYPE);

    if (!table && entry)
	return 0;		/* this shouldn't ever happen!! */

    if (!table && !entry) {
	const char * charArray[2];
	int count = 0;
	if (!lang || (lang[0] == 'C' && lang[1] == '\0')) {
	    /*@-observertrans -readonlytrans@*/
	    charArray[count++] = "C";
	    /*@=observertrans =readonlytrans@*/
	} else {
	    /*@-observertrans -readonlytrans@*/
	    charArray[count++] = "C";
	    /*@=observertrans =readonlytrans@*/
	    charArray[count++] = lang;
	}
	if (!headerAddEntry(h, HEADER_I18NTABLE, RPM_STRING_ARRAY_TYPE, 
			&charArray, count))
	    return 0;
	table = findEntry(h, HEADER_I18NTABLE, RPM_STRING_ARRAY_TYPE);
    }

    if (!table)
	return 0;
    if (!lang) lang = "C";

    {	const char * l = table->data;
	for (langNum = 0; langNum < table->info.count; langNum++) {
	    if (!strcmp(l, lang)) break;
	    l += strlen(l) + 1;
	}
    }

    if (langNum >= table->info.count) {
	length = strlen(lang) + 1;
	if (ENTRY_IN_REGION(table)) {
	    char * t = xmalloc(table->length + length);
	    memcpy(t, table->data, table->length);
	    table->data = t;
	    table->info.offset = 0;
	} else
	    table->data = xrealloc(table->data, table->length + length);
	memmove(((char *)table->data) + table->length, lang, length);
	table->length += length;
	table->info.count++;
    }

    if (!entry) {
	strArray = alloca(sizeof(*strArray) * (langNum + 1));
	for (i = 0; i < langNum; i++)
	    strArray[i] = "";
	strArray[langNum] = string;
	return headerAddEntry(h, tag, RPM_I18NSTRING_TYPE, strArray, 
				langNum + 1);
    } else if (langNum >= entry->info.count) {
	ghosts = langNum - entry->info.count;
	
	length = strlen(string) + 1 + ghosts;
	if (ENTRY_IN_REGION(entry)) {
	    char * t = xmalloc(entry->length + length);
	    memcpy(t, entry->data, entry->length);
	    entry->data = t;
	    entry->info.offset = 0;
	} else
	    entry->data = xrealloc(entry->data, entry->length + length);

	memset(((char *)entry->data) + entry->length, '\0', ghosts);
	memmove(((char *)entry->data) + entry->length + ghosts, string, strlen(string));

	entry->length += length;
	entry->info.count = langNum + 1;
    } else {
	char *b, *be, *e, *ee, *t;
	size_t bn, sn, en;

	/* Set beginning/end pointers to previous data */
	b = be = e = ee = entry->data;
	for (i = 0; i < table->info.count; i++) {
	    if (i == langNum)
		be = ee;
	    ee += strlen(ee) + 1;
	    if (i == langNum)
		e  = ee;
	}

	/* Get storage for new buffer */
	bn = (be-b);
	sn = strlen(string) + 1;
	en = (ee-e);
	length = bn + sn + en;
	t = buf = xmalloc(length);

	/* Copy values into new storage */
	memcpy(t, b, bn);
	t += bn;
	memcpy(t, string, sn);
	t += sn;
	memcpy(t, e, en);
	t += en;

	/* Replace I18N string array */
	entry->length -= strlen(be) + 1;
	entry->length += sn;
	
	if (ENTRY_IN_REGION(entry)) {
	    entry->info.offset = 0;
	} else
	    entry->data = _free(entry->data);
	/*@-dependenttrans@*/
	entry->data = buf;
	/*@=dependenttrans@*/
    }

    return 0;
}

int headerModifyEntry(Header h, int_32 tag, int_32 type, hPTR_t p, int_32 c)
{
    indexEntry entry;
    void * oldData;

    /* First find the tag */
    entry = findEntry(h, tag, type);
    if (!entry)
	return 0;

    /* make sure entry points to the first occurence of this tag */
    while (entry > h->index && (entry - 1)->info.tag == tag)  
	entry--;

    /* free after we've grabbed the new data in case the two are intertwined;
       that's a bad idea but at least we won't break */
    oldData = entry->data;

    entry->info.count = c;
    entry->info.type = type;
    entry->data = grabData(type, p, c, &entry->length);

    if (ENTRY_IN_REGION(entry)) {
	entry->info.offset = 0;
    } else
	oldData = _free(oldData);

    return 1;
}

int headerRemoveEntry(Header h, int_32 tag)
{
    indexEntry last = h->index + h->indexUsed;
    indexEntry entry, first;
    int ne;

    entry = findEntry(h, tag, RPM_NULL_TYPE);
    if (!entry) return 1;

    /* Make sure entry points to the first occurence of this tag. */
    while (entry > h->index && (entry - 1)->info.tag == tag)  
	entry--;

    /* Free data for tags being removed. */
    for (first = entry; first < last; first++) {
	void * data;
	if (first->info.tag != tag)
	    break;
	data = first->data;
	first->data = NULL;
	first->length = 0;
	if (ENTRY_IN_REGION(first))
	    continue;
	data = _free(data);
    }

    ne = (first - entry);
    if (ne > 0) {
	h->indexUsed -= ne;
	ne = last - first;
	if (ne > 0)
	    memmove(entry, first, (ne * sizeof(*entry)));
    }

    return 0;
}

/**
 */
static char escapedChar(const char ch)	/*@*/
{
    switch (ch) {
    case 'a': 	return '\a';
    case 'b': 	return '\b';
    case 'f': 	return '\f';
    case 'n': 	return '\n';
    case 'r': 	return '\r';
    case 't': 	return '\t';
    case 'v': 	return '\v';
    default:	return ch;
    }
}

/**
 * Destroy headerSprintf format array.
 * @param format	sprintf format array
 * @param num		number of elements
 * @return		NULL always
 */
static /*@null@*/ sprintfToken
freeFormat( /*@only@*/ /*@null@*/ sprintfToken format, int num)
	/*@modifies *format @*/
{
    int i;

    if (format == NULL) return NULL;
    for (i = 0; i < num; i++) {
	switch (format[i].type) {
	case PTOK_ARRAY:
	    format[i].u.array.format =
		freeFormat(format[i].u.array.format,
			format[i].u.array.numTokens);
	    break;
	case PTOK_COND:
	    format[i].u.cond.ifFormat =
		freeFormat(format[i].u.cond.ifFormat, 
			format[i].u.cond.numIfTokens);
	    format[i].u.cond.elseFormat =
		freeFormat(format[i].u.cond.elseFormat, 
			format[i].u.cond.numElseTokens);
	    break;
	case PTOK_NONE:
	case PTOK_TAG:
	case PTOK_STRING:
	default:
	    break;
	}
    }
    format = _free(format);
    return NULL;
}

/**
 */
static void findTag(char * name, const headerTagTableEntry tags, 
		    const headerSprintfExtension extensions,
		    /*@out@*/ headerTagTableEntry * tagMatch,
		    /*@out@*/ headerSprintfExtension * extMatch)
	/*@modifies *tagMatch, *extMatch @*/
{
    headerTagTableEntry entry;
    headerSprintfExtension ext;
    const char * tagname;

    *tagMatch = NULL;
    *extMatch = NULL;

    if (strncmp("RPMTAG_", name, sizeof("RPMTAG_")-1)) {
	char * t = alloca(strlen(name) + sizeof("RPMTAG_"));
	(void) stpcpy( stpcpy(t, "RPMTAG_"), name);
	tagname = t;
    } else {
	tagname = name;
    }

    /* Search extensions first to permit overriding header tags. */
    ext = extensions;
    while (ext->type != HEADER_EXT_LAST) {
	if (ext->name != NULL && ext->type == HEADER_EXT_TAG
	&& !xstrcasecmp(ext->name, tagname))
	    break;

	if (ext->type == HEADER_EXT_MORE)
	    ext = ext->u.more;
	else
	    ext++;
    }

    if (ext->type == HEADER_EXT_TAG) {
	*extMatch = ext;
	return;
    }

    /* Search header tags. */
    for (entry = tags; entry->name; entry++)
	if (entry->name && !xstrcasecmp(entry->name, tagname))
	    break;

    if (entry->name) {
	*tagMatch = entry;
	return;
    }
}

/* forward ref */
static int parseExpression(sprintfToken token, char * str, 
		const headerTagTableEntry tags, 
		const headerSprintfExtension extensions,
		/*@out@*/char ** endPtr, /*@null@*/ /*@out@*/ errmsg_t * errmsg)
	/*@modifies str, *str, *token, *endPtr, *errmsg @*/;

/**
 */
static int parseFormat(char * str, const headerTagTableEntry tags,
		const headerSprintfExtension extensions,
		/*@out@*/sprintfToken * formatPtr, /*@out@*/int * numTokensPtr,
		/*@null@*/ /*@out@*/ char ** endPtr, int state,
		/*@null@*/ /*@out@*/ errmsg_t * errmsg)
	/*@modifies str, *str, *formatPtr, *numTokensPtr, *endPtr, *errmsg @*/
{
    char * chptr, * start, * next, * dst;
    sprintfToken format;
    int numTokens;
    int currToken;
    headerTagTableEntry tag;
    headerSprintfExtension ext;
    int i;
    int done = 0;

    /* upper limit on number of individual formats */
    numTokens = 0;
    for (chptr = str; *chptr != '\0'; chptr++)
	if (*chptr == '%') numTokens++;
    numTokens = numTokens * 2 + 1;

    format = xcalloc(numTokens, sizeof(*format));
    if (endPtr) *endPtr = NULL;

    /*@-infloops@*/
    dst = start = str;
    currToken = -1;
    while (*start != '\0') {
	switch (*start) {
	case '%':
	    /* handle %% */
	    if (*(start + 1) == '%') {
		if (currToken < 0 || format[currToken].type != PTOK_STRING) {
		    currToken++;
		    format[currToken].type = PTOK_STRING;
		    /*@-temptrans -assignexpose@*/
		    dst = format[currToken].u.string.string = start;
		    /*@=temptrans =assignexpose@*/
		}

		start++;

		*dst++ = *start++;

		break; /* out of switch */
	    } 

	    currToken++;
	    *dst++ = '\0';
	    start++;

	    if (*start == '|') {
		char * newEnd;

		start++;
		if (parseExpression(format + currToken, start, tags, 
				    extensions, &newEnd, errmsg))
		{
		    format = freeFormat(format, numTokens);
		    return 1;
		}
		start = newEnd;
		break; /* out of switch */
	    }

	    /*@-assignexpose@*/
	    format[currToken].u.tag.format = start;
	    /*@=assignexpose@*/
	    format[currToken].u.tag.pad = 0;
	    format[currToken].u.tag.justOne = 0;
	    format[currToken].u.tag.arrayCount = 0;

	    chptr = start;
	    while (*chptr && *chptr != '{' && *chptr != '%') chptr++;
	    if (!*chptr || *chptr == '%') {
		/*@-observertrans -readonlytrans@*/
		if (errmsg) *errmsg = _("missing { after %");
		/*@=observertrans =readonlytrans@*/
		format = freeFormat(format, numTokens);
		return 1;
	    }

	    *chptr++ = '\0';

	    while (start < chptr) {
		if (xisdigit(*start)) {
		    i = strtoul(start, &start, 10);
		    format[currToken].u.tag.pad += i;
		} else {
		    start++;
		}
	    }

	    if (*start == '=') {
		format[currToken].u.tag.justOne = 1;
		start++;
	    } else if (*start == '#') {
		format[currToken].u.tag.justOne = 1;
		format[currToken].u.tag.arrayCount = 1;
		start++;
	    }

	    next = start;
	    while (*next && *next != '}') next++;
	    if (!*next) {
		/*@-observertrans -readonlytrans@*/
		if (errmsg) *errmsg = _("missing } after %{");
		/*@=observertrans =readonlytrans@*/
		format = freeFormat(format, numTokens);
		return 1;
	    }
	    *next++ = '\0';

	    chptr = start;
	    while (*chptr && *chptr != ':') chptr++;

	    if (*chptr != '\0') {
		*chptr++ = '\0';
		if (!*chptr) {
		    /*@-observertrans -readonlytrans@*/
		    if (errmsg) *errmsg = _("empty tag format");
		    /*@=observertrans =readonlytrans@*/
		    format = freeFormat(format, numTokens);
		    return 1;
		}
		/*@-assignexpose@*/
		format[currToken].u.tag.type = chptr;
		/*@=assignexpose@*/
	    } else {
		format[currToken].u.tag.type = NULL;
	    }
	    
	    if (!*start) {
		/*@-observertrans -readonlytrans@*/
		if (errmsg) *errmsg = _("empty tag name");
		/*@=observertrans =readonlytrans@*/
		format = freeFormat(format, numTokens);
		return 1;
	    }

	    i = 0;
	    findTag(start, tags, extensions, &tag, &ext);

	    if (tag) {
		format[currToken].u.tag.ext = NULL;
		format[currToken].u.tag.tag = tag->val;
	    } else if (ext) {
		format[currToken].u.tag.ext = ext->u.tagFunction;
		format[currToken].u.tag.extNum = ext - extensions;
	    } else {
		/*@-observertrans -readonlytrans@*/
		if (errmsg) *errmsg = _("unknown tag");
		/*@=observertrans =readonlytrans@*/
		format = freeFormat(format, numTokens);
		return 1;
	    }

	    format[currToken].type = PTOK_TAG;

	    start = next;

	    break;

	case '[':
	    *dst++ = '\0';
	    *start++ = '\0';
	    currToken++;

	    if (parseFormat(start, tags, extensions, 
			    &format[currToken].u.array.format,
			    &format[currToken].u.array.numTokens,
			    &start, PARSER_IN_ARRAY, errmsg)) {
		format = freeFormat(format, numTokens);
		return 1;
	    }

	    if (!start) {
		/*@-observertrans -readonlytrans@*/
		if (errmsg) *errmsg = _("] expected at end of array");
		/*@=observertrans =readonlytrans@*/
		format = freeFormat(format, numTokens);
		return 1;
	    }

	    dst = start;

	    format[currToken].type = PTOK_ARRAY;

	    break;

	case ']':
	case '}':
	    if ((*start == ']' && state != PARSER_IN_ARRAY) ||
	        (*start == '}' && state != PARSER_IN_EXPR)) {
		if (*start == ']') {
		    /*@-observertrans -readonlytrans@*/
		    if (errmsg) *errmsg = _("unexpected ]");
		    /*@=observertrans =readonlytrans@*/
		} else {
		    /*@-observertrans -readonlytrans@*/
		    if (errmsg) *errmsg = _("unexpected }");
		    /*@=observertrans =readonlytrans@*/
		}
		format = freeFormat(format, numTokens);
		return 1;
	    }
	    *start++ = '\0';
	    if (endPtr) *endPtr = start;
	    done = 1;
	    break;

	default:
	    if (currToken < 0 || format[currToken].type != PTOK_STRING) {
		currToken++;
		format[currToken].type = PTOK_STRING;
		/*@-temptrans -assignexpose@*/
		dst = format[currToken].u.string.string = start;
		/*@=temptrans =assignexpose@*/
	    }

	    if (*start == '\\') {
		start++;
		*dst++ = escapedChar(*start++);
	    } else {
		*dst++ = *start++;
	    }
	    break;
	}
	if (done)
	    break;
    }
    /*@=infloops@*/

    *dst = '\0';

    currToken++;
    for (i = 0; i < currToken; i++) {
	if (format[i].type == PTOK_STRING)
	    format[i].u.string.len = strlen(format[i].u.string.string);
    }

    *numTokensPtr = currToken;
    *formatPtr = format;

    return 0;
}

/**
 */
static int parseExpression(sprintfToken token, char * str, 
		const headerTagTableEntry tags, 
		const headerSprintfExtension extensions,
		/*@out@*/ char ** endPtr,
		/*@null@*/ /*@out@*/ errmsg_t * errmsg)
{
    headerTagTableEntry tag;
    headerSprintfExtension ext;
    char * chptr;
    char * end;

    if (errmsg) *errmsg = NULL;
    chptr = str;
    while (*chptr && *chptr != '?') chptr++;

    if (*chptr != '?') {
	/*@-observertrans -readonlytrans@*/
	if (errmsg) *errmsg = _("? expected in expression");
	/*@=observertrans =readonlytrans@*/
	return 1;
    }

    *chptr++ = '\0';;

    if (*chptr != '{') {
	/*@-observertrans -readonlytrans@*/
	if (errmsg) *errmsg = _("{ expected after ? in expression");
	/*@=observertrans =readonlytrans@*/
	return 1;
    }

    chptr++;

    if (parseFormat(chptr, tags, extensions, &token->u.cond.ifFormat, 
		    &token->u.cond.numIfTokens, &end, PARSER_IN_EXPR, errmsg)) 
	return 1;

    if (!*end) {
	/*@-observertrans -readonlytrans@*/
	if (errmsg) *errmsg = _("} expected in expression");
	/*@=observertrans =readonlytrans@*/
	token->u.cond.ifFormat =
		freeFormat(token->u.cond.ifFormat, token->u.cond.numIfTokens);
	return 1;
    }

    chptr = end;
    if (*chptr != ':' && *chptr != '|') {
	/*@-observertrans -readonlytrans@*/
	if (errmsg) *errmsg = _(": expected following ? subexpression");
	/*@=observertrans =readonlytrans@*/
	token->u.cond.ifFormat =
		freeFormat(token->u.cond.ifFormat, token->u.cond.numIfTokens);
	return 1;
    }

    if (*chptr == '|') {
	(void) parseFormat(xstrdup(""), tags, extensions,
			&token->u.cond.elseFormat, 
			&token->u.cond.numElseTokens, &end, PARSER_IN_EXPR, 
			errmsg);
    } else {
	chptr++;

	if (*chptr != '{') {
	    /*@-observertrans -readonlytrans@*/
	    if (errmsg) *errmsg = _("{ expected after : in expression");
	    /*@=observertrans =readonlytrans@*/
	    token->u.cond.ifFormat =
		freeFormat(token->u.cond.ifFormat, token->u.cond.numIfTokens);
	    return 1;
	}

	chptr++;

	if (parseFormat(chptr, tags, extensions, &token->u.cond.elseFormat, 
			&token->u.cond.numElseTokens, &end, PARSER_IN_EXPR, 
			errmsg)) 
	    return 1;
	if (!*end) {
	    /*@-observertrans -readonlytrans@*/
	    if (errmsg) *errmsg = _("} expected in expression");
	    /*@=observertrans =readonlytrans@*/
	    token->u.cond.ifFormat =
		freeFormat(token->u.cond.ifFormat, token->u.cond.numIfTokens);
	    return 1;
	}

	chptr = end;
	if (*chptr != '|') {
	    /*@-observertrans -readonlytrans@*/
	    if (errmsg) *errmsg = _("| expected at end of expression");
	    /*@=observertrans =readonlytrans@*/
	    token->u.cond.ifFormat =
		freeFormat(token->u.cond.ifFormat, token->u.cond.numIfTokens);
	    token->u.cond.elseFormat =
		freeFormat(token->u.cond.elseFormat, token->u.cond.numElseTokens);
	    return 1;
	}
    }
	
    chptr++;

    *endPtr = chptr;

    findTag(str, tags, extensions, &tag, &ext);

    if (tag) {
	token->u.cond.tag.ext = NULL;
	token->u.cond.tag.tag = tag->val;
    } else if (ext) {
	token->u.cond.tag.ext = ext->u.tagFunction;
	token->u.cond.tag.extNum = ext - extensions;
    } else {
	token->u.cond.tag.ext = NULL;
	token->u.cond.tag.tag = -1;
    }
	
    token->type = PTOK_COND;

    return 0;
}

/**
 * @return		0 on success, 1 on failure
 */
static int getExtension(Header h, headerTagTagFunction fn,
		/*@out@*/ hTYP_t typeptr,
		/*@out@*/ hPTR_t * data,
		/*@out@*/ hCNT_t countptr,
		extensionCache ext)
	/*@modifies *typeptr, *data, *countptr, ext @*/
{
    if (!ext->avail) {
	if (fn(h, &ext->type, &ext->data, &ext->count, &ext->freeit))
	    return 1;
	ext->avail = 1;
    }

    if (typeptr) *typeptr = ext->type;
    if (data) *data = ext->data;
    if (countptr) *countptr = ext->count;

    return 0;
}

/**
 */
static char * formatValue(sprintfTag tag, Header h, 
		const headerSprintfExtension extensions,
		extensionCache extCache, int element)
	/*@modifies extCache @*/
{
    int len;
    char buf[20];
    int_32 count, type;
    hPTR_t data;
    unsigned int intVal;
    char * val = NULL;
    const char ** strarray;
    int mayfree = 0;
    int countBuf;
    headerTagFormatFunction tagtype = NULL;
    headerSprintfExtension ext;

    if (tag->ext) {
	if (getExtension(h, tag->ext, &type, &data, &count, 
			 extCache + tag->extNum))
	{
	    count = 1;
	    type = RPM_STRING_TYPE;	
	    data = "(none)";		/* XXX i18n? NO!, sez; gafton */
	}
    } else {
	if (!headerGetEntry(h, tag->tag, &type, (void **)&data, &count)) {
	    count = 1;
	    type = RPM_STRING_TYPE;	
	    data = "(none)";		/* XXX i18n? NO!, sez; gafton */
	}

	mayfree = 1;
    }

    if (tag->arrayCount) {
	/*@-observertrans -modobserver@*/
	data = headerFreeData(data, type);
	/*@=observertrans =modobserver@*/

	countBuf = count;
	data = &countBuf;
	count = 1;
	type = RPM_INT32_TYPE;
    }

    (void) stpcpy( stpcpy(buf, "%"), tag->format);

    if (tag->type) {
	ext = extensions;
	while (ext->type != HEADER_EXT_LAST) {
	    if (ext->name != NULL && ext->type == HEADER_EXT_FORMAT
	    && !strcmp(ext->name, tag->type))
	    {
		tagtype = ext->u.formatFunction;
		break;
	    }

	    if (ext->type == HEADER_EXT_MORE)
		ext = ext->u.more;
	    else
		ext++;
	}
    }
    
    switch (type) {
    case RPM_STRING_ARRAY_TYPE:
	strarray = (const char **)data;

	if (tagtype)
	    val = tagtype(RPM_STRING_TYPE, strarray[element], buf, tag->pad, 0);

	if (!val) {
	    strcat(buf, "s");

	    len = strlen(strarray[element]) + tag->pad + 20;
	    val = xmalloc(len);
	    sprintf(val, buf, strarray[element]);
	}

	/*@-observertrans -modobserver@*/
	if (mayfree) data = _free(data);
	/*@=observertrans =modobserver@*/

	break;

    case RPM_STRING_TYPE:
	if (tagtype)
	    val = tagtype(RPM_STRING_ARRAY_TYPE, data, buf, tag->pad,  0);

	if (!val) {
	    strcat(buf, "s");

	    len = strlen(data) + tag->pad + 20;
	    val = xmalloc(len);
	    sprintf(val, buf, data);
	}
	break;

    case RPM_CHAR_TYPE:
    case RPM_INT8_TYPE:
    case RPM_INT16_TYPE:
    case RPM_INT32_TYPE:
	switch (type) {
	case RPM_CHAR_TYPE:	
	case RPM_INT8_TYPE:	intVal = *(((int_8 *) data) + element); break;
	case RPM_INT16_TYPE:	intVal = *(((uint_16 *) data) + element); break;
	default:		/* keep -Wall quiet */
	case RPM_INT32_TYPE:	intVal = *(((int_32 *) data) + element); break;
	}

	if (tagtype)
	    val = tagtype(RPM_INT32_TYPE, &intVal, buf, tag->pad,  element);

	if (!val) {
	    strcat(buf, "d");
	    len = 10 + tag->pad + 20;
	    val = xmalloc(len);
	    sprintf(val, buf, intVal);
	}
	break;

    default:
	val = xstrdup(_("(unknown type)"));
	break;
    }

    return val;
}

/**
 */
static const char * singleSprintf(Header h, sprintfToken token,
		const headerSprintfExtension extensions,
		extensionCache extCache, int element)
	/*@modifies h, extCache @*/
{
    char * val;
    const char * thisItem;
    int thisItemLen;
    int len, alloced;
    int i, j;
    int numElements;
    int type;
    sprintfToken condFormat;
    int condNumFormats;

    /* we assume the token and header have been validated already! */

    switch (token->type) {
    case PTOK_NONE:
	break;

    case PTOK_STRING:
	val = xmalloc(token->u.string.len + 1);
	strcpy(val, token->u.string.string);
	break;

    case PTOK_TAG:
	val = formatValue(&token->u.tag, h, extensions, extCache,
			  token->u.tag.justOne ? 0 : element);
	break;

    case PTOK_COND:
	if (token->u.cond.tag.ext ||
	    headerIsEntry(h, token->u.cond.tag.tag)) {
	    condFormat = token->u.cond.ifFormat;
	    condNumFormats = token->u.cond.numIfTokens;
	} else {
	    condFormat = token->u.cond.elseFormat;
	    condNumFormats = token->u.cond.numElseTokens;
	}

	alloced = condNumFormats * 20;
	val = xmalloc(alloced ? alloced : 1);
	*val = '\0';
	len = 0;

	if (condFormat)
	for (i = 0; i < condNumFormats; i++) {
	    thisItem = singleSprintf(h, condFormat + i, 
				     extensions, extCache, element);
	    thisItemLen = strlen(thisItem);
	    if ((thisItemLen + len) >= alloced) {
		alloced = (thisItemLen + len) + 200;
		val = xrealloc(val, alloced);	
	    }
	    strcat(val, thisItem);
	    len += thisItemLen;
	    thisItem = _free(thisItem);
	}

	break;

    case PTOK_ARRAY:
	numElements = -1;
	for (i = 0; i < token->u.array.numTokens; i++) {
	    if (token->u.array.format[i].type != PTOK_TAG ||
		token->u.array.format[i].u.tag.arrayCount ||
		token->u.array.format[i].u.tag.justOne) continue;

	    if (token->u.array.format[i].u.tag.ext) {
		const void * data;
		if (getExtension(h, token->u.array.format[i].u.tag.ext,
				 &type, &data, &numElements, 
				 extCache + 
				   token->u.array.format[i].u.tag.extNum))
		     continue;
	    } else {
		if (!headerGetEntry(h, token->u.array.format[i].u.tag.tag, 
				    &type, (void **) &val, &numElements))
		    continue;
		val = headerFreeData(val, type);
	    } 
	    /*@loopbreak@*/ break;
	}

	if (numElements == -1) {
	    val = xstrdup("(none)");	/* XXX i18n? NO!, sez; gafton */
	} else {
	    alloced = numElements * token->u.array.numTokens * 20;
	    val = xmalloc(alloced);
	    *val = '\0';
	    len = 0;

	    for (j = 0; j < numElements; j++) {
		for (i = 0; i < token->u.array.numTokens; i++) {
		    thisItem = singleSprintf(h, token->u.array.format + i, 
					     extensions, extCache, j);
		    thisItemLen = strlen(thisItem);
		    if ((thisItemLen + len) >= alloced) {
			alloced = (thisItemLen + len) + 200;
			val = xrealloc(val, alloced);	
		    }
		    strcat(val, thisItem);
		    len += thisItemLen;
		    thisItem = _free(thisItem);
		}
	    }
	}
	   
	break;
    }

    return val;
}

/**
 */
static /*@only@*/ extensionCache
allocateExtensionCache(const headerSprintfExtension extensions)
	/*@*/
{
    headerSprintfExtension ext = extensions;
    int i = 0;

    while (ext->type != HEADER_EXT_LAST) {
	i++;
	if (ext->type == HEADER_EXT_MORE)
	    ext = ext->u.more;
	else
	    ext++;
    }

    return xcalloc(i, sizeof(struct extensionCache));
}

/**
 * @return		NULL always
 */
static /*@null@*/ extensionCache
freeExtensionCache(const headerSprintfExtension extensions,
		        /*@only@*/ extensionCache cache)
	/*@*/
{
    headerSprintfExtension ext = extensions;
    int i = 0;

    while (ext->type != HEADER_EXT_LAST) {
	if (cache[i].freeit) cache[i].data = _free(cache[i].data);

	i++;
	if (ext->type == HEADER_EXT_MORE)
	    ext = ext->u.more;
	else
	    ext++;
    }

    cache = _free(cache);
    return NULL;
}

char * headerSprintf(Header h, const char * fmt, 
		const struct headerTagTableEntry * tabletags,
		const struct headerSprintfExtension * extensions,
		errmsg_t * errmsg)
{
    /*@-castexpose@*/	/* FIX: legacy API shouldn't change. */
    headerSprintfExtension exts = (headerSprintfExtension) extensions;
    headerTagTableEntry tags = (headerTagTableEntry) tabletags;
    /*@=castexpose@*/
    char * fmtString;
    sprintfToken format;
    int numTokens;
    char * answer;
    int answerLength;
    int answerAlloced;
    int i;
    extensionCache extCache;
 
    /*fmtString = escapeString(fmt);*/
    fmtString = xstrdup(fmt);
   
    if (parseFormat(fmtString, tags, exts, &format, &numTokens, 
		    NULL, PARSER_BEGIN, errmsg)) {
	fmtString = _free(fmtString);
	return NULL;
    }

    extCache = allocateExtensionCache(exts);

    answerAlloced = 1024;
    answerLength = 0;
    answer = xmalloc(answerAlloced);
    *answer = '\0';

    for (i = 0; i < numTokens; i++) {
	const char * piece;
	int pieceLength;

	/*@-mods@*/
	piece = singleSprintf(h, format + i, exts, extCache, 0);
	/*@=mods@*/
	if (piece) {
	    pieceLength = strlen(piece);
	    if ((answerLength + pieceLength) >= answerAlloced) {
		while ((answerLength + pieceLength) >= answerAlloced) 
		    answerAlloced += 1024;
		answer = xrealloc(answer, answerAlloced);
	    }

	    strcat(answer, piece);
	    answerLength += pieceLength;
	    piece = _free(piece);
	}
    }

    fmtString = _free(fmtString);
    extCache = freeExtensionCache(exts, extCache);
    format = _free(format);

    return answer;
}

/**
 */
static char * octalFormat(int_32 type, hPTR_t data, 
		char * formatPrefix, int padding, /*@unused@*/int element)
	/*@modifies formatPrefix @*/
{
    char * val;

    if (type != RPM_INT32_TYPE) {
	val = xstrdup(_("(not a number)"));
    } else {
	val = xmalloc(20 + padding);
	strcat(formatPrefix, "o");
	sprintf(val, formatPrefix, *((int_32 *) data));
    }

    return val;
}

/**
 */
static char * hexFormat(int_32 type, hPTR_t data, 
		char * formatPrefix, int padding, /*@unused@*/int element)
	/*@modifies formatPrefix @*/
{
    char * val;

    if (type != RPM_INT32_TYPE) {
	val = xstrdup(_("(not a number)"));
    } else {
	val = xmalloc(20 + padding);
	strcat(formatPrefix, "x");
	sprintf(val, formatPrefix, *((int_32 *) data));
    }

    return val;
}

/**
 */
static char * realDateFormat(int_32 type, hPTR_t data, 
		char * formatPrefix, int padding, /*@unused@*/int element,
		const char * strftimeFormat)
	/*@modifies formatPrefix @*/
{
    char * val;

    if (type != RPM_INT32_TYPE) {
	val = xstrdup(_("(not a number)"));
    } else {
	struct tm * tstruct;
	char buf[50];

	val = xmalloc(50 + padding);
	strcat(formatPrefix, "s");

	/* this is important if sizeof(int_32) ! sizeof(time_t) */
	{   time_t dateint = *((int_32 *) data);
	    tstruct = localtime(&dateint);
	}
	buf[0] = '\0';
	if (tstruct)
	    (void) strftime(buf, sizeof(buf) - 1, strftimeFormat, tstruct);
	sprintf(val, formatPrefix, buf);
    }

    return val;
}

/**
 */
static char * dateFormat(int_32 type, hPTR_t data, 
		         char * formatPrefix, int padding, int element)
	/*@modifies formatPrefix @*/
{
    return realDateFormat(type, data, formatPrefix, padding, element, "%c");
}

/**
 */
static char * dayFormat(int_32 type, hPTR_t data, 
		         char * formatPrefix, int padding, int element)
	/*@modifies formatPrefix @*/
{
    return realDateFormat(type, data, formatPrefix, padding, element, 
			  "%a %b %d %Y");
}

/**
 */
static char * shescapeFormat(int_32 type, hPTR_t data, 
		char * formatPrefix, int padding, /*@unused@*/int element)
	/*@modifies formatPrefix @*/
{
    char * result, * dst, * src, * buf;

    if (type == RPM_INT32_TYPE) {
	result = xmalloc(padding + 20);
	strcat(formatPrefix, "d");
	sprintf(result, formatPrefix, *((int_32 *) data));
    } else {
	buf = alloca(strlen(data) + padding + 2);
	strcat(formatPrefix, "s");
	sprintf(buf, formatPrefix, data);

	result = dst = xmalloc(strlen(buf) * 4 + 3);
	*dst++ = '\'';
	for (src = buf; *src != '\0'; src++) {
	    if (*src == '\'') {
		*dst++ = '\'';
		*dst++ = '\\';
		*dst++ = '\'';
		*dst++ = '\'';
	    } else {
		*dst++ = *src;
	    }
	}
	*dst++ = '\'';
	*dst = '\0';

    }

    return result;
}

const struct headerSprintfExtension headerDefaultFormats[] = {
    { HEADER_EXT_FORMAT, "octal", { octalFormat } },
    { HEADER_EXT_FORMAT, "hex", { hexFormat } },
    { HEADER_EXT_FORMAT, "date", { dateFormat } },
    { HEADER_EXT_FORMAT, "day", { dayFormat } },
    { HEADER_EXT_FORMAT, "shescape", { shescapeFormat } },
    { HEADER_EXT_LAST, NULL, { NULL } }
};

void headerCopyTags(Header headerFrom, Header headerTo, hTAG_t tagstocopy)
{
    int * p;

    if (headerFrom == headerTo)
	return;

    for (p = tagstocopy; *p != 0; p++) {
	char *s;
	int_32 type;
	int_32 count;
	if (headerIsEntry(headerTo, *p))
	    continue;
	if (!headerGetEntryMinMemory(headerFrom, *p, &type,
				(hPTR_t *) &s, &count))
	    continue;
	(void) headerAddEntry(headerTo, *p, type, s, count);
	s = headerFreeData(s, type);
    }
}

/**
 * Header tag iterator data structure.
 */
struct headerIteratorS {
/*@unused@*/ Header h;		/*!< Header being iterated. */
/*@unused@*/ int next_index;	/*!< Next tag index. */
};

HeaderIterator headerFreeIterator(HeaderIterator hi)
{
    hi->h = headerFree(hi->h);
    hi = _free(hi);
    return hi;
}

HeaderIterator headerInitIterator(Header h)
{
    HeaderIterator hi = xmalloc(sizeof(struct headerIteratorS));

    headerSort(h);

    hi->h = headerLink(h);
    hi->next_index = 0;
    return hi;
}

int headerNextIterator(HeaderIterator hi,
		 hTAG_t tag, hTYP_t type, hPTR_t * p, hCNT_t c)
{
    Header h = hi->h;
    int slot = hi->next_index;
    indexEntry entry = NULL;
    int rc;

    for (slot = hi->next_index; slot < h->indexUsed; slot++) {
	entry = h->index + slot;
	if (!ENTRY_IS_REGION(entry))
	    break;
    }
    hi->next_index = slot;
    if (entry == NULL || slot >= h->indexUsed)
	return 0;
    /*@-noeffect@*/	/* LCL: no clue */
    hi->next_index++;
    /*@=noeffect@*/

    if (tag)
	*tag = entry->info.tag;

    rc = copyEntry(entry, type, p, c, 0);

    /* XXX 1 on success */
    return ((rc == 1) ? 1 : 0);
}
static struct HV_s hdrVec1 = {
    headerNew,
    headerFree,
    headerLink,
    headerSort,
    headerUnsort,
    headerSizeof,
    headerUnload,
    headerReload,
    headerCopy,
    headerLoad,
    headerCopyLoad,
    headerRead,
    headerWrite,
    headerIsEntry,
    headerFreeTag,
    headerGetEntry,
    headerGetEntryMinMemory,
    headerAddEntry,
    headerAppendEntry,
    headerAddOrAppendEntry,
    headerAddI18NString,
    headerModifyEntry,
    headerRemoveEntry,
    headerSprintf,
    headerCopyTags,
    headerFreeIterator,
    headerInitIterator,
    headerNextIterator,
    NULL, NULL,
    1
};

/*@-compmempass -redef@*/
HV_t hdrVec = &hdrVec1;
/*@=compmempass =redef@*/
