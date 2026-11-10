#!/usr/bin/env python3
"""Prove the important tests can fail.

For each mutation: put a specific bug into the driver, rebuild, boot a fresh
guest, run the test that is supposed to catch it, and require that it FAILS.
The driver source is always restored afterwards (even on Ctrl-C).

    scripts/mutation-check.py            all mutations
    scripts/mutation-check.py no-zap     just one

A test that passes with the bug in place is not testing what it claims.
"""
import pathlib, subprocess, sys

LAB = pathlib.Path(__file__).resolve().parent.parent
GUEST = str(LAB / 'scripts/guest.sh')

# name: (file, exact text to replace, replacement, guest command that must fail)
MUTATIONS = {
    'no-zap': (
        'driver/svmd_svm.c',
        '\tsvmd_pt_zap(vm, zap_start, zap_end);\n',
        '\t/* MUTATION: entries left in place */\n',
        '/lab/tests/svm remap'),
    'no-tlb-flush': (
        'driver/svmd_svm.c',
        '\tsvmd_tlb_invalidate(vm->sdev, zap_start, zap_end - zap_start);\n',
        '\t/* MUTATION: TLB not flushed */\n',
        '/lab/tests/svm tlb_in_job'),
    'no-implicit-sync': (
        'driver/svmd_drv.c',
        '\t\tret = drm_sched_job_add_implicit_dependencies(&job->base,\n'
        '\t\t\t\t\tjob->bos[i], job->bo_write[i]);\n',
        '\t\tret = 0; /* MUTATION: no implicit dependencies */\n',
        '/lab/tests/phase2'),
    'iterator-reuse': (
        'driver/svmd_svm.c',
        '\trange = NULL;\n\tdrm_gpusvm_for_each_range(range, notifier, start, end) {\n'
        '\t\tstruct svmd_range *r = to_svmd_range(range);\n',
        '\tdrm_gpusvm_for_each_range(range, notifier, start, end) { /* MUTATION */\n'
        '\t\tstruct svmd_range *r = to_svmd_range(range);\n',
        '/lab/tests/stress two_threads'),
    'abort-wrong-job': (
        'driver/svmd_ring.c',
        '\t\tif (!svmd_seqno_passed(done, f->base.seqno)) {\n'
        '\t\t\tdma_fence_set_error(&f->base, error);',
        '\t\tif (1) { /* MUTATION: oldest listed, not the running one */\n'
        '\t\t\tdma_fence_set_error(&f->base, error);',
        '/lab/tests/stress abort_victim'),
    'no-dedup': (
        'driver/svmd_fault.c',
        '\t\t\tif (have_last && !memcmp(&f, &last, sizeof(f))) {',
        '\t\t\tif (0) { /* MUTATION: duplicates handled twice */',
        '/lab/tests/stress dup_faults'),
}


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str), cwd=LAB,
                          capture_output=True, text=True, **kw)


def build_and_load():
    r = sh('scripts/vm.sh make -C driver')
    if r.returncode:
        sys.exit('driver build failed:\n' + r.stdout + r.stderr)
    if sh('scripts/boot.sh').returncode:
        sys.exit('guest did not boot')
    if sh([GUEST, 'insmod /lab/driver/svmd.ko']).returncode:
        sys.exit('insmod failed')


def check(name):
    path, old, new, cmd = MUTATIONS[name]
    f = LAB / path
    orig = f.read_text()
    if orig.count(old) != 1:
        sys.exit(f'{name}: the text to mutate is not in {path} exactly once')
    try:
        f.write_text(orig.replace(old, new))
        build_and_load()
        r = sh([GUEST, cmd])
    finally:
        f.write_text(orig)
    caught = r.returncode != 0
    print(f'{"caught " if caught else "MISSED "} {name:18} ({cmd})')
    if not caught:
        print(r.stdout[-800:])
    return caught


def main():
    names = sys.argv[1:] or list(MUTATIONS)
    results = [check(n) for n in names]
    build_and_load()                    # leave the real driver loaded
    print(f'\n{sum(results)}/{len(results)} mutations caught by the tests')
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
