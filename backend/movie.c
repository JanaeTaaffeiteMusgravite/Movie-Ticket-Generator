/*
 * movie.c - Binge Buddies backend
 *
 * A dependency-free HTTP server (POSIX sockets only) that manages movies,
 * showtimes, hall layouts and ticket reservations, and also serves the
 * frontend page. Single-threaded: each request is handled to completion
 * before the next, so seat checks and bookings can never race.
 *
 * Endpoints:
 *   GET  /                      frontend page
 *   GET  /api/health            liveness probe
 *   GET  /api/movies            movies with their showtimes
 *   GET  /api/shows/{id}/seats  hall layout + booked seats for a show
 *   POST /api/book              {"show_id":1,"seats":["A1","A2"],"name":"Sam"}
 *
 * Environment: PORT (default 8080), FRONTEND (default frontend/binge_buddiesui.html)
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ---- Hall layout: rows A-D regular, rows E-F VIP, 10 seats per row ---- */
#define ROWS 6
#define COLS 10
#define VIP_FROM_ROW 4
#define PRICE_REGULAR 12
#define PRICE_VIP 20

#define MAX_MOVIES 4
#define SHOWS_PER_MOVIE 3
#define MAX_SHOWS (MAX_MOVIES * SHOWS_PER_MOVIE)
#define MAX_PER_BOOKING 8
#define MAX_BOOKINGS 1024
#define REQ_MAX 8192
#define OUT_MAX 16384

typedef struct {
    int id;
    const char *title, *genre, *rating, *duration, *synopsis, *emoji, *c1, *c2;
} Movie;

typedef struct {
    int id, movie_id;
    const char *time;
    unsigned char taken[ROWS][COLS]; /* 1 = booked */
} Show;

typedef struct {
    char id[24];
    int show_id, count, total;
    char name[48];
    char seats[MAX_PER_BOOKING][4];
} Booking;

static const Movie MOVIES[MAX_MOVIES] = {
    {1, "Midnight Reel", "Thriller", "PG-13", "2h 04m",
     "A projectionist finds a film that predicts tomorrow's headlines.",
     "\xF0\x9F\x8E\x9E\xEF\xB8\x8F", "#5b1022", "#1a0a12"},
    {2, "Orbit Kitchen", "Comedy", "PG", "1h 41m",
     "Five strangers run a diner on a space station that never closes.",
     "\xF0\x9F\x9A\x80", "#0f4c5c", "#10202a"},
    {3, "The Last Lighthouse", "Drama", "PG-13", "2h 18m",
     "One keeper, one storm, and a radio that keeps answering back.",
     "\xF0\x9F\x8C\x8A", "#2d3a6b", "#0e1226"},
    {4, "Paper Tigers", "Action", "R", "1h 57m",
     "Origami artists turn a museum heist into a rooftop chase.",
     "\xF0\x9F\x90\xAF", "#8a4b0f", "#24140a"},
};
static const char *SHOW_TIMES[SHOWS_PER_MOVIE] = {"14:00", "18:30", "21:15"};

static Show shows[MAX_SHOWS];
static Booking bookings[MAX_BOOKINGS];
static int booking_count = 0;
static const char *html_path = "frontend/binge_buddiesui.html";
static volatile sig_atomic_t running = 1;

/* ---- Small helpers --------------------------------------------------- */

typedef struct { char *p; size_t len, cap; } Buf;

static void bprintf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bprintf(Buf *b, const char *fmt, ...) {
    va_list ap;
    int n;
    size_t room;
    if (b->len + 1 >= b->cap) return;
    room = b->cap - b->len;
    va_start(ap, fmt);
    n = vsnprintf(b->p + b->len, room, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    b->len += ((size_t)n < room) ? (size_t)n : room - 1;
}

static int send_all(int fd, const char *d, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, d, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        d += w;
        n -= (size_t)w;
    }
    return 0;
}

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 503: return "Service Unavailable";
    default:  return "Internal Server Error";
    }
}

static void respond(int fd, int status, const char *ctype, const char *body, size_t blen) {
    char head[512];
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\nCache-Control: no-store\r\n"
        "Connection: close\r\n\r\n", status, reason(status), ctype, blen);
    if (n > 0 && send_all(fd, head, (size_t)n) == 0 && blen > 0)
        send_all(fd, body, blen);
}

static void respond_json(int fd, int status, const Buf *b) {
    respond(fd, status, "application/json", b->p, b->len);
}

static void respond_error(int fd, int status, const char *msg) {
    char body[256];
    int n = snprintf(body, sizeof body, "{\"error\":\"%s\"}", msg);
    respond(fd, status, "application/json", body, n > 0 ? (size_t)n : 0);
}

/* ---- Domain logic ---------------------------------------------------- */

static void init_data(void) {
    unsigned seed = 7;
    for (int i = 0; i < MAX_SHOWS; i++) {
        shows[i].id = i + 1;
        shows[i].movie_id = i / SHOWS_PER_MOVIE + 1;
        shows[i].time = SHOW_TIMES[i % SHOWS_PER_MOVIE];
        for (int k = 0; k < 14; k++) { /* pre-book some seats so the hall looks lived-in */
            seed = seed * 1103515245u + 12345u;
            shows[i].taken[(seed >> 16) % ROWS][(seed >> 8) % COLS] = 1;
        }
    }
}

static Show *find_show(int id) {
    return (id >= 1 && id <= MAX_SHOWS) ? &shows[id - 1] : NULL;
}

static int free_seats(const Show *s) {
    int n = 0;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (!s->taken[r][c]) n++;
    return n;
}

/* Parse "A1".."F10" into zero-based row/col. Returns 1 if valid. */
static int parse_seat(const char *s, int *r, int *c) {
    char *end;
    long n;
    if (s[0] < 'A' || s[0] >= 'A' + ROWS || !isdigit((unsigned char)s[1])) return 0;
    n = strtol(s + 1, &end, 10);
    if (*end != '\0' || n < 1 || n > COLS) return 0;
    *r = s[0] - 'A';
    *c = (int)n - 1;
    return 1;
}

/* ---- Minimal JSON field extraction (flat request bodies only) -------- */

static int json_int(const char *body, const char *key, int *out_val) {
    char pat[32];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(body, pat);
    if (!p || !(p = strchr(p + strlen(pat), ':'))) return 0;
    p++;
    while (*p == ' ') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    *out_val = atoi(p);
    return 1;
}

/* Copies a safe subset of characters so the name can be echoed into JSON. */
static void json_name(const char *body, char *dst, size_t cap) {
    const char *p = strstr(body, "\"name\"");
    char tmp[48];
    size_t n = 0;
    snprintf(dst, cap, "Guest");
    if (!p || !(p = strchr(p + 6, ':'))) return;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return;
    p++;
    while (*p && *p != '"' && n + 1 < sizeof tmp) {
        if (isalnum((unsigned char)*p) || *p == ' ' || *p == '.' || *p == '-') tmp[n++] = *p;
        p++;
    }
    tmp[n] = '\0';
    if (n > 0 && n < cap) snprintf(dst, cap, "%s", tmp);
}

/* ---- Endpoint handlers ------------------------------------------------ */

static char out[OUT_MAX];

static void handle_movies(int fd) {
    Buf b = {out, 0, sizeof out};
    bprintf(&b, "[");
    for (int m = 0; m < MAX_MOVIES; m++) {
        const Movie *mv = &MOVIES[m];
        int first = 1;
        bprintf(&b, "%s{\"id\":%d,\"title\":\"%s\",\"genre\":\"%s\",\"rating\":\"%s\","
                    "\"duration\":\"%s\",\"synopsis\":\"%s\",\"emoji\":\"%s\","
                    "\"c1\":\"%s\",\"c2\":\"%s\",\"shows\":[",
                m ? "," : "", mv->id, mv->title, mv->genre, mv->rating, mv->duration,
                mv->synopsis, mv->emoji, mv->c1, mv->c2);
        for (int s = 0; s < MAX_SHOWS; s++) {
            if (shows[s].movie_id != mv->id) continue;
            bprintf(&b, "%s{\"id\":%d,\"time\":\"%s\",\"free\":%d}", first ? "" : ",",
                    shows[s].id, shows[s].time, free_seats(&shows[s]));
            first = 0;
        }
        bprintf(&b, "]}");
    }
    bprintf(&b, "]");
    respond_json(fd, 200, &b);
}

static void handle_seats(int fd, int show_id) {
    Show *s = find_show(show_id);
    Buf b = {out, 0, sizeof out};
    int first = 1;
    if (!s) { respond_error(fd, 404, "Show not found"); return; }
    bprintf(&b, "{\"show_id\":%d,\"rows\":%d,\"cols\":%d,\"vip_from_row\":%d,"
                "\"prices\":{\"regular\":%d,\"vip\":%d},\"booked\":[",
            s->id, ROWS, COLS, VIP_FROM_ROW, PRICE_REGULAR, PRICE_VIP);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (s->taken[r][c]) {
                bprintf(&b, "%s\"%c%d\"", first ? "" : ",", 'A' + r, c + 1);
                first = 0;
            }
    bprintf(&b, "]}");
    respond_json(fd, 200, &b);
}

static void handle_book(int fd, const char *body) {
    int show_id, count = 0, rr[MAX_PER_BOOKING], cc[MAX_PER_BOOKING], total = 0;
    char ids[MAX_PER_BOOKING][8], name[48], conflict[128] = "";
    const char *p;
    Show *s;
    Booking *bk;
    Buf b = {out, 0, sizeof out};

    if (!json_int(body, "show_id", &show_id)) { respond_error(fd, 400, "show_id is required"); return; }
    if (!(s = find_show(show_id))) { respond_error(fd, 404, "Show not found"); return; }
    if (!(p = strstr(body, "\"seats\"")) || !(p = strchr(p, '['))) {
        respond_error(fd, 400, "seats array is required"); return;
    }
    for (p++; *p && *p != ']'; p++) {
        size_t n = 0;
        if (*p != '"') continue;
        if (count >= MAX_PER_BOOKING) { respond_error(fd, 400, "Maximum 8 seats per booking"); return; }
        for (p++; *p && *p != '"'; p++)
            if (n + 1 < sizeof ids[0]) ids[count][n++] = *p;
        ids[count][n] = '\0';
        count++;
        if (!*p) break;
    }
    if (count == 0) { respond_error(fd, 400, "Select at least one seat"); return; }

    /* Validate everything first; only commit once every seat passes. */
    for (int i = 0; i < count; i++) {
        if (!parse_seat(ids[i], &rr[i], &cc[i])) { respond_error(fd, 400, "Invalid seat id"); return; }
        for (int j = 0; j < i; j++)
            if (rr[i] == rr[j] && cc[i] == cc[j]) { respond_error(fd, 400, "Duplicate seat in request"); return; }
        if (s->taken[rr[i]][cc[i]]) {
            size_t l = strlen(conflict);
            snprintf(conflict + l, sizeof conflict - l, " %s", ids[i]);
        }
    }
    if (conflict[0]) {
        char msg[200];
        snprintf(msg, sizeof msg, "Already booked:%s", conflict);
        respond_error(fd, 409, msg);
        return;
    }
    if (booking_count >= MAX_BOOKINGS) { respond_error(fd, 503, "Booking capacity reached"); return; }

    json_name(body, name, sizeof name);
    bk = &bookings[booking_count];
    memset(bk, 0, sizeof *bk);
    for (int i = 0; i < count; i++) {
        s->taken[rr[i]][cc[i]] = 1;
        total += (rr[i] >= VIP_FROM_ROW) ? PRICE_VIP : PRICE_REGULAR;
        snprintf(bk->seats[i], sizeof bk->seats[i], "%s", ids[i]);
    }
    bk->show_id = show_id;
    bk->count = count;
    bk->total = total;
    snprintf(bk->name, sizeof bk->name, "%s", name);
    snprintf(bk->id, sizeof bk->id, "BB-%04X%03d", (unsigned)(time(NULL) & 0xFFFF), ++booking_count);

    bprintf(&b, "{\"booking_id\":\"%s\",\"movie\":\"%s\",\"time\":\"%s\",\"name\":\"%s\","
                "\"total\":%d,\"seats\":[", bk->id, MOVIES[s->movie_id - 1].title, s->time, bk->name, total);
    for (int i = 0; i < count; i++)
        bprintf(&b, "%s\"%s\"", i ? "," : "", bk->seats[i]);
    bprintf(&b, "]}");
    respond_json(fd, 201, &b);
}

static void serve_html(int fd) {
    FILE *f = fopen(html_path, "rb");
    long size;
    char *buf;
    if (!f) { respond_error(fd, 404, "Frontend file not found"); return; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    buf = (size > 0) ? malloc((size_t)size) : NULL;
    if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size)
        respond(fd, 200, "text/html; charset=utf-8", buf, (size_t)size);
    else
        respond_error(fd, 500, "Could not read frontend file");
    free(buf);
    fclose(f);
}

/* ---- Request loop ----------------------------------------------------- */

static size_t content_length(const char *hdr) {
    for (const char *p = hdr; p; ) {
        if (strncasecmp(p, "content-length:", 15) == 0) return (size_t)strtoul(p + 15, NULL, 10);
        p = strstr(p, "\r\n");
        if (p) p += 2;
    }
    return 0;
}

static void handle_client(int fd) {
    static char req[REQ_MAX + 1];
    size_t len = 0, need = 0;
    int have_headers = 0, id;
    char method[8], path[256], tail[8], *q;
    const char *body;

    while (len < REQ_MAX) {
        ssize_t r = recv(fd, req + len, REQ_MAX - len, 0);
        if (r <= 0) break;
        len += (size_t)r;
        req[len] = '\0';
        if (!have_headers) {
            char *he = strstr(req, "\r\n\r\n");
            if (he) { have_headers = 1; need = (size_t)(he - req) + 4 + content_length(req); }
        }
        if (have_headers && len >= need) break;
    }
    if (!have_headers || len < need || sscanf(req, "%7s %255s", method, path) != 2) {
        respond_error(fd, 400, "Malformed or oversized request");
        return;
    }
    if ((q = strchr(path, '?'))) *q = '\0';
    body = strstr(req, "\r\n\r\n") + 4;

    if (strcmp(method, "OPTIONS") == 0) { respond(fd, 204, "text/plain", "", 0); return; }

    if (strcmp(path, "/api/health") == 0) respond(fd, 200, "application/json", "{\"ok\":true}", 11);
    else if (strcmp(path, "/api/movies") == 0 && strcmp(method, "GET") == 0) handle_movies(fd);
    else if (sscanf(path, "/api/shows/%d/%7s", &id, tail) == 2 && strcmp(tail, "seats") == 0 &&
             strcmp(method, "GET") == 0) handle_seats(fd, id);
    else if (strcmp(path, "/api/book") == 0) {
        if (strcmp(method, "POST") == 0) handle_book(fd, body);
        else respond_error(fd, 405, "Use POST");
    }
    else if ((strcmp(path, "/") == 0 || strcmp(path, "/binge_buddiesui.html") == 0) &&
             strcmp(method, "GET") == 0) serve_html(fd);
    else respond_error(fd, 404, "Not found");
}

static void on_signal(int sig) { (void)sig; running = 0; }

int main(void) {
    const char *port_env = getenv("PORT"), *html_env = getenv("FRONTEND");
    int port = port_env ? atoi(port_env) : 8080, one = 1;
    struct sockaddr_in addr;
    struct sigaction sa;
    int srv;

    if (html_env) html_path = html_env;
    if (port < 1 || port > 65535) { fprintf(stderr, "Invalid PORT\n"); return 1; }
    init_data();

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; /* no SA_RESTART: accept() returns so we can exit */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(srv, 64) < 0) {
        perror("bind/listen");
        return 1;
    }
    printf("Binge Buddies listening on http://0.0.0.0:%d\n", port);
    fflush(stdout);

    while (running) {
        struct timeval tv = {5, 0}; /* a stalled client can't block everyone */
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        handle_client(fd);
        close(fd);
    }
    close(srv);
    puts("Shutting down.");
    return 0;
}
