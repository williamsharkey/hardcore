#!/usr/bin/env python3
"""Parse saved HTML into a small test DOM; no network or GUI process."""
from html.parser import HTMLParser
from pathlib import Path
import json
import sys

class Tree(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = {'tag': 'DOCUMENT', 'attrs': {}, 'children': []}
        self.stack = [self.root]

    def handle_starttag(self, tag, attrs):
        node = {'tag': tag.upper(), 'attrs': dict(attrs), 'children': []}
        self.stack[-1]['children'].append(node)
        if tag not in {'area','base','br','col','embed','hr','img','input','link','meta','param','source','track','wbr'}:
            self.stack.append(node)

    def handle_endtag(self, tag):
        for i in range(len(self.stack)-1, 0, -1):
            if self.stack[i]['tag'] == tag.upper():
                del self.stack[i:]
                break

    def handle_data(self, text):
        self.stack[-1]['children'].append(text)

if __name__ == '__main__':
    parser = Tree()
    parser.feed(Path(sys.argv[1]).read_text())
    json.dump(parser.root, sys.stdout, ensure_ascii=False)
