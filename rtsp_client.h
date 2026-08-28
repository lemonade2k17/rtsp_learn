//
// Created by ktypc on 2026/8/25.
//

#ifndef RTSP_RELAY_RTSP_CLIENT_H
#define RTSP_RELAY_RTSP_CLIENT_H

void process_option_ack(RTSPClient *rtsp_client, int result_code, char *result_string);
void process_describe_ack(RTSPClient *rtsp_client, int result_code, char *result_string);
void process_subsession_setup();
void process_setup_ack(RTSPClient *rtsp_client, int result_code, char *result_string);
int rtsp_client_run();

#endif //RTSP_RELAY_RTSP_CLIENT_H
