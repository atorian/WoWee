#!/usr/bin/env python3
"""Compare local notification, delayed angles, actual packets and callback order."""
import random
from check_original_collision import f32
from check_original_movement_reporting import cases as reporting_cases, probe_input as reporting_probe_input, main as compare

def cases():
    rng = random.Random(0x7413f0)
    for i, c in enumerate(reporting_cases()):
        c = dict(c, name=f'local_event_{i}', need_ack=bool(rng.getrandbits(1)),
                 stand_state=rng.choice([0, 1, 3]), player=bool(rng.getrandbits(1)),
                 turn_time=(c['time'] - rng.choice([0, 99, 100, 101, 1000, 0xffffffff])) & 0xffffffff,
                 pitch_time=(c['time'] - rng.choice([0, 99, 100, 101, 1000, 0xffffffff])) & 0xffffffff,
                 pending_turn=rng.choice([0, 0xbc, 0xbd]), pending_pitch=rng.choice([0, 0xbf, 0xc0]),
                 flush=i % 5 == 0)
        c['flags2'] |= rng.choice([0, 8, 16, 24])
        c['reported_facing'] = f32(c['facing'] + rng.choice([0, .05, .1, -.1, .11]))
        c['reported_pitch'] = f32(c['pitch'] + rng.choice([0, .05, .1, -.1, .11]))
        yield c

def probe_input(c):
    return reporting_probe_input(c).rstrip() + ' ' + ' '.join(map(str, [int(c['need_ack']),
        c['stand_state'], int(c['player']), c['turn_time'], c['pitch_time'],
        c['pending_turn'], c['pending_pitch'], int(c['flush'])])) + '\n'

if __name__ == '__main__':
    raise SystemExit(compare(cases, probe_input, 'original_movement_local_event_reference',
        'OriginalMovementLocalEvent', ('--local',),
        'ProcessLocalMoveEvent 0x7413f0, delayed angles 0x7219f0/0x721ac0/0x721b90/0x721c20, unchanged angles 0x71ae80; actual SendMovementUpdate and bytes; unit presentation/input callbacks observed, not executed; unattached, no active spline'))
