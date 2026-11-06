#ifndef ZTE_CONTROLLER_ACTIONS_H
#define ZTE_CONTROLLER_ACTIONS_H
#include <stddef.h>
#include <stdint.h>

/* What happens after the decision to act on a client, which is the part that is easy to get wrong and hard to
 * see: a kick that is retried on every loop pass floods the agent, a ban that is sent once is lost when the
 * agent reboots, and a ban the user removed has to be lifted on the agent as well or the client stays out.
 *
 * So the rules live here, as a state machine over (now, intents) with no sockets, no files and no clock of its
 * own: the daemon hands in the time and sends whatever tick() says is due; a host test hands in made-up times
 * and reads the same answers.
 *
 * Vocabulary: a KICK is one-shot (steer this client off, learn whether it worked), a BAN is a standing rule
 * (keep this client off this BSS until the time runs out or the user removes it). */

#define ACTION_MAX 8
#define ACTION_KICK_RETRY 5      /* seconds between BTM requests for the same client */
#define ACTION_KICK_TRIES 3      /* BTM requests before giving up */
#define ACTION_BAN_REFRESH 120   /* seconds between repetitions of a ban (an agent that restarted forgot it) */

enum action_kind { ACTION_KIND_NONE = 0, ACTION_KIND_KICK = 1, ACTION_KIND_BAN = 2 };
/* ACTION_EXPIRED: a timed ban whose time ran out. The entry stays so the unchanged rule does not re-arm it on the
 * next config pass (the daemon folds the same intents in once a second, and a rule re-created right after its
 * "allow" went out would flap the client off and on for ever). It is dropped when the rule is removed, and
 * re-armed when its lifetime in the page changes. */
enum action_state { ACTION_PENDING = 0, ACTION_SENT, ACTION_DONE, ACTION_FAILED, ACTION_EXPIRED };

struct action_entry {
    uint8_t mac[6];
    int kind;        /* action_kind */
    int state;       /* action_state */
    int lift;        /* ban only: the rule is gone, send the matching "allow" and drop it */
    unsigned int minutes; /* ban only: the lifetime this entry was anchored with (0 = until the user undoes it) */
    long expires;    /* ban: unban at this time (0 = until the user undoes it); kick: give up at this time */
    long next;       /* earliest time for the next transmission */
    unsigned int attempts;
};

struct action_table {
    struct action_entry entry[ACTION_MAX];
    unsigned int count;
    /* The clock of the last configure/tick. state.json's countdown is measured against this rather than against a
     * time the caller passes in: the daemon ticks with CLOCK_MONOTONIC and used to write the JSON with time(NULL),
     * which turned every "seconds left" into a wall clock minus a monotonic deadline. */
    long now;
};

/* One line of user intent: what to do with `mac`. minutes = 0 means "until undone" for a ban and is ignored
 * for a kick. */
struct action_intent {
    uint8_t mac[6];
    int kind;
    unsigned int minutes;
};

/* What a tick wants sent. `kind` is ACTION_KIND_KICK (0x8014) or ACTION_KIND_BAN (0x8016); `allow` = 1 asks
 * for the ban's release form (or, for a ban whose rule vanished, the undo). */
struct action_due {
    uint8_t mac[6];
    int kind;
    int allow;
};

void actions_clear(struct action_table *table);

/* Applies the configured intents and keeps the state of everything already known, so reloading the config does
 * not re-kick a client that has been dealt with. A ban that disappears from the intents is turned into a
 * pending undo instead of being dropped; a kick that disappears is simply forgotten.
 * Returns the number of entries after the change, or -1 when there are more intents than ACTION_MAX. */
int actions_configure(struct action_table *table, long now, const struct action_intent *intents,
                      unsigned int count);

/* Collects up to `max` transmissions that are due at `now` and advances the table. Returns how many were put
 * in `out`. Calling it twice with the same `now` never yields the same work twice. */
unsigned int actions_tick(struct action_table *table, long now, struct action_due *out, unsigned int max);

/* Feedback from the agent (a BTM report or a Steering Completed naming this client). `ok` = 1 means the client
 * did what was asked. Only KICK entries are affected; a ban has no per-client answer. */
void actions_observe(struct action_table *table, const uint8_t mac[6], int ok);

/* JSON array for state.json, bounded: *room bytes is always enough for fewer entries than ACTION_MAX too. The
 * countdown is measured against the table's own clock (the last configure/tick), never against a caller's. */
int actions_json(const struct action_table *table, char *out, size_t room);

#endif
