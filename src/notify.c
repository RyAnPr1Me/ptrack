#include "notify.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/* ---------- Email body builder ------------------------------------------ */

/*
 * Build a minimal RFC 2822 email message and store it in *out_buf.
 * The caller must free *out_buf.
 * Returns 0 on success, -1 on failure.
 */
static int build_email(const Config *cfg,
                        double current_price,
                        double reference_price,
                        const char *alert_reason,
                        char **out_buf,
                        size_t *out_len)
{
    /* RFC 2822 date */
    time_t  now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    char   date_str[64];
    strftime(date_str, sizeof(date_str),
             "%a, %d %b %Y %H:%M:%S +0000", tm_info);

    double savings = reference_price - current_price;
    double pct     = (reference_price > 0.0)
                     ? (savings / reference_price * 100.0)
                     : 0.0;

    /* Subject line */
    char subject[256];
    snprintf(subject, sizeof(subject),
             "ptrack: Price alert — $%.2f (%s)",
             current_price, alert_reason);

    /* Plain-text body — large enough for MAX_URL_LEN + fixed text */
    size_t body_cap = MAX_URL_LEN + 512;
    char  *body     = malloc(body_cap);
    if (!body)
        return -1;
    snprintf(body, body_cap,
             "Hello,\r\n\r\n"
             "ptrack has detected a price change you may be interested in.\r\n\r\n"
             "  Product URL    : %s\r\n"
             "  Current price  : $%.2f\r\n"
             "  Reference price: $%.2f\r\n"
             "  Savings        : $%.2f (%.1f%%)\r\n"
             "  Alert reason   : %s\r\n\r\n"
             "This alert was sent by ptrack.\r\n",
             cfg->url,
             current_price,
             reference_price,
             savings,
             pct,
             alert_reason);

    /* Full RFC 2822 message */
    size_t cap = body_cap + MAX_EMAIL_LEN * 2 + 512;
    char  *msg = malloc(cap);
    if (!msg) {
        free(body);
        return -1;
    }

    int n = snprintf(msg, cap,
             "Date: %s\r\n"
             "From: %s\r\n"
             "To: %s\r\n"
             "Subject: %s\r\n"
             "MIME-Version: 1.0\r\n"
             "Content-Type: text/plain; charset=UTF-8\r\n"
             "\r\n"
             "%s",
             date_str,
             cfg->email_from,
             cfg->email_to,
             subject,
             body);
    free(body);
    if (n < 0 || (size_t)n >= cap) {
        free(msg);
        return -1;
    }

    *out_buf = msg;
    *out_len = (size_t)n;
    return 0;
}

/* ---------- SMTP send via libcurl --------------------------------------- */

typedef struct {
    const char *data;
    size_t      pos;
    size_t      len;
} ReadState;

static size_t read_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    ReadState *rs  = (ReadState *)ud;
    size_t     avail = rs->len - rs->pos;
    size_t     want  = size * nmemb;
    size_t     give  = (want < avail) ? want : avail;

    if (give == 0)
        return 0;

    memcpy(ptr, rs->data + rs->pos, give);
    rs->pos += give;
    return give;
}

static int send_via_smtp(const Config *cfg,
                          const char *msg, size_t msg_len)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    ReadState rs = { msg, 0, msg_len };

    struct curl_slist *rcpt = NULL;
    rcpt = curl_slist_append(rcpt, cfg->email_to);

    curl_easy_setopt(curl, CURLOPT_URL,          cfg->smtp_url);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM,    cfg->email_from);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT,    rcpt);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_cb);
    curl_easy_setopt(curl, CURLOPT_READDATA,     &rs);
    curl_easy_setopt(curl, CURLOPT_UPLOAD,       1L);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE,   (curl_off_t)msg_len);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,      30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    if (cfg->smtp_user[0]) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, cfg->smtp_user);
        curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg->smtp_pass);
        /* Let libcurl auto-negotiate the best authentication method */
    }

    /* Use STARTTLS if not already smtps:// */
    if (strncmp(cfg->smtp_url, "smtps://", 8) != 0)
        curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_TRY);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(rcpt);
    curl_easy_cleanup(curl);

    return (rc == CURLE_OK) ? 0 : -1;
}

/* ---------- sendmail fallback ------------------------------------------- */

/*
 * Check that an email address string is safe to pass as an exec argument.
 * A valid address contains only: alnum . @ _ - +
 * Returns 1 if safe, 0 if not.
 */
static int email_is_safe(const char *email)
{
    if (!email || !*email)
        return 0;
    for (const char *p = email; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && c != '@' && c != '.' &&
            c != '_'    && c != '-' && c != '+')
            return 0;
    }
    return 1;
}

static int send_via_sendmail(const Config *cfg, const char *msg)
{
    /* Validate email_from before passing to exec to prevent argument
     * injection even via exec (e.g. options disguised as addresses). */
    if (!email_is_safe(cfg->email_from)) {
        fprintf(stderr,
                "ptrack: unsafe characters in email_from; "
                "cannot use sendmail fallback.\n");
        return -1;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: wire read-end of pipe to stdin, then exec sendmail */
        close(pipefd[1]);
        if (dup2(pipefd[0], STDIN_FILENO) == -1)
            _exit(127);
        close(pipefd[0]);
        /* sendmail -t  : read recipient from To: header
         * sendmail -f  : set envelope sender (safe; validated above) */
        execl("/usr/sbin/sendmail", "sendmail", "-t",
              "-f", cfg->email_from, (char *)NULL);
        /* If /usr/sbin/sendmail is not found, try PATH */
        execlp("sendmail", "sendmail", "-t",
               "-f", cfg->email_from, (char *)NULL);
        _exit(127);
    }

    /* Parent: write message to the pipe then wait for child */
    close(pipefd[0]);
    size_t total = strlen(msg);
    size_t written = 0;
    while (written < total) {
        ssize_t n = write(pipefd[1], msg + written, total - written);
        if (n <= 0)
            break;
        written += (size_t)n;
    }
    close(pipefd[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* ---------- Public API -------------------------------------------------- */

int send_price_alert(const Config *cfg,
                      double current_price,
                      double reference_price,
                      const char *alert_reason)
{
    char   *msg     = NULL;
    size_t  msg_len = 0;

    if (build_email(cfg, current_price, reference_price,
                    alert_reason, &msg, &msg_len) != 0) {
        fprintf(stderr, "ptrack: failed to build email message\n");
        return -1;
    }

    int ret;

    if (cfg->smtp_url[0]) {
        ret = send_via_smtp(cfg, msg, msg_len);
        if (ret != 0)
            fprintf(stderr,
                    "ptrack: SMTP send failed (url=%s); "
                    "check credentials and smtp_url.\n",
                    cfg->smtp_url);
    } else {
        /* Fall back to system sendmail */
        ret = send_via_sendmail(cfg, msg);
        if (ret != 0)
            fprintf(stderr,
                    "ptrack: sendmail failed; install sendmail/msmtp or "
                    "provide -s SMTP_URL.\n");
    }

    free(msg);
    return ret;
}
