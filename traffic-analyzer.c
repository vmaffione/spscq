#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

struct traffic_analyzer {
    unsigned int num_analyzers;
};

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

    return 0;
}
