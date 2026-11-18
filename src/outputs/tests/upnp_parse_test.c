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

// Checks the reading of what a UPnP renderer sends, and the level rules, with
// no daemon, no socket and no framework. The renderer's messages are copies of
// what an Edifier MS50A pair sent, with its addresses and ids replaced, kept in the source
// so the test needs no data files. From the top of the source tree:
//
//   cc -std=gnu11 -Wall -Wextra -Werror -o /tmp/upnp_parse_test src/outputs/tests/upnp_parse_test.c src/upnp_parse.c
//   /tmp/upnp_parse_test
//
// Exits 0 when every case passes, 1 otherwise, printing one line per failure
// and a count (with -v, one line per passing case too). Also listed under
// check_PROGRAMS and TESTS in src/Makefile.am.

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../upnp_parse.h"

static int cases_run;
static int cases_failed;
static bool verbose;
static const char *cur_name;
static bool cur_failed;

#define CHECK(cond) \
  do { \
    if (!(cond)) \
      { \
	printf("FAIL %s: line %d: %s\n", cur_name, __LINE__, #cond); \
	cur_failed = true; \
      } \
  } while (0)

static void
case_begin(const char *name)
{
  cur_name = name;
  cur_failed = false;
}

static void
case_end(void)
{
  cases_run++;
  if (cur_failed)
    cases_failed++;
  else if (verbose)
    printf("PASS %s\n", cur_name);
}

static int
head(const char *s, struct upnp_http_head *h)
{
  return upnp_http_head_parse(s, strlen(s), 4096, h);
}


/* ------------------------------- Captures --------------------------------- */

static const char ssdp_reply_edifier[] =
  "HTTP/1.1 200 OK\r\nCONNECTION: keep-alive\r\nCACHE-CONTROL: max-age=1800\r\n"
  "EXT:\r\n"
  "LOCATION: http://192.168.1.20:49152/description.xml\r\n"
  "BOOTID.UPNP.ORG: 0a1b2c3d-1dd2-11b2-8000-0123456789ab\r\nSERVER: Linux/4.9.118\r\n"
  "ST: urn:schemas-upnp-org:service:RenderingControl:1\r\n"
  "USN: uuid:2B4C6D8E-1A3B-4C5D-8E9F-0A1B2C3D4E5F::urn:schemas-upnp-org:service:RenderingControl:1\r\n\r\n";

static const char subscribe_reply_edifier[] =
  "HTTP/1.1 200 OK\r\nCONNECTION: keep-alive\r\n"
  "SERVER: Linux/4.9.118\r\nCONTENT-LENGTH: 0\r\n"
  "SID: uuid:5e6f7a8b-1dd2-11b2-8000-0123456789ab\r\nTIMEOUT: Second-60\r\n\r\n";

static const char notify_head_edifier[] =
  "NOTIFY /upnp/0011223344556677 HTTP/1.1\r\nHOST: 192.168.1.10:3699\r\n"
  "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\nCONTENT-LENGTH: 647\r\nNT: upnp:event\r\n"
  "NTS: upnp:propchange\r\nSID: uuid:5e6f7a8b-1dd2-11b2-8000-0123456789ab\r\nSEQ: 0\r\n\r\n";

// The first event after a subscribe: the whole state
static const char notify_initial_edifier[] =
  "<?xml version='1.0' encoding='UTF-8' ?>\r\n"
  "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">\r\n  <e:property>\r\n"
  "    <LastChange>&lt;Event xmlns=&quot;urn:schemas-upnp-org:metadata-1-0/RCS/&quot;&gt;\r\n"
  "  &lt;InstanceID val=&quot;0&quot;&gt;\r\n"
  "  &lt;Mute channel=&quot;Master&quot; val=&quot;0&quot;/&gt;\r\n"
  "  &lt;Channel channel=&quot;Master&quot; val=&quot;1&quot;/&gt;\r\n"
  "  &lt;Equaluzer channel=&quot;Master&quot; val=&quot;0&quot;/&gt;\r\n"
  "  &lt;Volume channel=&quot;Master&quot; val=&quot;25&quot;/&gt;\r\n"
  "  &lt;PresetNameList val=&quot;FactoryDefaults&quot;/&gt;\r\n"
  "  &lt;/InstanceID&gt;\r\n&lt;/Event&gt;</LastChange>\r\n</e:property>\r\n</e:propertyset>\r\n\r\n";

// After a knob turn: the second speaker's level, which has no Master entry
static const char notify_slave_edifier[] =
  "<?xml version='1.0' encoding='UTF-8' ?>\r\n"
  "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">\r\n  <e:property>\r\n"
  "    <LastChange>&lt;Event xmlns=&quot;urn:schemas-upnp-org:metadata-1-0/RCS/&quot;&gt;\r\n"
  "  &lt;InstanceID val=&quot;0&quot;&gt;\r\n"
  "  &lt;Slave action=&quot;volumechange&quot; val=&quot;0A1B2C3D4E5F60718293A4B5&quot; SlaveVolume=&quot;3&quot; TimeStamp=&quot;1595535424&quot;/&gt;\r\n"
  "  &lt;/InstanceID&gt;\r\n&lt;/Event&gt;</LastChange>\r\n</e:property>\r\n</e:propertyset>\r\n";

static const char notify_update_edifier[] =
  "<?xml version='1.0' encoding='UTF-8' ?>\r\n"
  "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">\r\n  <e:property>\r\n"
  "    <LastChange>&lt;Event xmlns=&quot;urn:schemas-upnp-org:metadata-1-0/RCS/&quot;&gt;\r\n"
  "  &lt;InstanceID val=&quot;0&quot;&gt;\r\n"
  "  &lt;Slave action=&quot;update&quot; val=&quot;0A1B2C3D4E5F60718293A4B5&quot;/&gt;\r\n"
  "  &lt;/InstanceID&gt;\r\n&lt;/Event&gt;</LastChange>\r\n</e:property>\r\n</e:propertyset>\r\n";

static const char description_edifier[] =
  "<?xml version=\"1.0\"?>\n"
  "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">\n"
  "<specVersion>\n<major>1</major>\n<minor>0</minor>\n</specVersion>\n<device>\n"
  "<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>\n"
  "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>\n"
  "<friendlyName>Dining Room</friendlyName>\n<manufacturer>Edifier Inc</manufacturer>\n"
  "<manufacturerURL>https://www.edifier.com/</manufacturerURL>\n"
  "<modelDescription> Edifier MS50A</modelDescription>\n<modelName>Edifier MS50A</modelName>\n"
  "<modelURL>https://www.edifier.com/</modelURL>\n<UDN>uuid:2B4C6D8E-1A3B-4C5D-8E9F-0A1B2C3D4E5F</UDN>\n"
  "<modelNumber>V01-Sep 23 2022\n</modelNumber>\n<serialNumber>00001</serialNumber>\n"
  "<iconList>\n<icon>\n<mimetype>image/png</mimetype>\n<width>48</width>\n<height>48</height>\n"
  "<depth>24</depth>\n<url>/upnp/grender-48x48.png</url>\n</icon>\n</iconList>\n"
  "<serviceList>\n"
  "<service>\n<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>\n"
  "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>\n<SCPDURL>/upnp/rendertransportSCPD.xml</SCPDURL>\n"
  "<controlURL>/upnp/control/rendertransport1</controlURL>\n<eventSubURL>/upnp/event/rendertransport1</eventSubURL>\n</service>\n"
  "<service>\n<serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>\n"
  "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>\n<SCPDURL>/upnp/renderconnmgrSCPD.xml</SCPDURL>\n"
  "<controlURL>/upnp/control/renderconnmgr1</controlURL>\n<eventSubURL>/upnp/event/renderconnmgr1</eventSubURL>\n</service>\n"
  "<service>\n<serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>\n"
  "<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>\n<SCPDURL>/upnp/rendercontrolSCPD.xml</SCPDURL>\n"
  "<controlURL>/upnp/control/rendercontrol1</controlURL>\n<eventSubURL>/upnp/event/rendercontrol1</eventSubURL>\n</service>\n"
  "<service>\n<serviceType>urn:schemas-wiimu-com:service:PlayQueue:1</serviceType>\n"
  "<serviceId>urn:wiimu-com:serviceId:PlayQueue</serviceId>\n<SCPDURL>/upnp/PlayQueueSCPD.xml</SCPDURL>\n"
  "<controlURL>/upnp/control/PlayQueue1</controlURL>\n<eventSubURL>/upnp/event/PlayQueue1</eventSubURL>\n</service>\n"
  "</serviceList>\n</device>\n</root>\n";

#define EDIFIER_LOCATION "http://192.168.1.20:49152/description.xml"


/* ------------------------------ HTTP heads -------------------------------- */

static void
head_ssdp_reply(void)
{
  struct upnp_http_head h;

  case_begin("head_ssdp_reply");
  CHECK(head(ssdp_reply_edifier, &h) == 1);
  CHECK(h.status == 200);
  CHECK(h.method[0] == '\0');
  CHECK(strcmp(h.location, EDIFIER_LOCATION) == 0);
  CHECK(strcmp(h.st, "urn:schemas-upnp-org:service:RenderingControl:1") == 0);
  CHECK(h.head_len == strlen(ssdp_reply_edifier));
  CHECK(h.content_length == -1);
  CHECK(h.timeout_s == -1);
  case_end();
}

static void
head_subscribe_reply(void)
{
  struct upnp_http_head h;

  case_begin("head_subscribe_reply");
  CHECK(head(subscribe_reply_edifier, &h) == 1);
  CHECK(h.status == 200);
  CHECK(strcmp(h.sid, "uuid:5e6f7a8b-1dd2-11b2-8000-0123456789ab") == 0);
  CHECK(h.timeout_s == 60);
  CHECK(h.content_length == 0);
  case_end();
}

static void
head_notify_request(void)
{
  struct upnp_http_head h;
  char buf[2048];

  case_begin("head_notify_request");
  snprintf(buf, sizeof(buf), "%s%s", notify_head_edifier, notify_initial_edifier);
  CHECK(upnp_http_head_parse(buf, strlen(buf), 4096, &h) == 1);
  CHECK(strcmp(h.method, "NOTIFY") == 0);
  CHECK(strcmp(h.target, "/upnp/0011223344556677") == 0);
  CHECK(h.status == 0);
  CHECK(strcmp(h.nt, "upnp:event") == 0);
  CHECK(strcmp(h.nts, "upnp:propchange") == 0);
  CHECK(h.content_length == 647);
  CHECK(h.head_len == strlen(notify_head_edifier));
  // The renderer sends one byte more than it says
  CHECK((size_t)h.content_length + 1 == strlen(notify_initial_edifier));
  case_end();
}

static void
head_forms(void)
{
  struct upnp_http_head h;

  case_begin("head_forms");
  // HTTP/1.0, folded case and bare line feeds
  CHECK(head("HTTP/1.0 412 Precondition Failed\nsid: uuid:x\ntimeout: second-1800\n\n", &h) == 1);
  CHECK(h.status == 412);
  CHECK(strcmp(h.sid, "uuid:x") == 0);
  CHECK(h.timeout_s == 1800);
  CHECK(head("NOTIFY /upnp/1 HTTP/1.0\r\nSid:   uuid:y   \r\n\r\n", &h) == 1);
  CHECK(strcmp(h.method, "NOTIFY") == 0);
  CHECK(strcmp(h.sid, "uuid:y") == 0);
  // A status line with no reason
  CHECK(head("HTTP/1.1 200\r\n\r\n", &h) == 1);
  CHECK(h.status == 200);
  // TIMEOUT forms
  CHECK(head("HTTP/1.1 200 OK\r\nTIMEOUT: Second-infinite\r\n\r\n", &h) == 1);
  CHECK(h.timeout_s == 0);
  CHECK(head("HTTP/1.1 200 OK\r\nTIMEOUT: infinite\r\n\r\n", &h) == 1);
  CHECK(h.timeout_s == 0);
  CHECK(head("HTTP/1.1 200 OK\r\nTIMEOUT: Second-\r\n\r\n", &h) == 1);
  CHECK(h.timeout_s == -1);
  CHECK(head("HTTP/1.1 200 OK\r\nTIMEOUT: Minute-3\r\n\r\n", &h) == 1);
  CHECK(h.timeout_s == -1);
  CHECK(head("HTTP/1.1 200 OK\r\nTIMEOUT: Second-0\r\n\r\n", &h) == 1);
  CHECK(h.timeout_s == -1);
  // Chunked
  CHECK(head("NOTIFY / HTTP/1.1\r\nTransfer-Encoding: Chunked\r\n\r\n", &h) == 1);
  CHECK(h.chunked);
  CHECK(h.content_length == -1);
  case_end();
}

static void
head_incomplete_and_malformed(void)
{
  struct upnp_http_head h;
  char big[6000];
  size_t n;
  int i;

  case_begin("head_incomplete_and_malformed");
  CHECK(head("HTTP/1.1 200 OK\r\nSID: uuid:x\r\n", &h) == 0);
  CHECK(head("HTTP/1.1 200 OK", &h) == 0);
  CHECK(head("", &h) == 0);
  CHECK(head("\r\n\r\n", &h) == -1);
  CHECK(head("HTTP/2 200 OK\r\n\r\n", &h) == -1);
  CHECK(head("HTTP/1.1 2x0 OK\r\n\r\n", &h) == -1);
  CHECK(head("notify / HTTP/1.1\r\n\r\n", &h) == -1);
  CHECK(head("NOTIFY / HTTP/1.2\r\n\r\n", &h) == -1);
  CHECK(head("NOTIFY /\r\n\r\n", &h) == -1);
  CHECK(head("HTTP/1.1 200 OK\r\nno colon here\r\n\r\n", &h) == -1);
  CHECK(head("HTTP/1.1 200 OK\r\n folded: x\r\n\r\n", &h) == -1);
  CHECK(head("HTTP/1.1 200 OK\r\nContent-Length: 12a\r\n\r\n", &h) == -1);
  CHECK(head("HTTP/1.1 200 OK\r\nContent-Length: 9999999999\r\n\r\n", &h) == -1);

  // A header line over the limit
  n = snprintf(big, sizeof(big), "HTTP/1.1 200 OK\r\nX: ");
  memset(big + n, 'a', 1100);
  n += 1100;
  n += snprintf(big + n, sizeof(big) - n, "\r\n\r\n");
  CHECK(upnp_http_head_parse(big, n, 4096, &h) == -1);
  // ... and one still arriving
  CHECK(upnp_http_head_parse(big, 1100, 4096, &h) == -1);

  // 64 header lines are allowed, 65 are not
  n = snprintf(big, sizeof(big), "HTTP/1.1 200 OK\r\n");
  for (i = 0; i < 64; i++)
    n += snprintf(big + n, sizeof(big) - n, "X-%d: y\r\n", i);
  CHECK(upnp_http_head_parse(big, n, 4096, &h) == 0);
  memcpy(big + n, "\r\n", 2);
  CHECK(upnp_http_head_parse(big, n + 2, 4096, &h) == 1);
  n += snprintf(big + n, sizeof(big) - n, "X-last: y\r\n\r\n");
  CHECK(upnp_http_head_parse(big, n, 4096, &h) == -1);

  // No blank line within the maximum
  CHECK(upnp_http_head_parse(subscribe_reply_edifier, strlen(subscribe_reply_edifier), 40, &h) == -1);
  // An over-long SID is refused rather than cut short
  n = snprintf(big, sizeof(big), "HTTP/1.1 200 OK\r\nSID: uuid:");
  memset(big + n, 'b', 120);
  n += 120;
  n += snprintf(big + n, sizeof(big) - n, "\r\n\r\n");
  CHECK(upnp_http_head_parse(big, n, 4096, &h) == -1);
  case_end();
}


/* --------------------------------- URLs ----------------------------------- */

static void
url_split(void)
{
  char host[16];
  char path[64];
  unsigned short port;

  case_begin("url_split");
  CHECK(upnp_url_split("http://192.168.1.20:49152/upnp/event/rendercontrol1", host, sizeof(host), &port, path, sizeof(path)) == 0);
  CHECK(strcmp(host, "192.168.1.20") == 0);
  CHECK(port == 49152);
  CHECK(strcmp(path, "/upnp/event/rendercontrol1") == 0);
  CHECK(upnp_url_split("http://10.0.0.1", host, sizeof(host), &port, path, sizeof(path)) == 0);
  CHECK(port == 80);
  CHECK(strcmp(path, "/") == 0);
  CHECK(upnp_url_split("https://10.0.0.1/", host, sizeof(host), &port, path, sizeof(path)) == -1);
  CHECK(upnp_url_split("http://speaker.local/", host, sizeof(host), &port, path, sizeof(path)) == -1);
  CHECK(upnp_url_split("http://10.0.0.1:/", host, sizeof(host), &port, path, sizeof(path)) == -1);
  CHECK(upnp_url_split("http://10.0.0.1:70000/", host, sizeof(host), &port, path, sizeof(path)) == -1);
  CHECK(upnp_url_split("http://[fe80::1]:80/", host, sizeof(host), &port, path, sizeof(path)) == -1);
  case_end();
}


/* ------------------------------ Description ------------------------------- */

static void
description_edifier_parsed(void)
{
  struct upnp_rc_service rc;

  case_begin("description_edifier_parsed");
  CHECK(upnp_description_parse(description_edifier, strlen(description_edifier), EDIFIER_LOCATION, &rc) == 0);
  CHECK(strcmp(rc.event_url, "http://192.168.1.20:49152/upnp/event/rendercontrol1") == 0);
  CHECK(strcmp(rc.control_url, "http://192.168.1.20:49152/upnp/control/rendercontrol1") == 0);
  CHECK(strcmp(rc.manufacturer, "Edifier Inc") == 0);
  CHECK(strcmp(rc.model, "Edifier MS50A") == 0);
  CHECK(strcmp(rc.friendly_name, "Dining Room") == 0);
  case_end();
}

static void
description_forms(void)
{
  struct upnp_rc_service rc;
  const char *d;

  case_begin("description_forms");
  d = "<root><URLBase>http://10.0.0.5:1400/</URLBase><device><serviceList><service>"
      "<serviceType>urn:schemas-upnp-org:service:RenderingControl:3</serviceType>"
      "<controlURL>MediaRenderer/RenderingControl/Control</controlURL>"
      "<eventSubURL>MediaRenderer/RenderingControl/Event</eventSubURL></service></serviceList></device></root>";
  CHECK(upnp_description_parse(d, strlen(d), "http://10.0.0.5:1400/xml/device_description.xml", &rc) == 0);
  CHECK(strcmp(rc.event_url, "http://10.0.0.5:1400/MediaRenderer/RenderingControl/Event") == 0);

  // Absolute URLs
  d = "<root><device><serviceList><service>"
      "<serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>"
      "<controlURL>http://10.0.0.6:8080/rc/ctl</controlURL>"
      "<eventSubURL>http://10.0.0.6:8080/rc/evt</eventSubURL></service></serviceList></device></root>";
  CHECK(upnp_description_parse(d, strlen(d), "http://10.0.0.6:8080/desc.xml", &rc) == 0);
  CHECK(strcmp(rc.event_url, "http://10.0.0.6:8080/rc/evt") == 0);

  // Relative to the description's directory
  d = "<root><device><serviceList><service>"
      "<serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>"
      "<eventSubURL>rc/evt</eventSubURL></service></serviceList></device></root>";
  CHECK(upnp_description_parse(d, strlen(d), "http://10.0.0.6:8080/dev/desc.xml", &rc) == 0);
  CHECK(strcmp(rc.event_url, "http://10.0.0.6:8080/dev/rc/evt") == 0);

  // An event URL on another host
  d = "<root><device><serviceList><service>"
      "<serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>"
      "<eventSubURL>http://10.0.0.99:8080/rc/evt</eventSubURL></service></serviceList></device></root>";
  CHECK(upnp_description_parse(d, strlen(d), "http://10.0.0.6:8080/desc.xml", &rc) == -2);

  // No RenderingControl
  d = "<root><device><serviceList><service>"
      "<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
      "<eventSubURL>/avt/evt</eventSubURL></service></serviceList></device></root>";
  CHECK(upnp_description_parse(d, strlen(d), "http://10.0.0.6:8080/desc.xml", &rc) == -1);

  // Only Sonos's GroupRenderingControl
  d = "<root><device><serviceList><service>"
      "<serviceType>urn:schemas-upnp-org:service:GroupRenderingControl:1</serviceType>"
      "<eventSubURL>/grc/evt</eventSubURL></service></serviceList></device></root>";
  CHECK(upnp_description_parse(d, strlen(d), "http://10.0.0.6:8080/desc.xml", &rc) == -1);

  // RenderingControl with no event URL
  d = "<root><device><serviceList><service>"
      "<serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>"
      "</service></serviceList></device></root>";
  CHECK(upnp_description_parse(d, strlen(d), "http://10.0.0.6:8080/desc.xml", &rc) == -2);

  // Truncated in the middle of the service
  CHECK(upnp_description_parse(description_edifier, strstr(description_edifier, "rendercontrol1</controlURL>") - description_edifier,
			       EDIFIER_LOCATION, &rc) == -2);
  // Not a description at all, or a bad LOCATION
  CHECK(upnp_description_parse("garbage", 7, EDIFIER_LOCATION, &rc) == -2);
  CHECK(upnp_description_parse(description_edifier, strlen(description_edifier), "http://speaker.local/d.xml", &rc) == -2);
  case_end();
}


/* -------------------------------- Events ---------------------------------- */

static void
notify_edifier(void)
{
  struct upnp_rc_state st;

  case_begin("notify_edifier");
  CHECK(upnp_notify_parse(notify_initial_edifier, strlen(notify_initial_edifier), &st) == 0);
  CHECK(st.has_volume && st.volume == 25);
  CHECK(st.has_mute && !st.mute);
  // Read to the length it gives
  CHECK(upnp_notify_parse(notify_initial_edifier, 647, &st) == 0);
  CHECK(st.has_volume && st.volume == 25);

  CHECK(upnp_notify_parse(notify_slave_edifier, strlen(notify_slave_edifier), &st) == 0);
  CHECK(!st.has_volume && !st.has_mute);
  CHECK(upnp_notify_parse(notify_update_edifier, strlen(notify_update_edifier), &st) == 0);
  CHECK(!st.has_volume && !st.has_mute);
  case_end();
}

static void
notify_forms(void)
{
  struct upnp_rc_state st;
  const char *b;

  case_begin("notify_forms");
  // Escaped twice
  b = "<e:propertyset><e:property><LastChange>&amp;lt;Event&amp;gt;&amp;lt;InstanceID val=&amp;quot;0&amp;quot;&amp;gt;"
      "&amp;lt;Volume channel=&amp;quot;Master&amp;quot; val=&amp;quot;31&amp;quot;/&amp;gt;"
      "&amp;lt;/InstanceID&amp;gt;&amp;lt;/Event&amp;gt;</LastChange></e:property></e:propertyset>";
  CHECK(upnp_notify_parse(b, strlen(b), &st) == 0);
  CHECK(st.has_volume && st.volume == 31);
  CHECK(!st.has_mute);

  // In CDATA, with single quotes, Mute only, other channels before Master
  b = "<e:propertyset><e:property><LastChange><![CDATA[<Event><InstanceID val='0'>"
      "<Volume channel='LF' val='3'/><Mute channel='Master' val='1'/></InstanceID></Event>]]></LastChange></e:property></e:propertyset>";
  CHECK(upnp_notify_parse(b, strlen(b), &st) == 0);
  CHECK(!st.has_volume);
  CHECK(st.has_mute && st.mute);

  // Only InstanceID 1
  b = "<LastChange>&lt;Event&gt;&lt;InstanceID val=&quot;1&quot;&gt;&lt;Volume channel=&quot;Master&quot; val=&quot;9&quot;/&gt;"
      "&lt;/InstanceID&gt;&lt;/Event&gt;</LastChange>";
  CHECK(upnp_notify_parse(b, strlen(b), &st) == 0);
  CHECK(!st.has_volume);

  // InstanceID 1, then 0
  b = "<LastChange>&lt;Event&gt;&lt;InstanceID val=&quot;1&quot;&gt;&lt;Volume channel=&quot;Master&quot; val=&quot;9&quot;/&gt;"
      "&lt;/InstanceID&gt;&lt;InstanceID val=&quot;0&quot;&gt;&lt;Volume channel=&quot;Master&quot; val=&quot;12&quot;/&gt;"
      "&lt;/InstanceID&gt;&lt;/Event&gt;</LastChange>";
  CHECK(upnp_notify_parse(b, strlen(b), &st) == 0);
  CHECK(st.has_volume && st.volume == 12);

  // A VolumeDB element is not the Volume
  b = "<LastChange>&lt;Event&gt;&lt;InstanceID val=&quot;0&quot;&gt;&lt;VolumeDB channel=&quot;Master&quot; val=&quot;-300&quot;/&gt;"
      "&lt;/InstanceID&gt;&lt;/Event&gt;</LastChange>";
  CHECK(upnp_notify_parse(b, strlen(b), &st) == 0);
  CHECK(!st.has_volume);

  // A value that is not a number
  b = "<LastChange>&lt;Event&gt;&lt;InstanceID val=&quot;0&quot;&gt;&lt;Volume channel=&quot;Master&quot; val=&quot;loud&quot;/&gt;"
      "&lt;/InstanceID&gt;&lt;/Event&gt;</LastChange>";
  CHECK(upnp_notify_parse(b, strlen(b), &st) == 0);
  CHECK(!st.has_volume);

  // Garbage and a missing LastChange
  CHECK(upnp_notify_parse("garbage", 7, &st) == -1);
  CHECK(upnp_notify_parse("<LastChange>never closed", 24, &st) == -1);
  b = "<e:propertyset><e:property><Volume>4</Volume></e:property></e:propertyset>";
  CHECK(upnp_notify_parse(b, strlen(b), &st) == -1);

  // Only the length given is read
  CHECK(upnp_notify_parse(notify_initial_edifier, 100, &st) == -1);
  case_end();
}

static void
notify_merge(void)
{
  struct upnp_rc_state known = { 0 };
  struct upnp_rc_state ev = { 0 };

  case_begin("notify_merge");
  ev.has_volume = true;
  ev.volume = 25;
  ev.has_mute = true;
  ev.mute = false;
  upnp_rc_state_merge(&known, &ev);
  CHECK(known.has_volume && known.volume == 25 && known.has_mute && !known.mute);

  memset(&ev, 0, sizeof(ev));
  ev.has_mute = true;
  ev.mute = true;
  upnp_rc_state_merge(&known, &ev);
  CHECK(known.volume == 25 && known.mute);

  memset(&ev, 0, sizeof(ev));
  ev.has_volume = true;
  ev.volume = 31;
  upnp_rc_state_merge(&known, &ev);
  CHECK(known.volume == 31 && known.mute);

  memset(&ev, 0, sizeof(ev));
  upnp_rc_state_merge(&known, &ev);
  CHECK(known.volume == 31 && known.mute);
  case_end();
}


/* -------------------------------- Chunked --------------------------------- */

static long
dechunk(const char *in, char *buf, size_t buflen)
{
  size_t n = strlen(in);

  if (n >= buflen)
    return -9;
  memcpy(buf, in, n + 1);
  return upnp_http_dechunk(buf, n);
}

static void
chunked(void)
{
  char buf[256];
  long n;

  case_begin("chunked");
  n = dechunk("5\r\nhello\r\n7;ext=1\r\n, world\r\n0\r\n\r\n", buf, sizeof(buf));
  CHECK(n == 12);
  CHECK(n == 12 && memcmp(buf, "hello, world", 12) == 0);
  n = dechunk("A\nabcdefghij\n0\nTrailer: x\n\n", buf, sizeof(buf));
  CHECK(n == 10 && memcmp(buf, "abcdefghij", 10) == 0);
  n = dechunk("0\r\n\r\n", buf, sizeof(buf));
  CHECK(n == 0);

  // Still arriving: the buffer is left alone
  CHECK(dechunk("5\r\nhel", buf, sizeof(buf)) == -2);
  CHECK(strcmp(buf, "5\r\nhel") == 0);
  CHECK(dechunk("5\r\nhello\r\n", buf, sizeof(buf)) == -2);
  CHECK(dechunk("5\r\nhello\r\n0\r\n", buf, sizeof(buf)) == -2);
  CHECK(dechunk("5", buf, sizeof(buf)) == -2);
  CHECK(dechunk("", buf, sizeof(buf)) == -2);

  // Malformed
  CHECK(dechunk("x\r\nhello\r\n0\r\n\r\n", buf, sizeof(buf)) == -1);
  CHECK(dechunk("5\r\nhelloXX0\r\n\r\n", buf, sizeof(buf)) == -1);
  CHECK(dechunk("5 \r\nhello\r\n0\r\n\r\n", buf, sizeof(buf)) == -1);
  CHECK(dechunk("fffffffff\r\n", buf, sizeof(buf)) == -1);
  CHECK(strcmp(buf, "fffffffff\r\n") == 0);
  case_end();
}


/* ------------------------------ Level rules ------------------------------- */

static void
level_map(void)
{
  case_begin("level_map");
  CHECK(upnp_level_map(25, false, 1) == 24);
  CHECK(upnp_level_map(25, false, 0) == 25);
  CHECK(upnp_level_map(25, true, 1) == 0);
  CHECK(upnp_level_map(0, false, 0) == 0);
  CHECK(upnp_level_map(0, false, -1) == 0);
  CHECK(upnp_level_map(-3, false, 0) == 0);
  // Never silent while the renderer plays
  CHECK(upnp_level_map(1, false, 1) == 1);
  CHECK(upnp_level_map(2, false, 2) == 1);
  CHECK(upnp_level_map(100, false, 1) == 99);
  CHECK(upnp_level_map(100, false, -2) == 100);
  CHECK(upnp_level_map(150, false, 0) == 100);
  case_end();
}

static void
level_echo(void)
{
  int offset;

  case_begin("level_echo");
  offset = 99;
  CHECK(upnp_level_echo(25, false, 24, &offset) == UPNP_ECHO_LEARN);
  CHECK(offset == 1);
  CHECK(upnp_level_echo(24, false, 24, &offset) == UPNP_ECHO_LEARN);
  CHECK(offset == 0);
  CHECK(upnp_level_echo(22, false, 24, &offset) == UPNP_ECHO_LEARN);
  CHECK(offset == -2);
  CHECK(upnp_level_echo(27, false, 24, &offset) == UPNP_ECHO_SCALE_UNKNOWN);
  CHECK(upnp_level_echo(21, false, 24, &offset) == UPNP_ECHO_SCALE_UNKNOWN);
  // A renderer on a 0-255 scale
  CHECK(upnp_level_echo(61, false, 24, &offset) == UPNP_ECHO_SCALE_UNKNOWN);

  // The ends of the scale teach nothing
  offset = 99;
  CHECK(upnp_level_echo(1, false, 1, &offset) == UPNP_ECHO_NO_LEARN);
  CHECK(upnp_level_echo(40, false, 1, &offset) == UPNP_ECHO_NO_LEARN);
  CHECK(upnp_level_echo(100, false, 99, &offset) == UPNP_ECHO_NO_LEARN);
  CHECK(upnp_level_echo(100, false, 100, &offset) == UPNP_ECHO_NO_LEARN);
  CHECK(upnp_level_echo(3, false, 2, &offset) == UPNP_ECHO_LEARN);
  CHECK(offset == 1);
  offset = 99;
  CHECK(upnp_level_echo(99, false, 98, &offset) == UPNP_ECHO_LEARN);
  CHECK(offset == 1);

  // Mutes
  offset = 99;
  CHECK(upnp_level_echo(0, false, 0, &offset) == UPNP_ECHO_NO_LEARN);
  CHECK(upnp_level_echo(25, true, 0, &offset) == UPNP_ECHO_NO_LEARN);
  CHECK(upnp_level_echo(0, true, 0, &offset) == UPNP_ECHO_NO_LEARN);
  CHECK(upnp_level_echo(25, true, 24, &offset) == UPNP_ECHO_NO_LEARN);
  CHECK(offset == 99);
  case_end();
}

static void
report_is_answer(void)
{
  case_begin("report_is_answer");
  // The first report soon after a level is sent is the answer
  CHECK(upnp_report_is_answer(1000, false, 1500, 2000));
  CHECK(upnp_report_is_answer(1000, false, 2999, 2000));
  // Once answered, the reports that follow in the window are the device's own:
  // the answer to 17 is 18, then the knob is turned to 25 and 31
  CHECK(!upnp_report_is_answer(1000, true, 1500, 2000));
  CHECK(!upnp_report_is_answer(1000, true, 1900, 2000));
  // Past the window, or with nothing sent, nothing is an answer
  CHECK(!upnp_report_is_answer(1000, false, 3000, 2000));
  CHECK(!upnp_report_is_answer(1000, false, 9000, 2000));
  CHECK(!upnp_report_is_answer(0, false, 500, 2000));
  case_end();
}

int
main(int argc, char **argv)
{
  verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

  head_ssdp_reply();
  head_subscribe_reply();
  head_notify_request();
  head_forms();
  head_incomplete_and_malformed();
  url_split();
  description_edifier_parsed();
  description_forms();
  notify_edifier();
  notify_forms();
  notify_merge();
  chunked();
  level_map();
  level_echo();
  report_is_answer();

  printf("%d cases, %d failed\n", cases_run, cases_failed);

  return cases_failed ? 1 : 0;
}
