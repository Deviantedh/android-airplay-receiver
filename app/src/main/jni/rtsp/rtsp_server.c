/*
 * rtsp_server.c  –  RTSP server accepting connections and managing RSA key
 */

#include "rtsp_server.h"
#include "rtsp_session.h"
#include "../crypto/airport_key.h"
#include "../crypto/rsakey.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <android/log.h>

#define TAG "RTSP-SRV"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct rtsp_server {
    int               listen_fd;
    volatile int      running;
    pthread_t         accept_thread;
    rtsp_callbacks_t  cb;
    uint8_t           hw_addr[6];
    rsakey_t         *rsakey;

    pthread_mutex_t   session_lock;
    rtsp_session_t   *active_session;
};

typedef struct {
    int              conn_fd;
    rtsp_server_t   *srv;
} conn_arg_t;

static void *session_thread(void *arg) {
    conn_arg_t    *ca  = (conn_arg_t *)arg;
    rtsp_server_t *srv = ca->srv;

    rtsp_session_t *sess = rtsp_session_create(ca->conn_fd, &srv->cb,
                                               srv->rsakey, srv->hw_addr);
    free(ca);
    if (!sess) return NULL;

    pthread_mutex_lock(&srv->session_lock);
    if (srv->active_session) {
        LOGI("Terminating stale previous active session %p", (void*)srv->active_session);
        rtsp_session_close(srv->active_session);
    }
    srv->active_session = sess;
    pthread_mutex_unlock(&srv->session_lock);

    rtsp_session_handle(sess);

    pthread_mutex_lock(&srv->session_lock);
    if (srv->active_session == sess) srv->active_session = NULL;
    pthread_mutex_unlock(&srv->session_lock);

    rtsp_session_destroy(sess);

    if (srv->cb.on_thread_exit) {
        srv->cb.on_thread_exit(srv->cb.userdata);
    }
    return NULL;
}

static void *accept_loop(void *arg) {
    rtsp_server_t *srv = (rtsp_server_t *)arg;
    LOGI("RTSP accept loop running on fd=%d", srv->listen_fd);

    while (srv->running) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int conn = accept(srv->listen_fd, (struct sockaddr *)&peer, &plen);
        if (conn < 0) {
            if (!srv->running) break;
            if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED) continue;
            LOGE("accept error: %s (retrying in 100ms)", strerror(errno));
            usleep(100000);
            continue;
        }
        LOGI("RTSP connection from %s:%d", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

        conn_arg_t *ca = (conn_arg_t *)malloc(sizeof(*ca));
        if (!ca) { close(conn); continue; }
        ca->conn_fd = conn;
        ca->srv     = srv;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 128 * 1024);
        pthread_t tid;
        if (pthread_create(&tid, &attr, session_thread, ca) != 0) {
            LOGE("pthread_create failed: %s", strerror(errno));
            close(conn); free(ca);
        }
        pthread_attr_destroy(&attr);
    }
    LOGI("RTSP accept loop exited");
    return NULL;
}

rtsp_server_t *rtsp_server_start(uint16_t port,
                                 const uint8_t hw_addr[6],
                                 const rtsp_callbacks_t *cb)
{
    rtsp_server_t *srv = (rtsp_server_t *)calloc(1, sizeof(*srv));
    if (!srv) return NULL;
    if (cb) srv->cb = *cb;
    if (hw_addr) memcpy(srv->hw_addr, hw_addr, 6);
    srv->running = 1;
    pthread_mutex_init(&srv->session_lock, NULL);

    /* Initialize Airport Express RSA private key */
    srv->rsakey = rsakey_init_pem(AIRPORT_KEY);
    if (!srv->rsakey) {
        LOGE("Failed to parse Airport Express RSA key!");
    } else {
        LOGI("Airport Express RSA key loaded successfully");
    }

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        LOGE("socket error: %s", strerror(errno));
        if (srv->rsakey) rsakey_destroy(srv->rsakey);
        free(srv);
        return NULL;
    }
    int yes = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(srv->listen_fd, 8) < 0) {
        LOGE("bind/listen on port %d error: %s", port, strerror(errno));
        close(srv->listen_fd);
        if (srv->rsakey) rsakey_destroy(srv->rsakey);
        free(srv);
        return NULL;
    }
    LOGI("RTSP server listening on port %u", port);

    if (pthread_create(&srv->accept_thread, NULL, accept_loop, srv) != 0) {
        close(srv->listen_fd);
        if (srv->rsakey) rsakey_destroy(srv->rsakey);
        free(srv);
        return NULL;
    }
    return srv;
}

void rtsp_server_stop(rtsp_server_t *srv) {
    if (!srv) return;
    srv->running = 0;
    shutdown(srv->listen_fd, SHUT_RDWR);
    close(srv->listen_fd);
    pthread_join(srv->accept_thread, NULL);
    pthread_mutex_destroy(&srv->session_lock);
    if (srv->rsakey) rsakey_destroy(srv->rsakey);
    free(srv);
}

void rtsp_server_send_command(rtsp_server_t *srv, int cmd) {
    if (!srv) return;
    pthread_mutex_lock(&srv->session_lock);
    rtsp_session_t *sess = srv->active_session;
    pthread_mutex_unlock(&srv->session_lock);

    if (sess) {
        rtsp_session_send_command(sess, cmd);
    } else {
        /* If session was torn down after pause, still send DACP using cached client info! */
        rtsp_session_send_cached_command(cmd);
    }
}

