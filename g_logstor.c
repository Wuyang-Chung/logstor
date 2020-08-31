/*-
 * Copyright (c) 2004-2006 Pawel Jakub Dawidek <pjd@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/bio.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/malloc.h>
#include <geom/geom.h>
#include <geom/logstor/g_logstor.h>

SYSCTL_DECL(_kern_geom);
static SYSCTL_NODE(_kern_geom, OID_AUTO, logstor, CTLFLAG_RW, 0, "GEOM_LOGSTOR stuff");
static u_int g_logstor_debug = 0;
SYSCTL_UINT(_kern_geom_logstor, OID_AUTO, debug, CTLFLAG_RW, &g_logstor_debug, 0,
    "Debug level");

#if defined(MY_DEBUG)
void my_break(void) {}

void my_debug(const char * fname, int line_num)
{

	printf("*** %s *** %s %d\n", __func__, fname, line_num);
	my_break();
}
#endif

/*
  Even with @fbuf_ratio set to 1, there will still be some fbuf_flush called
  during fbuf_alloc
*/
static double fbuf_ratio = 1.0; // the ration of allocated and needed fbufs

struct {
	unsigned r_logstor_read;
	unsigned r_logstor_read_one;
	unsigned r_seg_sum_read;
	unsigned r_superblock_read;
	unsigned r_gc_seg_clean;
	unsigned r_fbuf_read_and_hash;
	unsigned w_logstor_write;
	unsigned w_logstor_write_one;
	unsigned w_seg_sum_write;
	unsigned w_superblock_init;
	unsigned w_superblock_write;
	unsigned w_fbuf_write;
	unsigned d_delete_count;
} rw;

#define	SIG_LOGSTOR	0x4C4F4753	// "LOGS": Log-Structured Storage
#define	VER_MAJOR	0
#define	VER_MINOR	1

#define SEG_DATA_START	1	// the data segment starts here
#define SEG_SUM_OFF	(SECTORS_PER_SEG - 1) // segment summary offset in segment
#define	SEG_SIZE	0x400000		// 4M
#define	SECTORS_PER_SEG	(SEG_SIZE/SECTOR_SIZE) // 1024
#define SA2SEGA_SHIFT	10
#define BLOCKS_PER_SEG	(SEG_SIZE/SECTOR_SIZE - 1)
#define CLEAN_WINDOW	6
#define CLEAN_AGE_LIMIT	4

#define	META_BASE	0xC0000000u	// metadata block start address
#define	META_INVALID	0		// invalid metadata address

#define	SECTOR_NULL	0	// this sector address can not map to any block address
#define SECTOR_DELETE	2	// delete marker for a block

#define	IS_META_ADDR(x)	(((x) & META_BASE) == META_BASE)
#define META_LEAF_DEPTH 2

#define FILE_BUCKET_COUNT	12899

/*
  The file descriptor for the forward map files
*/
enum {
	FD_BASE,	// file descriptor for base map
	FD_ACTIVE,	// file descriptor for active map
	FD_DELTA,	// file descriptor for delta map
	FD_COUNT	// the number of file descriptors
};

struct _superblock {
	uint32_t	sig;		// signature
	uint8_t		ver_major;
	uint8_t		ver_minor;
	uint16_t	sb_gen;		// the generation number. Used for redo after system crash
	uint32_t	max_block_cnt;	// max number of blocks supported
	/*
	   The segments are treated as circular buffer
	 */
	int32_t	seg_cnt;	// total number of segments
	int32_t seg_free_cnt;	// number of free segments
	int32_t	seg_alloc_p;	// allocate this segment
	int32_t	seg_reclaim_p;	// clean this segment
	/*
	   The files for forward mapping file

	   Mapping is always updated in @fm_cur. When snapshot command is issued
	   @fm_cur is copied to @fm_delta and then cleaned.
	   Backup program then backs up the delta by reading @fm_delta.
	   After backup is finished, @fm_delta is merged into @fm_base and then cleaned.

	   If reduced to reboot restore usage, only @fm_cur and @fm_base are needed.
	   Each time a PC is rebooted @fm_cur is cleaned so all data are restored.

	   So the actual mapping is @fm_cur || @fm_delta || @fm_base.
	   The first mapping that is not empty is used.
	*/
	uint32_t ftab[FD_COUNT]; 	// the file table
	uint8_t seg_age[];
};

_Static_assert(sizeof(struct _superblock) < SECTOR_SIZE,
    "The size of the super block must be smaller than SECTOR_SIZE");

/*
  The last sector in a segment is the segment summary. It stores the reverse mapping table
*/
struct _seg_sum {
	uint32_t ss_rm[SECTORS_PER_SEG - 1];	// reverse map
	// reverse map SECTORS_PER_SEG - 1 is not used so we store something here
	uint16_t ss_gen;  // sequence number. used for redo after system crash
	uint16_t ss_alloc_p; // allocate sector at this location

	// below are not stored on disk
	struct _ss_soft {
		TAILQ_ENTRY(_seg_sum) queue;
		uint32_t sega; // the segment address of the segment summary
		unsigned live_count;		
	} ss_soft;
};

_Static_assert(sizeof(struct _seg_sum) - sizeof(struct _ss_soft) == SECTOR_SIZE,
    "The size of segment summary must be equal to SECTOR_SIZE");

/*
  File data and its indirect blocks are also stored in the downstream disk.
  The sectors used to store the file data and its indirect blocks are called metadata.

  Each metadata block has a corresponding metadata address.
  Below is the format of the metadata address.

  The metadata address occupies a small part of buffer address space. For buffer address
  that is >= META_BASE, it is actually a metadata address.
*/
union meta_addr { // metadata address for file data and its indirect blocks
	uint32_t	uint32;
	struct {
		uint32_t index  :20;	// index for indirect block
		uint32_t depth	:2;	// depth of the node
		uint32_t fd	:2;	// file descriptor
		uint32_t resv0	:6;	// reserved
		uint32_t meta	:2;	// 1 for metadata address
	};
};

_Static_assert(sizeof(union meta_addr) == 4, "The size of emta_addr must be 4");

/*
  Metadata is cached in memory. The access unit of metadata is block so each cache line
  stores a block of metadata
*/
struct _fbuf { // file buffer
	union {
		LIST_ENTRY(_fbuf) indir_queue; // for the indirect queue
		struct {
			struct _fbuf *next;
			struct _fbuf *prev;
		} cir_queue; // list entry for the circular queue
	};
	uint16_t ref_cnt;	// only used by active indirect block queue
	bool	on_cir_queue;	// on circular queue
	bool	accessed;	// only used by cache entries on circular queue
	bool	modified;	// the metadata is dirty
	
	LIST_ENTRY(_fbuf)	buffer_bucket_queue;// the pointer for bucket chain
	struct _fbuf	*parent;

	union meta_addr	ma;	// the metadata address
#if defined(MY_DEBUG)
	uint32_t	sa;	// the sector address of the @data
#endif
	// the metadata is cached here
	uint32_t	data[SECTOR_SIZE/sizeof(uint32_t)];	
};

/*
	logstor soft control
*/
struct g_logstor_softc {
	struct g_geom	*sc_geom;
	struct g_consumer *cp;
	struct mtx	sc_lock;

	struct _seg_sum seg_sum_cold;// segment summary for the cold segment
	struct _seg_sum seg_sum_hot;// segment summary for the hot segment
	
	TAILQ_HEAD(, _seg_sum) cc_head; // clean candidate
	struct _seg_sum clean_candidate[CLEAN_WINDOW];
	unsigned char cleaner_disabled;
	uint32_t clean_low_water;
	uint32_t clean_high_water;
	
	int fbuf_count;
	int fbuf_modified_count;
	struct _fbuf *fbuf;
#if 0
	uint32_t *fbuf_accessed;
	uint32_t *fbuf_modified;
	uint32_t *fbuf_on_cir_queue;
#endif	
	// buffer hash queue
	LIST_HEAD(_fbuf_bucket, _fbuf)	fbuf_bucket[FILE_BUCKET_COUNT];
	
	struct _fbuf *cir_buffer_head;	// head of the circular queue
	LIST_HEAD(, _fbuf) indirect_head[META_LEAF_DEPTH]; // indirect queue
	
#if defined(MY_DEBUG)
	int cir_queue_cnt;
#endif

	// statistics
	unsigned data_write_count;	// data block write to disk
	unsigned other_write_count;	// other write to disk, such as metadata write and segment cleaning
	unsigned fbuf_hit, fbuf_miss;	// statistics

#if defined(ORIGINAL) //del
	int disk_fd;
	char *ram_disk;
	void (*my_read) (uint32_t sa, void *buf, unsigned size);
	void (*my_write)(uint32_t sa, const void *buf, unsigned size);
#endif
	bool sb_modified;	// the super block is dirty
	uint32_t sb_sa; 	// superblock's sector address
	uint8_t *seg_age;
struct _superblock superblock;
};

static int _logstor_read(struct g_logstor_softc *sc, struct bio *bp,unsigned ba, int size);
static int _logstor_read_one(struct g_logstor_softc *sc, struct bio *bp, uint32_t ba);
static int _logstor_write(struct g_logstor_softc *sc, struct bio *bp,unsigned ba, int size, struct _seg_sum *seg_sum);
static int _logstor_write_one(struct g_logstor_softc *sc, struct bio *bp, uint32_t ba, struct _seg_sum *seg_sum);
static void seg_alloc(struct g_logstor_softc *sc, struct _seg_sum *seg_sum);
static void seg_reclaim_init(struct g_logstor_softc *sc, struct _seg_sum *seg_sum);
static void file_mod_flush(struct g_logstor_softc *sc);
static void file_mod_init(struct g_logstor_softc *sc);
static void file_mod_fini(struct g_logstor_softc *sc);
static uint32_t file_read_4byte(struct g_logstor_softc *sc, uint8_t fh, uint32_t ba);
static void file_write_4byte(struct g_logstor_softc *sc, uint8_t fh, uint32_t ba, uint32_t sa);

static uint32_t fbuf_ma2sa(struct g_logstor_softc *sc, union meta_addr ma);
static void seg_sum_write(struct g_logstor_softc *sc, struct _seg_sum *seg_sum);
static int superblock_read(struct g_logstor_softc *sc, struct g_consumer *cp);
static void superblock_write(struct g_logstor_softc *sc);
static void clean_check(struct g_logstor_softc *sc);
static void seg_clean(struct g_logstor_softc *sc, struct _seg_sum *seg_sum);
static void seg_live_count(struct g_logstor_softc *sc, struct _seg_sum *seg_sum);

static void disk_read (uint32_t sa, void *buf, unsigned size);
static void disk_write(uint32_t sa, const void *buf, unsigned size);
static void ram_read (uint32_t sa, void *buf, unsigned size);
static void ram_write(uint32_t sa, const void *buf, unsigned size);

static uint8_t *file_access(struct g_logstor_softc *sc, uint8_t fd, uint32_t offset, uint32_t *buf_off, bool bl_write);
static struct _fbuf *fbuf_get(struct g_logstor_softc *sc, union meta_addr ma);
static struct _fbuf *fbuf_read_and_hash(struct g_logstor_softc *sc, uint32_t sa, union meta_addr ma);
static struct _fbuf *fbuf_search(struct g_logstor_softc *sc, union meta_addr ma);
static void fbuf_flush(struct g_logstor_softc *sc, struct _fbuf *buf, struct _seg_sum *seg_sum);
static void fbuf_hash_insert(struct g_logstor_softc *sc, struct _fbuf *buf, unsigned key);

static int g_logstor_destroy(struct g_geom *gp, boolean_t force);
static int g_logstor_destroy_geom(struct gctl_req *req, struct g_class *mp,
    struct g_geom *gp);
static void g_logstor_ctlreq(struct gctl_req *req, struct g_class *mp,
    const char *verb);
static void g_logstor_dumpconf(struct sbuf *sb, const char *indent,
    struct g_geom *gp, struct g_consumer *cp, struct g_provider *pp);

struct g_class g_logstor_class = {
	.name = G_LOGSTOR_CLASS_NAME,
	.version = G_VERSION,
	.ctlreq = g_logstor_ctlreq,
	.destroy_geom = g_logstor_destroy_geom
};

static void
g_logstor_orphan(struct g_consumer *cp)
{

	g_topology_assert();
	g_logstor_destroy(cp->geom, 1);
}

#if defined(ORIGINAL)
static void
g_logstor_resize(struct g_consumer *cp)
{
	struct g_logstor_softc *sc;
	struct g_geom *gp;
	struct g_provider *pp;
	off_t size;

	g_topology_assert();

	gp = cp->geom;
	sc = gp->softc;

	if (sc->sc_explicitsize != 0)
		return;
	if (cp->provider->mediasize < sc->sc_offset) {
		g_logstor_destroy(gp, 1);
		return;
	}
	size = cp->provider->mediasize - sc->sc_offset;
	LIST_FOREACH(pp, &gp->provider, provider)
		g_resize_provider(pp, size);
}
#endif

static void
g_logstor_start(struct bio *bp)
{
	struct g_logstor_softc *sc;
	struct g_geom *gp;
	struct g_provider *pp;
	struct bio *cbp;

	gp = bp->bio_to->geom;
	sc = gp->softc;
	G_LOGSTOR_LOGREQ(bp, "Request received.");
	switch (bp->bio_cmd) {
	case BIO_READ:
		logstor_read(sc, bp);
		return;
	case BIO_WRITE:
		logstor_write(sc, bp);
		return;
	case BIO_DELETE:
		logstor_delete(sc, bp);
		return;
	case BIO_GETATTR:
		if (g_handleattr_int(bp, "GEOM::candelete", 1)) {
			return;
		}
		break;
	case BIO_FLUSH:
		break;
	case BIO_CMD0:
		break;
	case BIO_CMD1:
		break;
	case BIO_CMD2:
		break;
	}
	cbp = g_clone_bio(bp);
	if (cbp == NULL) {
		g_io_deliver(bp, ENOMEM);
		return;
	}
	cbp->bio_done = g_std_done;
	pp = LIST_FIRST(&gp->provider);
	KASSERT(pp != NULL, ("NULL pp"));
	cbp->bio_to = pp;
	G_LOGSTOR_LOGREQ(cbp, "Sending request.");
	g_io_request(cbp, LIST_FIRST(&gp->consumer));
}

static int
g_logstor_access(struct g_provider *pp, int dr, int dw, int de)
{
	struct g_geom *gp;
	struct g_consumer *cp;
	int error;

	gp = pp->geom;
	cp = LIST_FIRST(&gp->consumer);
	error = g_access(cp, dr, dw, de);

	return (error);
}

static int
g_logstor_create(struct gctl_req *req, struct g_class *mp, struct g_provider *pp)
{
	struct g_logstor_softc *sc;
	struct g_geom *gp;
	struct g_provider *newpp;
	struct g_consumer *cp;
	int error;

	g_topology_assert();

	gp = NULL;
	newpp = NULL;
	cp = NULL;

	//pp->mediasize
	LIST_FOREACH(gp, &mp->geom, geom) {
		if (strcmp(gp->name, pp->name) == 0) {
			gctl_error(req, "Logstor %s already exists.", gp->name);
			return (EEXIST);
		}
	}
	gp = g_new_geomf(mp, "%s", pp->name);
	sc = g_malloc(sizeof(*sc), M_WAITOK | M_ZERO);
	gp->softc = sc;
	sc->sc_geom = gp;
	gp->start = g_logstor_start;
	gp->orphan = g_logstor_orphan;
	//gp->resize = g_logstor_resize;
	gp->access = g_logstor_access;
	gp->dumpconf = g_logstor_dumpconf;

	cp = g_new_consumer(gp);
	cp->flags |= G_CF_DIRECT_SEND | G_CF_DIRECT_RECEIVE;
	error = g_attach(cp, pp);
	if (error != 0) {
		gctl_error(req, "Cannot attach to provider %s.", pp->name);
		goto fail;
	}

	logstor_open(sc, cp);

	newpp = g_new_providerf(gp, "logstor/%s", pp->name);
	newpp->flags |= G_PF_DIRECT_SEND | G_PF_DIRECT_RECEIVE;
	newpp->mediasize = sc->superblock.max_block_cnt * SECTOR_SIZE;
	newpp->sectorsize = SECTOR_SIZE;

	//newpp->flags |= pp->flags & G_PF_ACCEPT_UNMAPPED;
	g_error_provider(newpp, 0);
	G_LOGSTOR_DEBUG(0, "Device %s created.", newpp->name);
	return (0);
fail:
	if (cp->provider != NULL)
		g_detach(cp);
	g_destroy_consumer(cp);
	g_destroy_provider(newpp);
	sc = gp->softc;
	g_free(sc->fbuf);
	g_free(sc->seg_age);
	g_free(sc);
	g_destroy_geom(gp);
	return (error);
}

static int
g_logstor_destroy(struct g_geom *gp, boolean_t force)
{
	struct g_logstor_softc *sc;
	struct g_provider *pp;

	g_topology_assert();
	sc = gp->softc;
	if (sc == NULL)
		return (ENXIO);
	pp = LIST_FIRST(&gp->provider);
	if (pp != NULL && (pp->acr != 0 || pp->acw != 0 || pp->ace != 0)) {
		if (force) {
			G_LOGSTOR_DEBUG(0, "Device %s is still open, so it "
			    "can't be definitely removed.", pp->name);
		} else {
			G_LOGSTOR_DEBUG(1, "Device %s is still open (r%dw%de%d).",
			    pp->name, pp->acr, pp->acw, pp->ace);
			return (EBUSY);
		}
	} else {
		G_LOGSTOR_DEBUG(0, "Device %s removed.", gp->name);
	}
	gp->softc = NULL;
	g_free(sc);
	g_wither_geom(gp, ENXIO);

	return (0);
}

static int
g_logstor_destroy_geom(struct gctl_req *req, struct g_class *mp, struct g_geom *gp)
{

	return (g_logstor_destroy(gp, 0));
}

static void
g_logstor_ctl_create(struct gctl_req *req, struct g_class *mp)
{
	struct g_provider *pp;
	const char *name;
	char param[16];
	int i, *nargs;

	g_topology_assert();

	nargs = gctl_get_paraml(req, "nargs", sizeof(*nargs));
	if (nargs == NULL) {
		gctl_error(req, "No '%s' argument", "nargs");
		return;
	}
	if (*nargs <= 0) {
		gctl_error(req, "Missing device(s).");
		return;
	}

	for (i = 0; i < *nargs; i++) {
		snprintf(param, sizeof(param), "arg%d", i);
		name = gctl_get_asciiparam(req, param);
		if (name == NULL) {
			gctl_error(req, "No 'arg%d' argument", i);
			return;
		}
		if (strncmp(name, "/dev/", strlen("/dev/")) == 0)
			name += strlen("/dev/");
		pp = g_provider_by_name(name);
		if (pp == NULL) {
			G_LOGSTOR_DEBUG(1, "Provider %s is invalid.", name);
			gctl_error(req, "Provider %s is invalid.", name);
			return;
		}
		if (g_logstor_create(req, mp, pp) != 0) {
			return;
		}
	}
}

static void
g_logstor_ctl_configure(struct gctl_req *req, struct g_class *mp)
{
	struct g_geom *gp;
	struct g_logstor_softc *sc;
	const char *name;
	char param[32];
	int i, *nargs;

	g_topology_assert();

	nargs = gctl_get_paraml(req, "nargs", sizeof(*nargs));
	if (nargs == NULL) {
		gctl_error(req, "No '%s' argument", "nargs");
		return;
	}
	if (*nargs <= 0) {
		gctl_error(req, "Missing device(s).");
		return;
	}

	for (i = 0; i < *nargs; i++) {
		snprintf(param, sizeof(param), "arg%d", i);
		name = gctl_get_asciiparam(req, param);
		if (name == NULL) {
			gctl_error(req, "No 'arg%d' argument", i);
			return;
		}
		LIST_FOREACH(gp, &mp->geom, geom) {
			if (strcmp(gp->name, name) == 0)
				break;
		}
		if (gp == NULL || gp->softc == NULL) {
			G_LOGSTOR_DEBUG(1, "Geom %s is invalid.", name);
			gctl_error(req, "Geom %s is invalid.", name);
			return;
		}
		sc = gp->softc;
	}
}

static struct g_geom *
g_logstor_find_geom(struct g_class *mp, const char *name)
{
	struct g_geom *gp;

	LIST_FOREACH(gp, &mp->geom, geom) {
		if (strcmp(gp->name, name) == 0)
			return (gp);
	}
	return (NULL);
}

static void
g_logstor_ctl_destroy(struct gctl_req *req, struct g_class *mp)
{
	int *nargs, *force, error, i;
	struct g_geom *gp;
	const char *name;
	char param[32];

	g_topology_assert();

	nargs = gctl_get_paraml(req, "nargs", sizeof(*nargs));
	if (nargs == NULL) {
		gctl_error(req, "No '%s' argument", "nargs");
		return;
	}
	if (*nargs <= 0) {
		gctl_error(req, "Missing device(s).");
		return;
	}
	force = gctl_get_paraml(req, "force", sizeof(*force));
	if (force == NULL) {
		gctl_error(req, "No 'force' argument");
		return;
	}

	for (i = 0; i < *nargs; i++) {
		snprintf(param, sizeof(param), "arg%d", i);
		name = gctl_get_asciiparam(req, param);
		if (name == NULL) {
			gctl_error(req, "No 'arg%d' argument", i);
			return;
		}
		snprintf(param, sizeof(param), "%s", name);
		gp = g_logstor_find_geom(mp, param);
		if (gp == NULL) {
			G_LOGSTOR_DEBUG(1, "Device %s is invalid.", name);
			gctl_error(req, "Device %s is invalid.", name);
			return;
		}
		error = g_logstor_destroy(gp, *force);
		if (error != 0) {
			gctl_error(req, "Cannot destroy device %s (error=%d).",
			    gp->name, error);
			return;
		}
	}
}

static void
g_logstor_ctl_reset(struct gctl_req *req, struct g_class *mp)
{
	struct g_logstor_softc *sc;
	struct g_provider *pp;
	const char *name;
	char param[16];
	int i, *nargs;

	g_topology_assert();

	nargs = gctl_get_paraml(req, "nargs", sizeof(*nargs));
	if (nargs == NULL) {
		gctl_error(req, "No '%s' argument", "nargs");
		return;
	}
	if (*nargs <= 0) {
		gctl_error(req, "Missing device(s).");
		return;
	}

	for (i = 0; i < *nargs; i++) {
		snprintf(param, sizeof(param), "arg%d", i);
		name = gctl_get_asciiparam(req, param);
		if (name == NULL) {
			gctl_error(req, "No 'arg%d' argument", i);
			return;
		}
		if (strncmp(name, "/dev/", strlen("/dev/")) == 0)
			name += strlen("/dev/");
		pp = g_provider_by_name(name);
		if (pp == NULL || pp->geom->class != mp) {
			G_LOGSTOR_DEBUG(1, "Provider %s is invalid.", name);
			gctl_error(req, "Provider %s is invalid.", name);
			return;
		}
		sc = pp->geom->softc;
	}
}

static void
g_logstor_ctlreq(struct gctl_req *req, struct g_class *mp, const char *verb)
{
	uint32_t *version;

	g_topology_assert();

	version = gctl_get_paraml(req, "version", sizeof(*version));
	if (version == NULL) {
		gctl_error(req, "No '%s' argument.", "version");
		return;
	}
	if (*version != G_LOGSTOR_VERSION) {
		gctl_error(req, "Userland and kernel parts are out of sync.");
		return;
	}

	if (strcmp(verb, "create") == 0) {
		g_logstor_ctl_create(req, mp);
		return;
	} else if (strcmp(verb, "configure") == 0) {
		g_logstor_ctl_configure(req, mp);
		return;
	} else if (strcmp(verb, "destroy") == 0) {
		g_logstor_ctl_destroy(req, mp);
		return;
	} else if (strcmp(verb, "reset") == 0) {
		g_logstor_ctl_reset(req, mp);
		return;
	}

	gctl_error(req, "Unknown verb.");
}

static void
g_logstor_dumpconf(struct sbuf *sb, const char *indent, struct g_geom *gp,
    struct g_consumer *cp, struct g_provider *pp)
{
	struct g_logstor_softc *sc;

	if (pp != NULL || cp != NULL)
		return;
	sc = gp->softc;
}

/*******************************
 *        logstor              *
 *******************************/

/*
Description:
    segment address to sector address
*/
static uint32_t
sega2sa(uint32_t sega)
{
	return sega << SA2SEGA_SHIFT;
}

static int
g_logstor_read_data(struct g_consumer *cp, off_t offset, void *ptr, off_t length)
{
	struct bio *bp;
	int error;

	KASSERT(length > 0 && length >= cp->provider->sectorsize &&
	    length <= MAXPHYS, ("%s: invalid length %jd", __func__,
	    (intmax_t)length));

	bp = g_alloc_bio();
	bp->bio_cmd = BIO_READ;
	bp->bio_done = NULL;
	bp->bio_offset = offset;
	bp->bio_data = ptr;
	bp->bio_length = length;
	g_io_request(bp, cp);
	error = biowait(bp, "glogread");
	g_destroy_bio(bp);

	return (error);
}

static int
g_logstor_read_superblock(struct g_consumer *cp, off_t offset, void *ptr)
{
	int error;

	error = g_access(cp, 1, 0, 0);
	if (error != 0)
		return (error);
	g_topology_unlock();
	
	error = g_logstor_read_data(cp, offset, ptr, SECTOR_SIZE);
	MY_ASSERT(error != 0);
	rw.r_superblock_read++;

	g_topology_lock();
	g_access(cp, -1, 0, 0);

	return error;
}

static int
g_logstor_write_superblock(struct g_consumer *cp, off_t offset, void *ptr)
{
	int error;

	error = g_access(cp, 0, 1, 0);
	if (error != 0)
		return (error);
	g_topology_unlock();
	
	error = g_write_data(cp, offset, ptr, SECTOR_SIZE);
	MY_ASSERT(error != 0);
	rw.r_superblock_read++;

	g_topology_lock();
	g_access(cp,0 , -1, 0);

	return error;
}

/*
Description:
    Write the initialized supeblock to the downstream disk
*/
uint32_t
superblock_init(struct g_logstor_softc *sc, off_t media_size)
{
	int	i;
	int error;
	uint32_t sector_cnt;
	struct _superblock *sb, *sb_out;
	char buf[SECTOR_SIZE] __attribute__ ((aligned));

	sector_cnt = media_size / SECTOR_SIZE;

	sb = &sc->superblock;
	sb->sig = SIG_LOGSTOR;
	sb->ver_major = VER_MAJOR;
	sb->ver_minor = VER_MINOR;

	sb->sb_gen = arc4random();
	sb->seg_cnt = sector_cnt / SECTORS_PER_SEG;
	MY_ASSERT(sizeof(struct _superblock) + sb->seg_cnt < SECTOR_SIZE);
	sb->seg_free_cnt = sb->seg_cnt - SEG_DATA_START;

	// the physical disk must have at least the space for the metadata
	MY_ASSERT(sb->seg_free_cnt * BLOCKS_PER_SEG >
	    (sector_cnt / (SECTOR_SIZE / 4)) * FD_COUNT);

	sb->max_block_cnt =
	    sb->seg_free_cnt * BLOCKS_PER_SEG -
	    (sector_cnt / (SECTOR_SIZE / 4)) * FD_COUNT;
	sb->max_block_cnt *= 0.9;
#if defined(MY_DEBUG)
	printf("%s: sector_cnt %u max_block_cnt %u\n",
	    __func__, sector_cnt, sb->max_block_cnt);
#endif
	// the root sector address for the files
	for (i = 0; i < FD_COUNT; i++) {
		sb->ftab[i] = SECTOR_NULL;	// not allocated yet
	}
	if (sc->seg_age == NULL) {
		sc->seg_age = g_malloc(sc->superblock.seg_cnt, M_WAITOK | M_ZERO);
	}
	memset(sc->seg_age, 0, sb->seg_cnt);
	/*
	    Initially SEG_DATA_START is cold segment and
	    SEG_DATA_START + 1 is hot segment (see logstor_open)
	    Since seg_reclaim_p starts at SEG_DATA_START + 1,
	    we must protect this first cold segment (i.e SEG_DATA_START)
	    from being allocated by setting it's age to 1.
	*/
	sb->seg_alloc_p = SEG_DATA_START;	// start allocate from here
	sb->seg_reclaim_p = SEG_DATA_START;	// start reclaim from here
	//sc->seg_age[SEG_DATA_START] = 1; // protect this segment from being allocated before recycled

	// write out super block
	sb_out = (struct _superblock *)buf;
	memcpy(sb_out, &sc->superblock, sizeof(sc->superblock));
	memcpy(sb_out->seg_age, sc->seg_age, sb->seg_cnt);
	sc->sb_sa = 0;
	error = g_logstor_write_superblock(LIST_FIRST(&sc->sc_geom->consumer), sc->sb_sa * SECTOR_SIZE, sb_out);
	rw.w_superblock_init++;
	sc->sb_modified = false;

	return sb->max_block_cnt;
}

void logstor_init(void)
{
	//struct g_logstor_softc *sc;

}

void
logstor_fini(struct g_logstor_softc *sc)
{
	g_free(sc->seg_age);
}

int
logstor_open(struct g_logstor_softc *sc, struct g_consumer *cp)
{

	if (superblock_read(sc, cp) != 0) {
		superblock_init(sc, cp->provider->mediasize);
	}
	seg_alloc(sc, &sc->seg_sum_cold);
	seg_alloc(sc, &sc->seg_sum_hot);

	sc->data_write_count = sc->other_write_count = 0;
	sc->clean_low_water = CLEAN_WINDOW * 2;
	sc->clean_high_water = sc->clean_low_water + CLEAN_WINDOW * 2;

	file_mod_init(sc);
	//clean_check(sc);

	return 0;
}

void
logstor_close(struct g_logstor_softc *sc)
{

	file_mod_fini(sc);

	seg_sum_write(sc, &sc->seg_sum_cold);
	seg_sum_write(sc, &sc->seg_sum_hot);

	superblock_write(sc);
}

/*
Description:
  Read blocks from logstor

Parameters:
  @offset: disk offset
  @data: data buffer
  @length: data length
*/
int
logstor_read(struct g_logstor_softc *sc, struct bio *bp)
{
	off_t offset;
	off_t length;
	unsigned size;
	uint32_t ba;
	int error;

	offset = bp->bio_offset;
	MY_ASSERT((offset & (SECTOR_SIZE - 1)) == 0);
	length = bp->bio_length;
	MY_ASSERT((length & (SECTOR_SIZE - 1)) == 0);
	ba = offset / SECTOR_SIZE;
	MY_ASSERT(ba < sc->superblock.max_block_cnt);
	size = length / SECTOR_SIZE;

	if (size == 1) {
		error = _logstor_read_one(sc, bp, ba);
	} else {
		error = _logstor_read(sc, bp, ba, size);
	}
	return error;
}

/*
Description:
  Write blocks to logstor

Parameters:
  @offset: disk offset
  @data: data buffer
  @length: data length
*/
int
logstor_write(struct g_logstor_softc *sc, struct bio *bp)
{
	uint32_t ba;	// block address
	int size;	// number of remaining sectors to process
	int error;
	off_t offset = bp->bio_offset;
	off_t length = bp->bio_length;

	MY_ASSERT((offset & (SECTOR_SIZE - 1)) == 0);
	MY_ASSERT((length & (SECTOR_SIZE - 1)) == 0);
	ba = offset / SECTOR_SIZE;
	size = length / SECTOR_SIZE;

	if (size == 1) {
		error = _logstor_write_one(sc, bp, ba, &sc->seg_sum_hot);
	} else {
		error = _logstor_write(sc, bp, ba, size, &sc->seg_sum_hot);
	}
	return error;
}

int logstor_delete(struct g_logstor_softc *sc, struct bio *bp)
{
	off_t offset;
	off_t length;
	uint32_t ba;	// block address
	int size;	// number of remaining sectors to process
	int i;

	offset = bp->bio_offset;
	length = bp->bio_length;
	rw.d_delete_count++;
	MY_ASSERT((offset & (SECTOR_SIZE - 1)) == 0);
	MY_ASSERT((length & (SECTOR_SIZE - 1)) == 0);
	ba = offset / SECTOR_SIZE;
	size = length / SECTOR_SIZE;
	MY_ASSERT(ba < sc->superblock.max_block_cnt);

	if (size == 1) {
		file_write_4byte(sc, FD_ACTIVE, ba, SECTOR_DELETE);
	} else {
		for (i = 0; i<size; i++)
			file_write_4byte(sc, FD_ACTIVE, ba + i, SECTOR_DELETE);
	}
	return (0);
}

/*
Description:
  Read blocks from the logstor

Parameters:
  @ba: block address
  @data: data buffer
  @size: size in unit of block
*/
static int
_logstor_read(struct g_logstor_softc *sc, struct bio *bp,unsigned ba, int size)
{
	unsigned i, count;
	uint32_t start_sa, pre_sa, sa;	// sector address
	caddr_t data;
	struct bio *cbp, *tbp;
	TAILQ_HEAD(, bio) queue = TAILQ_HEAD_INITIALIZER(queue);
	struct g_consumer *cp = LIST_FIRST(&sc->sc_geom->consumer);

	start_sa = pre_sa = file_read_4byte(sc, FD_ACTIVE, ba);
	count = 1;
	data = bp->bio_data;
	for (i = 1; i < size; i++) {
		sa = file_read_4byte(sc, FD_ACTIVE, ba + i);
		if (sa == pre_sa + 1) {
			count++;
			pre_sa = sa;
		} else {
			if (start_sa == SECTOR_NULL || start_sa == SECTOR_DELETE) {
				memset(data, 0, (size_t) count * SECTOR_SIZE);
				bp->bio_completed += (off_t) count * SECTOR_SIZE;
			} else {
				rw.r_logstor_read++;
				cbp = g_clone_bio(bp);
				MY_ASSERT(cbp != NULL);
				cbp->bio_offset = (off_t)start_sa * SECTOR_SIZE;
				cbp->bio_data = data;
				cbp->bio_length = (off_t)count * SECTOR_SIZE;
				TAILQ_INSERT_TAIL(&queue, cbp, bio_queue);
			}
			// set the values for the next write
			data += count * SECTOR_SIZE;
			start_sa = pre_sa = sa;
			count = 1;
		}
	}
	if (start_sa == SECTOR_NULL || start_sa == SECTOR_DELETE) {
		memset(data, 0, (size_t) count * SECTOR_SIZE);
		bp->bio_completed += (off_t) count * SECTOR_SIZE;
	} else {
		rw.r_logstor_read++;
		cbp = g_clone_bio(bp);
		MY_ASSERT(cbp != NULL);
		cbp->bio_offset = (off_t)start_sa * SECTOR_SIZE;
		cbp->bio_data = data;
		cbp->bio_length = count * SECTOR_SIZE;
		TAILQ_INSERT_TAIL(&queue, cbp, bio_queue);
	}
	if (TAILQ_EMPTY(&queue)) {
		g_io_deliver(bp, 0);
	} else {
		TAILQ_FOREACH_SAFE(cbp, &queue, bio_queue, tbp) {
			cbp->bio_done = g_std_done;
			g_io_request(cbp, cp);
		}
	}
	return 0;
}

static int
_logstor_read_one(struct g_logstor_softc *sc, struct bio *bp, uint32_t ba)
{
	uint32_t start_sa;	// sector address
	struct bio *cbp;
	struct g_consumer *cp = LIST_FIRST(&sc->sc_geom->consumer);

	start_sa = file_read_4byte(sc, FD_ACTIVE, ba);
	if (start_sa == SECTOR_NULL || start_sa == SECTOR_DELETE) {
		memset(bp->bio_data, 0, SECTOR_SIZE);
		bp->bio_completed = SECTOR_SIZE;
		g_io_deliver(bp, 0);
	} else {
		rw.r_logstor_read_one++;
		cbp = g_clone_bio(bp);
		MY_ASSERT(cbp != NULL);
		cbp->bio_done = g_std_done;
		cbp->bio_offset = (off_t)start_sa * SECTOR_SIZE;
		g_io_request(cbp, cp);
	}

	return 0;
}

/*
Description:
  Write blocks to logstor

Parameters:
  @ba: block address
  @data: data buffer
  @size: size in unit of block

*/
static int
_logstor_write(struct g_logstor_softc *sc, struct bio *bp,unsigned ba, int size, struct _seg_sum *seg_sum)
{
	uint32_t sa;	// sector address
	int sec_remain;	// number of remaining sectors to process
	int sec_free;	// number of free sectors in current segment
	int i, count;
	struct g_consumer *cp = LIST_FIRST(&sc->sc_geom->consumer);
	caddr_t data;
	struct bio *cbp;

	MY_ASSERT(ba < sc->superblock.max_block_cnt);
	MY_ASSERT(seg_sum->ss_alloc_p < SEG_SUM_OFF);

	sec_remain = size;
	data = bp->bio_data;
	while (sec_remain > 0) {
		sec_free = SEG_SUM_OFF - seg_sum->ss_alloc_p;
		count = sec_remain <= sec_free? sec_remain: sec_free; // min(sec_remain, sec_free)
		sa = sega2sa(seg_sum->ss_soft.sega) + seg_sum->ss_alloc_p;
		MY_ASSERT(sa + count < sc->superblock.seg_cnt * SECTORS_PER_SEG);
		rw.w_logstor_write++;
		//sc->my_write(sa, data, count);
		cbp = g_clone_bio(bp);
		cbp->bio_done = g_std_done;
		cbp->bio_offset = (off_t)sa * SECTOR_SIZE;
		cbp->bio_data = data;
		cbp->bio_length = count * SECTOR_SIZE;
		g_io_request(cbp, cp);
		data += count * SECTOR_SIZE;

		if (sc->cleaner_disabled) // if doing segment cleaning
			sc->other_write_count += count;
		else
			sc->data_write_count += count;

		// record the reverse mapping immediately after the data have been written
		for (i = 0; i < count; i++)
			seg_sum->ss_rm[seg_sum->ss_alloc_p++] = ba + i;

		if (seg_sum->ss_alloc_p == SEG_SUM_OFF)
		{	// current segment is full
			seg_sum_write(sc, seg_sum);
			seg_alloc(sc, seg_sum);
			clean_check(sc);
		}
		// record the forward mapping later after the segment summary block is flushed
		for (i = 0; i < count; i++)
			file_write_4byte(sc, FD_ACTIVE, ba++, sa++);

		sec_remain -= count;
	}

	return 0;
}

static int
_logstor_write_one(struct g_logstor_softc *sc, struct bio *bp, uint32_t ba, struct _seg_sum *seg_sum)
{
	uint32_t sa;	// sector address
	struct bio *cbp;
	struct g_consumer *cp = LIST_FIRST(&sc->sc_geom->consumer);

	MY_ASSERT(ba < sc->superblock.max_block_cnt);
	MY_ASSERT(seg_sum->ss_alloc_p < SEG_SUM_OFF);

	sa = sega2sa(seg_sum->ss_soft.sega) + seg_sum->ss_alloc_p;
	MY_ASSERT(sa < sc->superblock.seg_cnt * SECTORS_PER_SEG);
	//sc->my_write(sa, data, 1);
	cbp = g_clone_bio(bp);
	MY_ASSERT(cbp != NULL);
	cbp->bio_done = g_std_done;
	cbp->bio_offset = (off_t)sa * SECTOR_SIZE;
	g_io_request(cbp, cp);
	
	rw.w_logstor_write_one++;
	if (sc->cleaner_disabled) // if doing segment cleaning
		sc->other_write_count++;
	else
		sc->data_write_count ++;

	// record the reverse mapping immediately after the data have been written
	seg_sum->ss_rm[seg_sum->ss_alloc_p++] = ba;

	if (seg_sum->ss_alloc_p == SEG_SUM_OFF)
	{	// current segment is full
		seg_sum_write(sc, seg_sum);
		seg_alloc(sc, seg_sum);
		clean_check(sc);
	}
	// record the forward mapping later after the segment summary block is flushed
	file_write_4byte(sc, FD_ACTIVE, ba, sa);

	return 0;
}

uint32_t
logstor_get_block_cnt(struct g_logstor_softc *sc)
{
	return sc->superblock.max_block_cnt;
}

unsigned
logstor_get_data_write_count(struct g_logstor_softc *sc)
{
	return sc->data_write_count;
}

unsigned
logstor_get_other_write_count(struct g_logstor_softc *sc)
{
	return sc->other_write_count;
}

unsigned
logstor_get_fbuf_hit(struct g_logstor_softc *sc)
{
	return sc->fbuf_hit;
}

unsigned
logstor_get_fbuf_miss(struct g_logstor_softc *sc)
{
	return sc->fbuf_miss;
}

static void
seg_sum_read(struct g_logstor_softc *sc, struct _seg_sum *seg_sum)
{
	uint32_t sa;

	sa = sega2sa(seg_sum->ss_soft.sega) + SEG_SUM_OFF;
	//sc->my_read(sa, seg_sum, 1);
	g_logstor_read_data(LIST_FIRST(&sc->sc_geom->consumer),
	    (off_t)sa * SECTOR_SIZE, seg_sum,SECTOR_SIZE);
	rw.r_seg_sum_read++;
}

/*
  write out the segment summary
*/
static void
seg_sum_write(struct g_logstor_softc *sc, struct _seg_sum *seg_sum)
{
	uint32_t sa;
	struct g_consumer *cp = LIST_FIRST(&sc->sc_geom->consumer);

	// segment summary is at the end of a segment
	sa = sega2sa(seg_sum->ss_soft.sega) + SEG_SUM_OFF;
	seg_sum->ss_gen = sc->superblock.sb_gen;
	g_write_data(cp, (off_t)sa * SECTOR_SIZE, (void *)seg_sum, SECTOR_SIZE);
	rw.w_seg_sum_write++;
	sc->other_write_count++;
}

static int
superblock_read(struct g_logstor_softc *sc, struct g_consumer *cp)
{
	int	i;
	int	error;
	uint16_t sb_gen;
	struct _superblock *sb_in;
	char buf[2][SECTOR_SIZE] __attribute__ ((aligned));
	
	_Static_assert(sizeof(sb_gen) == sizeof(sc->superblock.sb_gen), "sb_gen");

	error = g_logstor_read_superblock(cp, 0, buf[0]);

	memcpy(&sc->superblock, buf[0], sizeof(sc->superblock));
	if (sc->superblock.sig != SIG_LOGSTOR ||
	    sc->superblock.seg_alloc_p >= sc->superblock.seg_cnt ||
	    sc->superblock.seg_reclaim_p >= sc->superblock.seg_cnt)
		return EINVAL;

	sb_gen = sc->superblock.sb_gen;
	for (i = 1 ; i < SECTORS_PER_SEG; i++) {
		error = g_logstor_read_superblock(cp, i * SECTOR_SIZE, buf[i%2]);

		rw.r_superblock_read++;
		memcpy(&sc->superblock, buf[i%2], sizeof(sc->superblock));
		if (sc->superblock.sig != SIG_LOGSTOR)
			break;
		if (sc->superblock.sb_gen != (uint16_t)(sb_gen + 1)) // IMPORTANT type cast
			break;
		sb_gen = sc->superblock.sb_gen;
	}
	sc->sb_sa = (i - 1);
	sb_in = (struct _superblock *)buf[(i-1)%2];
	memcpy(&sc->superblock, sb_in, sizeof(sc->superblock));
	sc->sb_modified = false;
	if (sc->superblock.sig != SIG_LOGSTOR ||
	    sc->superblock.seg_alloc_p >= sc->superblock.seg_cnt ||
	    sc->superblock.seg_reclaim_p >= sc->superblock.seg_cnt)
		return EINVAL;
	if (sc->seg_age == NULL) {
		sc->seg_age = g_malloc(sc->superblock.seg_cnt, M_WAITOK | M_ZERO);
		MY_ASSERT(sc->seg_age != NULL);
	}
	memcpy(sc->seg_age, sb_in->seg_age, sb_in->seg_cnt);

	return 0;
}

static void
superblock_write(struct g_logstor_softc *sc)
{
	struct _superblock *sb_out;
	struct g_consumer * cp = LIST_FIRST(&sc->sc_geom->consumer);
	char buf[SECTOR_SIZE];

	sc->superblock.sb_gen++;
	if (++sc->sb_sa == SECTORS_PER_SEG)
		sc->sb_sa = 0;
	sb_out = (struct _superblock *)buf;
	memcpy(sb_out, &sc->superblock, sizeof(sc->superblock));
	memcpy(sb_out->seg_age, sc->seg_age, sb_out->seg_cnt);
	
	//sc->my_write(sc->sb_sa, sb_out, 1);
	g_write_data(cp ,(off_t)sc->sb_sa * SECTOR_SIZE, buf, SECTOR_SIZE);
	rw.w_superblock_write++;
	sc->other_write_count++;
}

/*
Description:
  Allocate a segment for writing and store the segment address into
  @ss_soft.sega of @seg_sum and initialize @ss_alloc_p of @seg_sum to 0
*/
static void
seg_alloc(struct g_logstor_softc *sc, struct _seg_sum *seg_sum)
{
	uint32_t sega;
	uint32_t sega_cold, sega_hot;

	sega_cold = sc->seg_sum_cold.ss_soft.sega;
	sega_hot = sc->seg_sum_hot.ss_soft.sega;

again:
	sega = sc->superblock.seg_alloc_p;
	if (++sc->superblock.seg_alloc_p == sc->superblock.seg_cnt)
		sc->superblock.seg_alloc_p = SEG_DATA_START;
	MY_ASSERT(sc->superblock.seg_alloc_p < sc->superblock.seg_cnt);
	MY_ASSERT(sc->superblock.seg_alloc_p + 1 != sc->superblock.seg_reclaim_p);
	MY_ASSERT(sega != sega_hot);

	if (sega == sega_cold)
		goto again;

	if (sc->seg_age[sega] != 0) // the segment has not been cleaned
		goto again;

	seg_sum->ss_soft.sega = sega;
	seg_sum->ss_alloc_p = 0;

	sc->superblock.seg_free_cnt--;
	MY_ASSERT(sc->superblock.seg_free_cnt > 0 &&
	    sc->superblock.seg_free_cnt < sc->superblock.seg_cnt);
	
}

/*
Description:
  This function does the following things:
  1. Get the segment address of the segment to reclaim
  2. Read the contents of the segment summary of the reclaimed segment
  3. Count the live blocks in this segment
*/
static void
seg_reclaim_init(struct g_logstor_softc *sc, struct _seg_sum *seg_sum)
{
	uint32_t sega;
	uint32_t sega_cold, sega_hot;

	sega_cold = sc->seg_sum_cold.ss_soft.sega;
	sega_hot = sc->seg_sum_hot.ss_soft.sega;
again:
	sega = sc->superblock.seg_reclaim_p;
	if (++sc->superblock.seg_reclaim_p == sc->superblock.seg_cnt)
		sc->superblock.seg_reclaim_p = SEG_DATA_START;
	MY_ASSERT(sc->superblock.seg_reclaim_p < sc->superblock.seg_cnt);
	MY_ASSERT(sega != sega_hot);
#if 0
	MY_ASSERT(sega != sega_cold);
#else
	if (sega == sega_cold)
		goto again;
#endif

	sc->seg_age[sega]++; // to prevent it from being allocated
	seg_sum->ss_soft.sega = sega;
	seg_sum_read(sc, seg_sum);
	if (sc->seg_age[sega] >= CLEAN_AGE_LIMIT) {
		seg_clean(sc, seg_sum);
		if (sc->superblock.seg_free_cnt > sc->clean_high_water) {
			seg_sum->ss_soft.sega = 0;
			return;
		}
		goto again;
	}
	seg_live_count(sc, seg_sum);
}

/********************
* segment cleaning  *
*********************/

/*
  Input:  seg_sum->ss_soft.seg_sa
  Output: seg_sum->ss_soft.live_count
*/
static void
seg_live_count(struct g_logstor_softc *sc, struct _seg_sum *seg_sum)
{
	int	i;
	uint32_t ba;
	uint32_t seg_sa;
	unsigned live_count = 0;
	struct _fbuf *buf;

	seg_sa = sega2sa(seg_sum->ss_soft.sega);
	for (i = 0; i < seg_sum->ss_alloc_p; i++)
	{
		ba = seg_sum->ss_rm[i];	// get the block address from reverse map
		if (IS_META_ADDR(ba)) {
			if (fbuf_ma2sa(sc, (union meta_addr)ba) == seg_sa + i) { // live metadata
				buf = fbuf_get(sc, (union meta_addr)ba);
				if (!buf->modified && !buf->accessed)
					live_count++;
			}
		} else {
			if (file_read_4byte(sc, FD_ACTIVE, ba) == seg_sa + i) // live data
				live_count++;
		}
	}
	seg_sum->ss_soft.live_count = live_count;
}

static void
seg_clean(struct g_logstor_softc *sc, struct _seg_sum *seg_sum)
{
	uint32_t ba, sa;
	uint32_t seg_sa;	// the sector address of the cleaning segment
	uint32_t sega;
	int	i;
	int error;
	struct _fbuf *fbuf;
	struct g_consumer *cp = LIST_FIRST(&sc->sc_geom->consumer);
	uint32_t buf[SECTOR_SIZE];

	//sega = seg_sum->ss_soft.sega;
	seg_sa = sega2sa(seg_sum->ss_soft.sega);
	for (i = 0; i < seg_sum->ss_alloc_p; i++) {
		ba = seg_sum->ss_rm[i];	// get the block address from reverse map
		if (IS_META_ADDR(ba)) {
			sa = fbuf_ma2sa(sc, (union meta_addr)ba); 
			if (sa == seg_sa + i) { // live metadata
				fbuf = fbuf_get(sc, (union meta_addr)ba);
				if (!fbuf->modified) {
					// Set it as modified and the buf
					// will be flushed to disk eventually.
					fbuf->modified = true;
					sc->fbuf_modified_count++;
					if (!fbuf->accessed)
						fbuf_flush(sc, fbuf, &sc->seg_sum_cold);
				}
			}
		} else {
			sa = file_read_4byte(sc, FD_ACTIVE, ba); 
			if (sa == seg_sa + i) { // live data
				//sc->my_read(seg_sa + i, buf, 1);
				error = g_logstor_read_data(cp, (off_t)seg_sa * SECTOR_SIZE, buf, SECTOR_SIZE);
				rw.r_gc_seg_clean++;
				_logstor_write_one(ba, (char *)buf, &sc->seg_sum_cold);
				g_write_data(cp, off_t offset,void * ptr,off_t length)
			}
		}
	}
	sega = seg_sum->ss_soft.sega;
	sc->seg_age[sega] = 0; // It's cleaned
	sc->superblock.seg_free_cnt++;
	//gc_trim(seg_sa);
}

static void
cleaner(struct g_logstor_softc *sc)
{
	struct _seg_sum *seg, *seg_to_clean, *seg_prev_head;
	unsigned live_count, live_count_min, live_count_avg;
	int	i;

//printf("\n%s >>>\n", __func__);
	TAILQ_INIT(&sc->cc_head);
	for (i = 0; i < CLEAN_WINDOW; i++) {
		seg = &sc->clean_candidate[i];
		seg_reclaim_init(sc, seg);
		if (seg->ss_soft.sega == 0) // reached the clean_high_water
			goto exit;
		TAILQ_INSERT_TAIL(&sc->cc_head, seg, ss_soft.queue);
	}

	seg_prev_head = NULL;
	for (;;) {
		// find the hottest segment
		live_count_min = -1; // the maximum unsigned integer
		live_count_avg = 0;
		TAILQ_FOREACH(seg, &sc->cc_head, ss_soft.queue) {
			live_count = seg->ss_soft.live_count;
			live_count_avg += live_count;
			if (live_count < live_count_min) {
				live_count_min = live_count;
				seg_to_clean = seg;
			}
		}
		live_count_avg = (live_count_avg - live_count_min) / (CLEAN_WINDOW - 1);
		seg = NULL; // the head has not been processed
clean:
		TAILQ_REMOVE(&sc->cc_head, seg_to_clean, ss_soft.queue);
		// clean the segment with min live data blocks
		// or the first segment in @cc_head
		seg_clean(sc, seg_to_clean);
		if (sc->superblock.seg_free_cnt > sc->clean_high_water) // reached the clean_high_water
			goto exit;
reclaim_init:
		// init @seg_to_clean with the next segment to reclaim
		seg_reclaim_init(sc, seg_to_clean);
		if (seg_to_clean->ss_soft.sega == 0)  // reached the clean_high_water
			goto exit;
		TAILQ_INSERT_TAIL(&sc->cc_head, seg_to_clean, ss_soft.queue);

		if (seg != NULL) // the head has been processed
			continue;

		// keep the CLEAN_WINDOW moving by cleaning the head of
		// cc_head if it has not been selected for cleaning for certain times
		seg = TAILQ_FIRST(&sc->cc_head);
		if (seg == seg_prev_head) {
			seg_prev_head = TAILQ_NEXT(seg, ss_soft.queue);
			live_count = seg->ss_soft.live_count;
			if (live_count >= live_count_avg) { // Don't clean it, age it.
				uint32_t sega = seg->ss_soft.sega;
				sc->seg_age[sega]++;
				seg_to_clean = seg;
				TAILQ_REMOVE(&sc->cc_head, seg_to_clean, ss_soft.queue);
				goto reclaim_init;
			} else {
				seg_to_clean = seg;
				goto clean;
			}
		} else
			seg_prev_head = seg;
	}
exit:;
	TAILQ_FOREACH(seg, &sc->cc_head, ss_soft.queue) {
		live_count = seg->ss_soft.live_count;
		if (live_count < BLOCKS_PER_SEG * 0.5)
			seg_clean(sc, seg);
	}
//printf("%s <<<\n", __func__);
}

static inline void
cleaner_enable(struct g_logstor_softc *sc)
{
	MY_ASSERT(sc->cleaner_disabled != 0);
	sc->cleaner_disabled--;
}

static inline void
cleaner_disable(struct g_logstor_softc *sc)
{
	MY_ASSERT(sc->cleaner_disabled <= 2);
	sc->cleaner_disabled++;
}

static void
clean_check(struct g_logstor_softc *sc)
{
	if (sc->superblock.seg_free_cnt <= sc->clean_low_water && !sc->cleaner_disabled) {
		cleaner_disable(sc);	// disable gargabe collection
		cleaner(sc);
		cleaner_enable(sc); // enable gargabe collection
	} 
}

/*********************************************************
 * The file buffer and indirect block cache              *
 *   Cache the the block to sector address translation   *
 *********************************************************/

/*
  Initialize metadata file buffer
*/
static void
file_mod_init(struct g_logstor_softc *sc)
{
	unsigned i;

	sc->fbuf_hit = sc->fbuf_miss = 0;
	sc->fbuf_count = sc->superblock.max_block_cnt / (SECTOR_SIZE / 4) * fbuf_ratio;
	sc->fbuf_modified_count = 0;
#if defined(MY_DEBUG)
	sc->cir_queue_cnt = sc->fbuf_count;
#endif

	for (i = 0; i < FILE_BUCKET_COUNT; i++)
		LIST_INIT(&sc->fbuf_bucket[i]);

	sc->fbuf = g_malloc(sizeof(*sc->fbuf) * sc->fbuf_count, M_WAITOK | M_ZERO);
	MY_ASSERT(sc->fbuf != NULL);
#if 0
	sc->fbuf_accessed = malloc(sc->fbuf_count/8);
	MY_ASSERT(sc->fbuf_accessed != NULL);
	sc->fbuf_modified = malloc(sc->fbuf_count/8);
	MY_ASSERT(sc->fbuf_modified != NULL);
	sc->fbuf_on_cir_queue = malloc(sc->fbuf_count/8);
	MY_ASSERT(sc->fbuf_on_cir_queue != NULL);
#endif
	for (i = 0; i < sc->fbuf_count; i++) {
		sc->fbuf[i].cir_queue.prev = &sc->fbuf[i-1];
		sc->fbuf[i].cir_queue.next = &sc->fbuf[i+1];
		sc->fbuf[i].parent = NULL;
		sc->fbuf[i].on_cir_queue = true;
		sc->fbuf[i].accessed = false;
		sc->fbuf[i].modified = false;
		// to distribute the file buffer to buckets evenly 
		// use @i as the key when the tag is META_INVALID
		sc->fbuf[i].ma.uint32 = META_INVALID;
		fbuf_hash_insert(sc, &sc->fbuf[i], i);
	}
	// fix the circular queue for the first and last buffer
	sc->fbuf[0].cir_queue.prev = &sc->fbuf[sc->fbuf_count-1];
	sc->fbuf[sc->fbuf_count-1].cir_queue.next = &sc->fbuf[0];
	sc->cir_buffer_head = &sc->fbuf[0]; // point to the circular queue

	for (i = 0; i < META_LEAF_DEPTH; i++)
		LIST_INIT(&sc->indirect_head[i]); // point to active indirect blocks with depth i
}

static void
file_mod_flush(struct g_logstor_softc *sc)
{
	struct _fbuf	*buf;
	int	i;
	unsigned count = 0;

//printf("%s: modified count before %d\n", __func__, sc->fbuf_modified_count);
	buf = sc->cir_buffer_head;
	do {
		MY_ASSERT(buf->on_cir_queue);
		if (buf->modified) {
			fbuf_flush(sc, buf, &sc->seg_sum_hot);
			count++;
		}
		buf = buf->cir_queue.next;
	} while (buf != sc->cir_buffer_head);
	
	// process active indirect blocks
	for (i = META_LEAF_DEPTH - 1; i >= 0; i--)
		LIST_FOREACH(buf, &sc->indirect_head[i], indir_queue) {
			MY_ASSERT(buf->on_cir_queue == false);
			if (buf->modified) {
				fbuf_flush(sc, buf, &sc->seg_sum_hot);
				count++;
			}
		}
//printf("%s: modified count after %d\n", __func__, sc->fbuf_modified_count);
//printf("%s: flushed count %u\n", __func__, count);
}

static void
file_mod_fini(struct g_logstor_softc *sc)
{
	file_mod_flush(sc);
}

/*
Description:
 	Get the sector address of the corresponding @ba in @file

Parameters:
	@fd: file descriptor
	@ba: block address

Return:
	The sector address of the @ba
*/
static uint32_t
file_read_4byte(struct g_logstor_softc *sc, uint8_t fd, uint32_t ba)
{
	uint8_t	*fbd;	// point to file buffer data
	uint32_t	offset;	// the offset within the file buffer data

	MY_ASSERT((ba & 0xc0000000u) == 0);
	fbd = file_access(sc, fd, ba << 2, &offset, false);
	return *((uint32_t *)(fbd + offset));
}

/*
Description:
 	Set the mapping of @ba to @sa in @file

Parameters:
	@fd: file descriptor
	@ba: block address
	@sa: sector address
*/
static void
file_write_4byte(struct g_logstor_softc *sc, uint8_t fd, uint32_t ba, uint32_t sa)
{
	uint8_t	*fbd;	// point to file buffer data
	uint32_t	offset;	// the offset within the file buffer data

	MY_ASSERT((ba & 0xc0000000u) == 0);
	fbd = file_access(sc, fd, ba << 2, &offset, true);
	*((uint32_t *)(fbd + offset)) = sa;
}

/*
Description:
    The metadata is cached in memory. This function returns the address
    of the metadata in memory for the forward mapping of the block @ba

Parameters:
	@fd: file descriptor
	@ba: block address
	@offset: the offset within the file buffer data
	@bl_write: true for write access, false for read access

Return:
	the address of the file buffer data
*/
static uint8_t *
file_access(struct g_logstor_softc *sc, uint8_t fd, uint32_t offset, uint32_t *buf_off, bool bl_write)
{
	union meta_addr	ma;		// metadata address
	struct _fbuf	*buf;

	*buf_off = offset & 0xfffu;

	// convert to metadata address from (@fd, @offset)
	ma.uint32 = META_BASE + (offset >> 12); // also set .index, .depth and .fd to 0
	ma.depth = META_LEAF_DEPTH;
	ma.fd = fd;
	buf = fbuf_get(sc, ma);
	buf->accessed = true;
	if (!buf->modified && bl_write) {
		buf->modified = true;
		sc->fbuf_modified_count++;
	}

	return (uint8_t *)buf->data;
}

static unsigned
ma_index_get(union meta_addr ma, unsigned depth)
{
	unsigned index;

	index = ma.uint32;
	switch (depth) {
	case 0:
		index >>= 10;
		break;
	case 1:
		break;
	default:
		MY_PANIC();
	}
	return (index & 0x3ffu);
}

static void
ma_index_set(union meta_addr *ma, unsigned depth, unsigned index)
{

	MY_ASSERT(depth < META_LEAF_DEPTH);
	MY_ASSERT(index < 1024);

	switch (depth) {
	case 0:
		index <<= 10;
		ma->uint32 &= 0xfff003ffu;
		break;
	case 1:
		ma->uint32 &= 0xfffffc00u;
		break;
	default:
		MY_PANIC();
	}
	ma->uint32 |= index;
}

#if 0
static uint32_t
fbuf_ma2sa(union meta_addr ma)
{
	struct _fbuf *pbuf;
	int pindex;		//index in the parent indirect block
	union meta_addr pma;	// parent's metadata address
	uint32_t sa;

	pma = ma;
	switch (ma.depth)
	{
	case 0:
		sa = sc->superblock.ftab[ma.fd];
		break;
	case 1:
		pindex = ma_index_get(ma, 0);
		//ma_index_set(&ma, 0, 0);
		//ma_index_set(&ma, 1, 0);
		pma.index = 0; // optimization of the above 2 statements
		pma.depth = 0; // i.e. ma.depth - 1
		goto get_sa;
	case 2:
		pindex = ma_index_get(ma, 1);
		ma_index_set(&pma, 1, 0);
		pma.depth = 1; // i.e. ma.depth - 1
get_sa:
		pbuf = fbuf_get(sc, pma);
		sa = pbuf->data[pindex];
		break;
	default:
		MY_PANIC();
	}
	return sa;
}
#else
static uint32_t
fbuf_ma2sa(struct g_logstor_softc *sc, union meta_addr ma)
{
	struct _fbuf *buf, *pbuf;
	int pindex;		//index in the parent indirect block
	uint32_t sa;

	switch (ma.depth)
	{
	case 0:
		sa = sc->superblock.ftab[ma.fd];
		break;
	case 1:
	case 2:
		buf = fbuf_get(sc, ma);
		pbuf = buf->parent;
		pindex = ma_index_get(ma, ma.depth - 1);
		sa = pbuf->data[pindex];
		break;
	default:
		MY_PANIC();
	}
	return sa;
}
#endif

static void
fbuf_hash_insert(struct g_logstor_softc *sc, struct _fbuf *buf, unsigned key)
{
	unsigned hash;
	struct _fbuf_bucket *bucket;

	hash = key % FILE_BUCKET_COUNT;
	bucket = &sc->fbuf_bucket[hash];
	LIST_INSERT_HEAD(bucket, buf, buffer_bucket_queue);
}

#if defined(MY_DEBUG)
static void
fbuf_queue_check(struct g_logstor_softc *sc)
{
	struct _fbuf *buf;
	unsigned i;
	unsigned total, indir_cnt[META_LEAF_DEPTH];

	buf = sc->cir_buffer_head;
	MY_ASSERT(buf != NULL);
	total = 0;
	do  {
		++total;
		MY_ASSERT(total <= sc->fbuf_count);
		MY_ASSERT(buf->on_cir_queue);
		buf = buf->cir_queue.next;
	} while (buf != sc->cir_buffer_head);

	for (i = 0; i < META_LEAF_DEPTH; i++)
		indir_cnt[i] = 0;

	for (i = 0; i < META_LEAF_DEPTH ; ++i) {
		buf = LIST_FIRST(&sc->indirect_head[i]);
		while (buf != NULL) {
			++indir_cnt[0];
			MY_ASSERT(indir_cnt[0] <= sc->fbuf_count);
			MY_ASSERT(buf->on_cir_queue == false);
			MY_ASSERT(buf->ma.depth == i);
			buf = LIST_NEXT(buf, indir_queue);
		}
	}

	for (i = 0; i < META_LEAF_DEPTH; i++)
		total += indir_cnt[i];
	
	MY_ASSERT(total == sc->fbuf_count);
}
#endif

/*
    Circular queue insert before
*/
static void
fbuf_cir_queue_insert(struct g_logstor_softc *sc, struct _fbuf *buf)
{
	struct _fbuf *prev;

	prev = sc->cir_buffer_head->cir_queue.prev;
	sc->cir_buffer_head->cir_queue.prev = buf;
	buf->cir_queue.next = sc->cir_buffer_head;
	buf->cir_queue.prev = prev;
	prev->cir_queue.next = buf;
	buf->on_cir_queue = true;
#if defined(MY_DEBUG)
	sc->cir_queue_cnt++;
#endif
}

/*
    Circular queue remove
    Must have at least tow elements on the queue before remove
*/
static void
fbuf_cir_queue_remove(struct g_logstor_softc *sc, struct _fbuf *buf)
{
	struct _fbuf *prev;
	struct _fbuf *next;

	MY_ASSERT(buf->on_cir_queue);
	MY_ASSERT(sc->cir_buffer_head->cir_queue.next != sc->cir_buffer_head);
	MY_ASSERT(sc->cir_buffer_head->cir_queue.prev != sc->cir_buffer_head);
	if (buf == sc->cir_buffer_head)
		sc->cir_buffer_head = sc->cir_buffer_head->cir_queue.next;
	prev = buf->cir_queue.prev;
	next = buf->cir_queue.next;
	prev->cir_queue.next = next;
	next->cir_queue.prev = prev;
	buf->on_cir_queue = false;
#if defined(MY_DEBUG)
	sc->cir_queue_cnt--;
#endif
}

/*
Description:
    Read or write the file buffer with metadata address @ma
*/
static struct _fbuf *
fbuf_get(struct g_logstor_softc *sc, union meta_addr ma)
{
	struct _fbuf *pbuf;	// parent buffer
	struct _fbuf *buf;
	union meta_addr	tma;	// temporary metadata address
	uint32_t sa;	// sector address where the metadata is stored
	unsigned i;
	unsigned index;

	MY_ASSERT(IS_META_ADDR(ma.uint32));
	buf = fbuf_search(sc, ma);
	if (buf != NULL) // cache hit
		return buf;

	// cache miss
	// get the root sector address of the file @ma.fd
	MY_ASSERT(ma.fd < FD_COUNT);
	sa = sc->superblock.ftab[ma.fd];
	pbuf = NULL;	// parent for root is NULL
	tma.uint32 = META_BASE; // also set .index, .depth and .fd to 0
	tma.fd = ma.fd;
	// read the metadata from root to leaf node
	for (i = 0; ; ++i) {	// read the indirect blocks to block cache
		tma.depth = i;
		buf = fbuf_search(sc, tma);
		if (buf == NULL) {
			buf = fbuf_read_and_hash(sc, sa, tma);
			buf->parent = pbuf;
			/*
			  Theoretically the parent's reference count should be
			  incremented here. But if imcremented here, the parent
			  might be reclaimed in the call fbuf_read_and_hash, so
			  it is actually incremented in the previous loop to
			  prevent it from being reclaimed by fbuf_read_and_hash.
			*/
		} else {
			MY_ASSERT(buf->parent == pbuf);
			MY_ASSERT(buf->sa == sa);
			if (pbuf) {
				MY_ASSERT(pbuf->ref_cnt != 1);
				/*
				  The reference count of the parent is always
				  incremented in the previous loop. In this case
				  we don't need to, so decremented it here to
				  compensate the increment in the previous loop.
				*/
				pbuf->ref_cnt--;
			}
		}
		if (i == ma.depth) // reach intended depth
			break;

		if (buf->on_cir_queue) {
			// move it to active indirect block queue
			fbuf_cir_queue_remove(sc, buf);
			LIST_INSERT_HEAD(&sc->indirect_head[i], buf, indir_queue);
			buf->ref_cnt = 0;
		}
		/*
		  Increment the reference count of this buffer to prevent it
		  from being reclaimed by the call to function fbuf_read_and_hash.
		*/
		buf->ref_cnt++;

		index = ma_index_get(ma, i);// the offset of indirect block for next level
		ma_index_set(&tma, i, index);
		sa = buf->data[index];	// the sector address of the next level indirect block
		pbuf = buf;		// @buf is the parent of next level indirect block
	}
#if defined(MY_DEBUG)
	fbuf_queue_check(sc);
#endif
	return buf;
}

/*
Description:
    Use the second chance algorithm to allocate a file buffer
*/
static struct _fbuf *
fbuf_alloc(struct g_logstor_softc *sc)
{
	struct _fbuf *pbuf;	// parent buffer
	struct _fbuf *buf;

	buf = sc->cir_buffer_head;
	do {
		MY_ASSERT(buf->on_cir_queue);
		if (!buf->accessed)
			break;
		buf->accessed = false;	// give this buffer a second chance
		buf = buf->cir_queue.next;
	} while (buf != sc->cir_buffer_head);
	sc->cir_buffer_head = buf->cir_queue.next;
	if (buf->modified)
		fbuf_flush(sc, buf, &sc->seg_sum_hot);

	// set buf's parent to NULL
	pbuf = buf->parent;
	if (pbuf != NULL) {
		MY_ASSERT(pbuf->on_cir_queue == false);
		buf->parent = NULL;
		pbuf->ref_cnt--;
		if (pbuf->ref_cnt == 0) {
			// move it from indirect queue to circular queue
			LIST_REMOVE(pbuf, indir_queue);
			fbuf_cir_queue_insert(sc, pbuf);
			// set @accessed to false so that it will be reclaimed
			// next time by the second chance algorithm
			pbuf->accessed = false;
		}
	}
	return buf;
}

/*
Description:
    Allocate a file buffer, fill it with data at sector address @sa
    and insert it into hash queue with key @ma
*/
static struct _fbuf *
fbuf_read_and_hash(struct g_logstor_softc *sc, uint32_t sa, union meta_addr ma)
{
	int error;
	struct _fbuf *buf;
	struct g_consumer *cp = LIST_FIRST(&sc->sc_geom->consumer);

	buf = fbuf_alloc(sc);

	if (sa == SECTOR_NULL)	// the metadata block does not exist
		memset(buf->data, 0, sizeof(buf->data));
	else {
		//sc->my_read(sa, buf->data, 1);
		error = g_logstor_read_data(cp, (off_t)sa * SECTOR_SIZE, buf->data, SECTOR_SIZE);
		MY_ASSERT(error == 0);
		rw.r_fbuf_read_and_hash++;
		//sc->other_write_count++;
	}

	LIST_REMOVE(buf, buffer_bucket_queue);
	buf->ma = ma;
	fbuf_hash_insert(sc, buf, ma.uint32);
#if defined(MY_DEBUG)
	buf->sa = sa;
#endif
	return buf;
}

static uint32_t
fbuf_write(struct g_logstor_softc *sc, struct _fbuf *buf, struct _seg_sum *seg_sum)
{
	uint32_t	sa;	// sector address
	struct g_consumer *cp = LIST_FIRST(&sc->sc_geom->consumer);
	int error;

	// get the sector address where the block will be written
	MY_ASSERT(seg_sum->ss_alloc_p < SEG_SUM_OFF);
	sa = sega2sa(seg_sum->ss_soft.sega) + seg_sum->ss_alloc_p;
	MY_ASSERT(sa < sc->superblock.seg_cnt * SECTORS_PER_SEG - 1);

	//sc->my_write(sa, buf->data, 1);
	error = g_write_data(cp, (off_t)sa * SECTOR_SIZE, buf->data, SECTOR_SIZE);
	MY_ASSERT(error == 0);
	rw.w_fbuf_write++;
	buf->modified = false;
	sc->fbuf_modified_count--;
	sc->other_write_count++;

	// store the reverse mapping in segment summary
	seg_sum->ss_rm[seg_sum->ss_alloc_p++] = buf->ma.uint32;

	if (seg_sum->ss_alloc_p == SEG_SUM_OFF) { // current segment is full
		seg_sum_write(sc, seg_sum);
		seg_alloc(sc, seg_sum);
		// Don't do segment cleaning when writing out fbuf
	}
	return sa;
}

/*
Description:
    Write the dirty data in file buffer to disk
*/
static void
fbuf_flush(struct g_logstor_softc *sc, struct _fbuf *buf, struct _seg_sum *seg_sum)
{
	struct _fbuf *pbuf;	// buffer parent
	unsigned pindex; // the index in parent indirect block
	uint32_t sa;	// sector address

	MY_ASSERT(buf->modified);
	MY_ASSERT(IS_META_ADDR(buf->ma.uint32));
	/*
	  Must disable segment cleaner until @sa is written out
	*/
	//cleaner_disable();
	sa = fbuf_write(sc, buf, seg_sum);
#if defined(MY_DEBUG)
	buf->sa = sa;
#endif
	pbuf = buf->parent;
	if (pbuf) {
		MY_ASSERT(buf->ma.depth != 0);
		MY_ASSERT(pbuf->ma.depth == buf->ma.depth - 1);
		pindex = ma_index_get(buf->ma, buf->ma.depth - 1);
		pbuf->data[pindex] = sa;
		if (!pbuf->modified) {
			pbuf->modified = true;
			sc->fbuf_modified_count++;
		}
	} else {
		MY_ASSERT(buf->ma.depth == 0);
		MY_ASSERT(buf->ma.fd < FD_COUNT);
		// store the root sector address to the corresponding file table in super block
		sc->superblock.ftab[buf->ma.fd] = sa;
		sc->sb_modified = true;
	}
	//cleaner_enable();
	return;
}

/*
Description:
    Search the file buffer with the tag value of @ma. Return NULL if not found
*/
static struct
_fbuf *
fbuf_search(struct g_logstor_softc *sc, union meta_addr ma)
{
	unsigned	hash;	// hash value
	struct _fbuf_bucket	*bucket;
	struct _fbuf	*buf;

	hash = ma.uint32 % FILE_BUCKET_COUNT;
	bucket = &sc->fbuf_bucket[hash];
	LIST_FOREACH(buf, bucket, buffer_bucket_queue)
#if defined(WYC) //wyc make the code friendly to SI
;
#endif
		if (buf->ma.uint32 == ma.uint32) { // cache hit
			sc->fbuf_hit++;
			return buf;
		}
	sc->fbuf_miss++;
	return NULL;	// cache miss
}



DECLARE_GEOM_CLASS(g_logstor_class, g_logstor);
MODULE_VERSION(geom_logstor, 0);

#if 0
static uint32_t
logstor_sa2ba(uint32_t sa)
{
	uint32_t seg_sa;
	unsigned seg_off;

	seg_sa = sa & ~(SECTORS_PER_SEG - 1);
	seg_off = sa & (SECTORS_PER_SEG - 1);
	MY_ASSERT(seg_off != SEG_SUM_OFF);
	if (seg_sa != sc.seg_sum_cache.ss_soft.ss_cached_sa) {
		sc.my_read(seg_sa + SEG_SUM_OFF, &sc.seg_sum_cache, 1);
		sc.seg_sum_cache.ss_soft.ss_cached_sa = seg_sa;
	}

	return (sc.seg_sum_cache.ss_rm[seg_off]);
}

/*
Description:
    Block address to sector address
*/
static uint32_t
logstor_ba2sa(uint32_t ba)
{
	uint32_t sa;

	if (IS_META_ADDR(ba))
		sa = fbuf_ma2sa((union meta_addr)ba);
	else {
		sa = file_read_4byte(FD_ACTIVE, ba);
	}

	return sa;
}

/*
Description:
  Check the integrity of the logstor
*/
void
logstor_check(void)
{
	uint32_t ba, sa, ba_exp;
	uint32_t max_block;
	uint32_t sa_min;

	printf("%s ...\n", __func__);
	file_mod_flush();
	if (sc.seg_sum_hot.ss_alloc_p != 0)
		seg_sum_write(&sc.seg_sum_hot);
	sa_min = -1;
	max_block = logstor_get_block_cnt();
	for (ba = 0; ba < max_block; ba++) {
		sa = logstor_ba2sa(ba);
		if (sa != SECTOR_NULL) {
			ba_exp = logstor_sa2ba(sa);
			if (ba_exp != ba) {
				if (sa < sa_min)
					sa_min = sa;
				printf("ERROR %s: ba %u sa %u ba_exp %u\n",
				    __func__, ba, sa, ba_exp);
				MY_PANIC();
			}
		}
	}
	printf("%s done. max_block %u\n\n", __func__, max_block);
}

static void
gc_trim(uint32_t sa)
{
	struct stat sb;
	off_t arg[2];

	if (fstat(sc.disk_fd, &sb) == -1)
		g_gate_xlog("fstat(): %s.", strerror(errno));
	if (S_ISCHR(sb.st_mode)) {
		arg[0] = sa * SECTOR_SIZE;
		arg[1] = SEG_SIZE;
		if (ioctl(sc.disk_fd, DIOCGDELETE, arg) == -1) {
			g_gate_xlog("Can't get media size: %s.",
			    strerror(errno));
		}
	}
}

/*********************************
 *  The merge sort for cache  *
 ********************************/
static struct cache_entry *merge_src;	// input source array
static struct cache_entry *merge_dst;	// output destination array
static uint8_t	merge_depth;	// recursive depth

void split_merge(unsigned begin, unsigned end);
void merge(unsigned begin, unsigned middle, unsigned end);

/*
Parameters:
  @src: source input array
  @dst: destination output array
  @n: number of elements in the array
*/void
merge_sort(struct cache_entry *src, struct cache_entry *dst, unsigned n)
{
	if (n < 1)
		return;

	merge_src = src;
	merge_dst = dst;
	merge_depth = 0;

	split_merge(0, n);

#if defined(MY_DEBUG)
	{
		unsigned i;

		/* make sure that it is sorted */
		for (i = 1; i < n ; ++i)
			MY_ASSERT(dst[i].ba > dst[i-1].ba);
	}
#endif
}

/*
Description:
  The merge sort algorithm first splits the array into two smaller arrays.
  It then sorts two array and merge them into one array.
Parameters:
  @begin is inclusive;
  @end is exclusive (merge_src[end] is not in the set)
*/
void
split_merge(unsigned begin, unsigned end)
{
	unsigned	middle;

	merge_depth++;
	if(end - begin == 1) {	// only one element in the array
		if (merge_depth & 1) {	// depth is odd
			merge_dst[begin].ba = merge_src[begin].ba;
			merge_dst[begin].sa = merge_src[begin].sa;
		}
		goto end;
	}

	// recursively split runs into two halves until run size == 1,
	// then merge them and return back up the call chain
	middle = (end + begin) / 2;
	split_merge(begin,  middle);	// split / merge left  half
	split_merge(middle, end);	// split / merge right half
	merge(begin, middle, end);
end:
	merge_depth--;
}

/*
 Left source half is  [ begin:middle-1].
 Right source half is [middle:end-1   ].
 Result is            [ begin:end-1   ].
*/
void
merge(unsigned begin, unsigned middle, unsigned end)
{
	unsigned	i, j, k;
	struct cache_entry	*from;
	struct cache_entry	*to;

	if (merge_depth & 1) {	// depth is odd, from merge_src to merge_dst
		from = merge_src;
		to = merge_dst;
	} else {		// depth is even, from merge_dst to merge_src
		from = merge_dst;
		to = merge_src;
	}

	// While there are elements in the left or right runs
	i = begin;
	j = middle;
	for (k = begin; k < end; k++) {
		// If left run head exists and is <= existing right run head.
		if (i < middle && (j >= end || from[i].ba <= from[j].ba)) {
			to[k].ba  = from[i].ba;
			to[k].sa = from[i].sa;
			i = i + 1;
		} else {
			to[k].ba  = from[j].ba;
			to[k].ba  = from[j].ba;
			to[k].sa = from[j].sa;
			j = j + 1;
		}
	}
}
#endif
