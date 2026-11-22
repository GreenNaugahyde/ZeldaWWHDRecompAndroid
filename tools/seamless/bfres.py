import struct
def ext_files(b):
    """bfres v3 embedded files: {name: (abs data offset, size, abs pos of the offset field)}"""
    g=0x4c+struct.unpack('>i',b[0x4c:0x50])[0]
    cnt=struct.unpack('>I',b[g+4:g+8])[0]
    out={}
    for k in range(1,cnt+1):
        e=g+8+16*k
        no=e+8+struct.unpack('>i',b[e+8:e+12])[0]; do=e+12+struct.unpack('>i',b[e+12:e+16])[0]
        nm=b[no:].split(b'\0')[0].decode()
        off=do+struct.unpack('>i',b[do:do+4])[0]; sz=struct.unpack('>I',b[do+4:do+8])[0]
        out[nm]=(off,sz)
    return out
