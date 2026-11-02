# ---- build stage ----
FROM gcc:13 AS build
WORKDIR /src
COPY Makefile ./
COPY backend ./backend
RUN make

# ---- runtime stage ----
FROM debian:bookworm-slim
RUN useradd --system --no-create-home app
WORKDIR /app
COPY --from=build /src/bin/movie ./bin/movie
COPY frontend ./frontend
USER app
ENV PORT=8080
EXPOSE 8080
CMD ["./bin/movie"]
