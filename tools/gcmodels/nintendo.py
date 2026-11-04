import struct
def sarc(d):
    assert d[:4]==b'SARC', d[:4]
    bom=d[6:8]; e='>' if bom==b'\xfe\xff' else '<'
    hl,=struct.unpack(e+'H',d[4:6]); dataoff,=struct.unpack(e+'I',d[0xC:0x10])
    p=hl; assert d[p:p+4]==b'SFAT'
    n,=struct.unpack(e+'H',d[p+6:p+8]); p+=12
    ents=[]
    for i in range(n):
        h,attr,s,en=struct.unpack(e+'IIII',d[p:p+16]); ents.append((attr,s,en)); p+=16
    assert d[p:p+4]==b'SFNT'; nt=p+8
    out={}
    for attr,s,en in ents:
        name=''
        if attr&0x01000000:
            o=nt+(attr&0xFFFF)*4; name=d[o:d.index(b'\0',o)].decode()
        out[name]=d[dataoff+s:dataoff+en]
    return out
def yaz0(d):
    assert d[:4]==b'Yaz0'
    size,=struct.unpack('>I',d[4:8]); out=bytearray(); p=16
    while len(out)<size:
        code=d[p]; p+=1
        for b in range(8):
            if len(out)>=size: break
            if code&0x80: out.append(d[p]); p+=1
            else:
                b1,b2=d[p],d[p+1]; p+=2
                dist=((b1&0xF)<<8|b2)+1
                n=b1>>4
                if n==0: n=d[p]+0x12; p+=1
                else: n+=2
                st=len(out)-dist
                for i in range(n): out.append(out[st+i])
            code<<=1
    return bytes(out)


def sarc_replace(d, new, align=0x2000):
    """the SARC d with some files' data replaced ({name: bytes}); its tables, order and names as they were, each
    file at a multiple of `align` (Wind Waker HD's packs: 0x2000)"""
    hl, = struct.unpack('>H', d[4:6])
    dataoff, = struct.unpack('>I', d[0xC:0x10])
    p = hl
    n, = struct.unpack('>H', d[p + 6:p + 8])
    ents = []
    for i in range(n):
        h, attr, s, en = struct.unpack('>IIII', d[p + 12 + i * 16:p + 28 + i * 16])
        ents.append((attr, s, en))
    nt = hl + 12 + n * 16 + 8
    names = []
    for attr, s, en in ents:
        o = nt + (attr & 0xFFFF) * 4
        names.append(d[o:d.index(b'\0', o)].decode())
    head = bytearray(d[:dataoff])
    body = bytearray()
    for i, (attr, s, en) in enumerate(ents):
        data = new.get(names[i], d[dataoff + s:dataoff + en])
        body += b'\0' * ((-len(body)) % align)
        start = len(body)
        body += data
        struct.pack_into('>II', head, hl + 12 + i * 16 + 8, start, start + len(data))
    out = head + body
    struct.pack_into('>I', out, 8, len(out))
    return bytes(out)
