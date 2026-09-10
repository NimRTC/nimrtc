// Check Chrome logs
const fs = require('fs');
const path = 'D:/MyOpen/NimRTC/build/demo_p2p_audio_stderr.txt';
const data = fs.readFileSync(path, 'utf8');
const lines = data.split('\n');
// Find recent activity
const lastN = lines.slice(-100);
for (const line of lastN) {
    if (line.match(/audio|dtls|srtp|peer|track|unprotect|stats|RTP|inbound/)) {
        console.log(line);
    }
}
