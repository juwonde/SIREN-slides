/* Minimal CSP v1 node. Ping(1)/echo(10); with --cmd the CMD service (port 11: NOOP,
 * time get/set from the OBC, and the FM reports: health, ground reset, boot slot, FM events). No reboot/memory service.
 * Linked to pinned, unmodified libcsp v1.6 (LGPL-2.1-or-later).
 * --selftest uses the real CFP fragmenter/reassembler with an in-memory driver;
 * it is not an external CAN interoperability measurement.
 */
#define _GNU_SOURCE                 /* clock_adjtime */
#include <csp/csp.h>
#include <csp/csp_rtable.h>
#include <csp/drivers/can_socketcan.h>
#include <csp/interfaces/csp_if_can.h>
#include <linux/rtc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

/* Optional bounded data-object sink, CSP application port 20. Not FTP.
 * Explicit operator-selected fresh /run file only; no names from the wire.
 * One session, one OBC, 32 MiB maximum. Sender restart is supported by offset
 * queries; process/power-loss restart is not certified by this prototype.
 */
static int sink_fd = -1, sink_enabled;
static uint32_t sink_session, sink_total, sink_offset, last_offset, last_crc;
static unsigned sink_src, unique_blocks, duplicates, rejected;

static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | p[3];
}
static void put32(uint8_t *p, uint32_t x) {
    p[0]=(uint8_t)(x>>24); p[1]=(uint8_t)(x>>16); p[2]=(uint8_t)(x>>8); p[3]=(uint8_t)x;
}
static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n) {
    while (n--) {
        crc ^= *p++;
        for (unsigned b=0; b<8; ++b) crc=(crc>>1) ^ (0xedb88320u & (0u-(crc&1u)));
    }
    return crc;
}
static uint32_t object_crc(const uint8_t *p, size_t n) {
    return ~crc_update(crc_update(~0u,p,16),p+20,n-20);
}
static void object_receive(csp_packet_t *packet) {
    uint32_t session = 0, status = 1;
    if (packet->length >= 20) {
        uint8_t *p = packet->data;
        session=get32(p+4);
        uint32_t offset=get32(p+8), total=get32(p+12), crc=get32(p+16);
        size_t bytes=packet->length-20;
        if (bytes<=480 && total>0 && total<=32u*1024u*1024u && crc==object_crc(p,packet->length)) {
            if (!memcmp(p,"ASQ1",4) && bytes==0) {
                status=(session==sink_session && packet->id.src==sink_src && total==sink_total) ? 0u : 3u;
            } else if (!memcmp(p,"ASD1",4) && bytes>0 && offset<=total && bytes<=total-offset) {
                if (!sink_session && offset==0 && session) {
                    sink_session=session; sink_total=total; sink_src=packet->id.src;
                }
                if (session!=sink_session || total!=sink_total || packet->id.src!=sink_src) status=3;
                else if (unique_blocks && offset==last_offset && offset+bytes==sink_offset && crc==last_crc) {
                    ++duplicates; status=0;
                } else if (offset!=sink_offset) status=2;
                else if (sink_fd<0) status=5;
                else {
                    size_t done=0;
                    while (done<bytes) {
                        ssize_t wrote=write(sink_fd,p+20+done,bytes-done);
                        if (wrote<0 && errno==EINTR) continue;
                        if (wrote<=0) { fprintf(stderr,"sink write failed\n"); exit(1); }
                        done+=(size_t)wrote;
                    }
                    last_offset=offset; last_crc=crc; sink_offset+=(uint32_t)bytes;
                    ++unique_blocks; status=0;
                    if (sink_offset==sink_total) {
                        if (fsync(sink_fd) || close(sink_fd)) { fprintf(stderr,"sink sync failed\n"); exit(1); }
                        sink_fd=-1;
                        printf("CSP_OBJECT_COMPLETE bytes=%u blocks=%u duplicates=%u rejected=%u source=%u session=%u\n",
                               sink_offset,unique_blocks,duplicates,rejected,sink_src,sink_session);
                        fflush(stdout);
                    }
                }
            }
        }
    }
    if (status) ++rejected;
    memcpy(packet->data,"ASA1",4); put32(packet->data+4,session);
    put32(packet->data+8,sink_offset); put32(packet->data+12,status); packet->length=16;
}

static csp_iface_t test_iface;
static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned frame_count;
static csp_can_driver_tx_t native_tx;
static unsigned tx_gap_us;
static int trace_commands;
static pthread_mutex_t protocol_tx_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned command_waiting;
static const char *background_control;
static uint64_t bg_frames, bg_bits, bg_bytes, bg_packets, bg_failures, cmd_frames, cmd_bits;

static uint64_t mono_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) exit(1);
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}
#include "csp-mixed-test.h"
/* Exact classic extended data-frame length, including CRC15 stuffing and IFS.
 * This is calculated successful-data wire time, not an error-frame analyzer.
 */
static unsigned wire_bits(uint32_t id,const uint8_t *data,unsigned dlc) {
    unsigned char bits[118];unsigned n=0;uint16_t crc=0;
    bits[n++]=0;
    for (int b=28;b>=18;--b) bits[n++]=(id>>b)&1u;
    bits[n++]=1;bits[n++]=1;
    for (int b=17;b>=0;--b) bits[n++]=(id>>b)&1u;
    bits[n++]=0;bits[n++]=0;bits[n++]=0;
    for (int b=3;b>=0;--b) bits[n++]=(dlc>>b)&1u;
    for (unsigned i=0;i<dlc;++i) for (int b=7;b>=0;--b) bits[n++]=(data[i]>>b)&1u;
    for (unsigned i=0;i<n;++i) {
        unsigned feedback=((crc>>14)&1u)^bits[i];crc=(crc<<1)&0x7fffu;
        if (feedback) crc^=0x4599u;
    }
    for (int b=14;b>=0;--b) bits[n++]=(crc>>b)&1u;
    unsigned count=0,last=2,run=0;
    for (unsigned i=0;i<n;++i) {
        ++count;
        if (bits[i]==last) ++run;else {last=bits[i];run=1;}
        if (run==5) {++count;last^=1u;run=1;}
    }
    return count+13u;
}

static uint32_t random32(uint32_t *state) {
    *state^=*state<<13;*state^=*state>>17;*state^=*state<<5;return *state;
}

static void *background_sender(void *unused) {
    (void)unused;
    unsigned phase=0,load=0;uint32_t random_state=1,sequence=0;
    uint64_t due=0,last_report=0;
    for (;;) {
        unsigned next_phase,next_load;uint32_t seed;
        FILE *control=fopen(background_control,"r");
        if (control) {
            int fields=fscanf(control,"%u %u %u",&next_phase,&next_load,&seed);fclose(control);
            if (fields==3 && next_phase<=65535 && next_load<=1000 && seed) {
                if (next_phase!=phase) {phase=next_phase;random_state=seed;due=mono_ns();}
                load=next_load;
            }
        }
        uint64_t now=mono_ns();
        if (now-last_report>=200000000ULL) {
            pthread_mutex_lock(&count_lock);
            printf("CSP_BG_STATE mono_ns=%llu phase=%u load=%u frames=%llu wire_bits=%llu bytes=%llu packets=%llu failures=%llu cmd_frames=%llu cmd_bits=%llu\n",
                   (unsigned long long)now,phase,load,(unsigned long long)bg_frames,(unsigned long long)bg_bits,
                   (unsigned long long)bg_bytes,(unsigned long long)bg_packets,(unsigned long long)bg_failures,(unsigned long long)cmd_frames,(unsigned long long)cmd_bits);
            pthread_mutex_unlock(&count_lock);fflush(stdout);last_report=now;
        }
        if (!load || command_waiting) {usleep(1000);continue;}
        if (load<1000 && now<due) {usleep((useconds_t)((due-now>1000000ULL)?1000:(due-now)/1000+1));continue;}
        unsigned bytes=8u+random32(&random_state)%505u;
        static const uint8_t nodes[]={2,3,4,5,6,7,8,9,28};
        uint8_t destination=nodes[random32(&random_state)%9u];
        csp_packet_t *packet=csp_buffer_get(bytes);
        if (!packet) {pthread_mutex_lock(&count_lock);++bg_failures;pthread_mutex_unlock(&count_lock);usleep(1000);continue;}
        packet->length=(uint16_t)bytes;memcpy(packet->data,"DUM1",4);put32(packet->data+4,sequence++);
        for (unsigned i=8;i<bytes;++i) packet->data[i]=(uint8_t)random32(&random_state);
        pthread_mutex_lock(&protocol_tx_lock);
        int result=csp_sendto(CSP_PRIO_LOW,destination,10,54,0,packet,1000);
        pthread_mutex_unlock(&protocol_tx_lock);
        pthread_mutex_lock(&count_lock);
        if (result==CSP_ERR_NONE) {bg_bytes+=bytes;++bg_packets;}
        else {++bg_failures;csp_buffer_free(packet);}
        pthread_mutex_unlock(&count_lock);
        /* 1000 is explicit saturation; lower steps use conservative wire budgets.
         * Random packet size/address/data produce varying amount and burst length.
         * No accumulated catch-up burst after scheduler delays.
         */
        unsigned full=(bytes+6u+7u)/8u;
        if (load<1000) due=now+(uint64_t)full*160u*1000000000ULL/(500000u*load/1000u);
    }
    return NULL;
}

static int command_send(csp_conn_t *connection,csp_packet_t *packet) {
    int is_command=trace_commands && packet->length==8 && !memcmp(packet->data,"CMD1",4);
    int is_mixed=mixed_enabled && packet->length==32 && !memcmp(packet->data,"MIA1",4);
    uint32_t sequence=(is_command || is_mixed)?get32(packet->data+4):0;
    uint64_t start=mono_ns();
    __sync_add_and_fetch(&command_waiting,1);
    pthread_mutex_lock(&protocol_tx_lock);
    int result=csp_send(connection,packet,1000);
    pthread_mutex_unlock(&protocol_tx_lock);
    __sync_sub_and_fetch(&command_waiting,1);
    if (is_command) {
        printf("CSP_CMD_TX mono_ns=%llu sequence=%u success=%d software_enqueue_ns=%llu\n",
               (unsigned long long)mono_ns(),sequence,result,(unsigned long long)(mono_ns()-start));fflush(stdout);
    }
    if (is_mixed) {
        printf("MIX_TX mono_ns=%llu sequence=%u success=%d software_enqueue_ns=%llu\n",
            (unsigned long long)mono_ns(),sequence,result,(unsigned long long)(mono_ns()-start));fflush(stdout);
    }
    return result;
}

/* CMD service, CSP port 11 (--cmd OBC_ID): satellite-style telecommand (TC) / telemetry (TM).
 *   TC "TC01" seq:u32 cmd:u16 len:u16 payload[len] crc:u32
 *   TM "TM01" seq:u32 cmd|0x8000:u16 status:u16 len:u16 payload[len] crc:u32
 * Big-endian, CRC-32/IEEE over every byte before it; tools/csp_cmd_protocol.py is the OBC side
 * and checks the same known answers. Only TCs from the OBC node are executed. A retried
 * TIME_SET/TIME_ADJUST (same seq and CRC) gets the cached TM and is not executed again. Time
 * steps are refused (BUSY) while a session holds the FPGA device lock. At start one BOOT event
 * TM (seq 0) goes to the OBC.
 */
#define CMD_PORT 11
enum { CMD_NOOP=0x0001, CMD_TIME_GET=0x0010, CMD_TIME_SET=0x0011, CMD_TIME_ADJUST=0x0012, CMD_FM_HEALTH=0x0020,
       CMD_FM_GROUND_RESET=0x0021, CMD_BOOT_SLOT=0x0022, CMD_EVENT_BOOT=0xC001, CMD_EVENT_FM=0xC020, CMD_EVENT_NN=0xC030 };
enum { ST_OK, ST_FORMAT, ST_UNKNOWN, ST_LENGTH, ST_RANGE, ST_FAILED, ST_SOURCE, ST_BUSY };
#define CMD_TIME_MIN 1704067200LL /* 2024-01-01 UTC */
#define CMD_TIME_MAX 4102444800LL /* 2100-01-01 UTC */
#define CMD_MAX_PAYLOAD 256
#define CMD_TM_MAX 96
static int cmd_enabled, cmd_syslog, cmd_rtc_disabled, cmd_time_state;
static unsigned cmd_obc;
static uint8_t cmd_boot_id[16];
static uint32_t cmd_accepted, cmd_rejected;
static const char *cmd_lock_path = "/run/lock/f-astra-fpga-device.lock";
/* Last set time (environment F_ASTRA_TIME_FILE, the SD card): written after every time step so that a board without
 * RTC backup power starts from it (f-astra-time-keep moves the clock forward only). Unset: not kept. */
static const char *cmd_time_file;
static int real_get_clock(struct timespec *t) { return clock_gettime(CLOCK_REALTIME,t); }
/* Every step is relative and applied atomically by the kernel (ADJ_SETOFFSET); TIME_SET steps by
 * its target minus the current time. A read-add-set from user space loses the time between the
 * read and the set (measured over CAN on 2026-10-13: a zero step kept realtime minus monotonic
 * to 1 us). */
static int real_shift_clock(int64_t offset_ns) {
    struct timex t; memset(&t,0,sizeof t);
    int64_t sec=offset_ns/1000000000LL, nsec=offset_ns%1000000000LL;
    if (nsec<0) { nsec+=1000000000LL; --sec; }
    t.modes=ADJ_SETOFFSET | ADJ_NANO;
    t.time.tv_sec=(time_t)sec; t.time.tv_usec=(suseconds_t)nsec;     /* nanoseconds with ADJ_NANO */
    return clock_adjtime(CLOCK_REALTIME,&t)<0;
}
/* The self-test replaces the system clock with a fake one. */
static int (*cmd_get_clock)(struct timespec *) = real_get_clock;
static int (*cmd_shift_clock)(int64_t) = real_shift_clock;
static uint8_t cmd_last_tm[CMD_TM_MAX];
static size_t cmd_last_len;
static uint32_t cmd_last_seq, cmd_last_crc;
static unsigned cmd_last_src;

static void put16(uint8_t *p, uint16_t x) { p[0]=(uint8_t)(x>>8); p[1]=(uint8_t)x; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0]<<8) | p[1]); }
static void put64(uint8_t *p, uint64_t x) { put32(p,(uint32_t)(x>>32)); put32(p+4,(uint32_t)x); }
static uint64_t get64(const uint8_t *p) { return ((uint64_t)get32(p)<<32) | get32(p+4); }
static void put_time(uint8_t *p, const struct timespec *t) { put64(p,(uint64_t)t->tv_sec); put32(p+8,(uint32_t)t->tv_nsec); }
static void cmd_now(struct timespec *t) { if (cmd_get_clock(t)) exit(1); }
static void cmd_log(const char *format, ...) {
    char line[256];
    va_list args;
    va_start(args,format); vsnprintf(line,sizeof line,format,args); va_end(args);
    printf("%s\n",line); fflush(stdout);
    if (cmd_syslog) syslog(LOG_INFO,"%s",line);
}
static void cmd_read_boot_id(void) {
    char text[64]; unsigned n=0;
    FILE *file=fopen("/proc/sys/kernel/random/boot_id","r");
    if (!file) return;
    if (fgets(text,sizeof text,file)) {
        for (const char *c=text; *c && n<32; ++c) {
            int v=(*c>='0'&&*c<='9')?*c-'0':(*c>='a'&&*c<='f')?*c-'a'+10:-1;
            if (v<0) continue;
            cmd_boot_id[n/2]=(uint8_t)(cmd_boot_id[n/2] | (n&1u ? v : v<<4)); ++n;
        }
    }
    fclose(file);
}
/* RTC reading: 0 ok, 1 absent, 2 read failed. */
static int rtc_read(uint64_t *seconds) {
    if (cmd_rtc_disabled) return 1;
    int fd=open("/dev/rtc0",O_RDONLY);
    if (fd<0) return errno==ENOENT ? 1 : 2;
    struct rtc_time r; memset(&r,0,sizeof r);
    int failed=ioctl(fd,RTC_RD_TIME,&r); close(fd);
    if (failed) return 2;
    struct tm t; memset(&t,0,sizeof t);
    t.tm_sec=r.tm_sec; t.tm_min=r.tm_min; t.tm_hour=r.tm_hour;
    t.tm_mday=r.tm_mday; t.tm_mon=r.tm_mon; t.tm_year=r.tm_year;
    time_t s=timegm(&t);
    if (s==(time_t)-1) return 2;
    *seconds=(uint64_t)s; return 0;
}
/* Write the system time to the RTC on the next second boundary (the RTC keeps whole
 * seconds): 0 written, 2 write failed, 3 absent. */
static int rtc_write(void) {
    if (cmd_rtc_disabled) return 3;
    int fd=open("/dev/rtc0",O_RDONLY);
    if (fd<0) return errno==ENOENT ? 3 : 2;
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME,&now)) { close(fd); return 2; }
    struct timespec wait={0,1000000000L-now.tv_nsec};
    nanosleep(&wait,NULL);
    struct tm t; time_t s;
    if (clock_gettime(CLOCK_REALTIME,&now) || (s=now.tv_sec, !gmtime_r(&s,&t))) { close(fd); return 2; }
    struct rtc_time r; memset(&r,0,sizeof r);
    r.tm_sec=t.tm_sec; r.tm_min=t.tm_min; r.tm_hour=t.tm_hour;
    r.tm_mday=t.tm_mday; r.tm_mon=t.tm_mon; r.tm_year=t.tm_year;
    int failed=ioctl(fd,RTC_SET_TIME,&r); close(fd);
    return failed ? 2 : 0;
}
/* A session holds the FPGA device lock (flock) while it runs; a time step takes it too, without
 * waiting, for the few microseconds of the clock call. Returns the lock to close, -1 when the lock
 * directory does not exist (no session can run), -2 when a session holds it. Reading /proc/locks
 * instead took about 34 ms on this kernel (measured over CAN), time a read-add-set would lose. */
static int fpga_lock(void) {
    int fd=open(cmd_lock_path,O_RDONLY|O_CREAT|O_CLOEXEC,0644);
    if (fd<0) return errno==ENOENT ? -1 : -2;
    if (flock(fd,LOCK_EX|LOCK_NB)) { close(fd); return -2; }
    return fd;
}
/* State block (48 bytes): NOOP reply and BOOT event. */
static void cmd_state(uint8_t *p) {
    struct timespec now; uint64_t rtc_s=0;
    cmd_now(&now);
    int rtc=rtc_read(&rtc_s);
    memcpy(p,cmd_boot_id,16); put64(p+16,mono_ns()); p[24]=(uint8_t)cmd_time_state; p[25]=(uint8_t)rtc;
    put16(p+26,0); put32(p+28,cmd_accepted); put32(p+32,cmd_rejected); put_time(p+36,&now);
}
static size_t cmd_finish(uint8_t *tm, uint32_t seq, uint16_t command, uint16_t status, uint16_t length) {
    memcpy(tm,"TM01",4); put32(tm+4,seq); put16(tm+8,command); put16(tm+10,status); put16(tm+12,length);
    put32(tm+14+length,~crc_update(~0u,tm,14u+length));
    return 18u+length;
}
static size_t cmd_boot_tm(uint8_t *tm) {
    cmd_state(tm+14);
    return cmd_finish(tm,0,CMD_EVENT_BOOT,ST_OK,48);
}
/* The set time, in whole seconds, to cmd_time_file, written in place and synced (the SD card's FAT is written only here
 * and at shutdown, never periodically). No rename: on FAT a rename cut by a reset left two directory entries with one
 * short name (2026-10-14); a reader ignores a missing or empty file. Failures are logged, never fatal. */
static void cmd_keep_time(void) {
    char text[32];
    struct timespec now;
    if (!cmd_time_file) return;
    cmd_now(&now);
    int n=snprintf(text,sizeof text,"%lld\n",(long long)now.tv_sec);
    int fd=open(cmd_time_file,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644), ok=fd>=0;
    if (ok) ok=write(fd,text,(size_t)n)==n && !fsync(fd);
    if (fd>=0 && close(fd)) ok=0;
    cmd_log("CMD_TIME_KEPT file=%s seconds=%lld ok=%d",cmd_time_file,(long long)now.tv_sec,ok);
}
/* TIME_SET (to sec.nsec) or TIME_ADJUST (by offset_ns) from the 16-byte TC payload p. Reply
 * payload (28 bytes): before, after, RTC result. The target must lie in [2024, 2100) and fit the
 * board's time_t (32-bit on the ARM build). */
static uint16_t cmd_step(const uint8_t *p, int relative, uint8_t *out) {
    uint32_t flags=get32(p+(relative ? 8 : 12));
    if ((flags & ~1u) || (relative && get32(p+12))) return ST_RANGE;
    if (!relative && (get64(p)>=(uint64_t)CMD_TIME_MAX || get32(p+8)>=1000000000u)) return ST_RANGE;
    int lock=fpga_lock();
    if (lock==-2) return ST_BUSY;
    struct timespec before, after, target;
    int64_t offset=(int64_t)get64(p), sec, nsec;
    cmd_now(&before);
    if (relative) { sec=(int64_t)before.tv_sec+offset/1000000000LL; nsec=before.tv_nsec+offset%1000000000LL; }
    else { sec=(int64_t)get64(p); nsec=get32(p+8); }
    if (nsec<0) { nsec+=1000000000LL; --sec; } else if (nsec>=1000000000LL) { nsec-=1000000000LL; ++sec; }
    target.tv_sec=(time_t)sec; target.tv_nsec=(long)nsec;
    uint16_t status=ST_OK;
    uint64_t call=0;
    if (sec<CMD_TIME_MIN || sec>=CMD_TIME_MAX || (int64_t)target.tv_sec!=sec) status=ST_RANGE;
    else {
        if (!relative) offset=(sec-(int64_t)before.tv_sec)*1000000000LL+(nsec-before.tv_nsec);
        call=mono_ns();
        if (cmd_shift_clock(offset)) status=ST_FAILED;
        call=mono_ns()-call;
        cmd_now(&after);
    }
    if (lock>=0) close(lock);
    if (status) return status;
    cmd_time_state=1;
    cmd_keep_time();
    int rtc=(flags & 1u) ? rtc_write() : 1;
    put_time(out,&before); put_time(out+12,&after); out[24]=(uint8_t)rtc; out[25]=(uint8_t)cmd_time_state; put16(out+26,0);
    cmd_log("CMD_TIME_STEP %s before=%lld.%09ld after=%lld.%09ld call_us=%llu rtc=%d",relative ? "adjust" : "set",
            (long long)before.tv_sec,before.tv_nsec,(long long)after.tv_sec,after.tv_nsec,
            (unsigned long long)(call/1000u),rtc);
    return ST_OK;
}
/* Execute one TC (n bytes from node src, read at rx) into tm; returns the TM length. */
/* FM reports (F4, SIR01600). The FM service (f_astra_fm_service.c) appends the OBC's FM events to FM_DIR/events.jsonl
 * and copies every health record to FM_DIR/health.json; this node forwards each new event line as TM 0xC020 (seq =
 * the event's number since the node started) and answers TC 0x0020 from the copy and the SD boot state
 * (docs/te0715-sd-fallback-boot.md). TC 0x0021 ("GRST") leaves a ground reset request on the SD card that the FM
 * service's next Initial applies once; TC 0x0022 selects the boot slot of the next boot (the ground's boot-slot). */
static const char *fm_dir="/run/f-astra-fm";
static const char *fm_ground_file="/media/sd-mmcblk0p1/f-astra-fm/ground-reset";
static const char *boot_state_file="/media/sd-mmcblk0p1/f-astra-boot.txt";
static uint32_t fm_events_sent;
static const char *const fm_items[16]={"L0-H1-01","L0-H1-02","L0-H1-03","L0-H3-02","L0-H4-05","L1-S2-01","L1-S2-02",
    "L1-S3-01","L1-S3-02","L0-H4-02","L0-H4-04","L2-G1-02","L0-H4-01","L0-H4-03","L1-S1-01","L1-S1-02"};
static size_t read_text(const char *path, char *buf, size_t size) {
    size_t n=0; FILE *f=fopen(path,"r");
    if (f) { n=fread(buf,1,size-1,f); fclose(f); }
    buf[n]=0; return n;
}
/* "key":"text" and "key":number of a flat JSON line (the FM service's own records only) */
static void json_text(const char *json, const char *key, char *out, size_t size) {
    char pattern[48]; snprintf(pattern,sizeof pattern,"\"%s\":\"",key);
    const char *at=strstr(json,pattern); size_t n=0;
    if (at) for (at+=strlen(pattern); *at && *at!='"' && n+1<size; ++at) out[n++]=*at;
    out[n]=0;
}
static long long json_number(const char *json, const char *key, long long fallback) {
    char pattern[48]; snprintf(pattern,sizeof pattern,"\"%s\":",key);
    const char *at=json ? strstr(json,pattern) : NULL;
    return at ? strtoll(at+strlen(pattern),NULL,10) : fallback;
}
/* 64 B: mode (0 no record, 1 STANDBY, 2 SAFEHOLD, 3 other) · safehold · boot slot (0 primary, 1 fallback, 255 unknown)
 * · boot tries (255 unknown) · safehold cause 16 · FM events sent u32 · health record time s u64 · 16 recovery counts u16 */
static void fm_health(uint8_t *p) {
    char json[4096], text[32], path[300], boot[256];
    struct stat st;
    memset(p,0,64);
    snprintf(path,sizeof path,"%s/health.json",fm_dir);
    if (read_text(path,json,sizeof json) && !stat(path,&st)) {
        json_text(json,"mode",text,sizeof text);
        p[0]=!strcmp(text,"STANDBY") ? 1 : !strcmp(text,"SAFEHOLD") ? 2 : 3;
        p[1]=(uint8_t)json_number(json,"safehold",0);
        json_text(json,"safehold_cause",text,sizeof text);
        memcpy(p+4,text,strlen(text)<16 ? strlen(text) : 16);
        put64(p+24,(uint64_t)st.st_mtime);
        const char *counts=strstr(json,"\"recovery_counts\":{");
        for (unsigned i=0;i<16;++i) put16(p+32+2*i,(uint16_t)json_number(counts,fm_items[i],0));
    }
    p[2]=p[3]=0xff;
    if (read_text(boot_state_file,boot,sizeof boot)) {
        const char *slot=strstr(boot,"fa_slot="), *tries=strstr(boot,"fa_tries=");
        p[2]=!slot || !strncmp(slot+8,"primary",7) ? 0 : 1;    /* boot.scr: no slot is the primary */
        if (tries) p[3]=(uint8_t)strtoul(tries+9,NULL,10);
    }
    put32(p+20,fm_events_sent);
}
static int write_atomic(const char *path, const char *text) {
    char tmp[320]; snprintf(tmp,sizeof tmp,"%s.tmp",path);
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644);
    if (fd<0) return -1;
    size_t n=strlen(text);
    int ok=write(fd,text,n)==(ssize_t)n && !fsync(fd);
    close(fd);
    if (!ok || rename(tmp,path)) { unlink(tmp); return -1; }
    return 0;
}
/* TM 0xC020, 48 B: event number u32 · batch i32 · item 12 · event 16 · read time (s u64, ns u32) */
static size_t fm_event_tm(const char *line, uint32_t number, const struct timespec *now, uint8_t *tm) {
    uint8_t *p=tm+14; char text[32];
    memset(p,0,48);
    put32(p,number);
    put32(p+4,(uint32_t)(int32_t)json_number(line,"batch",-1));
    json_text(line,"item",text,sizeof text); memcpy(p+8,text,strlen(text)<12 ? strlen(text) : 12);
    json_text(line,"event",text,sizeof text); memcpy(p+20,text,strlen(text)<16 ? strlen(text) : 16);
    put_time(p+36,now);
    return cmd_finish(tm,number,CMD_EVENT_FM,ST_OK,48);
}
/* new complete lines of FM_DIR/events.jsonl, at most 5 per 200 ms (a campaign's burst stays a fraction of the bus) */
static void *fm_forwarder(void *unused) {
    char path[300], line[4096];
    long offset=0; ino_t inode=0;
    (void)unused;
    snprintf(path,sizeof path,"%s/events.jsonl",fm_dir);
    for (;;) {
        struct stat st;
        if (!stat(path,&st)) {
            if (st.st_ino!=inode || st.st_size<offset) { inode=st.st_ino; offset=0; }
            FILE *f=st.st_size>offset ? fopen(path,"r") : NULL;
            if (f && !fseek(f,offset,SEEK_SET)) {
                for (unsigned sent=0; sent<5 && fgets(line,sizeof line,f); ++sent) {
                    size_t len=strlen(line);
                    if (!len || line[len-1]!='\n') break;            /* still being written: the next poll */
                    offset+=(long)len;
                    uint8_t tm[CMD_TM_MAX]; struct timespec now; cmd_now(&now);
                    size_t n=fm_event_tm(line,++fm_events_sent,&now,tm);
                    csp_packet_t *packet=csp_buffer_get(n);
                    if (!packet) continue;
                    memcpy(packet->data,tm,n); packet->length=(uint16_t)n;
                    if (csp_sendto(CSP_PRIO_NORM,(uint8_t)cmd_obc,CMD_PORT,CMD_PORT,CSP_O_NONE,packet,1000)!=CSP_ERR_NONE)
                        csp_buffer_free(packet);
                }
            }
            if (f) fclose(f);
        }
        usleep(200000);
    }
    return NULL;
}

/* NN decisions (F-ASTRA 6.214.0; user 11/06: the OBC gets the BUS and PD decisions over CAN, the Comp packet goes to the
 * transceiver over UART). f-astra-decide appends one 16-byte record an orbit to NN_DIR/results.bin; this node forwards
 * every new record as TM 0xC030 (32 B: record number u32 . the record . read time s u64, ns u32) and writes the number
 * of records sent to NN_DIR/sent (the orbit window waits for it before the poweroff). */
static const char *nn_dir="/run/f-astra-nn";
#define NN_RECORD 16
static uint32_t nn_records_sent;
static size_t nn_event_tm(const uint8_t *record, uint32_t number, const struct timespec *now, uint8_t *tm) {
    uint8_t *p=tm+14;
    memset(p,0,32);
    put32(p,number);
    memcpy(p+4,record,NN_RECORD);
    put_time(p+20,now);
    return cmd_finish(tm,number,CMD_EVENT_NN,ST_OK,32);
}
static void *nn_forwarder(void *unused) {
    char path[300], sent[300], text[16];
    long offset=0; ino_t inode=0;
    (void)unused;
    snprintf(path,sizeof path,"%s/results.bin",nn_dir);
    snprintf(sent,sizeof sent,"%s/sent",nn_dir);
    for (;;) {
        struct stat st;
        if (!stat(path,&st)) {
            if (st.st_ino!=inode || st.st_size<offset) { inode=st.st_ino; offset=0; }
            int fd=st.st_size>=offset+NN_RECORD ? open(path,O_RDONLY|O_CLOEXEC) : -1;
            if (fd>=0) {
                uint8_t record[NN_RECORD];
                while (pread(fd,record,NN_RECORD,offset)==NN_RECORD) {
                    offset+=NN_RECORD;
                    uint8_t tm[CMD_TM_MAX]; struct timespec now; cmd_now(&now);
                    size_t n=nn_event_tm(record,++nn_records_sent,&now,tm);
                    csp_packet_t *packet=csp_buffer_get(n);
                    if (!packet) { cmd_log("CMD_NN_RESULT failed=no_buffer number=%u",nn_records_sent); continue; }
                    memcpy(packet->data,tm,n); packet->length=(uint16_t)n;
                    if (csp_sendto(CSP_PRIO_NORM,(uint8_t)cmd_obc,CMD_PORT,CMD_PORT,CSP_O_NONE,packet,1000)!=CSP_ERR_NONE) {
                        csp_buffer_free(packet);
                        cmd_log("CMD_NN_RESULT failed=send number=%u",nn_records_sent);
                        continue;
                    }
                    snprintf(text,sizeof text,"%u\n",nn_records_sent);
                    if (write_atomic(sent,text)) cmd_log("CMD_NN_RESULT sent_file_failed number=%u",nn_records_sent);
                    cmd_log("CMD_NN_RESULT number=%u",nn_records_sent);
                }
                close(fd);
            }
        }
        usleep(50000);
    }
    return NULL;
}

static size_t cmd_process(const uint8_t *tc, size_t n, unsigned src, const struct timespec *rx, uint8_t *tm) {
    uint32_t seq=0, tc_crc=0; uint16_t command=0x7fff, status=ST_OK, length=0;
    uint8_t *out=tm+14;
    uint64_t mono=mono_ns();
    if (n>=12 && !memcmp(tc,"TC01",4)) { seq=get32(tc+4); command=get16(tc+8)&0x7fffu; }
    if (n>=4) tc_crc=get32(tc+n-4);
    size_t payload=n>=16 ? n-16 : 0;
    const uint8_t *p=tc+12;
    int step=command==CMD_TIME_SET || command==CMD_TIME_ADJUST;
    if (n<16 || n>16+CMD_MAX_PAYLOAD || memcmp(tc,"TC01",4) || get16(tc+10)!=payload
        || (get16(tc+8)&0x8000u) || tc_crc!=~crc_update(~0u,tc,n-4)) status=ST_FORMAT;
    else if (src!=cmd_obc) status=ST_SOURCE;
    else if (step && cmd_last_len && src==cmd_last_src && seq==cmd_last_seq && tc_crc==cmd_last_crc) {
        memcpy(tm,cmd_last_tm,cmd_last_len);
        cmd_log("CMD_TC_DUPLICATE seq=%u command=0x%04x",seq,command);
        return cmd_last_len;
    } else if (command==CMD_NOOP) {
        if (payload) status=ST_LENGTH; else { cmd_state(out); length=48; }
    } else if (command==CMD_TIME_GET) {
        /* any length: the OBC pads the TC to the TM's frame count */
        uint64_t rtc_s=0; int rtc=rtc_read(&rtc_s);
        put_time(out,rx); memcpy(out+24,cmd_boot_id,16); put64(out+40,mono);
        out[48]=(uint8_t)cmd_time_state; out[49]=(uint8_t)rtc; put16(out+50,0); put64(out+52,rtc ? 0 : rtc_s);
        length=60;
    } else if (step) {
        status=payload==16 ? cmd_step(p,command==CMD_TIME_ADJUST,out) : ST_LENGTH;
    } else if (command==CMD_FM_HEALTH) {
        if (payload) status=ST_LENGTH; else { fm_health(out); length=64; }
    } else if (command==CMD_FM_GROUND_RESET) {
        char text[64]; snprintf(text,sizeof text,"obc seq=%u\n",seq);
        if (payload!=4) status=ST_LENGTH;
        else if (memcmp(p,"GRST",4)) status=ST_RANGE;
        else if (write_atomic(fm_ground_file,text)) status=ST_FAILED;
        else { memcpy(out,"GRST",4); length=4; cmd_log("CMD_FM_GROUND_RESET_REQUESTED seq=%u",seq); }
    } else if (command==CMD_BOOT_SLOT) {
        if (payload!=4) status=ST_LENGTH;
        else if (p[0]>1 || p[1] || p[2] || p[3]) status=ST_RANGE;
        else if (write_atomic(boot_state_file,p[0] ? "fa_slot=fallback\nfa_tries=0\n" : "fa_slot=primary\nfa_tries=0\n"))
            status=ST_FAILED;
        else { memcpy(out,p,4); length=4; cmd_log("CMD_BOOT_SLOT slot=%s",p[0] ? "fallback" : "primary"); }
    } else status=ST_UNKNOWN;
    if (step && status==ST_OK) length=28;
    if (status) length=0;
    if (status==ST_OK) ++cmd_accepted; else ++cmd_rejected;
    if (status==ST_OK && command==CMD_TIME_GET) {
        struct timespec tx; cmd_now(&tx); put_time(out+12,&tx);
    }
    size_t tm_len=cmd_finish(tm,seq,(uint16_t)(command|0x8000u),status,length);
    if (step && status!=ST_FORMAT && status!=ST_SOURCE) {
        memcpy(cmd_last_tm,tm,tm_len); cmd_last_len=tm_len; cmd_last_seq=seq; cmd_last_crc=tc_crc; cmd_last_src=src;
    }
    if (command!=CMD_TIME_GET || status)
        cmd_log("CMD_TC seq=%u command=0x%04x source=%u status=%u",seq,command,src,status);
    return tm_len;
}
static void cmd_reply(csp_conn_t *connection, csp_packet_t *packet, const struct timespec *rx) {
    uint8_t tm[CMD_TM_MAX];
    size_t n=cmd_process(packet->data,packet->length,packet->id.src,rx,tm);
    memcpy(packet->data,tm,n); packet->length=(uint16_t)n;
    if (!command_send(connection,packet)) csp_buffer_free(packet);
}
static void cmd_boot_event(void) {
    uint8_t tm[CMD_TM_MAX];
    size_t n=cmd_boot_tm(tm);
    csp_packet_t *packet=csp_buffer_get(n);
    if (!packet) { cmd_log("CMD_BOOT_EVENT failed=no_buffer"); return; }
    memcpy(packet->data,tm,n); packet->length=(uint16_t)n;
    int result=csp_sendto(CSP_PRIO_NORM,(uint8_t)cmd_obc,CMD_PORT,CMD_PORT,CSP_O_NONE,packet,1000);
    if (result!=CSP_ERR_NONE) csp_buffer_free(packet);
    cmd_log("CMD_BOOT_EVENT obc=%u result=%d",cmd_obc,result);
}

static struct timespec fake_clock;
static unsigned fake_sets;
static int fake_get(struct timespec *t) { *t=fake_clock; return 0; }
static int fake_shift(int64_t offset_ns) {
    int64_t ns=(int64_t)fake_clock.tv_nsec+offset_ns%1000000000LL;
    fake_clock.tv_sec+=(time_t)(offset_ns/1000000000LL);
    if (ns<0) { ns+=1000000000LL; --fake_clock.tv_sec; } else if (ns>=1000000000LL) { ns-=1000000000LL; ++fake_clock.tv_sec; }
    fake_clock.tv_nsec=(long)ns; ++fake_sets; return 0;
}
static size_t cmd_tc(uint8_t *tc, uint32_t seq, uint16_t command, const uint8_t *payload, uint16_t length) {
    memcpy(tc,"TC01",4); put32(tc+4,seq); put16(tc+8,command); put16(tc+10,length);
    if (length) memcpy(tc+12,payload,length);
    put32(tc+12+length,~crc_update(~0u,tc,12u+length));
    return 16u+length;
}
#define CMD_CHECK(c) do { if (!(c)) { fprintf(stderr,"CMD selftest failed line %d\n",__LINE__); return 1; } } while (0)
/* CMD service: the OBC-side known answers (csp_cmd_protocol.py), every status, the duplicate
 * cache and the BUSY guard against a fake clock; NOOP and TIME_GET also through CSP/CFP. */
static int cmd_selftest(unsigned address) {
    static const uint8_t noop_kat[]={0x54,0x43,0x30,0x31,0x01,0x02,0x03,0x04,0x00,0x01,0x00,0x00,0x73,0x58,0x21,0xb5};
    static const uint8_t set_kat[]={0x54,0x43,0x30,0x31,0x00,0x00,0x00,0x07,0x00,0x11,0x00,0x10,0x00,0x00,0x00,0x00,
        0x6a,0xc9,0xa5,0x80,0x00,0x00,0x00,0x05,0x00,0x00,0x00,0x00,0xca,0xd6,0xfd,0xef};
    static const uint8_t reply_kat[]={0x54,0x4d,0x30,0x31,0x00,0x00,0x00,0x07,0x80,0x11,0x00,0x00,0x00,0x1c,0x00,0x00,
        0x00,0x00,0x6a,0xc9,0x65,0x22,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x6a,0xc9,0xa5,0x80,0x00,0x00,0x00,0x05,
        0x01,0x01,0x00,0x00,0x73,0x33,0xb4,0x43};
    uint8_t tm[CMD_TM_MAX], tc[300], reply[512], payload[64];
    struct timespec rx={1,0};
    char lock_path[]="f-astra-cmd-selftest-XXXXXX";       /* in the working directory, never the device lock */
    int lock=mkstemp(lock_path);
    CMD_CHECK(lock>=0);
    cmd_lock_path=lock_path;
    char time_path[]="f-astra-cmd-selftest-time";
    cmd_time_file=time_path;
    cmd_enabled=1; cmd_obc=address; cmd_rtc_disabled=1;
    cmd_get_clock=fake_get; cmd_shift_clock=fake_shift;
    fake_clock.tv_sec=1791583522; fake_clock.tv_nsec=1;
    size_t n=cmd_process(noop_kat,sizeof noop_kat,address,&rx,tm);
    CMD_CHECK(n==66 && !memcmp(tm,"TM01",4) && get32(tm+4)==0x01020304u && get16(tm+8)==0x8001 && get16(tm+10)==ST_OK
              && get16(tm+12)==48 && tm[14+24]==0 && get32(tm+62)==~crc_update(~0u,tm,62));
    n=cmd_process(set_kat,sizeof set_kat,address,&rx,tm);
    CMD_CHECK(n==sizeof reply_kat && !memcmp(tm,reply_kat,n) && fake_sets==1 && cmd_time_state==1);
    {                                                                   /* the set time kept, whole seconds */
        char kept[32]={0}; FILE *f=fopen(time_path,"r");
        CMD_CHECK(f && fgets(kept,sizeof kept,f) && !strcmp(kept,"1791600000\n"));
        fclose(f); unlink(time_path); cmd_time_file=NULL;
    }
    n=cmd_process(set_kat,sizeof set_kat,address,&rx,tm);                 /* a retry: cached TM, not executed */
    CMD_CHECK(n==sizeof reply_kat && !memcmp(tm,reply_kat,n) && fake_sets==1);
    put64(payload,1500000000ULL); put32(payload+8,0); put32(payload+12,0);
    n=cmd_process(tc,cmd_tc(tc,8,CMD_TIME_ADJUST,payload,16),address,&rx,tm);
    CMD_CHECK(get16(tm+10)==ST_OK && fake_clock.tv_sec==1791600001 && fake_clock.tv_nsec==500000005 && fake_sets==2);
    put64(payload,(uint64_t)-1600000000000000000LL);                    /* about -51 years: before 2024 */
    n=cmd_process(tc,cmd_tc(tc,9,CMD_TIME_ADJUST,payload,16),address,&rx,tm);
    CMD_CHECK(n==18 && get16(tm+10)==ST_RANGE && fake_sets==2);
    const uint32_t bad_set[][3]={{1000,0,0},{1791600000,1000000000,0},{1791600000,0,2},{(uint32_t)CMD_TIME_MAX,0,0}};
    for (unsigned i=0;i<4;++i) {
        put64(payload,bad_set[i][0]); put32(payload+8,bad_set[i][1]); put32(payload+12,bad_set[i][2]);
        n=cmd_process(tc,cmd_tc(tc,10+i,CMD_TIME_SET,payload,16),address,&rx,tm);
        CMD_CHECK(get16(tm+10)==ST_RANGE && get32(tm+4)==10+i && get16(tm+8)==0x8011);
    }
    memcpy(tc,set_kat,sizeof set_kat); tc[20]^=1;                         /* CRC */
    CMD_CHECK(cmd_process(tc,sizeof set_kat,address,&rx,tm)==18 && get16(tm+10)==ST_FORMAT);
    CMD_CHECK(cmd_process(tc,cmd_tc(tc,20,0x0077,NULL,0),address,&rx,tm)==18 && get16(tm+10)==ST_UNKNOWN);
    CMD_CHECK(cmd_process(tc,cmd_tc(tc,21,CMD_NOOP,payload,4),address,&rx,tm)==18 && get16(tm+10)==ST_LENGTH);
    CMD_CHECK(cmd_process(tc,cmd_tc(tc,22,0x8001,NULL,0),address,&rx,tm)==18 && get16(tm+10)==ST_FORMAT);
    CMD_CHECK(cmd_process(noop_kat,sizeof noop_kat,address+1,&rx,tm)==18 && get16(tm+10)==ST_SOURCE);
    CMD_CHECK(!flock(lock,LOCK_EX));                                      /* a session runs */
    put64(payload,1791700000ULL); put32(payload+8,0); put32(payload+12,0);
    n=cmd_process(tc,cmd_tc(tc,30,CMD_TIME_SET,payload,16),address,&rx,tm);
    CMD_CHECK(get16(tm+10)==ST_BUSY && fake_sets==2);
    flock(lock,LOCK_UN);
    n=cmd_process(tc,cmd_tc(tc,31,CMD_TIME_SET,payload,16),address,&rx,tm);
    CMD_CHECK(get16(tm+10)==ST_OK && fake_sets==3 && fake_clock.tv_sec==1791700000);
    close(lock); unlink(lock_path);
    cmd_lock_path="/run/lock/f-astra-fpga-device.lock";
    {                                                                   /* FM reports against files in the cwd */
        char health[]="f-astra-cmd-selftest-health.json", boot[]="f-astra-cmd-selftest-boot.txt",
             ground[]="f-astra-cmd-selftest-ground", line[600];
        FILE *f=fopen(health,"w");
        CMD_CHECK(f && fputs("{\"schema\":\"f-astra/fm-health-telemetry/v1\",\"mode\":\"SAFEHOLD\",\"cause\":\"L1-S3-02\","
                             "\"recovery_counts\":{\"L0-H1-01\":2,\"L1-S3-02\":3,\"L1-S1-02\":4},\"safehold\":1,"
                             "\"safehold_cause\":\"L1-S3-02\",\"boot_pending\":0}\n",f)>=0);
        fclose(f);
        f=fopen(boot,"w"); CMD_CHECK(f && fputs("fa_slot=primary\nfa_tries=2\n",f)>=0); fclose(f);
        fm_dir="."; boot_state_file=boot; fm_ground_file=ground;
        CMD_CHECK(!rename(health,"health.json"));
        n=cmd_process(tc,cmd_tc(tc,50,CMD_FM_HEALTH,NULL,0),address,&rx,tm);
        CMD_CHECK(n==82 && get16(tm+10)==ST_OK && get16(tm+12)==64 && tm[14]==2 && tm[15]==1 && tm[16]==0 && tm[17]==2
                  && !memcmp(tm+18,"L1-S3-02",8) && get16(tm+14+32)==2 && get16(tm+14+32+16)==3 && get16(tm+14+62)==4);
        CMD_CHECK(cmd_process(tc,cmd_tc(tc,51,CMD_FM_GROUND_RESET,(const uint8_t *)"GRSX",4),address,&rx,tm)==18
                  && get16(tm+10)==ST_RANGE && access(ground,F_OK));
        n=cmd_process(tc,cmd_tc(tc,52,CMD_FM_GROUND_RESET,(const uint8_t *)"GRST",4),address,&rx,tm);
        CMD_CHECK(n==22 && get16(tm+10)==ST_OK && read_text(ground,line,sizeof line) && !strcmp(line,"obc seq=52\n"));
        memset(payload,0,4); payload[0]=1;
        n=cmd_process(tc,cmd_tc(tc,53,CMD_BOOT_SLOT,payload,4),address,&rx,tm);
        CMD_CHECK(n==22 && get16(tm+10)==ST_OK && read_text(boot,line,sizeof line) && !strcmp(line,"fa_slot=fallback\nfa_tries=0\n"));
        payload[0]=2;
        CMD_CHECK(cmd_process(tc,cmd_tc(tc,54,CMD_BOOT_SLOT,payload,4),address,&rx,tm)==18 && get16(tm+10)==ST_RANGE);
        struct timespec at={1791600000,5};
        n=fm_event_tm("{\"t_ns\":1,\"batch\":7,\"event\":\"recovery_limit\",\"item\":\"L1-S3-02\",\"detail\":\"\"}\n",9,&at,tm);
        CMD_CHECK(n==66 && get16(tm+8)==CMD_EVENT_FM && get32(tm+4)==9 && get32(tm+14)==9 && get32(tm+18)==7
                  && !memcmp(tm+22,"L1-S3-02\0",9) && !memcmp(tm+34,"recovery_limit\0",15) && get64(tm+50)==1791600000ULL);
        unlink("health.json"); unlink(boot); unlink(ground);
        fm_dir="/run/f-astra-fm"; boot_state_file="/media/sd-mmcblk0p1/f-astra-boot.txt";
        fm_ground_file="/media/sd-mmcblk0p1/f-astra-fm/ground-reset";
    }
    n=cmd_boot_tm(tm);
    CMD_CHECK(n==66 && get32(tm+4)==0 && get16(tm+8)==CMD_EVENT_BOOT && tm[14+24]==1 && get32(tm+14+28)==7
              && get32(tm+14+32)==13 && get32(tm+62)==~crc_update(~0u,tm,62));   /* with the FM report TCs */
    /* through CSP/CFP: NOOP, and TIME_GET padded to the TM's frame count */
    int bytes=csp_transaction(CSP_PRIO_NORM,(uint8_t)address,CMD_PORT,2000,(void *)noop_kat,sizeof noop_kat,reply,-1);
    CMD_CHECK(bytes==66 && get16(reply+10)==ST_OK && get32(reply+4)==0x01020304u);
    memset(payload,0,sizeof payload);
    n=cmd_tc(tc,40,CMD_TIME_GET,payload,62);
    bytes=csp_transaction(CSP_PRIO_NORM,(uint8_t)address,CMD_PORT,2000,tc,(int)n,reply,-1);
    CMD_CHECK(n==78 && bytes==78 && get16(reply+8)==0x8010 && get16(reply+10)==ST_OK && get16(reply+12)==60
              && get64(reply+14+12)==1791700000ULL && reply[14+48]==1 && get32(reply+74)==~crc_update(~0u,reply,74));
    cmd_enabled=0; cmd_get_clock=real_get_clock; cmd_shift_clock=real_shift_clock;
    return 0;
}

static int paced_tx(void *driver, uint32_t id, const uint8_t *data, uint8_t dlc) {
    int result = native_tx(driver, id, data, dlc);
    if (result==CSP_ERR_NONE && background_control) {
        pthread_mutex_lock(&count_lock);
        if (((id>>19)&31u)==1u) {++cmd_frames;cmd_bits+=wire_bits(id,data,dlc);}
        else {++bg_frames;bg_bits+=wire_bits(id,data,dlc);}
        pthread_mutex_unlock(&count_lock);
    }
    if (result == CSP_ERR_NONE && tx_gap_us) usleep(tx_gap_us);
    return result;
}

static int test_tx(void *unused, uint32_t id, const uint8_t *data, uint8_t dlc) {
    (void)unused;
    if (id > 0x1fffffffu || dlc > 8) return CSP_ERR_INVAL;
    pthread_mutex_lock(&count_lock); ++frame_count; pthread_mutex_unlock(&count_lock);
    return csp_can_rx(&test_iface, id, data, dlc, NULL);
}

static void *mixed_connection(void *argument) {
    csp_conn_t *connection=argument;csp_packet_t *packet;
    while((packet=csp_read(connection,50))!=NULL) {
        unsigned port=csp_conn_dport(connection);
        if(port==10 || port==20) {
            mixed_receive(packet);
            if(command_send(connection,packet))continue;
        }
        csp_buffer_free(packet);
    }
    csp_close(connection);return NULL;
}
static void *server(void *unused) {
    (void)unused;
    csp_socket_t *socket = csp_socket(CSP_SO_NONE);
    if (!socket || csp_bind(socket, CSP_ANY) || csp_listen(socket, 10)) {
        fprintf(stderr, "CSP server setup failed\n"); exit(1);
    }
    for (;;) {
        csp_conn_t *connection = csp_accept(socket, 1000);
        if (!connection) continue;
        if(mixed_enabled) {
            /* Diagnostic mixed mode must not starve data port 20 while a
             * continuously active CMD port 10 connection occupies the server.
             * libcsp conn_max bounds connection workers; app state is locked. */
            pthread_t handler;
            if(pthread_create(&handler,NULL,mixed_connection,connection)) {
                fprintf(stderr,"MIX_FATAL connection_worker_create_failed\n");exit(1);
            }
            pthread_detach(handler);continue;
        }
        csp_packet_t *packet;
        while ((packet = csp_read(connection, 50)) != NULL) {
            unsigned port = csp_conn_dport(connection);
            if (cmd_enabled && port == CMD_PORT) {
                struct timespec rx;
                cmd_now(&rx);
                cmd_reply(connection, packet, &rx);
                continue;
            }
            if (mixed_enabled && (port==10 || port==20)) {
                mixed_receive(packet);
                if (command_send(connection,packet)) continue;
                csp_buffer_free(packet); continue;
            }
            if (trace_commands && port == 10 && packet->length == 8 && !memcmp(packet->data,"CMD1",4)) {
                struct timespec stamp;
                if (clock_gettime(CLOCK_MONOTONIC,&stamp)) exit(1);
                printf("CSP_CMD_RX mono_ns=%llu source=%u sequence=%u\n",
                       (unsigned long long)stamp.tv_sec*1000000000ULL+(unsigned long long)stamp.tv_nsec,
                       packet->id.src,get32(packet->data+4));
                fflush(stdout);
            }
            if (port == 20 && sink_enabled) {
                object_receive(packet);
                if (command_send(connection, packet)) continue;
                csp_buffer_free(packet); continue;
            }
            if ((port == 1 || port == 10) && command_send(connection, packet)) continue;
            csp_buffer_free(packet);
        }
        csp_close(connection);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if(argc==2 && !strcmp(argv[1],"--mono")) {printf("%llu\n",(unsigned long long)mono_ns());return 0;}
    int selftest = argc == 2 && !strcmp(argv[1], "--selftest");
    char *end = NULL;
    long address = selftest ? 1 : ((argc == 3 || argc == 5) ? strtol(argv[2], &end, 10) : 0);
    if ((!selftest && ((argc != 3 && argc != 5) || !end || *end)) || address < 1 || address > 30) {
        fprintf(stderr, "usage: csp-v1-node CAN_IF NODE_ID [--receive /run/NEW_FILE | --cmd OBC_ID] | --selftest\n"); return 2;
    }
    if (argc == 5) {
        if (!strcmp(argv[3],"--cmd")) {
            long obc=strtol(argv[4],&end,10);
            if (*end || obc<1 || obc>30 || obc==address) return 2;
            cmd_obc=(unsigned)obc; cmd_enabled=1; cmd_syslog=1;
            openlog("f-astra-can-cmd",LOG_PID,LOG_DAEMON);
            cmd_read_boot_id();
            cmd_time_file=getenv("F_ASTRA_TIME_FILE");
        } else if (!strcmp(argv[3],"--mixed")) {
            if (address!=12 || strncmp(argv[4],"/run/f-astra-finn-can-",20) || strstr(argv[4],"..")) return 2;
            mixed_fd=open(argv[4],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
            if(mixed_fd<0) {perror("fresh mixed payload file");return 1;}mixed_enabled=1;
        } else if (!strcmp(argv[3],"--background")) {
            if (strncmp(argv[4],"/run/f-astra-finn-can-",20) || strstr(argv[4],"..")) return 2;
            background_control=argv[4];
        } else {
            if (strcmp(argv[3],"--receive") || strncmp(argv[4],"/run/f-astra-csp-",15) || strstr(argv[4],"..")) return 2;
            sink_fd=open(argv[4],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
            if (sink_fd<0) { perror("fresh sink file"); return 1; }
            sink_enabled=1;
        }
    }
    if (selftest) alarm(20);
    trace_commands = getenv("F_ASTRA_CSP_TRACE_CMDS") && !strcmp(getenv("F_ASTRA_CSP_TRACE_CMDS"),"1");
    csp_conf_t conf;
    csp_conf_get_defaults(&conf);
    conf.address = (uint8_t)address;
    conf.buffers = 32;
    conf.buffer_data_size = 512;
    if (csp_init(&conf) != CSP_ERR_NONE) return 1;
    csp_iface_t *iface = NULL;
    csp_can_interface_data_t test_data = { .tx_func = test_tx };
    if (selftest) {
        const uint8_t kat[]="123456789";
        if (~crc_update(~0u,kat,9)!=0xcbf43926u) return 1;
        test_iface.name = "CFP-TEST";
        test_iface.interface_data = &test_data;
        if (csp_can_add_interface(&test_iface) != CSP_ERR_NONE) return 1;
        iface = &test_iface;
        if (csp_rtable_set((uint8_t)address, 5, iface, CSP_NO_VIA_ADDRESS) != CSP_ERR_NONE) return 1;
    } else {
        /* Linux ip config owns bitrate. This driver does not change it. */
        if (csp_can_socketcan_open_and_add_interface(argv[1], "CAN", 0, false, &iface) != CSP_ERR_NONE) return 1;
        /* Diagnostic SLCAN peer: bound CAN->USB bursts without changing CFP.
         * An explicitly selected gap remains outside the upstream driver.
         */
        const char *gap_text = getenv("F_ASTRA_CSP_CAN_TX_GAP_US");
        char *gap_end = NULL;
        long gap = gap_text ? strtol(gap_text, &gap_end, 10) : 3000;
        if (gap < 0 || gap > 50000 || (gap_text && (!gap_end || gap_end == gap_text || *gap_end))) return 2;
        tx_gap_us = (unsigned)gap;
        if (background_control) tx_gap_us=0;
        csp_can_interface_data_t *can_data = iface->interface_data;
        native_tx = can_data->tx_func;
        can_data->tx_func = paced_tx;
        if (csp_rtable_set(0, 0, iface, CSP_NO_VIA_ADDRESS) != CSP_ERR_NONE) return 1;
    }
    if (csp_route_start_task(500, 0) != CSP_ERR_NONE) return 1;
    pthread_t worker;
    if (pthread_create(&worker, NULL, server, NULL)) return 1;
    pthread_t background;
    if (background_control && pthread_create(&background,NULL,background_sender,NULL)) return 1;
    /* Give the bounded diagnostic server time to bind before self-test. */
    usleep(100000);
    if (selftest) {
        const int sizes[] = {1, 2, 3, 8, 64, 256, 512};
        unsigned expected_frames = 0;
        for (unsigned p = 0; p < 2; ++p) for (unsigned n = 0; n < sizeof(sizes)/sizeof(sizes[0]); ++n) {
            unsigned char sent[512], received[512];
            for (int b = 0; b < sizes[n]; ++b) sent[b] = (unsigned char)(b ^ (31 * n));
            int bytes = csp_transaction(CSP_PRIO_NORM, (uint8_t)address, p ? 10 : 1,
                                        2000, sent, sizes[n], received, sizes[n]);
            if (bytes != sizes[n] || memcmp(sent, received, (size_t)sizes[n])) {
                fprintf(stderr, "CFP roundtrip failed port=%u bytes=%d\n", p ? 10 : 1, sizes[n]); return 1;
            }
            expected_frames += 2u * (unsigned)((sizes[n] + 6 + 7) / 8);
        }
        pthread_mutex_lock(&count_lock);
        unsigned actual_frames = frame_count;
        pthread_mutex_unlock(&count_lock);
        if (actual_frames != expected_frames) {
            fprintf(stderr, "CFP route bypassed: %u vs %u\n", actual_frames, expected_frames); return 1;
        }
        if (cmd_selftest((unsigned)address)) return 1;
        printf("CSP_V1_CFP_SOFTWARE_PASS packets=14 frames=%u max_payload=512 cmd=pass external_CAN=not_tested\n", actual_frames);
        return 0;
    }
    if (cmd_enabled) {
        cmd_log("CSP_V1_NODE_READY node=%ld interface=%s ports=1,10,11 obc=%u max_payload=512 tx_gap_us=%u",
                address, argv[1], cmd_obc, tx_gap_us);
        cmd_boot_event();
        pthread_t forwarder;
        if (pthread_create(&forwarder,NULL,fm_forwarder,NULL)) cmd_log("CMD_FM_FORWARDER failed");
        pthread_t nn;
        if (pthread_create(&nn,NULL,nn_forwarder,NULL)) cmd_log("CMD_NN_FORWARDER failed");
    } else
        printf("CSP_V1_NODE_READY node=%ld interface=%s ports=1,10 max_payload=512 tx_gap_us=%u\n", address, argv[1], tx_gap_us);
    fflush(stdout);
    pthread_join(worker, NULL);
    return 0;
}
