#!/usr/bin/env python3
"""Source files read once, and which of them mention a word.

Several sweeps ask one question of hundreds or thousands of names: which lines
mention this one. Asked by running grep once per name, it cost a process and a
full read of the tree each time - over a minute and a half across the sweeps
that did it - and needed a grep on the PATH. Asked by compiling a regex per
name and running it over every file, it cost the same minute without the
processes.

Read once, with each word mapped to the files containing it, the same answers
come back in a second or two from Python alone, on any host. The answers are
the ones grep gave: `lines_with_word` is `grep -w`, `lines_containing` is plain
`grep`, a substring anywhere in the line.
"""
import re
from pathlib import Path

WORD = re.compile(r"\w+")


class SourceIndex:
    def __init__(self, roots, suffixes=None):
        """Read every file under `roots`, or only those with one of `suffixes`.

        Files are visited in sorted order, so anything reported from here
        comes out the same on every host rather than in directory order.
        """
        self.text = {}
        self._files_with = {}
        self._vocabulary = None
        for root in roots:
            root = Path(root)
            if not root.is_dir():
                continue
            for path in sorted(root.rglob("*")):
                if suffixes is not None and path.suffix not in suffixes:
                    continue
                if not path.is_file():
                    continue
                try:
                    text = path.read_text(encoding="utf-8", errors="ignore")
                except OSError:
                    continue
                self.text[path] = text
                for word in set(WORD.findall(text)):
                    self._files_with.setdefault(word, []).append(path)

    def files_with_word(self, word):
        """The files in which `word` appears as a whole word, in read order."""
        return self._files_with.get(word, [])

    def lines_with_word(self, word, within=None):
        """(path, line number, line) wherever `word` stands as a whole word.

        `within`, when given, limits the search to those files.
        """
        pattern = re.compile(r"\b%s\b" % re.escape(word))
        for path in self.files_with_word(word):
            if within is not None and path not in within:
                continue
            for number, line in enumerate(self.text[path].split("\n"), 1):
                if word in line and pattern.search(line):
                    yield path, number, line

    def lines_containing(self, fragment, under=None):
        """(path, line number, line) wherever `fragment` appears at all.

        A substring, as plain grep matches it: "Sounds_" finds
        "attackSounds_". `under`, when given, is a directory the file has to
        be inside. The fragment must contain a word character - that is what
        narrows the files to read.
        """
        run = max(WORD.findall(fragment), key=len, default="")
        if not run:
            raise ValueError(f"{fragment!r} has no word in it to look up")
        paths = set()
        for word in self._words_containing(run):
            paths.update(self._files_with[word])
        for path in sorted(paths):
            if under is not None and not path.is_relative_to(under):
                continue
            for number, line in enumerate(self.text[path].split("\n"), 1):
                if fragment in line:
                    yield path, number, line

    def _words_containing(self, run):
        """Every word in the tree with `run` inside it."""
        if self._vocabulary is None:
            self._vocabulary = "\n" + "\n".join(self._files_with) + "\n"
        vocabulary = self._vocabulary
        found = set()
        at = vocabulary.find(run)
        while at != -1:
            start = vocabulary.rfind("\n", 0, at) + 1
            end = vocabulary.find("\n", at)
            found.add(vocabulary[start:end])
            at = vocabulary.find(run, end)
        return found
