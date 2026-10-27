from pathlib import Path
import sys,json,numpy as np
sys.path.insert(0,str(Path(__file__).parent/'python-packages'))
import cv2
from PIL import Image

def load(folder,i,name):
 m=json.loads((folder/f'frame{i}-meta.json').read_text())
 p=next(p for p in m['planes'] if p['name']==name)
 fmt=p['dxgi_format']; dtype,n={10:('<f2',4),16:('<f4',2),41:('<f4',1),28:('u1',4),87:('u1',4),24:('<u4',1)}[fmt]
 a=np.fromfile(folder/f'frame{i}-{name}.raw',dtype=dtype)
 a=a[:(p['height']-1)*(p['row_pitch']//np.dtype(dtype).itemsize)+p['width']*n]
 # last row can be unpadded in GetCopyableFootprints total bytes
 stride=p['row_pitch']//np.dtype(dtype).itemsize
 a=np.lib.stride_tricks.as_strided(a,shape=(p['height'],p['width'],n),strides=(p['row_pitch'],n*np.dtype(dtype).itemsize,np.dtype(dtype).itemsize)).astype(np.float32)
 return a,m

def preview(folder):
 for name in ['color','dlssout','backbuffer']:
  a,m=load(folder,0,name)
  if a.shape[2]>=3:
   rgb=a[:,:,:3]
   if name!='backbuffer': rgb=np.power(np.maximum(rgb,0)/(1+np.maximum(rgb,0)),1/2.2)*255
   Image.fromarray(np.uint8(np.clip(rgb,0,255))).save(folder/(name+'.png'))

if __name__=='__main__':
 folder=Path(sys.argv[1]);preview(folder)
 print('metadata',[(i,json.loads((folder/f'frame{i}-meta.json').read_text())['jitter']) for i in range(8)])
 for name in ['color','depth','motion','dlssout']:
  a,m=load(folder,0,name);print(name,a.shape,'finite',float(np.isfinite(a).mean()),'range',np.nanmin(a),np.nanmax(a))
