/*
 * rtsp_session.c  –  RTSP session with Apple-Challenge authentication,
 *                    SDP RSA/AES key exchange, and real ALAC audio playback.
 */

#include "rtsp_session.h"
#include "../alac/alac_decoder.h"
#include "../audio/opensl_output.h"
#include "../crypto/rsakey.h"
#include "../crypto/base64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <android/log.h>

#define TAG "RTSP-SES"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

#define MAX_HEADERS   32
#define MAX_BODY      (512 * 1024)   /* max 512 KB body for artwork */

typedef struct {
    char key[64];
    char val[448];
} hdr_t;

typedef struct {
    char    method[32];
    char    uri[256];
    int     cseq;
    hdr_t   hdrs[MAX_HEADERS];
    int     nhdr;
    uint8_t *body;
    int     body_len;
} req_t;

typedef struct {
    int              sock_rtp;
    int              sock_ctrl;
    int              sock_timing;
    volatile int     running;
    alac_decoder_t  *alac;
    osl_ctx_t       *osl;
} rtp_ctx_t;

struct rtsp_session {
    int               conn_fd;
    rtsp_callbacks_t  cb;
    rsakey_t         *rsakey;
    uint8_t           hw_addr[6];

    /* SDP negotiated params */
    uint8_t           aeskey[16];
    int               has_aeskey;
    uint8_t           aesiv[16];
    int               has_aesiv;
    char              fmtp[256];

    /* RTP audio receiver */
    rtp_ctx_t        *rtp;
    pthread_t         rtp_thread;

    /* Playback state & cached metadata */
    char              title[256];
    char              artist[256];
    char              album[256];
    int               duration_sec;
    int               elapsed_sec;
    int               playing;

    /* DACP remote control */
    char              client_ip[64];
    char              dacp_id[64];
    char              active_remote[64];
};

/* Global cache of last connected AirPlay client for DACP control after TEARDOWN */
static char     g_last_client_ip[64] = {0};
static char     g_last_active_remote[64] = {0};
static char     g_last_dacp_id[64] = {0};
static uint16_t g_last_dacp_port = 0;

/* ── I/O helpers ─────────────────────────────────────────────────────────── */

static int readline_fd(int fd, char *buf, int cap) {
    int i = 0;
    while (i < cap - 1) {
        char c;
        int r = recv(fd, &c, 1, 0);
        if (r <= 0) return r;
        if (c == '\r') continue;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

static int sendall_fd(int fd, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int r = send(fd, buf + sent, len - sent, 0);
        if (r <= 0) return -1;
        sent += r;
    }
    return sent;
}

/* ── Request parser ───────────────────────────────────────────────────────── */

static int parse_request(int fd, req_t *req) {
    memset(req, 0, sizeof(*req));
    char line[512];
    int r = readline_fd(fd, line, sizeof(line));
    if (r <= 0) return -1;

    if (sscanf(line, "%31s %255s", req->method, req->uri) != 2) return -1;

    while (1) {
        r = readline_fd(fd, line, sizeof(line));
        if (r < 0) return -1;
        if (r == 0) break;
        if (req->nhdr >= MAX_HEADERS) continue;
        char *col = strchr(line, ':');
        if (!col) continue;
        *col = '\0';
        hdr_t *h = &req->hdrs[req->nhdr++];
        strncpy(h->key, line, sizeof(h->key) - 1);
        char *v = col + 1; while (*v == ' ') v++;
        strncpy(h->val, v, sizeof(h->val) - 1);

        /* Trim trailing \r, \n, spaces */
        int vlen = strlen(h->val);
        while (vlen > 0 && (h->val[vlen-1] == '\r' || h->val[vlen-1] == '\n' || h->val[vlen-1] == ' ')) {
            h->val[--vlen] = '\0';
        }

        LOGI("  hdr: '%s' = '%s'", h->key, h->val);
        if (strcasecmp(h->key, "CSeq") == 0) req->cseq = atoi(h->val);
    }

    const char *cl = NULL;
    for (int i = 0; i < req->nhdr; i++) {
        if (strcasecmp(req->hdrs[i].key, "Content-Length") == 0) {
            cl = req->hdrs[i].val; break;
        }
    }

    if (cl) {
        int blen = atoi(cl);
        if (blen > 0 && blen <= MAX_BODY) {
            req->body = (uint8_t *)malloc(blen + 1);
            if (!req->body) return -1;
            int got = 0;
            while (got < blen) {
                r = recv(fd, req->body + got, blen - got, 0);
                if (r <= 0) { free(req->body); req->body = NULL; return -1; }
                got += r;
            }
            req->body[blen] = '\0';
            req->body_len = blen;
        }
    }
    return 0;
}

static const char *hdr_get(const req_t *req, const char *key) {
    for (int i = 0; i < req->nhdr; i++) {
        if (strcasecmp(req->hdrs[i].key, key) == 0) return req->hdrs[i].val;
    }
    return NULL;
}

/* ── Apple-Challenge / Authentication response builder ──────────────────── */

static void build_auth_headers(rtsp_session_t *s, const req_t *req,
                               char *out, size_t out_cap) {
    out[0] = '\0';
    const char *challenge = hdr_get(req, "Apple-Challenge");
    if (challenge && s->rsakey) {
        char signature[512] = {0};
        struct sockaddr_in local_sa;
        socklen_t slen = sizeof(local_sa);
        memset(&local_sa, 0, sizeof(local_sa));
        getsockname(s->conn_fd, (struct sockaddr *)&local_sa, &slen);

        uint32_t ip_bytes = local_sa.sin_addr.s_addr;
        LOGI("Apple-Challenge: '%s' from peer, local_ip=%s", challenge, inet_ntoa(local_sa.sin_addr));

        int ret = rsakey_sign(s->rsakey, signature, sizeof(signature), challenge,
                              (unsigned char *)&ip_bytes, 4,
                              s->hw_addr, 6);
        if (ret == 0) {
            /* Apple AirPlay requires removing trailing '=' base64 padding */
            char *pad = strchr(signature, '=');
            if (pad) *pad = '\0';

            snprintf(out, out_cap,
                     "Apple-Response: %s\r\n"
                     "Audio-Jack-Status: connected; type=analog\r\n",
                     signature);
            LOGI("Generated Apple-Response for client challenge: %s", signature);
            return;
        } else {
            LOGE("rsakey_sign returned %d", ret);
        }
    }
    snprintf(out, out_cap, "Audio-Jack-Status: connected; type=analog\r\n");
}

static void send_response(int fd, int cseq, int status, const char *auth_hdr, const char *extra) {
    char buf[2048];
    const char *reason = status == 200 ? "OK" : status == 451 ? "Parameter Not Understood" : "Error";
    int n = snprintf(buf, sizeof(buf),
        "RTSP/1.0 %d %s\r\n"
        "CSeq: %d\r\n"
        "Server: AirTunes/105.1\r\n"
        "%s"
        "%s"
        "Content-Length: 0\r\n"
        "\r\n",
        status, reason, cseq,
        auth_hdr ? auth_hdr : "",
        extra ? extra : "");
    sendall_fd(fd, buf, n);
    LOGI("RTSP << Status %d CSeq %d:\n%s", status, cseq, buf);
}

/* ── DAAP parser ─────────────────────────────────────────────────────────── */

static void parse_daap_chunk(const uint8_t *d, int len,
                             char *title, int title_cap,
                             char *artist, int artist_cap,
                             char *album, int album_cap,
                             int *duration_ms)
{
    int i = 0;
    while (i + 8 <= len) {
        uint32_t tag = ((uint32_t)d[i]<<24)|((uint32_t)d[i+1]<<16)|
                       ((uint32_t)d[i+2]<<8)|d[i+3];
        uint32_t vl  = ((uint32_t)d[i+4]<<24)|((uint32_t)d[i+5]<<16)|
                       ((uint32_t)d[i+6]<<8)|d[i+7];
        i += 8;

        /* If container tag (mlit, mdcl, mbcl, mccr, msts, msrv), recurse into it */
        if (tag == 0x6D6C6974 || tag == 0x6D64636C || tag == 0x6D62636C ||
            tag == 0x6D636372 || tag == 0x6D737473 || tag == 0x6D737276) {
            int sub_len = (int)vl;
            if (i + sub_len > len) sub_len = len - i;
            if (sub_len > 0) {
                parse_daap_chunk(d + i, sub_len, title, title_cap, artist, artist_cap, album, album_cap, duration_ms);
            }
            i += (int)vl;
            continue;
        }

        if (i + (int)vl > len) break;

        if (tag == 0x6D696E6D && title && title[0] == '\0') { /* minm: Title */
            int n = (int)vl < title_cap - 1 ? (int)vl : title_cap - 1;
            memcpy(title, d + i, n); title[n] = '\0';
        } else if (tag == 0x61736172 && artist && artist[0] == '\0') { /* asar: Artist */
            int n = (int)vl < artist_cap - 1 ? (int)vl : artist_cap - 1;
            memcpy(artist, d + i, n); artist[n] = '\0';
        } else if (tag == 0x6173616C && album && album[0] == '\0') { /* asal: Album */
            int n = (int)vl < album_cap - 1 ? (int)vl : album_cap - 1;
            memcpy(album, d + i, n); album[n] = '\0';
        } else if ((tag == 0x6173746D || tag == 0x61737464 || tag == 0x61736364) && duration_ms && *duration_ms == 0) { /* astm/astd/ascd: Duration */
            if (vl >= 4) {
                *duration_ms = (int)(((uint32_t)d[i]<<24)|((uint32_t)d[i+1]<<16)|
                                     ((uint32_t)d[i+2]<<8)|d[i+3]);
            }
        }

        i += (int)vl;
    }
}

/* Fallback brute-force scanner in case non-standard nested containers are used */
static void scan_daap_string(const uint8_t *d, int len, const char *tag4, char *out, int cap) {
    if (!d || len < 8 || !out || cap <= 1 || out[0] != '\0') return;
    for (int i = 0; i + 8 <= len; i++) {
        if (memcmp(d + i, tag4, 4) == 0) {
            uint32_t vl = ((uint32_t)d[i+4]<<24)|((uint32_t)d[i+5]<<16)|
                          ((uint32_t)d[i+6]<<8)|d[i+7];
            if (vl > 0 && vl < 512 && i + 8 + (int)vl <= len) {
                int n = (int)vl < cap - 1 ? (int)vl : cap - 1;
                memcpy(out, d + i + 8, n);
                out[n] = '\0';
                return;
            }
        }
    }
}

static void scan_daap_uint(const uint8_t *d, int len, const char *tag4, int *out) {
    if (!d || len < 12 || !out || *out != 0) return;
    for (int i = 0; i + 12 <= len; i++) {
        if (memcmp(d + i, tag4, 4) == 0) {
            uint32_t vl = ((uint32_t)d[i+4]<<24)|((uint32_t)d[i+5]<<16)|
                          ((uint32_t)d[i+6]<<8)|d[i+7];
            if (vl >= 4 && i + 8 + (int)vl <= len) {
                *out = (int)(((uint32_t)d[i+8]<<24)|((uint32_t)d[i+9]<<16)|
                             ((uint32_t)d[i+10]<<8)|d[i+11]);
                return;
            }
        }
    }
}

/* ── SDP parser ─────────────────────────────────────────────────────────── */

static const char *sdp_attr(const char *sdp, const char *name, char *out, int cap) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "a=%s:", name);
    const char *p = strstr(sdp, prefix);
    if (!p) return NULL;
    p += strlen(prefix);
    int i = 0;
    while (*p && *p != '\r' && *p != '\n' && i < cap - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return out;
}

/* ── RTP receiver thread ─────────────────────────────────────────────────── */

#define RTP_BUF_SIZE   8192
#define PCM_BUF_FRAMES 4096

static void *rtp_thread_fn(void *arg) {
    rtp_ctx_t *rtp = (rtp_ctx_t *)arg;
    uint8_t  buf[RTP_BUF_SIZE];
    int16_t  pcm[PCM_BUF_FRAMES * 2];

    /* Elevate thread priority to near-realtime to avoid audio stutter */
    setpriority(PRIO_PROCESS, 0, -16);

    int rcvbuf = 256 * 1024;
    setsockopt(rtp->sock_rtp, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    LOGI("RTP audio thread started (UDP port 6000, priority -16, rcvbuf=256K)");
    while (rtp->running) {
        ssize_t len = recv(rtp->sock_rtp, buf, sizeof(buf), 0);
        if (len < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        if (len < 12) continue;

        /* RTP header parsing */
        int hdr_ext     = (buf[0] & 0x10) ? 1 : 0;
        int csrc_cnt    = buf[0] & 0x0F;
        int payload_off = 12 + csrc_cnt * 4;
        if (hdr_ext && payload_off + 4 <= len) {
            int ext_words = ((buf[payload_off+2]<<8)|buf[payload_off+3]);
            payload_off += 4 + ext_words * 4;
        }
        if (payload_off >= len) continue;

        int frames = 0;
        if (alac_decoder_decode(rtp->alac,
                                buf + payload_off, len - payload_off,
                                pcm, &frames) == 0 && frames > 0) {
            osl_play(rtp->osl, pcm, frames);
        }
    }
    LOGI("RTP audio thread exited");
    return NULL;
}

/* ── Method handlers ─────────────────────────────────────────────────────── */

static void handle_options(rtsp_session_t *s, const req_t *req) {
    char auth[512];
    build_auth_headers(s, req, auth, sizeof(auth));

    send_response(s->conn_fd, req->cseq, 200, auth,
        "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n");
}

static void handle_announce(rtsp_session_t *s, const req_t *req) {
    char auth[512];
    build_auth_headers(s, req, auth, sizeof(auth));

    if (req->body && req->body_len > 0) {
        const char *sdp = (const char *)req->body;

        /* 1. Extract and decrypt AES key from RSA */
        char rsaaeskey[512] = {0};
        if (sdp_attr(sdp, "rsaaeskey", rsaaeskey, sizeof(rsaaeskey)) && s->rsakey) {
            int klen = rsakey_decrypt(s->rsakey, s->aeskey, sizeof(s->aeskey), rsaaeskey);
            if (klen == 16) {
                s->has_aeskey = 1;
                LOGI("ANNOUNCE: Successfully decrypted 16-byte AES key!");
            } else {
                LOGE("ANNOUNCE: Failed to decrypt rsaaeskey (ret=%d)", klen);
            }
        }

        /* 2. Extract AES IV from Base64 */
        char aesiv_b64[128] = {0};
        if (sdp_attr(sdp, "aesiv", aesiv_b64, sizeof(aesiv_b64))) {
            base64_t *b64 = base64_init(NULL, 1, 1);
            uint8_t *dec_iv = NULL;

            int ivlen = base64_decode(b64, &dec_iv, aesiv_b64, strlen(aesiv_b64));
            if (ivlen == 16 && dec_iv) {
                memcpy(s->aesiv, dec_iv, 16);
                s->has_aesiv = 1;
                LOGI("ANNOUNCE: Decoded 16-byte AES IV");
                free(dec_iv);
            }
            base64_destroy(b64);
        }

        /* 3. Extract ALAC fmtp parameters */
        char fmtp[256] = {0};
        if (sdp_attr(sdp, "fmtp", fmtp, sizeof(fmtp))) {
            strncpy(s->fmtp, fmtp, sizeof(s->fmtp) - 1);
            LOGI("ANNOUNCE: fmtp=%s", s->fmtp);
        }
    }

    send_response(s->conn_fd, req->cseq, 200, auth, NULL);
}

static void handle_setup(rtsp_session_t *s, const req_t *req) {
    char auth[512];
    build_auth_headers(s, req, auth, sizeof(auth));

    int rtp_port  = 6000;
    int ctrl_port = 6001;
    int time_port = 6002;

    if (!s->rtp) {
        rtp_ctx_t *rtp = (rtp_ctx_t *)calloc(1, sizeof(*rtp));

        rtp->sock_rtp = socket(AF_INET, SOCK_DGRAM, 0);
        int yes = 1;
        setsockopt(rtp->sock_rtp, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        struct sockaddr_in a = {0};
        a.sin_family      = AF_INET;
        a.sin_port        = htons(rtp_port);
        a.sin_addr.s_addr = INADDR_ANY;
        bind(rtp->sock_rtp, (struct sockaddr *)&a, sizeof(a));

        /* Bind UDP control port */
        rtp->sock_ctrl = socket(AF_INET, SOCK_DGRAM, 0);
        setsockopt(rtp->sock_ctrl, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in actrl = {0};
        actrl.sin_family      = AF_INET;
        actrl.sin_port        = htons(ctrl_port);
        actrl.sin_addr.s_addr = INADDR_ANY;
        bind(rtp->sock_ctrl, (struct sockaddr *)&actrl, sizeof(actrl));

        /* Bind UDP timing port */
        rtp->sock_timing = socket(AF_INET, SOCK_DGRAM, 0);
        setsockopt(rtp->sock_timing, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in atime = {0};
        atime.sin_family      = AF_INET;
        atime.sin_port        = htons(time_port);
        atime.sin_addr.s_addr = INADDR_ANY;
        bind(rtp->sock_timing, (struct sockaddr *)&atime, sizeof(atime));

        rtp->alac = alac_decoder_create(s->fmtp[0] ? s->fmtp : NULL,
                                        s->has_aeskey ? s->aeskey : NULL,
                                        s->has_aesiv  ? s->aesiv  : NULL);
        rtp->osl  = osl_open();
        rtp->running = 1;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 96 * 1024);
        pthread_create(&s->rtp_thread, &attr, rtp_thread_fn, rtp);
        pthread_attr_destroy(&attr);

        s->rtp = rtp;
    }

    char extra[256];
    snprintf(extra, sizeof(extra),
        "Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;"
        "control_port=%d;timing_port=%d;server_port=%d\r\n"
        "Session: 1\r\n",
        ctrl_port, time_port, rtp_port);

    send_response(s->conn_fd, req->cseq, 200, auth, extra);
}

static void handle_record(rtsp_session_t *s, const req_t *req) {
    char auth[512];
    build_auth_headers(s, req, auth, sizeof(auth));

    s->playing = 1;
    if (s->cb.on_play_state) s->cb.on_play_state(1, s->cb.userdata);

    send_response(s->conn_fd, req->cseq, 200, auth, "Audio-Latency: 11025\r\n");
}

static void handle_set_parameter(rtsp_session_t *s, const req_t *req) {
    char auth[512];
    build_auth_headers(s, req, auth, sizeof(auth));

    const char *ct = hdr_get(req, "Content-Type");
    if (ct && req->body && req->body_len > 0) {
        if (strstr(ct, "application/x-dmap-tagged")) {
            char title[256] = {0}, artist[256] = {0}, album[256] = {0};
            int dur_ms = 0;
            parse_daap_chunk(req->body, req->body_len,
                             title, sizeof(title),
                             artist, sizeof(artist),
                             album, sizeof(album),
                             &dur_ms);

            /* Fallback scan if fields are inside non-standard containers */
            scan_daap_string(req->body, req->body_len, "minm", title, sizeof(title));
            scan_daap_string(req->body, req->body_len, "asar", artist, sizeof(artist));
            scan_daap_string(req->body, req->body_len, "asal", album, sizeof(album));
            scan_daap_uint(req->body, req->body_len, "astm", &dur_ms);
            if (dur_ms == 0) {
                scan_daap_uint(req->body, req->body_len, "astd", &dur_ms);
            }
            if (dur_ms == 0) {
                scan_daap_uint(req->body, req->body_len, "ascd", &dur_ms);
            }

            /* Update accumulated track metadata without wiping valid values */
            if (title[0] != '\0') {
                if (strcmp(s->title, title) != 0) {
                    /* Track changed: reset elapsed time */
                    s->elapsed_sec = 0;
                }
                strncpy(s->title, title, sizeof(s->title) - 1);
            }
            if (artist[0] != '\0') {
                strncpy(s->artist, artist, sizeof(s->artist) - 1);
            }
            if (album[0] != '\0') {
                strncpy(s->album, album, sizeof(s->album) - 1);
            }
            if (dur_ms > 0) {
                s->duration_sec = dur_ms / 1000;
            }

            LOGI("Metadata ACCUMULATED: Title='%s' Artist='%s' Album='%s' Duration=%ds",
                 s->title, s->artist, s->album, s->duration_sec);
            if (s->cb.on_track_info) {
                s->cb.on_track_info(s->title, s->artist, s->album, s->duration_sec, s->cb.userdata);
            }
        } else if (strstr(ct, "image/")) {
            LOGI("Artwork received (%d bytes, type=%s)", req->body_len, ct);
            if (s->cb.on_artwork) {
                s->cb.on_artwork(req->body, req->body_len, ct, s->cb.userdata);
            }
        } else if (strstr(ct, "text/parameters")) {
            char *body = (char *)req->body;
            LOGI("text/parameters: '%s'", body);
            char *prog = strstr(body, "progress:");
            if (prog) {
                long long start = 0, cur = 0, end = 0;
                if (sscanf(prog + 9, " %lld/%lld/%lld", &start, &cur, &end) == 3 ||
                    sscanf(prog + 9, "%lld/%lld/%lld", &start, &cur, &end) == 3) {
                    int elapsed = (int)((cur - start) / 44100);
                    int total   = (int)((end - start) / 44100);
                    if (elapsed < 0) elapsed = 0;
                    if (total > 0) {
                        s->elapsed_sec  = elapsed;
                        s->duration_sec = total;
                        LOGI("Progress update from iPhone: %d / %d sec", elapsed, total);
                        if (s->cb.on_progress) {
                            s->cb.on_progress(elapsed, total, s->cb.userdata);
                        }
                    }
                }
            }
        }
    }

    send_response(s->conn_fd, req->cseq, 200, auth, NULL);
}

static void handle_flush(rtsp_session_t *s, const req_t *req) {
    char auth[512];
    build_auth_headers(s, req, auth, sizeof(auth));
    /* Do NOT reset elapsed_sec here so pauses do not wipe playback time! */
    if (s->rtp && s->rtp->osl) {
        osl_flush(s->rtp->osl);
    }
    send_response(s->conn_fd, req->cseq, 200, auth, NULL);
}

static void handle_pause(rtsp_session_t *s, const req_t *req) {
    char auth[512];
    build_auth_headers(s, req, auth, sizeof(auth));
    s->playing = 0;
    if (s->cb.on_play_state) s->cb.on_play_state(0, s->cb.userdata);
    send_response(s->conn_fd, req->cseq, 200, auth, NULL);
    LOGI("RTSP PAUSE handled -> paused state notified");
}

static void handle_teardown(rtsp_session_t *s, const req_t *req) {
    char auth[512];
    build_auth_headers(s, req, auth, sizeof(auth));
    s->playing = 0;
    if (s->cb.on_play_state) s->cb.on_play_state(0, s->cb.userdata);
    send_response(s->conn_fd, req->cseq, 200, auth, NULL);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

rtsp_session_t *rtsp_session_create(int conn_fd,
                                    const rtsp_callbacks_t *cb,
                                    rsakey_t *rsakey,
                                    const uint8_t hw_addr[6])
{
    rtsp_session_t *s = (rtsp_session_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->conn_fd = conn_fd;
    if (cb) s->cb = *cb;
    s->rsakey = rsakey;
    if (hw_addr) memcpy(s->hw_addr, hw_addr, 6);

    /*
     * Keepalive: Never kill the RTSP session when paused!
     * AirPlay clients idle indefinitely while paused; SO_KEEPALIVE ensures
     * stale dead connections are dropped while valid paused sessions persist.
     */
    int keepalive = 1;
    setsockopt(conn_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    if (getpeername(conn_fd, (struct sockaddr *)&peer, &plen) == 0) {
        strncpy(s->client_ip, inet_ntoa(peer.sin_addr), sizeof(s->client_ip) - 1);
        LOGI("New RTSP session from client IP: %s", s->client_ip);
    }
    return s;
}

void rtsp_session_close(rtsp_session_t *s) {
    if (!s) return;
    if (s->conn_fd >= 0) {
        LOGI("Forcing close on RTSP session conn_fd=%d", s->conn_fd);
        shutdown(s->conn_fd, SHUT_RDWR);
    }
}

void rtsp_session_handle(rtsp_session_t *s) {
    if (s->cb.on_connected) s->cb.on_connected(s->cb.userdata);

    while (1) {
        req_t req;
        if (parse_request(s->conn_fd, &req) < 0) break;
        LOGI("RTSP >> %s CSeq=%d", req.method, req.cseq);

        const char *dacp = hdr_get(&req, "DACP-ID");
        if (dacp) {
            strncpy(s->dacp_id, dacp, sizeof(s->dacp_id) - 1);
            strncpy(g_last_dacp_id, dacp, sizeof(g_last_dacp_id) - 1);
        }
        const char *ar = hdr_get(&req, "Active-Remote");
        if (ar) {
            strncpy(s->active_remote, ar, sizeof(s->active_remote) - 1);
            strncpy(g_last_active_remote, ar, sizeof(g_last_active_remote) - 1);
        }
        if (s->client_ip[0]) {
            strncpy(g_last_client_ip, s->client_ip, sizeof(g_last_client_ip) - 1);
        }

        if      (!strcmp(req.method, "OPTIONS"))       handle_options(s, &req);
        else if (!strcmp(req.method, "ANNOUNCE"))      handle_announce(s, &req);
        else if (!strcmp(req.method, "SETUP"))         handle_setup(s, &req);
        else if (!strcmp(req.method, "RECORD"))        handle_record(s, &req);
        else if (!strcmp(req.method, "SET_PARAMETER")) handle_set_parameter(s, &req);
        else if (!strcmp(req.method, "FLUSH"))         handle_flush(s, &req);
        else if (!strcmp(req.method, "PAUSE"))         handle_pause(s, &req);
        else if (!strcmp(req.method, "TEARDOWN")) {
            handle_teardown(s, &req);
            if (req.body) free(req.body);
            break;
        } else {
            char auth[512];
            build_auth_headers(s, &req, auth, sizeof(auth));
            send_response(s->conn_fd, req.cseq, 501, auth, NULL);
        }

        if (req.body) { free(req.body); req.body = NULL; }
    }

    if (s->cb.on_disconnected) s->cb.on_disconnected(s->cb.userdata);
    LOGI("RTSP session completed and closed");
}

void rtsp_session_destroy(rtsp_session_t *s) {
    if (!s) return;
    if (s->rtp) {
        s->rtp->running = 0;
        shutdown(s->rtp->sock_rtp, SHUT_RDWR);
        close(s->rtp->sock_rtp);
        if (s->rtp->sock_ctrl >= 0) {
            shutdown(s->rtp->sock_ctrl, SHUT_RDWR);
            close(s->rtp->sock_ctrl);
        }
        if (s->rtp->sock_timing >= 0) {
            shutdown(s->rtp->sock_timing, SHUT_RDWR);
            close(s->rtp->sock_timing);
        }
        osl_close(s->rtp->osl);
        alac_decoder_destroy(s->rtp->alac);
        free(s->rtp);
    }
    if (s->conn_fd >= 0) {
        close(s->conn_fd);
    }
    free(s);
}

typedef struct {
    char client_ip[64];
    char active_remote[64];
    char dacp_id[64];
    char action[32];
} dacp_req_t;

static int pkt_mdns_name(uint8_t *buf, int *len, int cap, const char *name) {
    while (*name) {
        const char *dot = strchr(name, '.');
        int llen = dot ? (int)(dot - name) : (int)strlen(name);
        if (*len + 1 + llen > cap) return -1;
        buf[(*len)++] = (uint8_t)llen;
        memcpy(buf + *len, name, llen);
        *len += llen;
        name += llen;
        if (*name == '.') name++;
    }
    buf[(*len)++] = 0;
    return 0;
}

static uint16_t resolve_dacp_port(const char *dacp_id) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return g_last_dacp_port ? g_last_dacp_port : 3689;

    /* Bind to ephemeral port: NEVER bind to 5353 and NEVER join multicast here! */
    struct sockaddr_in baddr;
    memset(&baddr, 0, sizeof(baddr));
    baddr.sin_family      = AF_INET;
    baddr.sin_port        = 0;
    baddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(sock, (struct sockaddr *)&baddr, sizeof(baddr));

    /* Query _dacp._tcp.local (PTR) + instance (SRV) */
    uint8_t q[512] = {0};
    int qlen = 12;
    q[2] = 0x00; q[3] = 0x00;
    q[4] = 0x00; q[5] = 0x02; /* 2 questions */

    pkt_mdns_name(q, &qlen, sizeof(q), "_dacp._tcp.local");
    q[qlen++] = 0x00; q[qlen++] = 0x0C; /* PTR (12) */
    q[qlen++] = 0x80; q[qlen++] = 0x01; /* IN + QU (unicast response!) */

    if (dacp_id && dacp_id[0]) {
        char target[128];
        snprintf(target, sizeof(target), "iTunes_Ctrl_%s._dacp._tcp.local", dacp_id);
        pkt_mdns_name(q, &qlen, sizeof(q), target);
        q[qlen++] = 0x00; q[qlen++] = 0x21; /* SRV (33) */
        q[qlen++] = 0x80; q[qlen++] = 0x01; /* IN + QU (unicast response!) */
    } else {
        q[5] = 0x01;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(5353);
    dest.sin_addr.s_addr = inet_addr("224.0.0.251");
    sendto(sock, q, qlen, 0, (struct sockaddr *)&dest, sizeof(dest));

    /* Also send direct unicast to client IP in case multicast is filtered */
    if (g_last_client_ip[0] != '\0') {
        struct sockaddr_in udest;
        memset(&udest, 0, sizeof(udest));
        udest.sin_family      = AF_INET;
        udest.sin_port        = htons(5353);
        inet_aton(g_last_client_ip, &udest.sin_addr);
        sendto(sock, q, qlen, 0, (struct sockaddr *)&udest, sizeof(udest));
    }

    struct pollfd pfd = { .fd = sock, .events = POLLIN };
    uint16_t found_port = 0;
    for (int t = 0; t < 5; t++) {
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0) continue;
        uint8_t resp[2048];
        ssize_t n = recv(sock, resp, sizeof(resp), 0);
        if (n < 12) continue;

        for (int i = 12; i + 10 < n; i++) {
            if (resp[i] == 0x00 && resp[i+1] == 0x21) { /* Type SRV */
                int rdata = i + 2 + 2 + 4 + 2;
                if (rdata + 6 <= n) {
                    uint16_t port = ((uint16_t)resp[rdata+4] << 8) | resp[rdata+5];
                    if (port > 1024) {
                        found_port = port;
                        break;
                    }
                }
            }
        }
        if (found_port) break;
    }
    close(sock);
    if (found_port) {
        g_last_dacp_port = found_port;
        LOGI("Resolved DACP dynamic port: %u", found_port);
        return found_port;
    }
    return g_last_dacp_port ? g_last_dacp_port : 3689;
}

static void *dacp_worker_thread(void *arg) {
    dacp_req_t *req = (dacp_req_t *)arg;
    uint16_t port = g_last_dacp_port;

    int sock = -1;
    int connected = 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_aton(req->client_ip, &addr.sin_addr);

    struct timeval tv = { .tv_sec = 1, .tv_usec = 200000 };

    /* 1. Try cached port first for immediate, zero-latency response */
    if (port > 0) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) {
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            addr.sin_port = htons(port);
            if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                connected = 1;
            } else {
                close(sock);
                sock = -1;
            }
        }
    }

    /* 2. Fall back to mDNS lookup if cached port failed or was 0 */
    if (!connected) {
        port = resolve_dacp_port(req->dacp_id);
        if (port > 0) {
            sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0) {
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                addr.sin_port = htons(port);
                if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                    connected = 1;
                }
            }
        }
    }

    if (connected && sock >= 0) {
        char buf[512];
        int n = snprintf(buf, sizeof(buf),
            "GET /ctrl-int/1/%s HTTP/1.1\r\n"
            "Host: %s:%u\r\n"
            "Active-Remote: %s\r\n"
            "%s%s%s"
            "Connection: close\r\n\r\n",
            req->action,
            req->client_ip, (unsigned int)port,
            req->active_remote,
            req->dacp_id[0] ? "DACP-ID: " : "",
            req->dacp_id[0] ? req->dacp_id : "",
            req->dacp_id[0] ? "\r\n" : "");
        send(sock, buf, n, 0);
        LOGI("DACP command '%s' sent to %s:%u", req->action, req->client_ip, port);

        /* Read HTTP response so TCP connection terminates cleanly with FIN, not RST */
        char resp[256];
        int r = recv(sock, resp, sizeof(resp) - 1, 0);
        if (r > 0) {
            resp[r] = '\0';
            char *eol = strpbrk(resp, "\r\n");
            if (eol) *eol = '\0';
            LOGI("DACP response: %s", resp);
        }
    } else {
        LOGE("Could not connect to DACP on %s (port %u)", req->client_ip, port);
    }

    if (sock >= 0) close(sock);
    free(req);
    return NULL;
}

void rtsp_session_send_cached_command(int cmd) {
    const char *action = NULL;
    if      (cmd == 1) action = "playpause";
    else if (cmd == 2) action = "previtem";
    else if (cmd == 3) action = "nextitem";

    if (!action || g_last_client_ip[0] == '\0') {
        LOGI("No cached client IP for DACP command (cmd=%d)", cmd);
        return;
    }

    dacp_req_t *req = (dacp_req_t *)malloc(sizeof(*req));
    if (!req) return;
    strncpy(req->client_ip, g_last_client_ip, sizeof(req->client_ip) - 1);
    strncpy(req->active_remote, g_last_active_remote, sizeof(req->active_remote) - 1);
    strncpy(req->dacp_id, g_last_dacp_id, sizeof(req->dacp_id) - 1);
    strncpy(req->action, action, sizeof(req->action) - 1);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    if (pthread_create(&tid, &attr, dacp_worker_thread, req) != 0) {
        free(req);
    }
    pthread_attr_destroy(&attr);
}

void rtsp_session_send_command(rtsp_session_t *s, int cmd) {
    if (s) {
        if (s->client_ip[0])     strncpy(g_last_client_ip, s->client_ip, sizeof(g_last_client_ip) - 1);
        if (s->active_remote[0]) strncpy(g_last_active_remote, s->active_remote, sizeof(g_last_active_remote) - 1);
        if (s->dacp_id[0])       strncpy(g_last_dacp_id, s->dacp_id, sizeof(g_last_dacp_id) - 1);
    }
    rtsp_session_send_cached_command(cmd);
}

