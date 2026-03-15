#include "fetch.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------- HTTP fetch helpers ------------------------------------------ */

#define INITIAL_BUFSIZE (512 * 1024)   /* 512 KiB – grows as needed */

typedef struct {
    char  *data;
    size_t size;
    size_t cap;
} Buf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *ud)
{
    Buf   *b   = (Buf *)ud;
    size_t n   = size * nmemb;

    if (b->size + n + 1 > b->cap) {
        size_t nc = b->cap * 2;
        while (nc < b->size + n + 1)
            nc *= 2;
        char *tmp = realloc(b->data, nc);
        if (!tmp)
            return 0;
        b->data = tmp;
        b->cap  = nc;
    }
    memcpy(b->data + b->size, ptr, n);
    b->size += n;
    b->data[b->size] = '\0';
    return n;
}

/* Fetch URL and return the body as a malloc'd string (caller frees).
 * Returns NULL on error. */
static char *http_get(const char *url, int verbose)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return NULL;

    Buf b;
    b.data = malloc(INITIAL_BUFSIZE);
    if (!b.data) {
        curl_easy_cleanup(curl);
        return NULL;
    }
    b.data[0] = '\0';
    b.size    = 0;
    b.cap     = INITIAL_BUFSIZE;

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &b);
    /* Mimic a real browser to avoid bot detection */
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    NULL);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,      10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    /* Some stores need an Accept-Language header */
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept-Language: en-US,en;q=0.9");
    hdrs = curl_slist_append(hdrs, "Accept: text/html,application/xhtml+xml,"
                                   "application/xml;q=0.9,*/*;q=0.8");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        if (verbose)
            fprintf(stderr, "ptrack: HTTP error: %s\n",
                    curl_easy_strerror(rc));
        free(b.data);
        return NULL;
    }
    return b.data;
}

/* ---------- Price-string parser ----------------------------------------- */

/*
 * Skip a single UTF-8 currency symbol at *s and return the pointer past it.
 * Handles:
 *   $  (0x24, 1 byte)
 *   ¢ £ ¥ (0xC2 0xA2/0xA3/0xA5, 2 bytes)
 *   € ₹ ₩ etc. (0xE2 0x82 ..., 3 bytes)
 * Returns the original pointer if no currency symbol is found.
 */
static const char *skip_currency_sym(const char *s)
{
    unsigned char c = (unsigned char)*s;
    if (c == '$')
        return s + 1;
    /* 2-byte UTF-8 starting with 0xC2 covers ¢ (0xA2), £ (0xA3), ¥ (0xA5) */
    if (c == 0xC2 && (unsigned char)s[1] != 0)
        return s + 2;
    /* 3-byte UTF-8 starting with 0xE2 0x82 covers € (0xAC), ₹ (0xB9), ₩ (0xA9) */
    if (c == 0xE2 && (unsigned char)s[1] == 0x82 && (unsigned char)s[2] != 0)
        return s + 3;
    return s;
}

/* Returns non-zero if *s begins a currency symbol (single or multi-byte). */
static int is_currency_sym(const char *s)
{
    return skip_currency_sym(s) != s;
}

/*
 * Try to convert a string that may start with a currency symbol into a
 * positive double.  Handles "1,234.56", "$29.99", "€ 1.299,00" (DE style).
 * Returns > 0 on success, -1 on failure.
 */
static double parse_price(const char *s)
{
    if (!s)
        return -1.0;

    /* Skip whitespace and currency symbols */
    for (;;) {
        while (*s && isspace((unsigned char)*s))
            s++;
        const char *ns = skip_currency_sym(s);
        if (ns == s)
            break;
        s = ns;
    }

    /* Skip known 3-letter currency codes like "USD", "EUR" */
    if (isupper((unsigned char)s[0]) &&
        isupper((unsigned char)s[1]) &&
        isupper((unsigned char)s[2]) &&
        (s[3] == ' ' || s[3] == '\t'))
        s += 4;

    if (!isdigit((unsigned char)*s))
        return -1.0;

    char buf[48];
    int  i            = 0;
    int  dots         = 0;
    int  commas       = 0;
    const char *p     = s;

    while (*p && i < 47) {
        if (isdigit((unsigned char)*p)) {
            buf[i++] = *p;
        } else if (*p == '.') {
            dots++;
            buf[i++] = *p;
        } else if (*p == ',') {
            commas++;
            buf[i++] = *p;
        } else {
            break;
        }
        p++;
    }
    buf[i] = '\0';

    if (i == 0)
        return -1.0;

    /* Normalise: if commas appear after last dot → thousands separators */
    if (dots == 1 && commas >= 1) {
        /* e.g. "1,234.56" → remove commas */
        char norm[48];
        int  j = 0;
        for (int k = 0; k < i; k++)
            if (buf[k] != ',')
                norm[j++] = buf[k];
        norm[j] = '\0';
        double v = atof(norm);
        return (v > 0.0) ? v : -1.0;
    }
    if (commas == 1 && dots == 0) {
        /* Could be "29,99" (European decimal) – treat comma as decimal */
        buf[strcspn(buf, ",")] = '.';
    } else if (commas > 1 && dots == 0) {
        /* "1,234,567" → strip commas */
        char norm[48];
        int  j = 0;
        for (int k = 0; k < i; k++)
            if (buf[k] != ',')
                norm[j++] = buf[k];
        norm[j] = '\0';
        memcpy(buf, norm, (size_t)j + 1);
    }
    /* dots > 1: probably a version string – ignore */
    if (dots > 1)
        return -1.0;

    double v = atof(buf);
    return (v > 0.0) ? v : -1.0;
}

/* ---------- Extraction strategies --------------------------------------- */

/*
 * Return a malloc'd copy of the first occurrence of needle inside
 * [start, end).  Returns NULL if not found.
 */
static const char *find_str(const char *hay, const char *needle)
{
    return strstr(hay, needle);
}

/*
 * Extract a quoted attribute value (single or double quotes) immediately
 * after attr_name= within the window [p, p+win_size).
 * The value is written into out (max out_len bytes including NUL).
 * Returns 1 on success, 0 on failure.
 */
static int extract_attr(const char *p, size_t win_size,
                        const char *attr_name,
                        char *out, size_t out_len)
{
    const char *end = p + win_size;
    const char *a   = find_str(p, attr_name);
    if (!a || a >= end)
        return 0;

    a += strlen(attr_name);
    while (*a && (*a == ' ' || *a == '='))
        a++;
    if (!*a)
        return 0;

    char q = 0;
    if (*a == '"' || *a == '\'') {
        q = *a++;
    }

    size_t n = 0;
    while (*a && n < out_len - 1) {
        if (q && *a == q)
            break;
        if (!q && (isspace((unsigned char)*a) || *a == '>' || *a == '/'))
            break;
        out[n++] = *a++;
    }
    out[n] = '\0';
    return (n > 0) ? 1 : 0;
}

/* --- Strategy 1: JSON-LD structured data -------------------------------- */
/*
 * Looks for <script type="application/ld+json"> blocks and hunts for
 * "price":"XX.XX" or "price":XX.XX patterns.
 */
static double price_from_jsonld(const char *html)
{
    const char *p = html;
    while ((p = find_str(p, "application/ld+json")) != NULL) {
        p += strlen("application/ld+json");
        const char *start = find_str(p, ">");
        if (!start)
            break;
        start++;
        const char *end = find_str(start, "</script>");
        if (!end)
            break;

        /* Search for "price" key within this script block */
        const char *q = start;
        while (q < end && (q = find_str(q, "\"price\"")) != NULL) {
            q += 7; /* skip "price" */
            /* Skip whitespace and colon */
            while (q < end && (*q == ' ' || *q == ':' || *q == '\t'))
                q++;
            if (q >= end)
                break;

            char val[64];
            int  vi = 0;
            const char *vp = q;

            if (*vp == '"') {
                vp++;
                while (vi < 63 && *vp && *vp != '"')
                    val[vi++] = *vp++;
            } else {
                while (vi < 63 && *vp &&
                       (isdigit((unsigned char)*vp) || *vp == '.' || *vp == ','))
                    val[vi++] = *vp++;
            }
            val[vi] = '\0';

            double price = parse_price(val);
            if (price > 0.0)
                return price;
            q++;
        }
        p = end + 9;
    }
    return PRICE_NOT_FOUND;
}

/* --- Strategy 2: Meta / microdata tags ---------------------------------- */
static double price_from_meta(const char *html)
{
    /* itemprop="price" content="XX" */
    const char *p = html;
    while ((p = find_str(p, "itemprop")) != NULL) {
        const char *tag_start = p;
        /* walk back to find the '<' */
        while (tag_start > html && *tag_start != '<')
            tag_start--;

        /* find end of tag */
        const char *tag_end = find_str(p, ">");
        if (!tag_end) {
            p++;
            continue;
        }
        size_t win = (size_t)(tag_end - tag_start + 1);

        char iprop[64];
        if (extract_attr(tag_start, win, "itemprop", iprop, sizeof(iprop)) &&
            strcmp(iprop, "price") == 0) {
            char content[64];
            if (extract_attr(tag_start, win, "content",
                             content, sizeof(content))) {
                double price = parse_price(content);
                if (price > 0.0)
                    return price;
            }
        }
        p = tag_end + 1;
    }

    /* <meta property="product:price:amount" content="XX" /> */
    p = html;
    while ((p = find_str(p, "product:price:amount")) != NULL) {
        const char *tag_start = p;
        while (tag_start > html && *tag_start != '<')
            tag_start--;
        const char *tag_end = find_str(p, ">");
        if (!tag_end) { p++; continue; }
        size_t win = (size_t)(tag_end - tag_start + 1);
        char content[64];
        if (extract_attr(tag_start, win, "content",
                         content, sizeof(content))) {
            double price = parse_price(content);
            if (price > 0.0)
                return price;
        }
        p = tag_end + 1;
    }

    /* <meta property="og:price:amount" content="XX" /> */
    p = html;
    while ((p = find_str(p, "og:price:amount")) != NULL) {
        const char *tag_start = p;
        while (tag_start > html && *tag_start != '<')
            tag_start--;
        const char *tag_end = find_str(p, ">");
        if (!tag_end) { p++; continue; }
        size_t win = (size_t)(tag_end - tag_start + 1);
        char content[64];
        if (extract_attr(tag_start, win, "content",
                         content, sizeof(content))) {
            double price = parse_price(content);
            if (price > 0.0)
                return price;
        }
        p = tag_end + 1;
    }

    return PRICE_NOT_FOUND;
}

/* --- Strategy 3: Amazon-specific --------------------------------------- */
/*
 * Amazon renders the accessible price in
 *   <span class="a-offscreen">$29.99</span>
 * which is in the raw HTML before JavaScript runs – very reliable.
 * We also try the older priceblock IDs.
 */
static double price_from_amazon(const char *html)
{
    /* a-offscreen span (most reliable) */
    const char *p = html;
    while ((p = find_str(p, "a-offscreen")) != NULL) {
        const char *gt = find_str(p, ">");
        if (!gt) { p++; continue; }
        gt++;
        const char *lt = find_str(gt, "<");
        if (!lt) { p++; continue; }
        size_t len = (size_t)(lt - gt);
        if (len > 0 && len < 32) {
            char buf[32];
            memcpy(buf, gt, len);
            buf[len] = '\0';
            double price = parse_price(buf);
            if (price > 0.0)
                return price;
        }
        p = lt;
    }

    /* priceblock_ourprice / priceblock_dealprice */
    const char *ids[] = {
        "priceblock_ourprice", "priceblock_dealprice",
        "priceblock_saleprice", NULL
    };
    for (int i = 0; ids[i]; i++) {
        p = find_str(html, ids[i]);
        if (!p)
            continue;
        const char *gt = find_str(p, ">");
        if (!gt)
            continue;
        gt++;
        const char *lt = find_str(gt, "<");
        if (!lt)
            continue;
        size_t len = (size_t)(lt - gt);
        if (len > 0 && len < 32) {
            char buf[32];
            memcpy(buf, gt, len);
            buf[len] = '\0';
            double price = parse_price(buf);
            if (price > 0.0)
                return price;
        }
    }

    return PRICE_NOT_FOUND;
}

/* --- Strategy 4: eBay-specific ----------------------------------------- */
static double price_from_ebay(const char *html)
{
    /* <span class="ux-textspans" ...>$XX.XX</span> near "mainPrice" */
    const char *p = find_str(html, "mainPrice");
    if (!p)
        p = find_str(html, "x-price-primary");
    if (!p)
        return PRICE_NOT_FOUND;

    /* Look for a price within 512 bytes */
    const char *window = p;
    const char *win_end = p + 512;

    while (window < win_end) {
        const char *gt = find_str(window, ">");
        if (!gt || gt >= win_end)
            break;
        gt++;
        const char *lt = find_str(gt, "<");
        if (!lt)
            break;
        size_t len = (size_t)(lt - gt);
        if (len > 0 && len < 32) {
            char buf[32];
            memcpy(buf, gt, len);
            buf[len] = '\0';
            double price = parse_price(buf);
            if (price > 0.0)
                return price;
        }
        window = lt + 1;
    }
    return PRICE_NOT_FOUND;
}

/* --- Strategy 5: Walmart-specific --------------------------------------- */
static double price_from_walmart(const char *html)
{
    /*
     * Walmart embeds product data in a JSON blob assigned to
     * window.__WML_REDUX_INITIAL_STATE__ or in a <script id="__NEXT_DATA__">
     * We look for "currentPrice":{..."price":XX} or "priceDisplay":"$XX.XX"
     */
    const char *keys[] = {
        "\"currentPrice\"", "\"priceDisplay\"", "\"salePrice\"", NULL
    };
    for (int k = 0; keys[k]; k++) {
        const char *p = find_str(html, keys[k]);
        if (!p)
            continue;
        p += strlen(keys[k]);
        /* Skip up to 64 bytes looking for a number or quoted price */
        const char *end = p + 128;
        while (p < end) {
            if (isdigit((unsigned char)*p) || *p == '"' || *p == '$') {
                const char *start = p;
                if (*start == '"')
                    start++;
                char buf[32];
                size_t n = 0;
                const char *q = start;
                while (n < 31 && *q && (isdigit((unsigned char)*q) ||
                                        *q == '.' || *q == ',' || *q == '$'))
                    buf[n++] = *q++;
                buf[n] = '\0';
                double price = parse_price(buf);
                if (price > 0.0)
                    return price;
            }
            p++;
        }
    }
    return PRICE_NOT_FOUND;
}

/* --- Strategy 6: Generic fallback --------------------------------------- */
/*
 * Scan the whole document for the pattern:
 *   (price|cost|buy|sale)  … within 300 chars …  $XX.XX
 * and return the first plausible price found.
 */
static double price_from_generic(const char *html)
{
    const char *keywords[] = {
        "id=\"price\"", "class=\"price\"",
        "id='price'",   "class='price'",
        "\"price\":",   "price:",
        NULL
    };

    for (int k = 0; keywords[k]; k++) {
        const char *p = html;
        while ((p = find_str(p, keywords[k])) != NULL) {
            /* Scan forward up to 400 chars for a currency symbol + number */
            const char *end = p + 400;
            const char *q   = p;
            while (q < end && *q) {
                if (*q == '$' || is_currency_sym(q)) {
                    double price = parse_price(q);
                    if (price > 0.0)
                        return price;
                }
                q++;
            }
            p++;
        }
    }
    return PRICE_NOT_FOUND;
}

/* ---------- Public API -------------------------------------------------- */

double fetch_price(const char *url, int verbose)
{
    if (!url || !*url)
        return PRICE_NOT_FOUND;

    char *html = http_get(url, verbose);
    if (!html) {
        if (verbose)
            fprintf(stderr, "ptrack: failed to fetch URL\n");
        return PRICE_NOT_FOUND;
    }

    double price = PRICE_NOT_FOUND;

    /* Strategy 1: JSON-LD */
    price = price_from_jsonld(html);
    if (verbose && price > 0.0)
        fprintf(stderr, "ptrack: price extracted via JSON-LD: %.2f\n", price);

    /* Strategy 2: meta / microdata */
    if (price <= 0.0) {
        price = price_from_meta(html);
        if (verbose && price > 0.0)
            fprintf(stderr,
                    "ptrack: price extracted via meta/microdata: %.2f\n",
                    price);
    }

    /* Strategy 3: store-specific */
    if (price <= 0.0) {
        if (strstr(url, "amazon."))
            price = price_from_amazon(html);
        else if (strstr(url, "ebay."))
            price = price_from_ebay(html);
        else if (strstr(url, "walmart."))
            price = price_from_walmart(html);

        if (verbose && price > 0.0)
            fprintf(stderr,
                    "ptrack: price extracted via store-specific parser:"
                    " %.2f\n", price);
    }

    /* Strategy 4: generic fallback */
    if (price <= 0.0) {
        price = price_from_generic(html);
        if (verbose && price > 0.0)
            fprintf(stderr,
                    "ptrack: price extracted via generic parser: %.2f\n",
                    price);
    }

    free(html);
    return price;
}
