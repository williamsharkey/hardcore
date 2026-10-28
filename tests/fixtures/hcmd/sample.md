# hcmd fixture

A paragraph with **bold**, *italic*, _also italic_, `inline code`, ~~struck~~ and a
[relative link](docs/other.md) plus <https://example.com/auto> and a bare
https://example.org/path. Snake_case_words stay as they are. A long line that has to wrap because it keeps going well past the width of the dump.

Setext heading
--------------

## Lists

- first item
- second item with `code`
  - nested item
  - [x] done task
  - [ ] open task
- third

1. one
2. two
   continued text

> A quote with **bold** inside
> that continues.
>
> > nested quote

```c
int main(void) {
	return 0;   /* a tab above */
}
```

    indented code

| Name | Count | Note |
|:-----|------:|:----:|
| apples | 3 | crisp and sweet, from the orchard down the road |
| pears | 12 | `soft` |

---

![diagram](img/diagram.png) and [ref link][ex].

Line with hard break  
next line &amp; an entity &#8212; done.

<!-- hidden comment -->
[ex]: https://example.net/ref
