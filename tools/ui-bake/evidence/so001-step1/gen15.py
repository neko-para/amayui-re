# -*- coding: utf-8 -*-
"""
Generate so001_render15.html with all 15 blocks of Chinese text over the cleaned base image.
Pads positions inside each 77x78 button, centers text, scales width per char count.
"""
import io

ROWS = [795, 878, 961, 1044, 1127]
COLS = [[603, 'warm'], [685, 'cyan'], [767, 'gray']]
TEXTS = ["变更设置", "初始化", "全部移除", "关闭", "返回"]

def sc(t):
    return 0.6 if len(t) >= 4 else 0.8

def layers(mode, txt):
    if mode == 'warm':
        cls = ['w-shd', 'w-out', 'w-gin']
    elif mode == 'cyan':
        cls = ['c-out', 'c-gin']
    else:
        cls = ['g-out', 'g-gin']
    return ''.join(f'<span class="ly {c}">{txt}</span>' for c in cls)

spans = []
for ri, y0 in enumerate(ROWS):
    txt = TEXTS[ri]
    s = sc(txt)
    for x0, mode in COLS:
        left = x0 - 8
        top = y0 + 24
        inner = layers(mode, txt)
        spans.append(
            f'      <span class="tb" style="left:{left}px;top:{top}px;width:93px;height:26px;transform:scaleX({s});">{inner}</span>')
SPANS = "\n".join(spans)

CSS = r"""
  @font-face{font-family:"Sarasa Gothic SC";src:url("../res/fonts/SarasaGothicSC/SarasaGothicSC-Regular.ttf");font-weight:400;}
  @font-face{font-family:"Sarasa Gothic SC";src:url("../res/fonts/SarasaGothicSC/SarasaGothicSC-Bold.ttf");font-weight:700;}
  html,body{margin:0;padding:0;width:1280px;height:1792px;background:transparent;overflow:hidden;}
  #bg{position:absolute;left:0;top:0;}
  .tb{position:absolute;display:grid;place-items:center;transform-origin:center;
      font-family:"Sarasa Gothic SC";font-weight:700;font-size:22px;letter-spacing:1px;
      white-space:nowrap;line-height:22px;}
  .ly{grid-area:1/1;}
  .w-shd{color:transparent;-webkit-text-stroke:0;text-shadow:2px 2px 1px rgba(0,0,0,1);}
  .w-out{color:transparent;-webkit-text-stroke:1.5px #3A2709;}
  .w-gin{color:transparent;-webkit-text-fill-color:transparent;
         background:linear-gradient(to bottom,#FFFFFF 0%,#FDD896 30%,#FBC568 62%,#FDAB0A 100%);
         -webkit-background-clip:text;background-clip:text;}
  .c-out{color:transparent;-webkit-text-stroke:1.5px #1438E0;}
  .c-gin{color:transparent;-webkit-text-fill-color:transparent;
         background:linear-gradient(to bottom,#FFFFFF 0%,#C8ECFF 30%,#4FD4FF 70%,#01FEFF 100%);
         -webkit-background-clip:text;background-clip:text;}
  .g-out{color:transparent;-webkit-text-stroke:1px rgba(0,0,0,0.6);}
  .g-gin{-webkit-text-stroke:0;
         background:linear-gradient(to bottom,rgba(0,0,0,0) 0%,rgba(0,0,0,0.22) 45%,rgba(0,0,0,0.45) 100%);
         -webkit-background-clip:text;background-clip:text;-webkit-text-fill-color:transparent;color:transparent;}
"""

html = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>SO001 15块渲染</title>
<style>{CSS}</style>
</head>
<body>
  <img id="bg" src="so001_clean_full.png">
{SPANS}
</body>
</html>
"""

with io.open(r'.tmp\so001_render15.html', 'w', encoding='utf-8') as f:
    f.write(html)
print('wrote so001_render15.html, bytes', len(html.encode('utf-8')))
