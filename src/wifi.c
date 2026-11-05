#include "wifi.h"
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define IFACE "wlan0"
#define WPA "/usr/sbin/wpa_cli"

static int run(const char *cmd, char *out, size_t n){
    FILE *p = popen(cmd, "r");
    if(!p) return -1;
    size_t off=0;
    while(off+1<n){
        size_t got=fread(out+off,1,n-1-off,p);
        if(!got) break;
        off+=got;
    }
    out[off]=0;
    int rc=pclose(p);
    return rc==0 ? 0 : -1;
}

int wifi_available(void){
    char buf[64];
    if(run(WPA " -i " IFACE " ping 2>/dev/null", buf, sizeof buf)!=0) return 0;
    return strstr(buf,"PONG")!=NULL;
}

int wifi_status(wifi_state *st){
    memset(st,0,sizeof *st);
    st->available = wifi_available();
    if(!st->available) return -1;
    char buf[1024];
    if(run("/usr/sbin/wpa_cli -i " IFACE " status 2>/dev/null", buf, sizeof buf)!=0) return -1;
    char *save=NULL, *line=strtok_r(buf,"\n",&save);
    for(; line; line=strtok_r(NULL,"\n",&save)){
        if(!strncmp(line,"wpa_state=",10))      st->connected = !strncmp(line+10,"COMPLETED",9);
        else if(!strncmp(line,"ssid=",5))       snprintf(st->ssid,sizeof st->ssid,"%s", line+5);
        else if(!strncmp(line,"ip_address=",11))snprintf(st->ip,sizeof st->ip,"%s", line+11);
    }
    return 0;
}

/* wpa_cli 会把非 ASCII 转义成 \xNN，这里还原 */
static void unescape(char *s){
    char out[128]; int o=0;
    for(int i=0; s[i] && o<(int)sizeof out-1; ){
        if(s[i]=='\\' && s[i+1]=='x' && s[i+2] && s[i+3]){
            char h[3]={s[i+2],s[i+3],0};
            out[o++]=(char)strtol(h,NULL,16); i+=4;
        } else out[o++]=s[i++];
    }
    out[o]=0;
    strcpy(s,out);
}

int wifi_scan(wifi_ap *aps, int max){
    if(!wifi_available()) return 0;
    char buf[256];
    if(run("/usr/sbin/wpa_cli -i " IFACE " scan 2>/dev/null", buf, sizeof buf)!=0){
        logmsg("wifi: scan cmd failed"); return 0;
    }
    static char out[8192];
    int tries;
    for(tries=0; tries<3; tries++){
        usleep(2000000);
        if(run("/usr/sbin/wpa_cli -i " IFACE " scan_results 2>/dev/null", out, sizeof out)!=0) continue;
        /* 至少有一行数据（跳过表头）才认为扫到了 */
        char *nl=strchr(out,'\n');
        if(nl && nl[1] && nl[1]!='\n') break;
    }
    logmsg("wifi: scan raw %d bytes, tries=%d", (int)strlen(out), tries);
    int n=0; char *save=NULL;
    char *line=strtok_r(out,"\n",&save);
    if(line && strstr(line,"bssid")) line=strtok_r(NULL,"\n",&save);   /* skip header */
    for(; line && n<max; line=strtok_r(NULL,"\n",&save)){
        char sig[16]={0}, flags[128]={0}, ssid[64]={0};
        /* bssid \t freq \t signal \t flags \t ssid */
        char *f1=strchr(line,'\t'); if(!f1) continue;
        char *f2=strchr(f1+1,'\t'); if(!f2) continue;
        char *f3=strchr(f2+1,'\t'); if(!f3) continue;
        char *f4=strchr(f3+1,'\t'); if(!f4) continue;
        snprintf(sig,sizeof sig,"%.15s", f2+1);
        snprintf(flags,sizeof flags,"%.127s", f3+1);
        snprintf(ssid,sizeof ssid,"%.63s", f4+1);
        unescape(ssid);
        if(!ssid[0]) continue;
        int dup=0;
        for(int i=0;i<n;i++) if(!strcmp(aps[i].ssid,ssid)) dup=1;
        if(dup) continue;
        wifi_ap *a=&aps[n++];
        snprintf(a->ssid,sizeof a->ssid,"%s",ssid);
        a->signal=atoi(sig);
        a->secure = strstr(flags,"WPA")||strstr(flags,"WEP") ? 1 : 0;
    }
    return n;
}

int wifi_connect(const char *ssid, const char *psk){
    if(!wifi_available()) return -1;
    /* 如果目标就是当前网络，直接重连，别新建条目 */
    {
        wifi_state st; char cur[128] = {0};
        if(wifi_status(&st)==0 && st.ssid[0]) snprintf(cur,sizeof cur,"%s",st.ssid);
        if(cur[0] && strcmp(cur, ssid)==0){
            system("/usr/sbin/wpa_cli -i " IFACE " reconnect >/dev/null 2>&1");
            logmsg("wifi: same ssid, reconnect only");
            return 0;
        }
    }
    char buf[128];
    /* 已存在同 SSID 的网络就复用，避免每连一次就多一条 */
    int id = -1;
    {
        char lst[2048] = {0};
        if(run("/usr/sbin/wpa_cli -i " IFACE " list_networks 2>/dev/null", lst, sizeof lst)==0){
            char *save=NULL, *line=strtok_r(lst,"\n",&save);
            for(; line; line=strtok_r(NULL,"\n",&save)){
                if(strstr(line,"ssid") && strstr(line,"/")) continue;   /* 表头 */
                char *tab1 = strchr(line,'\t'); if(!tab1) continue;
                char *ssid_p = tab1+1;
                char *tab2 = strchr(ssid_p,'\t');
                if(tab2){ *tab2=0; }
                if(strcmp(ssid_p, ssid)==0){ id = atoi(line); break; }
            }
        }
    }
    if(id < 0){
        if(run("/usr/sbin/wpa_cli -i " IFACE " add_network 2>/dev/null", buf, sizeof buf)!=0) return -1;
        char *nl=strchr(buf,'\n'); if(nl) *nl=0;
        id = atoi(buf);
        if(id < 0) return -1;
    } else {
        logmsg("wifi: reusing existing network id %d for '%s'", id, ssid);
    }
    char cmd[512];
    snprintf(cmd,sizeof cmd,"/usr/sbin/wpa_cli -i " IFACE " set_network %d ssid '\"%s\"' 2>/dev/null", id, ssid);
    if(run(cmd,buf,sizeof buf)!=0 || !strstr(buf,"OK")) return -1;
    if(psk && psk[0]){
        snprintf(cmd,sizeof cmd,"/usr/sbin/wpa_cli -i " IFACE " set_network %d psk '\"%s\"' 2>/dev/null", id, psk);
        if(run(cmd,buf,sizeof buf)!=0 || !strstr(buf,"OK")) return -1;
    } else {
        snprintf(cmd,sizeof cmd,"/usr/sbin/wpa_cli -i " IFACE " set_network %d key_mgmt NONE 2>/dev/null", id);
        run(cmd,buf,sizeof buf);
    }
    snprintf(cmd,sizeof cmd,"/usr/sbin/wpa_cli -i " IFACE " select_network %d 2>/dev/null", id);
    run(cmd,buf,sizeof buf);

    /* 等 15 s 看是否连上；连不上就回退到已保存的网络，避免把设备留在断网状态 */
    int ok = 0;
    for(int i=0;i<25;i++){
        usleep(1000000);
        char st[512] = {0};
        if(run("/usr/sbin/wpa_cli -i " IFACE " status 2>/dev/null", st, sizeof st)==0){
            if(strstr(st,"wpa_state=COMPLETED")){ ok = 1; break; }
        }
    }
    if(ok){
        run("/usr/sbin/wpa_cli -i " IFACE " save_config 2>/dev/null", buf, sizeof buf);
        logmsg("wifi: connect '%s' ok (id %d)", ssid, id);
        return 0;
    }
    logmsg("wifi: connect '%s' FAILED, restoring (id %d)", ssid, id);
    /* 只删掉这次新建的条目；再用 reconfigure 从配置文件重载，恢复原有网络 */
    snprintf(cmd,sizeof cmd,"/usr/sbin/wpa_cli -i " IFACE " remove_network %d 2>/dev/null", id);
    run(cmd,buf,sizeof buf);
    run("/usr/sbin/wpa_cli -i " IFACE " reconfigure 2>/dev/null", buf, sizeof buf);
    usleep(500000);
    run("/usr/sbin/wpa_cli -i " IFACE " enable_network all 2>/dev/null", buf, sizeof buf);
    run("/usr/sbin/wpa_cli -i " IFACE " reconnect 2>/dev/null", buf, sizeof buf);
    return -2;
}
