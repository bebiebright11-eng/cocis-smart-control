#ifndef REGISTRY_H
#define REGISTRY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

typedef enum {
    ROLE_STUDENT,       /* card A1.x, number A/001 */
    ROLE_STAFF,         /* card S1.x, number S/001 */
    ROLE_CLEANER,       /* card C1.x, number C/001 */
    ROLE_OTHER,         /* card O1.x, number O/001 */
} role_t;

typedef enum {
    CARD_NORMAL,        /* belongs to one person for good        */
    CARD_RESERVE,       /* lent to someone who forgot their card (B1.x) */
    CARD_GUEST,         /* anonymous, for a meeting or event (G1.x)     */
} card_type_t;

typedef enum {
    CARD_AVAILABLE,     /* in the guard's drawer, not issued yet */
    CARD_ACTIVE,
    CARD_SUSPENDED,
    CARD_BLOCKED,
} card_status_t;

/* Everything the rest of the system needs to know about a card and its holder. */
typedef struct {
    char          card_label[8];   /* the simple card ID, e.g. "A1.1" */
    card_type_t   card_type;
    card_status_t card_status;
    role_t        role;
    char          name[32];
    char          reg_no[20];
    uint8_t       uid[4];
    bool          inside;          /* is this card inside the building now? */
    time_t        valid_until;     /* 0 = never expires */
} access_info_t;

typedef enum {
    ACCESS_GRANTED,
    ACCESS_UNKNOWN_CARD,
    ACCESS_NOT_ISSUED,           /* a reserve or guest card nobody holds yet */
    ACCESS_BLOCKED,
    ACCESS_SUSPENDED,
    ACCESS_EXPIRED,              /* temporary card past its time */
    ACCESS_ALREADY_INSIDE,       /* tried to enter twice */
    ACCESS_NOT_INSIDE,           /* tried to leave without being inside */
} access_result_t;

/* Results of registry_add_normal and registry_add_person */
#define REG_ADD_OK          0
#define REG_ADD_FULL       -1    /* no room for more people or cards */
#define REG_ADD_DUPLICATE  -2    /* that registration number already exists */
#define REG_ADD_BAD_REG    -3    /* registration number has the wrong shape or letter */
#define REG_ADD_BAD_CARD   -4    /* card ID has the wrong shape or letter */
#define REG_ADD_CARD_TAKEN -5    /* that card ID already exists */

/* Results of the card operations (reserve, return, lost, guests) */
typedef enum {
    CARDOP_OK,
    CARDOP_NO_SUCH_CARD,
    CARDOP_WRONG_TYPE,
    CARDOP_NOT_ACTIVE,
    CARDOP_PERSON_INSIDE,       /* misuse: the person shows as inside */
    CARDOP_NO_RESERVE_FREE,
    CARDOP_NO_CLOCK,
    CARDOP_ALREADY_BLOCKED,
    CARDOP_ON_LOAN,             /* a reserve card is out for this card */
    CARDOP_HOLDER_INSIDE,       /* reserve holder has not left yet */
    CARDOP_NOT_ON_LOAN,
    CARDOP_FULL,
    CARDOP_RESERVE_BUSY,        /* the chosen reserve card is already out */
    CARDOP_GUEST_BUSY,          /* some guest cards in the range are not free */
    CARDOP_BAD_RANGE,           /* the last guest card comes before the first */
} cardop_t;

/* Start the registry (creates the mutex; it starts with no people, only
 * the reserve cards B1.x and the guest cards G1.x). */
void registry_init(void);

/* Replace the starting data with the data saved on the SD card.
 * Returns false (and changes nothing) if there is no usable saved file. */
bool registry_load_from_sd(void);

/* Write the registry to the SD card if anything changed since the last save. */
bool registry_save_if_dirty(void);

/* Add a new person with the next free card of their role
 * (A1.n student, S1.n staff, C1.n cleaner, O1.n other).
 * label_out receives the new card's ID. Returns one of the REG_ADD_ codes.
 * (Older, simple version: it does not check the registration number's shape.) */
int registry_add_normal(const char *reg_no, const char *name, role_t role,
                        char label_out[8]);

/* The new way to add a person: the registration number must look like
 * A/001 (the role's letter, a slash, 3 to 6 digits), and card is the card the
 * guard chose (for example "S1.7"). If card is NULL or empty, the next free
 * card of the role is used. Returns one of the REG_ADD_ codes. */
int registry_add_person(const char *reg_no, const char *name, role_t role,
                        const char *card, char label_out[8]);

/* The lowest registration number of this role that nobody has yet, e.g. "S/003". */
void registry_suggest_reg_no(role_t role, char out[20]);

/* Is this a well-formed registration number for the role (not checking if it is free)? */
bool registry_reg_no_valid(role_t role, const char *reg_no);

/* Is this a well-formed card ID for the role, e.g. "S1.7" (not checking if it is free)? */
bool registry_card_valid(role_t role, const char *label);

/* How far this role's cards have been given out.
 * last_out = the highest card given so far ("" if none yet), next_out = the lowest free card. */
void registry_card_progress(role_t role, char last_out[8], char next_out[8]);

/* Who has this card? Writes "Name (reg no)" or "nobody". Returns false if there is no such card. */
bool registry_card_owner_text(const char *label, char *out, size_t cap);

/* Forgot the card: lend the next free reserve card for 'minutes' and suspend
 * the person's own card. Refused if the person shows as inside. */
cardop_t registry_issue_reserve(const char *normal_label, int minutes,
                                char reserve_label_out[8]);

/* The same, but the guard chooses which reserve card (reserve_label, e.g. "B1.3").
 * If reserve_label is NULL or empty, the next free reserve card is used. */
cardop_t registry_issue_reserve_with(const char *normal_label, const char *reserve_label,
                                     int minutes, char reserve_label_out[8]);

/* How many reserve cards are free, how many there are, and the next free one
 * ("" if none). */
void registry_reserve_progress(int *free_count, int *total, char next_out[8]);

/* Reserve card handed back: it becomes available, the own card works again. */
cardop_t registry_return_reserve(const char *reserve_label,
                                 char normal_label_out[8]);

/* How many guest cards are free to activate, how many there are, and the first
 * free one ("" if none). A guest card is free when it was never activated, or
 * when its time is over and its holder is not inside. */
void registry_guest_progress(int *free_count, int *total, char first_free[8]);

/* Look at a range of guest cards (first to last, e.g. "G1.1" to "G1.10") without
 * changing anything. Returns CARDOP_OK if all are free, CARDOP_GUEST_BUSY if not
 * (busy_out then lists the busy cards), CARDOP_BAD_RANGE or CARDOP_NO_SUCH_CARD.
 * count_out = how many cards are in the range. */
cardop_t registry_guest_range_check(const char *first, const char *last,
                                    int *count_out, char busy_out[], size_t busy_cap,
                                    int *busy_count_out);

/* Activate a range of guest cards for 'minutes'. They expire by themselves.
 * Same results as registry_guest_range_check, plus CARDOP_NO_CLOCK. */
cardop_t registry_activate_guests(const char *first, const char *last, int minutes,
                                  int *count_out);

/* Lost card: block it forever and issue a new card (same letter) to the same person. */
cardop_t registry_report_lost(const char *normal_label, char new_label_out[8]);

/* Decide only. They do NOT change anything. *out describes the card holder. */
access_result_t registry_check_entry(const uint8_t uid[4], access_info_t *out);
access_result_t registry_check_exit(const uint8_t uid[4], access_info_t *out);

/* Change the inside flag (called once the person has really passed). */
void registry_set_inside(const uint8_t uid[4], bool inside);

/* How many cards are inside the building now. */
int registry_count_inside(void);

/* Copy up to max of the cards that are inside into out. Returns how many. */
int registry_list_inside(access_info_t *out, int max);

/* Copy up to max of ALL cards into out. Returns how many. */
int registry_list_cards(access_info_t *out, int max);

/* Find a card's ID from its label such as "A1.2" (capitals do not matter).
 * Used by the virtual tap. */
bool registry_uid_by_label(const char *label, uint8_t uid[4]);

/* Readable text, for logs and screens. */
const char *access_result_text(access_result_t r);
const char *cardop_text(cardop_t r);
const char *registry_role_text(role_t r);
const char *registry_card_type_text(card_type_t t);
const char *registry_card_status_text(card_status_t s);

#endif