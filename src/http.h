#pragma once
int  http_start(const char *root, int port);
void http_stop(void);
int  http_running(void);
int  http_port(void);
int http_delete_seq(void);   /* 每次 HTTP 删除 +1，UI 用它判断要不要刷新列表 */
