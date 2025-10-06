FROM alpine:edge AS build

RUN apk update && \
    apk --no-cache upgrade && \
    apk add --no-cache alpine-sdk git pkgconf autoconf automake bash \
        cmake curl-dev ffmpeg-dev fmt-dev spdlog-dev zstd-dev sqlite-dev \
        openssl-dev libebur128-dev taglib-dev

WORKDIR /root

RUN git clone https://github.com/allinurl/gwsocket && \
    cd gwsocket && \
    autoreconf -fiv && \
    ./configure && \
    make

RUN git clone https://github.com/pbtrung/music && \
    cd music && \
    git submodule update --init --recursive && \
    ./build_docker.sh

FROM alpine:edge

RUN echo "@testing https://dl-cdn.alpinelinux.org/alpine/edge/testing" >> /etc/apk/repositories

RUN apk update && \
    apk --no-cache upgrade && \
    apk --no-cache add libcurl ffmpeg-libs fmt zstd-libs \
        sqlite-libs spdlog snapcast-server snapweb@testing \
        libssl3 libcrypto3 libebur128 taglib

COPY --from=build /root/gwsocket/gwsocket /usr/bin/
COPY --from=build /root/music/build/release/music /usr/bin/

WORKDIR /script
COPY entrypoint.sh /script/

ENTRYPOINT ["/script/entrypoint.sh"]
CMD ["/music/snapserver.conf", "/music/music_config.json"]
