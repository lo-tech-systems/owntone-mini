/* Part of owntone-mini, derived from OwnTone (GPL v2 or later, see COPYING).
 * Modified for owntone-mini. Modifications Copyright (C) 2026 James Pearce */

#ifndef __PLAYER_H__
#define __PLAYER_H__

#include <stdbool.h>
#include <stdint.h>

#include "db.h"
#include "misc.h"    // for struct media_quality
#include "outputs.h" // for enum output_mode

enum play_status {
  PLAY_STOPPED = 2,
  PLAY_PAUSED  = 3,
  PLAY_PLAYING = 4,
};


struct player_speaker_info {
  uint64_t id;
  uint32_t index;
  uint32_t active_remote;
  char name[255];
  char output_type[50];
  int relvol;
  int absvol;
  int offset_ms;

  enum media_format format;
  uint32_t supported_formats;

  // Protocol mode preference and availability (see enum output_mode). Sized
  // for the longest token, "airplay2_surround_stereo".
  char mode[32];
  uint32_t supported_modes;

  bool selected;
  bool has_password;
  bool requires_auth;
  bool needs_auth_key;
  bool pin_pending;

  bool prevent_playback;
  bool busy;

  // Session state of this device and why its last start failed
  enum output_device_state state;
  enum output_failure_reason last_failure;

  // True for an Apple TV that leads a group of HomePods which are present
  bool has_homepods;

  // The output row that stands for this device in the outputs list: the device
  // itself unless it is a hidden member of a group (a HomePod behind an Apple TV,
  // the second speaker of a stereo pair), then the visible device of the group
  uint64_t row_id;
  char row_name[255];

  bool has_video;

  // The device's IPv4 address ("" if it has none), and the name it is known
  // by in the settings file (its mDNS name, which name may not be)
  char v4_address[16];
  char config_name[255];

  // Whether the level the device sets itself is followed (see upnp_follow.h)
  enum output_volume_follow volume_follow;
};

enum player_followed_result {
  FOLLOWED_RECORDED,      // level recorded on the device and its group
  FOLLOWED_CONFIRMATION,  // the device answering a level sent to it; nothing recorded
  FOLLOWED_UNCHANGED,     // the level the device already has
  FOLLOWED_HELD,          // the device has just reported over DACP; dropped
  FOLLOWED_SCALE_UNKNOWN, // the device's scale is not ours plus a small offset; dropped
  FOLLOWED_GONE,          // no such device
};

// A level a device's UPnP renderer reports, and what the player made of it
struct player_followed_report {
  uint64_t id;                          // in
  int raw;                              // in: the renderer's Master volume
  bool mute;                            // in
  enum player_followed_result result;   // out
  int pct;                              // out: the level, 0 to 100
  int offset;                           // out: the scale offset in use
  bool learned;                         // out: the offset was learned from this report
  bool follow_changed;                  // out: what the rows show about following changed
  int sent;                             // out: the level last sent to the device
  uint64_t row_id;                      // out: the row the level is reported under
  char row_name[255];                   // out
};

struct player_status {
  enum play_status status;
  char consume;

  int volume;

  /* Id of the playing file/item in the files database */
  uint32_t id;
  /* Item-Id of the playing file/item in the queue */
  uint32_t item_id;
  /* Elapsed time in ms of playing item */
  uint32_t pos_ms;
  /* Length in ms of playing item */
  uint32_t len_ms;
};

typedef void (*spk_enum_cb)(struct player_speaker_info *spk, void *arg);

int
player_get_status(struct player_status *status);

int
player_input_config_reload(void);

// Copies the figures for the memory statistics line that belong to the player
// thread (the session count and the encoder budget), so the rest of the line can
// be gathered on another thread.
int
player_memstats_sample(int *sessions, int *enc_in_use, int *enc_budget);

void
player_speaker_enumerate(spk_enum_cb cb, void *arg);

void
player_set_device_removal_grace_secs(int secs);

int
player_speaker_set(uint64_t *ids);

int
player_speaker_get_byid(struct player_speaker_info *spk, uint64_t id);

int
player_speaker_get_byactiveremote(struct player_speaker_info *spk, uint32_t active_remote);

int
player_speaker_get_byaddress(struct player_speaker_info *spk, const char *address);

int
player_speaker_get_byindex(struct player_speaker_info *spk, uint32_t index);

// include_tv: 1 or 0 to enable the group of an Apple TV with or without the
// Apple TV, -1 to leave the option as it is. Returns -4 if the option would
// change while the device is selected. volume: the level (0 to 100) the
// speakers start at, -1 for none.
int
player_speaker_enable(uint64_t id, int include_tv, int volume);

int
player_speaker_disable(uint64_t id);

void
player_speaker_resurrect(void *arg);

int
player_speaker_authorize(uint64_t id, const char *pin);

int
player_speaker_format_set(uint64_t id, enum media_format format);

int
player_speaker_offset_ms_set(uint64_t id, int offset_ms);

int
player_speaker_mode_set(uint64_t id, enum output_mode mode);


int
player_playback_start(void);

int
player_playback_start_byitem(struct db_queue_item *queue_item);

int
player_playback_start_byid(uint32_t id);

int
player_playback_stop(void);

int
player_playback_pause(void);

int
player_playback_flush(void);

int
player_volume_set(int vol);

int
player_volume_setrel_speaker(uint64_t id, int relvol);

int
player_volume_setabs_speaker(uint64_t id, int vol);

int
player_volume_setraw_speaker(uint64_t id, const char *volstr);

// A level a device's UPnP renderer reports, which is recorded (never sent to
// the device) unless it is the device's answer to a level sent to it. Any
// thread but the player. Returns 0; the outcome is in r->result.
int
player_volume_followed_report(struct player_followed_report *r);

// Sets what the rows show about following the device's level. Any thread but
// the player. 0, or -1 if there is no such device.
int
player_speaker_follow_state_set(uint64_t id, enum output_volume_follow state);

int
player_device_add(void *device);

int
player_device_remove(void *device);

int
player_init(void);

void
player_deinit(void);

#endif /* !__PLAYER_H__ */
