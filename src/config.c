#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <getopt.h>

void config_init(Config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->interval = DEFAULT_INTERVAL;

    strncpy(cfg->email_from, "ptrack@localhost",
            sizeof(cfg->email_from) - 1);

    /* Default state file: ~/.ptrack_state */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(cfg->state_file, sizeof(cfg->state_file),
                 "%s/.ptrack_state", home);
    } else {
        strncpy(cfg->state_file, ".ptrack_state",
                sizeof(cfg->state_file) - 1);
    }
}

/* In-place trim of leading/trailing whitespace; returns pointer into s */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

/* Parse a comma-separated list of percentage milestones into cfg */
static void parse_milestones(Config *cfg, const char *val)
{
    char buf[256];
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *tok = strtok(buf, ",");
    while (tok && cfg->num_milestones < MAX_MILESTONES) {
        double m = atof(trim(tok));
        if (m > 0.0)
            cfg->milestones[cfg->num_milestones++] = m;
        tok = strtok(NULL, ",");
    }
}

int config_load(Config *cfg, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';')
            continue;

        char *eq = strchr(p, '=');
        if (!eq)
            continue;

        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        if (strcmp(key, "url") == 0) {
            strncpy(cfg->url, val, sizeof(cfg->url) - 1);
        } else if (strcmp(key, "target_price") == 0) {
            cfg->target_price = atof(val);
        } else if (strcmp(key, "interval") == 0) {
            cfg->interval = atol(val);
        } else if (strcmp(key, "email_to") == 0) {
            strncpy(cfg->email_to, val, sizeof(cfg->email_to) - 1);
        } else if (strcmp(key, "email_from") == 0) {
            strncpy(cfg->email_from, val, sizeof(cfg->email_from) - 1);
        } else if (strcmp(key, "smtp_url") == 0) {
            strncpy(cfg->smtp_url, val, sizeof(cfg->smtp_url) - 1);
        } else if (strcmp(key, "smtp_user") == 0) {
            strncpy(cfg->smtp_user, val, sizeof(cfg->smtp_user) - 1);
        } else if (strcmp(key, "smtp_pass") == 0) {
            strncpy(cfg->smtp_pass, val, sizeof(cfg->smtp_pass) - 1);
        } else if (strcmp(key, "milestones") == 0) {
            parse_milestones(cfg, val);
        } else if (strcmp(key, "verbose") == 0) {
            cfg->verbose = (strcmp(val, "true") == 0 ||
                            strcmp(val, "1")    == 0 ||
                            strcmp(val, "yes")  == 0);
        } else if (strcmp(key, "max_checks") == 0) {
            cfg->max_checks = atol(val);
        } else if (strcmp(key, "state_file") == 0) {
            strncpy(cfg->state_file, val, sizeof(cfg->state_file) - 1);
        }
    }

    fclose(f);
    return 0;
}

static void print_usage(const char *prog)
{
    printf("ptrack v" PTRACK_VERSION " - Online price tracking tool\n\n");
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  -u URL        Product URL to track (required)\n");
    printf("  -p PRICE      Target price; alert when price <= PRICE\n");
    printf("                (default: initial fetched price)\n");
    printf("  -i SECS       Check interval in seconds (default: %d)\n",
           DEFAULT_INTERVAL);
    printf("  -e EMAIL      Recipient email address (required)\n");
    printf("  -f FROM       Sender email address "
           "(default: ptrack@localhost)\n");
    printf("  -s SMTP       SMTP server URL "
           "(e.g. smtps://smtp.gmail.com:465)\n");
    printf("  -U USER       SMTP username\n");
    printf("  -P PASS       SMTP password\n");
    printf("  -m PCT,...    Milestone %% drops to notify at "
           "(e.g. 5,10,25)\n");
    printf("  -c FILE       Config file path "
           "(default: ~/.ptrackrc)\n");
    printf("  -n COUNT      Max checks (0 = unlimited, default: 0)\n");
    printf("  -1            Check once and exit\n");
    printf("  -v            Verbose output\n");
    printf("  -h            Show this help\n\n");
    printf("Config file (~/.ptrackrc) — key = value pairs:\n");
    printf("  url           = https://...\n");
    printf("  target_price  = 29.99\n");
    printf("  interval      = 3600\n");
    printf("  email_to      = user@example.com\n");
    printf("  email_from    = ptrack@example.com\n");
    printf("  smtp_url      = smtps://smtp.gmail.com:465\n");
    printf("  smtp_user     = user@gmail.com\n");
    printf("  smtp_pass     = secret\n");
    printf("  milestones    = 5,10,20\n\n");
    printf("Examples:\n");
    printf("  %s -u 'https://www.amazon.com/dp/B01234' "
           "-e me@example.com -p 19.99\n", prog);
    printf("  %s -c ~/.ptrackrc\n\n", prog);
}

int config_parse_args(Config *cfg, int argc, char *argv[])
{
    int opt;
    char default_config[MAX_PATH_LEN];

    /* Attempt to load default config file first */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(default_config, sizeof(default_config),
                 "%s/.ptrackrc", home);
        config_load(cfg, default_config); /* ignore errors */
    }

    while ((opt = getopt(argc, argv, "u:p:i:e:f:s:U:P:m:c:n:1vh")) != -1) {
        switch (opt) {
        case 'u':
            strncpy(cfg->url, optarg, sizeof(cfg->url) - 1);
            break;
        case 'p':
            cfg->target_price = atof(optarg);
            break;
        case 'i':
            cfg->interval = atol(optarg);
            break;
        case 'e':
            strncpy(cfg->email_to, optarg, sizeof(cfg->email_to) - 1);
            break;
        case 'f':
            strncpy(cfg->email_from, optarg, sizeof(cfg->email_from) - 1);
            break;
        case 's':
            strncpy(cfg->smtp_url, optarg, sizeof(cfg->smtp_url) - 1);
            break;
        case 'U':
            strncpy(cfg->smtp_user, optarg, sizeof(cfg->smtp_user) - 1);
            break;
        case 'P':
            strncpy(cfg->smtp_pass, optarg, sizeof(cfg->smtp_pass) - 1);
            break;
        case 'm':
            parse_milestones(cfg, optarg);
            break;
        case 'c':
            if (config_load(cfg, optarg) != 0) {
                fprintf(stderr, "Error: cannot read config file '%s'\n",
                        optarg);
                return -1;
            }
            break;
        case 'n':
            cfg->max_checks = atol(optarg);
            break;
        case '1':
            cfg->check_once = true;
            break;
        case 'v':
            cfg->verbose = true;
            break;
        case 'h':
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            fprintf(stderr, "Unknown option. Use -h for help.\n");
            return -1;
        }
    }

    return 0;
}

int config_validate(const Config *cfg)
{
    int ok = 0;

    if (cfg->url[0] == '\0') {
        fprintf(stderr, "Error: product URL is required (-u URL or "
                "'url' in config file).\n");
        ok = -1;
    }
    if (cfg->email_to[0] == '\0') {
        fprintf(stderr, "Error: recipient email is required (-e EMAIL or "
                "'email_to' in config file).\n");
        ok = -1;
    }
    if (cfg->interval <= 0) {
        fprintf(stderr, "Error: interval must be a positive number of "
                "seconds.\n");
        ok = -1;
    }

    return ok;
}

void config_print(const Config *cfg)
{
    printf("--- ptrack configuration ---\n");
    printf("  url          : %s\n", cfg->url);
    printf("  target_price : %.2f%s\n", cfg->target_price,
           cfg->target_price == 0.0 ? " (use initial price)" : "");
    printf("  interval     : %ld s\n", cfg->interval);
    printf("  email_to     : %s\n", cfg->email_to);
    printf("  email_from   : %s\n", cfg->email_from);
    if (cfg->smtp_url[0])
        printf("  smtp_url     : %s\n", cfg->smtp_url);
    if (cfg->smtp_user[0])
        printf("  smtp_user    : %s\n", cfg->smtp_user);
    if (cfg->num_milestones > 0) {
        printf("  milestones   :");
        for (int i = 0; i < cfg->num_milestones; i++)
            printf(" %.1f%%", cfg->milestones[i]);
        printf("\n");
    }
    if (cfg->max_checks)
        printf("  max_checks   : %ld\n", cfg->max_checks);
    printf("  state_file   : %s\n", cfg->state_file);
    printf("----------------------------\n");
}
