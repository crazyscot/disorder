/*
 * This file is part of DisOrder
 * Copyright (C) 2005, 2006, 2007 Richard Kettlewell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */
/** @file server/speaker.h
 * @brief Speaker process
 */
#ifndef SPEAKER_H
#define SPEAKER_H

#ifdef WORDS_BIGENDIAN
# define MACHINE_AO_FMT AO_FMT_BIG
#else
# define MACHINE_AO_FMT AO_FMT_LITTLE
#endif

/** @brief Minimum number of frames to try to play at once
 *
 * The main loop will only attempt to play any audio when this many
 * frames are available (or the current track has reached the end).
 * The actual number of frames it attempts to play will often be
 * larger than this (up to three times).
 *
 * For ALSA we request a buffer of three times this size and set the low
 * watermark to this amount.  The goal is then to keep between 1 and 3 times
 * this many frames in play.
 *
 * For other we attempt to play up to three times this many frames per
 * shot.  In practice we will often only send much less than this.
 */
#define FRAMES 4096

/** @brief Bytes to send per network packet
 *
 * This is the maximum number of bytes we pass to write(2); to determine actual
 * packet sizes, add a UDP header and an IP header (and a link layer header if
 * it's the link layer size you care about).
 *
 * Don't make this too big or arithmetic will start to overflow.
 */
#define NETWORK_BYTES (1500-8/*UDP*/-40/*IP*/-8/*conservatism*/)

/** @brief Maximum number of FDs to poll for */
#define NFDS 256

/** @brief Track structure
 *
 * Known tracks are kept in a linked list.  Usually there will be at most two
 * of these but rearranging the queue can cause there to be more.
 */
struct track {
  /** @brief Next track */
  struct track *next;

  /** @brief Input file descriptor */
  int fd;                               /* input FD */

  /** @brief Track ID */
  char id[24];

  /** @brief Start position of data in buffer */
  size_t start;

  /** @brief Number of bytes of data in buffer */
  size_t used;

  /** @brief Set @c fd is at EOF */
  int eof;

  /** @brief Total number of frames played */
  unsigned long long played;

  /** @brief Slot in @ref fds */
  int slot;

  /** @brief Set when playable
   *
   * A track becomes playable whenever it fills its buffer or reaches EOF; it
   * stops being playable when it entirely empties its buffer.  Tracks start
   * out life not playable.
   */
  int playable;
  
  /** @brief Input buffer
   *
   * 1Mbyte is enough for nearly 6s of 44100Hz 16-bit stereo
   */
  char buffer[1048576];
};

/** @brief Structure of a backend */
struct speaker_backend {
  /** @brief Which backend this is
   *
   * @c -1 terminates the list.
   */
  int backend;

  /** @brief Flags
   *
   * This field is currently not used and must be 0.
   */
  unsigned flags;
  
  /** @brief Initialization
   *
   * Called once at startup.  This is responsible for one-time setup
   * operations, for instance opening a network socket to transmit to.
   *
   * When writing to a native sound API this might @b not imply opening the
   * native sound device - that might be done by @c activate below.
   */
  void (*init)(void);

  /** @brief Activation
   * @return 0 on success, non-0 on error
   *
   * Called to activate the output device.
   *
   * On input @ref device_state may be anything.  If it is @ref
   * device_open then the device is already open but might be using
   * the wrong sample format.  The device should be reconfigured to
   * use the right sample format.
   *
   * If it is @ref device_error then a retry is underway and an
   * attempt to recover or re-open the device (with the right sample
   * format) should be made.
   *
   * If it is @ref device_closed then the device should be opened with
   * the right sample format.
   *
   * Some devices are effectively always open and have no error state, in which
   * case this callback can be NULL.  Note that @ref device_state still
   * switches between @ref device_open and @ref device_closed in this case.
   */
  void (*activate)(void);

  /** @brief Play sound
   * @param frames Number of frames to play
   * @return Number of frames actually played
   *
   * If an error occurs (and it is not immediately recovered) this
   * should set @ref device_state to @ref device_error.
   */
  size_t (*play)(size_t frames);
  
  /** @brief Deactivation
   *
   * Called to deactivate the sound device.  This is the inverse of @c
   * activate above.
   *
   * For sound devices that are open all the time and have no error
   * state, this callback can be NULL.  Note that @ref device_state
   * still switches between @ref device_open and @ref device_closed in
   * this case.
   */
  void (*deactivate)(void);

  /** @brief Called before poll()
   * @param timeoutp Pointer to timeout
   *
   * Called before the call to poll().
   *
   * If desirable, should call addfd() to update the FD array and stash the
   * slot number somewhere safe.  This will only be called if @ref device_state
   * is @ref device_open.
   *
   * @p timeoutp points to the poll timeout value in milliseconds.  It may be
   * reduced, but never increased.
   *
   * NB you can NOT assume that @c beforepoll is always called before @c play.
   */
  void (*beforepoll)(int *timeoutp);

  /** @brief Called after poll()
   * @return 1 if output device ready for play, 0 otherwise
   *
   * Called after the call to poll().  This will only be called if
   * @ref device_state = @ref device_open.
   *
   * The return value should be 1 if the device was ready to play, or
   * 0 if it was not.
   */
  int (*ready)(void);
};

/** @brief Possible device states */
enum device_states {
  /** @brief The device is closed */
  device_closed,

  /** @brief The device is open and ready to receive sound
   *
   * The current device sample format is potentially part of this state.
   */
  device_open,
  
  /** @brief An error has occurred on the device
   *
   * This state is used to ensure that a small interval is left
   * between retrying the device.  If errors just set @ref
   * device_closed then the main loop would busy-wait on broken output
   * devices.
   *
   * The current device sample format is potentially part of this state.
   */
  device_error
};

extern enum device_states device_state;
extern struct track *tracks;
extern struct track *playing;

extern const struct speaker_backend network_backend;
extern const struct speaker_backend alsa_backend;
extern const struct speaker_backend command_backend;
extern const struct speaker_backend coreaudio_backend;
extern const struct speaker_backend oss_backend;

extern struct pollfd fds[NFDS];
extern int fdno;
extern size_t bpf;
extern int idled;

int addfd(int fd, int events);
void abandon(void);

#endif /* SPEAKER_H */

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/
