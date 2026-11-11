# tools/gdnames.py <hex address...> - matches an AOTR game.dat function to its BFME2 1.06 twin by byte pattern and prints the Open-BFME-2 name.
# Nomlar fayli: grep -rIhoE '\\?[A-Za-z0-9_@?$]+ @0x[0-9A-Fa-f]{8}( [0-9]+B: .{0,100})?' $OB2/Code -> json.
import re,sys,struct,bisect,csv
import os
O=os.environ.get('OB2','/tmp/ob2')                       # git clone --depth 1 https://github.com/Open-BFME/Open-BFME-2 (GPL-3.0, read only: names, never code)
NAMES=os.environ.get('OB2NAMES','/tmp/ob2names.json')
R=open(os.environ['GAMEDAT'],'rb').read()   # AOTR's game.dat (RotWK 2.02 delayfix) - your own copy; not part of this repository
B=open(O+'/baselines/bfme2/workshop-vanilla-1.06/files/game.dat','rb').read()
def text(d):
    pe=struct.unpack_from('<I',d,0x3c)[0]; so=struct.unpack_from('<H',d,pe+20)[0]; sh=pe+24+so
    va,vs,rs,rp=struct.unpack_from('<IIII',d,sh+12)[0],struct.unpack_from('<I',d,sh+8)[0],struct.unpack_from('<I',d,sh+16)[0],struct.unpack_from('<I',d,sh+20)[0]
    return va,rp,min(vs,rs)
Rva,Rp,Rn=text(R); Bva,Bp,Bn=text(B)
starts=sorted(int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]{6})',open('/home/user/AOTR_MC/src/gd_funcs.inc').read()))
sym={}
for row in (csv.DictReader(open(O+'/reverse/symbols.csv')) if os.path.exists(O+'/reverse/symbols.csv') else []):
    try: sym.setdefault(int(row['address'],16),row['name'])
    except: pass
import json
for k,v in (json.load(open(NAMES)) if os.path.exists(NAMES) else {}).items():
    sym.setdefault(int(k,16), v[0]+'  '+v[1])
def startof(a):
    i=bisect.bisect_right(starts,a)-1; return starts[i] if i>=0 else None
def pattern(va,n=48):
    off=va-0x400000; b=bytearray(R[off:off+n]); mask=[1]*n
    i=0
    while i<n-4:
        v=struct.unpack_from('<I',b,i)[0]
        if 0x400000<=v<0xF00000: mask[i:i+4]=[0]*4; i+=4; continue
        if b[i] in (0xE8,0xE9) : mask[i+1:i+5]=[0]*4; i+=5; continue
        i+=1
    p=b''.join(re.escape(bytes([b[k]])) if mask[k] else b'.' for k in range(n))
    return re.compile(p,re.S)
Btext=B[Bp:Bp+Bn]
for a in [int(x,16) for x in sys.argv[1:]]:
    s=startof(a)
    if s is None: print(hex(a),'?'); continue
    ms=[]
    for n in (64,48,32,24):
        ms=[m.start() for m in pattern(s,n).finditer(Btext)][:3]
        if len(ms)==1: break
    names=[sym.get(m+Bva, '(unnamed %#x)'%(m+Bva+0x400000)) for m in ms]
    print('%#x fn %#x +%#x -> %s'%(a,s,a-s, ' / '.join(x[:110] for x in names) if names else 'not found'))
