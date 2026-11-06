/* Client action state machine: what to send, when to repeat it, and when to stop.
 *
 * See controller-actions.h for the rules. Everything here is a function of (table, now), so a test can drive
 * an hour of behaviour in a microsecond and the daemon's loop has nothing to remember of its own.
 */
#include <stdio.h>
#include <string.h>
#include "controller-actions.h"

static int same_mac(const uint8_t a[6], const uint8_t b[6])
{
    return !memcmp(a, b, 6);
}

static struct action_entry *find(struct action_table *table, const uint8_t mac[6], int kind)
{
    unsigned int i;
    for (i = 0; i < table->count; i++)
        if (table->entry[i].kind == kind && same_mac(table->entry[i].mac, mac))
            return &table->entry[i];
    return NULL;
}

static void drop(struct action_table *table, unsigned int index)
{
    if (index + 1 < table->count)
        memmove(&table->entry[index], &table->entry[index + 1],
                (table->count - index - 1) * sizeof(table->entry[0]));
    table->count--;
}

void actions_clear(struct action_table *table)
{
    if (table)
        memset(table, 0, sizeof(*table));
}

int actions_configure(struct action_table *table, long now, const struct action_intent *intents,
                      unsigned int count)
{
    unsigned int i;

    if (!table || (count && !intents))
        return -1;
    table->now = now;
    /* A rule that is no longer wanted still has to be undone on the agent, so bans are marked for lifting and
     * only removed after the undo has gone out. Everything else that vanished is forgotten right here. */
    for (i = 0; i < table->count;) {
        const struct action_entry *entry = &table->entry[i];
        unsigned int seen = 0, k;
        for (k = 0; k < count; k++)
            if (intents[k].kind == entry->kind && same_mac(intents[k].mac, entry->mac))
                seen = 1;
        if (seen) {
            i++;
        } else if (entry->kind == ACTION_KIND_BAN && entry->state == ACTION_EXPIRED) {
            drop(table, i); /* it ran out and was let back in; nothing to undo */
        } else if (entry->kind == ACTION_KIND_BAN && !entry->lift) {
            table->entry[i].lift = 1;
            table->entry[i].state = ACTION_PENDING;
            table->entry[i].expires = 0;
            table->entry[i].next = now;
            i++;
        } else if (entry->kind == ACTION_KIND_BAN) {
            i++; /* already waiting for its undo to go out */
        } else {
            drop(table, i);
        }
    }

    for (i = 0; i < count; i++) {
        struct action_entry *entry = find(table, intents[i].mac, intents[i].kind);
        if (intents[i].kind != ACTION_KIND_KICK && intents[i].kind != ACTION_KIND_BAN)
            return -1;
        if (entry) {
            /* Known rule. Only the expiry can change, and only for a ban that has not been lifted -- and only when
             * the configured lifetime itself changed: the daemon hands the same intents in once a second, so
             * re-anchoring on every pass would push the expiry a minute further out every second and a one-minute
             * ban would never end. */
            if (entry->kind == ACTION_KIND_BAN && entry->lift) {
                /* The rule came back before its undo went out: cancel the undo, and send the block again. */
                entry->lift = 0;
                entry->state = ACTION_PENDING;
                entry->next = now;
                entry->minutes = intents[i].minutes;
                entry->expires = intents[i].minutes ? now + (long)intents[i].minutes * 60 : 0;
            } else if (entry->kind == ACTION_KIND_BAN && entry->minutes != intents[i].minutes) {
                /* A new lifetime in the page re-arms the rule, including one that already ran out: that is the
                 * user saying "block it again, for this long". */
                entry->minutes = intents[i].minutes;
                entry->expires = intents[i].minutes ? now + (long)intents[i].minutes * 60 : 0;
                entry->state = ACTION_PENDING;
                entry->next = now;
            }
            continue;
        }
        if (table->count >= ACTION_MAX)
            return -1;
        entry = &table->entry[table->count++];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->mac, intents[i].mac, 6);
        entry->kind = intents[i].kind;
        entry->state = ACTION_PENDING;
        entry->next = now;
        entry->minutes = intents[i].minutes;
        if (entry->kind == ACTION_KIND_BAN && intents[i].minutes)
            entry->expires = now + (long)intents[i].minutes * 60;
        if (entry->kind == ACTION_KIND_KICK)
            entry->expires = now + (long)ACTION_KICK_RETRY * ACTION_KICK_TRIES + ACTION_KICK_RETRY;
    }
    return (int)table->count;
}

static void emit(struct action_due *out, unsigned int *used, unsigned int max, const struct action_entry *entry,
                 int allow)
{
    if (*used >= max)
        return;
    memcpy(out[*used].mac, entry->mac, 6);
    out[*used].kind = entry->kind;
    out[*used].allow = allow;
    (*used)++;
}

unsigned int actions_tick(struct action_table *table, long now, struct action_due *out, unsigned int max)
{
    unsigned int used = 0, i;

    if (!table || !out)
        return 0;
    table->now = now;
    for (i = 0; i < table->count;) {
        struct action_entry *entry = &table->entry[i];

        if (entry->kind == ACTION_KIND_BAN) {
            if (entry->lift) {
                /* The user removed the rule: say so on the agent, then forget it. */
                emit(out, &used, max, entry, 1);
                drop(table, i);
                continue;
            }
            if (entry->state == ACTION_EXPIRED) {
                i++;
                continue;
            }
            if (entry->expires && now >= entry->expires) {
                /* Time is up: say so on the agent and keep the row as "expired" instead of dropping it. Dropping
                 * would let the next config pass (one second later) create it again from the same unchanged rule,
                 * and the client would be blocked, released and blocked again for ever. */
                emit(out, &used, max, entry, 1);
                entry->state = ACTION_EXPIRED;
                entry->expires = 0;
                entry->next = 0;
                i++;
                continue;
            }
            if (entry->state == ACTION_PENDING || now >= entry->next) {
                emit(out, &used, max, entry, 0);
                entry->state = ACTION_SENT;
                entry->attempts++;
                entry->next = now + ACTION_BAN_REFRESH;
            }
            i++;
            continue;
        }

        /* A kick that has run out of time without an answer is reported as failed instead of retried forever. */
        if (entry->state == ACTION_SENT && now >= entry->expires) {
            entry->state = ACTION_FAILED;
            i++;
            continue;
        }
        if (entry->state == ACTION_DONE || entry->state == ACTION_FAILED) {
            i++;
            continue;
        }
        if (now >= entry->next && entry->attempts < ACTION_KICK_TRIES) {
            emit(out, &used, max, entry, 0);
            entry->state = ACTION_SENT;
            entry->attempts++;
            entry->next = now + ACTION_KICK_RETRY;
        }
        i++;
    }
    return used;
}

void actions_observe(struct action_table *table, const uint8_t mac[6], int ok)
{
    struct action_entry *entry;

    if (!table || !mac)
        return;
    entry = find(table, mac, ACTION_KIND_KICK);
    if (!entry)
        return;
    /* One report settles it: an agent that says "the client left" is not going to un-say it, and one that
     * refuses is not going to change its mind about this request. A later request is a new entry. */
    entry->state = ok ? ACTION_DONE : ACTION_FAILED;
    entry->next = 0;
    entry->expires = 0;
}

static const char *state_name(int state)
{
    switch (state) {
    case ACTION_PENDING: return "pending";
    case ACTION_SENT: return "sent";
    case ACTION_DONE: return "done";
    case ACTION_EXPIRED: return "expired";
    default: return "failed";
    }
}

int actions_json(const struct action_table *table, char *out, size_t room)
{
    size_t used = 0;
    unsigned int i;
    int first = 1;

    if (!table || !out || !room)
        return -1;
    out[0] = 0;
    for (i = 0; i < table->count; i++) {
        const struct action_entry *entry = &table->entry[i];
        long left = entry->expires ? entry->expires - table->now : -1;
        int written = snprintf(out + used, room - used,
                               "%s{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"action\":\"%s\",\"state\":\"%s\","
                               "\"left\":%ld,\"attempts\":%u}",
                               first ? "" : ",", entry->mac[0], entry->mac[1], entry->mac[2], entry->mac[3],
                               entry->mac[4], entry->mac[5], entry->kind == ACTION_KIND_BAN ? "ban" : "kick",
                               state_name(entry->state), left, entry->attempts);
        if (written < 0 || (size_t)written >= room - used)
            return -1;
        used += (size_t)written;
        first = 0;
    }
    return (int)used;
}
