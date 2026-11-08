'use strict';

// Curated stdlib metadata used by the extension's lightweight fallback
// completions/hover when a-lsp is unavailable. This is intentionally small and
// easy to maintain; a-lsp remains the authoritative language intelligence.
const STDLIB = {
  math: { detail: 'native module', doc: 'Math functions and constants.', members: ['sqrt','abs','min','max','pow','sin','cos','tan','asin','acos','atan','atan2','exp','log','log2','log10','floor','ceil','round','trunc','random','random_int','PI','E'] },
  io: { detail: 'native module', doc: 'Console I/O.', members: ['print','eprint','input','flush'] },
  fs: { detail: 'native module', doc: 'Filesystem operations.', members: ['read_file','write_file','append_file','file_exists','list_dir','remove_file','create_dir','remove_dir','rename','current_dir'] },
  collections: { detail: 'native module (bare import)', doc: 'Native collection/functional helpers. For pure-A Python-style containers, use quoted std/collections imports.', members: ['len','map','filter','reduce','any','all','sum','join','set'] },
  unsafe: { detail: 'native module', doc: 'Low-level unchecked operations.', members: ['get','set','address_of','load','store','offset'] },
  arena: { detail: 'native module', doc: 'Allocator operations.', members: ['new','pool','region','alloc','release','reset','used','total'] },
  core: { detail: 'native module', doc: 'Runtime panic/assert helpers.', members: ['panic','assert','assert_eq'] },
  time: { detail: 'native module', doc: 'Time primitives.', members: ['now','sleep'] },
  json: { detail: 'native module', doc: 'JSON parse/serialize/validate/query.', members: ['parse','stringify','pretty','is_valid','get','at','len','has','keys','kind','as_str','as_int','as_float','as_bool','as_array','as_map'] },
  binary: { detail: 'native module', doc: 'Binary encode/decode.', members: ['encode','decode'] },
  testing: { detail: 'native module', doc: 'Property-testing helpers.', members: ['seed','for_all_int','for_all_float','for_all_bool','for_all_string','for_all_elem'] },
  log: { detail: 'native module', doc: 'Structured logging.', members: ['set_level','set_json','debug','info','warn','error'] },
  metrics: { detail: 'native module', doc: 'Metrics counters/gauges/histograms.', members: ['counter','gauge','histogram','render','reset'] },
  trace: { detail: 'native module', doc: 'Tracing/span utilities.', members: ['start','end','current','set_attribute','add_event','inject','extract','spans','reset'] },
  net: { detail: 'native module', doc: 'TCP/UDP/polling/TLS primitives.', members: ['resolve','tcp_listen','tcp_connect','accept','send','recv','close','local_port','set_timeout','set_nonblocking','poll','udp_socket','udp_bind','udp_send','udp_recv','tls_connect','tls_available'] },
  http: { detail: 'native module', doc: 'HTTP client/server helpers.', members: ['get','post','request','serve','serve_once'] },
  terminal: { detail: 'native module', doc: 'Terminal/TTY control.', members: ['isatty','raw_mode','read_char','read_char_ms','write','flush','size','home'] },
  env: { detail: 'native module', doc: 'Environment variables.', members: ['get'] },
  repl: { detail: 'native/runtime module', doc: 'REPL evaluation hook.', members: ['eval'] },
  string: { detail: 'stdlib module (Python-inspired subset)', doc: 'ASCII constants and capwords.', members: ['whitespace','ascii_lowercase','ascii_uppercase','ascii_letters','digits','hexdigits','octdigits','punctuation','printable','capwords'] },
  textwrap: { detail: 'stdlib module (Python-inspired subset)', doc: 'Wrap/fill/dedent/indent/shorten.', members: ['wrap','fill','dedent','indent','shorten'] },
  statistics: { detail: 'stdlib module (Python-inspired subset)', doc: 'Descriptive statistics.', members: ['mean','fmean','median','median_low','median_high','mode','pvariance','variance','pstdev','stdev','geometric_mean','harmonic_mean'] },
  copy: { detail: 'stdlib module (adapted subset)', doc: 'Shallow/deep copy helpers.', members: ['copy','deepcopy'] },
  bisect: { detail: 'stdlib module (Python-inspired subset)', doc: 'Sorted list insertion/search.', members: ['bisect_left','bisect_right','bisect','bisect_left_range','bisect_right_range','bisect_range','insort_left','insort_right','insort','insort_left_range','insort_right_range','insort_range'] },
  keyword: { detail: 'stdlib module', doc: 'A/Python keyword lists and predicates.', members: ['kwlist','softkwlist','python_kwlist','python_softkwlist','iskeyword','issoftkeyword','ispykeyword','ispysoftkeyword'] },
  functools: { detail: 'stdlib module (Python-inspired subset)', doc: 'Functional helpers.', members: ['partial','partial1','partial2','partial3','partial4','reduce','reduce_init','cache','lru_cache','singledispatch'] },
  heapq: { detail: 'stdlib module (Python-inspired subset)', doc: 'Heap queue algorithms.', members: ['heappush','heappop','heapify','heapreplace','heappushpop','nlargest','nsmallest','merge'] },
  operator: { detail: 'stdlib module (adapted subset)', doc: 'Operator functions.', members: ['add','sub','mul','truediv','floordiv','mod','eq','ne','lt','le','gt','ge','truth','not_','and_','or_','getitem'] },
  pprint: { detail: 'stdlib module (Python-inspired subset)', doc: 'Pretty-printing helpers.', members: ['pprint','pformat','saferepr','isreadable','isrecursive','pp'] },
  shlex: { detail: 'stdlib module (Python-inspired subset)', doc: 'Shell-like lexing/quoting.', members: ['split','join','quote','Shlex','Shlex_new'] },
  fractions: { detail: 'stdlib module (Python-inspired subset)', doc: 'Exact rational arithmetic.', members: ['Fraction','Fraction_new','Fraction_from_float'] },
  bytes: { detail: 'stdlib module', doc: 'Byte/char utilities.', members: ['chr','ord','utf8_encode','utf8_decode','b2s','s2b'] },
  getopt: { detail: 'stdlib module (Python-inspired subset)', doc: 'CLI option parsing.', members: ['getopt','gnu_getopt','GetoptError'] },
  prelude: { detail: 'stdlib namespace', doc: 'Prelude helpers/types available via stdlib/prelude patterns.', members: ['prelude_version','Vec4','Result','Ok','Err','unwrap','is_ok','is_err','unwrap_or'] },
};

const KEYWORDS = ['fn','if','elif','else','while','for','in','loop','break','continue','return','defer','class','metaclass','enum','trait','type','import','from','as','match','and','or','not','true','false','None','self','pass','comptime','impl','where','do'];
const BUILTINS = ['print','eprint','input','flush','len','str','int','float','bool','range','map','filter','reduce','sum','any','all','min','max','abs','pow','sqrt','round','floor','ceil','sorted','reversed','enumerate','zip','open','type','isinstance','assert','panic'];

// Curated builtin signatures mirroring compiler/astra/frontend/name-resolution/
// builtin_docs.hpp (the authoritative table read by a-lsp). Used by the
// fallback hover when a-lsp is unavailable.
const BUILTIN_SIGS = {
  print: { signature: 'print(*objects, sep: String = " ", end: String = "\\n", file: IO = stdout, flush: bool = false) -> void', documentation: 'Writes values to a stream, separated, with an ending.', example: ">>> print('Hello, A!')\nHello, A!\n>>> print(1, 2, sep='-')\n1-2", seeAlso: 'eprint, open, close, flush' },
  eprint: { signature: 'eprint(*objects, sep: String = " ", end: String = "\\n", file: IO = stderr, flush: bool = false) -> void', documentation: 'Writes values to standard error (or file), separated, with an ending.', seeAlso: 'print' },
  open: { signature: 'open(path: String, mode: String = "w") -> IO', documentation: 'Opens a file for writing; mode is "w", "a", or "r".', seeAlso: 'close, print' },
  close: { signature: 'close(stream: IO)', documentation: 'Closes a handle returned by open().', seeAlso: 'open' },
  stdout: { signature: 'stdout: IO', documentation: 'The standard output stream handle.' },
  stderr: { signature: 'stderr: IO', documentation: 'The standard error stream handle.' },
  input: { signature: 'input([prompt]) -> String', documentation: 'Reads one line from standard input.' },
  flush: { signature: 'flush()', documentation: 'Flushes buffered standard output.' },
  len: { signature: 'len(value) -> i64', documentation: 'Length of a string, array, map, or set.', seeAlso: 'str' },
  str: { signature: 'str(value) -> String', documentation: 'Converts a value to its string representation.', seeAlso: 'print' },
  int: { signature: 'int(value) -> i64', documentation: 'Converts a number, bool, or numeric string to i64.' },
  float: { signature: 'float(value) -> f64', documentation: 'Converts a number, bool, or numeric string to f64.' },
  bool: { signature: 'bool(value) -> bool', documentation: 'Truth value of a value.' },
  range: { signature: 'range(stop) -> [i64]', documentation: 'Integers from 0 to stop (exclusive). Also range(start, stop[, step]).' },
  map: { signature: 'map(values, fn) -> array', documentation: 'Applies a function to every element.', seeAlso: 'filter' },
  filter: { signature: 'filter(values, fn) -> array', documentation: 'Keeps the elements for which a predicate is true.', seeAlso: 'map' },
  reduce: { signature: 'reduce(values, initial, fn) -> value', documentation: 'Folds an array from left to right.', seeAlso: 'sum' },
  sum: { signature: 'sum(values) -> number', documentation: 'Sum of an array of numbers.', seeAlso: 'reduce' },
  any: { signature: 'any(values, fn) -> bool', documentation: 'True when the predicate holds for some element.', seeAlso: 'all' },
  all: { signature: 'all(values, fn) -> bool', documentation: 'True when the predicate holds for every element.', seeAlso: 'any' },
  min: { signature: 'min(a, b) -> number', documentation: 'The smaller of two values of the same numeric type.', seeAlso: 'max' },
  max: { signature: 'max(a, b) -> number', documentation: 'The larger of two values of the same numeric type.', seeAlso: 'min' },
  abs: { signature: 'abs(x) -> number', documentation: 'Absolute value, keeping the argument type.', seeAlso: 'min, max' },
  pow: { signature: 'pow(base, exponent) -> f64', documentation: 'Raises base to the exponent.' },
  sqrt: { signature: 'sqrt(x) -> f64', documentation: 'Square root of x.', example: '>>> import std:math\n>>> math.sqrt(16)\n4.0', seeAlso: 'floor, ceil, round' },
  floor: { signature: 'floor(x) -> f64', documentation: 'Rounds down to the nearest integer value.' },
  ceil: { signature: 'ceil(x) -> f64', documentation: 'Rounds up to the nearest integer value.' },
  round: { signature: 'round(x) -> f64', documentation: 'Rounds to the nearest integer value.' },
  assert: { signature: 'assert(condition[, message])', documentation: 'Runtime error when the condition is false.' },
  panic: { signature: 'panic(message)', documentation: 'Aborts with exit status 2 and the message.' },
};

module.exports = { STDLIB, KEYWORDS, BUILTINS, BUILTIN_SIGS };
