import struct,sys
d=open(sys.argv[1],'rb').read()
u32=lambda o:struct.unpack('>I',d[o:o+4])[0]
u16=lambda o:struct.unpack('>H',d[o:o+2])[0]
rel=lambda o:(o+struct.unpack('>i',d[o:o+4])[0]) if u32(o) else 0
def s(o): return d[o:d.index(b'\0',o)].decode('latin1') if o else None
def group(o):
    if not o: return []
    n=u32(o+4); return [(s(rel(o+8+i*16+8)),rel(o+8+i*16+12)) for i in range(1,n+1)]
G=[rel(0x20+i*4) for i in range(12)]
for name,fm in group(G[0]):
    if name!=sys.argv[2]: continue
    vbl=rel(fm+16); nvb=u16(fm+32)
    for i in range(nvb):
        o=vbl+i*0x20
        na=d[o+4]; cnt=u32(o+8); skin=d[o+12]; al=rel(o+16); bl=rel(o+0x18)
        at=['%s@b%d+%d:%x'%(s(rel(al+k*12)),d[al+k*12+4],u16(al+k*12+6),u32(al+k*12+8)) for k in range(na)]
        bufs=['stride%d size%d'%(u16(bl+k*0x18+12),u32(bl+k*0x18+4)) for k in range(d[o+5])]
        print('VB',i,'n',cnt,'skin',skin,at,bufs)
    for sn,so in group(rel(fm+20)):
        print('SHAPE',sn,'mat',u16(so+0xE),'bone',u16(so+0x10),'vb',u16(so+0x12),'skinbones',u16(so+0x14),'skin',d[so+0x16],'meshes',d[so+0x17])
    for mn,mo in group(rel(fm+24)):
        sa=rel(mo+0x24)
        attrs=[(k,s(v)) for k,v in group(rel(sa+0x10))]
        samps=[(k,s(v)) for k,v in group(rel(sa+0x14))]
        opts=[(k,s(v)) for k,v in group(rel(sa+0x18))]
        tl=rel(mo+0x28); texs=[s(rel(tl+k*8)) for k in range(d[mo+0x11])]
        print('MAT',mn,'shader',s(rel(sa)),s(rel(sa+4)),'tex',texs,'attr',attrs,'samp',samps,'nopts',len(opts), 'params',u16(mo+0x12))
        if len(sys.argv)>3: print('   opts',opts)
