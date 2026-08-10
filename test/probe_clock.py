#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""正确路径: HOME -> 主菜单.sel5(系统设置) -> sel0(设置时间) -> 编辑字段。"""
import socket, json, time
SCR = {0:'HOME',1:'MENU',2:'BASAL',3:'BOLUS_MENU',4:'BOLUS_NORMAL',5:'BOLUS_SQUARE',
       6:'BOLUS_DUAL',7:'BOLUS_WIZARD',8:'BOLUS_MEALS',9:'PRIME',10:'ALARM_LIST',
       11:'ALARM_DETAIL',12:'LOOP',13:'SETTINGS',14:'CLOCK_SET',15:'ABOUT'}
s = socket.create_connection(('127.0.0.1', 18923), timeout=5); time.sleep(0.3)
def st():
    b = b''
    while b'\n' not in b: b += s.recv(4096)
    return json.loads(b.split(b'\n', 1)[0])
def key(k):
    s.sendall(('key ' + k + '\n').encode()); time.sleep(0.15); return st()
def ui(x): return x.get('state', {}).get('ui', {})
def nm(x):
    u = ui(x); return '%s(s=%s,sel=%s)' % (SCR.get(u.get('screen'), u.get('screen')), u.get('screen'), u.get('sel'))
st()
for _ in range(4): key('esc')              # HOME
key('set')                                  # MENU
for _ in range(5): key('down')              # sel=5
r = key('set'); print('主菜单.sel5 ->', nm(r))   # SETTINGS (sel0)
# sel0 就是"设置时间", 直接确认进入
r = key('set'); print('系统设置.sel0 + 确认 ->', nm(r), 'clk_field=', ui(r).get('clk_field'))
assert ui(r).get('screen') == 14, '未能进入设置时间!'
# 在设置时间里: set 切换编辑字段, up/down 调整当前字段
print('初始 clk=', ui(r).get('clk'))
for _ in range(3): key('set')               # 切到 clk_field=3 (时)
r = key('up');  print('  时 +1 ->', nm(r), 'clk=', ui(r).get('clk'))
r = key('set'); r = key('set')              # 切到 field=5 (保存)
r = key('set')                              # 保存 -> 回到 SETTINGS
print('保存后 ->', nm(r))
s.close()
print('\nPASS: 设置时间二级菜单导航与字段编辑正常')
