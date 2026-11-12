#!/usr/bin/env python3
"""
mutate.py -- the mutation engine of ember's fuzz brain.

The byte-level mutator behind fuzz/tools/fuzz_brain.py, usable on its own:
import it (`sys.path.insert(0, "fuzz/tools"); from mutate import Engine,
Dictionary, StructuredInsert`), subclass it for target-specific tweaks, or
run it from the command line to dump mutants. Stdlib-only, single file, no
dependency on the rest of ember.

  python3 fuzz/tools/mutate.py --selftest
  python3 fuzz/tools/mutate.py --seed seed.bin --out /tmp/mutants --count 50

Three audiences
---------------

  1. Quick start:   instantiate `Engine(rng)` and call `.mutate(seed)`.
                    The defaults are a full havoc stage.

  2. Tuned use:     supply a `Dictionary`, a list of `StructuredInsert`
                    callables, an `op_weights` dict, and a `corpus`
                    callable for splice. Hook `pre_mutate` /
                    `post_process` for framing-aware fixups.

  3. Example impl:  every op is a small method with a one-line
                    rationale. Copy pieces into a target-specific
                    mutator if the abstraction is in the way.

Architecture
------------

A *mutation* is one or more stacked *ops* applied to a copy of the
seed buffer. Op selection is weighted; weights are materialized as a
flat alias array at construct time so each op pick is O(1).

The stack count is log-uniform, `1 << (1 + randint(0, 7))`, which
yields {2,4,8,16,32,64,128,256}. The engine samples log-uniformly
between `stack_min` and `stack_max`.

The op menu (22 entries):

  bit/byte:        BIT_FLIP, BYTE_FLIP, BYTE_ARITH, RANDOM_BYTE
  multi-byte arith:
                   WORD_ARITH_LE, WORD_ARITH_BE,
                   DWORD_ARITH_LE, DWORD_ARITH_BE
  interesting set:
                   SET_INTERESTING_U8,
                   SET_INTERESTING_U16_LE, SET_INTERESTING_U16_BE,
                   SET_INTERESTING_U32_LE, SET_INTERESTING_U32_BE
  chunk:           DELETE_CHUNK, CLONE_CHUNK, OVERWRITE_CHUNK,
                   SWAP_CHUNKS
  dictionary:      DICT_INSERT, DICT_OVERWRITE, DICT_INT_ADJ
  structured:      STRUCTURED_INSERT, STRUCTURED_OVERWRITE

Splice is *not* in the havoc op menu; it's a separate strategy. When
a `corpus` callable is configured, `mutate()` may, with probability
`splice_prob` (default 0.15), splice the seed against a donor from
the corpus *before* the havoc stack runs.

Dictionary
----------

A `Dictionary` is a list of byte fragments with optional weights, plus
integer tokens. The expected use is protocol tokens, magic constants and
command words that pure byte havoc is statistically unlikely to
synthesize. When no dictionary is supplied the dict ops have weight 0
and are never picked.

`parse_dict(path, target_order)` reads ember's dictionary format, one
token per line (docs/fuzz-ideas.md, "Dictionaries"):

    # comment; blank lines are ignored
    "GET /"              bytes; escapes \\\\ \\" \\t \\n \\r \\xHH
    hex:7f454c46         raw bytes
    u8:0x5a              an integer of 1, 2, 4 or 8 bytes, laid out in
    u16:0x8100           the target's byte order (`target_order`: the
    u32:0xdeadbeef       brain takes it from ember's HELLO)
    u64:0x0123456789abcdef
    u32le:0xdeadbeef     an explicit byte order, whatever the target
    u32be:0xdeadbeef     (network fields are big-endian everywhere)

Integer values may be written in any base Python's int(x, 0) reads, and
negative ones within the width's signed range. Integer tokens are also
fragments (DICT_INSERT, DICT_OVERWRITE), and DICT_INT_ADJ writes one of
them plus or minus 1: the off-by-one neighbour of a boundary check,
which a byte string cannot express.

Structured inserts
------------------

A `StructuredInsert` wraps a user-defined callable
`(rng) -> bytes` that synthesizes one *structurally valid* chunk.
Where a dictionary is a fixed set of static fragments, a structured
insert is a *generator function* that produces fresh, valid-by-
construction bytes on every call -- random length, random body
content, but always honoring whatever framing the target requires
(length prefixes, IAC escaping, checksums, etc.).

This is the bridge between pure byte fuzzing and grammar-based
generation. Users supply as many StructuredInsert instances as they
want; the engine picks one weighted-random when a structured op
fires, then optionally runs a post-`escape` over the result for
protocol-specific byte stuffing.

Pre/post mutation hooks
-----------------------

Two optional callables, both outside the op menu:

  pre_mutate(src) -> src'
      Runs once at the top of `mutate()`, before splice. Typical
      use: strip framing the engine shouldn't fuzz; decode an
      envelope so byte havoc lands on the payload, not the
      wrapper.

  post_process(out) -> out'
      Runs once at the bottom of `mutate()`, after the length cap.
      Typical use: recompute a checksum, re-wrap a framing layer,
      enforce a minimum length.

Reproducibility
---------------

Set `trace=True` to record the sequence of ops + parameters applied
in each `mutate()` call. Access via `engine.last_trace` (a list of
`OpRecord`) and `engine.last_trace_hash` (16-char sha1 prefix). Off
by default to keep the hot path allocation-free.

Trace records cover *only* havoc-stack ops, not splice and not the
pre/post hooks. For bit-exact replay across splice, the caller must
also ensure their `corpus` callable is deterministic given a fixed
RNG seed.

CLI
---

  python3 fuzz/tools/mutate.py --selftest
      Run built-in correctness checks (determinism, length cap, op
      weighting, dict parsing, trace+hash, structured insert).
      Exits 0 on success.

  python3 fuzz/tools/mutate.py --seed FILE --out DIR/ --count N
                    [--dict D] [--splice-from CORPUS_DIR]
                    [--weights W.json] [--max-size N] [--seed-rng K]
                    [--trace]
      Dump N mutants of FILE to DIR/. Useful for ad-hoc tooling
      (priming a corpus, sanity-checking a dictionary, A/B
      comparing weight profiles).

Self-contained: stdlib-only, Python 3.7+ (for dataclasses).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import struct
import sys
from dataclasses import dataclass, field
from enum import IntEnum
from pathlib import Path
from typing import Callable, Iterable, List, Optional, Sequence


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# "Interesting" values: live near branch boundaries (off-by-one,
# signed/unsigned transitions, MSB-set checks, RFC-defined sentinels).
INTERESTING_I8 = [-128, -1, 0, 1, 16, 32, 64, 100, 127]
INTERESTING_U8 = [0, 1, 16, 32, 64, 100, 127, 128, 255]
INTERESTING_U16 = [0, 1, 128, 255, 256, 512, 1000, 1024,
                   4096, 32767, 32768, 65535]
INTERESTING_U32 = [0, 1, 32768, 65535, 65536, 100663045,
                   2147483647, 2147483648, 4294967295]

ARITH_MAX = 35              # the usual default; per-op delta range [-N, +N]
MAX_BLOCK = 32              # cap on chunk-op extent
DEFAULT_MAX_SIZE = 65536    # hard cap on output length


# ---------------------------------------------------------------------------
# Op IDs
# ---------------------------------------------------------------------------

class OP(IntEnum):
    """Stable IDs for every op the engine can apply.

    Stable across versions: never renumber. New ops append. Used by
    `op_weights`, `OpRecord.op`, and the trace hash.
    """
    BIT_FLIP                 = 1
    BYTE_FLIP                = 2
    BYTE_ARITH               = 3
    WORD_ARITH_LE            = 4
    WORD_ARITH_BE            = 5
    DWORD_ARITH_LE           = 6
    DWORD_ARITH_BE           = 7
    SET_INTERESTING_U8       = 8
    SET_INTERESTING_U16_LE   = 9
    SET_INTERESTING_U16_BE   = 10
    SET_INTERESTING_U32_LE   = 11
    SET_INTERESTING_U32_BE   = 12
    RANDOM_BYTE              = 13
    DELETE_CHUNK             = 14
    CLONE_CHUNK              = 15
    OVERWRITE_CHUNK          = 16
    SWAP_CHUNKS              = 17
    DICT_INSERT              = 18
    DICT_OVERWRITE           = 19
    STRUCTURED_INSERT        = 20
    STRUCTURED_OVERWRITE     = 21
    DICT_INT_ADJ             = 22


# Default relative weights. Tunable via `Engine(op_weights={...})`.
# Set a weight to 0 to disable an op.
DEFAULT_OP_WEIGHTS = {
    OP.BIT_FLIP:                 8,
    OP.BYTE_FLIP:                6,
    OP.BYTE_ARITH:               6,
    OP.WORD_ARITH_LE:            6,
    OP.WORD_ARITH_BE:            4,
    OP.DWORD_ARITH_LE:           4,
    OP.DWORD_ARITH_BE:           3,
    OP.SET_INTERESTING_U8:       8,
    OP.SET_INTERESTING_U16_LE:   4,
    OP.SET_INTERESTING_U16_BE:   3,
    OP.SET_INTERESTING_U32_LE:   3,
    OP.SET_INTERESTING_U32_BE:   2,
    OP.RANDOM_BYTE:              6,
    OP.DELETE_CHUNK:             6,
    OP.CLONE_CHUNK:              5,
    OP.OVERWRITE_CHUNK:          5,
    OP.SWAP_CHUNKS:              2,
    OP.DICT_INSERT:              8,    # auto-zeroed when no dict
    OP.DICT_OVERWRITE:           6,    # auto-zeroed when no dict
    OP.STRUCTURED_INSERT:        8,    # auto-zeroed when no inserts
    OP.STRUCTURED_OVERWRITE:     4,    # auto-zeroed when no inserts
    OP.DICT_INT_ADJ:             4,    # auto-zeroed when the dict has no integers
}


# ---------------------------------------------------------------------------
# Dictionary
# ---------------------------------------------------------------------------

@dataclass
class Dictionary:
    """A weighted collection of byte fragments inserted verbatim during
    mutation.

    Example:
        d = Dictionary([b"GET ", b"POST ", b"HTTP/1.1\\r\\n"])
        d.add(b"\\xff\\xfd\\x18")             # IAC DO TTYPE
        d.add(b"important", weight=10)       # 10x more likely

    `pick(rng)` returns one fragment per weighted random draw via a
    pre-built flat alias array (O(1) selection). Empty dictionaries
    are valid; the engine simply disables DICT_* ops in that case.
    """
    fragments: List[bytes] = field(default_factory=list)
    weights: List[int] = field(default_factory=list)
    ints: List["IntToken"] = field(default_factory=list)   # also in fragments, encoded

    def __post_init__(self):
        if not self.weights:
            self.weights = [1] * len(self.fragments)
        if len(self.fragments) != len(self.weights):
            raise ValueError("fragments and weights length mismatch")
        self.fragments = [bytes(f) for f in self.fragments]
        self.weights = [max(1, int(w)) for w in self.weights]
        self._rebuild()

    def _rebuild(self):
        self._alias = [i for i, w in enumerate(self.weights)
                       for _ in range(w)]

    def add(self, fragment: bytes, weight: int = 1):
        if not fragment:
            return
        self.fragments.append(bytes(fragment))
        self.weights.append(max(1, int(weight)))
        self._rebuild()

    def extend(self, fragments: Iterable[bytes]):
        for f in fragments:
            self.add(f)

    def add_int(self, tok: "IntToken"):
        """An integer token: its encoding as a fragment, and the token for DICT_INT_ADJ."""
        self.ints.append(tok)
        self.add(tok.encode(tok.value))

    def merge(self, other: "Dictionary"):
        for frag, w in zip(other.fragments, other.weights):
            self.add(frag, w)
        self.ints.extend(other.ints)

    def pick(self, rng: random.Random) -> bytes:
        return self.fragments[rng.choice(self._alias)]

    def __len__(self):
        return len(self.fragments)

    def __bool__(self):
        return bool(self.fragments)


@dataclass
class IntToken:
    """An integer dictionary token: `width` bytes, in `order` ("little" or "big")."""
    value: int
    width: int
    order: str

    def encode(self, value: int) -> bytes:
        return (value % (1 << (8 * self.width))).to_bytes(self.width, self.order)


_ESCAPES = {"\\": 0x5c, '"': 0x22, "t": 0x09, "n": 0x0a, "r": 0x0d}


def _unquote(body: str, where: str) -> bytes:
    out = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out += c.encode("utf-8")
            i += 1
            continue
        if i + 1 >= len(body):
            raise ValueError(f"{where}: trailing backslash")
        nxt = body[i + 1]
        if nxt in _ESCAPES:
            out.append(_ESCAPES[nxt])
            i += 2
        elif nxt == "x":
            try:
                out.append(int(body[i + 2:i + 4], 16))
            except ValueError:
                raise ValueError(f"{where}: bad \\x escape") from None
            if len(body[i + 2:i + 4]) != 2:
                raise ValueError(f"{where}: truncated \\x escape")
            i += 4
        else:
            raise ValueError(f"{where}: unknown escape \\{nxt}")
    return bytes(out)


def parse_dict(path, target_order: str = "little") -> Dictionary:
    """Read an ember dictionary file (the format is in this module's docstring).

    `target_order` ("little" or "big") lays out the integer tokens that do not name a
    byte order. Errors raise ValueError naming the file and line.
    """
    if target_order not in ("little", "big"):
        raise ValueError(f"target_order must be 'little' or 'big', not {target_order!r}")
    d = Dictionary()
    p = Path(path)
    with p.open("rb") as fh:
        for lineno, raw in enumerate(fh, 1):
            where = f"{p}:{lineno}"
            try:
                line = raw.decode("utf-8", "strict").strip()
            except UnicodeDecodeError as e:
                raise ValueError(f"{where}: invalid utf-8: {e}") from None
            if not line or line.startswith("#"):
                continue
            if line.startswith('"'):
                if len(line) < 2 or not line.endswith('"'):
                    raise ValueError(f"{where}: unterminated string")
                frag = _unquote(line[1:-1], where)
                if not frag:
                    raise ValueError(f"{where}: empty token")
                d.add(frag)
                continue
            kind, sep, val = line.partition(":")
            if not sep or not val.strip():
                raise ValueError(f"{where}: expected \"text\", hex:BYTES or uN[le|be]:VALUE")
            kind, val = kind.strip(), val.strip()
            if kind == "hex":
                try:
                    frag = bytes.fromhex(val)
                except ValueError:
                    raise ValueError(f"{where}: bad hex bytes") from None
                if not frag:
                    raise ValueError(f"{where}: empty token")
                d.add(frag)
                continue
            order = target_order
            base = kind
            for suffix, o in (("le", "little"), ("be", "big")):
                if kind.endswith(suffix):
                    base, order = kind[:-2], o
            widths = {"u8": 1, "u16": 2, "u32": 4, "u64": 8}
            if base not in widths:
                raise ValueError(f"{where}: unknown token kind {kind!r}")
            width = widths[base]
            try:
                v = int(val, 0)
            except ValueError:
                raise ValueError(f"{where}: bad integer {val!r}") from None
            if not -(1 << (8 * width - 1)) <= v < (1 << (8 * width)):
                raise ValueError(f"{where}: {val} does not fit {width * 8} bits")
            d.add_int(IntToken(v % (1 << (8 * width)), width, order))
    return d


# ---------------------------------------------------------------------------
# Structured inserts
# ---------------------------------------------------------------------------

@dataclass
class StructuredInsert:
    """A user-defined generator for a structurally valid chunk.

    `factory(rng) -> bytes` returns a fresh, well-formed payload on
    every call. `escape`, when provided, runs once over the factory's
    output for protocol-specific byte stuffing (e.g. RFC 855 IAC-IAC
    escaping in telnet, byte-stuffing in PPP, length-recompute in
    LV/TLV protocols). `weight` controls relative probability when
    the engine selects among multiple structured inserts.

    Example -- telnet SB envelope with RFC 855 body escaping:

        def _telnet_sb_body(rng):
            opt = rng.choice([0, 1, 24, 31, 34, 39])
            body = bytes(rng.randint(0, 255)
                         for _ in range(rng.randint(0, 64)))
            return b"\\xff\\xfa" + bytes([opt]) + body + b"\\xff\\xf0"

        def _iac_escape(b):
            head, tail = b[:3], b[-2:]
            middle = b[3:-2].replace(b"\\xff", b"\\xff\\xff")
            return head + middle + tail

        telnet_sb = StructuredInsert(
            name="telnet_sb",
            factory=_telnet_sb_body,
            escape=_iac_escape,
            weight=2)

    `name` is purely informational (shows up in introspection logs);
    it does not need to be unique.
    """
    name: str
    factory: Callable[[random.Random], bytes]
    weight: int = 1
    escape: Optional[Callable[[bytes], bytes]] = None

    def produce(self, rng: random.Random) -> bytes:
        out = self.factory(rng)
        if self.escape is not None:
            out = self.escape(out)
        return bytes(out)


# ---------------------------------------------------------------------------
# Trace records (opt-in reproducibility)
# ---------------------------------------------------------------------------

@dataclass
class OpRecord:
    """One entry in `engine.last_trace`.

    `op` is the `OP` int; `pos` is the byte offset where the op
    operated; `payload` is the op-specific bytes used (insert
    fragment, overwrite delta, etc.). Records describe only the
    havoc stack -- splice and pre/post hooks are not recorded.
    """
    op: int
    pos: int
    payload: bytes = b""

    def to_json(self) -> dict:
        return {"op": int(self.op),
                "op_name": OP(self.op).name,
                "pos": self.pos,
                "payload": self.payload.hex()}


# ---------------------------------------------------------------------------
# Engine
# ---------------------------------------------------------------------------

class Engine:
    """Havoc + splice mutation engine.

    Parameters
    ----------
    rng :
        A `random.Random` instance. Inject the same seed twice and
        you get the same outputs (modulo what your factories /
        corpus callable / hooks do).
    dictionary :
        Optional `Dictionary`. When None or empty, DICT_* ops are
        disabled (weight forced to 0).
    structured_inserts :
        Optional sequence of `StructuredInsert` (or bare
        `(rng) -> bytes` callables, which are promoted to
        StructuredInsert with weight=1 and no escape). When empty,
        STRUCTURED_* ops are disabled.
    corpus :
        Optional `() -> bytes` callable returning a donor for splice.
        When None, splice is disabled and `mutate()` is havoc-only.
        The callable is invoked at most once per `mutate()` call
        (only when splice fires).
    max_size :
        Hard cap on output bytes; truncation after every op and
        before return.
    op_weights :
        Optional dict[OP -> int] overriding `DEFAULT_OP_WEIGHTS`. Set
        a weight to 0 to disable an op. Backings (dict, inserts) are
        re-checked after override -- a user can't enable
        STRUCTURED_INSERT when no inserts are configured.
    pre_mutate :
        Optional `(bytes) -> bytes` run at the top of `mutate()`,
        before splice. Use to strip framing the engine shouldn't
        touch.
    post_process :
        Optional `(bytes) -> bytes` run at the bottom of `mutate()`,
        after the length cap. Use to recompute checksums, re-wrap
        framing, enforce minimum length.
    stack_min, stack_max :
        Inclusive bounds on per-call stacked op count, sampled from
        the log-uniform distribution `1 << (1 + r)`. Defaults
        (2, 256) give the full 2..256 range.
    splice_prob :
        Probability in [0,1] that `mutate()` first splices the seed
        against a corpus donor before havoc. Default 0.15. Ignored
        when no `corpus` callable was provided.
    trace :
        When True, record each op's (id, pos, payload) into
        `last_trace` and compute `last_trace_hash` (16-char sha1).
        Costs memory + a hash pass; off by default.

    Public methods
    --------------
    mutate(src) -> bytes
        Top-level entry. Runs pre_mutate, optional splice, havoc
        stack, length cap, post_process. Always returns a new bytes
        object.
    havoc(src) -> bytes
        Run only the havoc stack. Skips splice, pre_mutate,
        post_process. Useful when the caller wants its own
        scheduling.
    splice(a, b) -> bytes
        Splice: random crossover point between the first
        and last differing byte positions of a and b. Returns a
        unchanged if there's no usable crossover point.

    Public attributes
    -----------------
    op_weights : dict[OP, int]
        The post-resolution weight table actually in use.
    last_trace : list[OpRecord]
        Populated when `trace=True`.
    last_trace_hash : str
        Populated when `trace=True`; empty string otherwise.
    """

    def __init__(
        self,
        rng: random.Random,
        dictionary: Optional[Dictionary] = None,
        structured_inserts: Optional[Sequence] = None,
        corpus: Optional[Callable[[], bytes]] = None,
        max_size: int = DEFAULT_MAX_SIZE,
        op_weights: Optional[dict] = None,
        pre_mutate: Optional[Callable[[bytes], bytes]] = None,
        post_process: Optional[Callable[[bytes], bytes]] = None,
        stack_min: int = 2,
        stack_max: int = 256,
        splice_prob: float = 0.15,
        trace: bool = False,
    ):
        self.rng = rng
        self.dictionary = dictionary
        self.corpus = corpus
        self.max_size = max(1, int(max_size))
        self.pre_mutate = pre_mutate
        self.post_process = post_process
        self.splice_prob = float(splice_prob)
        self.trace_enabled = bool(trace)

        # Normalize structured inserts: bare callables become
        # StructuredInsert("anon_N", fn, weight=1).
        self.structured_inserts: List[StructuredInsert] = []
        if structured_inserts:
            for i, s in enumerate(structured_inserts):
                if isinstance(s, StructuredInsert):
                    self.structured_inserts.append(s)
                else:
                    self.structured_inserts.append(
                        StructuredInsert(name=f"anon_{i}",
                                         factory=s, weight=1))
        self._structured_alias = [
            i for i, s in enumerate(self.structured_inserts)
            for _ in range(max(1, s.weight))
        ]

        self.set_op_weights(op_weights)

        # Stack-count alias: log-uniform 1 << (1 + r), bounded.
        self.stack_min = max(1, int(stack_min))
        self.stack_max = max(self.stack_min, int(stack_max))
        self._stack_choices = self._log_stack_choices(
            self.stack_min, self.stack_max)

        # Op dispatch table.
        self._dispatch = {
            OP.BIT_FLIP:                self._op_bit_flip,
            OP.BYTE_FLIP:               self._op_byte_flip,
            OP.BYTE_ARITH:              self._op_byte_arith,
            OP.WORD_ARITH_LE:           self._op_word_arith_le,
            OP.WORD_ARITH_BE:           self._op_word_arith_be,
            OP.DWORD_ARITH_LE:          self._op_dword_arith_le,
            OP.DWORD_ARITH_BE:          self._op_dword_arith_be,
            OP.SET_INTERESTING_U8:      self._op_set_interesting_u8,
            OP.SET_INTERESTING_U16_LE:  self._op_set_interesting_u16_le,
            OP.SET_INTERESTING_U16_BE:  self._op_set_interesting_u16_be,
            OP.SET_INTERESTING_U32_LE:  self._op_set_interesting_u32_le,
            OP.SET_INTERESTING_U32_BE:  self._op_set_interesting_u32_be,
            OP.RANDOM_BYTE:             self._op_random_byte,
            OP.DELETE_CHUNK:            self._op_delete_chunk,
            OP.CLONE_CHUNK:             self._op_clone_chunk,
            OP.OVERWRITE_CHUNK:         self._op_overwrite_chunk,
            OP.SWAP_CHUNKS:             self._op_swap_chunks,
            OP.DICT_INSERT:             self._op_dict_insert,
            OP.DICT_OVERWRITE:          self._op_dict_overwrite,
            OP.STRUCTURED_INSERT:       self._op_structured_insert,
            OP.STRUCTURED_OVERWRITE:    self._op_structured_overwrite,
            OP.DICT_INT_ADJ:            self._op_dict_int_adj,
        }

        self.last_ops: List[OP] = []     # the ops the last mutate()/havoc() applied
        self.last_trace: List[OpRecord] = []
        self.last_trace_hash: str = ""

    def set_op_weights(self, op_weights: Optional[dict] = None):
        """Resolve and install op weights: the defaults, then `op_weights`, then what
        the backings allow (no dictionary, no integers, no structured inserts: those
        ops get 0). The brain calls it again as it learns which ops pay off."""
        weights = dict(DEFAULT_OP_WEIGHTS)
        if op_weights:
            for k, v in op_weights.items():
                weights[OP(k)] = max(0, int(v))
        if not (self.dictionary and len(self.dictionary)):
            weights[OP.DICT_INSERT] = 0
            weights[OP.DICT_OVERWRITE] = 0
        if not (self.dictionary and self.dictionary.ints):
            weights[OP.DICT_INT_ADJ] = 0
        if not self.structured_inserts:
            weights[OP.STRUCTURED_INSERT] = 0
            weights[OP.STRUCTURED_OVERWRITE] = 0
        alias = [op for op, w in weights.items() if w > 0 for _ in range(w)]
        if not alias:
            raise ValueError(
                "no enabled ops -- check op_weights or supply a "
                "dictionary / structured inserts")
        self.op_weights = weights
        self._op_alias = alias

    @staticmethod
    def _log_stack_choices(lo: int, hi: int) -> List[int]:
        # 1 << (1 + randint(0, 7)) -> {2,4,8,16,32,64,128,256}.
        choices = []
        v = 2
        while v <= hi:
            if v >= lo:
                choices.append(v)
            v *= 2
        if not choices:
            choices = [lo]
        return choices

    # ---- top-level entry --------------------------------------------------

    def mutate(self, src: bytes) -> bytes:
        """Return a mutated copy of `src`.

        Pipeline (in order):
          1. `pre_mutate(src)` if configured.
          2. With probability `splice_prob`, splice against
             `corpus()`.
          3. Stack 2..256 (log-uniform) havoc ops.
          4. Length cap at `max_size`.
          5. `post_process(out)` if configured.
          6. If `trace`, compute `last_trace_hash`.
        """
        if self.pre_mutate is not None:
            src = self.pre_mutate(src)

        if (self.corpus is not None
                and self.splice_prob > 0.0
                and self.rng.random() < self.splice_prob):
            donor = self.corpus()
            if donor:
                src = self.splice(src, donor)

        buf = bytearray(src)
        self.last_ops = []
        if self.trace_enabled:
            self.last_trace = []

        n_ops = self.rng.choice(self._stack_choices)
        for _ in range(n_ops):
            self._apply_op(buf)
            if len(buf) > self.max_size:
                del buf[self.max_size:]

        out = bytes(buf)
        if self.post_process is not None:
            out = self.post_process(out)
            if len(out) > self.max_size:
                out = out[:self.max_size]

        if self.trace_enabled:
            h = hashlib.sha1()
            for r in self.last_trace:
                h.update(struct.pack("<HI", int(r.op), r.pos))
                h.update(r.payload)
            self.last_trace_hash = h.hexdigest()[:16]
        return out

    def havoc(self, src: bytes) -> bytes:
        """Run only the havoc stack.

        Skips splice, `pre_mutate`, `post_process`. Useful when the
        caller wants explicit control over splice scheduling.
        """
        buf = bytearray(src)
        self.last_ops = []
        if self.trace_enabled:
            self.last_trace = []
        n_ops = self.rng.choice(self._stack_choices)
        for _ in range(n_ops):
            self._apply_op(buf)
            if len(buf) > self.max_size:
                del buf[self.max_size:]
        return bytes(buf)

    def splice(self, a: bytes, b: bytes) -> bytes:
        """Splice: random crossover point between the first
        and last differing byte positions of `a` and `b`.

        Returns `a` unchanged when `a` and `b` have no differing bytes
        in their common prefix (so no usable crossover point exists).
        Either input being empty returns the other.
        """
        if not a:
            return bytes(b)
        if not b:
            return bytes(a)
        n = min(len(a), len(b))
        first = -1
        last = -1
        for i in range(n):
            if a[i] != b[i]:
                if first < 0:
                    first = i
                last = i
        if first < 0 or first == last:
            return bytes(a)
        p = self.rng.randint(first + 1, last)
        out = a[:p] + b[p:]
        if len(out) > self.max_size:
            out = out[:self.max_size]
        return bytes(out)

    # ---- op dispatch ------------------------------------------------------

    def _apply_op(self, buf: bytearray):
        op = self.rng.choice(self._op_alias)
        # Empty buffer: only ops that grow the buffer make sense.
        # Fall back to whichever growth-op is available.
        if not buf and op not in (
                OP.DICT_INSERT, OP.DICT_INT_ADJ, OP.STRUCTURED_INSERT,
                OP.CLONE_CHUNK, OP.RANDOM_BYTE):
            if self.dictionary:
                op = OP.DICT_INSERT
            elif self.structured_inserts:
                op = OP.STRUCTURED_INSERT
            else:
                op = OP.RANDOM_BYTE
        self.last_ops.append(op)
        try:
            self._dispatch[op](buf)
        except (IndexError, struct.error):
            # An op declined (buf too small for its width / empty
            # alias). Silently skip.
            return

    def _record(self, op: OP, pos: int, payload: bytes = b""):
        if self.trace_enabled:
            self.last_trace.append(OpRecord(int(op), pos, payload))

    # ---- bit/byte ops -----------------------------------------------------

    def _op_bit_flip(self, buf: bytearray):
        # Flip one bit. The cheapest interesting op.
        if not buf: return
        pos = self.rng.randrange(len(buf))
        bit = self.rng.randrange(8)
        buf[pos] ^= 1 << bit
        self._record(OP.BIT_FLIP, pos, bytes([bit]))

    def _op_byte_flip(self, buf: bytearray):
        # XOR a byte with 0xff. Faster than 8 bit_flips for the
        # all-bits-toggle case.
        if not buf: return
        pos = self.rng.randrange(len(buf))
        buf[pos] ^= 0xff
        self._record(OP.BYTE_FLIP, pos)

    def _op_byte_arith(self, buf: bytearray):
        # Add/sub a small delta mod 256. Hits off-by-one length
        # checks better than bit flips do.
        if not buf: return
        pos = self.rng.randrange(len(buf))
        delta = self.rng.randint(-ARITH_MAX, ARITH_MAX)
        if delta == 0: delta = 1
        buf[pos] = (buf[pos] + delta) & 0xff
        self._record(OP.BYTE_ARITH, pos, struct.pack("<b", delta))

    def _op_word_arith_le(self, buf: bytearray):
        # Add/sub on a 16-bit little-endian word.
        if len(buf) < 2: return
        pos = self.rng.randrange(len(buf) - 1)
        v = struct.unpack_from("<H", buf, pos)[0]
        delta = self.rng.randint(-ARITH_MAX, ARITH_MAX)
        if delta == 0: delta = 1
        struct.pack_into("<H", buf, pos, (v + delta) & 0xffff)
        self._record(OP.WORD_ARITH_LE, pos, struct.pack("<h", delta))

    def _op_word_arith_be(self, buf: bytearray):
        # Add/sub on a 16-bit big-endian word. Common in network
        # protocols.
        if len(buf) < 2: return
        pos = self.rng.randrange(len(buf) - 1)
        v = struct.unpack_from(">H", buf, pos)[0]
        delta = self.rng.randint(-ARITH_MAX, ARITH_MAX)
        if delta == 0: delta = 1
        struct.pack_into(">H", buf, pos, (v + delta) & 0xffff)
        self._record(OP.WORD_ARITH_BE, pos, struct.pack(">h", delta))

    def _op_dword_arith_le(self, buf: bytearray):
        if len(buf) < 4: return
        pos = self.rng.randrange(len(buf) - 3)
        v = struct.unpack_from("<I", buf, pos)[0]
        delta = self.rng.randint(-ARITH_MAX, ARITH_MAX)
        if delta == 0: delta = 1
        struct.pack_into("<I", buf, pos, (v + delta) & 0xffffffff)
        self._record(OP.DWORD_ARITH_LE, pos, struct.pack("<i", delta))

    def _op_dword_arith_be(self, buf: bytearray):
        if len(buf) < 4: return
        pos = self.rng.randrange(len(buf) - 3)
        v = struct.unpack_from(">I", buf, pos)[0]
        delta = self.rng.randint(-ARITH_MAX, ARITH_MAX)
        if delta == 0: delta = 1
        struct.pack_into(">I", buf, pos, (v + delta) & 0xffffffff)
        self._record(OP.DWORD_ARITH_BE, pos, struct.pack(">i", delta))

    def _op_set_interesting_u8(self, buf: bytearray):
        # Replace one byte with a known boundary value.
        if not buf: return
        pos = self.rng.randrange(len(buf))
        v = self.rng.choice(INTERESTING_U8)
        buf[pos] = v
        self._record(OP.SET_INTERESTING_U8, pos, bytes([v]))

    def _op_set_interesting_u16_le(self, buf: bytearray):
        if len(buf) < 2: return
        pos = self.rng.randrange(len(buf) - 1)
        v = self.rng.choice(INTERESTING_U16)
        struct.pack_into("<H", buf, pos, v)
        self._record(OP.SET_INTERESTING_U16_LE, pos,
                     struct.pack("<H", v))

    def _op_set_interesting_u16_be(self, buf: bytearray):
        if len(buf) < 2: return
        pos = self.rng.randrange(len(buf) - 1)
        v = self.rng.choice(INTERESTING_U16)
        struct.pack_into(">H", buf, pos, v)
        self._record(OP.SET_INTERESTING_U16_BE, pos,
                     struct.pack(">H", v))

    def _op_set_interesting_u32_le(self, buf: bytearray):
        if len(buf) < 4: return
        pos = self.rng.randrange(len(buf) - 3)
        v = self.rng.choice(INTERESTING_U32)
        struct.pack_into("<I", buf, pos, v)
        self._record(OP.SET_INTERESTING_U32_LE, pos,
                     struct.pack("<I", v))

    def _op_set_interesting_u32_be(self, buf: bytearray):
        if len(buf) < 4: return
        pos = self.rng.randrange(len(buf) - 3)
        v = self.rng.choice(INTERESTING_U32)
        struct.pack_into(">I", buf, pos, v)
        self._record(OP.SET_INTERESTING_U32_BE, pos,
                     struct.pack(">I", v))

    def _op_random_byte(self, buf: bytearray):
        # Plain random byte. Grows empty buffers by one byte.
        v = self.rng.randint(0, 255)
        if not buf:
            buf.append(v)
            self._record(OP.RANDOM_BYTE, 0, bytes([v]))
            return
        pos = self.rng.randrange(len(buf))
        buf[pos] = v
        self._record(OP.RANDOM_BYTE, pos, bytes([v]))

    # ---- chunk ops --------------------------------------------------------

    def _op_delete_chunk(self, buf: bytearray):
        # Delete a 1..MAX_BLOCK byte run. Essential for length
        # mismatch / truncation bugs.
        if len(buf) < 2: return
        n = self.rng.randint(1, min(MAX_BLOCK, len(buf) - 1))
        start = self.rng.randint(0, len(buf) - n)
        del buf[start:start + n]
        self._record(OP.DELETE_CHUNK, start, bytes([n]))

    def _op_clone_chunk(self, buf: bytearray):
        # Insert a copy of an existing region elsewhere in the
        # buffer. Grows the buffer by 1..MAX_BLOCK bytes.
        if not buf:
            v = self.rng.randint(0, 255)
            buf.append(v)
            self._record(OP.CLONE_CHUNK, 0, bytes([v]))
            return
        n = self.rng.randint(1, min(MAX_BLOCK, len(buf)))
        src_pos = self.rng.randint(0, len(buf) - n)
        dst_pos = self.rng.randint(0, len(buf))
        chunk = bytes(buf[src_pos:src_pos + n])
        buf[dst_pos:dst_pos] = chunk
        self._record(OP.CLONE_CHUNK, dst_pos, chunk)

    def _op_overwrite_chunk(self, buf: bytearray):
        # Overwrite a 1..MAX_BLOCK byte run. 70% clone-from-elsewhere,
        # 30% constant-fill -- often two separate ops; we
        # collapse them with a coin flip to keep the op menu small.
        if len(buf) < 2: return
        n = self.rng.randint(1, min(MAX_BLOCK, len(buf)))
        dst_pos = self.rng.randint(0, len(buf) - n)
        if self.rng.random() < 0.7:
            src_pos = self.rng.randint(0, len(buf) - n)
            chunk = bytes(buf[src_pos:src_pos + n])
        else:
            val = self.rng.randint(0, 255)
            chunk = bytes([val] * n)
        buf[dst_pos:dst_pos + n] = chunk
        self._record(OP.OVERWRITE_CHUNK, dst_pos, chunk)

    def _op_swap_chunks(self, buf: bytearray):
        # Swap two non-overlapping equal-length regions. Useful for
        # state-machine reordering bugs (option A before option B vs.
        # the reverse).
        if len(buf) < 4: return
        n = self.rng.randint(1, min(MAX_BLOCK // 2, len(buf) // 2))
        a_pos = self.rng.randint(0, len(buf) - n)
        b_pos = self.rng.randint(0, len(buf) - n)
        if abs(a_pos - b_pos) < n:
            return
        a_chunk = bytes(buf[a_pos:a_pos + n])
        b_chunk = bytes(buf[b_pos:b_pos + n])
        buf[a_pos:a_pos + n] = b_chunk
        buf[b_pos:b_pos + n] = a_chunk
        self._record(OP.SWAP_CHUNKS, a_pos,
                     struct.pack("<II", b_pos, n))

    # ---- dictionary ops ---------------------------------------------------

    def _op_dict_insert(self, buf: bytearray):
        # Splice in a verbatim dictionary fragment.
        if not self.dictionary: return
        frag = self.dictionary.pick(self.rng)
        pos = self.rng.randint(0, len(buf)) if buf else 0
        buf[pos:pos] = frag
        self._record(OP.DICT_INSERT, pos, frag)

    def _op_dict_overwrite(self, buf: bytearray):
        # Paste a dictionary fragment over existing bytes (no length
        # change).
        if not self.dictionary or not buf: return
        frag = self.dictionary.pick(self.rng)
        if len(frag) > len(buf): return
        pos = self.rng.randint(0, len(buf) - len(frag))
        buf[pos:pos + len(frag)] = frag
        self._record(OP.DICT_OVERWRITE, pos, frag)

    def _op_dict_int_adj(self, buf: bytearray):
        # An integer token plus or minus 1, in its own width and byte order: the
        # neighbour of a boundary the firmware checks. Overwrites in place when it
        # fits, else inserts.
        if not self.dictionary or not self.dictionary.ints: return
        tok = self.rng.choice(self.dictionary.ints)
        frag = tok.encode(tok.value + self.rng.choice((-1, 1)))
        if len(buf) >= len(frag) and self.rng.random() < 0.5:
            pos = self.rng.randint(0, len(buf) - len(frag))
            buf[pos:pos + len(frag)] = frag
        else:
            pos = self.rng.randint(0, len(buf)) if buf else 0
            buf[pos:pos] = frag
        self._record(OP.DICT_INT_ADJ, pos, frag)

    # ---- structured ops ---------------------------------------------------

    def _pick_structured(self) -> StructuredInsert:
        return self.structured_inserts[
            self.rng.choice(self._structured_alias)]

    def _op_structured_insert(self, buf: bytearray):
        # Insert a freshly synthesized, structurally valid chunk.
        if not self.structured_inserts: return
        si = self._pick_structured()
        chunk = si.produce(self.rng)
        if not chunk: return
        pos = self.rng.randint(0, len(buf)) if buf else 0
        buf[pos:pos] = chunk
        self._record(OP.STRUCTURED_INSERT, pos, chunk)

    def _op_structured_overwrite(self, buf: bytearray):
        # Paste a freshly synthesized chunk over existing bytes.
        if not self.structured_inserts or not buf: return
        si = self._pick_structured()
        chunk = si.produce(self.rng)
        if not chunk or len(chunk) > len(buf): return
        pos = self.rng.randint(0, len(buf) - len(chunk))
        buf[pos:pos + len(chunk)] = chunk
        self._record(OP.STRUCTURED_OVERWRITE, pos, chunk)


# ---------------------------------------------------------------------------
# CLI / self-test
# ---------------------------------------------------------------------------

def _selftest() -> int:
    """Built-in correctness checks. Exit 0 on success, nonzero on
    failure with a printed diagnostic."""

    print("[selftest] determinism ...", end=" ", flush=True)
    rng_a = random.Random(42)
    rng_b = random.Random(42)
    eng_a = Engine(rng_a)
    eng_b = Engine(rng_b)
    seed = b"AAAAAAAA" * 8
    out_a = [eng_a.mutate(seed) for _ in range(20)]
    out_b = [eng_b.mutate(seed) for _ in range(20)]
    assert out_a == out_b, "determinism broken"
    print("ok")

    print("[selftest] max_size cap ...", end=" ", flush=True)
    rng = random.Random(1)
    eng = Engine(rng, max_size=128)
    for _ in range(200):
        out = eng.mutate(b"x" * 100)
        assert len(out) <= 128, f"cap broken: {len(out)}"
    print("ok")

    print("[selftest] empty input ...", end=" ", flush=True)
    rng = random.Random(2)
    eng = Engine(rng, dictionary=Dictionary([b"\x01\x02\x03"]))
    for _ in range(20):
        out = eng.mutate(b"")
        assert isinstance(out, bytes)
    print("ok")

    print("[selftest] no-dict/no-inserts works ...", end=" ", flush=True)
    rng = random.Random(3)
    eng = Engine(rng)
    for _ in range(20):
        eng.mutate(b"hello world")
    print("ok")

    print("[selftest] op weights honored ...", end=" ", flush=True)
    rng = random.Random(4)
    w = {op: 0 for op in OP}
    w[OP.BIT_FLIP] = 1
    eng = Engine(rng, op_weights=w, stack_min=1, stack_max=1)
    for _ in range(50):
        out = eng.mutate(b"A" * 32)
        assert len(out) == 32
        diff = bytes(a ^ b for a, b in zip(out, b"A" * 32))
        popcount = sum(bin(b).count("1") for b in diff)
        assert popcount == 1, f"expected 1-bit diff, got {popcount}"
    print("ok")

    print("[selftest] splice degrades ...", end=" ", flush=True)
    rng = random.Random(5)
    eng = Engine(rng)
    assert eng.splice(b"AAA", b"AAA") == b"AAA"
    assert eng.splice(b"", b"BBB") == b"BBB"
    assert eng.splice(b"AAA", b"") == b"AAA"
    s = eng.splice(b"AAA", b"BBB")
    assert s in (b"ABB", b"AAB"), f"unexpected splice: {s!r}"
    print("ok")

    print("[selftest] dict parse ...", end=" ", flush=True)
    import tempfile

    def dict_file(text):
        with tempfile.NamedTemporaryFile("w", suffix=".dict", delete=False) as fh:
            fh.write(text)
            return fh.name

    path = dict_file('# comment\n\n"hello"\n"\\xff\\xfd\\x18"\n"a\\"b\\\\c\\r\\n"\n'
                     'hex:7f454c46\nu8:0x5a\nu16:0x8100\nu32:0xdeadbeef\n'
                     'u32be:0x01020304\nu16le:-2\nu64:1\n')
    try:
        for order in ("little", "big"):
            d = parse_dict(path, order)
            f = d.fragments
            assert b"hello" in f and b"\xff\xfd\x18" in f and b'a"b\\c\r\n' in f, f
            assert b"\x7fELF" in f and b"\x5a" in f
            assert (0x8100).to_bytes(2, order) in f and (0xdeadbeef).to_bytes(4, order) in f
            assert b"\x01\x02\x03\x04" in f, "u32be ignores the target order"
            assert b"\xfe\xff" in f, "u16le:-2 is two's complement, little-endian"
            assert (1).to_bytes(8, order) in f
            assert len(f) == 10 and len(d.ints) == 6, (len(f), len(d.ints))
    finally:
        os.unlink(path)
    for bad, why in (('name="x"\n', "expected"), ('"open\n', "unterminated"),
                     ('u24:1\n', "unknown token kind"), ('u8:256\n', "does not fit"),
                     ('hex:abc\n', "bad hex"), ('"\\q"\n', "unknown escape"),
                     ('""\n', "empty token")):
        path = dict_file("# ok\n" + bad)
        try:
            parse_dict(path)
        except ValueError as e:
            assert ":2:" in str(e) and why in str(e), (bad, str(e))
        else:
            raise AssertionError(f"accepted {bad!r}")
        finally:
            os.unlink(path)
    print("ok")

    print("[selftest] integer token +-1 ...", end=" ", flush=True)
    d = Dictionary()
    d.add_int(IntToken(0x1000, 2, "big"))
    w = {op: 0 for op in OP}
    w[OP.DICT_INT_ADJ] = 1
    eng = Engine(random.Random(9), dictionary=d, op_weights=w, stack_min=1, stack_max=1)
    seen = set()
    for _ in range(64):
        out = eng.mutate(b"")
        assert out in (b"\x0f\xff", b"\x10\x01"), out
        assert eng.last_ops == [OP.DICT_INT_ADJ]
        seen.add(out)
    assert len(seen) == 2, "both neighbours"
    print("ok")

    print("[selftest] op records and re-weighting ...", end=" ", flush=True)
    eng = Engine(random.Random(10), stack_min=4, stack_max=4)
    eng.mutate(b"hello world")
    assert len(eng.last_ops) == 4 and all(isinstance(o, OP) for o in eng.last_ops)
    w = {op: 0 for op in OP}
    w[OP.BYTE_FLIP] = 1
    eng.set_op_weights(w)
    eng.mutate(b"hello world")
    assert set(eng.last_ops) == {OP.BYTE_FLIP}
    assert eng.op_weights[OP.DICT_INT_ADJ] == 0, "no integers: DICT_INT_ADJ stays off"
    print("ok")

    print("[selftest] trace + hash ...", end=" ", flush=True)
    rng = random.Random(6)
    eng = Engine(rng, trace=True, stack_min=4, stack_max=4)
    eng.mutate(b"hello world")
    assert len(eng.last_trace) == 4
    assert len(eng.last_trace_hash) == 16
    print("ok")

    print("[selftest] structured insert ...", end=" ", flush=True)
    rng = random.Random(7)
    def make_sb(r):
        return b"\xff\xfa\x1f\x00\x00\x00\x18\xff\xf0"
    w = {op: 0 for op in OP}
    w[OP.STRUCTURED_INSERT] = 1
    eng = Engine(
        rng,
        structured_inserts=[StructuredInsert("sb", make_sb, weight=1)],
        op_weights=w, stack_min=1, stack_max=1)
    out = eng.mutate(b"x")
    assert b"\xff\xfa\x1f" in out, f"structured chunk missing: {out!r}"
    print("ok")

    print("[selftest] ALL PASSED")
    return 0


def _cli() -> int:
    ap = argparse.ArgumentParser(
        prog="mutate.py",
        description=(
            "The mutation engine of ember's fuzz brain. Use "
            "--selftest to verify, or --seed/--out/--count to dump "
            "mutants for tooling."))
    ap.add_argument("--selftest", action="store_true",
                    help="run built-in correctness checks and exit")
    ap.add_argument("--seed", type=Path,
                    help="seed input file")
    ap.add_argument("--out", type=Path,
                    help="output directory (will be created)")
    ap.add_argument("--count", type=int, default=100,
                    help="number of mutants to emit (default 100)")
    ap.add_argument("--dict", type=Path, action="append", default=[],
                    help="ember dictionary file (see the module docstring); repeatable")
    ap.add_argument("--endian", choices=("little", "big"), default="little",
                    help="byte order of integer tokens that name none (default little)")
    ap.add_argument("--max-size", type=int, default=DEFAULT_MAX_SIZE,
                    help="hard cap on mutant length")
    ap.add_argument("--seed-rng", type=int, default=0xC0FFEE,
                    help="RNG seed for reproducibility")
    ap.add_argument("--splice-from", type=Path,
                    help="corpus dir whose files become splice donors")
    ap.add_argument("--splice-prob", type=float, default=0.15,
                    help="splice probability per mutate() call")
    ap.add_argument("--weights", type=Path,
                    help="JSON file: {op_name: int_weight} overrides")
    ap.add_argument("--trace", action="store_true",
                    help="print per-mutant op-trace hash to stderr")
    args = ap.parse_args()

    if args.selftest:
        return _selftest()

    if not args.seed or not args.out:
        ap.error("--seed and --out are required (unless --selftest)")

    seed_data = args.seed.read_bytes()
    args.out.mkdir(parents=True, exist_ok=True)

    dictionary = None
    if args.dict:
        dictionary = Dictionary()
        for p in args.dict:
            dictionary.merge(parse_dict(p, args.endian))

    corpus_cb = None
    if args.splice_from:
        donors = [p.read_bytes()
                  for p in sorted(args.splice_from.iterdir())
                  if p.is_file()]
        if donors:
            donor_rng = random.Random(args.seed_rng ^ 0xD0D0D0)
            corpus_cb = lambda: donor_rng.choice(donors)

    weights = None
    if args.weights:
        raw = json.loads(args.weights.read_text())
        weights = {OP[k]: int(v) for k, v in raw.items()}

    rng = random.Random(args.seed_rng)
    eng = Engine(
        rng, dictionary=dictionary, corpus=corpus_cb,
        max_size=args.max_size, op_weights=weights,
        splice_prob=args.splice_prob, trace=args.trace)

    width = len(str(max(0, args.count - 1))) or 1
    for i in range(args.count):
        out = eng.mutate(seed_data)
        (args.out / f"{i:0{width}d}.bin").write_bytes(out)
        if args.trace:
            print(f"{i:0{width}d}\t{eng.last_trace_hash}",
                  file=sys.stderr)
    print(f"[ok] wrote {args.count} mutants to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(_cli())
