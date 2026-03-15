#ifndef PTRACK_FETCH_H
#define PTRACK_FETCH_H

#define PRICE_NOT_FOUND (-1.0)

/*
 * Fetch the current price from url.
 * Returns the price (> 0) on success, PRICE_NOT_FOUND on failure.
 * If verbose is non-zero, diagnostic messages are printed to stderr.
 */
double fetch_price(const char *url, int verbose);

#endif /* PTRACK_FETCH_H */
