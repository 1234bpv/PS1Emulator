from pathlib import Path
import argparse, subprocess, sys, struct, random, hashlib, json, itertools, time
ROOT=Path(__file__).resolve().parents[2]
OUT=Path(__file__).resolve().parent/'artifacts'
sys.path.insert(0,str(OUT/'deps'))
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn.arm_const import *
TC=ROOT.parent/'toolchain/gcce/arm-2012.03/bin'
HEADER='plugins/gpu_unai/gpu_inner_light_arm.h'
def command(args):
    return subprocess.check_output([str(x) for x in args],stderr=subprocess.STDOUT,timeout=120)
def build(name,opt):
    stem=OUT/(name+opt[1:])
    command([TC/'arm-none-symbianelf-g++.exe','-march=armv6','-marm',opt,'-fomit-frame-pointer','-fno-exceptions','-fno-rtti','-fno-unwind-tables','-nostdlib','-I'+str(ROOT/'pcsx_rearmed/include'),'-DTEST_HEADER="'+name+'.h"','-c',OUT/'wrapper.cpp','-o',str(stem)+'.o'])
    command([TC/'arm-none-symbianelf-ld.exe','-Ttext=0x10000','-e','batch',str(stem)+'.o','-o',str(stem)+'.elf'])
    command([TC/'arm-none-symbianelf-objcopy.exe','-O','binary',str(stem)+'.elf',str(stem)+'.bin'])
    dis=command([TC/'arm-none-symbianelf-objdump.exe','-d',str(stem)+'.elf'])
    Path(str(stem)+'.dis').write_bytes(dis)
    nm=command([TC/'arm-none-symbianelf-nm.exe',str(stem)+'.elf']).decode()
    symbols={line.split()[2]:int(line.split()[0],16) for line in nm.splitlines() if len(line.split())==3}
    return Path(str(stem)+'.bin').read_bytes(),symbols

def execute(program, payload, n, fn='batch'):
    code,symbols=program
    u=Uc(UC_ARCH_ARM, UC_MODE_ARM)
    u.ctl_set_cpu_model(UC_CPU_ARM_1136)
    u.mem_map(0x10000,0x10000);u.mem_write(0x10000,code)
    u.mem_map(0x100000,0x1000000);u.mem_write(0x100000,payload)
    u.mem_map(0x2000000,0x1000000)
    u.mem_map(0x4000000,0x10000)
    callee=[UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_R6,UC_ARM_REG_R7,UC_ARM_REG_R8,UC_ARM_REG_R9,UC_ARM_REG_R10,UC_ARM_REG_R11]
    for i,reg in enumerate(callee):u.reg_write(reg,0xa5000000+i)
    u.reg_write(UC_ARM_REG_SP,0x4008000)
    u.reg_write(UC_ARM_REG_LR,0x1fff0)
    u.reg_write(UC_ARM_REG_R0,0x100000);u.reg_write(UC_ARM_REG_R1,0x2000000);u.reg_write(UC_ARM_REG_R2,n)
    u.emu_start(symbols[fn],0x1fff0,count=100000000)
    assert u.reg_read(UC_ARM_REG_PC)==0x1fff0, 'instruction limit'
    assert u.reg_read(UC_ARM_REG_SP)==0x4008000,'stack imbalance'
    for i,reg in enumerate(callee):assert u.reg_read(reg)==0xa5000000+i,'callee-saved register corruption'
    assert bytes(u.mem_read(0x100000,len(payload)))==payload,'input modified'
    return bytes(u.mem_read(0x2000000,n*(12 if fn=='sequence' else 4)))

def main():
    parser=argparse.ArgumentParser(description='Compare a live Gouraud ARM candidate with an explicit baseline revision.')
    parser.add_argument('--baseline-ref',default='72877425b09481908a8c69b13afc6aa8fb44349f')
    parser.add_argument('--candidate-header',type=Path,default=ROOT/'pcsx_rearmed'/HEADER)
    args=parser.parse_args()
    git=['git','-c','safe.directory='+str(ROOT/'pcsx_rearmed').replace('\\','/'),'-C',ROOT/'pcsx_rearmed']
    baseline_revision=command(git+['rev-parse','--verify',args.baseline_ref+'^{commit}']).decode().strip()
    baseline=command(git+['show',baseline_revision+':'+HEADER])
    candidate_path=args.candidate_header.resolve()
    candidate=candidate_path.read_bytes()
    OUT.mkdir(exist_ok=True)
    (OUT/'baseline.h').write_bytes(baseline)
    (OUT/'candidate.h').write_bytes(candidate)
    union=(ROOT/'pcsx_rearmed/plugins/gpu_unai/gpu_unai.h').read_text().split('union gcol_t {',1)[1].split('\n};',1)[0]
    wrapper='''typedef unsigned int u32; typedef unsigned short u16; typedef unsigned int uint_fast16_t; typedef int int_fast32_t;
#include "arm_features.h"
union gcol_t {'''+union+'''\n};
#define GPU_INLINE static inline __attribute__((always_inline))
#include TEST_HEADER
extern "C" void batch(const u32 *in,u32 *out,u32 n) {
 for(u32 i=0;i<n;i++){gcol_t c;c.raw32[0]=in[i*3+1];c.raw32[1]=in[i*3+2];out[i]=gpuLightingTXTGouraudARM(in[i*3],c);}
}
extern "C" void sequence(const u32 *in,u32 *out,u32 n) {
 gcol_t c,inc;c.raw32[0]=in[0];c.raw32[1]=in[1];inc.raw32[0]=in[2];inc.raw32[1]=in[3];
 for(u32 i=0;i<n;i++){out[i*3]=gpuLightingTXTGouraudARM((i*17389)^in[4],c);out[i*3+1]=c.raw32[0];out[i*3+2]=c.raw32[1];c+=inc;}
}
'''
    (OUT/'wrapper.cpp').write_text(wrapper)
    rng=random.Random(0x47555241)
    colors=[0,1,7,8,63,64,127,128,129,247,248,254,255]
    cases=[]
    # All texture words against representative neutral/black/saturating light.
    for light in [0,1,127,128,129,254,255]:
        for src in range(65536): cases.append((src,(light<<24)|(light<<8),light<<8))
    # Full cross product of lighting boundaries against independently mixed texel channels.
    for r,g,b in itertools.product(colors,repeat=3):
        for src in [0,0x1f,0x3e0,0x7c00,0x7fff,0x4210,0x1234,0x6b5a]:
            for mask in [0,0x8000]:cases.append((src|mask,(r<<8)|(g<<24)|0x00ff00ff,(b<<8)|0xffff00ff))
    # Every channel intensity and every 5-bit channel, isolated, with mask toggled.
    for channel in range(3):
        for tex in range(32):
            for light in range(256):
                ls=[0,0,0];ls[channel]=light
                for mask in [0,0x8000]:cases.append(((tex<<(channel*5))|mask,(ls[0]<<8)|(ls[1]<<24),ls[2]<<8))
    for _ in range(100000):cases.append((rng.getrandbits(32),rng.getrandbits(32),rng.getrandbits(32)))
    results=[]
    for opt in ['-O2','-O3','-Os']:
        baseline_prog=build('baseline',opt);candidate_prog=build('candidate',opt)
        for start in range(0,len(cases),50000):
            chunk=cases[start:start+50000];payload=b''.join(struct.pack('<III',*x) for x in chunk)
            a=execute(baseline_prog,payload,len(chunk));b=execute(candidate_prog,payload,len(chunk))
            assert a==b, 'mismatch '+opt+' chunk '+str(start)
            for i,(src,rg,bl) in enumerate(chunk):
                value=struct.unpack_from('<I',b,i*4)[0]
                expected=(src&0x8000)|min(31,((src&31)*((rg>>8)&255))>>7)|(min(31,(((src>>5)&31)*((rg>>24)&255))>>7)<<5)|(min(31,(((src>>10)&31)*((bl>>8)&255))>>7)<<10)
                assert value==expected,(opt,start+i,src,rg,bl,value,expected)
        sequences=[(0,0,0x00010001,0x00010001,0),(0xffffffff,0xffffffff,0xffffffff,0xffffffff,0x8000),(0x7fff8000,0xabcdff80,0xff81007f,0x0001ff80,0xffff),(0x12345678,0xdeadbeef,0x9e3779b9,0x1234fedc,0x1357)]
        for seq in sequences:
            payload=struct.pack('<IIIII',*seq)
            a=execute(baseline_prog,payload,65536,'sequence');b=execute(candidate_prog,payload,65536,'sequence')
            assert a==b,'sequence mismatch '+opt
        result={'opt':opt,'batch_cases':len(cases),'sequences':4,'pixels_per_sequence':65536,'status':'PASS'}
        results.append(result);print(result,flush=True)
    report={'baseline_revision':baseline_revision,'candidate_header':str(candidate_path),'compiler':command([TC/'arm-none-symbianelf-g++.exe','--version']).decode().splitlines()[0],'baseline_sha256':hashlib.sha256(baseline).hexdigest(),'candidate_sha256':hashlib.sha256(candidate).hexdigest(),'cpu':'Unicorn ARM1136','results':results}
    (OUT/'results.json').write_text(json.dumps(report,indent=2))
if __name__=='__main__': main()
