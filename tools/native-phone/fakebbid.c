/* Fake libbbid.so.1 for a private EBMService instance (BBM Video engine).
 * BlackBerry ID servers are gone; EBMService refuses to register to SIP without
 * a BBID user and token. Signatures recovered from EBMService (BbidPps.cpp):
 *   int bbidProfile_Init(int)
 *   int bbid_profile_set_gui_allowed(int, ok_cb, err_cb, void *ctx)
 *   int bbid_profile_get_user_properties(int n, const char **names, ok_cb, err_cb, void *ctx)
 *       ok_cb(int n, bbid_prop_t *props, void *ctx)
 *   int bbid_profile_get_token(const char *type, const char *scope, ok_cb, err_cb, void *ctx)
 *       ok_cb(const char *token, int n, bbid_prop_t *params, void *ctx)   (params: TOKEN_SECRET)
 * Callbacks run on a thread after the call returned (the caller then waits on a condvar).
 * Values: env FAKEBBID_USER (default "wacalls"), FAKEBBID_TOKEN, FAKEBBID_SECRET. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { const char *name; const char *value; } bbid_prop_t;
typedef void (*props_cb)(int, bbid_prop_t *, void *);
typedef void (*token_cb)(const char *, int, bbid_prop_t *, void *);

static const char *env(const char *k, const char *d) { const char *v = getenv(k); return v && *v ? v : d; }
static void logf_(const char *m, const char *a) { fprintf(stderr, "fakebbid: %s %s\n", m, a ? a : ""); fflush(stderr); }

struct job { int kind; void *cb; void *ctx; int n; bbid_prop_t props[8]; };

static void *runner(void *p)
{
    struct job *j = p;
    usleep(300000);
    if (j->kind == 0) {
        logf_("-> user properties callback", env("FAKEBBID_USER", "wacalls"));
        ((props_cb)j->cb)(j->n, j->props, j->ctx);
    } else {
        logf_("-> token callback", NULL);
        ((token_cb)j->cb)(env("FAKEBBID_TOKEN", "wacalls-token"), j->n, j->props, j->ctx);
    }
    return NULL; /* the job stays allocated: the caller may keep pointers into it */
}

static void spawn(struct job *j)
{
    pthread_t t; pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, runner, j);
}

int bbidProfile_Init(int x) { (void)x; logf_("init", NULL); return 0; }

int bbid_profile_set_gui_allowed(int allowed, void *ok, void *err, void *ctx)
{ (void)allowed; (void)ok; (void)err; (void)ctx; logf_("set_gui_allowed", NULL); return 0; }

int bbid_profile_get_user_properties(int n, const char **names, void *ok, void *err, void *ctx)
{
    struct job *j = calloc(1, sizeof *j);
    int i;
    (void)err;
    if (n > 8) n = 8;
    j->kind = 0; j->cb = ok; j->ctx = ctx; j->n = n;
    for (i = 0; i < n; i++) {
        const char *nm = names[i], *v = "";
        if (!strcasecmp(nm, "username")) v = env("FAKEBBID_USER", "wacalls");
        else if (!strcasecmp(nm, "nickname") || !strcasecmp(nm, "firstname")) v = "WhatsApp";
        else if (!strcasecmp(nm, "lastname")) v = "Calls";
        j->props[i].name = strdup(nm);
        j->props[i].value = strdup(v);
        logf_("get_user_properties", nm);
    }
    spawn(j);
    return 0;
}

int bbid_profile_get_token(const char *type, const char *scope, void *ok, void *err, void *ctx)
{
    struct job *j = calloc(1, sizeof *j);
    (void)err;
    logf_("get_token", scope);
    (void)type;
    j->kind = 1; j->cb = ok; j->ctx = ctx; j->n = 1;
    j->props[0].name = "TOKEN_SECRET";
    j->props[0].value = strdup(env("FAKEBBID_SECRET", "wacalls-secret"));
    spawn(j);
    return 0;
}
