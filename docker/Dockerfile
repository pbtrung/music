FROM alpine:edge AS build

RUN apk update && \
    apk --no-cache upgrade && \
    apk add --no-cache alpine-sdk git pkgconf autoconf automake bash \
        cmake curl-dev ffmpeg-dev openssl-dev fmt-dev spdlog-dev

WORKDIR /root

RUN git clone https://github.com/allinurl/gwsocket && \
    cd gwsocket && \
    autoreconf -fiv && \
    ./configure && \
    make

RUN git clone https://github.com/pbtrung/music && \
    cd music && \
    ./build.sh

FROM alpine:edge

RUN echo "@testing https://dl-cdn.alpinelinux.org/alpine/edge/testing" >> /etc/apk/repositories

RUN apk update && \
    apk --no-cache upgrade && \
    apk --no-cache add libcurl ffmpeg openssl fmt spdlog snapcast-server snapweb

COPY --from=build /root/gwsocket/gwsocket /usr/bin/
COPY --from=build /root/music/build/release/music /usr/bin/

WORKDIR /script
COPY entrypoint.sh /script/

ENTRYPOINT ["/script/entrypoint.sh"]
