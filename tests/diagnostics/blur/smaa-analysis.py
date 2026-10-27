from analyze import *
f=Path('local/blur-isolation')
for folder in ['jitter-on','smaa-off']:
 print(folder)
 for plane in ['color','dlssout','backbuffer']:
  a,_=load(f/folder,3 if folder=='smaa-off' else 0,plane);a=a[350:535,1660:1950,:3]
  if plane=='backbuffer':a/=255
  else:a=np.power(np.maximum(a,0)/(1+np.maximum(a,0)),1/2.2)
  print(plane,'gradient',np.mean(np.abs(np.diff(a,axis=1))))
# Per-frame double-composed motion, mathematical control of same captured matrices.
folder=f/'smaa-off'
for i in range(8):
 mat=np.loadtxt(folder/f'frame{i}-camera.txt');inv,prev,cur=[a.reshape(4,4) for a in mat];dep,meta=load(folder,i,'depth');j=meta['jitter'];y,x=np.mgrid[:1440,:3440];p=np.stack(((x+.5-j[0])/3440*2-1,1-(y+.5-j[1])/1440*2,dep[:,:,0],np.ones(x.shape)),axis=-1).astype(np.float32)
 clip=p@((inv@prev).astype(np.float32));q=clip[:,:,:2]/clip[:,:,3:4];v=np.stack(((q[:,:,0]+1)*1720-(x+.5-j[0]),(1-q[:,:,1])*720-(y+.5-j[1])),axis=-1).astype(np.float32)
 padded=np.zeros((1440,27648//4),dtype='<f4');padded[:,:3440*2]=v.reshape(1440,-1);raw=padded.tobytes()[:39812992];(folder/f'frame{i}-motion-corrected.raw').write_bytes(raw)
