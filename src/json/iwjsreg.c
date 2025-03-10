#include "iwjsreg.h"
#include "iwrefs.h"
#include "iwutils.h"
#include "iwpool.h"
#include "iwjson.h"
#include "iwconv.h"

#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#ifdef __APPLE__
#define fdatasync fsync
#endif

struct iwjsreg {
  struct iwpool    *pool;
  struct jbl_node  *root;
  const char       *path;
  const char       *path_tmp;
  pthread_rwlock_t *rwl;
  pthread_rwlock_t  _rwl;
  struct iwref_holder ref;
  iwrc     (*rlock_fn)(void*);
  iwrc     (*wlock_fn)(void*);
  iwrc     (*unlock_fn)(void*);
  void    *fn_data;
  unsigned flags;
  bool     dirty;
};

static iwrc _rlock(void *d) {
  struct iwjsreg *reg = d;
  int rci = pthread_rwlock_rdlock(reg->rwl);
  if (rci) {
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  return 0;
}

static iwrc _wlock(void *d) {
  struct iwjsreg *reg = d;
  int rci = pthread_rwlock_wrlock(reg->rwl);
  if (rci) {
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  return 0;
}

static iwrc _unlock(void *d) {
  struct iwjsreg *reg = d;
  int rci = pthread_rwlock_unlock(reg->rwl);
  if (rci) {
    return iwrc_set_errno(IW_ERROR_THREADING_ERRNO, rci);
  }
  return 0;
}

static iwrc _destroy_visitor(int lvl, struct jbl_node *n) {
  if (n->key) {
    free((void*) n->key);
  }
  if (n->type == JBV_STR) {
    free((void*) n->vptr);
  }
  free(n);
  return 0;
}

static void _destroy(void *op) {
  struct iwjsreg *reg = op;
  if (reg->root) {
    jbn_visit2(reg->root, 0, _destroy_visitor);
  }
  if (reg->rwl == &reg->_rwl) {
    pthread_rwlock_destroy(reg->rwl);
  }
  iwpool_destroy(reg->pool);
}

static iwrc _load(struct iwjsreg *reg) {
  iwrc rc = 0;
  JBL jbl = 0;
  size_t flen = 0;
  char *fbuf = iwu_file_read_as_buf_len(reg->path, &flen);
  if (!fbuf) {
    return IW_ERROR_NOT_EXISTS;
  }
  if (reg->flags & IWJSREG_FORMAT_BINARY) {
    RCC(rc, finish, jbl_from_buf_keep(&jbl, fbuf, flen, true));
    RCC(rc, finish, jbl_to_node(jbl, &reg->root, true, 0));
  } else {
    RCC(rc, finish, jbn_from_json(fbuf, &reg->root, 0));
  }

finish:
  jbl_destroy(&jbl);
  free(fbuf);
  return rc;
}

iwrc iwjsreg_open(struct iwjsreg_spec *spec, struct iwjsreg **out) {
  iwrc rc = 0;
  if (!spec || !spec->path || !out) {
    return IW_ERROR_INVALID_ARGS;
  }
  struct iwpool *pool;
  struct iwjsreg *reg = 0;

  RCB(finish, pool = iwpool_create_empty());
  RCB(finish, reg = iwpool_calloc(sizeof(*reg), pool));
  reg->pool = pool;
  iwref_init(&reg->ref, reg, _destroy);
  reg->flags = spec->flags;

  if (spec->wlock_fn && spec->rlock_fn && spec->unlock_fn) {
    reg->wlock_fn = spec->wlock_fn;
    reg->rlock_fn = spec->rlock_fn;
    reg->unlock_fn = spec->unlock_fn;
    reg->fn_data = spec->fn_data;
  } else {
    if (spec->rwl) {
      reg->rwl = spec->rwl;
    } else {
      RCN(finish, pthread_rwlock_init(&reg->_rwl, 0));
      reg->rwl = &reg->_rwl;
    }
    reg->wlock_fn = _wlock;
    reg->rlock_fn = _rlock;
    reg->unlock_fn = _unlock;
    reg->fn_data = reg;
  }

  RCB(finish, reg->path = iwpool_strdup2(pool, spec->path));
  RCB(finish, reg->path_tmp = iwpool_printf(pool, "%s.tmp", spec->path));

  rc = _load(reg);
  if (rc) {
    if (rc != IW_ERROR_NOT_EXISTS) {
      goto finish;
    }
    rc = 0;
    RCC(rc, finish, jbn_from_json("{}", &reg->root, 0));
  }

finish:
  if (rc) {
    if (reg) {
      iwref_unref(&reg->ref);
    } else {
      iwpool_destroy(pool);
    }
  } else {
    *out = reg;
  }
  return rc;
}

iwrc iwjsreg_sync(struct iwjsreg *reg) {
  if (!reg) {
    return IW_ERROR_INVALID_ARGS;
  }
  if (!reg->root || (reg->flags & IWJSREG_READONLY)) {
    return 0;
  }

  iwrc rc = 0;
  JBL jbl = 0;
  FILE *file = 0;

  RCRET(reg->wlock_fn(reg->fn_data));
  iwref_ref(&reg->ref);

  if (!reg->dirty) {
    rc = reg->unlock_fn(reg->fn_data);
    iwref_unref(&reg->ref);
    return rc;
  }

  file = fopen(reg->path_tmp, "w");
  if (!file) {
    rc = iwrc_set_errno(IW_ERROR_ERRNO, errno);
    goto finish;
  }

  if (reg->flags & IWJSREG_FORMAT_BINARY) {
    void *buf;
    size_t bufsz;
    RCC(rc, finish, jbl_from_node(&jbl, reg->root));
    RCC(rc, finish, jbl_as_buf(jbl, &buf, &bufsz));
    if (fwrite(buf, bufsz, 1, file) != 1) {
      rc = IW_ERROR_IO;
      goto finish;
    }
  } else {
    RCC(rc, finish, jbn_as_json(reg->root, jbl_fstream_json_printer, file, JBL_PRINT_PRETTY_INDENT2));
  }

  RCN(finish, fflush(file));
  RCN(finish, fdatasync(fileno(file)));
  RCN(finish, fclose(file));
  file = 0;
  RCN(finish, rename(reg->path_tmp, reg->path));
  reg->dirty = false;

finish:
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  iwref_unref(&reg->ref);

  if (jbl) {
    jbl_destroy(&jbl);
  }
  if (file) {
    fclose(file);
  }
  if (rc) {
    iwlog_ecode_error3(rc);
  }
  return rc;
}

iwrc iwjsreg_close(struct iwjsreg **regp) {
  iwrc rc = 0;
  if (!regp || !*regp) {
    return IW_ERROR_INVALID_ARGS;
  }
  struct iwjsreg *reg = *regp;
  *regp = 0;
  if (!(reg->flags & IWJSREG_READONLY)) {
    IWRC(iwjsreg_sync(reg), rc);
  }
  iwref_unref(&reg->ref);
  return rc;
}

iwrc iwjsreg_remove(struct iwjsreg *reg, const char *key) {
  iwrc rc = 0;
  if (!reg || !reg->root || !key) {
    return IW_ERROR_INVALID_ARGS;
  }
  RCRET(reg->wlock_fn(reg->fn_data));
  for (struct jbl_node *n = reg->root->child; n; n = n->next) {
    if (n->key && strncmp(n->key, key, n->klidx) == 0) {
      jbn_remove_item(reg->root, n);
      if (n->type == JBV_STR) {
        free((void*) n->vptr);
      }
      free((void*) n->key);
      free(n);
      reg->dirty = true;
      break;
    }
  }
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  return rc;
}

iwrc iwjsreg_set_str(struct iwjsreg *reg, const char *key, const char *value) {
  iwrc rc = 0;
  if (!reg || !reg->root || !key || !value) {
    return IW_ERROR_INVALID_ARGS;
  }
  struct jbl_node *nn = 0;
  char *nkey = 0, *nvalue = 0;

  RCRET(reg->wlock_fn(reg->fn_data));
  for (struct jbl_node *n = reg->root->child; n; n = n->next) {
    if (n->key && strncmp(n->key, key, n->klidx) == 0) {
      nn = n;
      break;
    }
  }
  if (!nn) {
    RCB(finish, nn = calloc(1, sizeof(*nn)));
    nn->type = JBV_STR;
    RCB(finish, nkey = strdup(key));
    RCB(finish, nvalue = strdup(value));
  } else {
    if (nn->type == JBV_STR) {
      free((void*) nn->vptr);
    } else {
      nn->type = JBV_STR;
    }
    RCB(finish, nvalue = strdup(value));
  }
  reg->dirty = true;

finish:
  if (rc) {
    free(nkey);
    free(nvalue);
  } else {
    nn->vptr = nvalue;
    nn->vsize = strlen(nn->vptr);
    if (nkey) {
      nn->key = nkey;
      nn->klidx = strlen(nn->key);
      jbn_add_item(reg->root, nn);
    }
  }

  IWRC(reg->unlock_fn(reg->fn_data), rc);
  if (!rc && (reg->flags & IWJSREG_AUTOSYNC)) {
    rc = iwjsreg_sync(reg);
  }
  return rc;
}

iwrc iwjsreg_merge(struct iwjsreg *reg, const char *path, struct jbl_node *json) {
  iwrc rc = 0;
  RCRET(reg->wlock_fn(reg->fn_data));
  rc = jbn_merge_patch_path(reg->root, path, json, 0);
  if (!rc) {
    reg->dirty = true;
  }
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  if (!rc && (reg->flags & IWJSREG_AUTOSYNC)) {
    rc = iwjsreg_sync(reg);
  }
  return rc;
}

iwrc iwjsreg_merge_str(struct iwjsreg *reg, const char *path, const char *value, int len) {
  struct jbl_node n = { .type = JBV_STR, .vptr = value, .vsize = len > -1 ? len : strlen(value) };
  return iwjsreg_merge(reg, path, &n);
}

iwrc iwjsreg_merge_i64(struct iwjsreg *reg, const char *path, int64_t value) {
  struct jbl_node n = { .type = JBV_I64, .vi64 = value };
  return iwjsreg_merge(reg, path, &n);
}

iwrc iwjsreg_merge_f64(struct iwjsreg *reg, const char *path, double value) {
  struct jbl_node n = { .type = JBV_F64, .vf64 = value };
  return iwjsreg_merge(reg, path, &n);
}

iwrc iwjsreg_merge_bool(struct iwjsreg *reg, const char *path, bool value) {
  struct jbl_node n = { .type = JBV_BOOL, .vbool = value };
  return iwjsreg_merge(reg, path, &n);
}

iwrc iwjsreg_merge_remove(struct iwjsreg *reg, const char *path) {
  struct jbl_node n = { .type = JBV_NULL };
  return iwjsreg_merge(reg, path, &n);
}

iwrc iwjsreg_replace(struct iwjsreg *reg, const char *path, struct jbl_node *json) {
  iwrc rc = 0;
  struct jbl_node *n;
  RCRET(reg->wlock_fn(reg->fn_data));

  // Remove subtree
  rc = jbn_at(reg->root, path, &n);
  if (rc == 0) {
    struct jbl_node *p = n->parent;
    if (n != reg->root) {
      jbn_remove_item(p, n);
      jbn_visit2(n, 0, _destroy_visitor);
    } else { // Remove the whoole tree keeping the reg->root
      for (struct jbl_node *n = reg->root->child; n; n = n->next) {
        jbn_visit2(n, 0, _destroy_visitor);
      }
      reg->root->child = 0;
    }
  } else if (rc == JBL_ERROR_PATH_NOTFOUND) {
    rc = 0;
  } else {
    goto finish;
  }

  // Perform insertion
  RCC(rc, finish, jbn_merge_patch_path(reg->root, path, json, 0));
  reg->dirty = true;

finish:
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  if (!rc && (reg->flags & IWJSREG_AUTOSYNC)) {
    rc = iwjsreg_sync(reg);
  }
  return rc;
}

iwrc iwjsreg_at_i64(struct iwjsreg *reg, const char *path, int64_t *out) {
  iwrc rc = 0;
  struct jbl_node *n;

  RCRET(reg->rlock_fn(reg->fn_data));
  RCC(rc, finish, jbn_at(reg->root, path, &n));

  switch (n->type) {
    case JBV_I64:
      *out = n->vi64;
      break;
    case JBV_F64:
      *out = (int64_t) n->vf64;
      break;
    case JBV_NULL:
      *out = 0;
      break;
    case JBV_BOOL:
      *out = n->vbool;
      break;
    case JBV_STR:
      *out = iw_strtoll(n->vptr, 10, &rc);
      break;
    default:
      rc = IW_ERROR_TYPE_NOT_COMPATIBLE;
      break;
  }

finish:
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  return rc;
}

iwrc iwjsreg_at_f64(struct iwjsreg *reg, const char *path, double *out) {
  iwrc rc = 0;
  struct jbl_node *n;

  RCRET(reg->rlock_fn(reg->fn_data));
  RCC(rc, finish, jbn_at(reg->root, path, &n));

  switch (n->type) {
    case JBV_F64:
      *out = n->vf64;
      break;
    case JBV_I64:
      *out = (double) n->vi64;
      break;
    case JBV_NULL:
      *out = 0;
      break;
    case JBV_BOOL:
      *out = (double) n->vbool;
      break;
    case JBV_STR:
      *out = iw_strtod(n->vptr, &rc);
      break;
    default:
      rc = IW_ERROR_TYPE_NOT_COMPATIBLE;
      break;
  }

finish:
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  return rc;
}

iwrc iwjsreg_at_bool(struct iwjsreg *reg, const char *path, bool *out) {
  iwrc rc = 0;
  struct jbl_node *n;

  RCRET(reg->rlock_fn(reg->fn_data));
  RCC(rc, finish, jbn_at(reg->root, path, &n));

  switch (n->type) {
    case JBV_BOOL:
      *out = n->vbool;
      break;
    case JBV_F64:
      *out = n->vf64 != 0;
      break;
    case JBV_I64:
      *out = (double) n->vi64 != 0.0;
      break;
    case JBV_NULL:
      *out = 0;
      break;
    case JBV_STR:
      *out = strcmp(n->vptr, "true") == 0;
      break;
    default:
      rc = IW_ERROR_TYPE_NOT_COMPATIBLE;
      break;
  }

finish:
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  return rc;
}

iwrc iwjsreg_at_str(struct iwjsreg *reg, const char *path, char **out) {
  iwrc rc = 0;
  struct jbl_node *n;
  char buf[IWNUMBUF_SIZE];

  *out = 0;

  RCRET(reg->rlock_fn(reg->fn_data));
  RCC(rc, finish, jbn_at(reg->root, path, &n));

  switch (n->type) {
    case JBV_STR:
      RCB(finish, *out = strdup(n->vptr));
      break;
    case JBV_BOOL:
      RCB(finish, *out = strdup(n->vbool ? "true" : "false"));
      break;
    case JBV_NULL:
      RCB(finish, *out = strdup("null"));
      break;
    case JBV_I64:
      iwitoa(n->vi64, buf, IWNUMBUF_SIZE);
      RCB(finish, *out = strdup(buf));
      break;
    case JBV_F64:
      iwftoa(n->vf64, buf);
      RCB(finish, *out = strdup(buf));
      break;
    default:
      rc = IW_ERROR_TYPE_NOT_COMPATIBLE;
      break;
  }

finish:
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  return rc;
}

iwrc iwjsreg_copy(struct iwjsreg *reg, const char *path, struct iwpool *pool, struct jbl_node **out) {
  iwrc rc = 0;
  struct jbl_node *root;
  RCRET(reg->rlock_fn(reg->fn_data));
  if (path) {
    RCC(rc, finish, jbn_at(reg->root, path, &root));
  } else {
    root = reg->root;
  }
  rc = jbn_clone(root, out, pool);

finish:
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  return rc;
}

iwrc iwjsreg_set_i64(struct iwjsreg *reg, const char *key, int64_t value) {
  iwrc rc = 0;
  if (!reg || !reg->root || !key) {
    return IW_ERROR_INVALID_ARGS;
  }
  struct jbl_node *nn = 0;
  char *nkey = 0;

  RCRET(reg->wlock_fn(reg->fn_data));
  for (struct jbl_node *n = reg->root->child; n; n = n->next) {
    if (n->key && strncmp(n->key, key, n->klidx) == 0) {
      nn = n;
      break;
    }
  }

  if (!nn) {
    RCB(finish, nn = calloc(1, sizeof(*nn)));
    nn->type = JBV_I64;
    nn->vi64 = value;
    RCB(finish, nkey = strdup(key));
  } else {
    if (nn->type == JBV_STR) {
      free((void*) nn->vptr);
    }
    nn->type = JBV_I64;
    nn->vi64 = value;
  }
  reg->dirty = true;

finish:
  if (rc) {
    free(nkey);
  } else {
    if (nkey) {
      nn->key = nkey;
      nn->klidx = strlen(nkey);
      jbn_add_item(reg->root, nn);
    }
  }
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  if (!rc && (reg->flags & IWJSREG_AUTOSYNC)) {
    rc = iwjsreg_sync(reg);
  }
  return rc;
}

iwrc iwjsreg_inc_i64(struct iwjsreg *reg, const char *key, int64_t inc, int64_t *out) {
  iwrc rc = 0;
  if (!reg || !reg->root || !key) {
    return IW_ERROR_INVALID_ARGS;
  }
  struct jbl_node *nn = 0;
  char *nkey = 0;

  RCRET(reg->wlock_fn(reg->fn_data));
  for (struct jbl_node *n = reg->root->child; n; n = n->next) {
    if (n->key && strncmp(n->key, key, n->klidx) == 0) {
      nn = n;
      break;
    }
  }

  if (!nn) {
    RCB(finish, nn = calloc(1, sizeof(*nn)));
    nn->type = JBV_I64;
    RCB(finish, nkey = strdup(key));
  } else {
    if (nn->type == JBV_STR) {
      free((void*) nn->vptr);
    }
    if (nn->type != JBV_I64) {
      nn->vi64 = 0;
    }
    nn->type = JBV_I64;
  }
  nn->vi64 += inc;
  if (*out) {
    *out = nn->vi64;
  }
  reg->dirty = true;

finish:
  if (rc) {
    free(nkey);
  } else {
    if (nkey) {
      nn->key = nkey;
      nn->klidx = strlen(nkey);
      jbn_add_item(reg->root, nn);
    }
  }
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  if (!rc && (reg->flags & IWJSREG_AUTOSYNC)) {
    rc = iwjsreg_sync(reg);
  }
  return rc;
}

iwrc iwjsreg_set_bool(struct iwjsreg *reg, const char *key, bool value) {
  iwrc rc = 0;
  if (!reg || !reg->root || !key) {
    return IW_ERROR_INVALID_ARGS;
  }
  struct jbl_node *nn = 0;
  char *nkey = 0;

  RCRET(reg->wlock_fn(reg->fn_data));
  for (struct jbl_node *n = reg->root->child; n; n = n->next) {
    if (n->key && strncmp(n->key, key, n->klidx) == 0) {
      nn = n;
      break;
    }
  }

  if (!nn) {
    RCB(finish, nn = calloc(1, sizeof(*nn)));
    nn->type = JBV_BOOL;
    nn->vbool = value;
    RCB(finish, nkey = strdup(key));
  } else {
    if (nn->type == JBV_STR) {
      free((void*) nn->vptr);
    }
    nn->type = JBV_BOOL;
    nn->vbool = value;
  }
  reg->dirty = true;

finish:
  if (rc) {
    free(nkey);
  } else {
    if (nkey) {
      nn->key = nkey;
      nn->klidx = strlen(nkey);
      jbn_add_item(reg->root, nn);
    }
  }
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  if (!rc && (reg->flags & IWJSREG_AUTOSYNC)) {
    rc = iwjsreg_sync(reg);
  }
  return rc;
}

iwrc iwjsreg_get_str(struct iwjsreg *reg, const char *key, char **out) {
  iwrc rc = 0;
  if (!reg || !key || !out) {
    return IW_ERROR_INVALID_ARGS;
  }
  bool found = false;
  *out = 0;
  RCRET(reg->rlock_fn(reg->fn_data));
  for (struct jbl_node *n = reg->root->child; n; n = n->next) {
    if (n->key && strcmp(n->key, key) == 0) {
      if (n->type == JBV_STR) {
        *out = strdup(n->vptr);
        found = true;
        break;
      }
    }
  }
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  if (!rc && !found) {
    rc = IW_ERROR_NOT_EXISTS;
  }
  return rc;
}

iwrc iwjsreg_get_i64(struct iwjsreg *reg, const char *key, int64_t *out) {
  iwrc rc = 0;
  if (!reg || !key || !out) {
    return IW_ERROR_INVALID_ARGS;
  }
  bool found = false;
  *out = 0;
  RCRET(reg->rlock_fn(reg->fn_data));
  for (struct jbl_node *n = reg->root->child; n; n = n->next) {
    if (n->key && strcmp(n->key, key) == 0) {
      if (n->type == JBV_I64) {
        *out = n->vi64;
        found = true;
        break;
      }
    }
  }
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  if (!rc && !found) {
    rc = IW_ERROR_NOT_EXISTS;
  }
  return rc;
}

iwrc iwjsreg_get_bool(struct iwjsreg *reg, const char *key, bool *out) {
  iwrc rc = 0;
  if (!reg || !key || !out) {
    return IW_ERROR_INVALID_ARGS;
  }
  bool found = false;
  *out = 0;
  RCRET(reg->rlock_fn(reg->fn_data));
  for (struct jbl_node *n = reg->root->child; n; n = n->next) {
    if (n->key && strcmp(n->key, key) == 0) {
      if (n->type == JBV_BOOL) {
        *out = n->vbool;
        found = true;
        break;
      }
    }
  }
  IWRC(reg->unlock_fn(reg->fn_data), rc);
  if (!rc && !found) {
    rc = IW_ERROR_NOT_EXISTS;
  }
  return rc;
}
