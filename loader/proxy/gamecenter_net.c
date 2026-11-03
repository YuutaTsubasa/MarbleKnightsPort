// Game Center matches over the local network (replaces GameKit's GKMatchmaker / GKMatch for the
// game's WFGameCenter plugin, DllImport "__Internal").
//
// The game makes online rooms with StartMatchmaking(min, max, playerGroup = room code, attrs):
// everyone who searches with the same room code ends up in one match. Its own GameCenterMessageBus
// (heartbeats, reliable messages, host migration) runs on top of the match, so we only provide
// matchmaking, the player list, connection-state events and data delivery.
//
// LAN protocol (UDP; one data socket per instance, discovery beacons broadcast to MK_DISC_PORT):
//   - a searching device broadcasts a beacon for its room code; the first one in a room (lowest id
//     when two start together) becomes the room's coordinator ("owner")
//   - others send JOIN to the owner while it accepts players (matchmaking or AddPlayersToMatch);
//     the owner sends the member list (MEMBERS) to everyone, peers then talk directly
//   - data: unreliable datagrams, or reliable (per-peer sequence, ack, resend, in-order delivery)
//   - heartbeats every second; a peer silent for PEER_TIMEOUT_MS is disconnected
// Events follow GameKit's order: found match (players still "connecting"), one Connected state
// change per player, then match started. All callbacks run on the game's main thread
// (mk_gc_net_pump, from il2cpp_runtime_invoke).
#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mkgc", __VA_ARGS__)

#define MK_DISC_PORT 47811
#define MAX_PEERS 7
#define ALIAS_LEN 24
#define BEACON_MS 500
#define JOIN_MS 500
#define OWNER_AFTER_MS 1500
#define HEARTBEAT_MS 1000
#define PEER_TIMEOUT_MS 10000
#define RESEND_MS 250
#define RX_WINDOW 128

enum { GC_STATE_UNKNOWN = 0, GC_STATE_CONNECTED = 1, GC_STATE_DISCONNECTED = 2 };  // GKPlayerConnectionState
enum { MODE_RELIABLE = 0, MODE_UNRELIABLE = 1 };                                    // GKMatchSendDataMode

typedef struct { const char *id, *alias; } gc_player;  // GameCenter.Player, by value in x0/x1

// callbacks, in the order of GameCenter.GameCenter.EventHandlers
enum { H_AUTH, H_RECENT, H_BEST_HOST, H_NEARBY_FOUND, H_NEARBY_LEFT, H_INVITE, H_INVITE_RESPONSE,
       H_MM_CANCELLED, H_MM_FAILED, H_FOUND_MATCH, H_MATCHMAKING_FAILED, H_MATCH_STARTED, H_MATCH_FAILED,
       H_CONN_STATE, H_DATA, H_ACHIEVEMENTS, H_FRIENDS, H_ROOM_CODE, H_WINDOW_CLOSED, H_DISCONNECTED, H_COUNT };
static void *g_h[H_COUNT];

// ---- wire format ----
#define MAGIC 0x31474b4du  // "MKG1"
typedef struct __attribute__((packed)) { uint32_t magic; uint8_t type, pad[3]; uint32_t group; uint64_t from; } hdr;
typedef struct __attribute__((packed)) { uint8_t flags, nmem, max, pad; char alias[ALIAS_LEN]; } beacon_body;
enum { BF_OWNER = 1, BF_OPEN = 2 };
typedef struct __attribute__((packed)) { uint64_t id; char alias[ALIAS_LEN]; uint32_t ip; uint16_t port, pad; } member_entry;

// ---- state (g_mu) ----
typedef struct out_pkt { struct out_pkt *next; uint32_t seq; long long first, last; size_t len; uint8_t buf[]; } out_pkt;
typedef struct { uint8_t *data; size_t len; uint32_t seq; } rx_slot;
typedef struct {
    uint64_t id;
    char alias[ALIAS_LEN];
    struct sockaddr_in addr;
    long long heard, hb_sent;
    uint32_t tx_seq, rx_next;
    out_pkt *unacked;
    rx_slot rx[RX_WINDOW];
} peer;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_main_thread, g_net_thread;
static int g_ready, g_sock = -1, g_disc = -1;
static uint64_t g_id;
static char g_alias[ALIAS_LEN];
static uint32_t g_group;
static int g_min = 2, g_max = 4;
static int g_searching;          // StartMatchmaking / AddPlayersToMatch active: the room accepts players
static long long g_search_start, g_beacon_sent, g_join_sent;
static uint64_t g_owner;         // room coordinator (0 = none yet)
static struct sockaddr_in g_owner_addr;
static int g_in_match, g_started;
static peer g_peers[MAX_PEERS];
static int g_npeers;
// the owner's member list (the owner itself is entry 0, address 0 = "the sender")
static member_entry g_members[MAX_PEERS + 1];
static int g_nmembers;
static uint32_t g_members_seq;

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void id_str(uint64_t id, char out[24]) { snprintf(out, 24, "G:%016llx", (unsigned long long)id); }

// ---- events for the main thread ----
typedef struct ev { struct ev *next; int h; uint64_t id; char alias[ALIAS_LEN]; int state; uint8_t *data; size_t len; char *msg; } ev;
static ev *g_ev_head, **g_ev_tail = &g_ev_head;
static void push_ev(int h, const peer *p, int state, const void *data, size_t len, const char *msg) {
    ev *e = calloc(1, sizeof(*e));
    e->h = h;
    if (p) { e->id = p->id; memcpy(e->alias, p->alias, ALIAS_LEN); }
    e->state = state;
    if (data) { e->data = malloc(len ? len : 1); memcpy(e->data, data, len); e->len = len; }
    if (msg) e->msg = strdup(msg);
    *g_ev_tail = e;
    g_ev_tail = &e->next;
}

void mk_gc_net_pump(void) {
    if (!g_ready || !pthread_equal(pthread_self(), g_main_thread)) return;
    pthread_mutex_lock(&g_mu);
    ev *e = g_ev_head;
    g_ev_head = NULL;
    g_ev_tail = &g_ev_head;
    pthread_mutex_unlock(&g_mu);
    while (e) {
        ev *n = e->next;
        char ids[24];
        id_str(e->id, ids);
        gc_player pl = { ids, e->alias };
        void *f = g_h[e->h];
        static const char *names[H_COUNT] = { [H_FOUND_MATCH] = "found match", [H_MATCH_STARTED] = "match started",
            [H_CONN_STATE] = "connection state", [H_MATCHMAKING_FAILED] = "matchmaking failed", [H_DISCONNECTED] = "disconnected",
            [H_FRIENDS] = "friends", [H_RECENT] = "recent players", [H_MM_CANCELLED] = "matchmaker cancelled" };
        if (e->h != H_DATA) LOG("event: %s %s %s state=%d", names[e->h] ? names[e->h] : "?", e->id ? ids : "", e->alias, e->state);
        if (f) {
            switch (e->h) {
            case H_FOUND_MATCH: case H_MATCH_STARTED: case H_DISCONNECTED: case H_MM_CANCELLED:
                ((void (*)(void))f)(); break;
            case H_MATCHMAKING_FAILED: case H_MATCH_FAILED: case H_MM_FAILED:
                ((void (*)(const char *))f)(e->msg ? e->msg : ""); break;
            case H_CONN_STATE: ((void (*)(gc_player, int))f)(pl, e->state); break;
            case H_DATA: ((void (*)(gc_player, const void *, int))f)(pl, e->data, (int)e->len); break;
            case H_FRIENDS: case H_RECENT: ((void (*)(const void *, int, const char *))f)(NULL, 0, NULL); break;
            }
        }
        free(e->data);
        free(e->msg);
        free(e);
        e = n;
    }
}

// ---- sending ----
static void send_to(const struct sockaddr_in *a, int type, const void *body, size_t blen) {
    uint8_t buf[65536];
    if (g_sock < 0 || sizeof(hdr) + blen > sizeof(buf)) return;
    hdr h = { MAGIC, (uint8_t)type, { 0 }, g_group, g_id };
    memcpy(buf, &h, sizeof(h));
    if (blen) memcpy(buf + sizeof(h), body, blen);
    sendto(g_sock, buf, sizeof(h) + blen, 0, (const struct sockaddr *)a, sizeof(*a));
}
static void send_beacon(void) {
    beacon_body b = { 0 };
    b.flags = (g_owner == g_id ? BF_OWNER : 0) | (g_searching ? BF_OPEN : 0);
    b.nmem = (uint8_t)(g_owner == g_id ? g_nmembers : 0);
    b.max = (uint8_t)g_max;
    memcpy(b.alias, g_alias, ALIAS_LEN);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(MK_DISC_PORT), .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
    send_to(&a, 'B', &b, sizeof(b));
}

static peer *find_peer(uint64_t id) {
    for (int i = 0; i < g_npeers; i++) if (g_peers[i].id == id) return &g_peers[i];
    return NULL;
}
static void free_peer(peer *p) {
    for (out_pkt *o = p->unacked, *n; o; o = n) { n = o->next; free(o); }
    for (int i = 0; i < RX_WINDOW; i++) free(p->rx[i].data);
}
static void drop_peer(int i, int notify) {
    peer *p = &g_peers[i];
    LOG("peer %016llx (%s) gone", (unsigned long long)p->id, p->alias);
    if (notify && g_in_match) push_ev(H_CONN_STATE, p, GC_STATE_DISCONNECTED, NULL, 0, NULL);
    free_peer(p);
    g_peers[i] = g_peers[--g_npeers];
    memset(&g_peers[g_npeers], 0, sizeof(peer));
}

static void send_members(void);
static void owner_remove(uint64_t id) {
    if (g_owner != g_id) return;
    for (int i = 1; i < g_nmembers; i++)
        if (g_members[i].id == id) { g_members[i] = g_members[--g_nmembers]; send_members(); return; }
}

// apply a member list (from the owner, or the owner's own): add/remove peers, emit GameKit events
static void apply_members(const member_entry *m, int n, const struct sockaddr_in *owner_src) {
    uint64_t keep[MAX_PEERS + 1];
    int nkeep = 0, me = 0, added[MAX_PEERS], nadded = 0;
    for (int i = 0; i < n; i++) {
        if (m[i].id == g_id) { me = 1; continue; }
        keep[nkeep++] = m[i].id;
        if (find_peer(m[i].id) || g_npeers >= MAX_PEERS) continue;
        peer *p = &g_peers[g_npeers];
        memset(p, 0, sizeof(*p));
        p->id = m[i].id;
        memcpy(p->alias, m[i].alias, ALIAS_LEN);
        p->alias[ALIAS_LEN - 1] = 0;
        p->addr.sin_family = AF_INET;
        if (m[i].ip || !owner_src) { p->addr.sin_addr.s_addr = m[i].ip; p->addr.sin_port = m[i].port; }
        else p->addr = *owner_src;
        p->heard = now_ms();
        added[nadded++] = g_npeers++;
        LOG("peer %016llx (%s) at %s:%d", (unsigned long long)p->id, p->alias, inet_ntoa(p->addr.sin_addr), ntohs(p->addr.sin_port));
    }
    if (!me) return;
    for (int i = g_npeers - 1; i >= 0; i--) {
        int found = 0;
        for (int k = 0; k < nkeep; k++) found |= keep[k] == g_peers[i].id;
        if (!found) drop_peer(i, 1);
    }
    if (!g_in_match && n >= g_min) {
        g_in_match = 1;
        g_searching = 0;  // like findMatch's completion: the room stops taking players until AddPlayersToMatch
        LOG("match found: %d players in room %u", n, g_group);
        push_ev(H_FOUND_MATCH, NULL, 0, NULL, 0, NULL);
        for (int i = 0; i < g_npeers; i++) push_ev(H_CONN_STATE, &g_peers[i], GC_STATE_CONNECTED, NULL, 0, NULL);
        g_started = 1;
        push_ev(H_MATCH_STARTED, NULL, 0, NULL, 0, NULL);
    } else if (g_in_match) {
        for (int i = 0; i < nadded; i++) push_ev(H_CONN_STATE, &g_peers[added[i]], GC_STATE_CONNECTED, NULL, 0, NULL);
    }
}

static void send_members(void) {
    uint8_t body[8 + sizeof(g_members)];
    uint32_t seq = ++g_members_seq;
    memcpy(body, &seq, 4);
    body[4] = (uint8_t)g_nmembers;
    memcpy(body + 8, g_members, (size_t)g_nmembers * sizeof(member_entry));
    for (int i = 1; i < g_nmembers; i++) {
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = g_members[i].port, .sin_addr.s_addr = g_members[i].ip };
        send_to(&a, 'M', body, 8 + (size_t)g_nmembers * sizeof(member_entry));
    }
    apply_members(g_members, g_nmembers, NULL);
}

static void become_owner(void) {
    g_owner = g_id;
    memset(g_members, 0, sizeof(g_members));
    g_members[0].id = g_id;
    memcpy(g_members[0].alias, g_alias, ALIAS_LEN);
    g_nmembers = 1;
    LOG("room %u: no one else here yet, coordinating it", g_group);
}

static void reset_match(int tell_peers) {
    for (int i = g_npeers - 1; i >= 0; i--) {
        if (tell_peers) send_to(&g_peers[i].addr, 'L', NULL, 0);
        drop_peer(i, 0);
    }
    g_in_match = g_started = 0;
    g_owner = 0;
    g_nmembers = 0;
}

// ---- receiving (net thread, g_mu held) ----
static void deliver(peer *p, const uint8_t *d, size_t n) { push_ev(H_DATA, p, 0, d, n, NULL); }

static void on_packet(const uint8_t *buf, size_t n, const struct sockaddr_in *src) {
    if (n < sizeof(hdr)) return;
    hdr h;
    memcpy(&h, buf, sizeof(h));
    if (h.magic != MAGIC || h.from == g_id) return;
    const uint8_t *b = buf + sizeof(h);
    size_t bl = n - sizeof(h);
    peer *p = find_peer(h.from);
    if (p) p->heard = now_ms();
    switch (h.type) {
    case 'B': {
        if (h.group != g_group || bl < sizeof(beacon_body) || !(g_searching || g_owner == g_id)) return;
        beacon_body bb;
        memcpy(&bb, b, sizeof(bb));
        if (!(bb.flags & BF_OWNER)) return;
        int alone = g_owner == g_id && g_nmembers == 1;
        if (g_owner == g_id && !alone) return;  // we already run this room
        if (g_owner == g_id && h.from > g_id) return;  // two coordinators: the lower id keeps the room
        if ((bb.flags & BF_OPEN) && bb.nmem < bb.max && !g_in_match) {
            if (g_owner != h.from) LOG("room %u: joining %016llx (%.24s)", g_group, (unsigned long long)h.from, bb.alias);
            g_owner = h.from;
            g_owner_addr = *src;
            g_nmembers = 0;
        }
        return;
    }
    case 'J': {
        if (h.group != g_group || g_owner != g_id || !g_searching || bl < ALIAS_LEN) return;
        for (int i = 1; i < g_nmembers; i++) if (g_members[i].id == h.from) { send_members(); return; }
        if (g_nmembers >= g_max || g_nmembers > MAX_PEERS) return;
        member_entry *m = &g_members[g_nmembers++];
        memset(m, 0, sizeof(*m));
        m->id = h.from;
        memcpy(m->alias, b, ALIAS_LEN);
        m->alias[ALIAS_LEN - 1] = 0;
        m->ip = src->sin_addr.s_addr;
        m->port = src->sin_port;
        LOG("room %u: %016llx (%s) joined, %d players", g_group, (unsigned long long)h.from, m->alias, g_nmembers);
        send_members();
        return;
    }
    case 'M': {
        if (h.group != g_group || h.from != g_owner || bl < 8) return;
        int cnt = b[4];
        if (bl < 8 + (size_t)cnt * sizeof(member_entry) || cnt > MAX_PEERS + 1) return;
        member_entry m[MAX_PEERS + 1];
        memcpy(m, b + 8, (size_t)cnt * sizeof(member_entry));
        apply_members(m, cnt, src);
        return;
    }
    case 'H': return;
    case 'L':
        if (p) { drop_peer((int)(p - g_peers), 1); owner_remove(h.from); }
        return;
    case 'U':
        if (p) deliver(p, b, bl);
        return;
    case 'R': {
        if (!p || bl < 4) return;
        uint32_t seq;
        memcpy(&seq, b, 4);
        send_to(&p->addr, 'A', &seq, 4);
        if (seq < p->rx_next || seq - p->rx_next >= RX_WINDOW) return;
        rx_slot *s = &p->rx[seq % RX_WINDOW];
        if (s->data && s->seq == seq) return;
        free(s->data);
        s->data = malloc(bl - 4 ? bl - 4 : 1);
        memcpy(s->data, b + 4, bl - 4);
        s->len = bl - 4;
        s->seq = seq;
        for (;;) {
            rx_slot *r = &p->rx[p->rx_next % RX_WINDOW];
            if (!r->data || r->seq != p->rx_next) break;
            deliver(p, r->data, r->len);
            free(r->data);
            r->data = NULL;
            p->rx_next++;
        }
        return;
    }
    case 'A': {
        if (!p || bl < 4) return;
        uint32_t seq;
        memcpy(&seq, b, 4);
        for (out_pkt **o = &p->unacked; *o; o = &(*o)->next)
            if ((*o)->seq == seq) { out_pkt *d = *o; *o = d->next; free(d); break; }
        return;
    }
    }
}

static void tick(void) {
    long long t = now_ms();
    if (g_searching) {
        if (t - g_beacon_sent >= BEACON_MS) { send_beacon(); g_beacon_sent = t; }
        if (!g_owner && t - g_search_start >= OWNER_AFTER_MS) become_owner();
        if (g_owner && g_owner != g_id && !g_in_match && t - g_join_sent >= JOIN_MS) {
            send_to(&g_owner_addr, 'J', g_alias, ALIAS_LEN);
            g_join_sent = t;
        }
    }
    for (int i = g_npeers - 1; i >= 0; i--) {
        peer *p = &g_peers[i];
        if (t - p->hb_sent >= HEARTBEAT_MS) { send_to(&p->addr, 'H', NULL, 0); p->hb_sent = t; }
        int dead = t - p->heard > PEER_TIMEOUT_MS;
        for (out_pkt *o = p->unacked; o && !dead; o = o->next) {
            if (t - o->first > PEER_TIMEOUT_MS) dead = 1;
            else if (t - o->last >= RESEND_MS) {
                sendto(g_sock, o->buf, o->len, 0, (const struct sockaddr *)&p->addr, sizeof(p->addr));
                o->last = t;
            }
        }
        if (dead) {
            uint64_t id = p->id;
            drop_peer(i, 1);
            owner_remove(id);
        }
    }
}

static void *net_thread(void *arg) {
    (void)arg;
    uint8_t buf[65536];
    for (;;) {
        struct pollfd pf[2] = { { g_sock, POLLIN, 0 }, { g_disc, POLLIN, 0 } };
        poll(pf, g_disc >= 0 ? 2 : 1, 50);
        pthread_mutex_lock(&g_mu);
        for (int k = 0; k < 2; k++) {
            if (!(pf[k].revents & POLLIN)) continue;
            for (;;) {
                struct sockaddr_in src;
                socklen_t sl = sizeof(src);
                ssize_t n = recvfrom(pf[k].fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&src, &sl);
                if (n <= 0) break;
                on_packet(buf, (size_t)n, &src);
            }
        }
        tick();
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

// ---- identity: <external files>/player.txt "<hex id> <alias>", made on first run ----
static void load_identity(const char *files_dir) {
    char path[600];
    snprintf(path, sizeof(path), "%s/player.txt", files_dir);
    FILE *f = fopen(path, "r");
    if (f) {
        unsigned long long id = 0;
        char alias[ALIAS_LEN] = "";
        if (fscanf(f, "%llx %23[^\n]", &id, alias) >= 1) { g_id = id; snprintf(g_alias, sizeof(g_alias), "%s", alias); }
        fclose(f);
    }
    if (!g_id) {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0 || read(fd, &g_id, sizeof(g_id)) != sizeof(g_id)) g_id = (uint64_t)now_ms() * 2654435761u;
        if (fd >= 0) close(fd);
        g_id |= 1;
    }
    if (!g_alias[0]) snprintf(g_alias, sizeof(g_alias), "Player %04X", (unsigned)(g_id & 0xffff));
    if ((f = fopen(path, "w"))) { fprintf(f, "%016llx %s\n", (unsigned long long)g_id, g_alias); fclose(f); }
    LOG("local player %016llx \"%s\" (edit %s to rename)", (unsigned long long)g_id, g_alias, path);
}

void mk_gc_net_init(const char *files_dir, void *const *handlers) {
    memcpy(g_h, handlers, sizeof(g_h));
    g_main_thread = pthread_self();
    if (g_ready) return;
    load_identity(files_dir);
    int one = 1;
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    g_disc = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0 || g_disc < 0) { LOG("socket failed: %s (INTERNET permission?)", strerror(errno)); return; }
    setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(g_sock, (struct sockaddr *)&a, sizeof(a));
    setsockopt(g_disc, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));  // a second test instance on the same device
    a.sin_port = htons(MK_DISC_PORT);
    if (bind(g_disc, (struct sockaddr *)&a, sizeof(a))) { LOG("discovery bind failed: %s", strerror(errno)); close(g_disc); g_disc = -1; }
    socklen_t sl = sizeof(a);
    getsockname(g_sock, (struct sockaddr *)&a, &sl);
    LOG("LAN matchmaking ready: data port %d, discovery port %d", ntohs(a.sin_port), MK_DISC_PORT);
    pthread_create(&g_net_thread, NULL, net_thread, NULL);
    g_ready = 1;
}

// ---- natives ----
static gc_player gc_local_player(void) {
    char ids[24];
    id_str(g_id, ids);
    gc_player p = { strdup(ids), strdup(g_alias[0] ? g_alias : "Player") };
    return p;
}
static int gc_multiplayer_restricted(void) { return 0; }

static void start_search(int min, int max, int group) {
    g_min = min < 2 ? 2 : min;
    g_max = max < g_min ? g_min : (max > MAX_PEERS + 1 ? MAX_PEERS + 1 : max);
    g_group = (uint32_t)group;
    g_searching = 1;
    g_search_start = now_ms();
    g_beacon_sent = g_join_sent = 0;
}
static void gc_start_matchmaking(int min, int max, int group, uint32_t attrs) {
    LOG("StartMatchmaking(min %d, max %d, room %d, attrs %#x)", min, max, group, attrs);
    pthread_mutex_lock(&g_mu);
    reset_match(1);
    start_search(min, max, group);
    pthread_mutex_unlock(&g_mu);
}
static void gc_add_players(int min, int max, int group, uint32_t attrs) {
    LOG("AddPlayersToMatch(min %d, max %d, room %d, attrs %#x)", min, max, group, attrs);
    pthread_mutex_lock(&g_mu);
    if (!g_in_match) { reset_match(1); start_search(min, max, group); }
    else {
        g_searching = 1;
        g_max = max > MAX_PEERS + 1 ? MAX_PEERS + 1 : (max < 2 ? 2 : max);
    }
    pthread_mutex_unlock(&g_mu);
}
static void gc_start_empty_match(int min, int max) {
    LOG("StartEmptyMatch(%d, %d): not supported on LAN", min, max);
}
static void gc_cancel_matchmaking(void) {
    LOG("CancelMatchmaking");
    pthread_mutex_lock(&g_mu);
    g_searching = 0;
    if (!g_in_match) reset_match(1);
    pthread_mutex_unlock(&g_mu);
}
static void gc_finish_matchmaking(void) {
    LOG("FinishMatchmaking");
    pthread_mutex_lock(&g_mu);
    g_searching = 0;
    pthread_mutex_unlock(&g_mu);
}
static void gc_disconnect_match(void) {
    LOG("DisconnectMatch");
    pthread_mutex_lock(&g_mu);
    g_searching = 0;
    reset_match(1);
    pthread_mutex_unlock(&g_mu);
}

// GetPlayersInMatch: malloc'd array of Player structs; the IL2CPP wrapper frees the strings and the
// array (and, being a native-sized return, reads only the first element).
static gc_player *gc_players_in_match(void) {
    pthread_mutex_lock(&g_mu);
    gc_player *r = NULL;
    if (g_npeers) {
        r = malloc(sizeof(gc_player) * (size_t)g_npeers);
        for (int i = 0; i < g_npeers; i++) {
            char ids[24];
            id_str(g_peers[i].id, ids);
            r[i].id = strdup(ids);
            r[i].alias = strdup(g_peers[i].alias);
        }
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

static void send_one(peer *p, const uint8_t *data, int len, int mode) {
    if (mode == MODE_UNRELIABLE) { send_to(&p->addr, 'U', data, (size_t)len); return; }
    size_t n = sizeof(hdr) + 4 + (size_t)len;
    out_pkt *o = malloc(sizeof(*o) + n);
    hdr h = { MAGIC, 'R', { 0 }, g_group, g_id };
    o->seq = p->tx_seq++;
    memcpy(o->buf, &h, sizeof(h));
    memcpy(o->buf + sizeof(h), &o->seq, 4);
    memcpy(o->buf + sizeof(h) + 4, data, (size_t)len);
    o->len = n;
    o->first = o->last = now_ms();
    o->next = NULL;
    out_pkt **t = &p->unacked;
    while (*t) t = &(*t)->next;
    *t = o;
    sendto(g_sock, o->buf, n, 0, (const struct sockaddr *)&p->addr, sizeof(p->addr));
}
static int gc_send(const uint8_t *data, int len, int mode) {
    if (len < 0 || len > 60000) return 0;
    pthread_mutex_lock(&g_mu);
    int ok = g_in_match && g_npeers;
    for (int i = 0; ok && i < g_npeers; i++) send_one(&g_peers[i], data, len, mode);
    pthread_mutex_unlock(&g_mu);
    return ok;
}
static int gc_send_to(const uint8_t *data, int len, const char *const *ids, int count, int mode) {
    if (len < 0 || len > 60000) return 0;
    pthread_mutex_lock(&g_mu);
    int sent = 0;
    for (int k = 0; k < count; k++) {
        unsigned long long id;
        if (!ids[k] || sscanf(ids[k], "G:%llx", &id) != 1) continue;
        peer *p = find_peer(id);
        if (p) { send_one(p, data, len, mode); sent++; }
    }
    pthread_mutex_unlock(&g_mu);
    return sent > 0;
}
static int gc_starting_room(void) { return -1; }
static void gc_noop(void) {}
static void gc_load_friends(void) {
    pthread_mutex_lock(&g_mu);
    push_ev(H_FRIENDS, NULL, 0, NULL, 0, NULL);
    pthread_mutex_unlock(&g_mu);
}
static void gc_load_recent(void) {
    pthread_mutex_lock(&g_mu);
    push_ev(H_RECENT, NULL, 0, NULL, 0, NULL);
    pthread_mutex_unlock(&g_mu);
}

// offsets of the WFGameCenter natives in UnityFramework 2.3.1 (targets of the GameCenter.GameCenter externs)
typedef struct { uintptr_t off; void *fn; const char *name; } mk_native;
const mk_native mk_gc_net_natives[] = {
    { 0x209b84, gc_local_player, "_GCLocalPlayer" }, { 0x209c3c, gc_multiplayer_restricted, "_GCIsMultiplayerRestricted" },
    { 0x209d84, gc_load_recent, "_GCLoadRecentPlayers" }, { 0x209dcc, gc_players_in_match, "_GCGetPlayersInMatch" },
    { 0x209e20, gc_noop, "_GCStartSearchingForNearbyPlayers" }, { 0x209e68, gc_noop, "_GCStopSearchingForNearbyPlayers" },
    { 0x209eb0, gc_noop, "_GCShowActivityViewController" }, { 0x20a114, gc_noop, "_GCDisplayDashboard" },
    { 0x20a15c, gc_start_matchmaking, "_GCShowMatchmaker" }, { 0x20a1d4, gc_noop, "_GCShowMatchmakerForInvite" },
    { 0x20a21c, gc_start_matchmaking, "_GCStartMatchmaking" }, { 0x20a294, gc_start_empty_match, "_GCStartEmptyMatch" },
    { 0x20a2f4, gc_add_players, "_GCAddPlayersToMatch" }, { 0x20a36c, gc_cancel_matchmaking, "_GCCancelMatchmaking" },
    { 0x20a3b4, gc_finish_matchmaking, "_GCFinishMatchmaking" }, { 0x20a3fc, gc_noop, "_GCAcceptMatchmakingInvite" },
    { 0x20a444, gc_disconnect_match, "_GCDisconnectMatch" }, { 0x20a48c, gc_send, "_GCSendMatchData" },
    { 0x20a528, gc_send_to, "_GCSendMatchDataToPlayers" }, { 0x20a6e8, gc_noop, "_GCLoadAchievements" },
    { 0x20a730, gc_noop, "_GCResetAchievements" }, { 0x20a778, gc_noop, "_GCShowAchievements" },
    { 0x20a7f0, gc_load_friends, "_GCLoadFriendPlayers" }, { 0x20a838, gc_noop, "_GCCancelInviteForPlayers" },
    { 0x20a914, gc_starting_room, "_GCGetStartingRoomNumber" },
    { 0, 0, 0 } };
