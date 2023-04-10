FROM golang:alpine@sha256:48f336ef8366b9d6246293e3047259d0f614ee167db1869bdbc343d6e09aed8a AS builder

RUN apk --no-cache add make
COPY . /app
RUN make -C /app/grpc-service

FROM alpine@sha256:124c7d2707904eea7431fffe91522a01e5a861a624ee31d03372cc1d138a3126

COPY --from=builder /app/grpc-service/server /app/server
CMD ["/app/server", "-users", "/etc/users.json"]
