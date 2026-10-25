/* POES SEM-2 archive / typed OBC replay -> sample list -> UTC50s means.
 * archive: native NCEI2544-byte records, MEPED22 + TED8 energy-count fields.
 * replay: PSMP0001, explicit kind/schema, per-row sequence/length/CRC32.
 * No FPGA MMIO. No model normalization or silent counts/flux conversion.
 * Usage: poes-sem-preprocess replay|archive IN OUT_PREFIX [ARCHIVE_SCHEMA_HEX]
 * Native archive decoding follows NOAA unpackSem2_v2.c, 2005-08-19.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <inttypes.h>
#include "poes_sem2_counts.h"
#define MAX_CH 64u
#define BIN_MS 50000ull
static uint32_t crc_table[256];
static void die(const char *s){fprintf(stderr,"POES_PREPROCESS_REJECT %s\n",s);exit(2);}
static uint32_t u32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t u64(const uint8_t *p){return u32(p)|(uint64_t)u32(p+4)<<32;}
static uint32_t be32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static unsigned be16(const uint8_t *p){return (unsigned)p[0]<<8|p[1];}
static void p32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static void p64(uint8_t *p,uint64_t v){p32(p,(uint32_t)v);p32(p+4,(uint32_t)(v>>32));}
static float f32(const uint8_t *p){uint32_t b=u32(p);float f;memcpy(&f,&b,4);return f;}
static void pf32(uint8_t *p,float f){uint32_t b;memcpy(&b,&f,4);p32(p,b);}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static uint32_t crc(const uint8_t *p,size_t n){uint32_t v=~0u;while(n--)v=crc_table[(v^*p++)&255]^(v>>8);return ~v;}
static uint64_t epoch(int y,int day,uint32_t ms){
    if(y<1970||y>2100||day<1||day>365+(y%4==0&&(y%100!=0||y%400==0))||ms>=86400000)die("archive date");
    uint64_t days=(unsigned)(day-1);
    for(int k=1970;k<y;k++)days+=365+(k%4==0&&(k%100!=0||k%400==0));
    return days*86400000ull+ms;
}
static void store(const char *prefix,const char *suffix,const void *p,size_t n){
    char path[4096];if(snprintf(path,sizeof path,"%s%s",prefix,suffix)>=(int)sizeof path)die("output path");
    FILE *f=fopen(path,"wbx");if(!f||fwrite(p,1,n,f)!=n||fclose(f))die("output write (fresh prefix required)");
}
static uint64_t invalid=0,gaps=0,flagged=0;
static void check_row(uint8_t *row,unsigned ch,uint64_t previous){
    uint64_t stamp=u64(row),mask=u64(row+20);
    if(previous&&stamp<=previous)die("duplicate or unordered timestamp");
    if(previous&&stamp-previous>3000)gaps++;
    if(ch<64&&(mask>>ch))die("valid mask channel overflow");
    if(u32(row+28))flagged++;
    for(unsigned c=0;c<ch;c++){
        float v=f32(row+32+c*4);
        if(mask>>c&1){if(!isfinite(v)||v<0)die("valid mask contradicts value");}
        else invalid++;
    }
}
static void archive_row(const uint8_t *a,unsigned s,uint8_t *out){
    unsigned block=s/4,within=s%4;const uint8_t *ih=a+216+204*block;
    uint64_t stamp=epoch((int)be32(ih+4),(int)be32(ih+8),be32(ih+12))+2000ull*within;
    p64(out,stamp);
    unsigned lp=88+32*block+4*within;
    pf32(out+8,(float)(int32_t)be32(a+lp)*.0001f);
    pf32(out+12,(float)(int32_t)be32(a+lp+16)*.0001f);pf32(out+16,-1.0f);
    const uint8_t *missing=a+260+204*block+40*within;
    uint64_t mask=(1ull<<30)-1;
    for(unsigned c=0;c<22;c++){
        /* Channel order is pro0/pro90, ele0/ele90, omni6..9. */
        unsigned tel,index,offset,mi;
        if(c<6){tel=0;index=c;}else if(c<12){tel=1;index=c-6;}
        else if(c<15){tel=0;index=c-12+6;}else if(c<18){tel=1;index=c-15+6;}
        else{tel=2;index=c-18;}
        if(tel<2){offset=1032+152*block+36*tel+9*within+index;mi=1+9*tel+index;}
        else{offset=1104+152*block+4*within+index;mi=index<2?19+index:21;}
        int bad=missing[mi]==1;
        if(tel==2&&index>=2&&((index==2&&s%2==1)||(index==3&&s%2==0)))bad=0;
        pf32(out+32+4*c,bad?-999.0f:sem2_counts[a[offset]]);
        if(bad)mask&=~(1ull<<c);
        /* P8/P9 alternate; the other slot's encoded0 is no observation. */
        if(tel==2&&((index==2&&s%2==1)||(index==3&&s%2==0)))mask&=~(1ull<<c);
    }
    const unsigned ted_index[4]={0,2,1,3};
    for(unsigned c=0;c<8;c++){
        unsigned k=ted_index[c/2],tel=c%2,mi=26+2*k+tel;
        int bad=missing[mi]==1;float v=sem2_counts[a[1120+32*tel+152*block+4*within+k]];
        pf32(out+32+4*(22+c),bad?-999.0f:v);if(bad)mask&=~(1ull<<(22+c));
    }
    /* Retain upstream quality; do not invent a byte-sum check over the archive. */
    unsigned quality=be16(a+244+204*block+2*within);
    if(be32(a)){quality|=0x80000000u;mask=0;}
    p64(out+20,mask);p32(out+28,quality);
}
int main(int argc,char **argv){
    if(argc<4||argc>5)die("usage: replay|archive INPUT PREFIX [SCHEMA_HEX]");
    for(unsigned i=0;i<256;i++){uint32_t v=i;for(unsigned k=0;k<8;k++)v=v&1?0xEDB88320u^(v>>1):v>>1;crc_table[i]=v;}
    double start=now();FILE *f=fopen(argv[2],"rb");if(!f)die("open input");
    if(fseek(f,0,SEEK_END))die("seek");
    long length=ftell(f);rewind(f);
    if(length<0||length>268435456)die("input size bound");
    uint8_t *in=malloc((size_t)length? (size_t)length:1);if(!in||fread(in,1,(size_t)length,f)!=(size_t)length||fclose(f))die("input read");
    unsigned ch,n,kind;size_t input_at=64;uint8_t header[64]={0};
    if(!strcmp(argv[1],"replay")){
        if(length<64||memcmp(in,"PSMP0001",8)||u32(in+8)!=1)die("replay header");
        memcpy(header,in,64);kind=u32(in+12);ch=u32(in+16);n=u32(in+20);
        if((kind!=1&&kind!=2)||ch<1||ch>MAX_CH||n<1||n>1000000u)die("replay shape/kind");
    }else if(!strcmp(argv[1],"archive")){
        if(argc!=5||strlen(argv[4])!=64||length<2544||length%2544)die("native2544 archive length/schema");
        ch=30;n=(unsigned)(length/2544)*16;kind=0;
        p32(header+8,1);p32(header+12,kind);p32(header+16,ch);p32(header+20,n);
        for(unsigned i=0;i<32;i++){unsigned b;if(sscanf(argv[4]+i*2,"%2x",&b)!=1)die("schema hex");header[24+i]=(uint8_t)b;}
    }else die("explicit source format required");
    size_t row_bytes=32+4*ch,total=64+(size_t)n*row_bytes;
    uint8_t *list=calloc(total,1);if(!list)die("list allocation");memcpy(list,header,64);memcpy(list,"PSLIST01",8);
    uint64_t previous=0;
    for(unsigned i=0;i<n;i++){
        uint8_t *row=list+64+(size_t)i*row_bytes;
        if(kind){
            size_t frame_bytes=12+row_bytes;
            if(input_at+frame_bytes+4>(size_t)length)die("truncated row");
            const uint8_t *frame=in+input_at;
            if(memcmp(frame,"SEM1",4)||u32(frame+4)!=i||(frame[8]|(unsigned)frame[9]<<8)!=row_bytes||
               (frame[10]|(unsigned)frame[11]<<8)!=kind)die("row sync/sequence/length/kind");
            if(crc(frame,frame_bytes)!=u32(frame+frame_bytes))die("row CRC32");
            memcpy(row,frame+12,row_bytes);input_at+=frame_bytes+4;
        }else archive_row(in+(size_t)(i/16)*2544,i%16,row);
        check_row(row,ch,previous);previous=u64(row);
    }
    if(kind&&input_at!=(size_t)length)die("trailing bytes");
    if(!kind)p64(list+56,u64(list+64)/86400000ull*86400000ull);
    store(argv[3],".list.bin",list,total);double listed=now();
    uint64_t first=u64(list+64)/BIN_MS*BIN_MS,last=previous/BIN_MS*BIN_MS;
    uint64_t bins=(last-first)/BIN_MS+1;if(bins>200000)die("bin span bound");
    size_t bin_bytes=20+6*ch,bin_total=64+(size_t)bins*bin_bytes;
    uint8_t *out=calloc(bin_total,1);if(!out)die("bins allocation");memcpy(out,list,64);memcpy(out,"PSBINS01",8);
    p32(out+20,(uint32_t)bins);p64(out+56,first);unsigned cursor=0;
    for(uint64_t b=0;b<bins;b++){
        double sums[MAX_CH]={0},la=0,lo=0;uint16_t counts[MAX_CH]={0};unsigned nr=0,np=0;
        uint64_t stamp=first+b*BIN_MS;
        while(cursor<n&&u64(list+64+(size_t)cursor*row_bytes)<stamp+BIN_MS){
            const uint8_t *r=list+64+(size_t)cursor++*row_bytes;nr++;
            float lat=f32(r+8),lon=f32(r+12);uint64_t mask=u64(r+20);
            if(isfinite(lat)&&isfinite(lon)&&lat>=-90&&lat<=90&&lon>=-180&&lon<=360){la+=lat;lo+=lon;np++;}
            for(unsigned c=0;c<ch;c++)if(mask>>c&1){if(counts[c]==65535)die("bin count overflow");sums[c]+=f32(r+32+4*c);counts[c]++;}
        }
        uint8_t *r=out+64+(size_t)b*bin_bytes;p64(r,stamp);p32(r+8,nr);pf32(r+12,np?(float)(la/np):NAN);pf32(r+16,np?(float)(lo/np):NAN);
        for(unsigned c=0;c<ch;c++){
            pf32(r+20+4*c,counts[c]?(float)(sums[c]/counts[c]):NAN);
            r[20+4*ch+2*c]=(uint8_t)counts[c];r[21+4*ch+2*c]=(uint8_t)(counts[c]>>8);
        }
    }
    store(argv[3],".bins.bin",out,bin_total);double done=now();
    printf("{\"kind\":%u,\"rows\":%u,\"channels\":%u,\"bins\":%" PRIu64 ",\"invalid_channel_samples\":%" PRIu64 ",\"flagged_rows\":%" PRIu64 ",\"gaps_over3s\":%" PRIu64 ",\"parse_list_seconds\":%.9f,\"bin_write_seconds\":%.9f,\"total_seconds\":%.9f}\n",kind,n,ch,bins,invalid,flagged,gaps,listed-start,done-listed,done-start);
    free(in);free(list);free(out);return 0;
}
