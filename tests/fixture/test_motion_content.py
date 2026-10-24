"""A7 real fixture planes: evaluate warps only on independently stable geometry."""
import argparse
import math
import subprocess
from scene import render


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--runner',required=True)
    args=parser.parse_args()
    counts={}
    for width,height in ((32,24),(64,48),(96,64),(128,96)):
        frames=[render(f,width,height) for f in range(12)]
        for number in range(1,12):
            prior,current=frames[number-1],frames[number]
            if current['reset']:
                continue  # cut/reset rejection is covered in the C++ test
            pp,cp=prior['planes'],current['planes']
            jp,jc=prior['camera']['jitter'],current['camera']['jitter']
            parts=[f'{width} {height} {jp[0]} {jp[1]} {jc[0]} {jc[1]}']
            for i,mv in enumerate(cp['motion']):
                x,y=i%width,i//width
                px,py=x+mv[0]+jp[0]-jc[0],y+mv[1]+jp[1]-jc[1]
                stable=cp['history_valid'][i] and cp['transparent_alpha'][i]==0 and 2<=x<width-2 and 2<=y<height-2
                stable=stable and 0<=px<width-1 and 0<=py<height-1
                if stable:
                    ix,iy=math.floor(px),math.floor(py)
                    footprint=[iy*width+ix,iy*width+ix+1,(iy+1)*width+ix,(iy+1)*width+ix+1]
                    # Independent fixture material/visibility evidence excludes
                    # raster interpolation across checker/surface/glass edges.
                    stable=all(pp['surface_id'][j]==cp['surface_id'][i] and pp['transparent_alpha'][j]==0
                               and pp['scene_color'][j]==cp['scene_color'][i] for j in footprint)
                values=[int(stable)]+pp['scene_color'][i]+cp['scene_color'][i]+mv
                parts.append(' '.join(map(str,values)))
            result=subprocess.run([args.runner],input='\n'.join(parts)+'\n',text=True,capture_output=True,timeout=10)
            if result.returncode:
                raise AssertionError(result.stderr or f'exit {result.returncode}')
            for line in result.stdout.splitlines():
                scale,verdict,checked,error=map(float,line.split())
                counts[(scale,int(verdict))]=counts.get((scale,int(verdict)),0)+1
                supported = verdict in (2,3) and checked>=16
                sparse = verdict==0 and checked<16
                if scale==1 and (not (supported or sparse) or error>1e-5):
                    raise AssertionError(f'correct warp rejected at {number} {width}x{height}: {line}')
    if not any(v for (s,r),v in counts.items() if s!=1 and r==1):
        raise AssertionError('no adversarial motion hypothesis contradicted')
    if not counts.get((1.0,3),0):
        raise AssertionError('no informative fixture warp corroborated')
    print(f'{sum(counts.values())} fixture motion hypotheses evaluated:',sorted(counts.items()))
    print('Flat/checker ambiguities retained; image agreement alone does not establish MV semantics.')


if __name__=='__main__':
    main()
