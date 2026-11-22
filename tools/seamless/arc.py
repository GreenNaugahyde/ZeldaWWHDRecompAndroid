import struct,sys
def yaz0(d):
    if d[:4]!=b'Yaz0': return d
    n=struct.unpack('>I',d[4:8])[0]; o=bytearray(); i=16
    while len(o)<n:
        c=d[i]; i+=1
        for b in range(8):
            if len(o)>=n: break
            if c&(0x80>>b): o.append(d[i]); i+=1
            else:
                b1,b2=d[i],d[i+1]; i+=2; dist=((b1&15)<<8|b2)+1; ln=b1>>4
                if ln==0: ln=d[i]+0x12; i+=1
                else: ln+=2
                for k in range(ln): o.append(o[-dist])
    return bytes(o)
def sarc(d):
    assert d[:4]==b'SARC', d[:4]
    bom=d[6:8]; e='>' if bom==b'\xfe\xff' else '<'
    hl,=struct.unpack(e+'H',d[4:6]); dataoff,=struct.unpack(e+'I',d[0xc:0x10])
    o=hl; assert d[o:o+4]==b'SFAT'; n,=struct.unpack(e+'H',d[o+6:o+8]); o+=12
    ents=[]
    for k in range(n):
        h,attr,s,en=struct.unpack(e+'IIII',d[o:o+16]); o+=16; ents.append((attr,s,en))
    assert d[o:o+4]==b'SFNT'; nt=o+8
    out={}
    for attr,s,en in ents:
        nm=d[nt+(attr&0xffff)*4:].split(b'\0')[0].decode() if attr>>24 else hex(attr)
        out[nm]=d[dataoff+s:dataoff+en]
    return out
def load(p): return sarc(yaz0(open(p,'rb').read()))
if __name__=='__main__':
    for k,v in load(sys.argv[1]).items(): print(k,len(v),v[:4])
