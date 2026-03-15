#ifndef PTRACK_NOTIFY_H
#define PTRACK_NOTIFY_H

#include "config.h"

/*
 * Send a price-drop alert email.
 *
 * current_price   – the newly observed price
 * reference_price – the baseline (initial price or user target)
 * alert_reason    – short description, e.g. "below target" or "10% drop"
 *
 * Returns 0 on success, -1 on failure.
 */
int send_price_alert(const Config *cfg,
                     double current_price,
                     double reference_price,
                     const char *alert_reason);

#endif /* PTRACK_NOTIFY_H */
