/*@-bounds@*/
/*@-modfilesys@*/
/** \ingroup rpmio
 * \file rpmio/rpmdav.c
 */

#include "system.h"

#if defined(HAVE_PTHREAD_H) && !defined(__LCLINT__)
#include <pthread.h>
#endif

#include <neon/ne_alloc.h>
#include <neon/ne_auth.h>
#include <neon/ne_basic.h>
#include <neon/ne_dates.h>
#include <neon/ne_locks.h>
#include <neon/ne_props.h>
#include <neon/ne_request.h>
#include <neon/ne_socket.h>
#include <neon/ne_string.h>
#include <neon/ne_uri.h>
#include <neon/ne_utils.h>

#include <rpmio_internal.h>

#define _RPMDAV_INTERNAL
#include <rpmdav.h>
                                                                                
#include "argv.h"
#include "ugid.h"
#include "debug.h"

/*@access DIR @*/
/*@access FD_t @*/
/*@access urlinfo @*/

/**
 * Wrapper to free(3), hides const compilation noise, permit NULL, return NULL.
 * @param p		memory to free
 * @retval		NULL always
 */
/*@unused@*/ static inline /*@null@*/ void *
_free(/*@only@*/ /*@null@*/ /*@out@*/ const void * p)
	/*@modifies p@*/
{
    if (p != NULL)	free((void *)p);
    return NULL;
}

/* =============================================================== */
/*@unchecked@*/ /*@null@*/ /*@only@*/
static ne_uri * server;
/*@unchecked@*/ /*@null@*/ /*@only@*/
static ne_session * sess;
/*@unchecked@*/ /*@null@*/ /*@only@*/
static ne_lock_store * lock_store;
/*@unchecked@*/
static ne_server_capabilities caps;

static int davFree(void)
	/*@globals sess, server, internalState @*/
	/*@modifies sess, server, internalState @*/
{
    if (sess != NULL)
	ne_session_destroy(sess);
    sess = NULL;
    if (server != NULL)
	ne_uri_free(server);
    server = _free(server);
    return 0;
}

static int
trust_all_server_certs(/*@unused@*/ void *userdata, /*@unused@*/ int failures,
		/*@unused@*/ const ne_ssl_certificate *cert)
	/*@*/
{
#if 0
    const char *hostname = userdata;
#endif
    return 0;	/* HACK: trust all server certificates. */
}

static int davInit(const char * url)
	/*@globals sess, server, lock_store, internalState @*/
	/*@modifies sess, server, lock_store, internalState @*/
{
    int xx;

    server = xcalloc(1, sizeof(*server));
/*@-noeffect@*/
    ne_debug_init(stderr, 0);
/*@=noeffect@*/
    xx = ne_sock_init();
    lock_store = ne_lockstore_create();
    (void) ne_uri_parse(url, server);
    if (server->scheme == NULL)
        server->scheme = ne_strdup("http");
    if (!server->port)
        server->port = ne_uri_defaultport(server->scheme);

    sess = ne_session_create(server->scheme, server->host, server->port);
    if (!strcasecmp(server->scheme, "https"))
	ne_ssl_set_verify(sess, trust_all_server_certs, server->host);
                                                                                
    ne_lockstore_register(lock_store, sess);
    ne_set_useragent(sess, PACKAGE "/" PACKAGE_VERSION);

    return 0;
}

static int davConnect(void)
	/*@globals internalState @*/
	/*@modifies internalState @*/
{
    int rc;

assert(sess != NULL);
assert(server != NULL);
    rc = ne_options(sess, server->path, &caps);
    switch (rc) {
    case NE_OK:
	break;
    case NE_ERROR:
	/* HACK: "301 Moved Permanently" on empty subdir. */
	if (!strncmp("301 ", ne_get_error(sess), sizeof("301 ")-1))
	    break;
	/*@fallthrough@*/
    case NE_CONNECT:
    case NE_LOOKUP:
    default:
fprintf(stderr, "Connect to %s:%d failed(%d):\n%s\n",
		   server->host, server->port, rc, ne_get_error(sess));
	break;
    }
    return rc;
}

/* =============================================================== */
enum fetch_rtype_e {
    resr_normal = 0,
    resr_collection,
    resr_reference,
    resr_error
};
                                                                                
struct fetch_resource_s {
    struct fetch_resource_s *next;
    char *uri;
/*@unused@*/
    char *displayname;
    enum fetch_rtype_e type;
    size_t size;
    time_t modtime;
    int is_executable;
    int is_vcr;    /* Is version resource. 0: no vcr, 1 checkin 2 checkout */
    char *error_reason; /* error string returned for this resource */
    int error_status; /* error status returned for this resource */
};

/*@null@*/
static void *fetch_destroy_item(/*@only@*/ struct fetch_resource_s *res)
	/*@modifies res @*/
{
    NE_FREE(res->uri);
    NE_FREE(res->error_reason);
    res = _free(res);
    return NULL;
}

/*@null@*/
static void *fetch_destroy_list(/*@only@*/ struct fetch_resource_s *res)
	/*@modifies res @*/
{
    struct fetch_resource_s *next;
/*@-branchstate@*/
    for (; res != NULL; res = next) {
	next = res->next;
	res = fetch_destroy_item(res);
    }
/*@=branchstate@*/
    return NULL;
}

static void *fetch_create_item(/*@unused@*/ void *userdata, /*@unused@*/ const char *uri)
        /*@*/
{
    struct fetch_resource_s * res = ne_calloc(sizeof(*res));
    return res;
}

/* =============================================================== */
struct fetch_context_s {
    struct fetch_resource_s **resrock;
/*@observer@*/
    const char *uri;
    unsigned int include_target; /* Include resource at href */
    int ac;
    int nalloced;
    ARGV_t av;
    mode_t * modes;
    size_t * sizes;
    time_t * mtimes;
};

/*@null@*/
static void *fetch_destroy_context(/*@only@*/ /*@null@*/ struct fetch_context_s *ctx)
	/*@modifies ctx @*/
{
    if (ctx == NULL)
	return NULL;
    if (ctx->av != NULL)
	ctx->av = argvFree(ctx->av);
    ctx->modes = _free(ctx->modes);
    ctx->sizes = _free(ctx->sizes);
    ctx->mtimes = _free(ctx->mtimes);
    ctx->uri = _free(ctx->uri);
    memset(ctx, 0, sizeof(*ctx));
    ctx = _free(ctx);
    return NULL;
}

static void *fetch_create_context(const char *uri)
        /*@*/
{
    struct fetch_context_s * ctx = ne_calloc(sizeof(*ctx));
    ctx->uri = xstrdup(uri);
    return ctx;
}

/*@unchecked@*/ /*@observer@*/
static const ne_propname fetch_props[] = {
    { "DAV:", "getcontentlength" },
    { "DAV:", "getlastmodified" },
    { "http://apache.org/dav/props/", "executable" },
    { "DAV:", "resourcetype" },
    { "DAV:", "checked-in" },
    { "DAV:", "checked-out" },
    { NULL, NULL }
};

#define ELM_resourcetype (NE_PROPS_STATE_TOP + 1)
#define ELM_collection (NE_PROPS_STATE_TOP + 2)

/*@unchecked@*/ /*@observer@*/
static const struct ne_xml_idmap fetch_idmap[] = {
    { "DAV:", "resourcetype", ELM_resourcetype },
    { "DAV:", "collection", ELM_collection }
};

static int fetch_startelm(void *userdata, int parent, 
		const char *nspace, const char *name,
		/*@unused@*/ const char **atts)
	/*@*/
{
    ne_propfind_handler *pfh = userdata;
    struct fetch_resource_s *r = ne_propfind_current_private(pfh);
    int state = ne_xml_mapid(fetch_idmap, NE_XML_MAPLEN(fetch_idmap),
                             nspace, name);

    if (r == NULL || 
        !((parent == NE_207_STATE_PROP && state == ELM_resourcetype) ||
          (parent == ELM_resourcetype && state == ELM_collection)))
        return NE_XML_DECLINE;

    if (state == ELM_collection) {
	r->type = resr_collection;
    }

    return state;
}

static int fetch_compare(const struct fetch_resource_s *r1, 
			    const struct fetch_resource_s *r2)
	/*@*/
{
    /* Sort errors first, then collections, then alphabetically */
    if (r1->type == resr_error) {
	return -1;
    } else if (r2->type == resr_error) {
	return 1;
    } else if (r1->type == resr_collection) {
	if (r2->type != resr_collection) {
	    return -1;
	} else {
	    return strcmp(r1->uri, r2->uri);
	}
    } else {
	if (r2->type != resr_collection) {
	    return strcmp(r1->uri, r2->uri);
	} else {
	    return 1;
	}
    }
}

static void fetch_results(void *userdata, const char *uri,
		    const ne_prop_result_set *set)
	/*@*/
{
    struct fetch_context_s *ctx = userdata;
    struct fetch_resource_s *current, *previous, *newres;
    const char *clength, *modtime, *isexec;
    const char *checkin, *checkout;
    const ne_status *status = NULL;
    ne_uri u;

fprintf(stderr, "Uri: %s\n", uri);

    newres = ne_propset_private(set);
    
    if (ne_uri_parse(uri, &u))
	return;
    
    if (u.path == NULL) {
	ne_uri_free(&u);
	return;
    }

fprintf(stderr, "URI path %s in %s\n", u.path, ctx->uri);
    
    if (ne_path_compare(ctx->uri, u.path) == 0 && !ctx->include_target) {
	/* This is the target URI */
fprintf(stderr, "Skipping target resource.\n");
	/* Free the private structure. */
	free(newres);
	ne_uri_free(&u);
	return;
    }

    newres->uri = ne_strdup(u.path);

    clength = ne_propset_value(set, &fetch_props[0]);    
    modtime = ne_propset_value(set, &fetch_props[1]);
    isexec = ne_propset_value(set, &fetch_props[2]);
    checkin = ne_propset_value(set, &fetch_props[4]);
    checkout = ne_propset_value(set, &fetch_props[5]);

    
    if (clength == NULL)
	status = ne_propset_status(set, &fetch_props[0]);
    if (modtime == NULL)
	status = ne_propset_status(set, &fetch_props[1]);

    if (newres->type == resr_normal && status != NULL) {
	/* It's an error! */
	newres->error_status = status->code;

	/* Special hack for Apache 1.3/mod_dav */
	if (strcmp(status->reason_phrase, "status text goes here") == 0) {
	    const char *desc;
	    if (status->code == 401) {
		desc = _("Authorization Required");
	    } else if (status->klass == 3) {
		desc = _("Redirect");
	    } else if (status->klass == 5) {
		desc = _("Server Error");
	    } else {
		desc = _("Unknown Error");
	    }
	    newres->error_reason = ne_strdup(desc);
	} else {
	    newres->error_reason = ne_strdup(status->reason_phrase);
	}
	newres->type = resr_error;
    }

    if (isexec && strcasecmp(isexec, "T") == 0) {
	newres->is_executable = 1;
    } else {
	newres->is_executable = 0;
    }

    if (modtime)
	newres->modtime = ne_httpdate_parse(modtime);

    if (clength)
	newres->size = atoi(clength);

    /* is vcr */
    if (checkin) {
	newres->is_vcr = 1;
    } else if (checkout) {
	newres->is_vcr = 2;
    } else {
	newres->is_vcr = 0;
    }

fprintf(stderr, "End resource %s\n", newres->uri);

    for (current = *ctx->resrock, previous = NULL; current != NULL; 
	previous = current, current = current->next)
    {
	if (fetch_compare(current, newres) >= 0) {
	    break;
	}
    }
    if (previous) {
	previous->next = newres;
    } else {
	*ctx->resrock = newres;
    }
    newres->next = current;

    ne_uri_free(&u);
}

#ifdef	DYING
static char *format_time(time_t when)
	/*@*/
{
    const char *fmt;
    static char ret[256];
    struct tm *local;
    time_t current_time;
    
    if (when == (time_t)-1) {
	/* Happens on lock-null resources */
	return "  (unknown) ";
    }

    /* from GNU fileutils... this section is 
     *  
     */
    current_time = time(NULL);
    if (current_time > when + 6L * 30L * 24L * 60L * 60L	/* Old. */
	|| current_time < when - 60L * 60L) {
	/* The file is fairly old or in the future.  POSIX says the
	   cutoff is 6 months old; approximate this by 6*30 days.
	   Allow a 1 hour slop factor for what is considered "the
	   future", to allow for NFS server/client clock disagreement.
	   Show the year instead of the time of day.  */
	fmt = "%b %e  %Y";
    } else {
	fmt = "%b %e %H:%M";
    }

    local = localtime(&when);
    if (local != NULL) {
	if (strftime(ret, 256, fmt, local)) {
	    return ret;
	}
    }
    return "???";
}

static void display_ls_line(struct fetch_resource_s *res)
	/*@modifies res @*/
{
    const char *restype;
    char exec_char, vcr_char, *name;

    switch (res->type) {
    case resr_normal: restype = ""; break;
    case resr_reference: restype = _("Ref:"); break;
    case resr_collection: restype = _("Coll:"); break;
    default:
	restype = "???"; break;
    }
    
    if (ne_path_has_trailing_slash(res->uri)) {
	res->uri[strlen(res->uri)-1] = '\0';
    }
    name = strrchr(res->uri, '/');
    if (name != NULL && strlen(name+1) > 0) {
	name++;
    } else {
	name = res->uri;
    }

    name = ne_path_unescape(name);

    if (res->type == resr_error) {
	printf(_("Error: %-30s %d %s\n"), name, res->error_status,
	       res->error_reason?res->error_reason:_("unknown"));
    } else {
	exec_char = res->is_executable ? '*' : ' ';
	/* 0: no vcr, 1: checkin, 2: checkout */
	vcr_char = res->is_vcr==0 ? ' ' : (res->is_vcr==1? '>' : '<');
	printf("%5s %c%c%-29s %10d  %s\n", 
	       restype, vcr_char, exec_char, name,
	       res->size, format_time(res->modtime));
    }

    free(name);
}
#endif

static int davFetch(struct fetch_context_s * ctx)
	/*@globals internalState @*/
	/*@modifies *avp, internalState @*/
{
    int depth = 1;					/* XXX passed arg? */
    unsigned int include_target = 0;			/* XXX passed arg? */
    struct fetch_resource_s * resitem = NULL;
    struct fetch_resource_s ** resrock = &resitem;	/* XXX passed arg? */
    ne_propfind_handler *pfh;
    struct fetch_resource_s *current, *next;
    mode_t st_mode;
    int rc = 0;
    int xx;

assert(sess != NULL);
assert(server != NULL);

    pfh = ne_propfind_create(sess, ctx->uri, depth);

    ctx->resrock = resrock;
    ctx->include_target = include_target;

    ne_xml_push_handler(ne_propfind_get_parser(pfh), 
                        fetch_startelm, NULL, NULL, pfh);

    ne_propfind_set_private(pfh, fetch_create_item, NULL);

    rc = ne_propfind_named(pfh, fetch_props, fetch_results, ctx);

    ne_propfind_destroy(pfh);

    for (current = resitem; current != NULL; current = next) {
	const char *s, *se;
	char * val;

	next = current->next;

	/* Collections have trailing '/' that needs trim. */
	/* The top level collection is returned as well. */
	se = current->uri + strlen(current->uri);
	if (se[-1] == '/') {
	    if (strlen(current->uri) <= strlen(server->path)) {
		current = fetch_destroy_item(current);
		continue;
	    }
	    se--;
	}
	s = se;
	while (s > current->uri && s[-1] != '/')
	    s--;

	val = ne_strndup(s, (se - s));

/*@-nullpass@*/
	val = ne_path_unescape(val);
/*@=nullpass@*/

	xx = argvAdd(&ctx->av, val);
if (_dav_debug)
fprintf(stderr, "*** argvAdd(%p,\"%s\")\n", &ctx->av, val);
	NE_FREE(val);

	while (ctx->ac >= ctx->nalloced) {
	    if (ctx->nalloced <= 0)
		ctx->nalloced = 1;
	    ctx->nalloced *= 2;
	    ctx->modes = xrealloc(ctx->modes,
				(sizeof(*ctx->modes) * ctx->nalloced));
	    ctx->sizes = xrealloc(ctx->sizes,
				(sizeof(*ctx->sizes) * ctx->nalloced));
	    ctx->mtimes = xrealloc(ctx->mtimes,
				(sizeof(*ctx->mtimes) * ctx->nalloced));
	}

	switch (current->type) {
	case resr_normal:
	    st_mode = S_IFREG;
	    break;
	case resr_collection:
	    st_mode = S_IFDIR;
	    break;
	case resr_reference:
	case resr_error:
	default:
	    st_mode = 0;
	    break;
	}
	ctx->modes[ctx->ac] = st_mode;
	ctx->sizes[ctx->ac] = current->size;
	ctx->mtimes[ctx->ac] = current->modtime;
	ctx->ac++;

	current = fetch_destroy_item(current);
    }

    return rc;
}

static int davNLST(struct fetch_context_s * ctx)
	/*@globals internalState @*/
	/*@modifies *avp, internalState @*/
{
    int rc;
    int xx;

    rc = davInit(ctx->uri);
    if (rc)
	goto exit;

    rc = davConnect();
    if (rc)
	goto exit;

assert(sess != NULL);
assert(server != NULL);

    rc = davFetch(ctx);
    switch (rc) {
    case NE_OK:
        break;
    default:
fprintf(stderr, "Fetch from %s:%d failed:\n%s\n",
		   server->host, server->port, ne_get_error(sess));
        break;
    }

exit:
    if (rc)
	xx = davFree();
    return rc;
}

/* =============================================================== */
#ifdef	NOTYET
static int davMkdir(const char * path, /*@unused@*/ mode_t mode)
	/*@globals h_errno, fileSystem, internalState @*/
	/*@modifies fileSystem, internalState @*/
{
    int rc;
    if ((rc = davCmd("MKD", path, NULL)) != 0)
	return rc;
#if NOTYET
    {	char buf[20];
	sprintf(buf, " 0%o", mode);
	(void) davCmd("SITE CHMOD", path, buf);
    }
#endif
    return rc;
}

static int davChdir(const char * path)
	/*@globals h_errno, fileSystem, internalState @*/
	/*@modifies fileSystem, internalState @*/
{
    return davCmd("CWD", path, NULL);
}

static int davRmdir(const char * path)
	/*@globals h_errno, fileSystem, internalState @*/
	/*@modifies fileSystem, internalState @*/
{
    return davCmd("RMD", path, NULL);
}

static int davRename(const char * oldpath, const char * newpath)
	/*@globals h_errno, fileSystem, internalState @*/
	/*@modifies fileSystem, internalState @*/
{
    int rc;
    if ((rc = davCmd("RNFR", oldpath, NULL)) != 0)
	return rc;
    return davCmd("RNTO", newpath, NULL);
}

static int davUnlink(const char * path)
	/*@globals h_errno, fileSystem, internalState @*/
	/*@modifies fileSystem, internalState @*/
{
    return davCmd("DELE", path, NULL);
}
#endif	/* NOTYET */

/* =============================================================== */

static const char * statstr(const struct stat * st,
		/*@returned@*/ /*@out@*/ char * buf)
	/*@modifies *buf @*/
{
    sprintf(buf,
	"*** dev %x ino %x mode %0o nlink %d uid %d gid %d rdev %x size %x\n",
	(unsigned)st->st_dev,
	(unsigned)st->st_ino,
	st->st_mode,
	(unsigned)st->st_nlink,
	st->st_uid,
	st->st_gid,
	(unsigned)st->st_rdev,
	(unsigned)st->st_size);
    return buf;
}

/*@unchecked@*/
static int dav_st_ino = 0xdead0000;

int davStat(const char * path, /*@out@*/ struct stat *st)
	/*@globals dav_st_ino, h_errno, fileSystem, internalState @*/
	/*@modifies *st, dav_st_ino, fileSystem, internalState @*/
{
    struct fetch_context_s * ctx = NULL;
    char buf[1024];
    int rc = 0;

/* HACK: neon really wants collections with trailing '/' */
    ctx = fetch_create_context(path);
    rc = davNLST(ctx);
    if (rc) {
/* HACK: errno = ??? */
	goto exit;
    }

    memset(st, 0, sizeof(*st));
    st->st_mode = ctx->modes[0];
    st->st_size = ctx->sizes[0];
    st->st_mtime = ctx->mtimes[0];
    if (S_ISDIR(st->st_mode)) {
	st->st_nlink = 2;
	st->st_mode |= 0755;
    } else
    if (S_ISREG(st->st_mode)) {
	st->st_nlink = 1;
	st->st_mode |= 0644;
    } 

    /* XXX fts(3) needs/uses st_ino, make something up for now. */
    if (st->st_ino == 0)
	st->st_ino = dav_st_ino++;
if (_dav_debug)
fprintf(stderr, "*** davStat(%s) rc %d\n%s", path, rc, statstr(st, buf));
exit:
    ctx = fetch_destroy_context(ctx);
    return rc;
}

int davLstat(const char * path, /*@out@*/ struct stat *st)
	/*@globals dav_st_ino, h_errno, fileSystem, internalState @*/
	/*@modifies *st, dav_st_ino, fileSystem, internalState @*/
{
    struct fetch_context_s * ctx = NULL;
    char buf[1024];
    int rc = 0;

/* HACK: neon really wants collections with trailing '/' */
    ctx = fetch_create_context(path);
    rc = davNLST(ctx);
    if (rc) {
/* HACK: errno = ??? */
	goto exit;
    }

    memset(st, 0, sizeof(*st));
    st->st_mode = ctx->modes[0];
    st->st_size = ctx->sizes[0];
    st->st_mtime = ctx->mtimes[0];
    if (S_ISDIR(st->st_mode)) {
	st->st_nlink = 2;
	st->st_mode |= 0755;
    } else
    if (S_ISREG(st->st_mode)) {
	st->st_nlink = 1;
	st->st_mode |= 0644;
    } 

    /* XXX fts(3) needs/uses st_ino, make something up for now. */
    if (st->st_ino == 0)
	st->st_ino = dav_st_ino++;
if (_dav_debug)
fprintf(stderr, "*** davLstat(%s) rc %d\n%s\n", path, rc, statstr(st, buf));
exit:
    ctx = fetch_destroy_context(ctx);
    return rc;
}

#ifdef	NOTYET
static int davReadlink(const char * path, /*@out@*/ char * buf, size_t bufsiz)
	/*@globals h_errno, fileSystem, internalState @*/
	/*@modifies *buf, fileSystem, internalState @*/
{
    int rc;
    rc = davNLST(path, DO_FTP_READLINK, NULL, buf, bufsiz);
if (_dav_debug)
fprintf(stderr, "*** davReadlink(%s) rc %d\n", path, rc);
    return rc;
}
#endif	/* NOTYET */

/* =============================================================== */
/*@unchecked@*/
int avmagicdir = 0x3607113;

int avClosedir(/*@only@*/ DIR * dir)
{
    AVDIR avdir = (AVDIR)dir;

if (_av_debug)
fprintf(stderr, "*** avClosedir(%p)\n", avdir);

#if defined(HAVE_PTHREAD_H)
/*@-moduncon -noeffectuncon @*/
    (void) pthread_mutex_destroy(&avdir->lock);
/*@=moduncon =noeffectuncon @*/
#endif

    avdir = _free(avdir);
    return 0;
}

struct dirent * avReaddir(DIR * dir)
{
    AVDIR avdir = (AVDIR)dir;
    struct dirent * dp;
    const char ** av;
    unsigned char * dt;
    int ac;
    int i;

    if (avdir == NULL || !ISAVMAGIC(avdir) || avdir->data == NULL) {
	/* XXX TODO: EBADF errno. */
	return NULL;
    }

    dp = (struct dirent *) avdir->data;
    av = (const char **) (dp + 1);
    ac = avdir->size;
    dt = (char *) (av + (ac + 1));
    i = avdir->offset + 1;

/*@-boundsread@*/
    if (i < 0 || i >= ac || av[i] == NULL)
	return NULL;
/*@=boundsread@*/

    avdir->offset = i;

    /* XXX glob(3) uses REAL_DIR_ENTRY(dp) test on d_ino */
/*@-type@*/
    dp->d_ino = i + 1;		/* W2DO? */
    dp->d_reclen = 0;		/* W2DO? */

#if !defined(hpux) && !defined(sun)
    dp->d_off = 0;		/* W2DO? */
/*@-boundsread@*/
    dp->d_type = dt[i];
/*@=boundsread@*/
#endif
/*@=type@*/

    strncpy(dp->d_name, av[i], sizeof(dp->d_name));
if (_av_debug)
fprintf(stderr, "*** avReaddir(%p) %p \"%s\"\n", (void *)avdir, dp, dp->d_name);
    
    return dp;
}

/*@-boundswrite@*/
DIR * avOpendir(const char * path)
{
    AVDIR avdir;
    struct dirent * dp;
    size_t nb = 0;
    const char ** av;
    unsigned char * dt;
    char * t;
    int ac;

if (_av_debug)
fprintf(stderr, "*** avOpendir(%s)\n", path);
    nb = sizeof(".") + sizeof("..");
    ac = 2;

    nb += sizeof(*avdir) + sizeof(*dp) + ((ac + 1) * sizeof(*av)) + (ac + 1);
    avdir = xcalloc(1, nb);
/*@-abstract@*/
    dp = (struct dirent *) (avdir + 1);
    av = (const char **) (dp + 1);
    dt = (char *) (av + (ac + 1));
    t = (char *) (dt + ac + 1);
/*@=abstract@*/

    avdir->fd = avmagicdir;
/*@-usereleased@*/
    avdir->data = (char *) dp;
/*@=usereleased@*/
    avdir->allocation = nb;
    avdir->size = ac;
    avdir->offset = -1;
    avdir->filepos = 0;

#if defined(HAVE_PTHREAD_H)
/*@-moduncon -noeffectuncon -nullpass @*/
    (void) pthread_mutex_init(&avdir->lock, NULL);
/*@=moduncon =noeffectuncon =nullpass @*/
#endif

    ac = 0;
    /*@-dependenttrans -unrecog@*/
    dt[ac] = DT_DIR;	av[ac++] = t;	t = stpcpy(t, ".");	t++;
    dt[ac] = DT_DIR;	av[ac++] = t;	t = stpcpy(t, "..");	t++;
    /*@=dependenttrans =unrecog@*/

    av[ac] = NULL;

/*@-kepttrans@*/
    return (DIR *) avdir;
/*@=kepttrans@*/
}
/*@=boundswrite@*/

/* =============================================================== */
/*@unchecked@*/
int davmagicdir = 0x8440291;

int davClosedir(/*@only@*/ DIR * dir)
{
    DAVDIR avdir = (DAVDIR)dir;

if (_dav_debug)
fprintf(stderr, "*** davClosedir(%p)\n", avdir);

#if defined(HAVE_PTHREAD_H)
/*@-moduncon -noeffectuncon @*/
    (void) pthread_mutex_destroy(&avdir->lock);
/*@=moduncon =noeffectuncon @*/
#endif

    avdir = _free(avdir);
    return 0;
}

struct dirent * davReaddir(DIR * dir)
{
    DAVDIR avdir = (DAVDIR)dir;
    struct dirent * dp;
    const char ** av;
    unsigned char * dt;
    int ac;
    int i;

    if (avdir == NULL || !ISDAVMAGIC(avdir) || avdir->data == NULL) {
	/* XXX TODO: EBADF errno. */
	return NULL;
    }

    dp = (struct dirent *) avdir->data;
    av = (const char **) (dp + 1);
    ac = avdir->size;
    dt = (char *) (av + (ac + 1));
    i = avdir->offset + 1;

/*@-boundsread@*/
    if (i < 0 || i >= ac || av[i] == NULL)
	return NULL;
/*@=boundsread@*/

    avdir->offset = i;

    /* XXX glob(3) uses REAL_DIR_ENTRY(dp) test on d_ino */
/*@-type@*/
    dp->d_ino = i + 1;		/* W2DO? */
    dp->d_reclen = 0;		/* W2DO? */

#if !defined(hpux) && !defined(sun)
    dp->d_off = 0;		/* W2DO? */
/*@-boundsread@*/
    dp->d_type = dt[i];
/*@=boundsread@*/
#endif
/*@=type@*/

    strncpy(dp->d_name, av[i], sizeof(dp->d_name));
if (_dav_debug)
fprintf(stderr, "*** davReaddir(%p) %p \"%s\"\n", (void *)avdir, dp, dp->d_name);
    
    return dp;
}

/*@-boundswrite@*/
DIR * davOpendir(const char * path)
{
    struct fetch_context_s * ctx;
    DAVDIR avdir;
    struct dirent * dp;
    size_t nb;
    const char ** av, ** nav;
    unsigned char * dt;
    char * t;
    int ac, nac;
    int rc;

if (_dav_debug)
fprintf(stderr, "*** davOpendir(%s)\n", path);

    /* Load DAV collection into argv. */
    ctx = fetch_create_context(path);
    rc = davNLST(ctx);
    if (rc)
	return NULL;

    nb = 0;
    ac = 0;
    av = ctx->av;
    if (av != NULL)
    while (av[ac] != NULL)
	nb += strlen(av[ac++]) + 1;
    ac += 2;	/* for "." and ".." */
    nb += sizeof(".") + sizeof("..");

    nb += sizeof(*avdir) + sizeof(*dp) + ((ac + 1) * sizeof(*av)) + (ac + 1);
    avdir = xcalloc(1, nb);
    /*@-abstract@*/
    dp = (struct dirent *) (avdir + 1);
    nav = (const char **) (dp + 1);
    dt = (char *) (nav + (ac + 1));
    t = (char *) (dt + ac + 1);
    /*@=abstract@*/

    avdir->fd = davmagicdir;
/*@-usereleased@*/
    avdir->data = (char *) dp;
/*@=usereleased@*/
    avdir->allocation = nb;
    avdir->size = ac;
    avdir->offset = -1;
    avdir->filepos = 0;

#if defined(HAVE_PTHREAD_H)
/*@-moduncon -noeffectuncon -nullpass @*/
    (void) pthread_mutex_init(&avdir->lock, NULL);
/*@=moduncon =noeffectuncon =nullpass @*/
#endif

    nac = 0;
    /*@-dependenttrans -unrecog@*/
    dt[nac] = DT_DIR;	nav[nac++] = t;	t = stpcpy(t, ".");	t++;
    dt[nac] = DT_DIR;	nav[nac++] = t;	t = stpcpy(t, "..");	t++;
    /*@=dependenttrans =unrecog@*/

    /* Copy DAV items into DIR elments. */
    ac = 0;
    if (av != NULL)
    while (av[ac] != NULL) {
	nav[nac] = t;
	dt[nac] = (S_ISDIR(ctx->modes[ac]) ? DT_DIR : DT_REG);
	t = stpcpy(t, av[ac]);
	ac++;
	t++;
	nac++;
    }
    nav[nac] = NULL;

    ctx = fetch_destroy_context(ctx);

/*@-kepttrans@*/
    return (DIR *) avdir;
/*@=kepttrans@*/
}
/*@=boundswrite@*/
/*@=modfilesys@*/
/*@=bounds@*/
