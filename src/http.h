#pragma once
int  http_start(const char *root, int port);
void http_stop(void);
int  http_running(void);
int  http_port(void);
