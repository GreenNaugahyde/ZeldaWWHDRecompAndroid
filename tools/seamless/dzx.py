import struct,sys,re
from arc import load
def tables(b):
    # find chunk tables: count, then count x (fourcc,u32 n,u32 off)
    res=[]
    for m in re.finditer(rb'[A-Z0-9]{4}',b):
        o=m.start()-4
        if o<0 or o%4: continue
        cnt=struct.unpack('>I',b[o:o+4])[0]
        if not (1<=cnt<=60): continue
        ok=True; ch=[]
        for k in range(cnt):
            e=b[o+4+12*k:o+16+12*k]
            if len(e)<12 or not re.fullmatch(rb'[A-Za-z0-9]{4}',e[:4]): ok=False;break
            n,off=struct.unpack('>II',e[4:])
            if off>len(b)-o or n>5000: ok=False;break
            ch.append((e[:4].decode(),n,off))
        if ok and cnt>=3: res.append((o,ch))
    return res
if __name__=='__main__':
    a=load(sys.argv[1])
    for nm,b in a.items():
        if not nm.endswith('bfres'): continue
        for o,ch in tables(b): print(nm,hex(o),ch)
