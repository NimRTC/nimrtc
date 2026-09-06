import hashlib, hmac, sys

def prf(secret_hex, label, seed_hex, out_len):
    secret = bytes.fromhex(secret_hex)
    seed   = bytes.fromhex(seed_hex)
    a_in = label.encode() + seed
    a = hmac.new(secret, a_in, hashlib.sha256).digest()
    p = b''
    while len(p) < out_len:
        p += hmac.new(secret, a + a_in, hashlib.sha256).digest()
        a  = hmac.new(secret, a, hashlib.sha256).digest()
    return p[:out_len].hex()

print('master_secret_KAT  =', prf('00'*32, 'master secret', '00'*64, 48))
print('key_block_KAT      =', prf('00'*32, 'key expansion', '00'*64, 40))
print('verify_data_KAT_sf =', prf('00'*32, 'server finished', '00'*64, 12))
print('verify_data_KAT_cf =', prf('00'*32, 'client finished', '00'*64, 12))
print('hmac_kat_rfc4231_1 =', hmac.new(b'\x0b'*20, b'Hi There', hashlib.sha256).hexdigest())
print('hmac_kat_rfc4231_2 =', hmac.new(b'Jefe', b'what do ya want for nothing?', hashlib.sha256).hexdigest())
# Same verify_data with label "client finished" but different seed (mimics handshake hash)
print('verify_data_KAT_nontriv =',
      prf('aa'*32, 'client finished',
          '01'*32 + '02'*32,  # client_random || server_random
          12))
print('verify_data_KAT_100B =', prf('11'*48, 'server finished', '22'*100, 100))
