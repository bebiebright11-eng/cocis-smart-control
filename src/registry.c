#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "registry.h"
#include "wallclock.h"
#include "sdcard.h"

#define MAX_PEOPLE   40
#define MAX_CARDS    100
#define RESERVE_CARDS 10
#define GUEST_CARDS   20

#define REG_FILE   SDCARD_MOUNT "/registry.txt"
#define REG_BUF    12288
#define REG_HEADER "CRG2"

static const char *TAG = "registry";

typedef struct {
    char   reg_no[20];
    char   name[32];
    role_t role;
} person_rec_t;

typedef struct {
    uint8_t       uid[4];
    char          label[8];       /* the simple card ID, e.g. "A1.1" */
    card_type_t   type;
    card_status_t status;
    int           owner;          /* index into s_people, or -1 */
    int           lent_for;       /* reserve card: index of the card it replaces, or -1 */
    time_t        valid_until;    /* 0 = never expires */
    bool          inside;
} card_rec_t;

static person_rec_t      s_people[MAX_PEOPLE];
static int               s_people_count = 0;
static card_rec_t        s_cards[MAX_CARDS];
static int               s_card_count   = 0;
static SemaphoreHandle_t s_lock         = NULL;
static bool              s_dirty        = false;   /* changed since the last save */

/* ---------- Helpers (the caller must already hold the lock) ---------- */
static int find_card_by_uid(const uint8_t uid[4])
{
    for (int i = 0; i < s_card_count; i++) {
        if (memcmp(s_cards[i].uid, uid, 4) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_card_by_label(const char *label)
{
    for (int i = 0; i < s_card_count; i++) {
        if (strcasecmp(s_cards[i].label, label) == 0) {
            return i;
        }
    }
    return -1;
}

static bool is_expired(const card_rec_t *c)
{
    if (c->valid_until == 0) {
        return false;
    }
    if (!wallclock_is_set()) {
        return false;                 /* no clock yet: cannot judge */
    }
    return time(NULL) > c->valid_until;
}

static void fill_info(const card_rec_t *c, access_info_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->card_label, sizeof(out->card_label), "%s", c->label);
    out->card_type   = c->type;
    out->card_status = c->status;
    out->inside      = c->inside;
    out->valid_until = c->valid_until;
    memcpy(out->uid, c->uid, 4);

    if (c->owner >= 0) {
        const person_rec_t *p = &s_people[c->owner];
        snprintf(out->name,   sizeof(out->name),   "%s", p->name);
        snprintf(out->reg_no, sizeof(out->reg_no), "%s", p->reg_no);
        out->role = p->role;
    } else if (c->type == CARD_GUEST) {
        snprintf(out->name,   sizeof(out->name),   "Guest %s", c->label);
        snprintf(out->reg_no, sizeof(out->reg_no), "GUEST");
        out->role = ROLE_OTHER;
    } else {
        snprintf(out->name,   sizeof(out->name),   "Reserve %s", c->label);
        snprintf(out->reg_no, sizeof(out->reg_no), "-");
        out->role = ROLE_OTHER;
    }
}

/* Copy text, replacing the characters our save file uses as separators. */
static void copy_clean(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
    for (char *p = dst; *p; p++) {
        if (*p == '|' || *p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }
}

static int add_person(const char *reg_no, const char *name, role_t role)
{
    if (s_people_count >= MAX_PEOPLE) {
        return -1;
    }
    person_rec_t *p = &s_people[s_people_count];
    memset(p, 0, sizeof(*p));
    copy_clean(p->reg_no, sizeof(p->reg_no), reg_no);
    copy_clean(p->name,   sizeof(p->name),   name);
    p->role = role;
    return s_people_count++;
}

static int add_card(const uint8_t uid[4], const char *label, card_type_t type,
                    card_status_t status, int owner)
{
    if (s_card_count >= MAX_CARDS) {
        return -1;
    }
    card_rec_t *c = &s_cards[s_card_count];
    memset(c, 0, sizeof(*c));
    memcpy(c->uid, uid, 4);
    snprintf(c->label, sizeof(c->label), "%s", label);
    c->type        = type;
    c->status      = status;
    c->owner       = owner;
    c->lent_for    = -1;
    c->valid_until = 0;
    c->inside      = false;
    return s_card_count++;
}

/* ---------- Card letters and IDs by role ---------- */
static char role_letter(role_t r)
{
    switch (r) {
    case ROLE_STUDENT: return 'A';
    case ROLE_STAFF:   return 'S';
    case ROLE_CLEANER: return 'C';
    case ROLE_OTHER:   return 'O';
    }
    return 'A';
}

static uint8_t role_uid_byte(role_t r)
{
    switch (r) {
    case ROLE_STUDENT: return 0xA1;
    case ROLE_STAFF:   return 0xA2;
    case ROLE_CLEANER: return 0xA3;
    case ROLE_OTHER:   return 0xA4;
    }
    return 0xA1;
}

/* The card's ID bytes. The very first student gets the Blue Card of the
 * Wokwi RFID reader (01 02 03 04), so the real simulated card can be tested. */
static void make_uid(role_t role, int n, uint8_t uid[4])
{
    if (role == ROLE_STUDENT && n == 1) {
        uid[0] = 0x01; uid[1] = 0x02; uid[2] = 0x03; uid[3] = 0x04;
        return;
    }
    uid[0] = role_uid_byte(role);
    uid[1] = 0x00;
    uid[2] = 0x00;
    uid[3] = (uint8_t)n;
}

/* Write a card label such as "S1.7" into out (8 characters).
 * The number is kept between 0 and 999 so it always fits. */
static void make_label(char out[8], role_t role, int n)
{
    int v = n;
    if (v < 0)   { v = 0; }
    if (v > 999) { v = 999; }
    snprintf(out, 8, "%c1.%d", role_letter(role), v);
}

/* Write a guest card label such as "G1.7" into out. */
static void guest_label(char out[8], int n)
{
    int v = n;
    if (v < 0)   { v = 0; }
    if (v > 999) { v = 999; }
    snprintf(out, 8, "G1.%d", v);
}

/* The first free card label of this role (A1.n, S1.n ...), written into label. Returns n. */
static int next_normal_label(role_t role, char label[8])
{
    int n = 1;
    for (; n < 250; n++) {
        make_label(label, role, n);
        if (find_card_by_label(label) < 0) {
            break;
        }
    }
    return n;
}

/* Is s a registration number like "S/003" for this role? (3 to 6 digits after the slash) */
static bool reg_shape_ok(role_t role, const char *s)
{
    if (s == NULL || toupper((unsigned char)s[0]) != role_letter(role) || s[1] != '/') {
        return false;
    }
    int  digits = 0;
    long value  = 0;
    for (const char *p = s + 2; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
        value = value * 10 + (*p - '0');
        if (++digits > 6) {
            return false;
        }
    }
    return digits >= 3 && value >= 1;
}

/* Is s a card ID like "S1.7" for this role? Returns the number (1..250), or 0 if not. */
static int card_shape_n(role_t role, const char *s)
{
    if (s == NULL || toupper((unsigned char)s[0]) != role_letter(role) ||
        s[1] != '1' || s[2] != '.') {
        return 0;
    }
    int digits = 0;
    int value  = 0;
    for (const char *p = s + 3; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
        value = value * 10 + (*p - '0');
        if (++digits > 3) {
            return 0;
        }
    }
    if (digits == 0 || value < 1 || value > 250) {
        return 0;
    }
    return value;
}

/* Is s a guest card ID like "G1.7"? Returns the number (1..250), or 0 if not. */
static int guest_n(const char *s)
{
    if (s == NULL || toupper((unsigned char)s[0]) != 'G' ||
        s[1] != '1' || s[2] != '.') {
        return 0;
    }
    int digits = 0;
    int value  = 0;
    for (const char *p = s + 3; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
        value = value * 10 + (*p - '0');
        if (++digits > 3) {
            return 0;
        }
    }
    if (digits == 0 || value < 1 || value > 250) {
        return 0;
    }
    return value;
}

/* A guest card is free when it was never activated, or when its time is over
 * and nobody holding it is still inside. */
static bool guest_is_free(const card_rec_t *c)
{
    if (c->status == CARD_AVAILABLE) {
        return true;
    }
    return c->status == CARD_ACTIVE && !c->inside && is_expired(c);
}

void registry_init(void)
{
    s_lock         = xSemaphoreCreateMutex();
    s_people_count = 0;
    s_card_count   = 0;

    char label[8];

    /* no people at the start: the guard adds everyone */

    /* reserve cards B1.1 ... */
    for (int i = 1; i <= RESERVE_CARDS; i++) {
        uint8_t uid[4] = { 0xB1, 0x00, 0x00, (uint8_t)i };
        snprintf(label, sizeof(label), "B1.%d", i);
        add_card(uid, label, CARD_RESERVE, CARD_AVAILABLE, -1);
    }

    /* guest cards G1.1 ... */
    for (int i = 1; i <= GUEST_CARDS; i++) {
        uint8_t uid[4] = { 0xC1, 0x00, 0x00, (uint8_t)i };
        snprintf(label, sizeof(label), "G1.%d", i);
        add_card(uid, label, CARD_GUEST, CARD_AVAILABLE, -1);
    }

    s_dirty = true;     /* the first save writes this starting data to the card */

    ESP_LOGI(TAG, "registry ready: %d people, %d cards", s_people_count, s_card_count);
}

/* ---------- Saving and loading (SD card) ---------- */
static void append(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    if (*len >= cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *len += (size_t)n;
        if (*len > cap) {
            *len = cap;
        }
    }
}

/* Cut a line at every '|' and point f[] at the pieces. Returns how many. */
static int split_fields(char *line, char *f[], int max)
{
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p != '\0' && n < max; p++) {
        if (*p == '|') {
            *p = '\0';
            f[n++] = p + 1;
        }
    }
    return n;
}

bool registry_save_if_dirty(void)
{
    if (!sdcard_is_ready()) {
        return false;
    }

    char *buf = malloc(REG_BUF);
    if (buf == NULL) {
        return false;
    }
    size_t len  = 0;
    bool   need = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dirty) {
        need    = true;
        s_dirty = false;

        append(buf, REG_BUF, &len, REG_HEADER "\n");
        for (int i = 0; i < s_people_count; i++) {
            append(buf, REG_BUF, &len, "P|%s|%s|%d\n",
                   s_people[i].reg_no, s_people[i].name, (int)s_people[i].role);
        }
        for (int i = 0; i < s_card_count; i++) {
            const card_rec_t *c = &s_cards[i];
            append(buf, REG_BUF, &len, "C|%s|%02X%02X%02X%02X|%d|%d|%d|%d|%lld|%d\n",
                   c->label,
                   c->uid[0], c->uid[1], c->uid[2], c->uid[3],
                   (int)c->type, (int)c->status, c->owner, c->lent_for,
                   (long long)c->valid_until, c->inside ? 1 : 0);
        }
        append(buf, REG_BUF, &len, "END\n");
    }
    xSemaphoreGive(s_lock);

    if (!need) {
        free(buf);
        return true;
    }

    /* the slow part (writing the file) happens without holding the lock */
    bool ok = false;
    FILE *f = fopen(REG_FILE, "w");
    if (f != NULL) {
        ok = (fwrite(buf, 1, len, f) == len);
        if (fclose(f) != 0) {
            ok = false;
        }
    }
    free(buf);

    if (ok) {
        ESP_LOGI(TAG, "registry saved to the SD card (%u bytes)", (unsigned)len);
    } else {
        ESP_LOGE(TAG, "could not save the registry to the SD card");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_dirty = true;                       /* try again next time */
        xSemaphoreGive(s_lock);
    }
    return ok;
}

bool registry_load_from_sd(void)
{
    if (!sdcard_is_ready()) {
        return false;
    }

    FILE *f = fopen(REG_FILE, "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "no saved registry yet: starting empty");
        return false;
    }

    /* read into temporary arrays first, so a damaged file changes nothing */
    static person_rec_t tp[MAX_PEOPLE];
    static card_rec_t   tc[MAX_CARDS];
    int  np = 0, nc = 0;
    bool header_ok = false, end_ok = false, bad = false, old_format = false;
    char line[160];
    char *fld[12];

    while (!bad && fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, REG_HEADER) == 0) {
            header_ok = true;
        } else if (strcmp(line, "CRG1") == 0) {
            old_format = true;
            break;
        } else if (strcmp(line, "END") == 0) {
            end_ok = true;
        } else if (line[0] == 'P') {
            int n = split_fields(line, fld, 12);
            int role = (n >= 4) ? atoi(fld[3]) : -1;
            if (n < 4 || np >= MAX_PEOPLE || role < 0 || role > 3) {
                bad = true;
                break;
            }
            memset(&tp[np], 0, sizeof(tp[np]));
            snprintf(tp[np].reg_no, sizeof(tp[np].reg_no), "%s", fld[1]);
            snprintf(tp[np].name,   sizeof(tp[np].name),   "%s", fld[2]);
            tp[np].role = (role_t)role;
            np++;
        } else if (line[0] == 'C') {
            int n = split_fields(line, fld, 12);
            if (n < 9 || nc >= MAX_CARDS) {
                bad = true;
                break;
            }
            unsigned long u      = strtoul(fld[2], NULL, 16);
            int           type   = atoi(fld[3]);
            int           status = atoi(fld[4]);
            int           owner  = atoi(fld[5]);
            int           lent   = atoi(fld[6]);
            long long     until  = strtoll(fld[7], NULL, 10);
            int           inside = atoi(fld[8]);

            if (type < 0 || type > 2 || status < 0 || status > 3 ||
                owner < -1 || owner >= np || lent < -1 || lent >= MAX_CARDS ||
                (inside != 0 && inside != 1)) {
                bad = true;
                break;
            }
            card_rec_t *c = &tc[nc];
            memset(c, 0, sizeof(*c));
            snprintf(c->label, sizeof(c->label), "%s", fld[1]);
            c->uid[0] = (uint8_t)(u >> 24);
            c->uid[1] = (uint8_t)(u >> 16);
            c->uid[2] = (uint8_t)(u >> 8);
            c->uid[3] = (uint8_t)u;
            c->type        = (card_type_t)type;
            c->status      = (card_status_t)status;
            c->owner       = owner;
            c->lent_for    = lent;
            c->valid_until = (time_t)until;
            c->inside      = (inside == 1);
            nc++;
        }
    }
    fclose(f);

    if (old_format) {
        ESP_LOGW(TAG, "the saved registry is in the old format: ignoring it, starting fresh");
        return false;
    }

    for (int i = 0; i < nc && !bad; i++) {
        if (tc[i].lent_for >= nc) {
            bad = true;
        }
    }

    /* a registry with no people is fine (a fresh start), but it needs cards */
    if (bad || !header_ok || !end_ok || nc == 0) {
        ESP_LOGE(TAG, "the saved registry is damaged or incomplete: ignoring it");
        return false;
    }

    int inside = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_people, tp, sizeof(tp[0]) * (size_t)np);
    memcpy(s_cards,  tc, sizeof(tc[0]) * (size_t)nc);
    s_people_count = np;
    s_card_count   = nc;
    s_dirty        = false;
    for (int i = 0; i < nc; i++) {
        if (s_cards[i].inside) {
            inside++;
        }
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "registry LOADED from the SD card: %d people, %d cards, %d inside",
             np, nc, inside);
    return true;
}

/* ---------- Adding a person ---------- */

/* The shared work. strict_reg = true checks the registration number's shape
 * (A/001 ...). card = NULL or "" means "the next free card of the role". */
static int add_person_with_card(const char *reg_no, const char *name, role_t role,
                                const char *card, bool strict_reg, char label_out[8])
{
    char reg[20];
    snprintf(reg, sizeof(reg), "%s", reg_no);
    if (strict_reg) {
        if (!reg_shape_ok(role, reg)) {
            return REG_ADD_BAD_REG;
        }
        reg[0] = (char)toupper((unsigned char)reg[0]);
    }

    char want[8] = "";
    int  want_n  = 0;
    if (card != NULL && card[0] != '\0') {
        want_n = card_shape_n(role, card);
        if (want_n == 0) {
            return REG_ADD_BAD_CARD;
        }
        make_label(want, role, want_n);
    }

    int res = REG_ADD_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* the same registration number may only exist once */
    for (int i = 0; i < s_people_count; i++) {
        if (strcasecmp(s_people[i].reg_no, reg) == 0) {
            res = REG_ADD_DUPLICATE;
        }
    }

    if (res == REG_ADD_OK && want_n != 0 && find_card_by_label(want) >= 0) {
        res = REG_ADD_CARD_TAKEN;
    }

    if (res == REG_ADD_OK &&
        (s_people_count >= MAX_PEOPLE || s_card_count >= MAX_CARDS)) {
        res = REG_ADD_FULL;
    }

    if (res == REG_ADD_OK) {
        char label[8];
        int  n;
        if (want_n != 0) {
            snprintf(label, sizeof(label), "%s", want);
            n = want_n;
        } else {
            n = next_normal_label(role, label);
        }

        uint8_t uid[4];
        make_uid(role, n, uid);

        int owner = add_person(reg, name, role);
        add_card(uid, label, CARD_NORMAL, CARD_ACTIVE, owner);
        snprintf(label_out, 8, "%s", label);
        s_dirty = true;

        ESP_LOGI(TAG, "added %s (%s) with card %s", name, reg, label);
    }

    xSemaphoreGive(s_lock);
    return res;
}

int registry_add_normal(const char *reg_no, const char *name, role_t role,
                        char label_out[8])
{
    return add_person_with_card(reg_no, name, role, NULL, false, label_out);
}

int registry_add_person(const char *reg_no, const char *name, role_t role,
                        const char *card, char label_out[8])
{
    return add_person_with_card(reg_no, name, role, card, true, label_out);
}

void registry_suggest_reg_no(role_t role, char out[20])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 1;
    for (; n < 1000; n++) {
        char t[20];
        snprintf(t, sizeof(t), "%c/%03d", role_letter(role), n);
        bool used = false;
        for (int i = 0; i < s_people_count; i++) {
            if (strcasecmp(s_people[i].reg_no, t) == 0) {
                used = true;
                break;
            }
        }
        if (!used) {
            break;
        }
    }
    snprintf(out, 20, "%c/%03d", role_letter(role), n);
    xSemaphoreGive(s_lock);
}

bool registry_reg_no_valid(role_t role, const char *reg_no)
{
    return reg_shape_ok(role, reg_no);
}

bool registry_card_valid(role_t role, const char *label)
{
    return card_shape_n(role, label) != 0;
}

void registry_card_progress(role_t role, char last_out[8], char next_out[8])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    int highest = 0;
    for (int i = 0; i < s_card_count; i++) {
        if (s_cards[i].type != CARD_NORMAL) {
            continue;
        }
        int n = card_shape_n(role, s_cards[i].label);
        if (n > highest) {
            highest = n;
        }
    }
    if (highest > 0) {
        make_label(last_out, role, highest);
    } else {
        last_out[0] = '\0';
    }
    next_normal_label(role, next_out);

    xSemaphoreGive(s_lock);
}

bool registry_card_owner_text(const char *label, char *out, size_t cap)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    int i = find_card_by_label(label);
    if (i >= 0) {
        found = true;
        if (s_cards[i].owner >= 0) {
            const person_rec_t *p = &s_people[s_cards[i].owner];
            snprintf(out, cap, "%s (%s)", p->name, p->reg_no);
        } else {
            snprintf(out, cap, "nobody");
        }
    }

    xSemaphoreGive(s_lock);
    return found;
}

/* ---------- Forgot the card: reserve ---------- */
cardop_t registry_issue_reserve_with(const char *normal_label, const char *reserve_label,
                                     int minutes, char reserve_label_out[8])
{
    cardop_t res = CARDOP_OK;
    int      r   = -1;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    int o = find_card_by_label(normal_label);

    if (o < 0) {
        res = CARDOP_NO_SUCH_CARD;
    } else if (s_cards[o].type != CARD_NORMAL) {
        res = CARDOP_WRONG_TYPE;
    } else if (s_cards[o].status == CARD_SUSPENDED) {
        res = CARDOP_ON_LOAN;
    } else if (s_cards[o].status != CARD_ACTIVE) {
        res = CARDOP_NOT_ACTIVE;
    } else if (s_cards[o].inside) {
        res = CARDOP_PERSON_INSIDE;             /* misuse */
    } else if (!wallclock_is_set()) {
        res = CARDOP_NO_CLOCK;
    } else if (reserve_label != NULL && reserve_label[0] != '\0') {
        /* the guard chose which reserve card */
        r = find_card_by_label(reserve_label);
        if (r < 0) {
            res = CARDOP_NO_SUCH_CARD;
        } else if (s_cards[r].type != CARD_RESERVE) {
            res = CARDOP_WRONG_TYPE;
        } else if (s_cards[r].status != CARD_AVAILABLE) {
            res = CARDOP_RESERVE_BUSY;
        }
    } else {
        /* the next free reserve card */
        for (int i = 0; i < s_card_count; i++) {
            if (s_cards[i].type == CARD_RESERVE && s_cards[i].status == CARD_AVAILABLE) {
                r = i;
                break;
            }
        }
        if (r < 0) {
            res = CARDOP_NO_RESERVE_FREE;
        }
    }

    if (res == CARDOP_OK) {
        s_cards[o].status = CARD_SUSPENDED;     /* the old card stops at once */

        card_rec_t *c = &s_cards[r];
        c->status      = CARD_ACTIVE;
        c->owner       = s_cards[o].owner;
        c->lent_for    = o;
        c->valid_until = time(NULL) + (time_t)minutes * 60;
        c->inside      = false;
        snprintf(reserve_label_out, 8, "%s", c->label);
        s_dirty = true;

        ESP_LOGW(TAG, "reserve %s issued for %s for %d min; %s suspended",
                 c->label, normal_label, minutes, s_cards[o].label);
    }

    xSemaphoreGive(s_lock);
    return res;
}

cardop_t registry_issue_reserve(const char *normal_label, int minutes,
                                char reserve_label_out[8])
{
    return registry_issue_reserve_with(normal_label, NULL, minutes, reserve_label_out);
}

void registry_reserve_progress(int *free_count, int *total, char next_out[8])
{
    int f = 0;
    int t = 0;
    next_out[0] = '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_card_count; i++) {
        if (s_cards[i].type != CARD_RESERVE) {
            continue;
        }
        t++;
        if (s_cards[i].status == CARD_AVAILABLE) {
            if (f == 0) {
                snprintf(next_out, 8, "%s", s_cards[i].label);
            }
            f++;
        }
    }
    xSemaphoreGive(s_lock);

    *free_count = f;
    *total      = t;
}

cardop_t registry_return_reserve(const char *reserve_label, char normal_label_out[8])
{
    cardop_t res = CARDOP_OK;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    int r = find_card_by_label(reserve_label);

    if (r < 0) {
        res = CARDOP_NO_SUCH_CARD;
    } else if (s_cards[r].type != CARD_RESERVE) {
        res = CARDOP_WRONG_TYPE;
    } else if (s_cards[r].status != CARD_ACTIVE || s_cards[r].lent_for < 0) {
        res = CARDOP_NOT_ON_LOAN;
    } else if (s_cards[r].inside) {
        res = CARDOP_HOLDER_INSIDE;
    } else {
        int o = s_cards[r].lent_for;
        if (s_cards[o].status == CARD_SUSPENDED) {
            s_cards[o].status = CARD_ACTIVE;    /* the own card works again */
        }
        snprintf(normal_label_out, 8, "%s", s_cards[o].label);

        s_cards[r].status      = CARD_AVAILABLE;
        s_cards[r].owner       = -1;
        s_cards[r].lent_for    = -1;
        s_cards[r].valid_until = 0;
        s_dirty = true;

        ESP_LOGW(TAG, "reserve %s returned; %s works again",
                 s_cards[r].label, s_cards[o].label);
    }

    xSemaphoreGive(s_lock);
    return res;
}

/* ---------- Guest cards: activate a range for some minutes ---------- */

/* Look at the range first..last (the lock must be held). busy may be NULL.
 * Returns CARDOP_OK, CARDOP_GUEST_BUSY, CARDOP_BAD_RANGE or CARDOP_NO_SUCH_CARD. */
static cardop_t guest_range_locked(const char *first, const char *last,
                                   int *lo_out, int *hi_out,
                                   char *busy, size_t busy_cap, int *busy_count)
{
    *busy_count = 0;
    if (busy != NULL && busy_cap > 0) {
        busy[0] = '\0';
    }

    int lo = guest_n(first);
    int hi = guest_n(last);
    if (lo == 0 || hi == 0) {
        return CARDOP_NO_SUCH_CARD;
    }
    if (lo > hi) {
        return CARDOP_BAD_RANGE;
    }

    size_t used = 0;
    for (int n = lo; n <= hi; n++) {
        char label[8];
        guest_label(label, n);

        int i = find_card_by_label(label);
        if (i < 0) {
            return CARDOP_NO_SUCH_CARD;
        }
        if (s_cards[i].type != CARD_GUEST) {
            return CARDOP_WRONG_TYPE;
        }
        if (!guest_is_free(&s_cards[i])) {
            (*busy_count)++;
            if (busy != NULL && used + 8 < busy_cap) {
                int w = snprintf(busy + used, busy_cap - used, "%s%s",
                                 (used > 0) ? ", " : "", label);
                if (w > 0) {
                    used += (size_t)w;
                }
            }
        }
    }

    *lo_out = lo;
    *hi_out = hi;
    return (*busy_count > 0) ? CARDOP_GUEST_BUSY : CARDOP_OK;
}

void registry_guest_progress(int *free_count, int *total, char first_free[8])
{
    int f = 0;
    int t = 0;
    first_free[0] = '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_card_count; i++) {
        if (s_cards[i].type != CARD_GUEST) {
            continue;
        }
        t++;
        if (guest_is_free(&s_cards[i])) {
            if (f == 0) {
                snprintf(first_free, 8, "%s", s_cards[i].label);
            }
            f++;
        }
    }
    xSemaphoreGive(s_lock);

    *free_count = f;
    *total      = t;
}

cardop_t registry_guest_range_check(const char *first, const char *last,
                                    int *count_out, char busy_out[], size_t busy_cap,
                                    int *busy_count_out)
{
    int lo = 0, hi = 0, bc = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    cardop_t res = guest_range_locked(first, last, &lo, &hi, busy_out, busy_cap, &bc);
    xSemaphoreGive(s_lock);

    *count_out      = (res == CARDOP_OK || res == CARDOP_GUEST_BUSY) ? (hi - lo + 1) : 0;
    *busy_count_out = bc;
    return res;
}

cardop_t registry_activate_guests(const char *first, const char *last, int minutes,
                                  int *count_out)
{
    int      lo = 0, hi = 0, bc = 0;
    cardop_t res;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!wallclock_is_set()) {
        res = CARDOP_NO_CLOCK;
    } else {
        res = guest_range_locked(first, last, &lo, &hi, NULL, 0, &bc);
    }

    if (res == CARDOP_OK) {
        time_t until = time(NULL) + (time_t)minutes * 60;

        for (int n = lo; n <= hi; n++) {
            char label[8];
            guest_label(label, n);
            int i = find_card_by_label(label);
            if (i >= 0) {
                s_cards[i].status      = CARD_ACTIVE;
                s_cards[i].valid_until = until;
                s_cards[i].owner       = -1;
                s_cards[i].lent_for    = -1;
                s_cards[i].inside      = false;
            }
        }
        s_dirty = true;
        ESP_LOGW(TAG, "guest cards G1.%d to G1.%d activated for %d min", lo, hi, minutes);
    }

    xSemaphoreGive(s_lock);

    *count_out = (res == CARDOP_OK) ? (hi - lo + 1) : 0;
    return res;
}

/* ---------- Lost card ---------- */
cardop_t registry_report_lost(const char *normal_label, char new_label_out[8])
{
    cardop_t res = CARDOP_OK;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    int o = find_card_by_label(normal_label);

    if (o < 0) {
        res = CARDOP_NO_SUCH_CARD;
    } else if (s_cards[o].type != CARD_NORMAL) {
        res = CARDOP_WRONG_TYPE;
    } else if (s_cards[o].status == CARD_BLOCKED) {
        res = CARDOP_ALREADY_BLOCKED;
    } else if (s_cards[o].status == CARD_SUSPENDED) {
        res = CARDOP_ON_LOAN;                   /* return the reserve card first */
    } else if (s_cards[o].status != CARD_ACTIVE) {
        res = CARDOP_NOT_ACTIVE;
    } else if (s_card_count >= MAX_CARDS) {
        res = CARDOP_FULL;
    }

    if (res == CARDOP_OK) {
        /* the new card keeps the owner's letter (a staff member gets another S card) */
        role_t role = (s_cards[o].owner >= 0) ? s_people[s_cards[o].owner].role
                                              : ROLE_STUDENT;
        char label[8];
        int  n = next_normal_label(role, label);
        uint8_t uid[4];
        make_uid(role, n, uid);

        int nw = add_card(uid, label, CARD_NORMAL, CARD_ACTIVE, s_cards[o].owner);

        /* if the person is still inside, the new card carries that on,
         * so they can still leave */
        s_cards[nw].inside = s_cards[o].inside;
        s_cards[o].inside  = false;
        s_cards[o].status  = CARD_BLOCKED;      /* forever */
        s_dirty = true;

        snprintf(new_label_out, 8, "%s", label);
        ESP_LOGW(TAG, "card %s reported lost: BLOCKED. New card %s issued",
                 s_cards[o].label, label);
    }

    xSemaphoreGive(s_lock);
    return res;
}

/* ---------- Decisions ---------- */
access_result_t registry_check_entry(const uint8_t uid[4], access_info_t *out)
{
    access_result_t res;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    int i = find_card_by_uid(uid);
    if (i < 0) {
        res = ACCESS_UNKNOWN_CARD;
        if (out) { memset(out, 0, sizeof(*out)); }
    } else {
        const card_rec_t *c = &s_cards[i];
        if (out) { fill_info(c, out); }

        if (c->status == CARD_BLOCKED) {
            res = ACCESS_BLOCKED;
        } else if (c->status == CARD_SUSPENDED) {
            res = ACCESS_SUSPENDED;
        } else if (c->status == CARD_AVAILABLE) {
            res = ACCESS_NOT_ISSUED;
        } else if (is_expired(c)) {
            res = ACCESS_EXPIRED;
        } else if (c->inside) {
            res = ACCESS_ALREADY_INSIDE;
        } else {
            res = ACCESS_GRANTED;
        }
    }

    xSemaphoreGive(s_lock);
    return res;
}

access_result_t registry_check_exit(const uint8_t uid[4], access_info_t *out)
{
    access_result_t res;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    int i = find_card_by_uid(uid);
    if (i < 0) {
        res = ACCESS_UNKNOWN_CARD;
        if (out) { memset(out, 0, sizeof(*out)); }
    } else {
        if (out) { fill_info(&s_cards[i], out); }
        /* Anyone who is inside may leave, even if blocked, suspended or expired. */
        res = s_cards[i].inside ? ACCESS_GRANTED : ACCESS_NOT_INSIDE;
    }

    xSemaphoreGive(s_lock);
    return res;
}

void registry_set_inside(const uint8_t uid[4], bool inside)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_card_by_uid(uid);
    if (i >= 0 && s_cards[i].inside != inside) {
        s_cards[i].inside = inside;
        s_dirty = true;
    }
    xSemaphoreGive(s_lock);
}

/* ---------- Reports ---------- */
int registry_count_inside(void)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_card_count; i++) {
        if (s_cards[i].inside) {
            n++;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

int registry_list_inside(access_info_t *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_card_count && n < max; i++) {
        if (s_cards[i].inside) {
            fill_info(&s_cards[i], &out[n++]);
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

int registry_list_cards(access_info_t *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_card_count && n < max; i++) {
        fill_info(&s_cards[i], &out[n++]);
    }
    xSemaphoreGive(s_lock);
    return n;
}

bool registry_uid_by_label(const char *label, uint8_t uid[4])
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_card_by_label(label);
    if (i >= 0) {
        memcpy(uid, s_cards[i].uid, 4);
        found = true;
    }
    xSemaphoreGive(s_lock);
    return found;
}

/* ---------- Text ---------- */
const char *access_result_text(access_result_t r)
{
    switch (r) {
    case ACCESS_GRANTED:        return "GRANTED";
    case ACCESS_UNKNOWN_CARD:   return "DENIED: unknown card";
    case ACCESS_NOT_ISSUED:     return "DENIED: card not issued";
    case ACCESS_BLOCKED:        return "DENIED: card blocked";
    case ACCESS_SUSPENDED:      return "DENIED: card suspended";
    case ACCESS_EXPIRED:        return "DENIED: card expired";
    case ACCESS_ALREADY_INSIDE: return "DENIED: already inside";
    case ACCESS_NOT_INSIDE:     return "DENIED: not inside";
    }
    return "DENIED";
}

const char *cardop_text(cardop_t r)
{
    switch (r) {
    case CARDOP_OK:              return "Done.";
    case CARDOP_NO_SUCH_CARD:    return "There is no card with that ID.";
    case CARDOP_WRONG_TYPE:      return "That is the wrong type of card for this action.";
    case CARDOP_NOT_ACTIVE:      return "That card is not active.";
    case CARDOP_PERSON_INSIDE:   return "MISUSE ALERT: this person shows as INSIDE the building. A reserve card was refused.";
    case CARDOP_NO_RESERVE_FREE: return "No reserve card is free right now.";
    case CARDOP_NO_CLOCK:        return "The clock is not set. Set it first (Configuration settings).";
    case CARDOP_ALREADY_BLOCKED: return "That card is already blocked.";
    case CARDOP_ON_LOAN:         return "A reserve card is out for this card. Return the reserve card first.";
    case CARDOP_HOLDER_INSIDE:   return "The holder is still inside. They must leave first.";
    case CARDOP_NOT_ON_LOAN:     return "That reserve card is not lent to anyone.";
    case CARDOP_FULL:            return "The registry is full.";
    case CARDOP_RESERVE_BUSY:    return "That reserve card is already out. Choose a free one.";
    case CARDOP_GUEST_BUSY:      return "Some guest cards in that range are not free.";
    case CARDOP_BAD_RANGE:       return "The last card must not come before the first card.";
    }
    return "Failed.";
}

const char *registry_role_text(role_t r)
{
    switch (r) {
    case ROLE_STUDENT: return "Student";
    case ROLE_STAFF:   return "Staff";
    case ROLE_CLEANER: return "Cleaner";
    case ROLE_OTHER:   return "Other";
    }
    return "?";
}

const char *registry_card_type_text(card_type_t t)
{
    switch (t) {
    case CARD_NORMAL:  return "normal";
    case CARD_RESERVE: return "reserve";
    case CARD_GUEST:   return "guest";
    }
    return "?";
}

const char *registry_card_status_text(card_status_t s)
{
    switch (s) {
    case CARD_AVAILABLE: return "available";
    case CARD_ACTIVE:    return "active";
    case CARD_SUSPENDED: return "suspended";
    case CARD_BLOCKED:   return "blocked";
    }
    return "?";
}