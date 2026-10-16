"""Build a self-contained local prototype from glyphs, fixtures and JavaScript."""
import gzip,json,textwrap
from pathlib import Path
D=Path(__file__).resolve().parent; ROOT=D.parents[1]
captures=json.loads((D/'captures.json').read_text())
fixtures=[
 dict(id='reflow-blank',title='Fox · following blank row',source='Your first example, unchanged. A 45-column grid places the 28-column dock at column 17.',columns=45,lines=['123','  the quick brown fox jumps over','  the sleeping','  dog','','456','  hello world']),
 dict(id='reflow-joined',title='Fox · joined paragraph',source='Your second example. Consecutive same-indent prose is joined, retaining the space between over and the. Use 50 columns.',columns=50,lines=['123','  the quick brown fox jumps over','  the quick brown fox jumps over','  the sleeping dog','','456']),
 dict(id='reflow-tail',title='Paragraph · no blank row',source='Constructed prose fixture: long first lines, a short final line, then a heading with no intervening blank. All words must stay within the original three rows.',columns=128,lines=[
 '123',
 '  The terminal menu needs a little room beside this paragraph while all of the running commands stay in place',
 '  and the existing text can use the space at the end of its final line without borrowing another blank row',
 '  or moving the next section.',
 '456',
 '  this heading remains on its original row',
 '',
 '789',
 '  A separate short paragraph already clears the dock.',
 '',
 '  These words should stay where they are.'
 ]),
 dict(id='reflow-work',title='Coding session · prose and code',source='Constructed review fixture mixing prose, code, a table and numbered headings. It exercises safe local reflow and literal-content fallback.',columns=128,lines=[
 '› Keep the menu in place while making space locally.',
 '',
 '123',
 '  The terminal menu needs a little room beside this paragraph while all of the running commands stay in place',
 '  with words intact and without changing the number of rows.',
 '456',
 '',
 '  const installation = resolveBundlePath("~/Applications/hardcore.app/Contents/MacOS/hardcore", options.destination);',
 '  source               process             state                   input                         terminal ownership',
 '  terminal.app         claude              alive                   connected                     original PTY master',
 '',
 '789',
 '  This paragraph has one long line that crosses the menu and it may borrow the blank row immediately below it.',
 '',
 '101112',
 '  A shorter section stays completely unchanged.',
 '',
 '  $ make test',
 '  PASS  same process after update',
 '  PASS  shell and terminal state preserved',
 '',
 '  The selected tab now uses one character row instead of two.',
 '',
 '› Review the changed lines and keep the code literal.',
 ]),
 dict(id='reflow-blocked',title='Literal output · cannot reflow',source='Constructed dense code/table fixture. Every row has literal syntax and reaches the final column. No safe reflow and no guaranteed rail.',columns=128,lines=[(f'{i+1:02} src/terminal/handoff.c | foreground remains attached | '+('terminal-state = original; '*5))[:128] for i in range(36)])
]

left_prose=(
 'The menu is only a local obstacle in the terminal display. A paragraph can begin above it using the full row width '
 'and then continue beside it without changing its words or its original indentation. When the paragraph reaches the '
 'bottom edge of the menu its following lines can become full width again. This keeps the dock in one stable place '
 'while the surrounding explanation uses the space that is already available. The text before the dock is not '
 'given a permanent margin and neither is the text below it. Only the rows beside the menu need a shorter measure. '
 'A small amount of spare space in the final line or a following blank row may be enough to make that fit. '
 'If the prose cannot fit within its local row budget then the terminal should keep its original layout and the menu '
 'should remain faintly visible underneath. Hovering the menu brings all of its controls forward at once so that '
 'a user can select a different tab or change a setting without searching for a visible fragment. '
 'Code blocks and tables continue to preserve their literal positions while we collect representative examples '
 'of the colored panels and box drawing used by coding assistants. Those examples can become regression cases '
 'with a known expected layout instead of encouraging the algorithm to guess at structure it does not understand.'
)
left_lines=['123','']+['  '+x for x in textwrap.wrap(left_prose,width=118)]+['','','456','  this section stays on its original row']
fixtures.insert(0,dict(id='reflow-left-band',title='Left dock · spanning paragraph',source='Constructed paragraph spanning above, beside and below the left menu. Local width changes are row-specific; the later numeric heading cannot move.',columns=128,lines=left_lines))

captures=fixtures+captures
chars=set(''.join(''.join(c['lines']) for c in captures))|set(chr(c) for c in range(32,127))|set(chr(c) for c in range(0x2500,0x2580))|set(chr(c) for c in range(0x2800,0x2900))|set('●○↑↓‹›×…−·–↦')
glyphs={}
with gzip.open(ROOT/'font/unifont-18.0.01.hex.gz','rt') as f:
 for line in f:
  cp,bits=line.strip().split(':')
  if chr(int(cp,16)) in chars and len(bits)==32:glyphs[str(int(cp,16))]=list(bytes.fromhex(bits))
def inline_json(data):return json.dumps(data,ensure_ascii=False).replace('</','<\\/')
html=(D/'template.html').read_text().replace('__DATA__',inline_json(captures)).replace('__GLYPHS__',inline_json(glyphs)).replace('__REFLOW__',(D/'reflow.js').read_text()).replace('__UI__',(D/'ui.js').read_text())
(D/'index.html').write_text(html)
(D/'profiles.json').write_text(json.dumps([{'id':c['id'],'source':c['source'],'rightmost':[len(x.rstrip()) for x in c['lines']]} for c in captures],indent=2)+'\n')
print('Built',D/'index.html')
