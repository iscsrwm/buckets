FROM golang:1.21-alpine AS builder

WORKDIR /app
COPY main.go .

RUN go build -o benchmark main.go

FROM alpine:3.19

COPY --from=builder /app/benchmark /benchmark

ENTRYPOINT ["/benchmark"]
