# Crossfade

Crossfade is a live streaming server that never remuxes: the stream OBS sends over SRT reaches RTSP and HTTPS viewers exactly as it was encoded.

## Install

```sh
mkdir -p /home/crossfade/conf /home/crossfade/data && cd /home/crossfade
curl -fsSLO https://raw.githubusercontent.com/Mihoko-Okayami/Crossfade/main/docker-compose.yml
curl -fsSLo conf/crossfade.conf https://raw.githubusercontent.com/Mihoko-Okayami/Crossfade/main/crossfade.conf
```

Generate a passphrase:

```sh
tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 64; echo
```

Put it after the stream name on the last line of `conf/crossfade.conf`, such as `mystream 3kQ9...`, then start:

```sh
docker compose up -d
```

Open UDP 8890 for OBS and TCP 554 for RTSP viewers. Every setting is explained in `crossfade.conf`.

## HTTPS

Crossfade serves MPEG-TS over plain HTTP on `127.0.0.1:8080`, for a web server to put HTTPS in front of it. With nginx, in the HTTPS `server { }` of your domain:

```nginx
location ~ ^/[A-Za-z0-9_-]+\.ts$ {
	limit_except GET { deny all; }
	send_timeout 1h;
	gzip off;
	proxy_buffer_size 16k;
	proxy_buffering off;
	proxy_buffers 4 16k;
	proxy_pass_request_headers off;
	proxy_read_timeout 1h;
	proxy_set_header X-Forwarded-For $remote_addr;
	proxy_pass http://127.0.0.1:8080;
}
```

The domain must not sit behind a proxy such as Cloudflare's.

## OBS

Settings > Stream, Service `Custom`, Server:

```
srt://<host>:8890?streamid=#!::r=<stream>,m=publish&passphrase=<passphrase>&pbkeylen=32&latency=1000000
```

`latency` is in microseconds: SRT keeps the larger of it and `[srt] latency`, which the log shows when OBS connects.

| Settings | Value |
|---|---|
| Output > Output Mode | Advanced |
| Output > Streaming > Audio Encoder | CoreAudio AAC, or FFmpeg AAC |
| Output > Streaming > Video Encoder | H.264 (x264, NVENC, QuickSync or AMD) |
| Output > Streaming > Rate Control | CBR |
| Output > Streaming > Keyframe Interval | 1 s |
| Output > Streaming > Profile | high |
| Output > Streaming > B-Frames | 0 |
| Output > Streaming > x264 Options, with x264 | bframes=0 |
| Output > Audio > Audio Bitrate | 320 kbps at most |
| Audio > Sample Rate | 48 kHz |

## Players

| Stream | URL |
|---|---|
| RTSP, over TCP only | `rtsp://<host>/<stream>` |
| MPEG-TS over HTTPS | `https://<host>/<stream>.ts` |

In VRChat, PC plays `rtspt://<host>/<stream>` and Quest the `.ts` URL.

## Offline video

While OBS is down, Crossfade can loop a short video: record 10 to 20 seconds, 64 MiB at most, in OBS with Settings > Output > Recording > Recording Format `MPEG-TS (.ts)`, with the same settings as the stream. Put it in `/home/crossfade/data` and set it in `[offline]`, which a reload applies:

```
[offline]
* 600 /data/offline.ts
```

## Running

| To | Do |
|---|---|
| Read the log | `docker logs crossfade` |
| Remove the colors from the log | `NO_COLOR=1` in the container's environment |
| Apply a change to `[streams]` or `[offline]` | `docker kill --signal=HUP crossfade` |
| Apply any other change to `crossfade.conf` | `docker restart crossfade` |
| Update | `docker compose pull && docker compose up -d` |

While a stream is live, another OBS streaming to it is refused until the first one stops or times out (`[srt] timeout`), and the offline video fills the gap between two DJs. An OBS that sends nothing usable within 5 s can be replaced.

## License

Crossfade is released under the [GNU Affero General Public License v3.0](LICENSE).

## Third-party software

The image contains [libsrt](https://github.com/Haivision/srt) (MPL-2.0), [OpenSSL](https://github.com/openssl/openssl) (Apache-2.0) and [musl](https://musl.libc.org) (MIT). Their licenses are in [`licenses/`](licenses), and in `/licenses` in the image.
