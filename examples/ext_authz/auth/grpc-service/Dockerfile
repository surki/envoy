FROM golang:alpine@sha256:1e75f9947ed25d36a785751f921727c80e21b7a9bd1ecec2f3caae85c18c1616 AS builder

RUN apk --no-cache add make
COPY . /app
RUN make -C /app/grpc-service

FROM alpine@sha256:686d8c9dfa6f3ccfc8230bc3178d23f84eeaf7e457f36f271ab1acc53015037c

COPY --from=builder /app/grpc-service/server /app/server
CMD ["/app/server", "-users", "/etc/users.json"]
