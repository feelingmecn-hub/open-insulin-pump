#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""验证基础率段编辑: 进入段 -> 调速率 -> 保存落盘 -> 重进确认 -> ESC 取消不落盘。"""
import socket, json, time, sys
SCR_BASAL = 2
s = socket.create_connection(('127.0.0.1', 18923), timeout=5); time.sleep(0.5)
def st():
    b = b''
    while b'\n' not in b: b += s.recv(4096)
    return json.loads(b.split(b'\n', 1)[0])
def key(k):
    s.sendall(('key ' + k + '\n').encode()); time.sleep(0.2); return st()
def ui(x): return x.get('state', {}).get('ui', {})
st()  # 丢弃初始广播
for _ in range(4): key('esc')              # 回 HOME
key('set')                                  # 主菜单
# 已在 MENU sel0 = 基础率 (home set 进 menu, sel=0); 确认进 BASAL
r = key('set')
assert ui(r).get('screen') == SCR_BASAL, '未进入基础率页'
print('进入基础率页, sel=%s' % ui(r).get('sel'))

def enter_basal_edit():
    """确保进入当前段的编辑态 (兼容 AAPS/本地模式)。"""
    r = key('set')
    if ui(r).get('set_edit') == 1:
        return r
    # AAPS 接管: 第一次 set 切到本地, 再 set 进入编辑
    r = key('set')
    assert ui(r).get('set_edit') == 1, '无法进入段编辑'
    return r

# --- 测试1: sel0 编辑并保存 ---
r0 = enter_basal_edit()
rate0 = ui(r0).get('sel_rate')
print('编辑 sel0, 进入前速率=%.2f' % rate0)
r1 = key('up'); r2 = key('up'); r3 = key('up')   # +0.15
rate_up = ui(r3).get('sel_rate')
print('upx3 后 sel_rate=%.2f (期望 %.2f)' % (rate_up, rate0 + 0.15))
assert abs(rate_up - (rate0 + 0.15)) < 1e-6, 'up 调节步长不对'
r_save = key('set')   # 保存
assert ui(r_save).get('set_edit') == 0, '保存后应退出编辑'
# 重进确认持久化
r_re = enter_basal_edit()
rate_re = ui(r_re).get('sel_rate')
print('重进 sel0, sel_rate=%.2f (期望 %.2f)' % (rate_re, rate0 + 0.15))
assert abs(rate_re - (rate0 + 0.15)) < 1e-6, '保存未持久化!'
key('esc')   # 退出编辑
print('测试1 PASS: 段内速率可编辑并落盘持久化')

# --- 测试2: ESC 取消不落盘 ---
key('down')                                  # 移到 sel1
r_e = enter_basal_edit()
rate1 = ui(r_e).get('sel_rate')
key('up'); key('up')                         # 改 +0.10
rate_changed = ui(key('up')).get('sel_rate')
assert abs(rate_changed - (rate1 + 0.15)) < 1e-6, 'ESC前调节异常'
r_cancel = key('esc')                        # 取消
assert ui(r_cancel).get('set_edit') == 0, 'ESC 应退出编辑'
r_re2 = enter_basal_edit()
rate_re2 = ui(r_re2).get('sel_rate')
print('ESC取消后重进 sel1, sel_rate=%.2f (期望原值 %.2f)' % (rate_re2, rate1))
assert abs(rate_re2 - rate1) < 1e-6, 'ESC 取消却落盘了!'
key('esc')
print('测试2 PASS: ESC 取消不写入')

# --- 测试3: 下限夹紧 (到 0 不再减) ---
# 当前在 BASAL 列表(上一测试已退回列表), key('esc') 会退回主菜单, 需重新进入基础率
key('esc')                                   # BASAL 列表 -> 主菜单
key('set')                                   # 主菜单
key('set')                                   # sel0 = 基础率, 进入 BASAL(sel0)
r_z = enter_basal_edit()
print('sel0 当前速率=%.2f, 连续 down 压下限' % ui(r_z).get('sel_rate'))
for _ in range(60): key('down')              # 远超下限
rate_min = ui(key('down')).get('sel_rate')
print('下限测试 sel_rate=%.2f (期望 0.00)' % rate_min)
assert rate_min >= 0.0, '速率出现负数!'
assert abs(rate_min - 0.0) < 1e-6, '下限未夹紧到 0'
key('esc')
print('测试3 PASS: 速率下限夹紧到 0')

s.close()
print('\nALL BASAL EDIT TESTS PASSED')
sys.exit(0)
