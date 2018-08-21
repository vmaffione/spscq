#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#include "mlib.h"

struct worker {
    pthread_t th;
};

struct traffic_analyzer {
    unsigned int num_analyzers;
    pthread_t lb_th;
    struct worker *workers;
};

/* Alloc zeroed cacheline-aligned memory, aborting on failure. */
static void *
szalloc(size_t size)
{
    void *p = NULL;
    int ret = posix_memalign(&p, ALIGN_SIZE, size);
    if (ret) {
        printf("allocation failure: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    memset(p, 0, size);
    return p;
}

static void *
lb(void *opaque)
{
    struct traffic_analyzer *ta = opaque;
    (void)ta;
    return NULL;
}

static void *
analyze(void *opaque)
{
    struct worker *w = opaque;
    (void)w;
    return NULL;
}

static void
usage(const char *progname)
{
    printf("%s\n"
           "    [-h (show this help and exit)]\n"
           "    [-n NUM_ANALYZERS = 2]\n"
           "\n",
           progname);
}

int
main(int argc, char **argv)
{
    struct traffic_analyzer _ta;
    struct traffic_analyzer *ta = &_ta;
    int opt;
    int i;

    memset(ta, 0, sizeof(*ta));
    ta->num_analyzers = 2;

    while ((opt = getopt(argc, argv, "hn:")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            return 0;

        case 'n':
            ta->num_analyzers = atoi(optarg);
            if (ta->num_analyzers == 0 || ta->num_analyzers > 1000) {
                printf("    Invalid number of analyzers '%s'\n", optarg);
            }
            break;

        default:
            usage(argv[0]);
            return 0;
            break;
        }
    }

    /* Setup phase. */
    ta->workers = szalloc(ta->num_analyzers * sizeof(ta->workers[0]));

    if (pthread_create(&ta->lb_th, NULL, lb, ta)) {
        printf("pthread_create(lb) failed\n");
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < ta->num_analyzers; i++) {
        struct worker *w = ta->workers + i;
        if (pthread_create(&w->th, NULL, analyze, w)) {
            printf("pthread_create(worker) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    /* Teardown phase. */
    for (i = 0; i < ta->num_analyzers; i++) {
        struct worker *w = ta->workers + i;
        if (pthread_join(w->th, NULL)) {
            printf("pthread_join(worker) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    if (pthread_join(ta->lb_th, NULL)) {
        printf("pthread_join(lb) failed\n");
        exit(EXIT_FAILURE);
    }

    free(ta->workers);

    return 0;
}
