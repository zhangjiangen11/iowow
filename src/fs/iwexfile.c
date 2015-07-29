#include "iwexfile.h"
#include "log/iwlog.h"
#include "iwcfg.h"

#include <pthread.h>
#include <sys/mman.h>

struct _MMAPSLOT;
typedef struct IWFS_EXFILE_IMPL {
    IWFS_FILE   *file;              /**< Underlying file pointer */
    off_t       fsize;              /**< Current file size */
    off_t       psize;              /**< System page size */
    pthread_rwlock_t *rwlock;       /**< Thread RW lock */
    IW_EXFILE_RSPOLICY  rspolicy;   /**< File resize policy function ptr. */
    void *rspolicy_ctx;             /**< Custom opaque data for policy functions. */
    struct _MMAPSLOT *mmslots;      /**< Memory mapping slots. */
    int         use_locks;          /**< Use rwlocks to guard method access */
    iwfs_omode  omode;              /**< File open mode. */
    HANDLE      fh;                 /**< File handle */
} _IWXF;

typedef struct _MMAPSLOT {
    off_t off;                      /**< Offset to a memory mapped region */
    size_t len;                     /**< Actual size of memory mapped region. */
    size_t maxlen;                  /**< Maximum length of memory mapped region */
#ifdef _WIN32
    HANDLE mmapfh;                      /**< Win32 file mapping handle. */
#endif
    struct _MMAPSLOT *prev;    /**< Previous mmap slot. */
    struct _MMAPSLOT *next;    /**< Next mmap slot. */
    uint8_t *mmap;                    /**< Pointer to a mmaped address space in the case if file data is memory mapped. */
} _MMAPSLOT;

static iwrc _exfile_initlocks(IWFS_EXFILE *f);
static iwrc _exfile_rwlock(IWFS_EXFILE *f, int wl);
static iwrc _exfile_unlock(IWFS_EXFILE *f);
static iwrc _exfile_unlock2(_IWXF *impl);
static iwrc _exfile_destroylocks(_IWXF *impl);
static iwrc _exfile_ensure_size(struct IWFS_EXFILE* f, off_t size);
static iwrc _exfile_ensure_size_impl(struct IWFS_EXFILE* f, off_t sz);
static iwrc _exfile_truncate(struct IWFS_EXFILE* f, off_t size);
static iwrc _exfile_truncate_impl(struct IWFS_EXFILE* f, off_t size);
static iwrc _exfile_add_mmap(struct IWFS_EXFILE* f, off_t off, size_t maxlen);
static iwrc _exfile_get_mmap(struct IWFS_EXFILE* f, off_t off, uint8_t **mmap, size_t *sp);
static iwrc _exfile_remove_mmap(struct IWFS_EXFILE* f, off_t off);
static iwrc _exfile_sync_mmap(struct IWFS_EXFILE* f, off_t off, int flags);
static iwrc _exfile_sync(struct IWFS_EXFILE *f, const IWFS_FILE_SYNC_OPTS *opts);
static iwrc _exfile_write(struct IWFS_EXFILE *f, off_t off, const void *buf, size_t siz, size_t *sp);
static iwrc _exfile_read(struct IWFS_EXFILE *f, off_t off, void *buf, size_t siz, size_t *sp);
static iwrc _exfile_close(struct IWFS_EXFILE *f);
static iwrc _exfile_initmmap(struct IWFS_EXFILE *f);
static iwrc _exfile_initmmap_slot(struct IWFS_EXFILE *f, _MMAPSLOT *slot);
static off_t _exfile_default_spolicy(off_t size, struct IWFS_EXFILE *f, void *ctx);


static iwrc _exfile_sync(struct IWFS_EXFILE *f, const IWFS_FILE_SYNC_OPTS *opts) {

    return 0;
}

static iwrc _exfile_write(struct IWFS_EXFILE *f, off_t off,
                          const void *buf, size_t siz, size_t *sp) {

    return 0;
}

static iwrc _exfile_read(struct IWFS_EXFILE *f, off_t off,
                         void *buf, size_t siz, size_t *sp) {

    return 0;
}


static iwrc _exfile_state(struct IWFS_EXFILE *f, IWFS_EXFILE_STATE* state) {
    int rc = _exfile_rwlock(f, 0);
    if (rc) {
        return rc;
    }
    IWRC(f->impl->file->state(f->impl->file, &state->fstate), rc);
    state->fsize = f->impl->fsize;
    IWRC(_exfile_unlock(f), rc);
    return rc;
}

static iwrc _exfile_close(struct IWFS_EXFILE *f) {
    assert(f);
    iwrc rc = _exfile_rwlock(f, 1);
    if (rc) {
        return rc;
    }
    _IWXF *impl = f->impl;
    IWRC(impl->file->close(impl->file), rc);
    f->impl = 0;
    IWRC(_exfile_unlock2(impl), rc);
    IWRC(_exfile_destroylocks(impl), rc);
    free(impl);
    return rc;
}

static iwrc _exfile_ensure_size(struct IWFS_EXFILE* f, off_t sz) {
    iwrc rc = _exfile_rwlock(f, 0);
    if (rc) {
        return rc;
    }
    off_t nsz = sz;
    _IWXF *impl = f->impl;
    
    //impl->fsize
    
    
    
     IWRC(_exfile_unlock2(impl), rc);
    return rc;
}

// Assumed:
//  +write lock
static iwrc _exfile_ensure_size_impl(struct IWFS_EXFILE* f, off_t sz) {
    iwrc rc = 0;
    off_t nsz = sz;
    
    
    return rc;
}

static iwrc _exfile_truncate(struct IWFS_EXFILE* f, off_t sz) {
    iwrc rc = _exfile_rwlock(f, 1);
    if (rc) {
        return rc;
    }
    rc = _exfile_truncate_impl(f, sz);
    IWRC(_exfile_unlock(f), rc);
    return rc;
}

// Assumed:
//  +write lock
static iwrc _exfile_truncate_impl(struct IWFS_EXFILE* f, off_t size) {
    assert(f && f->impl);
    iwrc rc = 0;
    _IWXF *impl = f->impl;
    iwfs_omode omode = impl->omode;
    off_t old_size = impl->fsize;

    size = IW_ROUNDUP(size, impl->psize);
    if (impl->fsize < size) {
        if (!(omode & IWFS_OWRITE)) {
            return IW_ERROR_READONLY;
        }
        rc = iwp_ftruncate(impl->fh, size);
        if (rc) {
            goto truncfail;
        }
        rc = _exfile_initmmap(f);
    } else if (impl->fsize > size) {
        if (!(omode & IWFS_OWRITE)) {
            return IW_ERROR_READONLY;
        }
        impl->fsize = size;
        rc = _exfile_initmmap(f);
        if (rc) {
            goto truncfail;
        }
        rc = iwp_ftruncate(impl->fh, size);
        if (rc) {
            goto truncfail;
        }
    }
    return rc;

truncfail:
    //restore old size
    impl->fsize = old_size;
    //try to reinit mmap slots
    IWRC(_exfile_initmmap(f), rc);
    return rc;
}

// Assumed:
//  +write lock
static iwrc _exfile_initmmap(struct IWFS_EXFILE *f) {
    assert(f);
    iwrc rc = 0;
    _IWXF *impl = f->impl;
    assert(!(impl->fsize & (impl->psize - 1)));
    _MMAPSLOT *s = impl->mmslots;
    while (s) {
        rc = _exfile_initmmap_slot(f, s);
        if (rc) {
            break;
        }
        s = s->next;
    }
    return rc;
}

// Assumed:
//  +write lock
static iwrc _exfile_initmmap_slot(struct IWFS_EXFILE *f, _MMAPSLOT *s) {
    assert(f && s);
    size_t nlen;
    _IWXF *impl = f->impl;
    if (s->off >= impl->fsize) {
        nlen = 0;
    } else {
        nlen = MIN(s->maxlen, impl->fsize - s->off);
    }
    if (nlen == s->len) {
        return 0;
    }
    if (s->len) { // unmap me first
        assert(s->mmap);
        if (munmap(s->mmap, s->len) == -1) {
            s->len = 0;
            return iwrc_set_errno(IW_ERROR_ERRNO, errno);
        }
        s->len = 0;
    }
    if (nlen > 0) {
        int flags = MAP_SHARED;
        int prot = (impl->omode & IWFS_OWRITE) ? (PROT_WRITE | PROT_READ) : (PROT_READ);
        s->len = nlen;
        s->mmap = mmap(0, s->len, prot, flags, impl->fh, s->off);
        if (s->mmap == MAP_FAILED) {
            return iwrc_set_errno(IW_ERROR_ERRNO, errno);
        }
    }
    return 0;
}

static iwrc _exfile_add_mmap(struct IWFS_EXFILE* f, off_t off, size_t maxlen) {
    assert(f);
    assert(off >= 0);

    iwrc rc;
    size_t tmp;
    _MMAPSLOT *ns;

    rc = _exfile_rwlock(f, 1);
    if (rc) {
        return rc;
    }
    _IWXF *impl = f->impl;
    if (off & (impl->psize)) {
        rc = IW_ERROR_NOT_ALIGNED;
        goto finish;
    }
    if (OFF_T_MAX - off < maxlen) {
        maxlen = OFF_T_MAX - off;
    }
    tmp = IW_ROUNDUP(maxlen, impl->psize);
    if (tmp < maxlen || OFF_T_MAX - off < tmp) {
        maxlen = IW_ROUNDOWN(maxlen, impl->psize);
    } else {
        maxlen = tmp;
    }
    if (!maxlen) {
        rc = IW_ERROR_OUT_OF_BOUNDS;
        goto finish;
    }
    assert(!(maxlen & (impl->psize - 1)));
    ns = calloc(1, sizeof(*ns));
    if (!ns) {
        rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
    }
    ns->off = off;
    ns->len = 0;
    ns->maxlen = maxlen;
#ifdef _WIN32
    ns->mmapfh = INVALIDHANDLE;
#endif
    rc = _exfile_initmmap_slot(f, ns);
    if (rc) {
        goto finish;
    }
    if (impl->mmslots == 0) {
        ns->next = 0;
        ns->prev = ns;
        impl->mmslots = ns;
    } else {
        _MMAPSLOT *s = impl->mmslots;
        while (s) {
            off_t e1 = s->off + s->maxlen;
            off_t e2 = ns->off + ns->maxlen;
            if (IW_RANGES_OVERLAP(s->off, e1, ns->off, e2)) {
                rc = IWFS_ERROR_MMAP_OVERLAP;
                goto finish;
            }
            if (ns->off < s->off) {
                break;
            }
            s = s->next;
        }
        if (s) { // insert before
            ns->next = s;
            ns->prev = s->prev;
            s->prev = ns;
            if (s == impl->mmslots) {
                impl->mmslots = ns;
                ns->prev->next = 0;
            }
        } else { // insert at the end
            s = impl->mmslots;
            ns->next = 0;
            ns->prev = s->prev;
            s->prev->next = ns;
            s->prev = ns;
        }
    }
finish:
    if (rc) {
        if (ns) {
            if (impl->mmslots == ns) {
                impl->mmslots = 0;
            }
            free(ns);
        }
    }
    IWRC(_exfile_unlock(f), rc);
    return rc;
}

iwrc _exfile_get_mmap(struct IWFS_EXFILE* f, off_t off, uint8_t **mm, size_t *sp) {
    assert(f);
    assert(off >= 0);
    assert(mm);

    if (sp) *sp = 0;
    *mm = 0;

    iwrc rc = _exfile_rwlock(f, 0);
    if (rc) {
        return rc;
    }
    _IWXF *impl = f->impl;
    _MMAPSLOT *s = impl->mmslots;
    while (s) {
        if (s->off == off) {
            if (!s->len) {
                rc = IWFS_ERROR_NOT_MMAPED;
                break;
            }
            *mm = s->mmap;
            if (sp) {
                *sp = s->len;
            }
            break;
        }
        s = s->next;
    }
    IWRC(_exfile_unlock(f), rc);
    return rc;
}

static iwrc _exfile_remove_mmap(struct IWFS_EXFILE* f, off_t off) {
    assert(f);
    assert(off >= 0);

    iwrc rc = _exfile_rwlock(f, 1);
    if (rc) {
        return rc;
    }
    _IWXF *impl = f->impl;
    _MMAPSLOT *s = impl->mmslots;
    while (s) {
        if (s->off == off) {
            break;
        }
        s = s->next;
    }
    if (!s) {
        rc = IWFS_ERROR_NOT_MMAPED;
        goto finish;
    }
    if (impl->mmslots == s) {
        if (s->next) {
            s->next->prev = s->prev;
        }
        impl->mmslots = s->next;
    } else if (impl->mmslots->prev == s) {
        s->prev->next = 0;
        impl->mmslots->prev = s->prev;
    } else {
        s->prev->next = s->next;
        s->next->prev = s->prev;
    }
    if (s->len) {
        if (munmap(s->mmap, s->len)) {
            rc = iwrc_set_errno(IW_ERROR_ERRNO, errno);
            goto finish;
        }
    }
finish:
    IWRC(_exfile_unlock(f), rc);
    return rc;
}

static iwrc _exfile_sync_mmap(struct IWFS_EXFILE* f, off_t off, int _flags) {
    assert(f);
    assert(off >= 0);
    
    iwrc rc = _exfile_rwlock(f, 0);
    if (rc) {
        return rc;
    }
    _IWXF *impl = f->impl;
    _MMAPSLOT *s = impl->mmslots;
    while (s) {
        if (s->off == off) {
            if (s->len == 0) {
                rc = IWFS_ERROR_NOT_MMAPED;
                break;
            }
            if (s->mmap && s->mmap != MAP_FAILED) {
                int flags = _flags;
                if (!flags) flags = MS_ASYNC;
                if (msync(s->mmap, s->len, flags)) {
                    rc = iwrc_set_errno(IW_ERROR_IO_ERRNO, errno);
                    break;
                }
            }
        }
        s = s->next;
    }
    if (!s) {
        rc = IWFS_ERROR_NOT_MMAPED;
    }
    
    IWRC(_exfile_unlock(f), rc);
    return rc;
}

iwrc iwfs_exfile_open(IWFS_EXFILE *f,
                      const IWFS_EXFILE_OPTS *opts) {

    assert(f);
    assert(opts);
    assert(opts->fopts.path);
    iwrc rc = 0;
    const char *path = opts->fopts.path;

    memset(f, 0, sizeof(*f));
    _IWXF *impl = f->impl = calloc(1, sizeof(*f->impl));
    if (!f->impl) {
        return iwrc_set_errno(IW_ERROR_ALLOC, errno);
    }

    f->close = _exfile_close;
    f->read = _exfile_read;
    f->write = _exfile_write;
    f->sync = _exfile_sync;
    f->state = _exfile_state;

    f->ensure_size = _exfile_ensure_size;
    f->truncate = _exfile_truncate;
    f->add_mmap = _exfile_add_mmap;
    f->get_mmap = _exfile_get_mmap;
    f->remove_mmap = _exfile_remove_mmap;
    f->sync_mmap = _exfile_sync_mmap;

    impl->psize = iwp_page_size();
    impl->rspolicy = opts->rspolicy ? opts->rspolicy : _exfile_default_spolicy;
    impl->rspolicy_ctx = opts->rspolicy_ctx;
    impl->use_locks = opts->use_locks;

    rc = _exfile_initlocks(f);
    if (rc) {
        goto finish;
    }
    rc = iwfs_file_open(impl->file, &opts->fopts);
    if (rc) {
        goto finish;
    }
    IWP_FILE_STAT fstat;
    rc = iwp_fstat(path, &fstat);
    if (rc) {
        goto finish;
    }
    impl->fsize = fstat.size;

    IWFS_FILE_STATE fstate;
    rc = impl->file->state(impl->file, &fstate);
    impl->omode = fstate.opts.open_mode;
    impl->fh = fstate.fh;

    if (impl->fsize < opts->initial_size) {
        rc = _exfile_truncate_impl(f, opts->initial_size);
    } else if (impl->fsize & (impl->psize - 1)) { //not a page aligned
        rc = _exfile_truncate_impl(f, impl->fsize);
    }

finish:
    if (rc) {
        if (f->impl) {
            _exfile_destroylocks(f->impl);
            free(f->impl);
            f->impl = 0;
        }
    }
    return rc;
}

static off_t _exfile_default_spolicy(off_t size, struct IWFS_EXFILE *f, void *ctx) {
    return size;
}

static iwrc _exfile_initlocks(IWFS_EXFILE *f) {
    assert(f && f->impl);
    assert(!f->impl->rwlock);
    _IWXF *impl = f->impl;
    if (!impl->use_locks) {
        return 0;
    }
    impl->rwlock = calloc(1, sizeof(*impl->rwlock));
    if (impl->rwlock) {
        return iwrc_set_errno(IW_ERROR_ALLOC, errno);
    }
    int rv = pthread_rwlock_init(impl->rwlock, (void*) 0);
    if (rv) {
        free(impl->rwlock);
        impl->rwlock = 0;
        return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rv);
    }
    return 0;
}

static iwrc _exfile_destroylocks(_IWXF *impl) {
    if (!impl) return IW_ERROR_INVALID_STATE;
    if (!impl->rwlock) return 0;
    int rv = pthread_rwlock_destroy(impl->rwlock);
    free(impl->rwlock);
    impl->rwlock = 0;
    return rv ? iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rv) : 0;
}

IW_INLINE iwrc _exfile_rwlock(IWFS_EXFILE *f, int wl) {
    assert(f);
    if (!f->impl) return IW_ERROR_INVALID_STATE;
    if (!f->impl->use_locks) return 0;
    if (!f->impl->rwlock) return IW_ERROR_INVALID_STATE;
    int rv = wl ? pthread_rwlock_wrlock(f->impl->rwlock)
             : pthread_rwlock_rdlock(f->impl->rwlock);
    return rv ? iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rv) : 0;
}

IW_INLINE iwrc _exfile_unlock(IWFS_EXFILE *f) {
    assert(f);
    if (!f->impl) return IW_ERROR_INVALID_STATE;
    if (!f->impl->use_locks) return 0;
    if (!f->impl->rwlock) return IW_ERROR_INVALID_STATE;
    int rv = pthread_rwlock_unlock(f->impl->rwlock);
    return rv ? iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rv) : 0;
}

IW_INLINE iwrc _exfile_unlock2(_IWXF *impl) {
    if (!impl) return IW_ERROR_INVALID_STATE;
    if (!impl->use_locks) return 0;
    if (!impl->rwlock) return IW_ERROR_INVALID_STATE;
    int rv = pthread_rwlock_unlock(impl->rwlock);
    return rv ? iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rv) : 0;
}

static const char* _exfile_ecodefn(locale_t locale, uint32_t ecode) {
    if (!(ecode > _IWFS_EXFILE_ERROR_START && ecode < _IWFS_EXFILE_ERROR_END)) {
        return 0;
    }
    switch (ecode) {
        case IWFS_ERROR_MMAP_OVERLAP:
            return "Region is mmaped already, mmaping overlaps. (IWFS_ERROR_MMAP_OVERLAP)";
        case IWFS_ERROR_NOT_MMAPED:
            return "Region is not mmaped (IWFS_ERROR_NOT_MMAPED)";
    }
    return 0;
}

iwrc iwfs_exfile_init(void) {
    static int _exfile_initialized = 0;
    iwrc rc;
    if (!__sync_bool_compare_and_swap(&_exfile_initialized, 0, 1)) {
        return 0; //initialized already
    }
    rc = iwlog_register_ecodefn(_exfile_ecodefn);
    return rc;
}