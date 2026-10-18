#!/usr/bin/env python3
"""Review 13x25/26/27 against their original Terminus 12x24 source."""
from gen_hairline_compact import build_font,SIZES
from proof_hairline_widths import render
if __name__=='__main__':render(build_font,SIZES,(12,24),'compact-comparison.png')
