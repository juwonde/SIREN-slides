"""POES SEM-2 named-field replay, sample-list reference and UTC-bin reduction.

PSMP0001 is our OBC replay envelope, NOT a NOAA native telemetry packet.
NOAA L0b archive records are decoded separately by poes-sem-preprocess.c.
NetCDF decompression/selection is an OBC/host operation; record parsing, list
construction and binning are separate, measurable payload operations.
"""
from __future__ import annotations
import argparse, hashlib, json, re, struct, zlib
from pathlib import Path
import numpy as np

HEADER = struct.Struct('<8s4I32sQ')
ROW = struct.Struct('<QfffQI')
FRAME = struct.Struct('<4sIHH')
COUNTS, FLUX = 1, 2
BIN_MS = 50_000

def channel_names(level):
    if level not in ('l1a', 'l1b'):raise ValueError('explicit l1a/l1b required')
    kind = 'cps' if level == 'l1a' else 'flux'
    names = [f'mep_pro_tel{tel}_{kind}_p{k}' for tel in (0,90) for k in range(1,7)]
    names += [f'mep_ele_tel{tel}_{kind}_e{k}' for tel in (0,90) for k in range(1,4 if level=='l1a' else 5)]
    names += [f'mep_omni_{kind}_p{k}' for k in (range(6,10) if level=='l1a' else range(1,4))]
    names += [f'ted_{species}_tel{tel}_{kind}_{k}' for species in ('ele','pro') for tel in (0,30) for k in (4,8,11,14)]
    names += [f'ted_{species}_tel{tel}_{band}_eflux'+('_cps' if level=='l1a' else '')
              for species in ('ele','pro') for band in ('low','hi') for tel in (0,30)]
    return names

def load_netcdf(path, level, first=0, limit=None):
    from netCDF4 import Dataset
    names=channel_names(level);columns=[];valids=[]
    with Dataset(path) as d:
        n=len(d['time']);end=n if limit is None else min(n,first+limit)
        if not 0<=first<end:raise ValueError('empty/out-of-range selection')
        sl=slice(first,end)
        times=np.asarray(d['time'][sl],np.uint64)
        if np.any(times[1:]<=times[:-1]):raise ValueError('duplicate/unordered timestamp')
        # The files document time as epoch milliseconds; cross-check Y/DOY/ms.
        from datetime import datetime,timezone,timedelta
        expected=np.asarray([int((datetime(int(y),1,1,tzinfo=timezone.utc)+timedelta(days=int(day)-1)).timestamp())*1000+int(ms)
            for y,day,ms in zip(d['year'][sl],d['day'][sl],d['msec'][sl])],np.uint64)
        # Published files can differ by1ms between redundant time and msec
        # coordinates. Preserve the delivered time; reject a larger discrepancy.
        time_delta=np.abs(times.astype(np.int64)-expected.astype(np.int64))
        if np.any(time_delta>1):raise ValueError('time is not epoch milliseconds (tolerance1ms)')
        max_time_delta_ms=int(time_delta.max())
        pos=np.column_stack([np.ma.filled(d[k][sl].astype('f4'),np.nan) for k in ('lat','lon','sat_direction')]).astype('<f4')
        instrument={};unknown={}
        # IFC is a32s instrument-status field stored sparsely in these2s files.
        # Carry status only within its32s lifetime; never fill science samples.
        left=max(0,first-16)
        status_times=np.asarray(d['time'][left:end],np.uint64)
        for k in ('mep','ted'):
            raw=d[k+'_IFC_on'][left:end];mask=np.ma.getmaskarray(raw)
            state=np.full(len(raw),-1,np.int8);last=-1;last_stamp=0
            for i,(stamp,value) in enumerate(zip(status_times,np.ma.getdata(raw))):
                if not mask[i] and value in (0,1):last=int(value);last_stamp=int(stamp)
                if last>=0 and int(stamp)-last_stamp<32000:state[i]=last
            state=state[first-left:];instrument[k]=state==1;unknown[k]=state<0
        quality=(instrument['mep'].astype('u4')|(instrument['ted'].astype('u4')<<1)|
                 (unknown['mep'].astype('u4')<<8)|(unknown['ted'].astype('u4')<<9))
        descriptors=[]
        for name in names:
            variable=d[name];raw=variable[sl];values=np.asarray(np.ma.filled(raw.astype('f4'),np.nan),dtype='<f4')
            good=np.isfinite(values)&(values>=0)&~instrument[name[:3]]&~unknown[name[:3]]
            units=variable.units
            if level=='l1a' and units not in ('#/s','counts'):raise ValueError('count units required: '+name)
            if level=='l1b' and not any(x in units for x in ('cm2','mW/')):raise ValueError('flux units required: '+name)
            columns.append(values);valids.append(good)
            descriptors.append(dict(name=name,units=units,dtype='float32',
                missing_value=str(getattr(variable,'missing_value',None))))
    values=np.column_stack(columns).astype('<f4');valid=np.column_stack(valids)
    masks=np.zeros(len(times),np.uint64)
    for c in range(len(names)):masks|=valid[:,c].astype(np.uint64)<<np.uint64(c)
    schema=dict(schema='astra/poes-sample-view/v1',level=level,channels=descriptors,
        selection='MEPED proton/electron/omni + TED four spectral bands and directional energy flux',
        quality='finite nonnegative, NetCDF mask, IFC=0; sparse IFC status held for<32s; unknown IFC invalid; science never interpolated',
        coordinates='lat/lon/sat_direction retained, invalid coordinates excluded from position means',
        time='delivered uint64 epoch milliseconds preserved; checked against year/day/msec to1ms',
        redundant_time_max_difference_ms=max_time_delta_ms)
    return times,pos,values,masks,quality,schema

def pack_records(times,pos,values,masks,quality,schema):
    if len(times)==0 or len(times)!=len(values):raise ValueError('row shape')
    kind=COUNTS if schema['level']=='l1a' else FLUX
    digest=hashlib.sha256(json.dumps(schema,sort_keys=True,separators=(',',':')).encode()).digest()
    header=HEADER.pack(b'PSMP0001',1,kind,values.shape[1],len(times),digest,int(times[0])//86_400_000*86_400_000)
    data=bytearray(header);rows=[]
    for i,(stamp,p,v,mask,q) in enumerate(zip(times,pos,values,masks,quality)):
        row=ROW.pack(int(stamp),*p,int(mask),int(q))+np.asarray(v,dtype='<f4').tobytes()
        frame=FRAME.pack(b'SEM1',i,len(row),kind)+row
        data.extend(frame);data.extend(struct.pack('<I',zlib.crc32(frame)))
        rows.append(row)
    return bytes(data),HEADER.pack(b'PSLIST01',1,kind,values.shape[1],len(times),digest,int(times[0])//86_400_000*86_400_000)+b''.join(rows)

def read_list(blob):
    magic,version,kind,ch,n,digest,day=HEADER.unpack_from(blob)
    if magic!=b'PSLIST01' or version!=1 or not 1<=ch<=64 or len(blob)!=64+n*(32+4*ch):raise ValueError('list layout')
    rows=[]
    for i in range(n):
        at=64+i*(32+4*ch);meta=ROW.unpack_from(blob,at);values=struct.unpack_from('<'+'f'*ch,blob,at+32)
        rows.append([meta[0],*meta[1:4],meta[4],meta[5],list(values)])
    return kind,ch,digest,rows

def reduce_list(blob):
    kind,ch,digest,rows=read_list(blob)
    if not rows:raise ValueError('empty list')
    first=rows[0][0]//BIN_MS*BIN_MS;last=rows[-1][0]//BIN_MS*BIN_MS
    n=(last-first)//BIN_MS+1
    out=bytearray(HEADER.pack(b'PSBINS01',1,kind,ch,n,digest,first))
    cursor=0
    for b in range(n):
        stamp=first+b*BIN_MS;sumv=[0.0]*ch;count=[0]*ch;lat=lon=0.0;np_=nr=0
        while cursor<len(rows) and rows[cursor][0]<stamp+BIN_MS:
            row=rows[cursor];cursor+=1;nr+=1
            if np.isfinite(row[1]) and np.isfinite(row[2]) and -90<=row[1]<=90 and -180<=row[2]<=360:
                lat+=row[1];lon+=row[2];np_+=1
            for c in range(ch):
                if row[4]>>c&1:
                    if not np.isfinite(row[6][c]) or row[6][c]<0:raise ValueError('valid mask contradicts value')
                    sumv[c]+=row[6][c];count[c]+=1
        means=[sumv[c]/count[c] if count[c] else float('nan') for c in range(ch)]
        out.extend(struct.pack('<QIff',stamp,nr,lat/np_ if np_ else float('nan'),lon/np_ if np_ else float('nan')))
        out.extend(struct.pack('<'+'f'*ch,*means));out.extend(struct.pack('<'+'H'*ch,*count))
    return bytes(out)

def require_counts_contract(schema,channels):
    if schema['level']!='l1a':raise ValueError('L1b flux cannot be relabelled as L1a counts; a new model/input contract is required')
    known={c['name']:c['units'] for c in schema['channels']}
    if any(name not in known for name in channels):raise ValueError('model channel absent')

def archive_list(data):
    """Independent offset reference for the selected30 native count channels.

    Preserve source quality/missing flags and detector counts; do not manufacture
    TED spectral samples, calibrated flux, direction or NetCDF-grid timestamps.
    """
    from datetime import datetime,timezone,timedelta
    if not data or len(data)%2544:raise ValueError('native archive requires whole2544-byte records')
    text=Path(__file__).with_name('poes_sem2_counts.h').read_text()
    table=[float(x) for x in re.findall(r'(\d+\.\d+)f',text)]
    if len(table)!=256:raise ValueError('SEM2 decompression table')
    names=channel_names('l1a')[:22]+channel_names('l1a')[-8:]
    schema=dict(schema='astra/poes-sem2-native-view/v1',channels=names,
        units='uncalibrated detector counts; preserve native accumulation semantics',
        fields='MEPED22 plus TED8 directional energy counts, including all Comp28 channels',
        direction='unknown(-1); downstream derivation requires latitude history',source_format='NOAA SEM2 L0b archive2544')
    digest=hashlib.sha256(json.dumps(schema,sort_keys=True,separators=(',',':')).encode()).digest()
    packed=[];previous=0
    for at in range(0,len(data),2544):
        block=data[at:at+2544]
        def be(offset):return struct.unpack_from('>I',block,offset)[0]
        for s in range(16):
            b,w=divmod(s,4);ih=216+204*b
            _,year,day,msec=struct.unpack_from('>4I',block,ih)
            if not 1970<=year<=2100 or not 1<=day<=366 or msec>=86400000:raise ValueError('native date')
            stamp=int((datetime(year,1,1,tzinfo=timezone.utc)+timedelta(days=day-1)).timestamp())*1000+msec+2000*w
            if stamp<=previous:raise ValueError('duplicate/unordered native time')
            previous=stamp
            lp=88+32*b+4*w
            lat,lon=[np.float32(np.float32(struct.unpack_from('>i',block,off)[0])*np.float32(.0001)) for off in (lp,lp+16)]
            missing=block[260+204*b+40*w:300+204*b+40*w];values=[];valid=[]
            for c in range(22):
                if c<6:tel,index=0,c
                elif c<12:tel,index=1,c-6
                elif c<15:tel,index=0,c-12+6
                elif c<18:tel,index=1,c-15+6
                else:tel,index=2,c-18
                if tel<2:offset=1032+152*b+36*tel+9*w+index;mi=1+9*tel+index
                else:offset=1104+152*b+4*w+index;mi=19+index if index<2 else 21
                bad=missing[mi]==1
                if tel==2 and index>=2 and ((index==2 and s%2==1) or (index==3 and s%2==0)):bad=False
                observed=not (tel==2 and ((index==2 and s%2==1) or (index==3 and s%2==0)))
                values.append(-999.0 if bad else table[block[offset]]);valid.append(not bad and observed)
            for c in range(8):
                k=(0,2,1,3)[c//2];tel=c%2;bad=missing[26+2*k+tel]==1
                values.append(-999.0 if bad else table[block[1120+32*tel+152*b+4*w+k]]);valid.append(not bad)
            quality=struct.unpack_from('>H',block,244+204*b+2*w)[0]
            mask=sum(int(ok)<<c for c,ok in enumerate(valid))
            if be(0):quality|=0x80000000;mask=0
            packed.append(ROW.pack(stamp,lat,lon,-1.0,mask,quality)+struct.pack('<30f',*values))
    header=HEADER.pack(b'PSLIST01',1,0,30,len(packed),digest,ROW.unpack_from(packed[0])[0]//86_400_000*86_400_000)
    return header+b''.join(packed),schema,digest.hex()

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True)
    p.add_argument('--level',choices=['l1a','l1b'],required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--first',type=int,default=0);p.add_argument('--rows',type=int)
    a=p.parse_args()
    if a.output.exists():p.error('fresh output directory required')
    data=load_netcdf(a.source,a.level,a.first,a.rows);packet,listed=pack_records(*data);bins=reduce_list(listed)
    a.output.mkdir(parents=True)
    for name,blob in [('packets.bin',packet),('expected-list.bin',listed),('expected-bins.bin',bins)]:
        (a.output/name).write_bytes(blob)
    kind,ch,_,rows=read_list(listed)
    preview=[]
    for r in rows[:3]:preview.append([r[0],*r[1:4],hex(r[4]),r[5],[v if np.isfinite(v) else None for v in r[6]]])
    (a.output/'list-preview.json').write_text(json.dumps(preview,indent=2))
    result=dict(schema=data[-1],source_name=a.source.name,source_sha256=hashlib.sha256(a.source.read_bytes()).hexdigest(),
        rows=len(rows),channels=ch,kind=kind,wire_is_custom_replay_envelope=True,native_NOAA_telemetry=False,
        NetCDF_decode_on_host=True,input_bytes=len(packet),list_bytes=len(listed),bins_bytes=len(bins),
        invalid_channel_samples=sum(ch-r[4].bit_count() for r in rows),
        timestamp_gaps_over3s=sum(b[0]-a[0]>3000 for a,b in zip(rows,rows[1:])),
        files={n:hashlib.sha256((a.output/n).read_bytes()).hexdigest() for n in ('packets.bin','expected-list.bin','expected-bins.bin')})
    (a.output/'manifest.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:v for k,v in result.items() if k not in ('schema','files')}))
if __name__=='__main__':main()
