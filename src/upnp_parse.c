/*
 * Copyright (C) 2026 James Pearce
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <arpa/inet.h>

#include "upnp_parse.h"

#define RC_SERVICE_PREFIX "urn:schemas-upnp-org:service:RenderingControl:"

// A level sent this far from either end of the scale may be clamped by the
// renderer, so its answer says nothing about the offset
#define LEARN_LOW   2
#define LEARN_HIGH  98
// How far a renderer's answer may be from the level sent and still be taken
// as that level on a scale with an offset. A knob step is larger.
#define LEARN_SPAN  2


/* ------------------------------- Helpers ---------------------------------- */

// Bounded search for needle in [buf + from, buf + len); the offset or -1
static long
find(const char *buf, size_t len, size_t from, const char *needle)
{
  size_t nlen = strlen(needle);
  size_t i;

  if (nlen == 0 || from > len || len - from < nlen)
    return -1;

  for (i = from; i + nlen <= len; i++)
    {
      if (buf[i] == needle[0] && memcmp(buf + i, needle, nlen) == 0)
	return (long)i;
    }

  return -1;
}

// Copies [s, s + n) without the whitespace around it. Returns -1 if it does
// not fit, unless truncate is set.
static int
copy_trimmed(char *dst, size_t dstlen, const char *s, size_t n, bool truncate)
{
  while (n > 0 && isspace((unsigned char)s[0]))
    {
      s++;
      n--;
    }
  while (n > 0 && isspace((unsigned char)s[n - 1]))
    n--;

  if (n >= dstlen)
    {
      if (!truncate || dstlen == 0)
	return -1;
      n = dstlen - 1;
    }

  memcpy(dst, s, n);
  dst[n] = '\0';
  return 0;
}

static bool
prefix_nocase(const char *s, size_t n, const char *prefix)
{
  size_t plen = strlen(prefix);

  return n >= plen && strncasecmp(s, prefix, plen) == 0;
}

static bool
contains_nocase(const char *s, size_t n, const char *needle)
{
  size_t nlen = strlen(needle);
  size_t i;

  for (i = 0; i + nlen <= n; i++)
    {
      if (strncasecmp(s + i, needle, nlen) == 0)
	return true;
    }

  return false;
}

static bool
is_dotted_quad(const char *s)
{
  struct in_addr a;

  return inet_pton(AF_INET, s, &a) == 1;
}


/* ------------------------------ HTTP heads -------------------------------- */

static int
start_line_parse(const char *s, size_t n, struct upnp_http_head *out)
{
  const char *sp1;
  const char *sp2;
  size_t i;

  if (prefix_nocase(s, n, "HTTP/1.0 ") || prefix_nocase(s, n, "HTTP/1.1 "))
    {
      if (n < 12 || !isdigit((unsigned char)s[9]) || !isdigit((unsigned char)s[10]) || !isdigit((unsigned char)s[11]))
	return -1;
      if (n > 12 && s[12] != ' ')
	return -1;

      out->status = (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
      return (out->status >= 100) ? 0 : -1;
    }

  sp1 = memchr(s, ' ', n);
  if (!sp1 || sp1 == s || (size_t)(sp1 - s) >= sizeof(out->method))
    return -1;
  sp2 = memchr(sp1 + 1, ' ', n - (sp1 + 1 - s));
  if (!sp2 || sp2 == sp1 + 1 || (size_t)(sp2 - sp1 - 1) >= sizeof(out->target))
    return -1;

  for (i = 0; s + i < sp1; i++)
    {
      if (!isupper((unsigned char)s[i]) && s[i] != '-')
	return -1;
    }

  n -= sp2 + 1 - s;
  if (n != 8 || (strncmp(sp2 + 1, "HTTP/1.0", 8) != 0 && strncmp(sp2 + 1, "HTTP/1.1", 8) != 0))
    return -1;

  memcpy(out->method, s, sp1 - s);
  out->method[sp1 - s] = '\0';
  memcpy(out->target, sp1 + 1, sp2 - sp1 - 1);
  out->target[sp2 - sp1 - 1] = '\0';
  return 0;
}

static long
timeout_parse(const char *v, size_t n)
{
  long secs = 0;
  size_t i;

  if (n == 8 && strncasecmp(v, "infinite", 8) == 0)
    return 0;
  if (!prefix_nocase(v, n, "Second-"))
    return -1;
  if (n == 15 && strncasecmp(v + 7, "infinite", 8) == 0)
    return 0;
  if (n == 7 || n > 7 + 9)
    return -1;

  for (i = 7; i < n; i++)
    {
      if (!isdigit((unsigned char)v[i]))
	return -1;
      secs = secs * 10 + (v[i] - '0');
    }

  // Second-0 would mean a subscription that has already lapsed
  return (secs > 0) ? secs : -1;
}

static int
header_parse(const char *s, size_t n, struct upnp_http_head *out)
{
  const char *colon;
  const char *v;
  size_t nlen;
  size_t vlen;
  size_t i;

  colon = memchr(s, ':', n);
  if (!colon || colon == s)
    return -1;

  nlen = colon - s;
  while (nlen > 0 && isspace((unsigned char)s[nlen - 1]))
    nlen--;
  if (nlen == 0 || isspace((unsigned char)s[0]))
    return -1;

  v = colon + 1;
  vlen = n - (v - s);
  while (vlen > 0 && isspace((unsigned char)v[0]))
    {
      v++;
      vlen--;
    }
  while (vlen > 0 && isspace((unsigned char)v[vlen - 1]))
    vlen--;

#define NAME_IS(x) (nlen == sizeof(x) - 1 && strncasecmp(s, x, nlen) == 0)
  if (NAME_IS("SID"))
    return copy_trimmed(out->sid, sizeof(out->sid), v, vlen, false);
  if (NAME_IS("NT"))
    return copy_trimmed(out->nt, sizeof(out->nt), v, vlen, false);
  if (NAME_IS("NTS"))
    return copy_trimmed(out->nts, sizeof(out->nts), v, vlen, false);
  if (NAME_IS("LOCATION"))
    return copy_trimmed(out->location, sizeof(out->location), v, vlen, false);
  if (NAME_IS("ST"))
    return copy_trimmed(out->st, sizeof(out->st), v, vlen, true);
  if (NAME_IS("TIMEOUT"))
    {
      out->timeout_s = timeout_parse(v, vlen);
      return 0;
    }
  if (NAME_IS("CONTENT-LENGTH"))
    {
      long cl = 0;

      if (vlen == 0 || vlen > 9)
	return -1;
      for (i = 0; i < vlen; i++)
	{
	  if (!isdigit((unsigned char)v[i]))
	    return -1;
	  cl = cl * 10 + (v[i] - '0');
	}
      out->content_length = cl;
      return 0;
    }
  if (NAME_IS("TRANSFER-ENCODING"))
    {
      out->chunked = contains_nocase(v, vlen, "chunked");
      return 0;
    }
#undef NAME_IS

  return 0;
}

int
upnp_http_head_parse(const char *buf, size_t len, size_t max, struct upnp_http_head *out)
{
  size_t limit = (len < max) ? len : max;
  size_t pos = 0;
  int lines = 0;
  const char *nl;
  size_t n;

  memset(out, 0, sizeof(*out));
  out->timeout_s = -1;
  out->content_length = -1;

  for (;;)
    {
      nl = memchr(buf + pos, '\n', limit - pos);
      if (!nl)
	{
	  if (limit - pos > UPNP_HEAD_LINE_MAX || len >= max)
	    return -1;
	  return 0;
	}

      n = nl - (buf + pos);
      if (n > 0 && buf[pos + n - 1] == '\r')
	n--;
      if (n > UPNP_HEAD_LINE_MAX)
	return -1;

      if (n == 0)
	{
	  if (lines == 0)
	    return -1;
	  out->head_len = nl + 1 - buf;
	  return 1;
	}

      if (lines == 0)
	{
	  if (start_line_parse(buf + pos, n, out) < 0)
	    return -1;
	}
      else if (lines > UPNP_HEAD_LINES_MAX || header_parse(buf + pos, n, out) < 0)
	return -1;

      lines++;
      pos = nl + 1 - buf;
    }
}


/* --------------------------------- URLs ----------------------------------- */

int
upnp_url_split(const char *url, char *host, size_t hostlen, unsigned short *port, char *path, size_t pathlen)
{
  const char *h;
  const char *end;
  const char *colon;
  const char *slash;
  long p = 80;
  char *ep;

  if (strncasecmp(url, "http://", 7) != 0)
    return -1;

  h = url + 7;
  slash = strchr(h, '/');
  end = slash ? slash : h + strlen(h);
  colon = memchr(h, ':', end - h);

  if (colon)
    {
      if (colon + 1 == end)
	return -1;
      p = strtol(colon + 1, &ep, 10);
      if (ep != end || p <= 0 || p > 65535)
	return -1;
    }
  else
    colon = end;

  if (colon == h || (size_t)(colon - h) >= hostlen)
    return -1;
  memcpy(host, h, colon - h);
  host[colon - h] = '\0';
  if (!is_dotted_quad(host))
    return -1;

  if (!slash)
    slash = "/";
  if (strlen(slash) >= pathlen)
    return -1;
  strcpy(path, slash);

  *port = (unsigned short)p;
  return 0;
}


/* ------------------------------ Description ------------------------------- */

// The text of the first <tag> in [from, to), trimmed. 0 found, -1 absent or
// too long for out (unless truncate).
static int
tag_text(const char *xml, size_t to, size_t from, const char *tag, char *out, size_t outlen, bool truncate)
{
  char open[64];
  char close[64];
  long a;
  long b;

  snprintf(open, sizeof(open), "<%s>", tag);
  snprintf(close, sizeof(close), "</%s>", tag);

  a = find(xml, to, from, open);
  if (a < 0)
    return -1;
  a += strlen(open);
  b = find(xml, to, a, close);
  if (b < 0)
    return -1;

  return copy_trimmed(out, outlen, xml + a, b - a, truncate);
}

static int
url_resolve(const char *rel, const char *base_host, unsigned short base_port, const char *base_path, char *out, size_t outlen)
{
  const char *dir_end;
  int n;

  if (strncasecmp(rel, "http://", 7) == 0)
    n = snprintf(out, outlen, "%s", rel);
  else if (rel[0] == '/')
    n = snprintf(out, outlen, "http://%s:%u%s", base_host, base_port, rel);
  else
    {
      // Relative to the base's directory
      dir_end = strrchr(base_path, '/');
      n = snprintf(out, outlen, "http://%s:%u%.*s/%s", base_host, base_port,
		   dir_end ? (int)(dir_end - base_path) : 0, base_path, rel);
    }

  return (n > 0 && (size_t)n < outlen) ? 0 : -1;
}

int
upnp_description_parse(const char *xml, size_t len, const char *location, struct upnp_rc_service *out)
{
  char loc_host[16];
  char base_host[16];
  char host[16];
  char base[256];
  char path[256];
  char base_path[256];
  char type[128];
  char event_rel[256];
  char control_rel[256];
  unsigned short loc_port;
  unsigned short base_port;
  unsigned short port;
  size_t pos = 0;
  long a;
  long b;
  bool found = false;

  memset(out, 0, sizeof(*out));

  if (upnp_url_split(location, loc_host, sizeof(loc_host), &loc_port, path, sizeof(path)) < 0)
    return -2;
  if (find(xml, len, 0, "<root") < 0)
    return -2;

  if (tag_text(xml, len, 0, "URLBase", base, sizeof(base), false) == 0 && base[0])
    {
      if (upnp_url_split(base, base_host, sizeof(base_host), &base_port, base_path, sizeof(base_path)) < 0)
	return -2;
    }
  else
    {
      snprintf(base_host, sizeof(base_host), "%s", loc_host);
      base_port = loc_port;
      snprintf(base_path, sizeof(base_path), "%s", path);
    }

  tag_text(xml, len, 0, "friendlyName", out->friendly_name, sizeof(out->friendly_name), true);
  tag_text(xml, len, 0, "manufacturer", out->manufacturer, sizeof(out->manufacturer), true);
  tag_text(xml, len, 0, "modelName", out->model, sizeof(out->model), true);

  for (;;)
    {
      a = find(xml, len, pos, "<service>");
      if (a < 0)
	break;
      b = find(xml, len, a, "</service>");
      if (b < 0)
	return -2;
      pos = b + strlen("</service>");

      if (tag_text(xml, b, a, "serviceType", type, sizeof(type), true) < 0)
	continue;
      // The exact prefix, so GroupRenderingControl is not taken for it
      if (strncmp(type, RC_SERVICE_PREFIX, strlen(RC_SERVICE_PREFIX)) != 0)
	continue;

      if (tag_text(xml, b, a, "eventSubURL", event_rel, sizeof(event_rel), false) < 0 || !event_rel[0])
	return -2;
      if (tag_text(xml, b, a, "controlURL", control_rel, sizeof(control_rel), false) < 0)
	control_rel[0] = '\0';
      found = true;
      break;
    }

  if (!found)
    return -1;

  if (url_resolve(event_rel, base_host, base_port, base_path, out->event_url, sizeof(out->event_url)) < 0)
    return -2;
  if (control_rel[0] && url_resolve(control_rel, base_host, base_port, base_path, out->control_url, sizeof(out->control_url)) < 0)
    return -2;

  // Events are only taken from the renderer itself
  if (upnp_url_split(out->event_url, host, sizeof(host), &port, path, sizeof(path)) < 0)
    return -2;
  if (strcmp(host, loc_host) != 0)
    return -2;

  return 0;
}


/* -------------------------------- Events ---------------------------------- */

// Unescapes the five XML entities in place; returns the new length
static size_t
xml_unescape(char *s, size_t n)
{
  static const struct { const char *ent; char c; } ents[] = {
    { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' }, { "&amp;", '&' },
  };
  size_t r = 0;
  size_t w = 0;
  size_t i;
  size_t elen;

  while (r < n)
    {
      if (s[r] == '&')
	{
	  for (i = 0; i < sizeof(ents) / sizeof(ents[0]); i++)
	    {
	      elen = strlen(ents[i].ent);
	      if (n - r >= elen && memcmp(s + r, ents[i].ent, elen) == 0)
		break;
	    }
	  if (i < sizeof(ents) / sizeof(ents[0]))
	    {
	      s[w++] = ents[i].c;
	      r += elen;
	      continue;
	    }
	}
      s[w++] = s[r++];
    }

  return w;
}

// The value of attribute name in the element text [s, s + n); 0 found
static int
attr_get(const char *s, size_t n, const char *name, char *out, size_t outlen)
{
  size_t nlen = strlen(name);
  size_t i;
  size_t j;
  char q;

  for (i = 1; i + nlen + 2 < n; i++)
    {
      if (!isspace((unsigned char)s[i - 1]) || memcmp(s + i, name, nlen) != 0 || s[i + nlen] != '=')
	continue;

      q = s[i + nlen + 1];
      if (q != '"' && q != '\'')
	return -1;
      for (j = i + nlen + 2; j < n && s[j] != q; j++)
	;
      if (j >= n)
	return -1;
      return copy_trimmed(out, outlen, s + i + nlen + 2, j - (i + nlen + 2), false);
    }

  return -1;
}

static bool
element_is(const char *s, size_t n, const char *name)
{
  size_t nlen = strlen(name);

  return n > nlen && memcmp(s, name, nlen) == 0 && (isspace((unsigned char)s[nlen]) || s[nlen] == '/' || s[nlen] == '>');
}

static void
instance_read(const char *s, size_t n, struct upnp_rc_state *out)
{
  char channel[32];
  char val[16];
  const char *lt;
  const char *gt;
  size_t pos = 0;
  long v;
  char *ep;

  for (;;)
    {
      lt = memchr(s + pos, '<', n - pos);
      if (!lt)
	return;
      gt = memchr(lt, '>', n - (lt - s));
      if (!gt)
	return;
      pos = gt + 1 - s;

      lt++;
      if (!element_is(lt, gt - lt, "Volume") && !element_is(lt, gt - lt, "Mute"))
	continue;

      // A renderer that has only the one channel may leave it out
      if (attr_get(lt, gt - lt, "channel", channel, sizeof(channel)) == 0 && strcmp(channel, "Master") != 0)
	continue;
      if (attr_get(lt, gt - lt, "val", val, sizeof(val)) < 0)
	continue;

      if (lt[0] == 'V')
	{
	  v = strtol(val, &ep, 10);
	  if (ep == val || *ep != '\0' || v < 0 || v > 65535)
	    continue;
	  out->has_volume = true;
	  out->volume = (int)v;
	}
      else
	{
	  if (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0)
	    out->mute = true;
	  else if (strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0)
	    out->mute = false;
	  else
	    continue;
	  out->has_mute = true;
	}
    }
}

int
upnp_notify_parse(const char *body, size_t len, struct upnp_rc_state *out)
{
  char val[16];
  char *s;
  size_t n;
  size_t pos = 0;
  long a;
  long b;
  long gt;
  long end;

  memset(out, 0, sizeof(*out));

  a = find(body, len, 0, "<LastChange>");
  if (a < 0)
    return -1;
  a += strlen("<LastChange>");
  b = find(body, len, a, "</LastChange>");
  if (b < 0)
    return -1;

  n = b - a;
  s = malloc(n + 1);
  if (!s)
    return -1;
  memcpy(s, body + a, n);

  if (n >= 12 && memcmp(s, "<![CDATA[", 9) == 0 && memcmp(s + n - 3, "]]>", 3) == 0)
    {
      memmove(s, s + 9, n - 12);
      n -= 12;
    }

  n = xml_unescape(s, n);
  if (find(s, n, 0, "&lt;") >= 0)
    n = xml_unescape(s, n);
  s[n] = '\0';

  for (;;)
    {
      a = find(s, n, pos, "<InstanceID");
      if (a < 0)
	break;
      gt = find(s, n, a, ">");
      if (gt < 0)
	break;
      pos = gt + 1;

      if (attr_get(s + a + 1, gt - a - 1, "val", val, sizeof(val)) < 0 || strcmp(val, "0") != 0)
	continue;

      end = find(s, n, pos, "</InstanceID>");
      instance_read(s + pos, ((end < 0) ? (long)n : end) - pos, out);
      break;
    }

  free(s);
  return 0;
}

void
upnp_rc_state_merge(struct upnp_rc_state *known, const struct upnp_rc_state *notify)
{
  if (notify->has_volume)
    {
      known->has_volume = true;
      known->volume = notify->volume;
    }
  if (notify->has_mute)
    {
      known->has_mute = true;
      known->mute = notify->mute;
    }
}


/* -------------------------------- Chunked --------------------------------- */

// One pass over the chunked framing; with write set, the data is moved down
// over the framing. The body's length, -2 incomplete, -1 malformed.
static long
dechunk_pass(char *buf, size_t len, bool write)
{
  size_t r = 0;
  size_t w = 0;
  size_t size;
  size_t digits;
  const char *nl;
  int c;

  for (;;)
    {
      size = 0;
      digits = 0;
      while (r < len && isxdigit((unsigned char)buf[r]))
	{
	  if (++digits > 7)
	    return -1;
	  c = tolower((unsigned char)buf[r]);
	  size = size * 16 + (size_t)(isdigit(c) ? c - '0' : c - 'a' + 10);
	  r++;
	}
      if (r >= len)
	return -2;
      if (digits == 0)
	return -1;

      // A chunk extension runs to the end of the line
      nl = memchr(buf + r, '\n', len - r);
      if (!nl)
	return (len - r > 256) ? -1 : -2;
      if (buf[r] != ';' && buf[r] != '\r' && buf[r] != '\n')
	return -1;
      r = nl + 1 - buf;

      if (size == 0)
	{
	  // Trailer lines, then the empty line
	  for (;;)
	    {
	      nl = memchr(buf + r, '\n', len - r);
	      if (!nl)
		return -2;
	      if (nl == buf + r || (nl == buf + r + 1 && buf[r] == '\r'))
		return (long)w;
	      r = nl + 1 - buf;
	    }
	}

      if (len - r < size + 1)
	return -2;
      if (write)
	memmove(buf + w, buf + r, size);
      w += size;
      r += size;

      if (buf[r] == '\r')
	{
	  if (r + 1 >= len)
	    return -2;
	  r++;
	}
      if (buf[r] != '\n')
	return -1;
      r++;
    }
}

long
upnp_http_dechunk(char *buf, size_t len)
{
  long ret;

  ret = dechunk_pass(buf, len, false);
  if (ret < 0)
    return ret;

  return dechunk_pass(buf, len, true);
}


/* ------------------------------ Level rules ------------------------------- */

int
upnp_level_map(int raw, bool mute, int offset)
{
  int level;

  if (mute || raw <= 0)
    return 0;

  level = raw - offset;
  if (level < 1)
    return 1;
  if (level > 100)
    return 100;

  return level;
}

enum upnp_echo
upnp_level_echo(int raw, bool mute, int sent, int *offset)
{
  // A mute (sent as -144 dB, or set on the renderer while the level was on
  // its way) says nothing about the scale
  if (mute || sent <= 0)
    return UPNP_ECHO_NO_LEARN;

  if (sent < LEARN_LOW || sent > LEARN_HIGH)
    return UPNP_ECHO_NO_LEARN;

  if (abs(raw - sent) > LEARN_SPAN)
    return UPNP_ECHO_SCALE_UNKNOWN;

  *offset = raw - sent;
  return UPNP_ECHO_LEARN;
}
