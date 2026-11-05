#pragma once

typedef struct { char ssid[64]; int signal; int secure; int current; } wifi_ap;
typedef struct { char ssid[64]; char ip[32]; int connected; int available; } wifi_state;

int  wifi_available(void);
int  wifi_status(wifi_state *st);
int  wifi_scan(wifi_ap *aps, int max);       /* 阻塞 ~3 s */
int  wifi_connect(const char *ssid, const char *psk);
