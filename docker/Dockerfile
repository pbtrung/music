FROM alpine:edge AS build

RUN apk update && \
    apk --no-cache upgrade && \
    apk add --no-cache alpine-sdk git pkgconf autoconf automake bash \
        cmake apr-dev apr-util-dev jansson-dev curl-dev sqlite-dev pcre2-dev ffmpeg-dev

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

RUN apk update && \
    apk --no-cache upgrade && \
    apk --no-cache add \
        apr apr-util jansson libcurl sqlite-libs pcre2 ffmpeg snapcast-server

COPY --from=build /root/gwsocket/gwsocket /usr/bin/
COPY --from=build /root/music/build/release/music /usr/bin/

WORKDIR /script
COPY entrypoint.sh /script/
ENTRYPOINT ["/script/entrypoint.sh"]
