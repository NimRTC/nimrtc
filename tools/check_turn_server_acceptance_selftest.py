"""Prove the v1-n5-turn-server acceptance file catches what it claims to.

Every case here re-introduces a defect that the acceptance file claims
to catch -- the three STUN/TURN protocol bugs from the 2026-11-20
handover list, the deployment defects, and the protocol-encoding
defects -- and asserts that the acceptance run goes RED.

A guard that cannot fail is not a guard, and an assertion that cannot
fail is worse than no assertion: it reads as coverage.

The acceptance file is copied into a scratch tree together with the
files it reads, so a mutation cannot touch the real tree.

    python tools/check_turn_server_acceptance_selftest.py
"""
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_REL = os.path.join('tests', 'acceptance', 'v1-n5-turn-server',
                         'acceptance.test.cjs')
DEPLOY_REL = os.path.join('examples', 'turn-server')
CI_REL = os.path.join('.github', 'workflows', 'ci.yml')


def _read(path):
    return io.open(path, 'r', encoding='utf-8').read()


def _write(path, text):
    io.open(path, 'w', encoding='utf-8', newline='\n').write(text)


def make_scratch():
    """A tree with just what the acceptance file reads."""
    tmp = tempfile.mkdtemp(prefix='nimrtc-n5-acceptance-')
    test_dir = os.path.join(tmp, os.path.dirname(TEST_REL))
    os.makedirs(test_dir)
    shutil.copy2(os.path.join(ROOT, TEST_REL),
                 os.path.join(tmp, TEST_REL))

    deploy = os.path.join(tmp, DEPLOY_REL)
    os.makedirs(deploy)
    src = os.path.join(ROOT, DEPLOY_REL)
    for name in os.listdir(src):
        full = os.path.join(src, name)
        if os.path.isfile(full):
            shutil.copy2(full, os.path.join(deploy, name))

    ci = os.path.join(tmp, CI_REL)
    os.makedirs(os.path.dirname(ci))
    shutil.copy2(os.path.join(ROOT, CI_REL), ci)
    return tmp


def run_acceptance(root):
    """Run the acceptance file in `root`; return (exit code, fail count)."""
    proc = subprocess.run(
        ['node', '--test', TEST_REL],
        cwd=root, capture_output=True, text=True,
        encoding='utf-8', errors='replace')
    out = (proc.stdout or '') + (proc.stderr or '')
    # node's TAP summary marks the counts with U+2139:
    #   "# pass 14"  "# fail 0"
    m = re.search(r'fail (\d+)', out)
    fails = int(m.group(1)) if m else -1
    return proc.returncode, fails, out


def mutate_text(path, old, new):
    text = _read(path)
    if old not in text:
        raise AssertionError('mutation anchor not found in %s: %r'
                             % (path, old[:70]))
    _write(path, text.replace(old, new, 1))


CASES = []


def case(name):
    def deco(fn):
        CASES.append((name, fn))
        return fn
    return deco


# ---------------------------------------------------------------------------
# The deployment defects
# ---------------------------------------------------------------------------

@case('the CI leg stops installing coturn natively')
def _c1(tmp):
    mutate_text(os.path.join(tmp, CI_REL),
                'sudo apt-get install -y coturn openssl',
                'sudo apt-get install -y openssl')


@case('the certificate generation step disappears')
def _c2(tmp):
    # The generator has TWO `openssl req` calls: one for the CA, one for
    # the leaf CSR.  Removing only the first leaves the second, so the
    # defect "the certificate generation step is gone" means removing
    # both.
    path = os.path.join(tmp, DEPLOY_REL, 'gen-certs.sh')
    text = _read(path)
    assert 'openssl req' in text
    _write(path, text.replace('openssl req', 'openssl rand'))


@case('the TLS listener moves onto the plain port')
def _c3(tmp):
    mutate_text(os.path.join(tmp, DEPLOY_REL, 'turnserver.conf'),
                'tls-listening-port=5349', 'tls-listening-port=3478')


@case('the CI leg starts using the compose file')
def _c4(tmp):
    mutate_text(os.path.join(tmp, CI_REL),
                'run: bash ./examples/turn-server/ci-acceptance.sh',
                'run: docker compose -f examples/turn-server/docker-compose.yml up -d')


@case('the acceptance file itself executes a lifecycle command')
def _c5(tmp):
    with io.open(os.path.join(tmp, TEST_REL), 'a', encoding='utf-8',
                 newline='\n') as fh:
        fh.write("\nrequire('node:child_process').execSync('docker compose up');\n")


@case('the deployment script loses its allocation assertion')
def _c6(tmp):
    mutate_text(os.path.join(tmp, DEPLOY_REL, 'ci-acceptance.sh'),
                'if ! grep -qE "Received relay addr"',
                'if ! grep -qE "relay-address-placeholder"')


# ---------------------------------------------------------------------------
# The three STUN/TURN protocol bugs from the 2026-11-20 handover list
# ---------------------------------------------------------------------------

@case('the STUN message type goes back to the RFC 3489 layout')
def _c7(tmp):
    # C1 moves from bit 8 back to bit 11.
    mutate_text(os.path.join(tmp, TEST_REL),
                'const c1 = (cls >> 1) & 0x01;     // C1 -> bit 8',
                'const c1 = ((cls >> 1) & 0x01) << 3;  // C1 -> bit 11')


@case('the REQUESTED-TRANSPORT protocol byte moves to byte 3')
def _c8(tmp):
    mutate_text(os.path.join(tmp, TEST_REL),
                'Buffer.from([0x11, 0x00, 0x00, 0x00])',
                'Buffer.from([0x00, 0x00, 0x00, 0x11])')


@case('the XOR-RELAYED-ADDRESS family byte is read from the reserved byte')
def _c9(tmp):
    mutate_text(os.path.join(tmp, TEST_REL),
                'const family = value[1];          // byte 0 是保留字节',
                'const family = value[0];          // byte 0 是保留字节')


@case('the attribute encoder stops padding to a 4-byte boundary')
def _c10(tmp):
    mutate_text(os.path.join(tmp, TEST_REL),
                'const padding = (4 - (value.length % 4)) % 4;',
                'const padding = 0;')


def main():
    print('=== baseline ===')
    tmp = make_scratch()
    try:
        code, fails, out = run_acceptance(tmp)
        print('  exit=%d fail=%d' % (code, fails))
        if code != 0 or fails != 0:
            sys.stdout.buffer.write(out[-3000:].encode('utf-8', 'replace'))
            print('\nthe baseline must be green -- fix that first')
            return 1
        print('  green, as required\n')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print('=== mutations ===')
    missed = []
    for name, fn in CASES:
        tmp = make_scratch()
        try:
            fn(tmp)
            code, fails, out = run_acceptance(tmp)
            caught = (code != 0 and fails > 0)
            print('  [%s] %s' % ('ok ' if caught else 'MISS', name))
            if not caught:
                missed.append(name)
                print('      exit=%d fail=%d -- the check stayed green'
                      % (code, fails))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    print('')
    if missed:
        print('%d mutation(s) were not caught' % len(missed))
        return 1
    print('every mutation was caught')
    return 0


if __name__ == '__main__':
    sys.exit(main())
