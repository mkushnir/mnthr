#include <err.h>
#include <getopt.h>
#include <libgen.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>

#ifdef HAVE_MALLOC_H
#   include <malloc.h>
#endif

#include <mrkcommon/dumpm.h>
#include <mrkcommon/util.h>

#include <mrkthr.h>

#include "diag.h"
#include "config.h"

#ifndef NDEBUG
const char *_malloc_options = "AJ";
#endif

static int develop = 0;
static int check_config = 0;

#define FOO_OPT_DEFAULT_CONFIG_FILE "/usr/local/etc/foo.conf"
static char *configfile = NULL;


static struct option optinfo[] = {
#define FOO_OPT_FILE 0
    {"file", required_argument, NULL, 'f'},
#define FOO_OPT_HELP 1
    {"help", no_argument, NULL, 'h'},
#define FOO_OPT_VERSION 2
    {"version", no_argument, NULL, 'V'},
#define FOO_OPT_DEVELOP 3
    {"develop", no_argument, &develop, 1},
#define FOO_OPT_CHECK_CONFIG 4
    {"check-config", no_argument, &check_config, 1},
#define FOO_OPT_PRINT_CONFIG 5
    {"print-config", required_argument, NULL, 0},
    {NULL, 0, NULL, 0},
};


static void
usage(char *p)
{
    printf(
"Usage: %s OPTIONS\n"
"\n"
"Options:\n"
"  --help|-h                    Show this message and exit.\n"
"  --file=FPATH|-f FPATH        Configuration file (default\n"
"                               %s).\n"
"  --version|-V                 Print version and exit.\n"
"  --develop                    Run in develop mode.\n"
"  --check-config               Check config and exit.\n"
"  --print-config=PREFIX        multiple times.  Passing 'all' will show all\n"
"                               configuration.\n",
           basename(p),
           FOO_OPT_DEFAULT_CONFIG_FILE);
}


#ifndef SIGINFO
UNUSED
#endif
static void
myinfo(UNUSED int sig)
{
    mrkthr_dump_all_ctxes();
}


static void
myterm(UNUSED int sig)
{
    //qwe_shutdown(0);
}


static int
testconfig(void)
{
    return 0;
}

static void
initall(void)
{
}


static int
configure(UNUSED const char *configfile)
{
    return 0;
}


static int
run0(UNUSED int argc, UNUSED void **argv)
{
    return 0;
}


int
main(int argc, char **argv)
{
    int ch;
    int idx;

#ifdef HAVE_MALLOC_H
#   ifndef NDEBUG
    /*
     * malloc options
     */
    if (mallopt(M_CHECK_ACTION, 1) != 1) {
        FAIL("mallopt");
    }
    if (mallopt(M_PERTURB, 0x5a) != 1) {
        FAIL("mallopt");
    }
#   endif
#endif

    /*
     * install signal handlers
     */
    if (signal(SIGINT, myterm) == SIG_ERR) {
        return 1;
    }
    if (signal(SIGTERM, myterm) == SIG_ERR) {
        return 1;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return 1;
    }
#ifdef SIGINFO
    if (signal(SIGINFO, myinfo) == SIG_ERR) {
        return 1;
    }
#endif


    while ((ch = getopt_long(argc, argv, "f:hV", optinfo, &idx)) != -1) {
        switch (ch) {
        case 'h':
            usage(argv[0]);
            exit(0);

        case 'V':
            printf("%s\n", VERSION);
            exit(0);

        case 'f':
            //CTRACE("read config from %s", optarg);
            configfile = strdup(optarg);
            break;

        case 0:
            if (idx == FOO_OPT_PRINT_CONFIG) {
            } else {
                /*
                 * other options
                 */
            }
            break;

        default:
            usage(argv[0]);
            exit(1);
        }
    }
    argc -= optind;
    argv += optind;

    if (configfile == NULL) {
        configfile = strdup(FOO_OPT_DEFAULT_CONFIG_FILE);
    }

    if (testconfig() != 0) {
        err(1, "File configuration error");
    }

    if (check_config) {
        TRACEC("Configuration check passed.\n");
        goto end;
    }

    /*
     * "real" configure
     */
    initall();

    if (configure(configfile) != 0) {
        FAIL("configure");
    }

    if (develop) {
        CTRACE("will run in develop mode");
    } else {
        /*
         * daemonize
         */
        //daemon_ize();
    }

    mrkthr_init();
    (void)MRKTHR_SPAWN("run0", run0, argc, argv);
    mrkthr_loop();
    mrkthr_fini();

end:

    free(configfile);
    configfile = NULL;
    return 0;
}
