# ptrack

A minimal CLI price-tracking tool written in C.

Feed it an Amazon (or other online store) product URL and it will periodically
check the price, then send you an email when the price drops to or below a
threshold you specify — or when it hits percentage milestones you define.

---

## Features

- Tracks prices on **Amazon, eBay, Walmart**, and any site that uses
  JSON-LD structured data or standard meta/microdata price tags.
- User-configurable **check interval** (default: every hour).
- Alert when price is **at or below a target price** you set.
- Optional **milestone alerts** at user-defined percentage drops
  (e.g. notify me at 5%, 10%, and 25% below the starting price).
- Sends alerts via **SMTP** (with TLS support) or falls back to the
  system **sendmail** binary if no SMTP server is configured.
- **Persists state** between runs so you can stop and resume without
  losing history or triggering duplicate alerts.
- Designed for minimal systems — only requires a C99 compiler and
  **libcurl**.

---

## Requirements

| Dependency  | Version  | Notes |
|-------------|----------|-------|
| gcc / clang | C99      | any recent version |
| libcurl     | ≥ 7.x    | with SSL support recommended |

### Install libcurl on common distros

```sh
# Debian / Ubuntu
sudo apt-get install libcurl4-openssl-dev

# Fedora / RHEL / CentOS
sudo dnf install libcurl-devel

# Alpine
apk add curl-dev

# macOS (Homebrew)
brew install curl
```

---

## Building

```sh
make          # produces ./ptrack
make install  # installs to /usr/local/bin (use PREFIX= to override)
make check    # run unit tests
make clean
```

---

## Usage

```
ptrack [OPTIONS]

Options:
  -u URL        Product URL to track (required)
  -p PRICE      Target price; alert when price <= PRICE
                (default: initial fetched price)
  -i SECS       Check interval in seconds (default: 3600)
  -e EMAIL      Recipient email address (required)
  -f FROM       Sender email address (default: ptrack@localhost)
  -s SMTP       SMTP server URL (e.g. smtps://smtp.gmail.com:465)
  -U USER       SMTP username
  -P PASS       SMTP password
  -m PCT,...    Milestone % drops to notify at (e.g. 5,10,25)
  -c FILE       Config file path (default: ~/.ptrackrc)
  -n COUNT      Max checks (0 = unlimited, default: 0)
  -1            Check once and exit
  -v            Verbose output
  -h            Show this help
```

---

## Quick start

### Watch an Amazon product; alert when it drops below $25

```sh
ptrack \
  -u 'https://www.amazon.com/dp/B0EXAMPLE' \
  -e you@example.com \
  -p 25.00 \
  -s smtps://smtp.gmail.com:465 \
  -U you@gmail.com \
  -P 'your-app-password'
```

### Alert at 10% and 20% below the starting price, check every 30 minutes

```sh
ptrack \
  -u 'https://www.amazon.com/dp/B0EXAMPLE' \
  -e you@example.com \
  -m 10,20 \
  -i 1800 \
  -s smtps://smtp.gmail.com:465 \
  -U you@gmail.com \
  -P 'your-app-password'
```

### Run in the background with nohup

```sh
nohup ptrack -c ~/.ptrackrc > ~/.ptrack.log 2>&1 &
```

### Run as a cron job (check once per hour)

```cron
0 * * * * /usr/local/bin/ptrack -c ~/.ptrackrc -1
```

---

## Config file (`~/.ptrackrc`)

All CLI options can also be placed in a config file:

```ini
url           = https://www.amazon.com/dp/B0EXAMPLE
target_price  = 19.99
interval      = 3600
email_to      = you@example.com
email_from    = ptrack@example.com
smtp_url      = smtps://smtp.gmail.com:465
smtp_user     = you@gmail.com
smtp_pass     = your-app-password
milestones    = 5,10,25
verbose       = false
```

Command-line flags override config file values.

---

## Email configuration

### Gmail (App Password)

1. Enable 2-factor authentication on your Google account.
2. Create an **App Password** at <https://myaccount.google.com/apppasswords>.
3. Use:
   ```
   smtp_url  = smtps://smtp.gmail.com:465
   smtp_user = you@gmail.com
   smtp_pass = xxxx-xxxx-xxxx-xxxx   # the app password
   ```

### Outlook / Hotmail

```
smtp_url  = smtp://smtp-mail.outlook.com:587
smtp_user = you@outlook.com
smtp_pass = yourpassword
```

### No SMTP server (sendmail fallback)

If `smtp_url` is omitted, ptrack calls the system `sendmail -t` binary.
You can satisfy this with lightweight tools such as **msmtp**:

```sh
sudo apt-get install msmtp msmtp-mta
```

---

## State file

ptrack stores its tracking state in `~/.ptrack_state` (or the path set by
`state_file` in the config).  Delete this file to reset the tracker and
re-fetch the initial price.

---

## Supported stores

| Store    | Extraction method |
|----------|-------------------|
| Amazon   | `<span class="a-offscreen">` (screen-reader price) |
| eBay     | `mainPrice` / `x-price-primary` region |
| Walmart  | JSON price fields in page data |
| Any site | JSON-LD `offers.price`, `itemprop="price"`, `og:price:amount`, `product:price:amount` |

> **Note**: Sites that load prices exclusively via JavaScript after the initial
> page load may not be detectable by ptrack.  Most major retailers include the
> price in the server-rendered HTML that ptrack fetches.

---

## License

MIT
