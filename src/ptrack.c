/*
 * ptrack — CLI price tracking tool
 *
 * Periodically fetches the price of an online product and sends an email
 * alert when the price drops to or below a target threshold (or reaches
 * user-defined percentage milestones).
 *
 * Build:  make
 * Usage:  ptrack -h
 */

#include "config.h"
#include "fetch.h"
#include "notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <math.h>

/* ---------- Persistent state ------------------------------------------- */

typedef struct {
    double initial_price;      /* Price at first successful fetch */
    double reference_price;    /* Effective target (user or initial) */
    double last_price;
    long   check_count;
    int    target_notified;    /* 1 once the "at-or-below target" alert sent */
    int    milestone_notified[MAX_MILESTONES]; /* 1 per triggered milestone */
} State;

static void state_clear(State *s)
{
    memset(s, 0, sizeof(*s));
}

/* Load state from file; silently returns defaults on failure */
static void state_load(State *s, const char *path)
{
    state_clear(s);
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    fscanf(f,
           "initial_price=%lf\n"
           "reference_price=%lf\n"
           "last_price=%lf\n"
           "check_count=%ld\n"
           "target_notified=%d\n",
           &s->initial_price,
           &s->reference_price,
           &s->last_price,
           &s->check_count,
           &s->target_notified);

    for (int i = 0; i < MAX_MILESTONES; i++) {
        int v = 0;
        if (fscanf(f, "milestone_%d=%d\n", &i, &v) == 2)
            s->milestone_notified[i] = v;
    }
    fclose(f);
}

static void state_save(const State *s, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return;

    fprintf(f,
            "initial_price=%.4f\n"
            "reference_price=%.4f\n"
            "last_price=%.4f\n"
            "check_count=%ld\n"
            "target_notified=%d\n",
            s->initial_price,
            s->reference_price,
            s->last_price,
            s->check_count,
            s->target_notified);

    for (int i = 0; i < MAX_MILESTONES; i++)
        fprintf(f, "milestone_%d=%d\n", i, s->milestone_notified[i]);

    fclose(f);
}

/* ---------- Signal handling -------------------------------------------- */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---------- Timestamp helper ------------------------------------------- */

static void print_ts(void)
{
    time_t     now = time(NULL);
    struct tm *tm  = localtime(&now);
    char       buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    printf("[%s] ", buf);
}

/* ---------- Core tracking loop ----------------------------------------- */

static int run(Config *cfg)
{
    State state;
    state_load(&state, cfg->state_file);

    /* First run: fetch initial price if not already stored */
    if (state.initial_price <= 0.0) {
        if (cfg->verbose) {
            print_ts();
            printf("Fetching initial price from: %s\n", cfg->url);
        }
        double p = fetch_price(cfg->url, cfg->verbose);
        if (p <= 0.0) {
            fprintf(stderr,
                    "ptrack: could not determine initial price. "
                    "Check the URL and try again.\n");
            return EXIT_FAILURE;
        }
        state.initial_price = p;
        state.last_price     = p;

        /* Determine reference / target price */
        if (cfg->target_price > 0.0) {
            state.reference_price = cfg->target_price;
        } else {
            state.reference_price = p; /* use initial as baseline */
        }

        print_ts();
        printf("Initial price: $%.2f  |  Target: $%.2f\n",
               p, state.reference_price);
        state_save(&state, cfg->state_file);

        if (cfg->check_once) {
            printf("(check-once mode: exiting after initial fetch)\n");
            return EXIT_SUCCESS;
        }
    } else {
        /* Resume: honour any updated target from command line */
        if (cfg->target_price > 0.0)
            state.reference_price = cfg->target_price;

        if (cfg->verbose) {
            print_ts();
            printf("Resuming tracking — initial: $%.2f  target: $%.2f  "
                   "last: $%.2f  checks: %ld\n",
                   state.initial_price,
                   state.reference_price,
                   state.last_price,
                   state.check_count);
        }
    }

    long checks_done = 0;
    long sleep_left;

    while (!g_stop) {
        /* Wait for the configured interval before next check */
        sleep_left = cfg->interval;
        while (sleep_left > 0 && !g_stop) {
            long slept = (sleep_left > 60) ? 60 : sleep_left;
            sleep((unsigned int)slept);
            sleep_left -= slept;
        }
        if (g_stop)
            break;

        /* Fetch current price */
        double price = fetch_price(cfg->url, cfg->verbose);
        if (price <= 0.0) {
            print_ts();
            fprintf(stderr,
                    "Warning: could not fetch price (will retry next "
                    "interval).\n");
            continue;
        }

        state.check_count++;
        checks_done++;
        state.last_price = price;

        print_ts();
        printf("Check #%ld — current price: $%.2f  (target: $%.2f  "
               "delta: %+.2f)\n",
               state.check_count,
               price,
               state.reference_price,
               price - state.reference_price);

        /* --- Check 1: price at or below target -------------------------- */
        if (!state.target_notified && price <= state.reference_price) {
            char reason[128];
            snprintf(reason, sizeof(reason),
                     "at or below target $%.2f", state.reference_price);
            print_ts();
            printf("ALERT: %s — sending email to %s\n",
                   reason, cfg->email_to);

            if (send_price_alert(cfg, price, state.reference_price,
                                 reason) == 0) {
                state.target_notified = 1;
                printf("       Email sent successfully.\n");
            } else {
                fprintf(stderr,
                        "ptrack: failed to send alert email.\n");
            }
        }

        /* --- Check 2: milestone drops below initial price --------------- */
        for (int i = 0; i < cfg->num_milestones; i++) {
            if (state.milestone_notified[i])
                continue;

            double thresh = state.initial_price *
                            (1.0 - cfg->milestones[i] / 100.0);
            if (price <= thresh) {
                char reason[128];
                snprintf(reason, sizeof(reason),
                         "%.1f%% drop from initial $%.2f",
                         cfg->milestones[i], state.initial_price);
                print_ts();
                printf("MILESTONE: %s — sending email to %s\n",
                       reason, cfg->email_to);

                if (send_price_alert(cfg, price, state.initial_price,
                                     reason) == 0) {
                    state.milestone_notified[i] = 1;
                    printf("           Email sent successfully.\n");
                } else {
                    fprintf(stderr,
                            "ptrack: failed to send milestone email.\n");
                }
            }
        }

        state_save(&state, cfg->state_file);

        /* Stop if max_checks reached */
        if (cfg->max_checks > 0 && checks_done >= cfg->max_checks) {
            print_ts();
            printf("Reached max_checks (%ld). Exiting.\n", cfg->max_checks);
            break;
        }

        if (cfg->check_once)
            break;
    }

    if (g_stop) {
        printf("\nptrack: interrupted — state saved to %s\n",
               cfg->state_file);
    }
    return EXIT_SUCCESS;
}

/* ---------- Entry point ------------------------------------------------- */

int main(int argc, char *argv[])
{
    /* Make stdout line-buffered so progress messages appear in order
     * even when piped alongside stderr */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Install signal handlers for clean shutdown */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    Config cfg;
    config_init(&cfg);

    if (config_parse_args(&cfg, argc, argv) != 0)
        return EXIT_FAILURE;

    if (config_validate(&cfg) != 0) {
        fprintf(stderr, "Run '%s -h' for usage.\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (cfg.verbose)
        config_print(&cfg);

    return run(&cfg);
}
