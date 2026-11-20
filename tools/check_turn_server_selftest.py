"""Prove check_turn_server.py catches each defect it claims to catch.

Every case here re-introduces a defect that was actually in
examples/turn-server/ and asserts that the check fails, and names the
failure.  A guard that cannot fail is not a guard.
"""
import io, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECK = os.path.join(ROOT, 'tools', 'check_turn_server.py')
DIR = os.path.join(ROOT, 'examples', 'turn-server')


def run_check(root):
    """Run the checker against a temporary copy of the tree."""
    env = dict(os.environ)
    proc = subprocess.run(
        [sys.executable, CHECK],
        cwd=root, capture_output=True, text=True, env=env)
    return proc.returncode, proc.stdout + proc.stderr


def resolve(rel):
    """Where a mutation target lives.

    Most targets are in examples/turn-server/.  The CI workflow is
    not -- it is two directories up and one sideways.
    """
    if rel == 'ci.yml':
        return os.path.join(ROOT, '.github', 'workflows', 'ci.yml')
    return os.path.join(DIR, rel)


def with_mutated(files, mutate):
    """Copy the tree, apply `mutate`, run the check, restore."""
    backup = {}
    try:
        for rel in files:
            path = resolve(rel)
            backup[rel] = io.open(path, 'r', encoding='utf-8').read()
            io.open(path, 'w', encoding='utf-8', newline='\n').write(mutate(rel, backup[rel]))
        code, out = run_check(ROOT)
        return code, out
    finally:
        for rel, text in backup.items():
            path = resolve(rel)
            io.open(path, 'w', encoding='utf-8', newline='\n').write(text)


CASES = []


def case(name, files, mutate, needle):
    CASES.append((name, files, mutate, needle))


def case_tls_port_moved(files):
    # The defect: a second --tls-listening-port in the compose file,
    # which REPLACES the config's value and moves the TLS listener off
    # the port the client was told to use.
    def mutate(rel, text):
        if rel == 'docker-compose.yml':
            return text.replace(
                '      - --external-ip=$(detect-external-ip)',
                '      - --external-ip=$(detect-external-ip)\n'
                '      - --tls-listening-port=443', 1)
        return text
    return mutate


def case_unknown_option(files):
    # The defect: a config line naming an option coturn does not have.
    def mutate(rel, text):
        if rel == 'turnserver.conf':
            return text.replace('no-dtls=1', 'no-dtls=1\nno-dtlsv1=1', 1)
        return text
    return mutate


def case_machine_path(files):
    # The defect: one developer's absolute path, committed.
    def mutate(rel, text):
        if rel == 'turnserver.wsl.conf':
            return text.replace('@TURN_ROOT@', '/mnt/d/MyOpen/NimRTC/examples/turn-server')
        return text
    return mutate


def case_key_not_readable(files):
    # The defect: OpenSSL's 0600 key, unreadable by the image's user.
    def mutate(rel, text):
        if rel == 'gen-certs.sh':
            return text.replace('chmod 0644 "$key" 2>/dev/null || true', '')
        return text
    return mutate


def case_no_external_ip(files):
    # The defect: overriding `command` drops the image's CMD, so no
    # external IP is advertised and every relayed session is dead.
    def mutate(rel, text):
        if rel == 'docker-compose.yml':
            return text.replace('      - --external-ip=$(detect-external-ip)\n', '')
        return text
    return mutate


def case_evidence_undocumented(files):
    # The defect: the README never says how to tell TLS from plain.
    def mutate(rel, text):
        if rel == 'README.md':
            return text.replace('cipher=', 'CIPHER_PLACEHOLDER_')
        return text
    return mutate


def case_placeholder_not_substituted(files):
    # The defect: the WSL config has a placeholder and nothing fills it.
    def mutate(rel, text):
        if rel == 'run-native.sh':
            return text.replace('@TURN_ROOT@', 'PLACEHOLDER')
        return text
    return mutate


def case_config_spelling(files):
    # The defect: `--config` instead of `-c`.  coturn has no `config`
    # long option, so the server exits with "unrecognized option" and
    # the container never starts.
    def mutate(rel, text):
        if rel == 'docker-compose.yml':
            return text.replace('      - -c\n', '      - --config=/etc/coturn/turnserver.conf\n')
        return text
    return mutate


def case_config_not_mounted(files):
    # The defect: the config file is not mounted, so coturn falls back
    # to its built-in defaults -- no realm, no user, no certificate,
    # and therefore no TLS listener.
    def mutate(rel, text):
        if rel == 'docker-compose.yml':
            return text.replace(
                '      - ./turnserver.conf:/etc/coturn/turnserver.conf:ro\n', '')
        return text
    return mutate


def case_log_file_on_the_command_line(files):
    # The defect: the two runs are separated by a command-line
    # `--log-file`, which coturn silently ignores -- so both write to
    # the same file and the control destroys the positive server's
    # evidence.
    def mutate(rel, text):
        if rel == 'docker-compose.yml':
            return text.replace(
                '      - --external-ip=$(detect-external-ip)\n',
                '      - --external-ip=$(detect-external-ip)\n'
                '      - --log-file=/etc/coturn/logs/turnserver-plain.log\n')
        return text
    return mutate


def case_control_has_no_own_config(files):
    # The defect: the control has no configuration file of its own, so
    # it has no way to keep its log separate.
    def mutate(rel, text):
        if rel == 'turnserver.plain.conf':
            return text.replace('no-tls=1', '# no-tls=1')
        return text
    return mutate


def case_control_on_the_wrong_port(files):
    # The defect: the control listens on a different port, which makes
    # it a connection error rather than a protocol downgrade.
    def mutate(rel, text):
        if rel == 'turnserver.plain.conf':
            return text.replace('listening-port=5349', 'listening-port=9999')
        return text
    return mutate


def case_script_invoked_by_path(files):
    # The defect: CI invokes the script by path.  The exec bit is
    # recorded in the index, but a checkout on a machine with
    # core.fileMode=false cannot have it -- and the run then exits 126
    # ("permission denied") before the script ever starts.  This is
    # what actually happened on the first run of the leg.
    def mutate(rel, text):
        if rel == 'ci.yml':
            return text.replace(
                'run: bash ./examples/turn-server/gen-certs.sh',
                'run: ./examples/turn-server/gen-certs.sh')
        return text
    return mutate


def case_control_judged_by_failure_text(files):
    # The defect: the control is judged by the *text* of an OpenSSL
    # failure.  A failure's wording is not a contract, and a timeout
    # erases it entirely -- which is what happened on the CI runner.
    # The mutation relaxes the verdict so that the OpenSSL wording
    # counts as proof.
    def mutate(rel, text):
        if rel == 'ci-acceptance.sh':
            return text.replace(
                '    if ! grep -qE "Received relay addr" <<<"$plain_allocation"; then',
                '    if ! grep -qE "Received relay addr" <<<"$plain_allocation" '
                '&& ! grep -q "Cipher is (NONE)" <<<"$control"; then')
        return text
    return mutate


def case_no_plain_client_against_the_control(files):
    # The defect: only a TLS client is tried against the control.  Then
    # "the TLS client got nothing" would also be satisfied by a control
    # that is simply broken, and the control would prove nothing.
    # The mutation gives the plain client the '-S' flag, so both
    # clients ask for TLS and the plain side no longer proves the
    # control is alive.
    def mutate(rel, text):
        if rel != 'ci-acceptance.sh':
            return text
        lines = text.split('\n')
        for index, line in enumerate(lines):
            if 'plain_allocation=' not in line or 'turnutils_uclient' not in line:
                continue
            for offset in range(1, 8):
                if '-t -v -y -c' in lines[index + offset]:
                    lines[index + offset] = lines[index + offset].replace(
                        '-t -v -y -c', '-S -t -v -y -c')
                    return '\n'.join(lines)
            raise AssertionError('the plain client invocation moved')
        raise AssertionError('the plain client invocation moved')
    return mutate


case('the TLS listener is moved off 5349',
     ['docker-compose.yml'], case_tls_port_moved, 'tls-listening-port')

case('a config line names an option coturn does not have',
     ['turnserver.conf'], case_unknown_option, 'no-dtlsv1')

case("one developer's absolute path is committed",
     ['turnserver.wsl.conf'], case_machine_path, 'machine-specific absolute path')

case('the private key is left unreadable by the server user',
     ['gen-certs.sh'], case_key_not_readable, 'private key')

case('no external IP is advertised',
     ['docker-compose.yml'], case_no_external_ip, 'external-ip')

case('the README does not say how to tell TLS from plain',
     ['README.md'], case_evidence_undocumented, 'cipher=')

case('the WSL placeholder is never substituted',
     ['run-native.sh'], case_placeholder_not_substituted, '@TURN_ROOT@')

case('the config file is named with an option coturn does not have',
     ['docker-compose.yml'], case_config_spelling, '--config')

case('the config file is not mounted into the container',
     ['docker-compose.yml'], case_config_not_mounted, 'not mounted')

case('the two runs are separated by an ignored --log-file',
     ['docker-compose.yml'], case_log_file_on_the_command_line, '--log-file')

case('the negative control does not disable the TLS listeners',
     ['turnserver.plain.conf'], case_control_has_no_own_config, 'no-tls')

case('the negative control listens on the wrong port',
     ['turnserver.plain.conf'], case_control_on_the_wrong_port, 'listening-port')

case('CI invokes the script by path instead of through bash',
     ['ci.yml'], case_script_invoked_by_path, 'exit 126')

case('the control is judged by the text of an OpenSSL failure',
     ['ci-acceptance.sh'], case_control_judged_by_failure_text, 'timeout erases it')

case('no plain client is tried against the control',
     ['ci-acceptance.sh'], case_no_plain_client_against_the_control,
     'plain* turnutils_uclient')


def main():
    failures = []

    code, out = run_check(ROOT)
    if code != 0:
        failures.append('the tree as it stands does not pass the check:\n' + out)
    else:
        print('OK   the tree as it stands passes')

    for name, files, mutate, needle in CASES:
        code, out = with_mutated(files, mutate(files))
        if code == 0:
            failures.append(f'NOT CAUGHT: {name}')
        elif needle not in out:
            failures.append(
                f'CAUGHT BUT WRONG REASON: {name}\n'
                f'  expected the message to mention {needle!r}\n' + out)
        else:
            print(f'OK   caught: {name}')

    if failures:
        print('\nFAILURES:', file=sys.stderr)
        for f in failures:
            print('  ' + f, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
