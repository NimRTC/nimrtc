import json, sys
path = '/mnt/c/Users/Administrator/.cursor/projects/d-MyOpen-NimRTC/agent-transcripts/8e2d542b-9bd2-43d3-b932-226f86bbdcec/8e2d542b-9bd2-43d3-b932-226f86bbdcec.jsonl'

with open(path) as f:
    for i, line in enumerate(f):
        try:
            obj = json.loads(line)
        except:
            continue
        content_str = json.dumps(obj)
        if 'dtls_gmssl_session' not in content_str and 'dtls_wolfssl_session' not in content_str and 'test_dtls_gmssl_tlcp' not in content_str:
            continue
        if obj.get('role') == 'assistant':
            for tc in obj.get('tool_calls', []):
                args_str = tc.get('function', {}).get('arguments', '{}')
                try:
                    args = json.loads(args_str)
                except:
                    continue
                p = args.get('path', '')
                c = args.get('contents', '')
                if 'gmssl_session' in p or 'wolfssl_session' in p or 'gmssl_tlcp' in p:
                    print(f"WRITE Line {i}: {p}")
                    print(f"  Contents length: {len(c)}")
                    print(f"  First 80: {c[:80]}")
                    print()
                elif 'dtls_gmssl_session' in p or 'dtls_wolfssl_session' in p:
                    print(f"WRITE Line {i}: {p}")
                    print(f"  Contents length: {len(c)}")
                    print()
