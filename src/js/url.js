// URL and URLSearchParams: the WHATWG URL Standard (https://url.spec.whatwg.org/).
//
// Compiled on first use (px_web.c): the global URL and URLSearchParams are
// getters that evaluate this file, so programs that never touch them pay
// neither the compile time nor the memory. The whole file is one function
// expression; it returns { URL, URLSearchParams }.
//
// Differences from the standard, all on purpose:
// - International domain names: UTS #46 needs Unicode tables PXJS does not
//   carry. The common cases work (see domainToASCII): ignored characters,
//   full-width forms, lower-casing, then Punycode. Text that needs NFC or
//   NFKC mapping (decomposed accents, styled letters) is encoded as it is,
//   which differs from browsers. "xn--" labels are checked for valid
//   Punycode only.
// - Only UTF-8 (the standard's other encodings concern HTML forms).
// - No URL.createObjectURL / revokeObjectURL (there are no Blobs).
// The Web Platform Tests' URL data (tests/wpt/url) is the reference:
// tools/url_wpt.py runs it and lists the expected failures with reasons.
(function () {
  'use strict';

  const EOF = -1;
  const fromCP = String.fromCodePoint;
  const SPECIAL = { __proto__: null, 'ftp': 21, 'file': null, 'http': 80, 'https': 443, 'ws': 80, 'wss': 443 };
  const isSpecialScheme = s => s in SPECIAL;
  const isAlpha = c => (c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A);
  const isDigit = c => c >= 0x30 && c <= 0x39;
  const isHex = c => isDigit(c) || (c >= 0x41 && c <= 0x46) || (c >= 0x61 && c <= 0x66);
  const isAlnum = c => isAlpha(c) || isDigit(c);
  const hexVal = c => (c <= 0x39 ? c - 0x30 : (c | 0x20) - 0x61 + 10);
  const HEX = '0123456789ABCDEF';

  // ------------------------------------------------------------ strings and bytes

  // USVString: lone surrogates become U+FFFD
  function usv(v) {
    const s = String(v); // a Symbol throws TypeError, as WebIDL says
    let i = 0;
    for (; i < s.length; i++) {
      const c = s.charCodeAt(i);
      if (c < 0xD800 || c > 0xDFFF) continue;
      const d = i + 1 < s.length ? s.charCodeAt(i + 1) : 0;
      if (c <= 0xDBFF && d >= 0xDC00 && d <= 0xDFFF) { i++; continue; }
      break;
    }
    if (i >= s.length) return s;
    let r = s.slice(0, i);
    for (; i < s.length; i++) {
      const c = s.charCodeAt(i);
      const d = i + 1 < s.length ? s.charCodeAt(i + 1) : 0;
      if (c >= 0xD800 && c <= 0xDBFF && d >= 0xDC00 && d <= 0xDFFF) { r += s[i] + s[i + 1]; i++; }
      else if (c >= 0xD800 && c <= 0xDFFF) r += '\uFFFD';
      else r += s[i];
    }
    return r;
  }

  // code points (a lone surrogate is one code point here, as in the standard)
  function codePoints(s) {
    const a = [];
    for (const ch of s) a.push(ch.codePointAt(0));
    return a;
  }

  function utf8Bytes(cp, out) {
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
    if (cp < 0x80) out.push(cp);
    else if (cp < 0x800) out.push(0xC0 | (cp >> 6), 0x80 | (cp & 63));
    else if (cp < 0x10000) out.push(0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
    else out.push(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 63), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
    return out;
  }

  function utf8Encode(s) {
    const out = [];
    for (const ch of s) utf8Bytes(ch.codePointAt(0), out);
    return out;
  }

  // UTF-8 decode without BOM, replacing each maximal invalid subpart with U+FFFD
  function utf8Decode(bytes) {
    let out = '', need = 0, cp = 0, lower = 0x80, upper = 0xBF;
    for (let i = 0; i < bytes.length; i++) {
      const b = bytes[i];
      if (need === 0) {
        if (b <= 0x7F) out += String.fromCharCode(b);
        else if (b >= 0xC2 && b <= 0xDF) { need = 1; cp = b & 0x1F; }
        else if (b >= 0xE0 && b <= 0xEF) { if (b === 0xE0) lower = 0xA0; if (b === 0xED) upper = 0x9F; need = 2; cp = b & 0xF; }
        else if (b >= 0xF0 && b <= 0xF4) { if (b === 0xF0) lower = 0x90; if (b === 0xF4) upper = 0x8F; need = 3; cp = b & 7; }
        else out += '\uFFFD';
        continue;
      }
      if (b < lower || b > upper) {
        need = 0; lower = 0x80; upper = 0xBF;
        out += '\uFFFD';
        i--; // the byte starts over
        continue;
      }
      lower = 0x80; upper = 0xBF;
      cp = (cp << 6) | (b & 0x3F);
      if (--need === 0) out += fromCP(cp);
    }
    if (need) out += '\uFFFD';
    return out;
  }

  function percentDecode(bytes) {
    const out = [];
    for (let i = 0; i < bytes.length; i++) {
      const b = bytes[i];
      if (b === 0x25 && i + 2 < bytes.length && isHex(bytes[i + 1]) && isHex(bytes[i + 2])) {
        out.push(hexVal(bytes[i + 1]) * 16 + hexVal(bytes[i + 2]));
        i += 2;
      } else out.push(b);
    }
    return out;
  }

  // ------------------------------------------------------------ percent-encode sets

  const inC0 = c => c < 0x20 || c > 0x7E;
  const inFragment = c => inC0(c) || c === 0x20 || c === 0x22 || c === 0x3C || c === 0x3E || c === 0x60;
  const inQuery = c => inC0(c) || c === 0x20 || c === 0x22 || c === 0x23 || c === 0x3C || c === 0x3E;
  const inSpecialQuery = c => inQuery(c) || c === 0x27;
  const inPath = c => inQuery(c) || c === 0x3F || c === 0x5E || c === 0x60 || c === 0x7B || c === 0x7D;
  const inUserinfo = c => inPath(c) || c === 0x2F || c === 0x3A || c === 0x3B || c === 0x3D || c === 0x40 ||
                          (c >= 0x5B && c <= 0x5E) || c === 0x7C;
  const inComponent = c => inUserinfo(c) || (c >= 0x24 && c <= 0x26) || c === 0x2B || c === 0x2C;
  const inForm = c => inComponent(c) || c === 0x21 || (c >= 0x27 && c <= 0x29) || c === 0x7E;

  const pctByte = b => '%' + HEX[b >> 4] + HEX[b & 15];

  function encodeCP(cp, inSet) {
    if (!inSet(cp)) return fromCP(cp);
    let s = '';
    for (const b of utf8Bytes(cp, [])) s += pctByte(b);
    return s;
  }

  function encodeString(s, inSet) {
    let out = '';
    for (const ch of s) out += encodeCP(ch.codePointAt(0), inSet);
    return out;
  }

  // ------------------------------------------------------------ hosts

  const forbiddenHost = c => c === 0 || c === 9 || c === 0xA || c === 0xD || c === 0x20 || c === 0x23 || c === 0x2F ||
                             c === 0x3A || c === 0x3C || c === 0x3E || c === 0x3F || c === 0x40 || c === 0x5B ||
                             c === 0x5C || c === 0x5D || c === 0x5E || c === 0x7C;
  const forbiddenDomain = c => forbiddenHost(c) || c <= 0x1F || c === 0x25 || c === 0x7F;

  function parseIPv6(s) {
    const input = codePoints(s), len = input.length;
    const address = [0, 0, 0, 0, 0, 0, 0, 0];
    let piece = 0, compress = null, p = 0;
    const at = i => (i < len ? input[i] : EOF);
    if (at(p) === 0x3A) {
      if (at(p + 1) !== 0x3A) return null;
      p += 2;
      piece++;
      compress = piece;
    }
    while (at(p) !== EOF) {
      if (piece === 8) return null;
      if (at(p) === 0x3A) {
        if (compress !== null) return null;
        p++;
        piece++;
        compress = piece;
        continue;
      }
      let value = 0, length = 0;
      while (length < 4 && isHex(at(p))) { value = value * 16 + hexVal(at(p)); p++; length++; }
      if (at(p) === 0x2E) {
        if (length === 0) return null;
        p -= length;
        if (piece > 6) return null;
        let seen = 0;
        while (at(p) !== EOF) {
          let v4 = null;
          if (seen > 0) {
            if (at(p) === 0x2E && seen < 4) p++;
            else return null;
          }
          if (!isDigit(at(p))) return null;
          while (isDigit(at(p))) {
            const n = at(p) - 0x30;
            if (v4 === null) v4 = n;
            else if (v4 === 0) return null;
            else v4 = v4 * 10 + n;
            if (v4 > 255) return null;
            p++;
          }
          address[piece] = address[piece] * 0x100 + v4;
          seen++;
          if (seen === 2 || seen === 4) piece++;
        }
        if (seen !== 4) return null;
        break;
      } else if (at(p) === 0x3A) {
        p++;
        if (at(p) === EOF) return null;
      } else if (at(p) !== EOF) return null;
      address[piece] = value;
      piece++;
    }
    if (compress !== null) {
      let swaps = piece - compress;
      piece = 7;
      while (piece !== 0 && swaps > 0) {
        const t = address[piece];
        address[piece] = address[compress + swaps - 1];
        address[compress + swaps - 1] = t;
        piece--;
        swaps--;
      }
    } else if (piece !== 8) return null;
    return address;
  }

  function serializeIPv6(a) {
    // the first longest run of two or more zero pieces is compressed
    let best = -1, bestLen = 1;
    for (let i = 0; i < 8;) {
      if (a[i] !== 0) { i++; continue; }
      let j = i;
      while (j < 8 && a[j] === 0) j++;
      if (j - i > bestLen) { best = i; bestLen = j - i; }
      i = j;
    }
    let out = '', ignore0 = false;
    for (let i = 0; i < 8; i++) {
      if (ignore0 && a[i] === 0) continue;
      ignore0 = false;
      if (best === i) {
        out += i === 0 ? '::' : ':';
        ignore0 = true;
        continue;
      }
      out += a[i].toString(16);
      if (i !== 7) out += ':';
    }
    return out;
  }

  // an IPv4 part: a number, or null
  function parseIPv4Number(s) {
    if (s === '') return null;
    let r = 10;
    if (s.length >= 2 && s[0] === '0' && (s[1] === 'x' || s[1] === 'X')) { s = s.slice(2); r = 16; }
    else if (s.length >= 2 && s[0] === '0') { s = s.slice(1); r = 8; }
    if (s === '') return 0;
    let n = 0;
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      const d = isDigit(c) ? c - 0x30 : isHex(c) ? hexVal(c) : 99;
      if (d >= r) return null;
      n = n * r + d;
    }
    return n;
  }

  function endsInNumber(s) {
    const parts = s.split('.');
    if (parts[parts.length - 1] === '') {
      if (parts.length === 1) return false;
      parts.pop();
    }
    const last = parts[parts.length - 1];
    if (last !== '' && /^[0-9]+$/.test(last)) return true;
    return parseIPv4Number(last) !== null;
  }

  // the serialized address, or null
  function parseIPv4(s) {
    const parts = s.split('.');
    if (parts[parts.length - 1] === '' && parts.length > 1) parts.pop();
    if (parts.length > 4) return null;
    const nums = [];
    for (const p of parts) {
      const n = parseIPv4Number(p);
      if (n === null) return null;
      nums.push(n);
    }
    for (let i = 0; i < nums.length - 1; i++) if (nums[i] > 255) return null;
    if (nums[nums.length - 1] >= 256 ** (5 - nums.length)) return null;
    let v = nums[nums.length - 1];
    for (let i = 0; i < nums.length - 1; i++) v += nums[i] * 256 ** (3 - i);
    const o = [];
    for (let i = 0; i < 4; i++) { o.unshift(v % 256); v = Math.floor(v / 256); }
    return o.join('.');
  }

  // RFC 3492 decoding, only to check that an "xn--" label is valid
  function punycodeValid(s) {
    const base = 36, tmin = 1, tmax = 26, skew = 38, damp = 700;
    let n = 128, i = 0, bias = 72, out = 0;
    let b = s.lastIndexOf('-');
    if (b < 0) b = 0;
    for (let j = 0; j < b; j++) if (s.charCodeAt(j) >= 0x80) return false;
    out = b;
    for (let k = b > 0 ? b + 1 : 0; k < s.length;) {
      const oldi = i;
      let w = 1;
      for (let t = base; ; t += base) {
        if (k >= s.length) return false;
        const c = s.charCodeAt(k++);
        const d = c - 48 < 10 ? c - 22 : c - 65 < 26 ? c - 65 : c - 97 < 26 ? c - 97 : base;
        if (d >= base) return false;
        i += d * w;
        if (i > 0x7FFFFFFF) return false;
        const th = t <= bias ? tmin : t >= bias + tmax ? tmax : t - bias;
        if (d < th) break;
        w *= base - th;
        if (w > 0x7FFFFFFF) return false;
      }
      out++;
      let delta = oldi === 0 ? Math.floor((i - oldi) / damp) : (i - oldi) >> 1;
      delta += Math.floor(delta / out);
      let kk = 0;
      while (delta > ((base - tmin) * tmax) >> 1) { delta = Math.floor(delta / (base - tmin)); kk += base; }
      bias = kk + Math.floor(((base - tmin + 1) * delta) / (delta + skew));
      n += Math.floor(i / out);
      i %= out;
      if (n > 0x10FFFF || (n >= 0xD800 && n <= 0xDFFF)) return false;
      i++;
    }
    return true;
  }

  // RFC 3492 encoding of one label's code points
  function punycodeEncode(cps) {
    const base = 36, tmin = 1, tmax = 26, skew = 38, damp = 700;
    const digit = d => String.fromCharCode(d < 26 ? d + 97 : d + 22);
    let n = 128, delta = 0, bias = 72, out = '';
    for (const c of cps) if (c < 0x80) out += String.fromCharCode(c);
    const b = out.length;
    let h = b;
    if (b > 0) out += '-';
    while (h < cps.length) {
      let m = 0x110000;
      for (const c of cps) if (c >= n && c < m) m = c;
      delta += (m - n) * (h + 1);
      n = m;
      for (const c of cps) {
        if (c < n) delta++;
        if (c !== n) continue;
        let q = delta;
        for (let k = base; ; k += base) {
          const t = k <= bias ? tmin : k >= bias + tmax ? tmax : k - bias;
          if (q < t) break;
          out += digit(t + ((q - t) % (base - t)));
          q = Math.floor((q - t) / (base - t));
        }
        out += digit(q);
        let d = h === b ? Math.floor(delta / damp) : delta >> 1;
        d += Math.floor(d / (h + 1));
        let kk = 0;
        while (d > ((base - tmin) * tmax) >> 1) { d = Math.floor(d / (base - tmin)); kk += base; }
        bias = kk + Math.floor(((base - tmin + 1) * d) / (d + skew));
        delta = 0;
        h++;
      }
      delta++;
      n++;
    }
    return out;
  }

  // Domain to ASCII: UTS #46 in part (see the note at the top). The mapping
  // step covers what shows up in real host names: characters that are
  // ignored, full-width ASCII and dots, and lower-casing; a character
  // UTS #46 disallows among these ranges is a failure. Other labels are
  // Punycode-encoded as they are, which is right for text that is already
  // lower case and composed (NFC), the form people type.
  function mapDomainChar(c) {
    if (c === 0xAD || c === 0x34F || (c >= 0x180B && c <= 0x180F) || c === 0x200B || c === 0x2060 || c === 0xFEFF ||
        (c >= 0xFE00 && c <= 0xFE0F)) return '';
    if (c === 0x3002 || c === 0xFF0E || c === 0xFF61) return '.';
    if (c >= 0xFF01 && c <= 0xFF5E) return String.fromCharCode(c - 0xFEE0);
    // mathematical letters and digits (styled A-Z, a-z, 0-9)
    if (c >= 0x1D400 && c <= 0x1D6A3) { const i = (c - 0x1D400) % 52; return String.fromCharCode(i < 26 ? 0x41 + i : 0x61 + i - 26); }
    if (c >= 0x1D7CE && c <= 0x1D7FF) return String.fromCharCode(0x30 + (c - 0x1D7CE) % 10);
    if (c === 0xA0 || c === 0x3000 || (c >= 0x2000 && c <= 0x200A)) return ' ';
    if ((c >= 0x80 && c <= 0x9F) || (c >= 0xD800 && c <= 0xDFFF) || (c >= 0xE000 && c <= 0xF8FF) ||
        (c >= 0xFDD0 && c <= 0xFDEF) || (c & 0xFFFE) === 0xFFFE || c === 0xFFFD || c === 0x2028 || c === 0x2029 ||
        (c >= 0x200C && c <= 0x200F) || (c >= 0x202A && c <= 0x202E))
      return null;
    return fromCP(c);
  }

  function domainToASCII(domain) {
    let mapped = '';
    for (const ch of domain) {
      const cp = ch.codePointAt(0);
      if (cp < 0x80) { mapped += ch; continue; }
      const m = mapDomainChar(cp);
      if (m === null) return null;
      mapped += m;
    }
    const labels = mapped.toLowerCase().split('.');
    for (let i = 0; i < labels.length; i++) {
      const label = labels[i];
      if (label.startsWith('xn--')) {
        if (!punycodeValid(label.slice(4))) return null;
      } else if (/[^\x00-\x7f]/.test(label)) {
        labels[i] = 'xn--' + punycodeEncode(codePoints(label));
      }
    }
    const result = labels.join('.');
    if (result === '') return null;
    for (let i = 0; i < result.length; i++) if (forbiddenDomain(result.charCodeAt(i))) return null;
    return result;
  }

  // the serialized host, or null for failure
  function parseHost(input, isOpaque) {
    if (input[0] === '[') {
      if (input[input.length - 1] !== ']') return null;
      const a = parseIPv6(input.slice(1, -1));
      return a === null ? null : '[' + serializeIPv6(a) + ']';
    }
    if (isOpaque) {
      for (const ch of input) if (forbiddenHost(ch.codePointAt(0))) return null;
      return encodeString(input, inC0);
    }
    const domain = utf8Decode(percentDecode(utf8Encode(input)));
    const ascii = domainToASCII(domain);
    if (ascii === null) return null;
    if (endsInNumber(ascii)) return parseIPv4(ascii);
    return ascii;
  }

  // ------------------------------------------------------------ the parser

  const S_SCHEME_START = 1, S_SCHEME = 2, S_NO_SCHEME = 3, S_SPECIAL_RELATIVE_OR_AUTHORITY = 4,
        S_PATH_OR_AUTHORITY = 5, S_RELATIVE = 6, S_RELATIVE_SLASH = 7, S_SPECIAL_AUTHORITY_SLASHES = 8,
        S_SPECIAL_AUTHORITY_IGNORE_SLASHES = 9, S_AUTHORITY = 10, S_HOST = 11, S_HOSTNAME = 12, S_PORT = 13,
        S_FILE = 14, S_FILE_SLASH = 15, S_FILE_HOST = 16, S_PATH_START = 17, S_PATH = 18, S_OPAQUE_PATH = 19,
        S_QUERY = 20, S_FRAGMENT = 21;
  const FAILURE = null;

  function newRecord() {
    return { scheme: '', username: '', password: '', host: null, port: null, path: [], query: null, fragment: null };
  }

  const isSpecial = u => isSpecialScheme(u.scheme);
  const hasOpaquePath = u => typeof u.path === 'string';
  const includesCredentials = u => u.username !== '' || u.password !== '';
  const cannotHaveCredentialsOrPort = u => u.host === null || u.host === '' || u.scheme === 'file';

  const isDriveLetter = (s, normalized) =>
    s.length === 2 && isAlpha(s.charCodeAt(0)) && (s[1] === ':' || (!normalized && s[1] === '|'));
  // code points cps, from i: a Windows drive letter, then the end or / \ ? #
  function startsWithDrive(cps, i) {
    if (cps.length - i < 2 || !isAlpha(cps[i]) || (cps[i + 1] !== 0x3A && cps[i + 1] !== 0x7C)) return false;
    if (cps.length - i === 2) return true;
    const c = cps[i + 2];
    return c === 0x2F || c === 0x5C || c === 0x3F || c === 0x23;
  }

  function shortenPath(u) {
    const path = u.path;
    if (u.scheme === 'file' && path.length === 1 && isDriveLetter(path[0], true)) return;
    path.pop();
  }

  const isSingleDot = s => s === '.' || s.toLowerCase() === '%2e';
  const isDoubleDot = s => {
    s = s.toLowerCase();
    return s === '..' || s === '.%2e' || s === '%2e.' || s === '%2e%2e';
  };

  function clonePath(p) { return typeof p === 'string' ? p : p.slice(); }

  // The basic URL parser. Returns the record, or FAILURE. With a state
  // override (the setters), changes url in place; the result is then only
  // FAILURE or not.
  function basicParse(input, base, url, override) {
    if (!url) {
      url = newRecord();
      input = input.replace(/^[\u0000- ]+|[\u0000- ]+$/g, '');
    }
    input = input.replace(/[\t\n\r]/g, '');
    let state = override || S_SCHEME_START;
    let buffer = '', atSign = false, inBrackets = false, passwordToken = false;
    const cps = codePoints(input), len = cps.length;
    const rest = (p, cp) => p + 1 < len && cps[p + 1] === cp;

    for (let p = 0; ; p++) {
      const c = p < len ? cps[p] : EOF;
      const special = isSpecial(url);
      switch (state) {
        case S_SCHEME_START:
          if (isAlpha(c)) { buffer += String.fromCharCode(c | 0x20); state = S_SCHEME; }
          else if (!override) { state = S_NO_SCHEME; p--; }
          else return FAILURE;
          break;

        case S_SCHEME:
          if (isAlnum(c) || c === 0x2B || c === 0x2D || c === 0x2E) {
            buffer += String.fromCharCode(c >= 0x41 && c <= 0x5A ? c | 0x20 : c);
          } else if (c === 0x3A) {
            if (override) {
              if (isSpecialScheme(url.scheme) !== isSpecialScheme(buffer)) return url;
              if ((includesCredentials(url) || url.port !== null) && buffer === 'file') return url;
              if (url.scheme === 'file' && url.host === '') return url;
            }
            url.scheme = buffer;
            if (override) {
              if (url.port === SPECIAL[url.scheme]) url.port = null;
              return url;
            }
            buffer = '';
            if (url.scheme === 'file') state = S_FILE;
            else if (isSpecial(url) && base && base.scheme === url.scheme) state = S_SPECIAL_RELATIVE_OR_AUTHORITY;
            else if (isSpecial(url)) state = S_SPECIAL_AUTHORITY_SLASHES;
            else if (rest(p, 0x2F)) { state = S_PATH_OR_AUTHORITY; p++; }
            else { url.path = ''; state = S_OPAQUE_PATH; }
          } else if (!override) {
            buffer = '';
            state = S_NO_SCHEME;
            p = -1; // start over
          } else return FAILURE;
          break;

        case S_NO_SCHEME:
          if (!base || (hasOpaquePath(base) && c !== 0x23)) return FAILURE;
          if (hasOpaquePath(base) && c === 0x23) {
            url.scheme = base.scheme;
            url.path = clonePath(base.path);
            url.query = base.query;
            url.fragment = '';
            state = S_FRAGMENT;
          } else if (base.scheme !== 'file') { state = S_RELATIVE; p--; }
          else { state = S_FILE; p--; }
          break;

        case S_SPECIAL_RELATIVE_OR_AUTHORITY:
          if (c === 0x2F && rest(p, 0x2F)) { state = S_SPECIAL_AUTHORITY_IGNORE_SLASHES; p++; }
          else { state = S_RELATIVE; p--; }
          break;

        case S_PATH_OR_AUTHORITY:
          if (c === 0x2F) state = S_AUTHORITY;
          else { state = S_PATH; p--; }
          break;

        case S_RELATIVE:
          url.scheme = base.scheme;
          if (c === 0x2F) state = S_RELATIVE_SLASH;
          else if (isSpecial(url) && c === 0x5C) state = S_RELATIVE_SLASH;
          else {
            url.username = base.username;
            url.password = base.password;
            url.host = base.host;
            url.port = base.port;
            url.path = clonePath(base.path);
            url.query = base.query;
            if (c === 0x3F) { url.query = ''; state = S_QUERY; }
            else if (c === 0x23) { url.fragment = ''; state = S_FRAGMENT; }
            else if (c !== EOF) {
              url.query = null;
              shortenPath(url);
              state = S_PATH;
              p--;
            }
          }
          break;

        case S_RELATIVE_SLASH:
          if (special && (c === 0x2F || c === 0x5C)) state = S_SPECIAL_AUTHORITY_IGNORE_SLASHES;
          else if (c === 0x2F) state = S_AUTHORITY;
          else {
            url.username = base.username;
            url.password = base.password;
            url.host = base.host;
            url.port = base.port;
            state = S_PATH;
            p--;
          }
          break;

        case S_SPECIAL_AUTHORITY_SLASHES:
          if (c === 0x2F && rest(p, 0x2F)) { state = S_SPECIAL_AUTHORITY_IGNORE_SLASHES; p++; }
          else { state = S_SPECIAL_AUTHORITY_IGNORE_SLASHES; p--; }
          break;

        case S_SPECIAL_AUTHORITY_IGNORE_SLASHES:
          if (c !== 0x2F && c !== 0x5C) { state = S_AUTHORITY; p--; }
          break;

        case S_AUTHORITY:
          if (c === 0x40) {
            if (atSign) buffer = '%40' + buffer;
            atSign = true;
            for (const ch of buffer) {
              const cp = ch.codePointAt(0);
              if (cp === 0x3A && !passwordToken) { passwordToken = true; continue; }
              const enc = encodeCP(cp, inUserinfo);
              if (passwordToken) url.password += enc;
              else url.username += enc;
            }
            buffer = '';
          } else if (c === EOF || c === 0x2F || c === 0x3F || c === 0x23 || (special && c === 0x5C)) {
            if (atSign && buffer === '') return FAILURE;
            p -= codePoints(buffer).length + 1;
            buffer = '';
            state = S_HOST;
          } else buffer += fromCP(c);
          break;

        case S_HOST:
        case S_HOSTNAME:
          if (override && url.scheme === 'file') { p--; state = S_FILE_HOST; }
          else if (c === 0x3A && !inBrackets) {
            if (buffer === '') return FAILURE;
            if (override === S_HOSTNAME) return FAILURE;
            const host = parseHost(buffer, !special);
            if (host === null) return FAILURE;
            url.host = host;
            buffer = '';
            state = S_PORT;
          } else if (c === EOF || c === 0x2F || c === 0x3F || c === 0x23 || (special && c === 0x5C)) {
            p--;
            if (special && buffer === '') return FAILURE;
            if (override && buffer === '' && (includesCredentials(url) || url.port !== null)) return FAILURE;
            const host = parseHost(buffer, !special);
            if (host === null) return FAILURE;
            url.host = host;
            buffer = '';
            state = S_PATH_START;
            if (override) return url;
          } else {
            if (c === 0x5B) inBrackets = true;
            if (c === 0x5D) inBrackets = false;
            buffer += fromCP(c);
          }
          break;

        case S_PORT:
          if (isDigit(c)) buffer += String.fromCharCode(c);
          else if (c === EOF || c === 0x2F || c === 0x3F || c === 0x23 || (special && c === 0x5C) || override) {
            if (buffer !== '') {
              const port = Number(buffer);
              if (port > 65535) return FAILURE;
              url.port = port === SPECIAL[url.scheme] ? null : port;
              buffer = '';
              if (override) return url;
            }
            if (override) return FAILURE;
            state = S_PATH_START;
            p--;
          } else return FAILURE;
          break;

        case S_FILE:
          url.scheme = 'file';
          url.host = '';
          if (c === 0x2F || c === 0x5C) state = S_FILE_SLASH;
          else if (base && base.scheme === 'file') {
            url.host = base.host;
            url.path = clonePath(base.path);
            url.query = base.query;
            if (c === 0x3F) { url.query = ''; state = S_QUERY; }
            else if (c === 0x23) { url.fragment = ''; state = S_FRAGMENT; }
            else if (c !== EOF) {
              url.query = null;
              if (!startsWithDrive(cps, p)) shortenPath(url);
              else url.path = [];
              state = S_PATH;
              p--;
            }
          } else { state = S_PATH; p--; }
          break;

        case S_FILE_SLASH:
          if (c === 0x2F || c === 0x5C) state = S_FILE_HOST;
          else {
            if (base && base.scheme === 'file') {
              url.host = base.host;
              if (!startsWithDrive(cps, p) && base.path.length > 0 && isDriveLetter(base.path[0], true))
                url.path.push(base.path[0]);
            }
            state = S_PATH;
            p--;
          }
          break;

        case S_FILE_HOST:
          if (c === EOF || c === 0x2F || c === 0x5C || c === 0x3F || c === 0x23) {
            p--;
            if (!override && isDriveLetter(buffer, false)) state = S_PATH; // buffer stays: the path's first segment
            else if (buffer === '') {
              url.host = '';
              if (override) return url;
              state = S_PATH_START;
            } else {
              let host = parseHost(buffer, !special);
              if (host === null) return FAILURE;
              if (host === 'localhost') host = '';
              url.host = host;
              if (override) return url;
              buffer = '';
              state = S_PATH_START;
            }
          } else buffer += fromCP(c);
          break;

        case S_PATH_START:
          if (special) {
            state = S_PATH;
            if (c !== 0x2F && c !== 0x5C) p--;
          } else if (!override && c === 0x3F) { url.query = ''; state = S_QUERY; }
          else if (!override && c === 0x23) { url.fragment = ''; state = S_FRAGMENT; }
          else if (c !== EOF) {
            state = S_PATH;
            if (c !== 0x2F) p--;
          } else if (override && url.host === null) url.path.push('');
          break;

        case S_PATH:
          if (c === EOF || c === 0x2F || (special && c === 0x5C) || (!override && (c === 0x3F || c === 0x23))) {
            const slash = c === 0x2F || (special && c === 0x5C);
            if (isDoubleDot(buffer)) {
              shortenPath(url);
              if (!slash) url.path.push('');
            } else if (isSingleDot(buffer) && !slash) url.path.push('');
            else if (!isSingleDot(buffer)) {
              if (url.scheme === 'file' && url.path.length === 0 && isDriveLetter(buffer, false))
                buffer = buffer[0] + ':';
              url.path.push(buffer);
            }
            buffer = '';
            if (c === 0x3F) { url.query = ''; state = S_QUERY; }
            if (c === 0x23) { url.fragment = ''; state = S_FRAGMENT; }
          } else buffer += encodeCP(c, inPath);
          break;

        case S_OPAQUE_PATH:
          if (c === 0x3F) { url.query = ''; state = S_QUERY; }
          else if (c === 0x23) { url.fragment = ''; state = S_FRAGMENT; }
          else if (c === 0x20) {
            const next = p + 1 < len ? cps[p + 1] : EOF;
            url.path += next === 0x3F || next === 0x23 ? '%20' : ' ';
          } else if (c !== EOF) url.path += encodeCP(c, inC0);
          break;

        case S_QUERY:
          if ((!override && c === 0x23) || c === EOF) {
            url.query += encodeString(buffer, special ? inSpecialQuery : inQuery);
            buffer = '';
            if (c === 0x23) { url.fragment = ''; state = S_FRAGMENT; }
          } else if (c !== EOF) buffer += fromCP(c);
          break;

        case S_FRAGMENT:
          if (c !== EOF) url.fragment += encodeCP(c, inFragment);
          break;
      }
      if (p >= len) break;
    }
    return url;
  }

  function serializePath(u) {
    if (hasOpaquePath(u)) return u.path;
    let out = '';
    for (const seg of u.path) out += '/' + seg;
    return out;
  }

  function serialize(u, excludeFragment) {
    let out = u.scheme + ':';
    if (u.host !== null) {
      out += '//';
      if (includesCredentials(u)) {
        out += u.username;
        if (u.password !== '') out += ':' + u.password;
        out += '@';
      }
      out += u.host;
      if (u.port !== null) out += ':' + u.port;
    }
    if (u.host === null && !hasOpaquePath(u) && u.path.length > 1 && u.path[0] === '') out += '/.';
    out += serializePath(u);
    if (u.query !== null) out += '?' + u.query;
    if (!excludeFragment && u.fragment !== null) out += '#' + u.fragment;
    return out;
  }

  function origin(u) {
    switch (u.scheme) {
      case 'blob': {
        const inner = parse(serializePath(u));
        if (inner && (inner.scheme === 'http' || inner.scheme === 'https')) return origin(inner);
        return 'null';
      }
      case 'ftp': case 'http': case 'https': case 'ws': case 'wss':
        return u.scheme + '://' + u.host + (u.port !== null ? ':' + u.port : '');
      default:
        return 'null';
    }
  }

  function parse(input, baseString) {
    let base = null;
    if (baseString !== undefined) {
      base = basicParse(baseString, null);
      if (base === FAILURE) return FAILURE;
    }
    return basicParse(input, base);
  }

  // ------------------------------------------------------------ application/x-www-form-urlencoded

  function formParse(s) {
    const out = [];
    for (const seq of s.split('&')) {
      if (seq === '') continue;
      const eq = seq.indexOf('=');
      let name = eq < 0 ? seq : seq.slice(0, eq), value = eq < 0 ? '' : seq.slice(eq + 1);
      name = utf8Decode(percentDecode(utf8Encode(name.replace(/\+/g, ' '))));
      value = utf8Decode(percentDecode(utf8Encode(value.replace(/\+/g, ' '))));
      out.push([name, value]);
    }
    return out;
  }

  function formEncode(s) {
    let out = '';
    for (const b of utf8Encode(s)) {
      if (b === 0x20) out += '+';
      else if (inForm(b)) out += pctByte(b);
      else out += String.fromCharCode(b);
    }
    return out;
  }

  function formSerialize(list) {
    let out = '';
    for (let i = 0; i < list.length; i++) {
      if (i) out += '&';
      out += formEncode(list[i][0]) + '=' + formEncode(list[i][1]);
    }
    return out;
  }

  // ------------------------------------------------------------ URLSearchParams

  const need = (n, got, what) => {
    if (got < n) throw new TypeError(what + ': ' + n + ' argument(s) required, but only ' + got + ' present');
  };

  let linkParams; // (params, url) -> void, set below (the classes share private state through it)
  let setParamsList; // (params, list) -> void

  class URLSearchParams {
    #list = [];
    #url = null; // the URL object this belongs to, if any

    constructor(init = '') {
      if (init !== null && (typeof init === 'object' || typeof init === 'function')) {
        const iter = init[Symbol.iterator];
        if (iter !== undefined && iter !== null) {
          if (typeof iter !== 'function') throw new TypeError('URLSearchParams: the init object is not iterable');
          for (const pair of init) {
            if (pair === null || (typeof pair !== 'object' && typeof pair !== 'function'))
              throw new TypeError('URLSearchParams: each pair must be a sequence of two strings');
            const a = [...pair];
            if (a.length !== 2) throw new TypeError('URLSearchParams: each pair must hold exactly two items');
            this.#list.push([usv(a[0]), usv(a[1])]);
          }
        } else {
          for (const key of Reflect.ownKeys(init)) {
            const d = Reflect.getOwnPropertyDescriptor(init, key);
            if (d === undefined || !d.enumerable) continue;
            if (typeof key === 'symbol') continue;
            this.#list.push([usv(key), usv(init[key])]);
          }
        }
      } else {
        let s = usv(init);
        if (s[0] === '?') s = s.slice(1);
        this.#list = formParse(s);
      }
    }

    static {
      linkParams = (params, url) => { params.#url = url; };
      setParamsList = (params, list) => { params.#list = list; };
    }

    #update() {
      if (this.#url === null) return;
      const s = formSerialize(this.#list);
      urlSetQuery(this.#url, s === '' ? null : s);
    }

    get size() { return this.#list.length; }

    append(name, value) {
      need(2, arguments.length, 'URLSearchParams.append');
      this.#list.push([usv(name), usv(value)]);
      this.#update();
    }

    delete(name, value) {
      need(1, arguments.length, 'URLSearchParams.delete');
      name = usv(name);
      const v = value === undefined ? undefined : usv(value);
      this.#list = this.#list.filter(([n, x]) => !(n === name && (v === undefined || x === v)));
      this.#update();
    }

    get(name) {
      need(1, arguments.length, 'URLSearchParams.get');
      name = usv(name);
      for (const [n, v] of this.#list) if (n === name) return v;
      return null;
    }

    getAll(name) {
      need(1, arguments.length, 'URLSearchParams.getAll');
      name = usv(name);
      const out = [];
      for (const [n, v] of this.#list) if (n === name) out.push(v);
      return out;
    }

    has(name, value) {
      need(1, arguments.length, 'URLSearchParams.has');
      name = usv(name);
      const v = value === undefined ? undefined : usv(value);
      for (const [n, x] of this.#list) if (n === name && (v === undefined || x === v)) return true;
      return false;
    }

    set(name, value) {
      need(2, arguments.length, 'URLSearchParams.set');
      name = usv(name);
      value = usv(value);
      const i = this.#list.findIndex(([n]) => n === name);
      if (i < 0) this.#list.push([name, value]);
      else {
        this.#list[i][1] = value;
        this.#list = this.#list.filter(([n], j) => j <= i || n !== name);
      }
      this.#update();
    }

    sort() {
      // stable, by the names' UTF-16 code units
      this.#list = this.#list
        .map((p, i) => [p, i])
        .sort((a, b) => (a[0][0] < b[0][0] ? -1 : a[0][0] > b[0][0] ? 1 : a[1] - b[1]))
        .map(x => x[0]);
      this.#update();
    }

    toString() { return formSerialize(this.#list); }

    forEach(fn, thisArg) {
      need(1, arguments.length, 'URLSearchParams.forEach');
      if (typeof fn !== 'function') throw new TypeError('URLSearchParams.forEach: the callback is not a function');
      for (let i = 0; i < this.#list.length; i++) fn.call(thisArg, this.#list[i][1], this.#list[i][0], this);
    }

    // live iterators: they read the list as it is at each step
    *entries() { for (let i = 0; i < this.#list.length; i++) yield [this.#list[i][0], this.#list[i][1]]; }
    *keys() { for (let i = 0; i < this.#list.length; i++) yield this.#list[i][0]; }
    *values() { for (let i = 0; i < this.#list.length; i++) yield this.#list[i][1]; }
  }
  Object.defineProperty(URLSearchParams.prototype, Symbol.iterator, {
    value: URLSearchParams.prototype.entries, writable: true, configurable: true,
  });
  Object.defineProperty(URLSearchParams.prototype, Symbol.toStringTag, { value: 'URLSearchParams', configurable: true });

  // ------------------------------------------------------------ URL

  let urlSetQuery; // (url, query or null) -> void, for URLSearchParams' updates

  class URL {
    #u;
    #params;

    constructor(url, base) {
      need(1, arguments.length, 'URL');
      const r = parse(usv(url), base === undefined ? undefined : usv(base));
      if (r === FAILURE) throw new TypeError('Invalid URL: ' + usv(url));
      this.#u = r;
      this.#params = new URLSearchParams(r.query === null ? '' : r.query);
      linkParams(this.#params, this);
    }

    static {
      urlSetQuery = (url, q) => { url.#u.query = q; };
    }

    static parse(url, base) {
      need(1, arguments.length, 'URL.parse');
      try { return new URL(url, base); } catch (e) { return null; }
    }

    static canParse(url, base) {
      need(1, arguments.length, 'URL.canParse');
      return parse(usv(url), base === undefined ? undefined : usv(base)) !== FAILURE;
    }

    get href() { return serialize(this.#u); }
    set href(v) {
      const r = parse(usv(v));
      if (r === FAILURE) throw new TypeError('Invalid URL: ' + usv(v));
      this.#u = r;
      setParamsList(this.#params, r.query === null ? [] : formParse(r.query));
    }

    get origin() { return origin(this.#u); }

    get protocol() { return this.#u.scheme + ':'; }
    set protocol(v) { basicParse(usv(v) + ':', null, this.#u, S_SCHEME_START); }

    get username() { return this.#u.username; }
    set username(v) {
      if (cannotHaveCredentialsOrPort(this.#u)) return;
      this.#u.username = encodeString(usv(v), inUserinfo);
    }

    get password() { return this.#u.password; }
    set password(v) {
      if (cannotHaveCredentialsOrPort(this.#u)) return;
      this.#u.password = encodeString(usv(v), inUserinfo);
    }

    get host() {
      const u = this.#u;
      if (u.host === null) return '';
      return u.port === null ? u.host : u.host + ':' + u.port;
    }
    set host(v) {
      if (hasOpaquePath(this.#u)) return;
      basicParse(usv(v), null, this.#u, S_HOST);
    }

    get hostname() { return this.#u.host === null ? '' : this.#u.host; }
    set hostname(v) {
      if (hasOpaquePath(this.#u)) return;
      basicParse(usv(v), null, this.#u, S_HOSTNAME);
    }

    get port() { return this.#u.port === null ? '' : String(this.#u.port); }
    set port(v) {
      if (cannotHaveCredentialsOrPort(this.#u)) return;
      v = usv(v);
      if (v === '') this.#u.port = null;
      else basicParse(v, null, this.#u, S_PORT);
    }

    get pathname() { return serializePath(this.#u); }
    set pathname(v) {
      if (hasOpaquePath(this.#u)) return;
      this.#u.path = [];
      basicParse(usv(v), null, this.#u, S_PATH_START);
    }

    get search() {
      const q = this.#u.query;
      return q === null || q === '' ? '' : '?' + q;
    }
    set search(v) {
      v = usv(v);
      const u = this.#u;
      if (v === '') {
        u.query = null;
        setParamsList(this.#params, []);
        return;
      }
      const input = v[0] === '?' ? v.slice(1) : v;
      u.query = '';
      basicParse(input, null, u, S_QUERY);
      setParamsList(this.#params, formParse(input));
    }

    get searchParams() { return this.#params; }

    get hash() {
      const f = this.#u.fragment;
      return f === null || f === '' ? '' : '#' + f;
    }
    set hash(v) {
      v = usv(v);
      const u = this.#u;
      if (v === '') {
        u.fragment = null;
        return;
      }
      u.fragment = '';
      basicParse(v[0] === '#' ? v.slice(1) : v, null, u, S_FRAGMENT);
    }

    toString() { return serialize(this.#u); }
    toJSON() { return serialize(this.#u); }
  }
  Object.defineProperty(URL.prototype, Symbol.toStringTag, { value: 'URL', configurable: true });

  return { URL, URLSearchParams };
})
