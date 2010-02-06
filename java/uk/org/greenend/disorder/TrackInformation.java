/*
 * This file is part of DisOrder.
 * Copyright (C) 2010 Richard Kettlewell
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
package uk.org.greenend.disorder;

import java.util.*;

/**
 * Information about a track in the queue or another list.
 *
 * Returned by various methods of {@link DisorderServer DisorderServer}.
 *
 * <p>Queued track data appears in the following contexts:
 * <ul>
 * <li>The playing track
 * <li>The queue
 * <li>Recently played tracks
 * </ul>
 *
 * <p>The same format is used for each although the available
 * information differs slightly.
 *
 * <p>It is possible that future versions of the server will send keys
 * unknown to this class.  The current implementation just ignores
 * them.  However, unknown states and origins will currently generate
 * an exception.
 */
public class TrackInformation {
  /**
   * Possible track origins.
   */
  public enum Origin {
    Adopted,
    Picked,
    Random,
    Scheduled,
    Scratch
  }

  /**
   * Possible track states
   */
  public enum State {
    Failed,
    Ok,
    Scratched,
    Paused,
    Unplayed,
    Quitting
  }

  /**
   * When the track is expected to play.
   */
  public Date expected;

  /**
   * Unique ID in the queue
   */
  public String id;

  /**
   * When the track was played, or <code>null</code>.
   */
  public Date played;

  /**
   * The user that scratched the track, or <code>null</code>.
   */
  public String scratched;

  /**
   * The origin of the track.
   */
  public Origin origin;

  /**
   * The current state of the track
   */
  public State state;

  /**
   * The user that submitted the track, or <code>null</code>.
   */
  public String submitter;

  /**
   * The name of the track.
   */
  public String track;

  /**
   * When the track was added to the queue.
   */
  public Date when;

  /**
   * The exit status of the player.
   *
   * Note that this is packed UNIX “wait status” code, not a simple exit
   * status.
   */
  public int wstat;

  /**
   * Construct an empty track information object.
   *
   * <p>The usual way to get one is for it to be returned by some
   * method of DisorderServer.  However, in some cases it may be
   * convenient to create your own and fill it in, so this default
   * constructor is provided.
   */
  public TrackInformation() {
  }

  /**
   * Construct a new track information object from a string.
   *
   * @param s Encoded track information
   * @throws DisorderParseError If the track information is malformed
   */
  TrackInformation(String s) throws DisorderParseError {
    unpack(DisorderMisc.split(s, false), 0);
  }

  /**
   * Construct a new track information object from a parsed response.
   *
   * @param v Response from server
   * @param start Start index of track information
   * @throws DisorderParseError If the track information is malformed
   */
  TrackInformation(Vector<String> v, int start)
    throws DisorderParseError {
    unpack(v, start);
  }

  /**
   * Fill in track information from a parsed response
   *
   * @param v Response from server
   * @param start Start index of track information
   * @throws DisorderParseError If the track information is malformed
   */
  private void unpack(Vector<String> v, int start) throws DisorderParseError {
    if((v.size() - start) % 2 == 1)
      throw new DisorderParseError("odd-length track information");
    int n = start;
    while(n < v.size()) {
      String key = v.get(n++);
      String value = v.get(n++);
      if(key == "expected")
        expected = new Date(Long.parseLong(value));
      else if(key == "id")
        id = value;
      else if(key == "played")
        played = new Date(Long.parseLong(value));
      else if(key == "scratched")
        scratched = value;
      else if(key == "origin")
        try {
          origin = Origin.valueOf(fixCase(value));
        } catch(IllegalArgumentException e) {
          throw new DisorderParseError("unknown origin: " + value);
        }
      else if(key == "state")
        try {
          state = State.valueOf(fixCase(value));
        } catch(IllegalArgumentException e) {
          throw new DisorderParseError("unknown state: " + value);
        }
      else if(key == "submitter")
        submitter = value;
      else if(key == "track")
        track = value;
      else if(key == "when")
        when = new Date(Long.parseLong(value));
      else if(key == "wstat")
        wstat = Integer.parseInt(value);
      else {
        /* We interpret unknown keys as coming from the future and
         * ignore them */
      }
    }
  }

  /**
   * Uppercase the first letter of a string
   *
   * @param s String to modify
   * @return Same string with first letter converted to upper case
   */
  static private String fixCase(String s) {
    char cs[] = s.toCharArray();
    if(cs.length > 0)
      cs[0] = Character.toUpperCase(cs[0]);
    return new String(cs);
  }

};

/*
Local Variables:
mode:java
indent-tabs-mode:nil
c-basic-offset:2
End:
*/
