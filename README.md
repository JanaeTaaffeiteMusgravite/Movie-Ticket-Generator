# Binge Buddies - Movie Ticket Booking Simulator

A C backend (plain POSIX sockets, no dependencies) plus a single-page HTML frontend.
The C server serves the page and a small JSON API, tracks seats in memory, and rejects double-bookings.

```
backend/movie.c               HTTP server, movies, showtimes, seat map, booking logic
frontend/binge_buddiesui.html Catalog, seat picker, booking summary, receipt
Makefile                      Strict build (-Wall -Wextra -Wpedantic -Werror)
Dockerfile, render.yaml       Deployment
```

## Build and run

```bash
make          # builds bin/movie
make run      # http://localhost:8080
PORT=3000 ./bin/movie   # custom port
```
Needs a C11 compiler (gcc/clang) on Linux or macOS.

## API

| Method | Path | Notes |
|---|---|---|
| GET | `/api/movies` | Movies with showtimes and free-seat counts |
| GET | `/api/shows/{id}/seats` | Layout, prices, booked seats |
| POST | `/api/book` | `{"show_id":1,"seats":["A1","A2"],"name":"Sam"}` -> 201 receipt, 400 invalid, 409 already booked |
| GET | `/api/health` | Liveness probe |

Hall: rows A-D regular ($12), rows E-F VIP ($20), 10 seats per row. Bookings live in memory and reset on restart.

## Push to GitHub

```bash
git remote add origin https://github.com/<your-username>/binge-buddies.git
git branch -M main
git push -u origin main
```

## Deploy

**Render:** New > Web Service > connect the GitHub repo. It reads `render.yaml` and builds the `Dockerfile`.
(Free instances sleep after ~15 minutes idle, so the first visit after a pause takes about a minute to wake.)
**Railway:** New Project > Deploy from GitHub repo. It detects the `Dockerfile`; the server uses the `PORT` variable it sets.
**Replit:** Import from GitHub, set the run command to `make run`.
**Docker anywhere:** `docker build -t binge-buddies . && docker run -p 8080:8080 binge-buddies`

## Author

Built by JanaeTaaffeiteMusgravite.
