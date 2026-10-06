#!/usr/bin/env python3
"""Compile hot lean C674x packets ahead of time.

Input: one or more profiles written by cdj_c674x_aot_profile_dump (replay
with CDJ_DSP_AOT_PROFILE=path, or the board's --dsp-aot-profile).  Output:
one C file for a build to include as CDJ_C674X_AOT_FILE (see "ahead-of-time
regions" in emulator/qemu/cdj_c674x.c).  It is derived from the DSP image
the profile ran, so it lives with the build, not in the repository.

Each node is one variant: a packet (its fetch blocks and bytes), the queue
shape it starts from and its pmask predicates.  Its code is dts_lean's
pieces with the variant's constants (the issue per instruction, the checks,
the bus and retirement programs unrolled), then dt_run's post-packet steps,
then its profiled successors: the next pc picks a packet, its entry must be
current and hold that packet's bytes, the predicates pick the node (whose
entry shape is the one this node leaves).  Nodes reachable from each other
share a C function (goto); an edge into another function is a tail call.
The approach follows Stijn Jacobs' c14_jitgen.py for cdj-nxs2-qemu
(THIRD_PARTY.md); the semantics are this core's.

usage: aot_gen.py OUT.c PROFILE [PROFILE...] [--min N] [--group N]
"""
import argparse
import collections
import sys

P_END, P_TICK, P_COMMIT, P_E3 = 0, 1, 2, 3
(P_STAIL, P_SMOVE, P_SFILL, P_SCOUNT, P_LTAIL, P_LE3, P_LRET, P_LMOVE,
 P_LFILL, P_LCOUNT, P_BRANCH) = range(4, 15)
OP_FIELDS = [("kind", "kind"), ("creg", "creg"), ("cb", "creg_bank"),
             ("cr", "creg_reg"), ("z", "z"), ("side", "side"), ("dst", "dst"),
             ("a", "a"), ("b", "b"), ("cross", "cross"), ("bank", "bank"),
             ("mode", "mode"), ("size", "size"), ("scale", "scale"),
             ("pair", "pair"), ("nonaligned", "nonaligned"),
             ("store", "is_store"), ("sx", "sign_extend"),
             ("mul", "multiply"), ("swap", "swap"), ("oper", "operation")]


class Node:
    pass


def fields(tokens):
    return {tokens[i]: int(tokens[i + 1], 0) for i in range(0, len(tokens) - 1, 2)}


def tuples(text):
    return [tuple(int(x) for x in t.split(",")) for t in text.split()]


def shape(tokens):
    """S/O line -> hashable shape key (None when not known)."""
    if tokens[0] == "none":
        return None
    loads, stores, branches = (int(t) for t in tokens[:3])
    land = (int(tokens[3], 0), int(tokens[4], 0))
    rest = tokens[5:]
    ld = tuple(tuple(int(x) for x in t.split(",")) for t in rest[:loads])
    st = tuple(tuple(int(x) for x in t.split(",")) for t in rest[loads:loads + stores])
    br = tuple(int(t) for t in rest[loads + stores:loads + stores + branches])
    return (ld, st, br, land)


def parse(paths):
    nodes = {}
    for path in paths:
        cur = None
        for line in open(path):
            t = line.split()
            if not t:
                continue
            if t[0] == "V":
                cur = Node()
                cur.ptr, cur.runs = t[1], int(t[2])
                cur.pc, cur.next = int(t[4], 0), int(t[6], 0)
                nb = int(t[8])
                cur.blocks = []
                for b in t[9:9 + nb]:
                    addr, data = b.split(":")
                    cur.blocks.append((int(addr, 0), bytes.fromhex(data)))
                cur.insns = []
                cur.file = path
            elif t[0] == "I":
                cur.insns.append(fields(t[2:]))
            elif t[0] == "S":
                cur.ins = shape(t[1:])
            elif t[0] == "P":
                head, *parts = line.split("|")
                h = head.split()
                f = fields(h[1:21])
                cur.outcome, cur.pmask = f["outcome"], f["pmask"]
                cur.br_plan, cur.generic = f["branches_plan"], f["generic"]
                cur.ctl = f["ctl"]
                cur.cycles, cur.last = f["cycles"], f["last"]
                cur.ns, cur.nl, cur.nb = f["ns"], f["nl"], f["nb"]
                cur.branch_rd = [int(x) for x in h[21:21 + cur.nb]]
                cur.new_loads = tuples(parts[0])
                cur.new_stores = tuples(parts[1])
                cur.pairs = tuples(parts[2])
            elif t[0] == "G":
                cur.prog = [int(x) for x in t[1:-2]]
                cur.bus_len = int(t[-1])
            elif t[0] == "O":
                cur.out = shape(t[1:])
            elif t[0] == "E":
                cur.succ = [(t[1], int(t[2])), (t[3], int(t[4]))]
                key = (cur.pc, tuple(cur.blocks), cur.ins, cur.outcome)
                old = nodes.get(key)
                if old:                 # several profiles: add up
                    old.runs += cur.runs
                    old.ptrs.add((cur.file, cur.ptr))
                    old.succ_all += [(cur.file, p, r) for p, r in cur.succ]
                else:
                    cur.ptrs = {(cur.file, cur.ptr)}
                    cur.succ_all = [(cur.file, p, r) for p, r in cur.succ]
                    nodes[key] = cur
    return list(nodes.values())


def c_bytes(b):
    return "{" + ",".join("0x%02x" % x for x in b) + "}"


def emit(nodes, out, group):
    # packets: by (pc, blocks); nodes sorted so each packet's are contiguous
    nodes.sort(key=lambda n: (n.pc, tuple(n.blocks), -n.runs))
    packets = collections.OrderedDict()
    for n in nodes:
        packets.setdefault((n.pc, tuple(n.blocks)), []).append(n)
    order = [n for ns in packets.values() for n in ns]
    for i, n in enumerate(order):
        n.id = i
    for k, (key, ns) in enumerate(packets.items()):
        for n in ns:
            n.packet = k
    by_ptr = {p: n for n in order for p in n.ptrs}
    # edges: profiled successors whose entry shape is this node's exit shape
    for n in order:
        succ = collections.Counter()
        for f, p, r in n.succ_all:
            s = by_ptr.get((f, p))
            if s and r and n.out is not None and s.ins == n.out:
                succ[s.id] += r
        n.edges = [order[i] for i, _ in succ.most_common()]
    # groups: weakly connected, in chunks of at most `group` nodes
    parent = list(range(len(order)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for n in order:
        for s in n.edges:
            parent[find(n.id)] = find(s.id)
    comps = collections.defaultdict(list)
    for n in order:
        comps[find(n.id)].append(n)
    groups = []
    for comp in comps.values():
        # breadth first from the hottest, so chunks keep hot chains together
        comp.sort(key=lambda n: -n.runs)
        seen, seq = set(), []
        for root in comp:
            if root.id in seen:
                continue
            q = collections.deque([root])
            seen.add(root.id)
            while q:
                n = q.popleft()
                seq.append(n)
                for s in n.edges:
                    if s.id not in seen and find(s.id) == find(root.id):
                        seen.add(s.id)
                        q.append(s)
        for i in range(0, len(seq), group):
            groups.append(seq[i:i + group])
    for g, ns in enumerate(groups):
        for k, n in enumerate(ns):
            n.group, n.local = g, k

    # entry and exit shapes, numbered
    shapes = {}
    for n in order:
        n.in_id = shapes.setdefault(n.ins, len(shapes))
    for n in order:
        n.out_id = shapes.setdefault(n.out, len(shapes)) if n.out is not None else None
    w = out.write
    w("/* Generated by tools/cdj_dsp/aot_gen.py from %s: %u packets, %u nodes,"
      " %u functions.  Included by cdj_c674x.c (CDJ_C674X_AOT_FILE). */\n"
      % (" ".join(sorted({n.file for n in order})), len(packets), len(order), len(groups)))
    w("static const AotPacket aot_packets[%u] = {\n" % len(packets))
    for k, ((pc, blocks), ns) in enumerate(packets.items()):
        bl = ",".join("%#x" % a for a, _ in blocks)
        by = ",".join(c_bytes(b) for _, b in blocks)
        n0 = ns[0]
        pred = ",".join("{%u,%u,%u}" % (ins["cb"], ins["cr"], ins["z"]) if n0.pmask >> i & 1 else "{0,0,0}"
                        for i, ins in enumerate(n0.insns)) or "{0}"
        w("  {%#x, %u, {%s}, {%s}, %u, %u, %u, {%s}},\n" % (pc, len(blocks), bl, by, n0.id, len(ns),
                                                       n0.pmask, pred))
    w("};\nstatic const unsigned aot_npackets = %u;\n" % len(packets))
    for g in range(len(groups)):
        w("static int aot_g%u(CdjC674x *cpu, unsigned node, CdjC674xCacheEntry *e, AotCtx *x);\n" % g)
    for n in order:
        ld, st, br, _ = n.ins
        if ld:
            w("static const DtsLoad aot_l%u[] = {%s};\n" % (n.id, ",".join("{%u,%u,%u,%u,%u}" % t for t in ld)))
        if st:
            w("static const DtsStore aot_s%u[] = {%s};\n" % (n.id, ",".join("{%u,%u}" % t for t in st)))
    w("static const AotNode aot_nodes[%u] = {\n" % len(order))
    for n in order:
        ld, st, br, _ = n.ins
        w("  {%u, %u, %u, %u, %u, {%s}, %s, %s, aot_g%u, %u},\n" % (
            n.in_id, n.outcome, len(ld), len(st), len(br), ",".join(map(str, br)) or "0",
            "aot_l%u" % n.id if ld else "NULL", "aot_s%u" % n.id if st else "NULL",
            n.group, n.local))
    w("};\n")
    for n in order:
        nl = ",".join("{%u,%u,%u,%u,%u}" % t for t in n.new_loads) or "{0}"
        ns = ",".join("{%u,%u}" % t for t in n.new_stores) or "{0}"
        pr = ",".join("{%u,%u}" % t for t in n.pairs) or "{0}"
        w("static const DtsVariant aot_v%u = {.new_stores = %u, .new_loads = %u, "
          ".branches = %u, .cycles = %u, .last = %u, .branch_rd = {%s}, "
          ".new_load = {%s}, .new_store = {%s}, .pairs = %u, .pair = {%s}};\n" % (
              n.id, n.ns, n.nl, n.nb, n.cycles, n.last,
              ",".join(map(str, n.branch_rd)) or "0", nl, ns, len(n.pairs), pr))
    for g, ns in enumerate(groups):
        emit_group(w, g, ns)


def emit_group(w, g, ns):
    w("\nstatic int aot_g%u(CdjC674x *cpu, unsigned node, CdjC674xCacheEntry *e, AotCtx *x)\n{\n" % g)
    w("    const CdjC674xRead rd = x->read;\n    const CdjC674xWrite wr = x->write;\n"
      "    void *const op = x->opaque;\n    switch (node) {\n")
    for n in ns:
        w("    case %u: goto n%u;\n" % (n.local, n.id))
    w("    default: return AOT_DECLINED;\n    }\n")
    for n in ns:
        emit_node(w, n)
    w("}\n")


def emit_node(w, n):
    br, gen, ctl = n.br_plan, n.generic, n.ctl
    land = n.ins[3]
    w("n%u: { /* pc %#x, %u runs */\n" % (n.id, n.pc, n.runs))
    w("    DtsLean L CDJ_C674X_UNINITIALIZED;\n")
    w("    lean_begin(cpu, &L, %u, %u, %u);\n" % (br, gen, ctl))
    conds = []
    for i, ins in enumerate(n.insns):
        enabled = (n.outcome >> i) & 1 if (n.pmask >> i) & 1 else 1
        op = ", ".join(".%s = %u" % (c, ins[f]) for f, c in OP_FIELDS)
        conds.append("lean_issue(cpu, e, %u, %u, &(const JitOp){%s}, %u, &L, rd, wr, op)"
                     % (i, ins["lop"], op, enabled))
    conds.append("lean_check(cpu, %u, %u, &aot_v%u, %#xu, %#xu, &L)" % (br, gen, n.id, land[0], land[1]))
    w("    if (!(" + " &&\n          ".join(conds) + ") || aot_stress()) {\n")
    w("        lean_decline(cpu, &L, %u, %u);\n" % (br, ctl))
    w("        return x->packets ? AOT_REDO : AOT_DECLINED;\n    }\n")
    prog, i = n.prog, 0
    bus = []
    while i < n.bus_len:
        o = prog[i]
        if o == P_TICK:
            bus.append("lean_tick(cpu, %u);" % prog[i + 1])
            i += 2
        elif o == P_COMMIT:
            bus.append("if ((broke = lean_commit(cpu, %u, wr, op))) goto broke%u;" % (prog[i + 1], n.id))
            i += 2
        elif o == P_E3:
            bus.append("if ((broke = lean_e3(cpu, %u, &L, rd, op))) goto broke%u;" % (prog[i + 1], n.id))
            i += 2
        else:
            break
    fallible = any("broke" in s for s in bus)
    if fallible:
        w("    const char *broke;\n")
    for s in bus:
        w("    %s\n" % s)
    w("    lean_apply(cpu, &L, %#xu);\n" % n.next)
    i = n.bus_len
    retire_len = len(prog) - n.bus_len
    while retire_len > 1 and i < len(prog):
        o = prog[i]
        a = prog[i + 1] if i + 1 < len(prog) else 0
        b = prog[i + 2] if i + 2 < len(prog) else 0
        if o == P_STAIL:
            w("    L.stail = cpu->stores[%u];\n" % a); i += 2
        elif o == P_SMOVE:
            w("    cpu->stores[%u] = cpu->stores[%u];\n" % (a, b)); i += 3
        elif o == P_SFILL:
            for k in range(a, b):
                w("    cpu->stores[%u] = L.stail;\n" % k)
            i += 3
        elif o == P_SCOUNT:
            w("    cpu->store_count = %u;\n" % a); i += 2
        elif o == P_LTAIL:
            w("    L.ltail = cpu->loads[%u];\n" % a); i += 2
        elif o == P_LMOVE:
            w("    cpu->loads[%u] = cpu->loads[%u];\n" % (a, b)); i += 3
        elif o == P_LFILL:
            for k in range(a, b):
                w("    cpu->loads[%u] = L.ltail;\n" % k)
            i += 3
        elif o == P_LCOUNT:
            w("    cpu->load_count = %u;\n" % a); i += 2
        elif o == P_LE3:
            w("    cpu->loads[%u].value = L.data[L.reads++];\n" % a); i += 2
        elif o == P_LRET:
            w("    lean_lret(cpu, %u);\n" % a); i += 2
        elif o == P_BRANCH:
            w("    lean_branch(cpu);\n"); i += 1
        else:
            break
    w("    lean_end(cpu, &L, %u);\n" % n.last)
    w("    ++x->packets;\n    if (++x->n == x->limit) return AOT_LIMIT;\n")
    w("    if (!horizon_skip(cpu)) return AOT_BETWEEN;\n")
    if n.edges:
        w("    if (cpu->loop_active || cpu->idle_cycles || cpu->fault) return AOT_CONT;\n")
        w("    const uint32_t pc = cpu->pc;\n")
        bypc = collections.OrderedDict()
        for s in n.edges:
            bypc.setdefault((s.pc, s.packet), []).append(s)
        for (pc, packet), ss in bypc.items():
            w("    if (pc == %#xu) {\n" % pc)
            w("        CdjC674xCacheEntry *f = &x->cache[packet_cache_index(%#xu)];\n" % pc)
            w("        if (f->blocks && f->pc == pc && f->aot == &aot_packets[%u] && f->dt > 0 &&\n"
              "            aot_current(f, op)) {\n" % packet)
            w("            const uint8_t key = aot_key(cpu, &aot_packets[%u]);\n" % packet)
            for s in ss:
                if s.group == n.group:
                    w("            if (key == %u) { e = f; goto n%u; }\n" % (s.outcome, s.id))
                else:
                    w("            if (key == %u) AOT_TAIL return aot_g%u(cpu, %u, f, x);\n"
                      % (s.outcome, s.group, s.local))
            w("        }\n    }\n")
    if n.out_id is not None:
        w("    AOT_TAIL return aot_next(cpu, %u, e, x);\n" % n.out_id)
    else:
        w("    return AOT_CONT;\n")
    if fallible:
        w("broke%u:\n    lean_broke(cpu, &L, %u, %u, broke);\n    return AOT_FAULT;\n" % (n.id, br, ctl))
    w("}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("profiles", nargs="+")
    ap.add_argument("--min", type=int, default=64, help="runs a node needs")
    ap.add_argument("--group", type=int, default=256, help="nodes per C function")
    a = ap.parse_args()
    nodes = [n for n in parse(a.profiles) if n.runs >= a.min and n.ins is not None]
    with open(a.out, "w") as f:
        emit(nodes, f, a.group)
    print("%s: %u nodes" % (a.out, len(nodes)), file=sys.stderr)


if __name__ == "__main__":
    main()
