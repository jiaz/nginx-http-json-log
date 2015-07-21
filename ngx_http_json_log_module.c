
/*
 * Copyright (C) Jiaji Zhou
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <jansson.h>

#define PRINT(...) fprintf(stderr, __VA_ARGS__)

static ngx_str_t   ngx_http_json_access_log = ngx_string("/tmp/ngx_json.log");
static ngx_pool_t  *current_pool = NULL;

/******************************************************************************
 * Type defs
 *****************************************************************************/

typedef struct {
    ngx_str_t name;
    ngx_int_t index;
} ngx_http_json_log_field_t;

typedef struct {
    ngx_str_t   name;
    ngx_array_t *fields;   // array of ngx_str_t
} ngx_http_json_log_fmt_t;

typedef struct {
    ngx_http_json_log_fmt_t *format;
    ngx_open_file_t         *file;
} ngx_http_json_log_t;

typedef struct {
    ngx_array_t *formats;   // array of ngx_http_json_log_fmt_t
} ngx_http_json_log_main_conf_t;

typedef struct {
    ngx_array_t *logs;      // array of ngx_http_json_log_t
    ngx_uint_t  off;        // off: 1
} ngx_http_json_log_loc_conf_t;

/******************************************************************************
 * Function declarations
 *****************************************************************************/

//
// Memory alloc for json
//
static void set_current_mem_pool(ngx_pool_t *pool);
static ngx_pool_t *get_current_mem_pool();
static void *my_pmalloc(size_t size);
static void my_pfree(void *ptr);

//
// Module conf
//
static void *ngx_http_json_log_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_json_log_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_json_log_merge_loc_conf(ngx_conf_t *cf, void *parent,
        void *child);
static char *ngx_http_json_log_set_log(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf);
static char *ngx_http_json_log_set_fields(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf);
static ngx_int_t ngx_http_json_log_init(ngx_conf_t *cf);

//
// Logging
//
static void ngx_http_json_log_write(ngx_http_request_t *r,
        ngx_http_json_log_t *log, u_char *buf, size_t len);

/******************************************************************************
 * Static Variables
 *****************************************************************************/

static ngx_str_t combined_fields[] = {
    ngx_string("remote_addr"),
    ngx_string("remote_user"),
    ngx_string("time_local"),
    ngx_string("request"),
    ngx_string("status"),
    ngx_string("body_bytes_sent"),
    ngx_string("http_referer"),
    ngx_string("http_user_agent"),
};

//
// Module configuration commands
//
static ngx_command_t ngx_http_json_log_commands[] = {

    { ngx_string("json_log_fields"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_2MORE,
      ngx_http_json_log_set_fields,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("access_json_log"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
                        |NGX_HTTP_LMT_CONF|NGX_CONF_1MORE,
      ngx_http_json_log_set_log,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    ngx_null_command
};

//
// Module Context
//
static ngx_http_module_t ngx_http_json_log_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_json_log_init,                /* postconfiguration */

    ngx_http_json_log_create_main_conf,    /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_json_log_create_loc_conf,     /* create location configuration */
    ngx_http_json_log_merge_loc_conf       /* merge location configuration */
};

//
// Module definition
//
ngx_module_t ngx_http_json_log_module = {
    NGX_MODULE_V1,
    &ngx_http_json_log_module_ctx,         /* module context */
    ngx_http_json_log_commands,            /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

/******************************************************************************
 * Function Implementations
 *****************************************************************************/

//
// Main Handler Function
//
static ngx_int_t
ngx_http_json_log_handler(ngx_http_request_t *r)
{
    ngx_http_json_log_loc_conf_t  *llcf;
    ngx_http_json_log_t           *log;
    u_char                        *line, *p, *field_val;
    ngx_uint_t                    s, l;
    ngx_int_t                     index;
    ngx_http_json_log_field_t     *field;
    ngx_http_variable_value_t     *value;

    llcf = ngx_http_get_module_loc_conf(r, ngx_http_json_log_module);

    if (llcf->off) {
        return NGX_OK;
    }

    log = llcf->logs->elts;
    set_current_mem_pool(r->pool);
    for (l = 0; l < llcf->logs->nelts; l++) {
        // compose json object
        json_t *obj = json_object();
        field = log[l].format->fields->elts;
        for (s = 0; s < log[l].format->fields->nelts; s++) {
            index = field[s].index;
            value = ngx_http_get_indexed_variable(r, index);
            if (value == NULL || value->not_found) {
                json_object_set_new(obj, (char *)field[s].name.data,
                        json_string("-"));
            } else {
                field_val = ngx_pnalloc(r->pool, value->len + 1);
                ngx_cpystrn(field_val, value->data, value->len + 1);
                json_object_set_new(obj, (char *)field[s].name.data,
                        json_string((char *)field_val));
            }
        }
        char *json_str = json_dumps(obj, JSON_COMPACT);
        size_t len = strlen(json_str);
        line = ngx_pnalloc(r->pool, len+1);
        p = line;
        p = ngx_cpymem(p, json_str, len);
        ngx_linefeed(p);

        ngx_http_json_log_write(r, &log[l], line, len+1);

        json_decref(obj);
        my_pfree(json_str);
    }
    set_current_mem_pool(NULL);

    return NGX_OK;
}

static void
ngx_http_json_log_write(ngx_http_request_t *r, ngx_http_json_log_t *log,
                        u_char *buf, size_t len)
{
    ssize_t n;

    n = ngx_write_fd(log->file->fd, buf, len);

    if (n == (ssize_t) len) {
        return;
    }

    // handle error
    return;
}

static void *
ngx_http_json_log_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_json_log_main_conf_t *conf;
    ngx_http_json_log_fmt_t       *fmt;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_json_log_main_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->formats = ngx_array_create(cf->pool, 4,
            sizeof(ngx_http_json_log_fmt_t));
    if (conf->formats == NULL) {
        return NULL;
    }

    fmt = ngx_array_push(conf->formats);
    if (fmt == NULL) {
        return NULL;
    }

    ngx_str_set(&fmt->name, "combined");

    return conf;
}

static void *
ngx_http_json_log_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_json_log_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_json_log_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    return conf;
}

static char *
ngx_http_json_log_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_json_log_loc_conf_t *prev = parent;
    ngx_http_json_log_loc_conf_t *conf = child;

    ngx_http_json_log_t *log;

    if (conf->logs || conf->off) {
        return NGX_CONF_OK;
    }

    conf->logs = prev->logs;
    conf->off = prev->off;

    if (conf->logs || conf->off) {
        return NGX_CONF_OK;
    }

    conf->logs = ngx_array_create(cf->pool, 2, sizeof(ngx_http_json_log_t));
    if (conf->logs == NULL) {
        return NGX_CONF_ERROR;
    }

    log = ngx_array_push(conf->logs);
    if (log == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(log, sizeof(ngx_http_json_log_t));

    log->file = ngx_conf_open_file(cf->cycle, &ngx_http_json_access_log);
    if (log->file == NULL) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static char *
ngx_http_json_log_set_log(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_json_log_loc_conf_t   *llcf;
    ngx_http_json_log_main_conf_t  *lmcf;
    ngx_str_t                      *value, name;
    ngx_http_json_log_t            *log;
    ngx_http_json_log_fmt_t        *fmt;
    ngx_uint_t                     i;

    llcf = conf;
    value = cf->args->elts;

    if (ngx_strcmp(value[1].data, "off") == 0) {
        llcf->off = 1;
        if (cf->args->nelts == 2) {
            return NGX_CONF_OK;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "invalid parameter \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }

    if (llcf->logs == NULL) {
        llcf->logs = ngx_array_create(cf->pool, 2, sizeof(ngx_http_json_log_t));
        if (llcf->logs == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    lmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_json_log_module);

    log = ngx_array_push(llcf->logs);
    if (log == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(log, sizeof(ngx_http_json_log_t));

    log->file = ngx_conf_open_file(cf->cycle, &value[1]);
    if (log->file == NULL) {
        return NGX_CONF_ERROR;
    }

    if (cf->args->nelts >= 3) {
        name = value[2];
    } else {
        ngx_str_set(&name, "combined");
    }

    fmt = lmcf->formats->elts;
    for (i = 0; i < lmcf->formats->nelts; i++) {
        if (fmt[i].name.len == name.len
            && ngx_strcasecmp(fmt[i].name.data, name.data) == 0)
        {
            log->format = &fmt[i];
            break;
        }
    }

    if (log->format == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "unknown log format \"%V\"", &name);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static char *
ngx_http_json_log_set_fields(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_json_log_main_conf_t  *lmcf;
    ngx_str_t                      *value;
    ngx_http_json_log_field_t      *field;
    ngx_uint_t                     s;
    ngx_http_json_log_fmt_t        *fmt;

    lmcf = conf;
    fmt = lmcf->formats->elts;
    value = cf->args->elts;

    // check duplicated format
    for (s = 0; s < lmcf->formats->nelts; s++) {
        if (fmt[s].name.len == value[1].len
            && ngx_strcmp(fmt[s].name.data, value[1].data) == 0)
        {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "duplicated \"json_log_fields\" name \"%v\"",
                    &value[1]);
            return NGX_CONF_ERROR;
        }
    }

    fmt = ngx_array_push(lmcf->formats);
    if (fmt == NULL) {
        return NGX_CONF_ERROR;
    }

    fmt->name = value[1];

    fmt->fields = ngx_array_create(cf->pool, 16,
            sizeof(ngx_http_json_log_field_t));

    for (s = 2; s < cf->args->nelts; s++) {
        field = ngx_array_push(fmt->fields);
        if (field == NULL) {
            return NGX_CONF_ERROR;
        }

        field->name = value[s];

        field->index = ngx_http_get_variable_index(cf, &value[s]);
        if (field->index == NGX_ERROR) {
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_json_log_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt             *h;
    ngx_http_core_main_conf_t       *cmcf;
    ngx_http_json_log_main_conf_t   *lmcf;
    ngx_http_json_log_field_t       *field;
    ngx_uint_t                      i, l;
    ngx_http_json_log_fmt_t         *fmt;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    lmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_json_log_module);

    // initialize combined format
    fmt = lmcf->formats->elts;
    fmt->fields = ngx_array_create(cf->pool, 16,
            sizeof(ngx_http_json_log_field_t));
    if (fmt->fields == NULL) {
        return NGX_ERROR;
    }

    l = sizeof(combined_fields) / sizeof(ngx_str_t);

    for (i = 0; i < l; i++) {
        field = ngx_array_push(fmt->fields);
        if (field == NULL) {
            return NGX_ERROR;
        }

        field->name = combined_fields[i];

        field->index = ngx_http_get_variable_index(cf, &field->name);
        if (field->index == NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    // setup jansson memory allocator
    json_set_alloc_funcs(my_pmalloc, my_pfree);

    *h = ngx_http_json_log_handler;

    return NGX_OK;
}

static void
set_current_mem_pool(ngx_pool_t *pool)
{
    current_pool = pool;
}

static ngx_pool_t *
get_current_mem_pool()
{
    return current_pool;
}

static void *
my_pmalloc(size_t size)
{
    ngx_pool_t *pool = get_current_mem_pool();
    if (pool == NULL) {
        return NULL;
    }
    return ngx_pcalloc(pool, size);
}

static void
my_pfree(void *ptr)
{
    ngx_pool_t *pool = get_current_mem_pool();
    if (pool == NULL) {
        return;
    }
    ngx_pfree(pool, ptr);
}

