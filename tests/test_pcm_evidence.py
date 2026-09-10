import hashlib
import json
import struct
import wave
import pytest
from tools.cdj_dsp.pcm_evidence import report

@pytest.mark.parametrize('corrupt',[False,True])
def test_distinct_channels_and_corruption(tmp_path, corrupt):
    samples=[x for i in range(588) for x in (i,-i)]
    pcm=struct.pack('<1176h',*samples)
    wav=tmp_path/'test.wav'
    with wave.open(str(wav),'wb') as w:
        w.setparams((2,2,44100,588,'NONE','not compressed'));w.writeframes(pcm)
    a=struct.pack('<588f',*(x/32768 for x in samples[::2]))
    b=struct.pack('<588f',*(x/32768 for x in samples[1::2]))
    if corrupt: b=a
    records=[dict(event='pcm_observation',pc=0xc003c698,b4=0,b6=2,a6=0,packets=1),
        dict(event='pcm_observation',pc=0xc003c398,a6=1176,a4=0x118381e0,
             raw_banks=dict(address=0x118381e0,hex=pcm.hex())),
        dict(event='pcm_observation',pc=0xc0049e6c,return_observation=True,packets=2,
             output_plane0=dict(address=0x11800200,hex=a.hex()),
             output_plane1=dict(address=0x11800fc8,hex=b.hex()))]
    trace=tmp_path/'trace.jsonl'
    trace.write_text(''.join(json.dumps(r)+'\n' for r in records))
    (tmp_path/'gate.json').write_text(json.dumps(dict(passed=True,repeat_matches=True,
        final_state_and_memory_match=True,verified_connected_stops=1,
        trace_sha256=hashlib.sha256(trace.read_bytes()).hexdigest())))
    assert report(tmp_path,wav)['passed'] is not corrupt
    trace.write_text(trace.read_text()+'\n')
    with pytest.raises(ValueError,match='verified repeat'): report(tmp_path,wav)
