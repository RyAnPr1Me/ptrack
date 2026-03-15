/*
 * tests/test_fetch.c
 *
 * Unit tests for the price extraction logic in fetch.c.
 * We duplicate the relevant static helpers here so they can be tested
 * without linking against libcurl or making real network requests.
 *
 * Build & run:
 *   make -C .. tests
 *   ./test_fetch
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

/* ---- Minimal stubs replicated from fetch.c ----------------------------- */

#define PRICE_NOT_FOUND (-1.0)

static const char *skip_currency_sym(const char *s)
{
    unsigned char c = (unsigned char)*s;
    if (c == '$')
        return s + 1;
    if (c == 0xC2 && (unsigned char)s[1] != 0)
        return s + 2;
    if (c == 0xE2 && (unsigned char)s[1] == 0x82 && (unsigned char)s[2] != 0)
        return s + 3;
    return s;
}

static int is_currency_sym(const char *s) __attribute__((unused));
static int is_currency_sym(const char *s)
{
    return skip_currency_sym(s) != s;
}

static double parse_price(const char *s)
{
    if (!s)
        return -1.0;
    for (;;) {
        while (*s && isspace((unsigned char)*s))
            s++;
        const char *ns = skip_currency_sym(s);
        if (ns == s)
            break;
        s = ns;
    }
    /* Skip known 3-letter currency codes */
    if (isupper((unsigned char)s[0]) &&
        isupper((unsigned char)s[1]) &&
        isupper((unsigned char)s[2]) &&
        (s[3] == ' ' || s[3] == '\t'))
        s += 4;
    if (!isdigit((unsigned char)*s))
        return -1.0;
    char buf[48];
    int  i      = 0;
    int  dots   = 0;
    int  commas = 0;
    const char *p = s;
    while (*p && i < 47) {
        if (isdigit((unsigned char)*p))      { buf[i++] = *p; }
        else if (*p == '.')                  { dots++;   buf[i++] = *p; }
        else if (*p == ',')                  { commas++; buf[i++] = *p; }
        else                                 { break; }
        p++;
    }
    buf[i] = '\0';
    if (i == 0)
        return -1.0;
    if (dots == 1 && commas >= 1) {
        char norm[48]; int j = 0;
        for (int k = 0; k < i; k++)
            if (buf[k] != ',') norm[j++] = buf[k];
        norm[j] = '\0';
        double v = atof(norm);
        return (v > 0.0) ? v : -1.0;
    }
    if (commas == 1 && dots == 0)
        buf[strcspn(buf, ",")] = '.';
    else if (commas > 1 && dots == 0) {
        char norm[48]; int j = 0;
        for (int k = 0; k < i; k++)
            if (buf[k] != ',') norm[j++] = buf[k];
        norm[j] = '\0';
        strcpy(buf, norm);
    }
    if (dots > 1)
        return -1.0;
    double v = atof(buf);
    return (v > 0.0) ? v : -1.0;
}

static const char *find_str(const char *hay, const char *needle)
{
    return strstr(hay, needle);
}

/* JSON-LD extraction (copied from fetch.c) */
static double price_from_jsonld(const char *html)
{
    const char *p = html;
    while ((p = find_str(p, "application/ld+json")) != NULL) {
        p += strlen("application/ld+json");
        const char *start = find_str(p, ">");
        if (!start) break;
        start++;
        const char *end = find_str(start, "</script>");
        if (!end) break;
        const char *q = start;
        while (q < end && (q = find_str(q, "\"price\"")) != NULL) {
            q += 7;
            while (q < end && (*q == ' ' || *q == ':' || *q == '\t'))
                q++;
            if (q >= end) break;
            char val[64]; int vi = 0;
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
            if (price > 0.0) return price;
            q++;
        }
        p = end + 9;
    }
    return PRICE_NOT_FOUND;
}

/* Meta/microdata extraction (copied from fetch.c) */
static int extract_attr(const char *p, size_t win_size,
                         const char *attr_name, char *out, size_t out_len)
{
    const char *end = p + win_size;
    const char *a   = find_str(p, attr_name);
    if (!a || a >= end) return 0;
    a += strlen(attr_name);
    while (*a && (*a == ' ' || *a == '=')) a++;
    if (!*a) return 0;
    char q = 0;
    if (*a == '"' || *a == '\'') q = *a++;
    size_t n = 0;
    while (*a && n < out_len - 1) {
        if (q && *a == q) break;
        if (!q && (isspace((unsigned char)*a) || *a == '>' || *a == '/')) break;
        out[n++] = *a++;
    }
    out[n] = '\0';
    return (n > 0) ? 1 : 0;
}

static double price_from_meta(const char *html)
{
    const char *p = html;
    while ((p = find_str(p, "itemprop")) != NULL) {
        const char *tag_start = p;
        while (tag_start > html && *tag_start != '<') tag_start--;
        const char *tag_end = find_str(p, ">");
        if (!tag_end) { p++; continue; }
        size_t win = (size_t)(tag_end - tag_start + 1);
        char iprop[64];
        if (extract_attr(tag_start, win, "itemprop", iprop, sizeof(iprop)) &&
            strcmp(iprop, "price") == 0) {
            char content[64];
            if (extract_attr(tag_start, win, "content",
                             content, sizeof(content))) {
                double price = parse_price(content);
                if (price > 0.0) return price;
            }
        }
        p = tag_end + 1;
    }

    /* <meta property="product:price:amount" content="XX" /> */
    p = html;
    while ((p = find_str(p, "product:price:amount")) != NULL) {
        const char *tag_start = p;
        while (tag_start > html && *tag_start != '<') tag_start--;
        const char *tag_end = find_str(p, ">");
        if (!tag_end) { p++; continue; }
        size_t win = (size_t)(tag_end - tag_start + 1);
        char content[64];
        if (extract_attr(tag_start, win, "content", content, sizeof(content))) {
            double price = parse_price(content);
            if (price > 0.0) return price;
        }
        p = tag_end + 1;
    }

    /* <meta property="og:price:amount" content="XX" /> */
    p = html;
    while ((p = find_str(p, "og:price:amount")) != NULL) {
        const char *tag_start = p;
        while (tag_start > html && *tag_start != '<') tag_start--;
        const char *tag_end = find_str(p, ">");
        if (!tag_end) { p++; continue; }
        size_t win = (size_t)(tag_end - tag_start + 1);
        char content[64];
        if (extract_attr(tag_start, win, "content", content, sizeof(content))) {
            double price = parse_price(content);
            if (price > 0.0) return price;
        }
        p = tag_end + 1;
    }

    return PRICE_NOT_FOUND;
}

/* Amazon a-offscreen extraction (copied from fetch.c) */
static double price_from_amazon(const char *html)
{
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
            if (price > 0.0) return price;
        }
        p = lt;
    }
    return PRICE_NOT_FOUND;
}

/* ---- Test helpers ------------------------------------------------------- */

static int passed = 0;
static int failed = 0;

#define CHECK_PRICE(label, got, expected)                               \
    do {                                                                \
        double _g = (got);                                             \
        double _e = (expected);                                        \
        int    _ok = ((_g > 0.0 && _e > 0.0 &&                        \
                        _g > _e - 0.01 && _g < _e + 0.01) ||          \
                       (_g <= 0.0 && _e <= 0.0));                      \
        if (_ok) {                                                      \
            printf("  PASS  %s\n", (label));                           \
            passed++;                                                   \
        } else {                                                        \
            printf("  FAIL  %s  got=%.4f  expected=%.4f\n",           \
                   (label), _g, _e);                                   \
            failed++;                                                   \
        }                                                               \
    } while (0)

/* ---- Tests -------------------------------------------------------------- */

static void test_parse_price(void)
{
    printf("\n=== parse_price ===\n");
    CHECK_PRICE("plain integer",      parse_price("42"),         42.0);
    CHECK_PRICE("dollar sign",        parse_price("$29.99"),     29.99);
    CHECK_PRICE("with spaces",        parse_price("  $  19.99"), 19.99);
    CHECK_PRICE("thousands comma",    parse_price("$1,299.00"),  1299.0);
    CHECK_PRICE("no symbol decimal",  parse_price("9.99"),       9.99);
    CHECK_PRICE("european comma",     parse_price("29,99"),      29.99);
    CHECK_PRICE("USD prefix",         parse_price("USD 14.50"),  14.50);
    CHECK_PRICE("zero returns -1",    parse_price("0.00"),      -1.0);
    CHECK_PRICE("empty string -1",    parse_price(""),          -1.0);
    CHECK_PRICE("NULL returns -1",    parse_price(NULL),        -1.0);
    CHECK_PRICE("text only -1",       parse_price("free"),      -1.0);
}

static void test_jsonld(void)
{
    printf("\n=== JSON-LD extraction ===\n");

    const char *simple =
        "<script type=\"application/ld+json\">"
        "{\"@type\":\"Product\",\"offers\":{\"price\":\"24.99\"}}"
        "</script>";
    CHECK_PRICE("simple JSON-LD string price", price_from_jsonld(simple), 24.99);

    const char *numeric =
        "<script type=\"application/ld+json\">"
        "{\"@type\":\"Product\",\"offers\":{\"price\":39.95}}"
        "</script>";
    CHECK_PRICE("numeric JSON-LD price", price_from_jsonld(numeric), 39.95);

    const char *nested =
        "<html><head>"
        "<script type=\"application/ld+json\">"
        "{\"@context\":\"https://schema.org\",\"@type\":\"Product\","
        "\"name\":\"Widget\","
        "\"offers\":{\"@type\":\"Offer\",\"price\":\"12.49\","
        "\"priceCurrency\":\"USD\"}}"
        "</script></head></html>";
    CHECK_PRICE("full nested JSON-LD", price_from_jsonld(nested), 12.49);

    const char *none = "<html><body><p>No price here</p></body></html>";
    CHECK_PRICE("no JSON-LD returns NOT_FOUND",
                price_from_jsonld(none), PRICE_NOT_FOUND);
}

static void test_meta(void)
{
    printf("\n=== Meta/microdata extraction ===\n");

    const char *itemprop =
        "<span itemprop=\"price\" content=\"34.99\">$34.99</span>";
    CHECK_PRICE("itemprop=price content", price_from_meta(itemprop), 34.99);

    const char *og_price =
        "<meta property=\"product:price:amount\" content=\"49.95\" />";
    CHECK_PRICE("og product:price:amount", price_from_meta(og_price), 49.95);

    const char *none = "<p>No price info</p>";
    CHECK_PRICE("no meta returns NOT_FOUND",
                price_from_meta(none), PRICE_NOT_FOUND);
}

static void test_amazon(void)
{
    printf("\n=== Amazon a-offscreen extraction ===\n");

    const char *html =
        "<span class=\"a-price aok-align-center reinventPricePriceToPayMargin "
        "priceToPay\">"
        "<span class=\"a-offscreen\">$27.97</span>"
        "</span>";
    CHECK_PRICE("a-offscreen price", price_from_amazon(html), 27.97);

    const char *multi =
        "<span class=\"a-offscreen\">$100.00</span>"
        " some text "
        "<span class=\"a-offscreen\">$27.97</span>";
    /* Should return first match */
    CHECK_PRICE("a-offscreen first match", price_from_amazon(multi), 100.0);

    const char *none = "<html><p>$29.99</p></html>";
    CHECK_PRICE("no a-offscreen returns NOT_FOUND",
                price_from_amazon(none), PRICE_NOT_FOUND);
}

/* ---- Main --------------------------------------------------------------- */

int main(void)
{
    printf("ptrack price-extraction unit tests\n");
    printf("====================================\n");

    test_parse_price();
    test_jsonld();
    test_meta();
    test_amazon();

    printf("\n====================================\n");
    printf("Results: %d passed, %d failed\n", passed, failed);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
