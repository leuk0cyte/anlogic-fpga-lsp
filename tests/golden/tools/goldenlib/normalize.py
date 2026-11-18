"""Normalisation and serialisation rules for golden snapshots.

Stability is the whole point of this module: two runs over the same corpus must produce
byte-identical files.  Every rule below exists because the reference implementation emits
something that is *not* a function of the input, or because the value legitimately differs
between machines but must not differ between runs on one machine.

Rules, in application order (see :func:`normalize`):

1. **Path rewriting** - any absolute path under a known corpus root or under the reference
   repository is rewritten to a stable token (``<corpus:NAME>/...`` or ``<repo>/...``) with
   forward slashes.  Unrecognised absolute paths keep only their basename so that a different
   checkout location cannot cause a diff.
2. **Volatile keys are dropped** - timestamps, durations, run/session identifiers and other
   values that change between two runs of identical input (``GOLDEN_VOLATILE_KEYS``).
3. **Volatile trailing keys are masked** - free-form strings that embed a timestamp but are
   otherwise semantic (e.g. a TD run directory ``td_20261109_141838``) are rewritten so the
   semantic part survives the diff but the clock reading does not.
4. **Dict keys are sorted**; list order is preserved because the reference implementation's
   ordering is part of the contract under test.

Serialisation (``dump_json``) is always ``indent=2``, ``sort_keys=True``, ``ensure_ascii=False``
and ``\\n`` newlines, written as UTF-8 without a BOM.
"""

from __future__ import annotations

import hashlib
import json
import os
import re

# Values that change between two runs of identical input.  Dropped wholesale.
GOLDEN_VOLATILE_KEYS = frozenset({
    "generatedAt",
    "mtime",
    "startedAt",
    "finishedAt",
    "elapsed",
    "elapsedMs",
    "elapsedSeconds",
    "runId",
    "run_id",
    "sessionId",
    "session_id",
    "timestamp",
    "time",
    "duration",
    "durationMs",
    "durationSeconds",
    "pid",
    "lastUpdate",
    "updatedAt",
    "createdAt",
    "bootTime",
    "uptimeMs",
    # Live-session liveness flags.  ``fpgalsp.info`` reports ``td_session`` and
    # ``device_loaded``, which depend on whether a TD child process happens to have finished
    # booting at the instant of the poll - not on the workspace.  They were observed to flip
    # between two runs of identical input, so they are dropped here.  What the session
    # *produced* (pins, device table, diagnostics) is still compared.
    "td_session",
    "device_loaded",
})

# Clock readings that appear *inside* free-form strings.  Masked rather than dropped so the
# surrounding structure still participates in the diff.
_CLOCK_PATTERNS = [
    (re.compile(r"\d{8}_\d{6}"), "<TS>"),                       # td_20261109_141838
    (re.compile(r"\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:?\d{2})?"), "<TS>"),
    (re.compile(r"\d{2}-\d{2} \d{2}:\d{2}:\d{2}"), "<TS>"),
    (re.compile(r"\b\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\b"), "<TS>"),
]

_TOKEN_RE = re.compile(r"<(corpus:[A-Za-z0-9_.-]+|repo)>")

# Number formatting: floats are rounded so that harmless last-bit differences between two
# runs do not create diff noise.  Structured data here is coordinates and counts, so 6 decimal
# places is far more precision than the contract actually carries.
FLOAT_DIGITS = 6


class Normalizer:
    """Rewrites absolute paths to stable tokens.

    ``roots`` maps a stable token name to an absolute directory; the longest matching prefix
    wins so a nested root is preferred over its ancestor.
    """

    def __init__(self, roots=None):
        self.roots = {}
        for name, path in (roots or {}).items():
            resolved = os.path.normpath(os.path.abspath(path))
            self.roots[name.lower()] = (resolved, resolved.replace("\\", "/"))
        self._ordered = sorted(self.roots.items(), key=lambda item: len(item[1][0]), reverse=True)
        self._unanchored = self._build_unanchored()

    # -- path handling --------------------------------------------------------
    def relative_token(self, path):
        """Return ``<token>/rel/path`` for a path under a known root, else ``None``."""
        if not isinstance(path, str) or not path:
            return None
        normal = os.path.normpath(os.path.abspath(path))
        for name, (native, _posix) in self._ordered:
            if normal == native:
                return "<%s>" % name
            prefix = native if native.endswith(os.sep) else native + os.sep
            if normal.lower().startswith(prefix.lower()):
                rel = normal[len(prefix):].replace("\\", "/")
                return "<%s>/%s" % (name, rel)
        return None

    def rewrite_string(self, text):
        """Rewrite every path occurrence (absolute or bare) inside a free-form string."""
        for name, (_native, posix) in self._ordered:
            # Longest root first.  Windows paths are case-insensitive but the server may echo
            # either separator, so match both.
            pattern = re.compile(re.escape(posix).replace("/", r"[\\/]"), re.IGNORECASE)
            text = pattern.sub("<%s>" % name, text)
        # The server sometimes names a sibling path that is not itself a corpus root: TD
        # resolves a project through the ancestor workspace, so a diagnostic about
        # "%WORKSPACE%/CorpusAlpha" can appear while the corpus root is "%WORKSPACE%/CorpusAlpha (elsewhere)".
        # Unanchored patterns use lookarounds instead of \b so Chinese text and slashes around
        # the path are handled correctly.
        for pattern, replacement in self._unanchored:
            text = pattern.sub(replacement, text)
        return text

    def _build_unanchored(self):
        """Path-shaped fragments that must be neutralised but are not under a root.

        Two classes:
        * drive paths ``D:/x`` / ``D:\\x`` anywhere in the string;
        * a bare POSIX root (``/opt/x``) immediately preceded by a character that cannot be
          part of a path, i.e. a path starting a sentence or a quoted value.
        The drive letter is kept as ``<abs:>`` and the rest shortened to its basename, so two
        runs agree but the meaningful tail survives the diff.
        """
        patterns = []
        patterns.append((
            re.compile(r"\b([A-Za-z]):[\\/]([^\s\"'<>|,;:()\[\]]+)"),
            lambda m: "<abs:>" + self._fold_rest(m.group(2)),
        ))
        return patterns

    @staticmethod
    def _fold_rest(rest):
        """Fold the remainder of an unknown path to ``.../basename`` plus its parent names.

        Keeping only the basename would merge unrelated files that share a name, so one parent
        level is retained when present.
        """
        parts = [part for part in re.split(r"[\\/]", rest) if part]
        if not parts:
            return ""
        tail = parts[-2:] if len(parts) >= 2 else parts
        return ".../" + "/".join(tail)

    def decode_token(self, text):
        """Inverse of the path rewriting, for callers that need a real filesystem path."""
        return _TOKEN_RE.sub(
            lambda match: self.roots.get(match.group(1).lower(), ("", match.group(0)))[1],
            text,
        )

    # -- recursive normalisation ---------------------------------------------
    def normalize(self, value, key=None):
        if isinstance(value, dict):
            out = {}
            for child_key, child_value in value.items():
                if child_key in GOLDEN_VOLATILE_KEYS:
                    continue
                out[child_key] = self.normalize(child_value, child_key)
            return out
        if isinstance(value, (list, tuple)):
            return [self.normalize(item, key) for item in value]
        if isinstance(value, str):
            return self._normalize_string(value, key)
        if isinstance(value, bool):
            return value
        if isinstance(value, float):
            return round(value, FLOAT_DIGITS)
        return value

    def _normalize_string(self, text, key):
        token = self.relative_token(text)
        if token is not None:
            return token
        rewritable = key in _PATHISH_KEYS or "\\" in text or re.search(r"[A-Za-z]:[/\\]", text)
        if rewritable:
            text = self.rewrite_string(text)
        text = _TOKEN_RE.sub(lambda m: "<%s>" % m.group(1).lower(), text)
        if "<?xml" not in text[:64]:
            for pattern, replacement in _CLOCK_PATTERNS:
                text = pattern.sub(replacement, text)
        return text


# Keys whose string values are (or may contain) filesystem paths even when the value is not an
# absolute path itself.
_PATHISH_KEYS = frozenset({
    "uri", "path", "file", "dir", "root", "rootUri", "al", "alPath", "target", "source",
    "location", "locationUri", "includePath", "logPath", "tdPath", "exe", "cwd", "detail",
    "message", "targetUri", "sourceUri",
})


def normalize(value, roots=None):
    return Normalizer(roots).normalize(value)


# ---------------------------------------------------------------- serialisation
def dump_json(value):
    """Canonical snapshot text: indent=2, sorted keys, UTF-8, ``\\n`` newlines."""
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


MAX_SNAPSHOT_BYTES = 2 * 1024 * 1024
MAX_SNAPSHOT_LINES = 2000


def _measure(value):
    text = dump_json(value)
    return len(text.encode("utf-8")), text.count("\n")


def _fits(measure, limit):
    """Compare ``(bytes, lines)`` against a limit *per dimension*.

    Python compares tuples lexicographically, so ``(100, 5000) <= (2097152, 2000)`` is True even
    though the line count is far over budget.  Every size decision therefore goes through this
    helper instead of a bare tuple comparison.
    """
    return measure[0] <= limit[0] and measure[1] <= limit[1]


def _effectively_empty(value):
    """True for a container that carries nothing but truncation markers.

    Dropping such a value buys no space but loses the informative marker, so the shrinker leaves
    it alone and lets an outer level make the decision.
    """
    if isinstance(value, dict):
        if _is_marker(value):
            return True
        return all(key == MARKER_KEY or _effectively_empty(child) for key, child in value.items())
    if isinstance(value, list):
        return all(_effectively_empty(item) for item in value)
    return False


def _is_marker(value):
    return isinstance(value, dict) and value.get(MARKER_KEY) is True


MARKER_KEY = "__goldenTruncated"

# Bytes of headroom kept free when a list is shortened, so the marker that reports the dropped
# tail (``droppedItems`` plus a note) still fits.  Without this the marker pushes the result
# back over budget and the tail gets dropped a second time.
MARKER_RESERVE_BYTES = 400
MARKER_RESERVE_LINES = 6

# Keys the shrinker may trim.  Everything else (the snapshot envelope, a case's ``id``, a
# tool's ``arguments``) is structure the diff depends on, so it is preserved verbatim and only
# values *below* those keys are reduced.
SHRINKABLE_KEYS = frozenset({"result", "cases"})


def _shrink(value, limit, depth=0):
    """Reduce ``value`` until ``_measure`` fits ``limit`` while keeping the JSON structure.

    Returns ``(value, measure)``.  The reduction is *bounded*: it descends the tree until it
    finds containers, so the cost is proportional to the amount actually rewritten rather than
    to a fixed set of whole-document attempts.  The result is always valid JSON; a shortened
    list gains a trailing marker carrying a digest of the dropped tail, so a diff reports
    "content shrank" instead of silently agreeing.
    """
    measure = _measure(value)
    if _fits(measure, limit) or depth > 40:
        return value, measure

    if isinstance(value, list):
        if not value:
            return value, measure
        items = [item for item in value if not _is_marker(item)]
        if len(items) != len(value):
            # Already shortened once.  Only collapse further if the marker itself is what is
            # overflowing; otherwise leave the list alone so an outer level can make progress.
            if len(value) == 1:
                collapsed = _digest(value[0], "marker could not be reduced")
                return collapsed, _measure(collapsed)
            return value, measure

        marker = {MARKER_KEY: True, "droppedItems": len(value),
                  "_note": "list shortened to fit the snapshot size limit"}
        marker_measure = _measure([marker])
        inner = (max(256, limit[0] - marker_measure[0] - MARKER_RESERVE_BYTES),
                 max(1, limit[1] - marker_measure[1] - MARKER_RESERVE_LINES))

        # 1. Everything from the first element on is worth more than the suffix, so find the
        #    longest prefix that fits the inner budget.
        low, high = 0, len(value)
        while low < high:
            mid = (low + high + 1) // 2
            if _fits(_measure(value[:mid]), inner):
                low = mid
            else:
                high = mid - 1

        # Keep one slot free for the marker so the shortened list still reports what it dropped.
        if 0 < low < len(value):
            low -= 1

        if low >= 1:
            # 2. Give each kept element an even share so one huge element cannot starve the rest.
            marker = {MARKER_KEY: True, "droppedItems": len(value) - low,
                      "_note": "list shortened to fit the snapshot size limit"}
            marker_measure = _measure([marker])
            budget = (max(256, (limit[0] - marker_measure[0]) // low),
                      max(4, (limit[1] - marker_measure[1]) // low))
            kept = [_shrink(item, budget, depth + 1)[0] for item in value[:low]]
            result = kept + [marker]
            return result, _measure(result)

        # 3. Not even one element fits the inner budget: replace the elements by digests, which
        #    keeps the list shape (and therefore ``cases`` stays an array).
        digests = [_digest(item, "list item too large") for item in value]
        if _fits(_measure(digests), inner):
            result = digests + [marker]
            return result, _measure(result)
        collapsed = _digest(value, "list collection too large with %d items" % len(value))
        return collapsed, _measure(collapsed)

    if isinstance(value, dict):
        shrunk = dict(value)
        # Sibling values (schema, caseCount, category, skipped...) are tiny next to the payload
        # and are part of the contract, so only the shrinkable children are ever reduced.
        for key in sorted(SHRINKABLE_KEYS, key=lambda k: _measure(value.get(k)), reverse=True):
            if key not in value or not isinstance(value[key], (dict, list)) or not value[key]:
                continue
            current = _measure(shrunk)
            if _fits(current, limit):
                return shrunk, current
            if _effectively_empty(value[key]):
                # Already reduced (for example an MCP tool result replaced by a digest at
                # capture time); replacing it again would only lose the marker.
                continue
            # Hand the child the budget left over by everything that has to survive verbatim,
            # so a large sibling key cannot make the child believe it must fit the whole limit.
            others = _measure({k: v for k, v in value.items() if k != key})
            inner = (max(256, limit[0] - others[0]), max(1, limit[1] - others[1]))
            shrunk[key] = _shrink(value[key], inner, depth + 1)[0]
        return shrunk, _measure(shrunk)

    return value, measure


def _digest(value, reason):
    payload = dump_json(value).encode("utf-8")
    return {
        MARKER_KEY: True,
        "_note": reason,
        "bytes": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
    }


def build_snapshot(cases, extra=None):
    """Wrap ``cases`` into the snapshot envelope written to disk.

    ``cases`` is a list of per-case objects, each already normalised.  The envelope carries the
    case list plus run-level metadata that must itself be deterministic.

    Truncation contract (also documented in ``README.md``):

    * The file is always valid JSON and always within ``MAX_SNAPSHOT_BYTES`` /
      ``MAX_SNAPSHOT_LINES``.
    * Reduction is deterministic and structure-preserving: bulkiest values first, lists
      shortened from the tail, every removal replaced by a marker carrying a digest of what was
      dropped.
    * If even a fully reduced envelope cannot fit (an extreme edge case: the 2000-line budget is
      smaller than the case *skeleton* alone), the whole snapshot collapses to a single marker
      listing every case id and a digest of the lost content.  A comparison against such a file
      always reports a difference, never a false pass.

    Shrinking only removes content; it never invents it, so a C++ implementation cannot satisfy
    a truncated snapshot by guessing.
    """
    envelope = {"schema": 1, "caseCount": len(cases), "cases": cases}
    for key, value in (extra or {}).items():
        envelope[key] = value

    limit = (MAX_SNAPSHOT_BYTES, MAX_SNAPSHOT_LINES)
    shrunk, measure = _shrink(envelope, limit)
    if _fits(measure, limit):
        return dump_json(shrunk)

    # Last resort: one marker describing the whole snapshot.  Still valid JSON, still stable,
    # and honest about the loss.
    overflow = {
        "schema": envelope.get("schema"),
        "category": envelope.get("category"),
        "caseCount": len(cases),
        MARKER_KEY: True,
        "_note": ("snapshot was larger than %d bytes / %d lines even after structural "
                  "reduction; the whole payload was replaced by this marker"
                  % (MAX_SNAPSHOT_BYTES, MAX_SNAPSHOT_LINES)),
        "caseIds": [case.get("id") for case in cases if isinstance(case, dict)],
        "skipped": envelope.get("skipped"),
        "identity": _digest(envelope, "full snapshot payload"),
    }
    return dump_json(overflow)


def write_snapshot(path, text):
    """Write snapshot text with ``\\n`` newlines, UTF-8, no BOM, and a trailing newline."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)