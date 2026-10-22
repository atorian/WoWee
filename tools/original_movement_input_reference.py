#!/usr/bin/env python3
"""Execute original shared movement inputs without a spline or transport."""
from original_collision_reference import OriginalCollision

class OriginalMovementInput(OriginalCollision):
    def prepare_surface_world(self,case,body):self.body=body
    def run(self,case):
        self.surface_move(dict(case,elapsed=0),routine=0x6eac40)
        address,args,void=[
            (0x988a20,[1,0],False),(0x988a20,[0,0],False),(0x98c8d0,[],False),
            (0x988b00,[1],False),(0x988b00,[0],False),(0x98bf80,[],False),
            (0x988df0,[1],True),(0x988df0,[0],True),(0x989010,[],False),
            (0x989220,[1],True),(0x989220,[0],True),(0x989450,[],False),
            (0x9898e0,[1],False),(0x9898e0,[0],False),(0x989940,[],False),
            (0x9883f0,[1],False)][case['input']]
        result=self.call(address,args,self.body)
        state=self.surface_state(case,self.body)
        state['elapsed']=0
        return dict(**state,accepted=True if void else bool(result),
            flags2=int.from_bytes(self.cpu.mem_read(self.body+0x48,2),'little'),
            requests=[],events=[],geometry_status=0)
