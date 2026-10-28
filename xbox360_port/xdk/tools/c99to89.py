#!/usr/bin/env python3
"""c99to89.py - C99 -> C89 transpile step for the Xbox 360 XDK build.

The XDK compiler is MSVC 16 (VS2010) for PowerPC. Its C mode is C89: no
declarations after statements, no for(int i ...), no designated initialisers,
no compound literals, and block-scope aggregates may only be initialised with
constants. The decomp's map data alone has tens of thousands of designated
initialiser lines, so the sources cannot be edited into C89 by hand and
should not be: they are shared with every other port.

So the XDK build transpiles. Per translation unit:

  1. clang -E, against pycparser's fake libc headers (never the XDK's own,
     whose Microsoft syntax no C parser takes), with xdk_compat.h included.
  2. pycparser parses the result into an AST.
  3. The AST is rewritten:
       - designated initialisers  -> positional, zero-filled gaps
       - decl after statement     -> decl at block top (+ assignment in place)
       - for (T i = ...)          -> T i hoisted, for (i = ...)
       - non-constant aggregate init at block scope -> memset + member stores
       - compound literal         -> hoisted temporary
  4. Only the declarations that came from PROJECT files are regenerated; the
     fake-header part is dropped and replaced by the real #include lines, so
     cl.exe compiles the output against the XDK CRT.

    python3 c99to89.py --clang <clang.exe> --fake <dir> --fake-extra <dir>
                       --compat <xdk_compat.h> [-I.. -D..] -o out.c src.c
"""
import argparse
import os
import re
import subprocess
import sys
import threading

from pycparser import c_ast, c_generator, c_parser

sys.setrecursionlimit(20000)

LINEMARK = re.compile(r'^# (\d+) "([^"]*)"((?: \d)*)\s*$')


def norm(p):
    return os.path.normcase(os.path.normpath(p))


# --------------------------------------------------------------------------
# Preprocessing
# --------------------------------------------------------------------------

def preprocess(a):
    cmd = [a.clang, '-E', '--target=powerpc-unknown-none-elf', '-ffreestanding',
           '-nostdinc', '-nobuiltininc', '-x', 'c', '-std=gnu99',
           '-U__GNUC__', '-U__GNUC_MINOR__', '-U__GNUC_PATCHLEVEL__', '-U__clang__',
           # Mirror cl.exe's own predefines, so every #if in the tree resolves
           # the way it will when cl compiles the output. _WIN32 in particular:
           # the XDK compiler defines it, and the tree uses it to mean "not
           # POSIX" (dirent.h, strings.h, ...).
           '-DSH_C89_TRANSPILE', '-D_MSC_VER=1600', '-D_WIN32', '-D_M_PPCBE', '-D_XBOX_VER=200',
           '-include', a.compat, '-I', a.fake_extra, '-I', a.fake]
    for i in a.I:
        cmd += ['-I', i]
    for d in a.D:
        cmd += ['-D' + d]
    for u in a.U:
        cmd += ['-U' + u]
    cmd.append(a.src)
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        sys.exit('clang -E failed')
    return r.stdout


def system_includes(text, fake_dirs):
    """The system headers project files include directly, from the line
    markers' enter/leave flags, as paths relative to the fake dir."""
    stack = []
    found = []
    for line in text.split('\n'):
        m = LINEMARK.match(line)
        if not m:
            continue
        f = norm(m.group(2))
        flags = m.group(3).split()
        if '1' in flags:
            includer = stack[-1] if stack else None
            stack.append(f)
            fake = None
            for fd in fake_dirs:
                if f.startswith(norm(fd) + os.sep):
                    fake = os.path.relpath(f, norm(fd)).replace('\\', '/')
            if fake and includer is not None and not any(includer.startswith(norm(fd) + os.sep) for fd in fake_dirs):
                if fake not in found and not fake.startswith('_fake'):
                    found.append(fake)
        elif '2' in flags:
            if stack:
                stack.pop()
            if stack:
                stack[-1] = f
        else:
            if stack:
                stack[-1] = f
            else:
                stack.append(f)
    return found


# --------------------------------------------------------------------------
# Type environment
# --------------------------------------------------------------------------

class TypeEnv(c_ast.NodeVisitor):
    def __init__(self, ast):
        self.typedefs = {}
        self.tags = {}
        self.enumerators = set()
        self.enum_values = {}
        self.functions = set()
        self.braced_scalar = False
        self.visit(ast)

    def visit_Enum(self, node):
        if node.values is None:
            return
        nxt = 0
        for en in node.values.enumerators:
            self.enumerators.add(en.name)
            if en.value is not None:
                try:
                    nxt = eval_const(en.value, self)
                except ValueError:
                    nxt = None
            if nxt is not None:
                self.enum_values[en.name] = nxt
                nxt += 1

    def visit_Typedef(self, node):
        self.typedefs[node.name] = node.type
        self.generic_visit(node)

    def visit_Struct(self, node):
        if node.decls is not None and node.name:
            self.tags[('struct', node.name)] = node
        self.generic_visit(node)

    def visit_Union(self, node):
        if node.decls is not None and node.name:
            self.tags[('union', node.name)] = node
        self.generic_visit(node)

    def visit_Enumerator(self, node):
        self.enumerators.add(node.name)

    def visit_FuncDef(self, node):
        self.functions.add(node.decl.name)
        self.generic_visit(node)

    def visit_Decl(self, node):
        if isinstance(node.type, c_ast.FuncDecl):
            self.functions.add(node.name)
        self.generic_visit(node)

    def resolve(self, t):
        """-> ('struct'|'union', [field Decls]) | ('array', (elem_type, dim)) | ('scalar', None)"""
        seen = 0
        while seen < 64:
            seen += 1
            if isinstance(t, c_ast.TypeDecl):
                t = t.type
            elif isinstance(t, c_ast.IdentifierType):
                if len(t.names) == 1 and t.names[0] in self.typedefs:
                    t = self.typedefs[t.names[0]]
                else:
                    return ('scalar', None)
            elif isinstance(t, (c_ast.Struct, c_ast.Union)):
                kind = 'struct' if isinstance(t, c_ast.Struct) else 'union'
                node = t
                if node.decls is None:
                    node = self.tags.get((kind, t.name))
                if node is None or node.decls is None:
                    return ('scalar', None)
                return (kind, list(node.decls))
            elif isinstance(t, c_ast.ArrayDecl):
                return ('array', (t.type, t.dim))
            else:
                return ('scalar', None)
        return ('scalar', None)

    def is_aggregate(self, t):
        return self.resolve(t)[0] in ('struct', 'union', 'array')


# --------------------------------------------------------------------------
# Initialiser trees
# --------------------------------------------------------------------------

class Agg:
    """A partially specified aggregate value, by slot."""
    def __init__(self, env, typ):
        self.env = env
        self.typ = typ
        self.kind, info = env.resolve(typ)
        if self.kind in ('struct', 'union'):
            self.fields = info
            self.count = len(info)
        elif self.kind == 'array':
            self.elem, dim = info
            self.count = None
            if isinstance(dim, c_ast.Constant) and dim.type == 'int':
                self.count = int(dim.value.rstrip('uUlL'), 0)
        self.slots = {}

    def slot_type(self, i):
        if self.kind == 'array':
            return self.elem
        return self.fields[i].type

    def in_range(self, i):
        return self.count is None or i < self.count

    def field_index(self, name):
        """Index of a named field, descending into anonymous members: returns
        (index, [further implicit names]) or None."""
        for i, f in enumerate(self.fields):
            if f.name == name:
                return (i, [])
        for i, f in enumerate(self.fields):
            if f.name is None:
                sub = Agg(self.env, f.type)
                if sub.kind in ('struct', 'union'):
                    r = sub.field_index(name)
                    if r:
                        return (i, [name])
        return None

    def get_or_create(self, i):
        v = self.slots.get(i)
        if not isinstance(v, Agg):
            v = Agg(self.env, self.slot_type(i))
            self.slots[i] = v
        return v


def designator_steps(agg, name_list):
    """Turn a NamedInitializer's designator list into [(slot index), ...]
    walking down through agg, expanding anonymous members."""
    steps = []
    cur = agg
    i = 0
    while i < len(name_list):
        d = name_list[i]
        # pycparser gives `.name` and `[name]` the same ID node, so an ID on
        # an array can only be an enumerator index.
        if isinstance(d, c_ast.ID) and cur.kind in ('struct', 'union'):
            r = cur.field_index(d.name)
            if r is None:
                raise ValueError('no field %s' % d.name)
            idx, implicit = r
            steps.append(idx)
            cur = cur.get_or_create(idx)
            if implicit:
                name_list = name_list[:i + 1] + [c_ast.ID(n) for n in implicit] + name_list[i + 1:]
        else:
            if cur.kind != 'array':
                raise ValueError('index designator on non-array')
            idx = eval_const(d.expr if isinstance(d, c_ast.ArrayDesignator) else d, agg.env)
            steps.append(idx)
            cur = cur.get_or_create(idx)
        i += 1
    return steps


def eval_const(e, env=None):
    try:
        return _eval_const(e, env)
    except (ZeroDivisionError, OverflowError, KeyError, IndexError):
        raise ValueError('non-constant expression')


def _eval_const(e, env):
    if isinstance(e, c_ast.Constant):
        if e.type == 'char':
            return ord(e.value.strip("'").encode().decode('unicode_escape')[0])
        return int(e.value.rstrip('uUlL'), 0)
    if isinstance(e, c_ast.ID) and env is not None and e.name in env.enum_values:
        return env.enum_values[e.name]
    if isinstance(e, c_ast.UnaryOp):
        v = _eval_const(e.expr, env)
        return {'-': -v, '+': v, '~': ~v, '!': int(not v)}[e.op]
    if isinstance(e, c_ast.Cast):
        return eval_const(e.expr, env)
    if isinstance(e, c_ast.BinaryOp):
        l, r = eval_const(e.left, env), eval_const(e.right, env)
        return {'+': l + r, '-': l - r, '*': l * r, '/': l // r, '%': l % r, '<<': l << r, '>>': l >> r,
                '|': l | r, '&': l & r, '^': l ^ r}[e.op]
    if isinstance(e, c_ast.TernaryOp):
        return eval_const(e.iftrue, env) if eval_const(e.cond, env) else eval_const(e.iffalse, env)
    raise ValueError('non-constant array designator: %s %s' % (type(e).__name__, c_generator.CGenerator().visit(e)[:80]))


def fill_from_initlist(agg, initlist):
    items = list(initlist.exprs)
    i, cursor = 0, 0
    while i < len(items):
        e = items[i]
        if isinstance(e, c_ast.NamedInitializer):
            steps = designator_steps(agg, list(e.name))
            cur = agg
            for s in steps[:-1]:
                cur = cur.get_or_create(s)
            last = steps[-1]
            set_slot(cur, last, e.expr)
            cursor = steps[0] + 1
            i += 1
            continue
        if not agg.in_range(cursor):
            break
        sub_t = agg.slot_type(cursor)
        if agg.env.is_aggregate(sub_t):
            if isinstance(e, c_ast.InitList):
                agg.slots[cursor] = fill_from_initlist(Agg(agg.env, sub_t), e)
                i += 1
            else:
                i = fill_stream(agg.get_or_create(cursor), items, i)
        else:
            agg.slots[cursor] = unbrace_scalar(agg.env, e)
            i += 1
        cursor += 1
    return agg


def unbrace_scalar(env, e):
    """`{ 0, 0, 0 }` for a scalar member: gcc takes the first element with a
    warning, MSVC refuses (C2078). The decomp's data has a few."""
    if isinstance(e, c_ast.InitList):
        env.braced_scalar = True
        return unbrace_scalar(env, e.exprs[0]) if e.exprs else c_ast.Constant('int', '0')
    return e


def set_slot(agg, idx, expr):
    sub_t = agg.slot_type(idx)
    if isinstance(expr, c_ast.InitList) and agg.env.is_aggregate(sub_t):
        target = agg.get_or_create(idx)
        fill_from_initlist(target, expr)
    else:
        agg.slots[idx] = unbrace_scalar(agg.env, expr) if not agg.env.is_aggregate(sub_t) else expr


def fill_stream(agg, items, i):
    """Brace elision: positional scalars pour into a nested aggregate."""
    cursor = 0
    while agg.in_range(cursor) and i < len(items):
        e = items[i]
        if isinstance(e, c_ast.NamedInitializer):
            break
        sub_t = agg.slot_type(cursor)
        if agg.env.is_aggregate(sub_t):
            if isinstance(e, c_ast.InitList):
                agg.slots[cursor] = fill_from_initlist(Agg(agg.env, sub_t), e)
                i += 1
            else:
                i = fill_stream(agg.get_or_create(cursor), items, i)
        else:
            agg.slots[cursor] = unbrace_scalar(agg.env, e)
            i += 1
        cursor += 1
        if agg.kind == 'array' and agg.count is None and cursor > 100000:
            break
    return i


def zero_for(env, t):
    if env.is_aggregate(t):
        return c_ast.InitList([c_ast.Constant('int', '0')])
    return c_ast.Constant('int', '0')


def sub_lvalue(agg, lhs, k):
    """lhs for slot k of agg (None when lhs is unknown)."""
    if lhs is None:
        return None
    if agg.kind == 'array':
        return c_ast.ArrayRef(lhs, c_ast.Constant('int', str(k)))
    f = agg.fields[k]
    if f.name is None:
        return lhs      # anonymous member: its fields are addressed through the parent
    return c_ast.StructRef(lhs, '.', c_ast.ID(f.name))


def to_initlist(agg, lhs=None, deferred=None):
    """Positional initialiser for agg. A union whose designated member is not
    its first cannot be initialised in C89 at all; with an lvalue and a
    deferred list, that member is zeroed here and stored at startup instead."""
    if not agg.slots:
        return c_ast.InitList([c_ast.Constant('int', '0')])
    if agg.kind == 'union' and any(k > 0 for k in agg.slots):
        if lhs is None or deferred is None:
            raise ValueError('designated init of a non-first union member (not expressible in C89)')
        for k, v in agg.slots.items():
            sub = sub_lvalue(agg, lhs, k)
            if isinstance(v, Agg):
                deferred.extend(member_stores(agg.env, sub, v))
            else:
                deferred.append(c_ast.Assignment('=', sub, v))
        return c_ast.InitList([c_ast.Constant('int', '0')])
    n = max(agg.slots) + 1
    out = []
    for k in range(n):
        v = agg.slots.get(k)
        if v is None:
            out.append(zero_for(agg.env, agg.slot_type(k)))
        elif isinstance(v, Agg):
            out.append(to_initlist(v, sub_lvalue(agg, lhs, k), deferred))
        else:
            out.append(v)
    return c_ast.InitList(out)


def has_designator(node):
    if isinstance(node, c_ast.NamedInitializer):
        return True
    if isinstance(node, c_ast.InitList):
        return any(has_designator(e) for e in node.exprs)
    return False


def convert_init(env, init, typ, lhs=None, deferred=None):
    """Designated -> positional, recursively. A purely positional list is
    left as written unless it braces a scalar member, so the bulk of the
    decomp's tables pass through byte-for-byte."""
    if isinstance(init, c_ast.InitList) and env.is_aggregate(typ):
        env.braced_scalar = False
        try:
            agg = fill_from_initlist(Agg(env, typ), init)
        except ValueError:
            if has_designator(init):
                raise
            return init
        if has_designator(init) or env.braced_scalar:
            return to_initlist(agg, lhs, deferred)
    return init


# --------------------------------------------------------------------------
# Constant-ness (for "may this initialiser stay attached at block scope")
# --------------------------------------------------------------------------

class ConstCheck(c_ast.NodeVisitor):
    def __init__(self, env, locals_):
        self.env = env
        self.locals = locals_
        self.ok = True

    def visit_ID(self, node):
        if node.name in self.env.enumerators or node.name in self.env.functions:
            return
        self.ok = False

    def visit_UnaryOp(self, node):
        if node.op == '&' and isinstance(node.expr, c_ast.ID) and node.expr.name not in self.locals:
            return
        if node.op == 'sizeof':
            return
        self.generic_visit(node)

    def visit_FuncCall(self, node):
        self.ok = False

    def visit_StructRef(self, node):
        self.ok = False

    def visit_ArrayRef(self, node):
        self.ok = False

    def visit_CompoundLiteral(self, node):
        self.ok = False


def is_constant(env, locals_, expr):
    c = ConstCheck(env, locals_)
    c.visit(expr)
    return c.ok


class Renamer(c_ast.NodeVisitor):
    def __init__(self, old, new):
        self.old, self.new = old, new

    def visit_ID(self, node):
        if node.name == self.old:
            node.name = self.new

    def visit_StructRef(self, node):
        self.visit(node.name)


# --------------------------------------------------------------------------
# Block rewriting
# --------------------------------------------------------------------------

class FuncCtx:
    def __init__(self, env, funcdef):
        self.env = env
        self.locals = set()
        self.tmp = 0
        self.collect(funcdef)

    def collect(self, node):
        for n in node:
            pass
        class V(c_ast.NodeVisitor):
            def __init__(s, outer):
                s.outer = outer
            def visit_Decl(s, d):
                if d.name:
                    s.outer.locals.add(d.name)
                s.generic_visit(d)
        V(self).visit(node)
        if node.decl.type.args:
            for p in node.decl.type.args.params:
                if isinstance(p, c_ast.Decl) and p.name:
                    self.locals.add(p.name)


class BlockCtx:
    def __init__(self):
        self.front = []          # hoisted Decl nodes
        self.names = {}          # name -> (type text, from_for)


def type_text(gen, decl):
    d = c_ast.Decl(decl.name, decl.quals, decl.align if hasattr(decl, 'align') else [], decl.storage,
                   decl.funcspec, decl.type, None, None)
    return gen.visit(d)


def strip_const(decl):
    decl.quals = [q for q in decl.quals if q != 'const']
    t = decl.type
    while t is not None and not isinstance(t, c_ast.TypeDecl):
        if isinstance(t, c_ast.PtrDecl):
            return           # const applies to the pointee past here
        t = getattr(t, 'type', None)
    if isinstance(t, c_ast.TypeDecl):
        t.quals = [q for q in t.quals if q != 'const']


def member_stores(env, lhs, agg):
    """Statements assigning every specified slot of agg into lhs."""
    out = []
    for k in sorted(agg.slots):
        v = agg.slots[k]
        if agg.kind == 'array':
            sub = c_ast.ArrayRef(lhs, c_ast.Constant('int', str(k)))
        else:
            f = agg.fields[k]
            if f.name is None:
                sub = lhs      # anonymous member: fields are addressed through the parent
            else:
                sub = c_ast.StructRef(lhs, '.', c_ast.ID(f.name))
        if isinstance(v, Agg):
            out += member_stores(env, sub, v)
        else:
            out.append(c_ast.Assignment('=', sub, v))
    return out


def split_decl(env, fctx, decl, force=False):
    """Decl with init -> (decl without init, [statements]). The decl keeps its
    init when C89 allows that at block scope; force=True splits anyway."""
    init = decl.init
    if init is None or 'static' in decl.storage or 'extern' in decl.storage:
        return decl, []
    if isinstance(init, c_ast.InitList):
        if not force and is_constant(env, fctx.locals, init):
            decl.init = convert_init(env, init, decl.type)
            return decl, []
        if not env.is_aggregate(decl.type):
            decl.init = None
            strip_const(decl)
            return decl, [c_ast.Assignment('=', c_ast.ID(decl.name), init.exprs[0])]
        agg = fill_from_initlist(Agg(env, decl.type), init)
        decl.init = None
        strip_const(decl)
        stmts = [c_ast.FuncCall(c_ast.ID('memset'), c_ast.ExprList([
            c_ast.UnaryOp('&', c_ast.ID(decl.name)), c_ast.Constant('int', '0'),
            c_ast.UnaryOp('sizeof', c_ast.ID(decl.name))]))]
        stmts += member_stores(env, c_ast.ID(decl.name), agg)
        return decl, stmts
    if isinstance(init, c_ast.CompoundLiteral):
        decl.init = None
        strip_const(decl)
        return decl, [c_ast.Assignment('=', c_ast.ID(decl.name), init)]
    # scalar / struct-from-expression
    if isinstance(decl.type, c_ast.ArrayDecl):
        # char s[] = "lit" style: the literal is constant, keep attached
        return decl, []
    decl.init = None
    strip_const(decl)
    return decl, [c_ast.Assignment('=', c_ast.ID(decl.name), init)]


class Rewriter:
    def __init__(self, env):
        self.env = env
        self.gen = c_generator.CGenerator()
        self.fctx = None
        self.deferred = []       # [(object name, [Assignment...])] run at startup

    # -- entry -------------------------------------------------------------
    def rewrite_file(self, ast):
        for ext in ast.ext:
            if isinstance(ext, c_ast.FuncDef):
                self.fctx = FuncCtx(self.env, ext)
                self.compound(ext.body)
                self.fctx = None
            elif isinstance(ext, c_ast.Decl) and ext.init is not None:
                deferred = []
                ext.init = convert_init(self.env, ext.init, ext.type, c_ast.ID(ext.name), deferred)
                if deferred:
                    # Stored at startup, so the object cannot live in .rdata.
                    strip_const(ext)
                    self.deferred.append((ext.name, deferred))

    # -- blocks ------------------------------------------------------------
    def compound(self, comp):
        if comp.block_items is None:
            return
        bctx = BlockCtx()
        items = []
        seen_stmt = False
        for item in comp.block_items:
            if isinstance(item, c_ast.Decl):
                self.hoist_decl(item, bctx, items, at_top=not seen_stmt)
            elif isinstance(item, c_ast.Typedef):
                bctx.front.append(item)
            else:
                seen_stmt = True
                items.extend(self.stmt(item, bctx))
        comp.block_items = bctx.front + items

    def hoist_decl(self, decl, bctx, items, at_top):
        force = False
        if decl.init is not None:
            try:
                decl.init = convert_init(self.env, decl.init, decl.type)
            except ValueError:
                force = True     # non-first union member: member stores instead
        pre = self.lift_literals(decl.init, bctx) if decl.init is not None else []
        if at_top and not pre and not force:
            d, stmts = split_decl(self.env, self.fctx, decl) if (isinstance(decl.init, c_ast.InitList) and not is_constant(self.env, self.fctx.locals, decl.init)) else (decl, [])
            self.add_front(d, bctx, from_for=False)
            items.extend(stmts)
            return
        d, stmts = split_decl(self.env, self.fctx, decl, force=force)
        self.add_front(d, bctx, from_for=False)
        items.extend(pre + stmts)

    def add_front(self, decl, bctx, from_for):
        if decl.name is None:
            bctx.front.append(decl)
            return None
        tt = type_text(self.gen, decl)
        prev = bctx.names.get(decl.name)
        if prev is None:
            bctx.names[decl.name] = (tt, from_for)
            bctx.front.append(decl)
            return decl.name
        if from_for and prev[1] and prev[0] == tt:
            return decl.name
        if prev[0] == tt and from_for:
            return decl.name
        # clash: rename
        n = 1
        while ('%s_%d' % (decl.name, n)) in bctx.names:
            n += 1
        new = '%s_%d' % (decl.name, n)
        old = decl.name
        decl.name = new
        self.rename_typedecl(decl.type, old, new)
        bctx.names[new] = (type_text(self.gen, decl), from_for)
        bctx.front.append(decl)
        return new

    @staticmethod
    def rename_typedecl(t, old, new):
        while t is not None and not isinstance(t, c_ast.TypeDecl):
            t = getattr(t, 'type', None)
        if isinstance(t, c_ast.TypeDecl) and t.declname == old:
            t.declname = new

    def stmt(self, s, bctx):
        """Rewrite one statement; returns the list that replaces it."""
        if isinstance(s, c_ast.Compound):
            self.compound(s)
            return [s]
        if isinstance(s, c_ast.For):
            pre = []
            if isinstance(s.init, c_ast.DeclList):
                assigns = []
                for d in s.init.decls:
                    d.init = convert_init(self.env, d.init, d.type) if d.init is not None else None
                    old = d.name
                    d2, stmts = split_decl(self.env, self.fctx, d)
                    new = self.add_front(d2, bctx, from_for=True)
                    if new != old:
                        Renamer(old, new).visit(s)
                        for st in stmts:
                            Renamer(old, new).visit(st)
                    for st in stmts:
                        if isinstance(st, c_ast.Assignment):
                            assigns.append(st)
                        else:
                            pre.append(st)
                s.init = c_ast.ExprList(assigns) if assigns else None
            elif s.init is not None:
                pre += self.lift_literals(s.init, bctx)
            s.stmt = self.sub(s.stmt, bctx)
            return pre + [s]
        if isinstance(s, (c_ast.While, c_ast.DoWhile)):
            s.stmt = self.sub(s.stmt, bctx)
            return [s]
        if isinstance(s, c_ast.If):
            pre = self.lift_literals(s.cond, bctx)
            s.iftrue = self.sub(s.iftrue, bctx)
            s.iffalse = self.sub(s.iffalse, bctx) if s.iffalse is not None else None
            return pre + [s]
        if isinstance(s, c_ast.Switch):
            pre = self.lift_literals(s.cond, bctx)
            s.stmt = self.sub(s.stmt, bctx)
            return pre + [s]
        if isinstance(s, (c_ast.Case, c_ast.Default)):
            new = []
            for st in s.stmts or []:
                new.extend(self.stmt(st, bctx) if not isinstance(st, c_ast.Decl) else self.decl_in_stmt(st, bctx))
            s.stmts = new
            return [s]
        if isinstance(s, c_ast.Label):
            r = self.stmt(s.stmt, bctx) if not isinstance(s.stmt, c_ast.Decl) else self.decl_in_stmt(s.stmt, bctx)
            if not r:
                s.stmt = c_ast.EmptyStatement()
                return [s]
            s.stmt = r[0]
            return [s] + r[1:]
        if isinstance(s, c_ast.Decl):
            return self.decl_in_stmt(s, bctx)
        if isinstance(s, c_ast.Pragma):
            return [s]
        pre = self.lift_literals(s, bctx)
        return pre + [s]

    def decl_in_stmt(self, d, bctx):
        items = []
        self.hoist_decl(d, bctx, items, at_top=False)
        return items

    def sub(self, s, bctx):
        """A statement in a non-compound position (loop/if body)."""
        if s is None:
            return None
        r = self.stmt(s, bctx)
        if len(r) == 1:
            return r[0]
        return c_ast.Compound(r)

    # -- compound literals --------------------------------------------------
    def lift_literals(self, node, bctx):
        """Replace (T){...} inside node by hoisted temporaries; returns the
        statements that must run before node."""
        pre = []
        found = []

        class Finder(c_ast.NodeVisitor):
            def visit_CompoundLiteral(s, n):
                found.append(n)
                s.generic_visit(n)

        if node is None:
            return pre
        Finder().visit(node)
        if not found:
            return pre
        for lit in found:
            self.fctx.tmp += 1
            name = 'sh_c89_lit_%d' % self.fctx.tmp
            typ = lit.type
            # type: a Typename; turn it into a TypeDecl with the temp's name
            tdecl = typ.type
            t = tdecl
            while t is not None and not isinstance(t, c_ast.TypeDecl):
                t = getattr(t, 'type', None)
            if isinstance(t, c_ast.TypeDecl):
                t.declname = name
            d = c_ast.Decl(name, typ.quals, [], [], [], tdecl, None, None)
            self.add_front(d, bctx, from_for=False)
            init = convert_init(self.env, lit.init, tdecl)
            if self.env.is_aggregate(tdecl):
                agg = fill_from_initlist(Agg(self.env, tdecl), init)
                pre.append(c_ast.FuncCall(c_ast.ID('memset'), c_ast.ExprList([
                    c_ast.UnaryOp('&', c_ast.ID(name)), c_ast.Constant('int', '0'),
                    c_ast.UnaryOp('sizeof', c_ast.ID(name))])))
                pre += member_stores(self.env, c_ast.ID(name), agg)
            else:
                pre.append(c_ast.Assignment('=', c_ast.ID(name), init.exprs[0] if isinstance(init, c_ast.InitList) else init))
        # replace the literal nodes by IDs
        class Replacer(c_ast.NodeVisitor):
            def generic_visit(s, n):
                for attr, child in n.children():
                    if isinstance(child, c_ast.CompoundLiteral):
                        idx = found.index(child)
                        new = c_ast.ID('sh_c89_lit_%d' % (self.fctx.tmp - len(found) + idx + 1))
                        if '[' in attr:
                            base, i = attr[:-1].split('[')
                            getattr(n, base)[int(i)] = new
                        else:
                            setattr(n, attr, new)
                    else:
                        s.visit(child)
        Replacer().visit(node)
        return pre


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------

def emit(ast, excluded_prefixes, includes, deferred):
    gen = c_generator.CGenerator()
    out = ['/* generated by xbox360_port/xdk/tools/c99to89.py - do not edit */']
    out.append('#include <string.h>')
    for inc in includes:
        out.append('#include <%s>' % inc)
    out.append('')
    for ext in ast.ext:
        f = norm(ext.coord.file) if ext.coord and ext.coord.file else ''
        if any(f.startswith(p) for p in excluded_prefixes):
            continue
        if isinstance(ext, c_ast.FuncDef):
            out.append(gen.visit(ext))
        elif isinstance(ext, c_ast.Pragma):
            out.append(gen.visit(ext))
        else:
            out.append(gen.visit(ext) + ';')
    if deferred:
        # File-scope initialisers C89 cannot spell (a union's non-first
        # member) become stores run from main through a pointer table the
        # linker assembles from .shinit$a/$m/$z (xdk_crt.c owns the ends and
        # the walker). Not .CRT$XCU: the linker merged that into .data with
        # attributes that put our entries outside __xc_a..__xc_z.
        out.append('')
        # The attributes must match the CRT's own .CRT$XC* sections or the
        # linker files these into a different output section, outside the
        # __xc_a..__xc_z range _initterm walks -- which is exactly what
        # happened with a read-only declaration. sh_c89_init_count lets main
        # log that the stores ran.
        out.append('#pragma section(".shinit$m", read)')
        out.append('extern int sh_c89_init_count;')
        for i, (name, stmts) in enumerate(deferred):
            out.append('static void sh_c89_init_%d(void)' % i)
            out.append('{')
            out.append('    sh_c89_init_count++;')
            for st in stmts:
                out.append('    ' + gen.visit(st) + ';')
            out.append('}')
            out.append('__declspec(allocate(".shinit$m")) static void (*sh_c89_init_ptr_%d)(void) = sh_c89_init_%d;'
                       % (i, i))
    return '\n'.join(out) + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--clang', required=True)
    ap.add_argument('--fake', required=True)
    ap.add_argument('--fake-extra', required=True)
    ap.add_argument('--compat', required=True)
    ap.add_argument('-I', action='append', default=[])
    ap.add_argument('-D', action='append', default=[])
    ap.add_argument('-U', action='append', default=[])
    ap.add_argument('-o', required=True)
    ap.add_argument('--keep-pp', action='store_true')
    ap.add_argument('src')
    a = ap.parse_args()

    text = preprocess(a)
    if a.keep_pp:
        open(a.o + '.i', 'w', encoding='utf-8').write(text)
    fake_dirs = [a.fake, a.fake_extra]
    includes = system_includes(text, fake_dirs)

    parser = c_parser.CParser()
    try:
        ast = parser.parse(text, a.src)
    except Exception as e:  # noqa
        sys.exit('parse error: %s' % e)

    env = TypeEnv(ast)
    rw = Rewriter(env)
    try:
        rw.rewrite_file(ast)
    except ValueError as e:
        sys.exit('rewrite error: %s' % e)

    # `= {}` is a gcc/C23 spelling; C89 wants at least one element. A
    # zero-length array (`u16 tbl[0]`, gcc) becomes an incomplete one.
    class GnuSpellings(c_ast.NodeVisitor):
        def visit_InitList(self, n):
            if not n.exprs:
                n.exprs.append(c_ast.Constant('int', '0'))
            self.generic_visit(n)

        def visit_ArrayDecl(self, n):
            if isinstance(n.dim, c_ast.Constant) and n.dim.value.rstrip('uUlL') == '0':
                n.dim = None
            self.generic_visit(n)
    GnuSpellings().visit(ast)

    excluded = [norm(d) + os.sep for d in fake_dirs] + [norm(a.compat)]
    open(a.o, 'w', encoding='utf-8', newline='\n').write(emit(ast, excluded, includes, rw.deferred))


if __name__ == '__main__':
    # The parser and generator recurse deeply on big initialiser tables, so
    # run on a thread with a large stack. A SystemExit raised on that thread
    # does not reach the process, hence the explicit code hand-off.
    threading.stack_size(128 * 1024 * 1024)
    result = {'code': 0}

    def run():
        try:
            main()
        except SystemExit as e:
            if isinstance(e.code, int):
                result['code'] = e.code
            elif e.code is not None:
                sys.stderr.write(str(e.code) + '\n')
                result['code'] = 1
        except Exception as e:  # noqa
            sys.stderr.write('%s: %s\n' % (type(e).__name__, e))
            result['code'] = 1

    t = threading.Thread(target=run)
    t.start()
    t.join()
    sys.exit(result['code'])
