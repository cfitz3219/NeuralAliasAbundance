"""A6: scan anonymized float snapshots containing fixture cameras and decoys."""
import argparse
import math
import random
import subprocess
from scene import camera


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--runner',required=True)
    args=parser.parse_args()
    found=0
    for width,height in ((32,24),(47,31),(1920,1080)):
        for frame in range(12):
            for column in (False,True):
                cam=camera(frame,width,height)
                rng=random.Random(frame*100+width+int(column))
                # Noise contains no labels and positions vary independently of kind.
                values=[rng.uniform(2,9) for _ in range(512)]
                slots=rng.sample(range(8,480,20),3)
                projection=cam['projection'][:]
                projection[2]+=2*cam['jitter'][0]/width
                projection[6]-=2*cam['jitter'][1]/height
                # Object transform intentionally has a camera-like rigid shape;
                # structural analysis cannot establish its semantics.
                obj=cam['view'][:]; obj[3]=7.5
                for slot,matrix in zip(slots,(projection,cam['view'],obj)):
                    values[slot:slot+16]=[matrix[c*4+r] if column else matrix[r*4+c]
                                          for r in range(4) for c in range(4)]
                result=subprocess.run([args.runner],input=str(len(values))+'\n'+' '.join(map(str,values)),
                                      text=True,capture_output=True,timeout=10)
                if result.returncode:
                    raise AssertionError(result.stderr or f'runner exit {result.returncode}')
                rows=[list(map(float,line.split())) for line in result.stdout.splitlines()]
                def at(slot,kind):
                    matches=[r for r in rows if r[0]==slot*4 and r[1]==kind and r[2]==int(column)]
                    if len(matches)!=1:
                        raise AssertionError(f'expected unique layout hypothesis at {slot}, got {matches}')
                    return matches[0]
                p=at(slots[0],0)
                if not math.isclose(p[3],cam['near'],abs_tol=1e-6) or not math.isclose(p[4],cam['far'],abs_tol=0.001):
                    raise AssertionError('wrong near/far')
                if abs(p[8]*width/2-cam['jitter'][0])>1e-6 or abs(-p[9]*height/2-cam['jitter'][1])>1e-6:
                    raise AssertionError('wrong projection offset')
                v=at(slots[1],1)
                if any(abs(a-b)>1e-5 for a,b in zip(v[5:8],cam['position'])):
                    raise AssertionError('wrong view inverse position')
                at(slots[2],1)  # semantic decoy remains a hypothesis, never a selected camera
                found+=3
    print(f'{found} embedded matrix hypotheses found across 72 snapshots; '
          'near/far, view position and projection offsets agree. '
          'Object transforms remain ambiguous; no live camera qualification.')


if __name__=='__main__':
    main()
