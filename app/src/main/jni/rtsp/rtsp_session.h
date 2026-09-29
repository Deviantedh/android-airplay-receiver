/*
 * rtsp_session.h  –  RTSP session with Apple-Challenge authentication
 */
#pragma once

#include "rtsp_server.h"
#include "../crypto/rsakey.h"

typedef struct rtsp_session rtsp_session_t;

rtsp_session_t *rtsp_session_create(int conn_fd,
                                    const rtsp_callbacks_t *cb,
                                    rsakey_t *rsakey,
                                    const uint8_t hw_addr[6]);

void rtsp_session_handle(rtsp_session_t *sess);
void rtsp_session_close(rtsp_session_t *sess);
void rtsp_session_destroy(rtsp_session_t *sess);
void rtsp_session_send_command(rtsp_session_t *sess, int cmd);
void rtsp_session_send_cached_command(int cmd);

