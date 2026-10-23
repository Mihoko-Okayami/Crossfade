FROM alpine:latest AS build

ARG BUILDKIT_SBOM_SCAN_STAGE=true

RUN apk add --no-cache build-base cmake fortify-headers git linux-headers openssl-dev openssl-libs-static

ARG SRT_VERSION=v1.5.7

ARG SRT_COMMIT=899348d8318eb9a3c5a5b6ec43c4a1114288773a

RUN git clone --depth 1 --branch "$SRT_VERSION" https://github.com/Haivision/srt.git /srt && test "$(git -C /srt rev-parse HEAD)" = "$SRT_COMMIT" && cmake -S /srt -B /srt/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/srt -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DENABLE_SHARED=OFF -DENABLE_APPS=OFF -DENABLE_ENCRYPTION=ON -DUSE_ENCLIB=openssl-evp && cmake --build /srt/build -j "$(nproc)" && cmake --install /srt/build

WORKDIR /build

COPY src ./src

ARG VERSION=dev

RUN cc -O2 -Wall -Wextra -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fPIE "-DVERSION=\"$VERSION\"" -isystem /opt/srt/include -c src/*.c && c++ -static-pie -Wl,-z,relro,-z,now -s -o crossfade *.o -L/opt/srt/lib -lsrt -lssl -lcrypto

FROM scratch

COPY --from=build /build/crossfade /crossfade

COPY licenses /licenses

ENTRYPOINT ["/crossfade"]

CMD ["/etc/crossfade/crossfade.conf"]
