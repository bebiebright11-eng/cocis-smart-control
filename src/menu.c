#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "menu.h"
#include "lane.h"
#include "registry.h"
#include "room_task.h"
#include "wallclock.h"
#include "eventlog.h"
#include "schedule.h"
#include "alerts.h"

static const char *TAG = "menu";

/* ---------- Settings ---------- */
#define LINE_MAX_LEN        64
#define USER_MAX_LEN        16
#define PAGE_WIDTH          42
#define MAX_LIST            100       /* rows on the people and card pages */
#define EVENTS_SHOWN        12        /* rows on the event, alarm and blocked pages */
#define MAX_STAYS           50        /* rows on the occupancy history page */
#define SESSION_TIMEOUT_MS  180000    /* logged out after 3 minutes of silence */
#define MAX_LOGIN_FAILS     3
#define LOCKOUT_MS          30000     /* login locked for 30 s after 3 failures */

#define LEVEL_NONE   0
#define LEVEL_GUARD  1
#define LEVEL_ADMIN  2

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---------- Accounts (temporary: later they come from the SD card) ---------- */
typedef struct {
    const char *user;
    const char *pin;
    int         level;
} account_t;

static const account_t s_accounts[] = {
    { "guard", "1234", LEVEL_GUARD },
    { "admin", "9999", LEVEL_ADMIN },
};

/* ---------- What the screen is doing ---------- */
typedef enum {
    ST_USER,        /* asking for the username                  */
    ST_PIN,         /* asking for the password                  */
    ST_MENU,        /* a menu is shown, waiting for a number    */
    ST_PAUSE,       /* an information page, waiting for Enter   */
    ST_ASK_ROOM,    /* correcting a headcount: which room       */
    ST_ASK_COUNT,   /* correcting a headcount: how many         */
    ST_ASK_DATE,    /* setting the clock: the date              */
    ST_ASK_TIME,    /* setting the clock: the time              */
    ST_ASK_TAP,     /* virtual tap: which card ID               */
    ST_ASK_ROLE,    /* new person: role                         */
    ST_ASK_REG,     /* new person: registration number          */
    ST_ASK_NAME,    /* new person: full name                    */
    ST_ASK_CARD,    /* new person: which card                   */
    ST_ASK_SAVE,    /* new person: save or cancel               */
    ST_ASK_RES_CARD,   /* reserve: whose card was forgotten     */
    ST_ASK_RES_PICK,   /* reserve: which reserve card           */
    ST_ASK_RES_MIN,    /* reserve: how many minutes             */
    ST_ASK_RES_SAVE,   /* reserve: save or cancel               */
    ST_ASK_RETURN,     /* return: which reserve card            */
    ST_ASK_GST_FROM,   /* guest cards: first card of the range  */
    ST_ASK_GST_TO,     /* guest cards: last card of the range   */
    ST_ASK_GST_MIN,    /* guest cards: how many minutes         */
    ST_ASK_GST_NAME,   /* guest cards: meeting name (optional)  */
    ST_ASK_GST_SAVE,   /* guest cards: save or cancel           */
    ST_ASK_LOST_CARD,  /* lost: which card                      */
    ST_ASK_LOST_YES,   /* lost: confirm with YES                */
    ST_ASK_HDATE,      /* occupancy history: the date           */
    ST_ASK_ADATE,      /* alarm history: the date (or Enter)    */
    ST_ASK_OPEN,       /* schedule: opening time                */
    ST_ASK_CLOSE,      /* schedule: closing time                */
    ST_ASK_WINDOW,     /* activate entry/exit: minutes          */
    ST_ASK_ALARM,      /* closing alarm: minutes before closing */
} state_t;

static state_t s_state = ST_USER;

/* ---------- Session ---------- */
static int         s_level        = LEVEL_NONE;
static const char *s_user         = "";
static char        s_pending_user[USER_MAX_LEN];
static uint32_t    s_last_active  = 0;
static int         s_fail_count   = 0;
static uint32_t    s_locked_until = 0;

/* values collected by the question screens */
static int  s_w_room = 0;
static int  s_w_year = 0, s_w_month = 0, s_w_day = 0;
static int  s_w_open_min = 0;
static bool s_w_tap_entry = true;
static bool s_w_win_entry = false;
static int  s_w_role = 0;          /* index into s_roles */
static char s_w_sugg[20];          /* the suggested registration number */
static char s_w_next[8];           /* the next free card of the role */
static char s_w_reg[20];
static char s_w_name[32];
static char s_w_label[8];
static char s_w_res[8];            /* reserve card chosen */
static int  s_w_minutes = 0;       /* reserve or guest cards: minutes */
static char s_w_gfrom[8];          /* guest range: first card */
static char s_w_gto[8];            /* guest range: last card */
static int  s_w_gcount = 0;        /* guest range: how many cards */
static char s_w_event[32];         /* guest range: meeting name (may be empty) */

/* shared lists for the pages (only this task uses them) */
static access_info_t s_list[MAX_LIST];
static event_line_t  s_events[EVENTS_SHOWN];
static stay_t        s_stays[MAX_STAYS];

static const role_t s_roles[] = { ROLE_STUDENT, ROLE_STAFF, ROLE_CLEANER, ROLE_OTHER };

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static const char *level_name(int level)
{
    return (level >= LEVEL_ADMIN) ? "admin" : "guard";
}

static bool parse_number(const char *s, int *out)
{
    if (*s == '\0') {
        return false;
    }
    char *end;
    long v = strtol(s, &end, 10);
    if (*end != '\0') {
        return false;
    }
    *out = (int)v;
    return true;
}

/* "07:00" or "7:00" -> minutes since midnight (420). false if not understood. */
static bool parse_hhmm(const char *s, int *out_min)
{
    int h, m;
    if (sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
        return false;
    }
    *out_min = h * 60 + m;
    return true;
}

/* Look up a card's details (owner, number...) by its ID, e.g. "A1.2". */
static bool card_info(const char *label, access_info_t *out)
{
    int n = registry_list_cards(s_list, MAX_LIST);
    for (int i = 0; i < n; i++) {
        if (strcasecmp(s_list[i].card_label, label) == 0) {
            *out = s_list[i];
            return true;
        }
    }
    return false;
}

/* Does a person with this registration number already exist? who gets the name. */
static bool reg_exists(const char *reg, char *who, size_t cap)
{
    int n = registry_list_cards(s_list, MAX_LIST);
    for (int i = 0; i < n; i++) {
        if (s_list[i].reg_no[0] != '\0' && strcasecmp(s_list[i].reg_no, reg) == 0) {
            snprintf(who, cap, "%s", s_list[i].name);
            return true;
        }
    }
    return false;
}

/* Find a person's normal card from a card ID (A1.2) or a registration number (A/002). */
static bool find_normal_card(const char *key, access_info_t *out)
{
    int n = registry_list_cards(s_list, MAX_LIST);

    /* first as a card ID */
    for (int i = 0; i < n; i++) {
        if (s_list[i].card_type == CARD_NORMAL &&
            strcasecmp(s_list[i].card_label, key) == 0) {
            *out = s_list[i];
            return true;
        }
    }
    /* then as a registration number: the person's current card, not a blocked one */
    for (int i = 0; i < n; i++) {
        if (s_list[i].card_type == CARD_NORMAL &&
            s_list[i].card_status != CARD_BLOCKED &&
            strcasecmp(s_list[i].reg_no, key) == 0) {
            *out = s_list[i];
            return true;
        }
    }
    return false;
}

/* The exact ID of a guest card from what the guard typed (g1.5 becomes G1.5). */
static bool guest_card_from_text(const char *line, char out[8])
{
    access_info_t c;
    if (!card_info(line, &c) || c.card_type != CARD_GUEST) {
        return false;
    }
    snprintf(out, 8, "%s", c.card_label);
    return true;
}

/* Minutes left until closing time today (0 or less if closed, -1 if the clock is not set). */
static int minutes_until_closing(void)
{
    if (!wallclock_is_set()) {
        return -1;
    }
    int o, c;
    schedule_get(&o, &c);

    time_t    t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    return c - (tmv.tm_hour * 60 + tmv.tm_min);
}

/* "HH:MM" of a moment in time. */
static void format_clock(time_t t, char *out, size_t cap)
{
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, cap, "%H:%M", &tmv);
}

/* The question "how many minutes?" used by reserve and guest cards. */
static void print_minutes_question(const char *what)
{
    int left = minutes_until_closing();
    if (left > 1440) {
        left = 1440;
    }
    printf("How many minutes %s (1 to 1440)?\n", what);
    if (left > 0) {
        printf("Press Enter for 'until closing' (%d min), or type the minutes (0 = cancel): ", left);
    } else {
        printf("Type the minutes (0 = cancel): ");
    }
    fflush(stdout);
}

/* Understand the answer. Returns 0 = minutes understood, 1 = cancelled,
 * 2 = ask again (a message has been printed). */
static int read_minutes(const char *line, int *minutes_out)
{
    int minutes;
    int left = minutes_until_closing();
    if (left > 1440) {
        left = 1440;
    }

    if (strcmp(line, "0") == 0) {
        return 1;
    }
    if (line[0] == '\0') {
        if (left <= 0) {
            printf("Closing time has passed, so there is no 'until closing'.\n");
            printf("Please type the minutes.\n");
            return 2;
        }
        minutes = left;
    } else if (!parse_number(line, &minutes) || minutes < 1 || minutes > 1440) {
        printf("Please type a number from 1 to 1440.\n");
        return 2;
    }
    *minutes_out = minutes;
    return 0;
}

/* Write an action to the event log, with the guard's name. tag is a short word
 * shown in the card column (for example SCH, RES, GST or ALR). */
static void log_action_event(const char *tag, const char *note)
{
    access_info_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.card_label, sizeof(ev.card_label), "%s", tag);
    snprintf(ev.name, sizeof(ev.name), "%s", s_user);
    snprintf(ev.reg_no, sizeof(ev.reg_no), "-");
    eventlog_add(EV_CORRECT, &ev, note);
}

/* Write a schedule-related action to the event log, with the guard's name. */
static void log_sched_event(const char *note)
{
    log_action_event("SCH", note);
}

/* ---------- Drawing ---------- */
static void rule(void)
{
    for (int i = 0; i < PAGE_WIDTH; i++) {
        putchar('-');
    }
    putchar('\n');
}

static void center(const char *text)
{
    int len = (int)strlen(text);
    int pad = (PAGE_WIDTH - len) / 2;
    if (pad < 0) {
        pad = 0;
    }
    printf("%*s%s\n", pad, "", text);
}

/* The heading every page shows. */
static void header(const char *title)
{
    printf("\n");
    rule();
    center("CONTROL SYSTEM");
    center("Smart Control and Monitoring System");
    center(title);
    if (s_level > LEVEL_NONE) {
        printf("  Logged in as: %s (%s)\n", s_user, level_name(s_level));
    }
    rule();
}

/* The end of an information page. */
static void page_end(void)
{
    rule();
    printf("Press Enter to go back. ");
    fflush(stdout);
    s_state = ST_PAUSE;
}

/* Banners for the rooms alert, the closing alarm and the guard's open windows. */
static void print_banners(void)
{
    bool any = false;

    alerts_status_t al;
    alerts_status(&al);
    if (al.kind != ALERT_NONE) {
        printf("*** ALERT: %s%s ***\n", alerts_short_text(al.kind),
               al.acknowledged ? " (silenced)" : "");
        any = true;
    }

    int closing = schedule_closing_in();
    if (closing >= 0) {
        printf("*** CLOSING IN %d MIN ***\n", closing);
        any = true;
    }
    int ex = schedule_window_left_sec(false);
    if (ex > 0) {
        printf("*** EXIT OPEN for %d more min ***\n", (ex + 59) / 60);
        any = true;
    }
    int en = schedule_window_left_sec(true);
    if (en > 0) {
        printf("*** ENTRY OPEN for %d more min ***\n", (en + 59) / 60);
        any = true;
    }
    if (any) {
        rule();
    }
}

/* ---------- The menus ---------- */
typedef enum {
    SCR_MAIN,
    SCR_OCCUPANCY,
    SCR_ROOMS,
    SCR_ASSIGN,
    SCR_CONFIG,
    SCR_SCHEDULE,
    SCR_SIM,
    SCR_HISTORY,
    SCR_ALERTS,
    SCR_COUNT
} screen_id_t;

typedef enum {
    ACT_OPEN_OCCUPANCY,
    ACT_OPEN_ROOMS,
    ACT_OPEN_ASSIGN,
    ACT_OPEN_CONFIG,
    ACT_OPEN_SCHEDULE,
    ACT_OPEN_SIM,
    ACT_OPEN_HISTORY,
    ACT_OPEN_ALERTS,
    ACT_HELP,
    ACT_OCC_BUILDING,
    ACT_OCC_ROOM1,
    ACT_OCC_ROOM2,
    ACT_OCC_SUMMARY,
    ACT_ROOM_LIGHTS,
    ACT_ROOM_CORRECT,
    ACT_ASSIGN_NORMAL,
    ACT_ASSIGN_RESERVE,
    ACT_ASSIGN_RETURN,
    ACT_ASSIGN_GUEST,
    ACT_ASSIGN_LOST,
    ACT_CFG_CLOCK,
    ACT_SCHED_SET,
    ACT_SCHED_EXIT,
    ACT_SCHED_ENTRY,
    ACT_SCHED_ALARM,
    ACT_SIM_ENTRY,
    ACT_SIM_EXIT,
    ACT_SIM_LIST,
    ACT_HIST_OCC,
    ACT_HIST_BLOCKED,
    ACT_HIST_ALARMS,
    ACT_HIST_EVENTS,
    ACT_ALERT_STATUS,
    ACT_ALERT_ACK,
    ACT_COMING_SOON,
} action_t;

typedef struct {
    const char *label;
    action_t    action;
    int         min_level;      /* who sees this item */
} item_t;

typedef struct {
    const char   *title;
    const item_t *items;
    size_t        count;
    screen_id_t   parent;       /* where "0. Back" goes */
} screen_t;

static const item_t s_main_items[] = {
    { "Occupancy status",               ACT_OPEN_OCCUPANCY, LEVEL_GUARD },
    { "Room control (lights)",          ACT_OPEN_ROOMS,     LEVEL_GUARD },
    { "Assign cards",                   ACT_OPEN_ASSIGN,    LEVEL_GUARD },
    { "Configuration settings",         ACT_OPEN_CONFIG,    LEVEL_GUARD },
    { "Simulation tools (virtual tap)", ACT_OPEN_SIM,       LEVEL_GUARD },
    { "History and reports",            ACT_OPEN_HISTORY,   LEVEL_GUARD },
    { "Alerts",                         ACT_OPEN_ALERTS,    LEVEL_GUARD },
    { "Help",                           ACT_HELP,           LEVEL_GUARD },
};

static const item_t s_occupancy_items[] = {
    { "Building: how many people are inside, and who", ACT_OCC_BUILDING, LEVEL_GUARD },
    { "Room 1: people inside",                         ACT_OCC_ROOM1,    LEVEL_GUARD },
    { "Room 2: people inside",                         ACT_OCC_ROOM2,    LEVEL_GUARD },
    { "Summary of everything",                         ACT_OCC_SUMMARY,  LEVEL_GUARD },
};

static const item_t s_rooms_items[] = {
    { "Light status (Room 1 and Room 2)",  ACT_ROOM_LIGHTS,  LEVEL_GUARD },
    { "Correct a room's headcount",        ACT_ROOM_CORRECT, LEVEL_GUARD },
};

static const item_t s_assign_items[] = {
    { "Normal card (add a student, staff, cleaner...)", ACT_ASSIGN_NORMAL,  LEVEL_GUARD },
    { "Reserve card (forgot the card)",                 ACT_ASSIGN_RESERVE, LEVEL_GUARD },
    { "Return a reserve card",                          ACT_ASSIGN_RETURN,  LEVEL_GUARD },
    { "Guest cards (activate for a meeting)",           ACT_ASSIGN_GUEST,   LEVEL_GUARD },
    { "Lost card (block it forever)",                   ACT_ASSIGN_LOST,    LEVEL_GUARD },
};

static const item_t s_config_items[] = {
    { "Set the clock (date and time)",   ACT_CFG_CLOCK,     LEVEL_GUARD },
    { "Access schedule and alarm",       ACT_OPEN_SCHEDULE, LEVEL_GUARD },
    { "Users (add or remove guards)",    ACT_COMING_SOON,   LEVEL_ADMIN },
};

static const item_t s_schedule_items[] = {
    { "Set opening and closing time",            ACT_SCHED_SET,   LEVEL_GUARD },
    { "Activate EXIT for some minutes",          ACT_SCHED_EXIT,  LEVEL_GUARD },
    { "Activate ENTRY for some minutes",         ACT_SCHED_ENTRY, LEVEL_GUARD },
    { "Closing alarm (minutes before closing)",  ACT_SCHED_ALARM, LEVEL_GUARD },
};

static const item_t s_sim_items[] = {
    { "Tap a card at the ENTRY reader",  ACT_SIM_ENTRY, LEVEL_GUARD },
    { "Tap a card at the EXIT reader",   ACT_SIM_EXIT,  LEVEL_GUARD },
    { "List all cards (and their IDs)",  ACT_SIM_LIST,  LEVEL_GUARD },
};

static const item_t s_history_items[] = {
    { "Occupancy history (who was in, on a date)", ACT_HIST_OCC,     LEVEL_GUARD },
    { "Blocked cards (and their owners)",          ACT_HIST_BLOCKED, LEVEL_GUARD },
    { "Alarm history (which cards caused them)",   ACT_HIST_ALARMS,  LEVEL_GUARD },
    { "Event log (latest events)",                 ACT_HIST_EVENTS,  LEVEL_GUARD },
};

static const item_t s_alerts_items[] = {
    { "Alert status (rooms versus building)",  ACT_ALERT_STATUS, LEVEL_GUARD },
    { "Silence the alarm (acknowledge)",       ACT_ALERT_ACK,    LEVEL_GUARD },
    { "Alert history (all alarms)",            ACT_HIST_ALARMS,  LEVEL_GUARD },
};

static const screen_t s_screens[SCR_COUNT] = {
    [SCR_MAIN]      = { "MAIN MENU",              s_main_items,      ARRAY_LEN(s_main_items),      SCR_MAIN },
    [SCR_OCCUPANCY] = { "OCCUPANCY STATUS",       s_occupancy_items, ARRAY_LEN(s_occupancy_items), SCR_MAIN },
    [SCR_ROOMS]     = { "ROOM CONTROL (LIGHTS)",  s_rooms_items,     ARRAY_LEN(s_rooms_items),     SCR_MAIN },
    [SCR_ASSIGN]    = { "ASSIGN CARDS",           s_assign_items,    ARRAY_LEN(s_assign_items),    SCR_MAIN },
    [SCR_CONFIG]    = { "CONFIGURATION SETTINGS", s_config_items,    ARRAY_LEN(s_config_items),    SCR_MAIN },
    [SCR_SCHEDULE]  = { "ACCESS SCHEDULE",        s_schedule_items,  ARRAY_LEN(s_schedule_items),  SCR_CONFIG },
    [SCR_SIM]       = { "SIMULATION TOOLS",       s_sim_items,       ARRAY_LEN(s_sim_items),       SCR_MAIN },
    [SCR_HISTORY]   = { "HISTORY AND REPORTS",    s_history_items,   ARRAY_LEN(s_history_items),   SCR_MAIN },
    [SCR_ALERTS]    = { "ALERTS",                 s_alerts_items,    ARRAY_LEN(s_alerts_items),    SCR_MAIN },
};

static screen_id_t s_screen = SCR_MAIN;

static void ask_choice(void)
{
    s_state = ST_MENU;
    printf("Choose an option: ");
    fflush(stdout);
}

static void show_screen(void)
{
    const screen_t *s = &s_screens[s_screen];

    header(s->title);
    int n = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (s_level >= s->items[i].min_level) {
            printf("  %d. %s\n", ++n, s->items[i].label);
        }
    }
    printf("  0. %s\n", (s_screen == SCR_MAIN) ? "Log out" : "Back");
    rule();

    /* the current opening hours and alarm, on the schedule menu */
    if (s_screen == SCR_SCHEDULE) {
        int o, c;
        schedule_get(&o, &c);
        int a = schedule_get_alarm();
        printf("Open every day: %02d:%02d to %02d:%02d\n", o / 60, o % 60, c / 60, c % 60);
        if (a > 0) {
            printf("Closing alarm : %d min before closing\n", a);
        } else {
            printf("Closing alarm : off\n");
        }
        rule();
    }

    /* alarm, alert and window banners */
    if (s_screen == SCR_MAIN || s_screen == SCR_SCHEDULE || s_screen == SCR_ALERTS) {
        print_banners();
    }

    /* a reminder while the clock is unknown */
    if (s_screen == SCR_MAIN && !wallclock_is_set()) {
        printf("NOTICE: the clock is not set yet.\n");
        printf("Please set it: option 4, then option 1.\n");
        rule();
    }
    ask_choice();
}

/* The n-th item (1, 2, 3...) that this user can see. */
static const item_t *pick_item(int n)
{
    const screen_t *s = &s_screens[s_screen];
    int k = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (s_level >= s->items[i].min_level) {
            k++;
            if (k == n) {
                return &s->items[i];
            }
        }
    }
    return NULL;
}

/* ---------- Login screens ---------- */
static void ask_user(void)
{
    s_state = ST_USER;
    printf("Username: ");
    fflush(stdout);
}

static void ask_pin(void)
{
    s_state = ST_PIN;
    printf("Password: ");
    fflush(stdout);
}

static void show_login_screen(void)
{
    header("LOGIN");
    printf("Please log in.\n");
    ask_user();
}

static void do_logout(void)
{
    ESP_LOGI(TAG, "logout: %s", s_user);
    s_level = LEVEL_NONE;
    s_user  = "";
    show_login_screen();
}

/* ---------- Information pages ---------- */
static void page_building(void)
{
    header("OCCUPANCY STATUS - BUILDING");

    int total = registry_count_inside();
    printf("People inside the building: %d\n", total);

    if (total > 0) {
        int n = registry_list_inside(s_list, MAX_LIST);
        printf("\n  Card   Name                     Number\n");
        for (int i = 0; i < n; i++) {
            printf("  %-5s  %-23.23s  %s\n",
                   s_list[i].card_label, s_list[i].name, s_list[i].reg_no);
        }
    }
    page_end();
}

static void page_room(int room)
{
    header(room == 1 ? "OCCUPANCY STATUS - ROOM 1" : "OCCUPANCY STATUS - ROOM 2");
    printf("People inside Room %d: %d\n", room, room_headcount_get(room));
    printf("Light               : %s\n", room_light_is_on(room) ? "ON" : "OFF");
    printf("\n(Rooms count people by beam, so names are not known here.)\n");
    page_end();
}

static void page_summary(void)
{
    char when[24];
    wallclock_format(when, sizeof(when));

    header("OCCUPANCY STATUS - SUMMARY");
    printf("Time     : %s\n", when);
    printf("Building : %d people inside\n", registry_count_inside());
    for (int r = 1; r <= 2; r++) {
        printf("Room %d   : %d people, light %s\n",
               r, room_headcount_get(r), room_light_is_on(r) ? "ON" : "OFF");
    }
    page_end();
}

static void page_lights(void)
{
    header("ROOM CONTROL - LIGHT STATUS");
    for (int r = 1; r <= 2; r++) {
        printf("Room %d : light %-3s  (%d people inside)\n",
               r, room_light_is_on(r) ? "ON" : "OFF", room_headcount_get(r));
    }
    page_end();
}

static void page_cards(void)
{
    header("SIMULATION TOOLS - ALL CARDS");

    int    n   = registry_list_cards(s_list, MAX_LIST);
    time_t now = time(NULL);
    printf("  Card   Type     Status     Holder            In   Until\n");
    for (int i = 0; i < n; i++) {
        char until[8] = "-";
        if (s_list[i].valid_until != 0 && s_list[i].card_status == CARD_ACTIVE) {
            if (wallclock_is_set() && s_list[i].valid_until < now) {
                snprintf(until, sizeof(until), "expired");
            } else {
                format_clock(s_list[i].valid_until, until, sizeof(until));
            }
        }
        printf("  %-5s  %-7s  %-9s  %-16.16s  %-3s  %s\n",
               s_list[i].card_label,
               registry_card_type_text(s_list[i].card_type),
               registry_card_status_text(s_list[i].card_status),
               s_list[i].name,
               s_list[i].inside ? "yes" : "-",
               until);
    }
    page_end();
}

/* The latest events from the SD card. */
static void page_events(void)
{
    header("HISTORY - EVENT LOG");

    int first = 0;
    int n = eventlog_recent(s_events, EVENTS_SHOWN, &first);

    if (n < 0) {
        printf("No events are saved yet.\n");
    } else if (n == 0) {
        printf("The event log is empty.\n");
    } else {
        printf("Latest %d event(s), oldest first:\n\n", n);
        for (int i = 0; i < n; i++) {
            printf("%s\n", s_events[(first + i) % EVENTS_SHOWN].text);
        }
    }
    page_end();
}

/* "2026-10-21" or "today" -> "2026-10-21". false if not understood. */
static bool parse_date(const char *line, char out[11])
{
    if (strcasecmp(line, "today") == 0) {
        if (!wallclock_is_set()) {
            return false;
        }
        char when[24];
        wallclock_format(when, sizeof(when));
        snprintf(out, 11, "%.10s", when);
        return true;
    }
    int y, mo, d;
    if (sscanf(line, "%d-%d-%d", &y, &mo, &d) != 3 ||
        y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31) {
        return false;
    }
    snprintf(out, 11, "%04d-%02d-%02d", y, mo, d);
    return true;
}

/* HH:MM:SS on the chosen date, MM-DD HH:MM on another day. */
static void show_stamp(const char *stamp, const char *date, char *buf, size_t len)
{
    if (stamp[0] == '\0') {
        snprintf(buf, len, "still inside");
    } else if (stamp[0] == '?') {
        snprintf(buf, len, "not recorded");
    } else if (strncmp(stamp, date, 10) == 0) {
        snprintf(buf, len, "%.8s", stamp + 11);
    } else {
        snprintf(buf, len, "%.11s", stamp + 5);
    }
}

static void print_rows_newest_first(int n, int first, const char *last_col)
{
    printf("  %-19s %-5s %-12s %s\n", "Time", "Card", "Owner", last_col);
    for (int i = n - 1; i >= 0; i--) {
        printf("  %s\n", s_events[(first + i) % EVENTS_SHOWN].text);
    }
}

/* Who was inside on a date, and when each person came in and left. */
static void page_occ_history(const char *date)
{
    header("HISTORY - OCCUPANCY ON A DATE");
    printf("Date: %s\n", date);

    int dropped = 0;
    int n = eventlog_stays_on(date, s_stays, MAX_STAYS, &dropped);

    if (n < 0) {
        printf("No events are saved yet.\n");
    } else if (n == 0) {
        printf("Nobody was recorded inside on this date.\n");
    } else {
        printf("%d stay(s). A time with MM-DD is on another day.\n\n", n);
        printf("  %-18s %-13s %-12s %s\n", "Name", "Number", "In", "Out");
        for (int i = 0; i < n; i++) {
            char a[16], b[16];
            show_stamp(s_stays[i].in,  date, a, sizeof(a));
            show_stamp(s_stays[i].out, date, b, sizeof(b));
            printf("  %-18.18s %-13.13s %-12s %s\n",
                   s_stays[i].name, s_stays[i].key, a, b);
        }
        if (dropped > 0) {
            printf("\n(%d more not shown)\n", dropped);
        }
    }
    page_end();
}

/* The alarms: all the latest, or only those of one date. */
static void page_alarms(const char *date)       /* NULL = the latest alarms */
{
    header("HISTORY - ALARMS");
    if (date != NULL) {
        printf("Date: %s\n", date);
    } else {
        printf("Latest alarms, newest first\n");
    }

    int first = 0;
    int n = eventlog_query("ALARM", date, s_events, EVENTS_SHOWN, &first);

    if (n < 0) {
        printf("No events are saved yet.\n");
    } else if (n == 0) {
        printf("No alarms on record.\n");
    } else {
        printf("\n");
        print_rows_newest_first(n, first, "Cause");
    }
    page_end();
}

/* Blocked cards: who has them, and when they were blocked. */
static void page_blocked(void)
{
    header("HISTORY - BLOCKED CARDS");

    int n = registry_list_cards(s_list, MAX_LIST);
    int shown = 0;
    printf("Blocked now:\n");
    printf("  %-6s %-20s %s\n", "Card", "Owner", "Number");
    for (int i = 0; i < n; i++) {
        if (s_list[i].card_status == CARD_BLOCKED) {
            printf("  %-6s %-20.20s %s\n",
                   s_list[i].card_label, s_list[i].name, s_list[i].reg_no);
            shown++;
        }
    }
    if (shown == 0) {
        printf("  (none)\n");
    }

    int first = 0;
    int m = eventlog_query("BLOCKED", NULL, s_events, EVENTS_SHOWN, &first);
    printf("\nWhen they were blocked (newest first):\n");
    if (m <= 0) {
        printf("  (nothing recorded)\n");
    } else {
        print_rows_newest_first(m, first, "Who blocked it, new card");
    }
    page_end();
}

/* The alerts page: the numbers, and whether they agree. */
static void page_alert_status(void)
{
    alerts_status_t al;
    alerts_status(&al);

    header("ALERTS - STATUS");
    printf("People inside the building : %d\n", al.building);
    printf("Room 1                     : %d people, light %s\n",
           al.room1, al.light1 ? "ON" : "OFF");
    printf("Room 2                     : %d people, light %s\n",
           al.room2, al.light2 ? "ON" : "OFF");
    printf("Rooms together             : %d people\n", al.room1 + al.room2);
    rule();

    if (al.kind == ALERT_NONE) {
        printf("All fine: the rooms and the building agree.\n");
    } else {
        printf("*** ALERT ***\n");
        printf("%s\n", alerts_long_text(al.kind));
        printf("Go and check where the extra people came from.\n");
        if (al.acknowledged) {
            printf("(The alarm is silenced. The alert clears when the\n");
            printf("numbers agree again.)\n");
        } else {
            printf("(Use 'Silence the alarm' to stop the beeps.)\n");
        }
    }
    page_end();
}

static void do_alert_ack(void)
{
    if (alerts_acknowledge()) {
        printf("The alarm is silenced.\n");
        ESP_LOGW(TAG, "%s silenced the alert", s_user);
        log_action_event("ALR", "alert silenced");
    } else {
        printf("There is no active alert.\n");
    }
    show_screen();
}

static void page_help(void)
{
    header("HELP");
    printf("HOW TO MOVE AROUND\n");
    printf("  - Type the number of an option, press Enter.\n");
    printf("  - Type 0 to go back (or to log out).\n");
    printf("  - Information pages wait for Enter.\n");
    printf("  - You are logged out after 3 minutes of no typing.\n");
    printf("\nWHAT EACH OPTION DOES\n");
    printf("  Occupancy status  who is inside now.\n");
    printf("  Room control      are the lights on; fix a headcount.\n");
    printf("  Assign cards      add people, reserve, guest, lost\n");
    printf("                    cards.\n");
    printf("  Configuration     clock, opening hours, closing\n");
    printf("                    alarm, open exit/entry for a time.\n");
    printf("                    Users (guards): admin only.\n");
    printf("  Simulation tools  tap a card by its ID, e.g. A1.2\n");
    printf("  History           who was in on a date, blocked\n");
    printf("                    cards, alarms, the event log.\n");
    printf("  Alerts            warns when the rooms hold more\n");
    printf("                    people than the building, or the\n");
    printf("                    building is empty but a room is\n");
    printf("                    active. Silence the alarm here.\n");
    printf("\nCARD IDS AND NUMBERS\n");
    printf("  Student A1.x (A/001)   Staff S1.x (S/001)\n");
    printf("  Cleaner C1.x (C/001)   Other O1.x (O/001)\n");
    printf("  Reserve B1.x           Guest G1.x\n");
    printf("\nFORGOT THE CARD?\n");
    printf("  Assign cards -> Reserve card. Type the card ID or\n");
    printf("  the registration number. Later: Return a reserve\n");
    printf("  card, and the person's own card works again.\n");
    printf("\nGUEST CARDS\n");
    printf("  Assign cards -> Guest cards. Give the range (G1.1 to\n");
    printf("  G1.10) and the minutes. They expire by themselves.\n");
    printf("\nAFTER CLOSING TIME\n");
    printf("  Nobody can enter or leave. Use Configuration ->\n");
    printf("  Access schedule -> Activate EXIT / ENTRY.\n");
    page_end();
}

static void page_coming_soon(void)
{
    header("COMING SOON");
    printf("This feature will be added in the next steps.\n");
    page_end();
}

/* ---------- Question screens ---------- */
static void cancel_question(void)
{
    printf("Cancelled.\n");
    show_screen();
}

static void ask_room(void)
{
    s_state = ST_ASK_ROOM;
    printf("Room number (1 or 2), or Enter to cancel: ");
    fflush(stdout);
}

static void ask_count(void)
{
    s_state = ST_ASK_COUNT;
    printf("New number of people in Room %d (0 to 500): ", s_w_room);
    fflush(stdout);
}

static void ask_date(void)
{
    s_state = ST_ASK_DATE;
    printf("Date as YYYY-MM-DD, or Enter to cancel: ");
    fflush(stdout);
}

static void ask_time(void)
{
    s_state = ST_ASK_TIME;
    printf("Time as HH:MM (24-hour), or Enter to cancel: ");
    fflush(stdout);
}

static void ask_tap(void)
{
    s_state = ST_ASK_TAP;
    printf("Card ID to tap (for example A1.2), or Enter to cancel: ");
    fflush(stdout);
}

static void ask_role(void)
{
    s_state = ST_ASK_ROLE;
    printf("Role: 1 Student  2 Staff  3 Cleaner  4 Other (Enter to cancel): ");
    fflush(stdout);
}

static void ask_reg(void)
{
    s_state = ST_ASK_REG;
    printf("Registration number. Next: %s\n", s_w_sugg);
    printf("Press Enter to accept it, or type another (0 = cancel): ");
    fflush(stdout);
}

static void ask_name(void)
{
    s_state = ST_ASK_NAME;
    printf("Full name, or Enter to cancel: ");
    fflush(stdout);
}

static void ask_card(void)
{
    char last[8];
    registry_card_progress(s_roles[s_w_role], last, s_w_next);

    s_state = ST_ASK_CARD;
    if (last[0] != '\0') {
        printf("%s cards given so far: up to %s.\n",
               registry_role_text(s_roles[s_w_role]), last);
    } else {
        printf("No %s cards given yet.\n", registry_role_text(s_roles[s_w_role]));
    }
    printf("Next free card: %s\n", s_w_next);
    printf("Press Enter to accept it, or type another free card (0 = cancel): ");
    fflush(stdout);
}

static void ask_save(void)
{
    s_state = ST_ASK_SAVE;
    printf("\nPlease check:\n");
    printf("  Role   : %s\n", registry_role_text(s_roles[s_w_role]));
    printf("  Number : %s\n", s_w_reg);
    printf("  Name   : %s\n", s_w_name);
    printf("  Card   : %s\n", s_w_label);
    printf("Save this person? (y = save, anything else = cancel): ");
    fflush(stdout);
}

static void ask_res_card(void)
{
    s_state = ST_ASK_RES_CARD;
    printf("Forgotten card: type its ID (A1.2) or the person's\n");
    printf("registration number (A/002), or Enter to cancel: ");
    fflush(stdout);
}

static void ask_res_pick(void)
{
    int  free_count, total;
    char next[8];
    registry_reserve_progress(&free_count, &total, next);

    s_state = ST_ASK_RES_PICK;
    printf("Reserve cards free: %d of %d. Next free: %s\n", free_count, total, next);
    printf("Press Enter to accept it, or type another free reserve card (0 = cancel): ");
    fflush(stdout);
}

static void ask_res_min(void)
{
    s_state = ST_ASK_RES_MIN;
    print_minutes_question("may the reserve card be used");
}

static void ask_res_save(void)
{
    char until[8];
    format_clock(time(NULL) + (time_t)s_w_minutes * 60, until, sizeof(until));

    s_state = ST_ASK_RES_SAVE;
    printf("\nPlease check:\n");
    printf("  Person   : %s (%s)\n", s_w_name, s_w_reg);
    printf("  Old card : %s (will be suspended)\n", s_w_label);
    printf("  Reserve  : %s\n", s_w_res);
    printf("  Active   : %d minutes, until %s\n", s_w_minutes, until);
    printf("Save? (y = save, anything else = cancel): ");
    fflush(stdout);
}

static void ask_return(void)
{
    s_state = ST_ASK_RETURN;
    printf("Reserve card ID being returned (e.g. B1.1), or Enter to cancel: ");
    fflush(stdout);
}

static void ask_gst_from(void)
{
    int  free_count, total;
    char first[8];
    registry_guest_progress(&free_count, &total, first);

    s_state = ST_ASK_GST_FROM;
    printf("Guest cards free: %d of %d.\n", free_count, total);
    printf("First card of the range. Press Enter for %s, or type one (0 = cancel): ", first);
    fflush(stdout);
}

static void ask_gst_to(void)
{
    s_state = ST_ASK_GST_TO;
    printf("Last card of the range. Press Enter for just %s, or type one (0 = cancel): ",
           s_w_gfrom);
    fflush(stdout);
}

static void ask_gst_min(void)
{
    s_state = ST_ASK_GST_MIN;
    print_minutes_question("should the guest cards stay active");
}

static void ask_gst_name(void)
{
    s_state = ST_ASK_GST_NAME;
    printf("Meeting or event name (optional). Press Enter to skip, or 0 to cancel: ");
    fflush(stdout);
}

static void ask_gst_save(void)
{
    char until[8];
    format_clock(time(NULL) + (time_t)s_w_minutes * 60, until, sizeof(until));

    s_state = ST_ASK_GST_SAVE;
    printf("\nPlease check:\n");
    if (s_w_gcount == 1) {
        printf("  Cards  : %s (1 card)\n", s_w_gfrom);
    } else {
        printf("  Cards  : %s to %s (%d cards)\n", s_w_gfrom, s_w_gto, s_w_gcount);
    }
    printf("  Active : %d minutes, until %s\n", s_w_minutes, until);
    printf("  Event  : %s\n", (s_w_event[0] != '\0') ? s_w_event : "(none)");
    printf("Activate them? (y = yes, anything else = cancel): ");
    fflush(stdout);
}

static void ask_lost_card(void)
{
    s_state = ST_ASK_LOST_CARD;
    printf("The lost card's ID (e.g. A1.2), or Enter to cancel: ");
    fflush(stdout);
}

static void ask_lost_yes(void)
{
    s_state = ST_ASK_LOST_YES;
    printf("Block %s FOREVER and issue a new card? Type YES to confirm: ", s_w_label);
    fflush(stdout);
}

static void ask_hdate(void)
{
    s_state = ST_ASK_HDATE;
    printf("Date as YYYY-MM-DD (or 'today'), or Enter to cancel: ");
    fflush(stdout);
}

static void ask_adate(void)
{
    s_state = ST_ASK_ADATE;
    printf("Date as YYYY-MM-DD (or 'today'), or just Enter for the latest alarms: ");
    fflush(stdout);
}

static void ask_open(void)
{
    s_state = ST_ASK_OPEN;
    printf("Opening time as HH:MM (for example 07:00), or Enter to cancel: ");
    fflush(stdout);
}

static void ask_close(void)
{
    s_state = ST_ASK_CLOSE;
    printf("Closing time as HH:MM (for example 18:00), or Enter to cancel: ");
    fflush(stdout);
}

static void ask_window(void)
{
    s_state = ST_ASK_WINDOW;
    printf("Minutes to keep the %s open (1 to 240, 0 = close it now), or Enter to cancel: ",
           s_w_win_entry ? "ENTRY" : "EXIT");
    fflush(stdout);
}

static void ask_alarm(void)
{
    s_state = ST_ASK_ALARM;
    printf("Minutes before closing (0 = off, up to 120), or Enter to cancel: ");
    fflush(stdout);
}

static void handle_ask_room(const char *line)
{
    int n;
    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    if (!parse_number(line, &n) || n < 1 || n > 2) {
        printf("The room must be 1 or 2.\n");
        ask_room();
        return;
    }
    s_w_room = n;
    ask_count();
}

static void handle_ask_count(const char *line)
{
    int n;
    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    if (!parse_number(line, &n) || n < 0 || n > 500) {
        printf("Please type a number from 0 to 500.\n");
        ask_count();
        return;
    }

    int before = room_headcount_get(s_w_room);
    room_headcount_set(s_w_room, n);
    printf("Room %d headcount set to %d.\n", s_w_room, n);
    ESP_LOGW(TAG, "%s corrected Room %d headcount to %d", s_user, s_w_room, n);

    /* every correction is written to the event log, with the guard's name */
    access_info_t ev;
    char note[40];
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.card_label, sizeof(ev.card_label), "R%d", s_w_room);
    snprintf(ev.name, sizeof(ev.name), "%s", s_user);
    snprintf(ev.reg_no, sizeof(ev.reg_no), "-");
    snprintf(note, sizeof(note), "count %d -> %d", before, n);
    eventlog_add(EV_CORRECT, &ev, note);

    show_screen();
}

/* The guard (or admin) sets the clock. */
static void start_clock(void)
{
    char when[24];
    wallclock_format(when, sizeof(when));

    header("CONFIGURATION - SET THE CLOCK");
    printf("Current time: %s\n", when);
    ask_date();
}

static void handle_ask_date(const char *line)
{
    int y, mo, d;
    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    if (sscanf(line, "%d-%d-%d", &y, &mo, &d) != 3 ||
        y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31) {
        printf("Please use the form YYYY-MM-DD, for example 2026-10-29.\n");
        ask_date();
        return;
    }
    s_w_year = y;
    s_w_month = mo;
    s_w_day = d;
    ask_time();
}

static void handle_ask_time(const char *line)
{
    int h, mi;
    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    if (sscanf(line, "%d:%d", &h, &mi) != 2 || h < 0 || h > 23 || mi < 0 || mi > 59) {
        printf("Please use the form HH:MM, for example 17:44.\n");
        ask_time();
        return;
    }
    if (!wallclock_set(s_w_year, s_w_month, s_w_day, h, mi)) {
        printf("That is not a valid date and time.\n");
        show_screen();
        return;
    }

    char when[24];
    wallclock_format(when, sizeof(when));
    printf("Clock set to %s\n", when);
    ESP_LOGW(TAG, "%s set the clock to %s", s_user, when);
    show_screen();
}

/* ---------- Access schedule: opening and closing time ---------- */
static void start_schedule_set(void)
{
    int o, c;
    schedule_get(&o, &c);

    header("ACCESS SCHEDULE - OPENING HOURS");
    printf("Now: %02d:%02d to %02d:%02d, every day.\n", o / 60, o % 60, c / 60, c % 60);
    printf("Outside these hours nobody can enter or leave.\n");
    ask_open();
}

static void handle_ask_open(const char *line)
{
    int m;
    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    if (!parse_hhmm(line, &m)) {
        printf("Please use the form HH:MM, for example 07:00.\n");
        ask_open();
        return;
    }
    s_w_open_min = m;
    ask_close();
}

static void handle_ask_close(const char *line)
{
    int m;
    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    if (!parse_hhmm(line, &m)) {
        printf("Please use the form HH:MM, for example 18:00.\n");
        ask_close();
        return;
    }
    if (!schedule_set(s_w_open_min, m)) {
        printf("The closing time must be later than the opening time.\n");
        ask_open();
        return;
    }

    printf("Saved: open every day from %02d:%02d to %02d:%02d.\n",
           s_w_open_min / 60, s_w_open_min % 60, m / 60, m % 60);
    ESP_LOGW(TAG, "%s set the schedule to %02d:%02d - %02d:%02d", s_user,
             s_w_open_min / 60, s_w_open_min % 60, m / 60, m % 60);

    char note[40];
    snprintf(note, sizeof(note), "schedule %02d:%02d-%02d:%02d",
             s_w_open_min / 60, s_w_open_min % 60, m / 60, m % 60);
    log_sched_event(note);

    show_screen();
}

/* ---------- Activate ENTRY or EXIT for some minutes ---------- */
static void start_window(bool entry)
{
    s_w_win_entry = entry;

    header(entry ? "ACCESS SCHEDULE - ACTIVATE ENTRY" : "ACCESS SCHEDULE - ACTIVATE EXIT");

    int left = schedule_window_left_sec(entry);
    if (left > 0) {
        printf("Already open: %d more min. A new time replaces it.\n", (left + 59) / 60);
    }
    if (entry) {
        printf("People can enter for this time, even after closing.\n");
    } else {
        printf("People inside can leave for this time, even after closing.\n");
    }
    printf("Blocked and unknown cards are still refused.\n");
    ask_window();
}

static void handle_ask_window(const char *line)
{
    int n;
    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    if (!parse_number(line, &n) || n < 0 || n > 240) {
        printf("Please type a number from 0 to 240.\n");
        ask_window();
        return;
    }

    schedule_activate(s_w_win_entry, n);

    char note[40];
    if (n == 0) {
        printf("The %s window is closed.\n", s_w_win_entry ? "ENTRY" : "EXIT");
        snprintf(note, sizeof(note), "%s closed by %s", s_w_win_entry ? "entry" : "exit", s_user);
    } else {
        printf("The %s is open for %d minute(s). It closes by itself.\n",
               s_w_win_entry ? "ENTRY" : "EXIT", n);
        snprintf(note, sizeof(note), "%s open %d min", s_w_win_entry ? "entry" : "exit", n);
    }
    ESP_LOGW(TAG, "%s: %s", s_user, note);
    log_sched_event(note);

    show_screen();
}

/* ---------- Closing alarm: minutes before closing ---------- */
static void start_alarm_set(void)
{
    int a = schedule_get_alarm();

    header("ACCESS SCHEDULE - CLOSING ALARM");
    if (a > 0) {
        printf("Now: the alarm sounds %d min before closing.\n", a);
    } else {
        printf("Now: the closing alarm is off.\n");
    }
    printf("It sounds once, as beep-beep three times.\n");
    ask_alarm();
}

static void handle_ask_alarm(const char *line)
{
    int n;
    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    if (!parse_number(line, &n) || n < 0 || n > 120) {
        printf("Please type a number from 0 to 120.\n");
        ask_alarm();
        return;
    }
    if (!schedule_set_alarm(n)) {
        printf("That is longer than the opening hours. Choose less.\n");
        ask_alarm();
        return;
    }

    char note[40];
    if (n == 0) {
        printf("Closing alarm is now off.\n");
        snprintf(note, sizeof(note), "closing alarm off");
    } else {
        printf("The alarm will sound %d minute(s) before closing.\n", n);
        snprintf(note, sizeof(note), "closing alarm %d min before", n);
    }
    ESP_LOGW(TAG, "%s: %s", s_user, note);
    log_sched_event(note);

    show_screen();
}

static void handle_ask_hdate(const char *line)
{
    char d[11];
    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    if (!parse_date(line, d)) {
        printf("Use YYYY-MM-DD, for example 2026-10-21, or type today.\n");
        ask_hdate();
        return;
    }
    page_occ_history(d);
}

static void handle_ask_adate(const char *line)
{
    char d[11];
    if (line[0] == '\0') {
        page_alarms(NULL);
        return;
    }
    if (!parse_date(line, d)) {
        printf("Use YYYY-MM-DD, for example 2026-10-21, or type today.\n");
        ask_adate();
        return;
    }
    page_alarms(d);
}

static void handle_ask_tap(const char *line)
{
    uint8_t uid[4];

    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    if (!registry_uid_by_label(line, uid)) {
        printf("There is no card with the ID '%s'. Try A1.1 (see 'List all cards').\n", line);
        ask_tap();
        return;
    }
    if (!lane_virtual_tap(s_w_tap_entry, uid)) {
        printf("The %s lane is busy or not ready. Try again in a moment.\n",
               s_w_tap_entry ? "ENTRY" : "EXIT");
        show_screen();
        return;
    }

    printf("Virtual tap sent: card %s at the %s reader.\n",
           line, s_w_tap_entry ? "ENTRY" : "EXIT");
    printf("Watch the log, and press the beam button when door 1 opens.\n");
    ESP_LOGI(TAG, "%s sent a virtual tap: card %s at %s", s_user, line,
             s_w_tap_entry ? "ENTRY" : "EXIT");
    show_screen();
}

/* ---------- Adding a person: role, number, name, card, save ---------- */
static void start_add_person(void)
{
    header("ASSIGN CARDS - NORMAL CARD");
    printf("Adds a new person and gives them a card.\n");
    ask_role();
}

static void handle_ask_role(const char *line)
{
    int n;

    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    if (!parse_number(line, &n) || n < 1 || n > 4) {
        printf("Please type 1, 2, 3 or 4.\n");
        ask_role();
        return;
    }
    s_w_role = n - 1;
    registry_suggest_reg_no(s_roles[s_w_role], s_w_sugg);
    ask_reg();
}

static void handle_ask_reg(const char *line)
{
    char who[32];

    if (strcmp(line, "0") == 0) {
        cancel_question();
        return;
    }

    /* Enter alone accepts the suggestion */
    const char *use = (line[0] == '\0') ? s_w_sugg : line;

    if (strlen(use) >= sizeof(s_w_reg) ||
        !registry_reg_no_valid(s_roles[s_w_role], use)) {
        printf("For a %s the number looks like %s:\n",
               registry_role_text(s_roles[s_w_role]), s_w_sugg);
        printf("the letter %c, a slash, then 3 to 6 digits.\n", s_w_sugg[0]);
        ask_reg();
        return;
    }

    snprintf(s_w_reg, sizeof(s_w_reg), "%s", use);
    s_w_reg[0] = (char)toupper((unsigned char)s_w_reg[0]);

    if (reg_exists(s_w_reg, who, sizeof(who))) {
        printf("The number %s already belongs to %s.\n", s_w_reg, who);
        ask_reg();
        return;
    }
    ask_name();
}

static void handle_ask_name(const char *line)
{
    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    if (strlen(line) >= sizeof(s_w_name)) {
        printf("That name is too long (at most %d characters).\n",
               (int)sizeof(s_w_name) - 1);
        ask_name();
        return;
    }
    snprintf(s_w_name, sizeof(s_w_name), "%s", line);
    ask_card();
}

static void handle_ask_card(const char *line)
{
    char owner[56];

    if (strcmp(line, "0") == 0) {
        cancel_question();
        return;
    }

    if (line[0] == '\0') {
        /* accept the next free card */
        snprintf(s_w_label, sizeof(s_w_label), "%s", s_w_next);
        ask_save();
        return;
    }

    if (strlen(line) > 6 || !registry_card_valid(s_roles[s_w_role], line)) {
        printf("For a %s the card looks like %s:\n",
               registry_role_text(s_roles[s_w_role]), s_w_next);
        printf("the letter %c, then 1. and a number from 1 to 250.\n", s_w_next[0]);
        ask_card();
        return;
    }

    /* write the card in its plain form (S1.007 becomes S1.7) */
    int n = atoi(line + 3);
    if (n < 1)   { n = 1; }
    if (n > 250) { n = 250; }
    snprintf(s_w_label, sizeof(s_w_label), "%c1.%d", toupper((unsigned char)line[0]), n);

    if (registry_card_owner_text(s_w_label, owner, sizeof(owner))) {
        printf("Card %s is already taken by %s.\n", s_w_label, owner);
        ask_card();
        return;
    }
    ask_save();
}

static void handle_ask_save(const char *line)
{
    char label[8];

    if (strcasecmp(line, "y") != 0 && strcasecmp(line, "yes") != 0) {
        cancel_question();
        return;
    }

    int r = registry_add_person(s_w_reg, s_w_name, s_roles[s_w_role], s_w_label, label);

    if (r == REG_ADD_OK) {
        printf("Added %s (%s), %s.\n", s_w_name, s_w_reg,
               registry_role_text(s_roles[s_w_role]));
        printf("Their card ID is %s. You can tap it under Simulation tools.\n", label);
        ESP_LOGW(TAG, "%s added %s (%s, %s) with card %s",
                 s_user, s_w_name, s_w_reg, registry_role_text(s_roles[s_w_role]), label);
    } else if (r == REG_ADD_DUPLICATE) {
        printf("A person with the number %s already exists. Nothing added.\n", s_w_reg);
    } else if (r == REG_ADD_CARD_TAKEN) {
        printf("Card %s is already taken. Nothing added.\n", s_w_label);
    } else if (r == REG_ADD_FULL) {
        printf("The registry is full. Nothing added.\n");
    } else {
        printf("The number or card was not accepted. Nothing added.\n");
    }
    show_screen();
}

/* ---------- Reserve card (forgot the card) ---------- */
static void start_reserve(void)
{
    int  free_count, total;
    char next[8];
    registry_reserve_progress(&free_count, &total, next);

    header("ASSIGN CARDS - RESERVE CARD");
    printf("For someone who forgot their card. Their own card is\n");
    printf("suspended at once. Refused if they show as inside.\n");
    printf("Reserve cards free: %d of %d.\n", free_count, total);
    ask_res_card();
}

static void handle_ask_res_card(const char *line)
{
    access_info_t info;

    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    if (!find_normal_card(line, &info)) {
        printf("No person found for '%s'.\n", line);
        printf("Try the card ID or the registration number.\n");
        ask_res_card();
        return;
    }

    printf("Found: %s (%s), card %s\n", info.name, info.reg_no, info.card_label);

    if (info.card_status == CARD_BLOCKED) {
        printf("That card is blocked (lost). Use the person's new card\n");
        printf("or their registration number.\n");
        show_screen();
        return;
    }
    if (info.card_status == CARD_SUSPENDED) {
        printf("A reserve card is already out for this person.\n");
        printf("Return it first (Assign cards -> Return a reserve card).\n");
        show_screen();
        return;
    }
    if (info.inside) {
        printf("MISUSE ALERT: this person shows as INSIDE the building.\n");
        printf("A reserve card was refused.\n");
        ESP_LOGE(TAG, "MISUSE ALERT: %s asked for a reserve card for %s, but the person is INSIDE",
                 s_user, info.card_label);
        eventlog_add(EV_ALARM, &info, "reserve asked, person inside");
        show_screen();
        return;
    }
    if (!wallclock_is_set()) {
        printf("The clock is not set yet, so the time limit cannot work.\n");
        printf("Go to Configuration settings -> Set the clock, then try again.\n");
        show_screen();
        return;
    }

    int  free_count, total;
    char next[8];
    registry_reserve_progress(&free_count, &total, next);
    if (free_count == 0) {
        printf("No reserve card is free right now.\n");
        show_screen();
        return;
    }

    snprintf(s_w_label, sizeof(s_w_label), "%s", info.card_label);
    snprintf(s_w_name,  sizeof(s_w_name),  "%s", info.name);
    snprintf(s_w_reg,   sizeof(s_w_reg),   "%s", info.reg_no);
    ask_res_pick();
}

static void handle_ask_res_pick(const char *line)
{
    int  free_count, total;
    char next[8];
    registry_reserve_progress(&free_count, &total, next);

    if (strcmp(line, "0") == 0) {
        cancel_question();
        return;
    }
    if (line[0] == '\0') {
        snprintf(s_w_res, sizeof(s_w_res), "%s", next);
        ask_res_min();
        return;
    }

    /* a reserve card the guard typed: it must exist, be a reserve card, and be free */
    access_info_t card;
    if (!card_info(line, &card) || card.card_type != CARD_RESERVE) {
        printf("'%s' is not a reserve card. Reserve cards look like %s.\n", line, next);
        ask_res_pick();
        return;
    }
    if (card.card_status != CARD_AVAILABLE) {
        printf("%s is already out. Choose a free reserve card.\n", card.card_label);
        ask_res_pick();
        return;
    }
    snprintf(s_w_res, sizeof(s_w_res), "%s", card.card_label);
    ask_res_min();
}

static void handle_ask_res_min(const char *line)
{
    int minutes = 0;
    int r = read_minutes(line, &minutes);

    if (r == 1) {
        cancel_question();
    } else if (r == 2) {
        ask_res_min();
    } else {
        s_w_minutes = minutes;
        ask_res_save();
    }
}

static void handle_ask_res_save(const char *line)
{
    char reserve[8];

    if (strcasecmp(line, "y") != 0 && strcasecmp(line, "yes") != 0) {
        cancel_question();
        return;
    }

    cardop_t r = registry_issue_reserve_with(s_w_label, s_w_res, s_w_minutes, reserve);

    if (r == CARDOP_OK) {
        char until[8];
        format_clock(time(NULL) + (time_t)s_w_minutes * 60, until, sizeof(until));

        printf("Give the person card %s. It works until %s (%d minutes).\n",
               reserve, until, s_w_minutes);
        printf("Their own card %s is suspended until %s is returned.\n", s_w_label, reserve);
        ESP_LOGW(TAG, "%s issued reserve %s for %s (%d min)", s_user, reserve, s_w_label, s_w_minutes);

        char note[48];
        snprintf(note, sizeof(note), "%s for %s, %d min", reserve, s_w_label, s_w_minutes);
        log_action_event("RES", note);
    } else {
        printf("%s\n", cardop_text(r));
        if (r == CARDOP_PERSON_INSIDE) {
            ESP_LOGE(TAG, "MISUSE ALERT: %s asked for a reserve card for %s, but the person is INSIDE",
                     s_user, s_w_label);
        }
    }
    show_screen();
}

/* ---------- Return a reserve card ---------- */
static void start_return(void)
{
    header("ASSIGN CARDS - RETURN A RESERVE CARD");
    printf("The reserve card goes back to the drawer and the\n");
    printf("person's own card works again at once.\n\n");

    int n   = registry_list_cards(s_list, MAX_LIST);
    int out = 0;
    printf("Reserve cards out now:\n");
    for (int i = 0; i < n; i++) {
        if (s_list[i].card_type == CARD_RESERVE && s_list[i].card_status == CARD_ACTIVE) {
            char until[8] = "-";
            if (s_list[i].valid_until != 0) {
                format_clock(s_list[i].valid_until, until, sizeof(until));
            }
            printf("  %-5s  %-20.20s  until %s\n",
                   s_list[i].card_label, s_list[i].name, until);
            out++;
        }
    }
    if (out == 0) {
        printf("  (none)\n");
    }
    printf("\n");
    ask_return();
}

static void handle_ask_return(const char *line)
{
    char normal[8];

    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    cardop_t r = registry_return_reserve(line, normal);

    if (r == CARDOP_OK) {
        printf("Reserve card %s returned. Card %s works again.\n", line, normal);
        ESP_LOGW(TAG, "%s took back reserve %s; %s works again", s_user, line, normal);

        char note[48];
        snprintf(note, sizeof(note), "returned, %s works", normal);
        log_action_event("RES", note);
    } else {
        printf("%s\n", cardop_text(r));
    }
    show_screen();
}

/* ---------- Guest cards: activate a range for some minutes ---------- */
static void start_guest(void)
{
    int  free_count, total;
    char first[8];
    registry_guest_progress(&free_count, &total, first);

    header("ASSIGN CARDS - GUEST CARDS");
    printf("Activate a range of guest cards for a meeting or event.\n");
    printf("They expire by themselves, so they can be used again.\n");

    if (!wallclock_is_set()) {
        printf("The clock is not set yet, so the time limit cannot work.\n");
        printf("Go to Configuration settings -> Set the clock, then try again.\n");
        show_screen();
        return;
    }
    if (free_count == 0) {
        printf("No guest card is free right now.\n");
        show_screen();
        return;
    }
    ask_gst_from();
}

static void handle_ask_gst_from(const char *line)
{
    int  free_count, total;
    char first[8];
    registry_guest_progress(&free_count, &total, first);

    if (strcmp(line, "0") == 0) {
        cancel_question();
        return;
    }
    if (line[0] == '\0') {
        snprintf(s_w_gfrom, sizeof(s_w_gfrom), "%s", first);
        ask_gst_to();
        return;
    }
    if (!guest_card_from_text(line, s_w_gfrom)) {
        printf("'%s' is not a guest card. Guest cards look like %s.\n", line, first);
        ask_gst_from();
        return;
    }
    ask_gst_to();
}

static void handle_ask_gst_to(const char *line)
{
    char last[8];

    if (strcmp(line, "0") == 0) {
        cancel_question();
        return;
    }
    if (line[0] == '\0') {
        snprintf(last, sizeof(last), "%s", s_w_gfrom);
    } else if (!guest_card_from_text(line, last)) {
        printf("'%s' is not a guest card. Guest cards look like G1.10.\n", line);
        ask_gst_to();
        return;
    }

    char busy[64];
    int  count = 0, busy_count = 0;
    cardop_t r = registry_guest_range_check(s_w_gfrom, last, &count,
                                            busy, sizeof(busy), &busy_count);

    if (r == CARDOP_BAD_RANGE) {
        printf("%s\n", cardop_text(r));
        ask_gst_to();
    } else if (r == CARDOP_GUEST_BUSY) {
        printf("%d card(s) in that range are not free: %s\n", busy_count, busy);
        printf("(still active, or the holder is still inside)\n");
        printf("Please choose another range.\n");
        ask_gst_from();
    } else if (r != CARDOP_OK) {
        printf("%s\n", cardop_text(r));
        ask_gst_to();
    } else {
        snprintf(s_w_gto, sizeof(s_w_gto), "%s", last);
        s_w_gcount = count;
        ask_gst_min();
    }
}

static void handle_ask_gst_min(const char *line)
{
    int minutes = 0;
    int r = read_minutes(line, &minutes);

    if (r == 1) {
        cancel_question();
    } else if (r == 2) {
        ask_gst_min();
    } else {
        s_w_minutes = minutes;
        ask_gst_name();
    }
}

static void handle_ask_gst_name(const char *line)
{
    if (strcmp(line, "0") == 0) {
        cancel_question();
        return;
    }
    if (strlen(line) >= sizeof(s_w_event)) {
        printf("That name is too long (at most %d characters).\n",
               (int)sizeof(s_w_event) - 1);
        ask_gst_name();
        return;
    }
    snprintf(s_w_event, sizeof(s_w_event), "%s", line);
    ask_gst_save();
}

static void handle_ask_gst_save(const char *line)
{
    int count = 0;

    if (strcasecmp(line, "y") != 0 && strcasecmp(line, "yes") != 0) {
        cancel_question();
        return;
    }

    cardop_t r = registry_activate_guests(s_w_gfrom, s_w_gto, s_w_minutes, &count);

    if (r == CARDOP_OK) {
        char until[8];
        format_clock(time(NULL) + (time_t)s_w_minutes * 60, until, sizeof(until));

        if (count == 1) {
            printf("Guest card %s is active until %s (%d minutes).\n",
                   s_w_gfrom, until, s_w_minutes);
        } else {
            printf("Guest cards %s to %s (%d cards) are active until %s (%d minutes).\n",
                   s_w_gfrom, s_w_gto, count, until, s_w_minutes);
        }
        printf("They expire by themselves. After that they can be used again.\n");
        ESP_LOGW(TAG, "%s activated guest cards %s to %s for %d min (%s)",
                 s_user, s_w_gfrom, s_w_gto, s_w_minutes,
                 (s_w_event[0] != '\0') ? s_w_event : "no event name");

        char note[64];
        if (s_w_event[0] != '\0') {
            snprintf(note, sizeof(note), "%s-%s %dm %s",
                     s_w_gfrom, s_w_gto, s_w_minutes, s_w_event);
        } else {
            snprintf(note, sizeof(note), "%s-%s %dm", s_w_gfrom, s_w_gto, s_w_minutes);
        }
        log_action_event("GST", note);
    } else {
        printf("%s\n", cardop_text(r));
    }
    show_screen();
}

/* ---------- Lost card ---------- */
static void start_lost(void)
{
    header("ASSIGN CARDS - LOST CARD");
    printf("The lost card is blocked forever. The same person\n");
    printf("gets a new card with a new ID.\n");
    ask_lost_card();
}

static void handle_ask_lost_card(const char *line)
{
    uint8_t uid[4];

    if (line[0] == '\0') {
        cancel_question();
        return;
    }
    if (strlen(line) >= sizeof(s_w_label) || !registry_uid_by_label(line, uid)) {
        printf("There is no card with the ID '%s'.\n", line);
        ask_lost_card();
        return;
    }
    snprintf(s_w_label, sizeof(s_w_label), "%s", line);
    ask_lost_yes();
}

static void handle_ask_lost_yes(const char *line)
{
    char fresh[8];

    if (strcasecmp(line, "YES") != 0) {
        cancel_question();
        return;
    }

    /* remember who owns the card before it is blocked, for the event log */
    access_info_t owner;
    bool have_owner = card_info(s_w_label, &owner);

    cardop_t r = registry_report_lost(s_w_label, fresh);

    if (r == CARDOP_OK) {
        printf("Card %s is blocked forever.\n", s_w_label);
        printf("The person's new card is %s.\n", fresh);
        ESP_LOGW(TAG, "%s reported %s lost: blocked. New card %s", s_user, s_w_label, fresh);

        if (have_owner) {
            char note[40];
            snprintf(note, sizeof(note), "by %s, new %s", s_user, fresh);
            eventlog_add(EV_BLOCKED, &owner, note);
        }
    } else {
        printf("%s\n", cardop_text(r));
    }
    show_screen();
}

/* ---------- Doing what a menu item says ---------- */
static void run_action(action_t a)
{
    switch (a) {
    case ACT_OPEN_OCCUPANCY: s_screen = SCR_OCCUPANCY; show_screen(); break;
    case ACT_OPEN_ROOMS:     s_screen = SCR_ROOMS;     show_screen(); break;
    case ACT_OPEN_ASSIGN:    s_screen = SCR_ASSIGN;    show_screen(); break;
    case ACT_OPEN_CONFIG:    s_screen = SCR_CONFIG;    show_screen(); break;
    case ACT_OPEN_SCHEDULE:  s_screen = SCR_SCHEDULE;  show_screen(); break;
    case ACT_OPEN_SIM:       s_screen = SCR_SIM;       show_screen(); break;
    case ACT_OPEN_HISTORY:   s_screen = SCR_HISTORY;   show_screen(); break;
    case ACT_OPEN_ALERTS:    s_screen = SCR_ALERTS;    show_screen(); break;
    case ACT_HELP:           page_help();              break;
    case ACT_OCC_BUILDING:   page_building();          break;
    case ACT_OCC_ROOM1:      page_room(1);             break;
    case ACT_OCC_ROOM2:      page_room(2);             break;
    case ACT_OCC_SUMMARY:    page_summary();           break;
    case ACT_ROOM_LIGHTS:    page_lights();            break;
    case ACT_ROOM_CORRECT:
        header("ROOM CONTROL - CORRECT A HEADCOUNT");
        ask_room();
        break;
    case ACT_ASSIGN_NORMAL:  start_add_person();       break;
    case ACT_ASSIGN_RESERVE: start_reserve();          break;
    case ACT_ASSIGN_RETURN:  start_return();           break;
    case ACT_ASSIGN_GUEST:   start_guest();            break;
    case ACT_ASSIGN_LOST:    start_lost();             break;
    case ACT_CFG_CLOCK:      start_clock();            break;
    case ACT_SCHED_SET:      start_schedule_set();     break;
    case ACT_SCHED_EXIT:     start_window(false);      break;
    case ACT_SCHED_ENTRY:    start_window(true);       break;
    case ACT_SCHED_ALARM:    start_alarm_set();        break;
    case ACT_SIM_ENTRY:
        s_w_tap_entry = true;
        header("SIMULATION TOOLS - TAP AT ENTRY");
        ask_tap();
        break;
    case ACT_SIM_EXIT:
        s_w_tap_entry = false;
        header("SIMULATION TOOLS - TAP AT EXIT");
        ask_tap();
        break;
    case ACT_SIM_LIST:       page_cards();             break;
    case ACT_HIST_OCC:
        header("HISTORY - OCCUPANCY ON A DATE");
        ask_hdate();
        break;
    case ACT_HIST_BLOCKED:   page_blocked();           break;
    case ACT_HIST_ALARMS:
        header("HISTORY - ALARMS");
        ask_adate();
        break;
    case ACT_HIST_EVENTS:    page_events();            break;
    case ACT_ALERT_STATUS:   page_alert_status();      break;
    case ACT_ALERT_ACK:      do_alert_ack();           break;
    case ACT_COMING_SOON:    page_coming_soon();       break;
    }
}

static void handle_choice(const char *line)
{
    if (line[0] == '\0') {              /* just Enter: redraw the screen */
        show_screen();
        return;
    }

    int n;
    if (!parse_number(line, &n) || n < 0) {
        printf("Please type a number from the menu.\n");
        ask_choice();
        return;
    }

    if (n == 0) {
        if (s_screen == SCR_MAIN) {
            do_logout();
        } else {
            s_screen = s_screens[s_screen].parent;
            show_screen();
        }
        return;
    }

    const item_t *it = pick_item(n);
    if (it == NULL) {
        printf("There is no option %d. Please choose from the menu.\n", n);
        ask_choice();
        return;
    }
    run_action(it->action);
}

/* ---------- Login steps ---------- */
static void handle_username(const char *line)
{
    uint32_t now = now_ms();

    if (line[0] == '\0') {
        ask_user();
        return;
    }
    if (now < s_locked_until) {
        printf("Login is locked. Try again in %u s.\n",
               (unsigned)((s_locked_until - now + 999) / 1000));
        ask_user();
        return;
    }
    snprintf(s_pending_user, sizeof(s_pending_user), "%s", line);
    ask_pin();
}

static void handle_password(const char *pin)
{
    uint32_t now = now_ms();

    for (size_t i = 0; i < ARRAY_LEN(s_accounts); i++) {
        if (strcmp(s_pending_user, s_accounts[i].user) == 0 &&
            strcmp(pin, s_accounts[i].pin) == 0) {
            s_level       = s_accounts[i].level;
            s_user        = s_accounts[i].user;
            s_fail_count  = 0;
            s_last_active = now;
            s_screen      = SCR_MAIN;
            ESP_LOGI(TAG, "login OK: %s", s_user);
            show_screen();
            return;
        }
    }

    s_fail_count++;
    ESP_LOGW(TAG, "login FAILED for user '%s' (%d/%d)",
             s_pending_user, s_fail_count, MAX_LOGIN_FAILS);

    if (s_fail_count >= MAX_LOGIN_FAILS) {
        s_locked_until = now + LOCKOUT_MS;
        s_fail_count   = 0;
        printf("Too many wrong tries. Login locked for %d s.\n", LOCKOUT_MS / 1000);
        ESP_LOGE(TAG, "login LOCKED for %d s", LOCKOUT_MS / 1000);
    } else {
        printf("Wrong username or password. Please try again.\n");
    }
    ask_user();
}

/* One finished line has been typed. */
static void handle_line(const char *line)
{
    if (s_state != ST_USER && s_state != ST_PIN) {
        s_last_active = now_ms();
    }

    switch (s_state) {
    case ST_USER:          handle_username(line);      break;
    case ST_PIN:           handle_password(line);      break;
    case ST_MENU:          handle_choice(line);        break;
    case ST_PAUSE:         show_screen();              break;
    case ST_ASK_ROOM:      handle_ask_room(line);      break;
    case ST_ASK_COUNT:     handle_ask_count(line);     break;
    case ST_ASK_DATE:      handle_ask_date(line);      break;
    case ST_ASK_TIME:      handle_ask_time(line);      break;
    case ST_ASK_TAP:       handle_ask_tap(line);       break;
    case ST_ASK_ROLE:      handle_ask_role(line);      break;
    case ST_ASK_REG:       handle_ask_reg(line);       break;
    case ST_ASK_NAME:      handle_ask_name(line);      break;
    case ST_ASK_CARD:      handle_ask_card(line);      break;
    case ST_ASK_SAVE:      handle_ask_save(line);      break;
    case ST_ASK_RES_CARD:  handle_ask_res_card(line);  break;
    case ST_ASK_RES_PICK:  handle_ask_res_pick(line);  break;
    case ST_ASK_RES_MIN:   handle_ask_res_min(line);   break;
    case ST_ASK_RES_SAVE:  handle_ask_res_save(line);  break;
    case ST_ASK_RETURN:    handle_ask_return(line);    break;
    case ST_ASK_GST_FROM:  handle_ask_gst_from(line);  break;
    case ST_ASK_GST_TO:    handle_ask_gst_to(line);    break;
    case ST_ASK_GST_MIN:   handle_ask_gst_min(line);   break;
    case ST_ASK_GST_NAME:  handle_ask_gst_name(line);  break;
    case ST_ASK_GST_SAVE:  handle_ask_gst_save(line);  break;
    case ST_ASK_LOST_CARD: handle_ask_lost_card(line); break;
    case ST_ASK_LOST_YES:  handle_ask_lost_yes(line);  break;
    case ST_ASK_HDATE:     handle_ask_hdate(line);     break;
    case ST_ASK_ADATE:     handle_ask_adate(line);     break;
    case ST_ASK_OPEN:      handle_ask_open(line);      break;
    case ST_ASK_CLOSE:     handle_ask_close(line);     break;
    case ST_ASK_WINDOW:    handle_ask_window(line);    break;
    case ST_ASK_ALARM:     handle_ask_alarm(line);     break;
    }
}

/* ---------- The menu task ---------- */
static void menu_task(void *arg)
{
    (void)arg;
    char line[LINE_MAX_LEN];
    int  len = 0;
    bool last_was_cr = false;

    vTaskDelay(pdMS_TO_TICKS(2000));     /* let the start-up messages finish */
    show_login_screen();

    for (;;) {
        uint8_t c;
        int n = uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100));

        if (n <= 0) {
            /* nothing typed: check the idle timeout (only when logged in) */
            if (s_state != ST_USER && s_state != ST_PIN &&
                (now_ms() - s_last_active) > SESSION_TIMEOUT_MS) {
                ESP_LOGW(TAG, "session of %s timed out", s_user);
                s_level = LEVEL_NONE;
                s_user  = "";
                len = 0;
                printf("\nSession timed out. You have been logged out.\n");
                show_login_screen();
            }
            continue;
        }

        if (c == '\n' && last_was_cr) {      /* second half of CR+LF: ignore */
            last_was_cr = false;
            continue;
        }
        last_was_cr = (c == '\r');

        if (c == '\r' || c == '\n') {
            printf("\n");
            line[len] = '\0';
            len = 0;
            handle_line(line);
        } else if (c == 0x08 || c == 0x7F) {  /* backspace */
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (c >= 32 && c < 127 && len < LINE_MAX_LEN - 1) {
            line[len++] = (char)c;
            putchar(s_state == ST_PIN ? '*' : c);   /* hide the password */
            fflush(stdout);
        }
    }
}

void menu_start(void)
{
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    xTaskCreate(menu_task, "menu_task", 6144, NULL, 3, NULL);
}