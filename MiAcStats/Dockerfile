# Multi-stage build for a lightweight static Go binary (~19 MB final image)
FROM golang:1.25-alpine AS builder

WORKDIR /src
COPY go.mod go.sum* ./
RUN go mod download

COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /out/miacstats ./cmd/miacstats

FROM alpine:3.21

RUN apk add --no-cache ca-certificates tzdata && \
    adduser -D -H -u 10001 appuser

WORKDIR /app
COPY --from=builder /out/miacstats /app/miacstats

USER appuser
EXPOSE 8080

ENTRYPOINT ["/app/miacstats"]
