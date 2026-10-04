// fodprobe — root FOD-HBM probe harness (runs as root from the module).
// Lets us iterate the onyx doze-FOD illumination sequence WITHOUT reflashing:
// it blocks on the prop `sys.fodprobe.go`; on each change it reads the command
// file /data/local/tmp/fodprobe.cmd (shell-writable) and executes it.
//
// Command file: whitespace-separated tokens, one op per line:
//   ui   <str...>      write "<str...>" to /sys/class/drm/card0-DSI-1/fod_ui_ready (before the loop)
//   uiend <str...>     write "<str...>" to fod_ui_ready (after the loop)
//   node <path> <str>  write "<str>" to an arbitrary sysfs path (before the loop)
//   doze <n>           SET_DOZE_BRIGHTNESS(n)   each 30ms during hold
//   lhbm <n>           SET_LOCAL_HBM(n)         each 30ms during hold
//   feat <id> <val>    SET_FEATURE(id,val)      each 30ms during hold
//   hold <ms>          loop duration (default 6000)
// Everything is best-effort; results (rc/errno) go to logcat tag "fodprobe".

#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <android/log.h>
#include <sys/system_properties.h>

#define LOG_TAG "fodprobe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define SET_FEATURE         0xc0304401
#define SET_DOZE_BRIGHTNESS  0x400c4402
#define SET_LOCAL_HBM        0x400c440e
#define FOD_UI_READY "/sys/class/drm/card0-DSI-1/fod_ui_ready"
#define CMD_FILE "/data/local/tmp/fodprobe.cmd"

static void writeNode(const char* path, const char* val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) { LOGI("write %s='%s' -> OPEN FAIL (errno %d)", path, val, errno); return; }
    ssize_t n = write(fd, val, strlen(val));
    LOGI("write %s='%s' -> %s", path, val, n >= 0 ? "ok" : "WRITE FAIL");
    close(fd);
}

struct Op { char kind; long a, b; char s[128]; char path[160]; };

static void runCmd() {
    FILE* f = fopen(CMD_FILE, "r");
    if (!f) { LOGI("no cmd file"); return; }
    Op pre[16]; int npre = 0;      // ops before loop (ui/node)
    Op post[8]; int npost = 0;     // ops after loop (uiend)
    int doze = -1, lhbm = -1, featId = -1, featVal = 1, hold = 6000;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char op[32] = {0}, arg1[160] = {0}, arg2[160] = {0};
        int nf = sscanf(line, "%31s %159s %159[^\n]", op, arg1, arg2);
        if (nf < 1) continue;
        if (!strcmp(op, "ui") && npre < 16)      { pre[npre].kind='u'; snprintf(pre[npre].s,128,"%s",line+3); char* nl=strchr(pre[npre].s,'\n'); if(nl)*nl=0; npre++; }
        else if (!strcmp(op, "node") && npre < 16){ pre[npre].kind='n'; snprintf(pre[npre].path,160,"%s",arg1); snprintf(pre[npre].s,128,"%s",arg2); npre++; }
        else if (!strcmp(op, "uiend") && npost < 8){ post[npost].kind='u'; snprintf(post[npost].s,128,"%s",line+6); char* nl=strchr(post[npost].s,'\n'); if(nl)*nl=0; npost++; }
        else if (!strcmp(op, "doze")) doze = atoi(arg1);
        else if (!strcmp(op, "lhbm")) lhbm = atoi(arg1);
        else if (!strcmp(op, "feat")) { featId = atoi(arg1); featVal = atoi(arg2); }
        else if (!strcmp(op, "hold")) hold = atoi(arg1);
    }
    fclose(f);
    LOGI("run: pre=%d doze=%d lhbm=%d feat=%d:%d hold=%d", npre, doze, lhbm, featId, featVal, hold);

    int fd = open("/dev/mi_display/disp_feature", O_RDWR);
    // pre ops
    for (int i = 0; i < npre; i++) {
        if (pre[i].kind == 'u') writeNode(FOD_UI_READY, pre[i].s);
        else writeNode(pre[i].path, pre[i].s);
    }
    // loop
    uint32_t db[3]={0,0,(uint32_t)(doze<0?0:doze)};
    uint8_t feat[48]; memset(feat,0,sizeof(feat)); *(uint32_t*)(feat+8)=(uint32_t)(featId<0?0:featId); *(int32_t*)(feat+12)=featVal;
    uint32_t lh[3]={0,0,(uint32_t)(lhbm<0?0:lhbm)};
    int iters = hold/30; if (iters<1) iters=1;
    for (int i=0;i<iters && fd>=0;i++){
        if (doze>=0) ioctl(fd, SET_DOZE_BRIGHTNESS, db);
        if (featId>=0) ioctl(fd, SET_FEATURE, feat);
        if (lhbm>=0) ioctl(fd, SET_LOCAL_HBM, lh);
        usleep(30*1000);
    }
    // cleanup ioctls
    if (fd>=0){ uint32_t z[3]={0,0,0}; if(lhbm>=0) ioctl(fd, SET_LOCAL_HBM, z); if(featId>=0){*(int32_t*)(feat+12)=0; ioctl(fd,SET_FEATURE,feat);} close(fd); }
    // post ops
    for (int i = 0; i < npost; i++) writeNode(FOD_UI_READY, post[i].s);
    LOGI("done");
}

int main() {
    LOGI("fodprobe started, waiting on sys.fodprobe.go");
    __system_property_set("sys.fodprobe.go", "0");
    const prop_info* pi = __system_property_find("sys.fodprobe.go");
    uint32_t serial = 0;
    while (true) {
        if (!pi) { sleep(1); pi = __system_property_find("sys.fodprobe.go"); continue; }
        uint32_t ns = serial;
        __system_property_wait(pi, serial, &ns, nullptr);
        serial = ns;
        runCmd();
    }
    return 0;
}
