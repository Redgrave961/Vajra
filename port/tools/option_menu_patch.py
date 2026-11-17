import struct
import sys
from pathlib import Path


class Asm:
    def __init__(self):
        self.items = []
        self.count = 0

    def label(self):
        self.count += 1
        return self.count

    def op(self, code, payload=b''):
        self.items.append(bytes([code]) + (struct.pack('<H', len(payload)) + payload if code >= 0x80 else b''))

    def branch(self, code, target):
        self.items.append((code, target))

    def mark(self, target):
        self.items.append(('mark', target))

    def assemble(self):
        offsets, at = {}, 0
        for item in self.items:
            if isinstance(item, bytes):
                at += len(item)
            elif item[0] == 'mark':
                offsets[item[1]] = at
            else:
                at += 5
        out, at = bytearray(), 0
        for item in self.items:
            if isinstance(item, bytes):
                out += item
                at += len(item)
            elif item[0] != 'mark':
                at += 5
                out += bytes([item[0]]) + struct.pack('<Hh', 2, offsets[item[1]] - at)
        return bytes(out)


class R:
    def __init__(self, n):
        self.n = n


THIS = R(1)
UNDEFINED = object()


def value(v):
    if v is UNDEFINED:
        return bytes([3])
    if isinstance(v, R):
        return bytes([4, v.n])
    if isinstance(v, bool):
        return bytes([5, int(v)])
    if isinstance(v, int):
        return bytes([7]) + struct.pack('<i', v)
    if isinstance(v, float):
        return bytes([6]) + struct.pack('<d', v)[4:] + struct.pack('<d', v)[:4]
    return bytes([0]) + v.encode('latin-1') + b'\0'


class Gen:
    def __init__(self):
        self.a = Asm()

    def push(self, *vals):
        self.a.op(0x96, b''.join(value(v) for v in vals))

    def emit(self, e):
        if isinstance(e, tuple):
            e[0](self, *e[1:])
        else:
            self.push(e)

    def stmt(self, e):
        self.emit(e)
        self.a.op(0x17)


def g_var(g, name):
    g.push(name)
    g.a.op(0x1C)


def g_get(g, obj, name):
    g.emit(obj)
    g.emit(name)
    g.a.op(0x4E)


def g_set(g, obj, name, val):
    g.emit(obj)
    g.emit(name)
    g.emit(val)
    g.a.op(0x4F)


def g_call(g, obj, method, *args):
    for x in reversed(args):
        g.emit(x)
    g.push(len(args))
    g.emit(obj)
    g.push(method)
    g.a.op(0x52)


def g_binop(g, code, x, y):
    g.emit(x)
    g.emit(y)
    g.a.op(code)


def g_not(g, x):
    g.emit(x)
    g.a.op(0x12)


def g_and(g, x, y):
    end = g.a.label()
    g.emit(x)
    g.a.op(0x4C)
    g.a.op(0x12)
    g.a.branch(0x9D, end)
    g.a.op(0x17)
    g.emit(y)
    g.a.mark(end)


def g_new(g, cls):
    g.push(0, cls)
    g.a.op(0x40)


def g_object(g, *pairs):
    for k, v in pairs:
        g.push(k)
        g.emit(v)
    g.push(len(pairs))
    g.a.op(0x43)


def g_store(g, reg, val):
    g.emit(val)
    g.a.op(0x87, bytes([reg.n]))


def g_func(g, params, regs, body):
    inner = Gen()
    body(inner)
    code = inner.a.assemble()
    head = b'\0' + struct.pack('<HB', len(params), regs) + bytes([0x29, 0])
    for i, p in enumerate(params):
        head += bytes([i + 2]) + p.encode('latin-1') + b'\0'
    g.a.op(0x8E, head + struct.pack('<H', len(code)))
    g.a.items.append(code)


def var(name): return (g_var, name)
def get(obj, *names):
    for n in names:
        obj = (g_get, obj, n)
    return obj
def setm(obj, name, val): return (g_set, obj, name, val)
def call(obj, method, *args): return (g_call, obj, method, *args)
def add(x, y): return (g_binop, 0x47, x, y)
def sub(x, y): return (g_binop, 0x0B, x, y)
def mul(x, y): return (g_binop, 0x0C, x, y)
def div(x, y): return (g_binop, 0x0D, x, y)
def less(x, y): return (g_binop, 0x48, x, y)
def eq(x, y): return (g_binop, 0x49, x, y)
def neq(x, y): return (g_not, eq(x, y))
def ge(x, y): return (g_not, less(x, y))
def land(x, y): return (g_and, x, y)
def new(cls): return (g_new, cls)
def obj(*pairs): return (g_object, *pairs)
def store(reg, val): return (g_store, reg, val)
def func(params, regs, body): return (g_func, params, regs, body)
def member(o, name): return (g_get, o, name)


OM = var('OptionMenu')
PM = var('PauseMenu')
EI = get(var('flash'), 'external', 'ExternalInterface')
GAME = var('BCGame')
TAG = 'PCOpt'


def ei(name, *args): return call(EI, 'call', name, *args)
def proto(pkg, cls): return get(pkg, cls, 'prototype')
def text(tag): return call(GAME, 'GetText', tag)


def if_(g, cond, then, other=None):
    skip, end = g.a.label(), g.a.label()
    g.emit(cond)
    g.a.op(0x12)
    g.a.branch(0x9D, skip)
    then(g)
    if other:
        g.a.branch(0x99, end)
    g.a.mark(skip)
    if other:
        other(g)
        g.a.mark(end)


def loop(g, reg, limit, body):
    top, end = g.a.label(), g.a.label()
    g.stmt(store(reg, 0))
    g.a.mark(top)
    g.emit(less(reg, limit))
    g.a.op(0x12)
    g.a.branch(0x9D, end)
    body(g)
    g.emit(reg)
    g.a.op(0x50)
    g.a.op(0x87, bytes([reg.n]))
    g.a.op(0x17)
    g.a.branch(0x99, top)
    g.a.mark(end)


def ret(g, val):
    g.emit(val)
    g.a.op(0x3E)


def define_class(g, pkg, name, base, ctor_regs, ctor):
    g.emit(setm(pkg, name, func([], ctor_regs, ctor)))
    g.emit(get(pkg, name))
    g.emit(get(pkg, base))
    g.a.op(0x69)


def method(g, pkg, cls, name, params, regs, body):
    g.emit(setm(proto(pkg, cls), name, func(params, regs, body)))


def wrap(g, pkg, cls, name):
    g.emit(setm(proto(pkg, cls), 'PCOpt_' + name, get(proto(pkg, cls), name)))


def register(g, pkg, symbol, cls):
    g.stmt(call(var('Object'), 'registerClass', symbol, get(pkg, cls)))


def base_call(pkg, cls, name):
    return lambda g: g.stmt(call(get(proto(pkg, cls), name), 'call', THIS))


def row_values(g, i, count, getf, param):
    g.stmt(store(count, ei('PCOpt_Count', i)))
    g.stmt(store(getf, add('PCOpt_Get', i)))
    g.stmt(store(param, call(GAME, 'CallInt', getf)))


def cycle_select(g, p, active):
    g.emit(setm(THIS, 'm_active', eq(active, True)))
    if_(g, land(get(THIS, 'm_active'), neq(get(THIS, 'm_param'), p)),
        lambda g: g.stmt(call(GAME, 'Call', 'OnSoundSubSelect')))
    g.emit(setm(THIS, 'm_param', p))
    return ei('PCOpt_Label', get(THIS, 'm_row'), p)


def layout(visible, pitch, first, members):
    def body(g):
        n, top, i, y, vis, mc = (R(k) for k in range(2, 8))
        g.stmt(store(n, get(THIS, 'm_details', 'length')))
        g.stmt(store(top, sub(get(THIS, 'm_cursor'), 1)))
        if_(g, less(sub(n, visible), top), lambda g: g.stmt(store(top, sub(n, visible))))
        if_(g, less(top, 0), lambda g: g.stmt(store(top, 0)))

        def row(g):
            g.stmt(store(y, sub(mul(sub(i, top), pitch), first)))
            g.stmt(store(vis, land(ge(i, top), less(i, add(top, visible)))))
            for clip, dy in members:
                g.stmt(store(mc, clip(i)))
                if dy is not None:
                    g.emit(setm(mc, '_y', add(y, dy) if dy else y))
                g.emit(setm(mc, '_visible', vis))

        loop(g, i, n, row)
    return body


def scrolled_cursor(pkg, base):
    return lambda g: (base_call(pkg, base, 'SetCursor')(g), g.stmt(call(THIS, 'Layout')))


def tag(code, payload):
    if len(payload) < 0x3F and code not in (39, 59):
        return struct.pack('<H', code << 6 | len(payload)) + payload
    return struct.pack('<HI', code << 6 | 0x3F, len(payload)) + payload


def sprite(cid, body):
    return tag(39, struct.pack('<HH', cid, 1) + body + tag(1, b'') + tag(0, b''))


def matrix(sx=1.0, tx=0.0, ty=0.0):
    bits = ''
    if sx != 1.0:
        scale = round(sx * 65536)
        bits += '1' + format(18, '05b') + format(scale & 0x3FFFF, '018b') + format(65536, '018b')
    else:
        bits += '0'
    tw = [round(v * 20) for v in (tx, ty)]
    nbits = max((abs(v).bit_length() + 1 for v in tw), default=0) if any(tw) else 0
    bits += '0' + format(nbits, '05b') + ''.join(format(v & ((1 << nbits) - 1), '0%db' % nbits) for v in tw if nbits)
    bits += '0' * (-len(bits) % 8)
    return bytes(int(bits[k:k + 8], 2) for k in range(0, len(bits), 8))


def place(depth, cid, m, name=None):
    flags = 0x06 | (0x20 if name else 0)
    return tag(26, bytes([flags]) + struct.pack('<HH', depth, cid) + m + (name.encode() + b'\0' if name else b''))


def export(*pairs):
    return tag(56, struct.pack('<H', len(pairs)) + b''.join(struct.pack('<H', i) + n.encode() + b'\0' for i, n in pairs))


def init_action(cid, g):
    g.a.op(0)
    return tag(59, struct.pack('<H', cid) + g.a.assemble())


TITLE_PAGE = 128
TITLE_CYCLE = 129
TITLE_CHOICE = 46
TITLE_VISIBLE = 3
TITLE_PITCH = 110
TITLE_FIRST = 135
TITLE_SEL_DY = 55
TITLE_LIST_SPAN = 300.0


def title_cycle_set_param(g):
    p, active, open_ = R(2), R(3), R(4)
    g.stmt(call(THIS, 'SetText', 'mcInst_ChoiceL', cycle_select(g, p, active)))
    g.stmt(call(THIS, 'ChoiceL', get(THIS, 'm_active'), eq(open_, True)))


def title_page_ctor(g):
    for k, v in (('m_input', False), ('m_repeat', 0), ('m_cursor', 0), ('m_active', False)):
        g.emit(setm(THIS, k, v))


def title_page_init(g):
    n, i, title, sel, s, count, getf, param = (R(k) for k in range(2, 10))
    g.stmt(store(n, ei('PCOpt_Rows')))
    g.emit(setm(THIS, 'm_details', new('Array')))

    def toggle(g):
        g.stmt(call(THIS, 'attachMovie', 'mc_Select_Choice2LR_unit', sel, call(THIS, 'getNextHighestDepth')))
        g.stmt(call(THIS, 'InitOptionParam', sel, param, text('Option.OptionMenu.OpOn'), text('Option.OptionMenu.OpOff')))

    def cycle(g):
        g.stmt(store(s, call(THIS, 'attachMovie', 'PCOpt_Cycle', sel, call(THIS, 'getNextHighestDepth'))))
        g.emit(setm(s, 'm_row', i))
        g.emit(setm(s, 'm_count', count))
        g.stmt(call(s, 'SetParam', param))

    def row(g):
        g.stmt(store(title, add('mcInst_Title', i)))
        g.stmt(store(sel, add('mcInst_Sel', i)))
        row_values(g, i, count, getf, param)
        g.stmt(call(THIS, 'attachMovie', 'mc_Text', title, call(THIS, 'getNextHighestDepth')))
        g.stmt(call(THIS, 'SetDetailTitle', title, ei('PCOpt_Title', i)))
        if_(g, eq(count, 0), toggle, cycle)
        g.stmt(call(get(THIS, 'm_details'), 'push', obj(
            ('Title', title), ('Sel', sel), ('KeepParam', param), ('GetFunc', getf),
            ('SetFunc', add('PCOpt_Set', i)), ('HelpText', ei('PCOpt_Help', i)))))

    loop(g, i, n, row)
    g.emit(setm(THIS, 'm_cursor', 0))
    g.stmt(call(THIS, 'Layout'))


def title_details_init(g):
    page = R(2)
    main = get(OM, 'OptionMain')
    g.stmt(call(THIS, 'PCOpt_Init'))
    g.stmt(store(page, call(THIS, 'attachMovie', 'PCOpt_Page', 'mcInst_DetailPC', call(THIS, 'getNextHighestDepth'))))
    g.stmt(call(page, 'Init'))
    g.stmt(call(page, 'SetMenuMC', get(THIS, 'm_instMenu')))
    g.emit(setm(page, '_visible', False))
    g.emit(setm(get(THIS, 'm_detailList'), get(main, 'CLAUSE_ID_DEFAULT'), obj(('Lb', 'detail07'))))
    g.emit(setm(get(THIS, 'm_detailList'), get(main, 'CLAUSE_ID_GRAPHICS'), obj(('mc', page), ('Lb', 'detail07'))))


def title_details_set(g):
    cid = R(2)
    g.stmt(call(THIS, 'PCOpt_SetDetails', cid))
    g.emit(setm(get(THIS, 'mcInst_DetailPC'), '_visible', eq(cid, get(OM, 'OptionMain', 'CLAUSE_ID_GRAPHICS'))))


def title_selector_add(g):
    cid, label, help_, count, k = R(2), R(3), R(4), R(5), R(6)
    main = get(OM, 'OptionMain')

    def graphics(g):
        g.stmt(store(count, add(get(THIS, 'm_items', 'length'), 2)))
        g.stmt(call(THIS, 'attachMovie', 'mc_Text', add('mcInst_Sel', count), call(THIS, 'getNextHighestDepth')))

        def slot(g):
            g.emit(setm(member(THIS, add('mcInst_Sel', add(k, 1))), '_y',
                        sub(div(mul(k, TITLE_LIST_SPAN), sub(count, 1)), TITLE_LIST_SPAN / 2)))

        loop(g, k, count, slot)
        g.stmt(call(THIS, 'PCOpt_AddSelect', get(main, 'CLAUSE_ID_GRAPHICS'), ei('PCOpt_Page'), ei('PCOpt_PageHelp')))

    if_(g, eq(cid, get(main, 'CLAUSE_ID_DEFAULT')), graphics)
    g.stmt(call(THIS, 'PCOpt_AddSelect', cid, label, help_))


def title_tags():
    g = Gen()
    main = get(OM, 'OptionMain')
    g.emit(setm(main, 'CLAUSE_ID_GRAPHICS', add(get(main, 'CLAUSE_ID_DEFAULT'), 1)))

    define_class(g, OM, 'OptionCycle', 'OptionChoice', 2, lambda g: None)
    method(g, OM, 'OptionCycle', 'SetParam', ['Prm', 'bActive', 'bOpen'], 5, title_cycle_set_param)
    method(g, OM, 'OptionCycle', 'GetParamMax', [], 2, lambda g: ret(g, get(THIS, 'm_count')))
    register(g, OM, 'PCOpt_Cycle', 'OptionCycle')

    define_class(g, OM, 'DetailGraphics', 'OptionDetail', 2, title_page_ctor)
    method(g, OM, 'DetailGraphics', 'Init', [], 10, title_page_init)
    method(g, OM, 'DetailGraphics', 'Layout', [], 8, layout(TITLE_VISIBLE, TITLE_PITCH, TITLE_FIRST, (
        (lambda i: member(THIS, get(member(get(THIS, 'm_details'), i), 'Title')), 0),
        (lambda i: member(THIS, get(member(get(THIS, 'm_details'), i), 'Sel')), TITLE_SEL_DY))))
    method(g, OM, 'DetailGraphics', 'SetCursor', [], 2, scrolled_cursor(OM, 'OptionDetail'))
    method(g, OM, 'DetailGraphics', 'DefaultSetting', [], 2,
           lambda g: (g.stmt(ei('PCOpt_Default')), base_call(OM, 'OptionDetail', 'DefaultSetting')(g)))
    method(g, OM, 'DetailGraphics', 'IsChangeParam', [], 2, lambda g: ret(g, False))
    method(g, OM, 'DetailGraphics', 'Reset', [], 2, lambda g: None)
    register(g, OM, 'PCOpt_Page', 'DetailGraphics')

    wrap(g, OM, 'OptionDetails', 'Init')
    method(g, OM, 'OptionDetails', 'Init', [], 3, title_details_init)
    wrap(g, OM, 'OptionDetails', 'SetDetails')
    method(g, OM, 'OptionDetails', 'SetDetails', ['ClauseID'], 3, title_details_set)
    wrap(g, OM, 'OptionSelector', 'AddSelect')
    method(g, OM, 'OptionSelector', 'AddSelect', ['ClauseID', 'Text', 'HelpText'], 7, title_selector_add)

    choice = place(1, TITLE_CHOICE, matrix(), 'mcInst_ChoiceL')
    return (sprite(TITLE_PAGE, b'') + sprite(TITLE_CYCLE, choice)
            + export((TITLE_PAGE, 'PCOpt_Page'), (TITLE_CYCLE, 'PCOpt_Cycle')) + init_action(TITLE_PAGE, g))


PAUSE_PAGE = 123
PAUSE_SLOT = 124
PAUSE_SLOT_BASE = 39
PAUSE_SLOT_LINE = 40
PAUSE_VISIBLE = 3
PAUSE_PITCH = 90
PAUSE_FIRST = 90
PAUSE_SLOT_PITCH = 50
PAUSE_SLOT_FIRST = 50
PAUSE_BASE_DEPTH = 9
PAUSE_ITEM_DEPTH = 10
PAUSE_BASE_SLOTS = 3
CONTROLS_LABEL = 'loop'


def pause_cycle_set_param(g):
    p, active, open_, state = R(2), R(3), R(4), R(5)
    g.stmt(store(state, 'off'))
    label = cycle_select(g, p, active)
    if_(g, get(THIS, 'm_active'), lambda g: g.stmt(store(state, 'on')))
    g.emit(setm(get(THIS, 'mcInst_TextL', 'mcInst_Text', 'mcInst_Text'), 'text', label))
    g.emit(setm(get(THIS, 'mcInst_TextL'), '_visible', True))
    g.emit(setm(get(THIS, 'mcInst_TextR'), '_visible', False))
    g.emit(setm(get(THIS, 'mcInst_Cursor'), '_visible', False))
    g.stmt(call(get(THIS, 'mcInst_TextL'), 'gotoAndPlay', state))
    g.stmt(call(THIS, 'gotoAndStop', state))


def pause_page_ctor(g):
    for k, v in (('m_details', new('Array')), ('m_input', False), ('m_repeat', 0), ('m_cursor', 0)):
        g.emit(setm(THIS, k, v))


def pause_page_init(g):
    n, i, item, s, count, getf, param, on, off = (R(k) for k in range(2, 11))
    page = get(PM, 'DetailGraphics')
    g.stmt(store(n, ei('PCOpt_Rows')))

    def toggle(g):
        g.stmt(store(on, 'On'))
        g.stmt(store(off, 'Off'))

    def cycle(g):
        g.stmt(store(on, ''))
        g.stmt(store(off, ''))
        g.stmt(store(s, get(item, 'mcInst_Select')))
        for k, v in (('m_row', i), ('m_count', count), ('SetParam', get(page, 'CycleSetParam')),
                     ('GetParamMax', get(page, 'CycleParamMax'))):
            g.emit(setm(s, k, v))
        g.emit(setm(get(s, 'mcInst_TextL'), '_x', 0))

    def row(g):
        g.stmt(store(item, call(THIS, 'attachMovie', 'TextR', add('mcInst_Sel', add(i, 1)), call(THIS, 'getNextHighestDepth'))))
        row_values(g, i, count, getf, param)
        if_(g, eq(count, 0), toggle, cycle)
        g.stmt(call(THIS, 'AddDetail', get(PM, 'OptionSelectItem', 'TYPE_CHOICE'), ei('PCOpt_Title', i), on, off,
                    ei('PCOpt_Help', i), add('PCOpt_Set', i), getf))

        def reveal(g):
            for side in ('mcInst_TextL', 'mcInst_TextR'):
                g.emit(setm(get(item, 'mcInst_Select', side), '_visible', True))

        if_(g, eq(count, 0), reveal)

    loop(g, i, n, row)
    g.emit(setm(THIS, 'm_cursor', 0))
    g.stmt(call(THIS, 'Layout'))


def pause_selector_init(g):
    page = R(2)
    details = get(THIS, 'm_instDetails')
    g.stmt(call(THIS, 'PCOpt_Init'))
    if_(g, neq(get(THIS, 'm_pcPage'), UNDEFINED), lambda g: g.stmt(call(get(THIS, 'm_pcPage'), 'removeMovieClip')))
    g.stmt(store(page, call(details, 'attachMovie', 'PCOpt_PausePage', 'mcInst_PCOpt', call(details, 'getNextHighestDepth'))))
    g.stmt(call(page, 'Init'))
    g.emit(setm(page, '_visible', False))
    g.emit(setm(THIS, 'm_pcPage', page))


def pause_selector_change(g):
    tag_, shown = R(2), R(3)
    details = get(THIS, 'm_instDetails')
    g.stmt(call(THIS, 'PCOpt_ChangeOptionDetails', tag_))
    g.stmt(store(shown, eq(tag_, TAG)))
    g.emit(setm(get(THIS, 'm_pcPage'), '_visible', shown))
    for clip in ('mcInst_Camera', 'mcInst_Vibration'):
        g.emit(setm(get(details, clip), '_visible', (g_not, shown)))


def pause_selector_open(g):
    tag_ = R(2)
    g.stmt(call(THIS, 'PCOpt_OpenOptionDetails', tag_))

    def graphics(g):
        g.emit(setm(THIS, 'm_menuType', get(PM, 'OptionSelector', 'MENU_TYPE_PCOPT')))
        g.stmt(call(get(THIS, 'm_pcPage'), 'Active'))

    if_(g, eq(tag_, TAG), graphics)


def pause_selector_input(g):
    details, path = R(2), R(3)

    def graphics(g):
        g.stmt(call(get(THIS, 'm_pcPage'), 'handleInput', details, path))
        ret(g, False)

    if_(g, eq(get(THIS, 'm_menuType'), get(PM, 'OptionSelector', 'MENU_TYPE_PCOPT')), graphics)
    ret(g, call(THIS, 'PCOpt_handleInput', details, path))


def pause_menu_add_main(g):
    args = [R(k) for k in range(2, 11)]
    g.stmt(call(THIS, 'PCOpt_AddMainSelect', *args))
    if_(g, eq(args[8], CONTROLS_LABEL), lambda g: g.stmt(call(
        THIS, 'PCOpt_AddMainSelect', call(ei('PCOpt_Page'), 'toUpperCase'), '', True, ei('PCOpt_PageHelp'), '', '', '', '', TAG)))


def pause_menu_set_sub(g):
    main = get(var('_root'), 'mcInst_Main')
    if_(g, get(main, 'm_pcGraphics'), lambda g: g.stmt(call(main, 'OpenOptionDetails', TAG)),
        lambda g: g.stmt(call(THIS, 'PCOpt_SetSubSelect')))


def pause_title(main):
    return get(main, 'm_instSelect', 'mcInst_Title', 'mcInst_Text', 'mcInst_Text')


def pause_main_open_option(g):
    tag_, select, result = R(2), R(3), R(4)
    g.stmt(store(select, call(get(THIS, 'm_instSelect'), 'GetSelectMC')))
    g.emit(setm(THIS, 'm_pcGraphics', eq(get(member(get(select, 'm_items'), get(select, 'm_cursor')), 'EndLabel'), TAG)))

    def graphics(g):
        g.emit(setm(THIS, 'm_pcTitle', get(pause_title(THIS), 'text')))
        g.stmt(call(get(THIS, 'm_instSelect'), 'SetTitle', call(ei('PCOpt_Page'), 'toUpperCase')))
        g.stmt(store(result, call(THIS, 'PCOpt_OpenOption', TAG)))
        g.emit(setm(THIS, 'm_selOptionTag', TAG))
        ret(g, result)

    if_(g, get(THIS, 'm_pcGraphics'), graphics)
    ret(g, call(THIS, 'PCOpt_OpenOption', tag_))


def pause_main_close_details(g):
    select = R(2)
    g.stmt(call(THIS, 'PCOpt_CloseOptionDetails'))

    def graphics(g):
        g.emit(setm(THIS, 'm_pcGraphics', False))
        g.stmt(call(get(THIS, 'm_instSelect'), 'SetTitle', get(THIS, 'm_pcTitle')))
        g.stmt(store(select, call(get(THIS, 'm_instSelect'), 'GetSelectMC')))
        g.emit(setm(select, 'm_proc', get(select, 'CCD_PROC_DEFAULT')))
        g.emit(setm(select, 'm_decide', False))
        g.stmt(call(THIS, 'CloseOption'))

    if_(g, get(THIS, 'm_pcGraphics'), graphics)


def pause_menu_set_select(g):
    kind, cursor, count, select, k, slot, item = (R(n) for n in range(2, 9))
    g.stmt(store(count, get(THIS, 'm_subItems', 'length')))
    if_(g, eq(kind, get(THIS, 'SELECT_MAIN')), lambda g: g.stmt(store(count, get(THIS, 'm_mainItems', 'length'))))
    g.stmt(store(select, call(THIS, 'GetSelectMC')))

    def ensure(g):
        def create(g):
            y = sub(mul(k, PAUSE_SLOT_PITCH), PAUSE_SLOT_FIRST)
            g.stmt(store(slot, call(select, 'attachMovie', 'PCOpt_PauseSlot', add('mcInst_Base', add(k, 1)),
                                    add(mul(k, 3), PAUSE_BASE_DEPTH))))
            g.stmt(store(item, call(select, 'attachMovie', 'mc_SelectItem', add('mcInst_Sel', add(k, 1)),
                                    add(mul(k, 3), PAUSE_ITEM_DEPTH))))
            for clip in (slot, item):
                g.emit(setm(clip, '_y', y))
                g.emit(setm(clip, '_visible', False))

        if_(g, eq(member(select, add('mcInst_Sel', add(k, 1))), UNDEFINED), create)

    loop(g, k, count, ensure)
    g.stmt(call(THIS, 'PCOpt_SetSelect', kind))
    g.stmt(store(cursor, get(THIS, 'mcInst_Select', 'mcInst_DownCursor')))
    if_(g, eq(get(THIS, 'm_pcDownY'), UNDEFINED), lambda g: g.emit(setm(THIS, 'm_pcDownY', get(cursor, '_y'))))
    if_(g, less(count, PAUSE_BASE_SLOTS), lambda g: g.stmt(store(count, PAUSE_BASE_SLOTS)))
    g.emit(setm(cursor, '_y', add(get(THIS, 'm_pcDownY'), mul(sub(count, PAUSE_BASE_SLOTS), PAUSE_SLOT_PITCH))))


def pause_tags():
    g = Gen()
    selector = get(PM, 'OptionSelector')
    g.emit(setm(selector, 'MENU_TYPE_PCOPT', add(get(selector, 'MENU_TYPE_OPERATION'), 1)))

    define_class(g, PM, 'DetailGraphics', 'DetailCamera', 2, pause_page_ctor)
    g.emit(setm(get(PM, 'DetailGraphics'), 'CycleSetParam', func(['Prm', 'bActive', 'bOpen'], 6, pause_cycle_set_param)))
    g.emit(setm(get(PM, 'DetailGraphics'), 'CycleParamMax', func([], 2, lambda g: ret(g, get(THIS, 'm_count')))))
    method(g, PM, 'DetailGraphics', 'Init', [], 11, pause_page_init)
    method(g, PM, 'DetailGraphics', 'Layout', [], 8, layout(PAUSE_VISIBLE, PAUSE_PITCH, PAUSE_FIRST, (
        (lambda i: member(THIS, add('mcInst_Sel', add(i, 1))), 0),
        (lambda i: get(member(THIS, add('mcInst_Sel', add(i, 1))), 'mcInst_Title'), None),
        (lambda i: get(member(THIS, add('mcInst_Sel', add(i, 1))), 'mcInst_Select'), None))))
    method(g, PM, 'DetailGraphics', 'SetCursor', [], 2, scrolled_cursor(PM, 'DetailCamera'))
    register(g, PM, 'PCOpt_PausePage', 'DetailGraphics')

    wrap(g, PM, 'OptionSelector', 'Init')
    method(g, PM, 'OptionSelector', 'Init', [], 3, pause_selector_init)
    wrap(g, PM, 'OptionSelector', 'ChangeOptionDetails')
    method(g, PM, 'OptionSelector', 'ChangeOptionDetails', ['OptionTag'], 4, pause_selector_change)
    wrap(g, PM, 'OptionSelector', 'OpenOptionDetails')
    method(g, PM, 'OptionSelector', 'OpenOptionDetails', ['OptionTag'], 3, pause_selector_open)
    wrap(g, PM, 'OptionSelector', 'handleInput')
    method(g, PM, 'OptionSelector', 'handleInput', ['details', 'pathToFocus'], 4, pause_selector_input)
    wrap(g, PM, 'MenuSelector', 'AddMainSelect')
    method(g, PM, 'MenuSelector', 'AddMainSelect', ['selectText', 'selectFunc', 'bEnable', 'helpText', 'dialogType',
                                                    'dialogMsg', 'dialogIdx', 'dialogImg', 'endLabel'], 11, pause_menu_add_main)
    wrap(g, PM, 'MenuSelector', 'SetSelect')
    method(g, PM, 'MenuSelector', 'SetSelect', ['SelType'], 9, pause_menu_set_select)
    wrap(g, PM, 'MenuSelector', 'SetSubSelect')
    method(g, PM, 'MenuSelector', 'SetSubSelect', [], 2, pause_menu_set_sub)
    wrap(g, PM, 'PauseMain', 'OpenOption')
    method(g, PM, 'PauseMain', 'OpenOption', ['OptionTag'], 5, pause_main_open_option)
    wrap(g, PM, 'PauseMain', 'CloseOptionDetails')
    method(g, PM, 'PauseMain', 'CloseOptionDetails', [], 3, pause_main_close_details)

    slot = place(1, PAUSE_SLOT_BASE, matrix(ty=-3.0)) + place(2, PAUSE_SLOT_LINE, matrix(0.85, -217.6, 12.0))
    return (sprite(PAUSE_PAGE, b'') + sprite(PAUSE_SLOT, slot)
            + export((PAUSE_PAGE, 'PCOpt_PausePage'), (PAUSE_SLOT, 'PCOpt_PauseSlot')) + init_action(PAUSE_PAGE, g))


def array(name, data):
    rows = [', '.join('0x%02X' % b for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    return 'constexpr unsigned char %s[] = {\n    %s,\n};\n' % (name, ',\n    '.join(rows))


def write_inc(path):
    Path(path).write_text(array('k_title_menu_tags', title_tags()) + '\n' + array('k_pause_menu_tags', pause_tags()),
                          encoding='ascii', newline='\n')


if __name__ == '__main__':
    write_inc(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / 'option_menu_tags.inc')
