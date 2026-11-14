# SPDX-License-Identifier: Apache-2.0
# mkcfg.py out.plist [base.plist] — iPhone-like profile: 420v+420f pairs, 4:3 photo format with 12MP stills
#   Keeps everything in base.plist except the formats list (and formatTemplate.PixelFormatType). base defaults to
#   engine/profiles/cfg-iphone.plist, which this script reproduces unchanged; the spikes used m5's build/cfg-m5.plist.
import os, plistlib, sys
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
base = plistlib.load(open(sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, 'engine/profiles/cfg-iphone.plist'), 'rb'))
t = base['formatTemplate']; t['PixelFormatType'] = '$pf'
hd = ["AVCaptureSessionPresetHigh","AVCaptureSessionPresetMedium","AVCaptureSessionPresetLow","AVCaptureSessionPreset1920x1080",
      "AVCaptureSessionPreset1280x720","AVCaptureSessionPreset640x480","AVCaptureSessionPreset352x288","AVCaptureSessionPresetiFrame960x540",
      "AVCaptureSessionPresetiFrame1280x720","AVCaptureSessionPresetInputPriority"]
specs = [  # w, h, fps, presets, default, stills
  (1920,1440,30,["AVCaptureSessionPresetPhoto","AVCaptureSessionPresetInputPriority"],False,[(4032,3024),(1920,1440)]),
  (1920,1080,30,hd,True,[(4032,2268),(1920,1080)]),
  (1920,1080,60,[],False,[(1920,1080)]),
  (3840,2160,30,["AVCaptureSessionPreset3840x2160"],False,[(3840,2160)]),
  (3840,2160,60,[],False,[(3840,2160)]),
]
fmts=[]
for (w,h,fps,pr,d,st) in specs:
  for pf,name in ((875704438,'420v'),(875704422,'420f')):
    fmts.append({'w':w,'h':h,'fps':fps,'presets':pr,'def':d and pf==875704422,'pf':pf,'name':f'{w}x{h}@{fps}-{name}',
                 'extra':{'HighResStillImageDimensions':[{'Width':a,'Height':b} for a,b in st]}})
base['formats']=fmts
plistlib.dump(base, open(sys.argv[1],'wb'))
print(len(fmts),'formats')
