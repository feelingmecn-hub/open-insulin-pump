#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""干净状态: 遍历主菜单全部项。每个 key() 已自带回读, 不再额外 st()。"""
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
st()  # 丢弃初始广播
# 规范化到 HOME
last = None
for _ in range(4): last = key('esc')
print('Normalized:', nm(last))
# 遍历主菜单 6 项
for sel in range(6):
    for _ in range(3): key('esc')        # 回 HOME
    key('set')                            # 进 MENU
    for _ in range(sel): key('down')      # 移到目标项
    r = key('set')                        # 进入该项
    print('MENU item%-2d -> %s' % (sel, nm(r)))
s.close()
