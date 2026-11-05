#include "common.h"
#include "rec.h"
#ifndef FREEL1_NO_UI
#include "ui.h"
#endif
#include "wifi.h"
#include <signal.h>

static volatile int g_stop=0;
static void on_sig(int sig){ (void)sig; g_stop=1; }

static void usage(void){
    fprintf(stderr,
      "freE1 — audio skeleton + UI preview\n"
      "  freE1 record  --dir DIR [--segment-sec N] [--device] [--omni]\n"
      "  freE1 recover --dir DIR\n"
      "  freE1 ui      [--dir DIR]\n"
      "  freE1 shots   --out DIR [--dir DIR]\n");
}

int main(int argc, char **argv){
    if(argc<2){ usage(); return 1; }
    const char *mode=argv[1];
    if(!strcmp(mode,"wifi")){
        const char *sub = argc>2 ? argv[2] : "status";
        if(!strcmp(sub,"status")){
            wifi_state st; wifi_status(&st);
            printf("available=%d connected=%d ssid=%s ip=%s\n", st.available, st.connected, st.ssid, st.ip);
            return 0;
        }
        if(!strcmp(sub,"scan")){
            wifi_ap aps[32]; int n=wifi_scan(aps,32);
            printf("%d networks\n", n);
            for(int i=0;i<n;i++) printf("  %4d dBm  %s%s\n", aps[i].signal, aps[i].ssid, aps[i].secure?" *":"");
            return 0;
        }
        if(!strcmp(sub,"connect") && argc>4){ return wifi_connect(argv[3],argv[4]); }
    }

    const char *dir="."; const char *out="shots";
    int seg=3600, dev=0, omni=0, http_port=0;
    for(int i=2;i<argc;i++){
        if(!strcmp(argv[i],"--dir") && i+1<argc) dir=argv[++i];
        else if(!strcmp(argv[i],"--out") && i+1<argc) out=argv[++i];
        else if(!strcmp(argv[i],"--segment-sec") && i+1<argc) seg=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--device")) dev=1;
        else if(!strcmp(argv[i],"--omni")) omni=1;
        else if(!strcmp(argv[i],"--http-port") && i+1<argc) http_port=atoi(argv[++i]);
        else { fprintf(stderr,"unknown arg: %s\n",argv[i]); usage(); return 1; }
    }
    if(!strcmp(mode,"record")){
        signal(SIGINT,on_sig); signal(SIGTERM,on_sig);
        rec_cfg c; memset(&c,0,sizeof c);
        c.dir=dir; c.segment_sec=seg; c.use_device=dev; c.dev="hw:2,0";
        c.nch_in=8; c.rate=48000; c.omni=omni; c.stop=&g_stop;
        return rec_run(&c);
    }
    if(!strcmp(mode,"recover")){
        int n=rec_recover(dir);
        printf("recovered %d file(s) in %s\n",n,dir);
        return 0;
    }
#ifndef FREEL1_NO_UI
    if(!strcmp(mode,"ui"))   return ui_run_x11(dir, dev, http_port);
    if(!strcmp(mode,"ui-dev")) return ui_run_device(dir, http_port);
    if(!strcmp(mode,"shots"))return ui_run_shots(dir,out);
    if(!strcmp(mode,"swipetest")) return ui_test_swipe();
#endif
    usage(); return 1;
}
