#ifndef PTRACK_CONFIG_H
#define PTRACK_CONFIG_H

#include <stdbool.h>

#define PTRACK_VERSION   "1.0.0"

#define MAX_URL_LEN      2048
#define MAX_EMAIL_LEN    256
#define MAX_PASS_LEN     256
#define MAX_PATH_LEN     4096
#define MAX_MILESTONES   16

#define DEFAULT_INTERVAL 3600   /* 1 hour */

typedef struct {
    char   url[MAX_URL_LEN];          /* Product URL to track */
    double target_price;              /* 0.0 = use initial fetched price */
    long   interval;                  /* Seconds between checks */
    char   email_to[MAX_EMAIL_LEN];   /* Recipient email */
    char   email_from[MAX_EMAIL_LEN]; /* Sender email */
    char   smtp_url[MAX_URL_LEN];     /* e.g. smtps://smtp.gmail.com:465 */
    char   smtp_user[MAX_EMAIL_LEN];  /* SMTP username (optional) */
    char   smtp_pass[MAX_PASS_LEN];   /* SMTP password (optional) */
    double milestones[MAX_MILESTONES];/* % drops below reference to notify */
    int    num_milestones;
    long   max_checks;                /* 0 = unlimited */
    bool   check_once;                /* Exit after first check */
    bool   verbose;
    char   state_file[MAX_PATH_LEN];  /* File to persist tracking state */
} Config;

/* Initialize config with default values */
void config_init(Config *cfg);

/* Load key=value pairs from file into cfg (missing keys are left as-is) */
int config_load(Config *cfg, const char *path);

/* Parse command-line arguments into cfg */
int config_parse_args(Config *cfg, int argc, char *argv[]);

/* Validate required fields; prints errors to stderr */
int config_validate(const Config *cfg);

/* Print current config (verbose mode) */
void config_print(const Config *cfg);

#endif /* PTRACK_CONFIG_H */
