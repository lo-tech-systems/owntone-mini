
sbin_PROGRAMS = owntone



PAIR_AP_SRC = \
	pair_ap/pair_fruit.c pair_ap/pair_homekit.c pair_ap/pair-tlv.c \
	pair_ap/pair.c pair_ap/pair-tlv.h pair_ap/pair-internal.h pair_ap/pair.h
AM_CPPFLAGS += -DCONFIG_GCRYPT


if COND_AVAHI
MDNS_SRC=mdns_avahi.c
else
MDNS_SRC=mdns_dnssd.c
endif


AM_CPPFLAGS += \
	$(OWNTONE_CPPFLAGS) \
	$(OWNTONE_OPTS_CPPFLAGS) \
	$(COMMON_CPPFLAGS) \
	\
	-D_GNU_SOURCE \
	-DDATADIR=\"$(pkgdatadir)\" \
	-DCONFDIR=\"$(sysconfdir)\" \
	-DSTATEDIR=\"$(localstatedir)\" \
	-DPKGLIBDIR=\"$(pkglibdir)\"

owntone_LDADD = \
	$(OWNTONE_LIBS) \
	$(OWNTONE_OPTS_LIBS) \
	$(COMMON_LIBS)

owntone_SOURCES = main.c \
	db.c db.h \
	queue.c queue.h \
	logger.c logger.h \
	owntone_config.c owntone_config.h \
	conffile.c conffile.h \
	artwork.c artwork.h \
	dmap_common.c dmap_common.h \
	$(MDNS_SRC) mdns.h \
	httpd_libevhttp.c \
	httpd.c httpd.h httpd_internal.h \
	httpd_jsonapi.c \
	httpd_dacp.c \
	transcode.c transcode.h \
	misc.c misc.h \
	misc_json.c misc_json.h \
	player.c player.h \
	worker.c worker.h \
	memstats.c memstats.h \
	input.h input.c \
	inputs/pipe.c \
	outputs.h outputs.c \
	outputs/rtp_common.h outputs/rtp_common.c \
	outputs/raop.c outputs/airplay.c $(PAIR_AP_SRC) \
	outputs/airplay_events.c outputs/airplay_events.h outputs/airplay_remote_level.h \
	outputs/airplay_mrp.c outputs/airplay_mrp.h \
	outputs/airplay_buffered.c outputs/airplay_buffered.h \
	outputs/airplay_anchor.c outputs/airplay_anchor.h \
	outputs/airplay_encoder.c outputs/airplay_encoder.h \
	outputs/airplay_crypto.c outputs/airplay_crypto.h \
	outputs/airplay_common.h \
	evrtsp/rtsp.c evrtsp/evrtsp.h evrtsp/rtsp-internal.h evrtsp/log.h \
	evthr.c evthr.h \
	listener.c listener.h \
	commands.c commands.h \
	evchan.c evchan.h \
	evchan_proto.c evchan_proto.h \
	upnp_parse.c upnp_parse.h \
	level_rules.c level_rules.h \
	upnp_sub.c upnp_sub.h \
	upnp_http.c upnp_http.h \
	upnp_follow.c upnp_follow.h \
	outputs_group_flags.h \
	output_failure.h \
	output_phase.c output_phase.h \
	outputs_callbacks.h \
	ptpd.c ptpd.h \
	outputs/plist_wrap.h

TESTS = airplay_anchor_test output_failure_test output_phase_test outputs_callbacks_test airplay_remote_level_test evchan_proto_test level_rules_test tv_proxy_mode_test speakers_summary_test upnp_parse_test upnp_sub_test worker_test
# upnp_http_check is run by hand against tools/upnp_fake_renderer.py
check_PROGRAMS = $(TESTS) upnp_http_check
airplay_anchor_test_SOURCES = outputs/tests/airplay_anchor_test.c outputs/airplay_anchor.c outputs/airplay_anchor.h
airplay_remote_level_test_SOURCES = outputs/tests/airplay_remote_level_test.c outputs/airplay_remote_level.h
airplay_remote_level_test_LDADD = -lm
evchan_proto_test_SOURCES = outputs/tests/evchan_proto_test.c evchan_proto.c evchan_proto.h
evchan_proto_test_LDADD = $(OWNTONE_LIBS)
level_rules_test_SOURCES = outputs/tests/level_rules_test.c level_rules.c level_rules.h
tv_proxy_mode_test_SOURCES = outputs/tests/tv_proxy_mode_test.c outputs_group_flags.h
output_failure_test_SOURCES = outputs/tests/output_failure_test.c output_failure.h
output_phase_test_SOURCES = outputs/tests/output_phase_test.c output_phase.c output_phase.h output_failure.h
outputs_callbacks_test_SOURCES = outputs/tests/outputs_callbacks_test.c outputs_callbacks.h
speakers_summary_test_SOURCES = outputs/tests/speakers_summary_test.c speakers_summary.h output_phase.h output_failure.h
upnp_parse_test_SOURCES = outputs/tests/upnp_parse_test.c upnp_parse.c upnp_parse.h
upnp_sub_test_SOURCES = outputs/tests/upnp_sub_test.c upnp_sub.c upnp_sub.h
worker_test_SOURCES = outputs/tests/worker_test.c worker.c worker.h evthr.c evthr.h
worker_test_LDADD = $(OWNTONE_LIBS)
upnp_http_check_SOURCES = outputs/tests/upnp_http_check.c upnp_http.c upnp_http.h upnp_parse.c upnp_parse.h upnp_sub.c upnp_sub.h
upnp_http_check_LDADD = $(OWNTONE_LIBS)

SUBDIRS = libairptp
