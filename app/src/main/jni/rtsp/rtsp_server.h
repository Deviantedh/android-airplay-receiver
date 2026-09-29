/*
 * rtsp_server.h  –  RTSP/1.0 server for AirPlay RAOP
 */
#pragma once

#include <stdint.h>

typedef struct rtsp_server rtsp_server_t;

typedef struct {
    void (*on_connected)   (void *ud);
    void (*on_disconnected)(void *ud);
    void (*on_track_info)  (const char *title, const char *artist,
                            const char *album, int duration_sec, void *ud);
    void (*on_progress)    (int elapsed, int duration, void *ud);
    void (*on_play_state)  (int is_playing, void *ud);
    void (*on_artwork)     (const uint8_t *data, int len,
                            const char *mime, void *ud);
    void (*on_thread_exit) (void *ud);
    void  *userdata;
} rtsp_callbacks_t;

rtsp_server_t *rtsp_server_start(uint16_t port,
                                 const uint8_t hw_addr[6],
                                 const rtsp_callbacks_t *cb);

void           rtsp_server_stop(rtsp_server_t *srv);
void           rtsp_server_send_command(rtsp_server_t *srv, int cmd);
