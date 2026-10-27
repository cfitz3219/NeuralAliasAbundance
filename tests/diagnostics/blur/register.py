from analyze import *
patches={'roof':(1660,350,1950,535),'cliff':(2110,90,2450,330),'dock':(1670,650,2070,710),'left_house':(760,240,1130,390)}
def gray(a): return np.log1p(np.maximum(a[:,:,:3].mean(2),0)) if a.shape[2]>1 else a[:,:,0]*100
crit=(cv2.TERM_CRITERIA_EPS|cv2.TERM_CRITERIA_COUNT,150,1e-7)
def shift(a,b):
 w=np.eye(2,3,dtype=np.float32)
 cc,w=cv2.findTransformECC(a,b,w,cv2.MOTION_TRANSLATION,crit,None,3)
 return [round(float(cc),6),*np.round(w[:,2],4).tolist()]
rng=np.random.default_rng(1); a=cv2.GaussianBlur(rng.random((200,200),dtype=np.float32),(5,5),1)
b=cv2.warpAffine(a,np.float32([[1,0,.5],[0,1,-.25]]),(200,200),borderMode=cv2.BORDER_REFLECT)
print('calibration +.5,-.25:',shift(a[10:-10,10:-10],b[10:-10,10:-10]))
for folder in map(Path,sys.argv[1:]):
 print('\nFOLDER',folder)
 j=np.array([json.loads((folder/f'frame{i}-meta.json').read_text())['jitter'] for i in range(8)])
 print('jitter delta',np.round(j-j[0],4).tolist())
 for plane in ['color','depth']:
  frames=[gray(load(folder,i,plane)[0]) for i in range(8)]
  for name,(x,y,X,Y) in patches.items():
   a=frames[0][y:Y,x:X].copy()
   try: print(plane,name,[shift(a,f[y:Y,x:X].copy()) for f in frames])
   except Exception as e: print(e)
 for i in range(8):
  m,_=load(folder,i,'motion');x,y,X,Y=patches['roof'];print('motion roof',i,np.round(np.median(m[y:Y,x:X],axis=(0,1)),5).tolist())
